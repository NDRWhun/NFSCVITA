#include <lib/thread.h>
#include <lib/event.h>
#include <lib/scheduler.h>
#include <lib/teb.h>
#include <lib/window.h>
#include <platform/log.h>
#include <platform/sync.h>
#include <platform/time.h>
#include <nfsc_layout.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>

namespace win32
{

static const uint32_t s_hostStackSize = 1024 * 1024;

// A parked thread re-checks its terminate flag this often.
static const uint32_t s_parkCheckMs = 100;

static std::atomic<uint32_t> s_threadCount{1};

struct ThreadControl
{
    explicit ThreadControl(x86::reg32 guestId)
        :   guestId(guestId)
        ,   level(scheduler::LevelNormal)
        ,   priority(0)
        ,   holder(pf::perf::HolderMain)
        ,   suspendCount(0)
        ,   startPending(false)
        ,   start(nullptr)
        ,   host(nullptr)
        ,   resumed("thread_resume", 0)
        ,   hostId(0)
        ,   entry(0)
        ,   watch(scheduler::WatchIdle)
        ,   block(0)
        ,   caller(0)
        ,   hostCaller(nullptr)
        ,   cpu(nullptr)
    {
        name[0] = 0;
    }

    x86::reg32          guestId;
    std::atomic<int>    level;          // scheduler::Level
    std::atomic<int>    priority;       // Windows thread priority
    pf::perf::Holder    holder;         // kind of thread, for perf_log
    std::atomic<int>    suspendCount;   // changed under s_tableMutex
    bool                startPending;   // created suspended and not started yet
    pf::Semaphore*      start;          // start semaphore of a guest thread
    pf::Thread*         host;           // host thread of a guest thread, null for other host threads
    pf::Semaphore       resumed;        // posted when the suspend count drops to 0
    // Hang watchdog; written by the thread itself, the table fields under s_tableMutex.
    int                         hostId;     // kernel thread id, 0 before the thread runs
    x86::reg32                  entry;      // guest entry point (CreateThread, after start thunks)
    char                        name[32];   // from the debugger naming exception
    std::atomic<int>            watch;      // scheduler::WatchState
    std::atomic<int>            block;      // pf::perf::Block of the last lock release
    std::atomic<x86::reg32>     caller;     // guest return address of the last lock release
    std::atomic<const void*>    hostCaller; // host code of the last lock release
    std::atomic<const x86::CPU*> cpu;       // CPU state of the last lock acquisition
};

// Host thread id -> guest thread id and control. Unregistered threads report the main thread (1).
struct ThreadIdEntry
{
    int             hostId;
    x86::reg32      guestId;
    ThreadControl*  control;
};
static const int    s_maxThreadIds = 64;
static ThreadIdEntry s_threadIds[s_maxThreadIds];
static int          s_threadIdCount = 0;
// Guards the thread id table, the controls and the suspend counts.
static pf::LwMutex  s_tableMutex("thread_table");
// Controls of guest threads (CreateThread), by guest thread id; host threads' controls live as long as the process.
static std::vector<std::unique_ptr<ThreadControl>> s_guestControls;
static std::vector<std::unique_ptr<ThreadControl>> s_hostControls;
static std::atomic<int> s_suspended{0};    // controls with a suspend count above 0

struct RoleEntry
{
    x86::reg32      entryPoint;
    pf::cores::Role role;
    const char*     name;
};
struct StartThunk
{
    x86::reg32      thunk;
    x86::reg32      entryOffset;        // of the real entry point in the parameter block
    x86::reg32      parameterOffset;    // of the real entry point's parameter
};
// Written before guest code runs, read-only afterwards.
static std::vector<RoleEntry> s_roles;
static std::vector<StartThunk> s_startThunks;
static std::vector<x86::reg32> s_lockFreeEntries;

// Under s_tableMutex.
static ThreadControl* findGuestControl(x86::reg32 guestId)
{
    for (const std::unique_ptr<ThreadControl>& control : s_guestControls)
    {
        if (control->guestId == guestId)
            return control.get();
    }
    return nullptr;
}

// Under s_tableMutex.
static ThreadIdEntry* findEntry(int hostId)
{
    for (int i = 0; i < s_threadIdCount; ++i)
    {
        if (s_threadIds[i].hostId == hostId)
            return &s_threadIds[i];
    }
    return nullptr;
}

// Under s_tableMutex.
static void registerEntry(x86::reg32 guestId, ThreadControl* control)
{
    if (s_threadIdCount < s_maxThreadIds)
        s_threadIds[s_threadIdCount++] = ThreadIdEntry{ pf::Thread::currentId(), guestId, control };
    else
        pf::logError("thread id table full");
    if (control)
        control->hostId = pf::Thread::currentId();
}

static void unregisterCurrentThread()
{
    pf::LwLockGuard guard(s_tableMutex);
    ThreadIdEntry* entry = findEntry(pf::Thread::currentId());
    if (entry)
        *entry = s_threadIds[--s_threadIdCount];
}

std::vector<Thread::Shared*> Thread::s_live;
static pf::LwMutex s_liveMutex("threads");

Thread::Shared::Shared(WinApplication* app, x86::reg32 entryPoint, x86::reg32 parameter, x86::reg32 flags)
    :   m_refCount(1)
    ,   m_app(app)
    ,   m_entryPoint(entryPoint)
    ,   m_parameter(parameter)
    ,   m_flags(flags)
    ,   m_threadId(1 + s_threadCount.fetch_add(1))
    ,   m_priority(0)
    ,   m_stop(false)
    ,   m_finished(false)
    ,   m_lockFree(false)
    ,   m_semaphore(nullptr)
    ,   m_thread(nullptr)
    ,   m_ended(new Event(Event::ManualReset, false))
    ,   m_cpu{}
{
    m_ended->setThreadEnd(m_threadId);
}

Thread::Shared::~Shared()
{
    delete m_ended;
    delete m_thread;
    delete m_semaphore;
}

// Handle of the calling thread (DuplicateHandle of GetCurrentThread): the state of the guest thread that calls, or
// a state of its own (never ended) for a thread without one.
Thread::Thread()
    :   m_shared(nullptr)
    ,   m_handle(0)
{
    const x86::reg32 id = currentThreadId();
    {
        pf::LwLockGuard guard(s_liveMutex);
        for (Shared* shared : s_live)
        {
            if (shared->m_threadId == id && !shared->m_finished.load())
            {
                shared->m_refCount.fetch_add(1);
                m_shared = shared;
                break;
            }
        }
    }
    if (!m_shared)
    {
        m_shared = new Shared(nullptr, 0, 0, 0);
        m_shared->m_threadId = id;
        m_shared->m_ended->setThreadEnd(id);
    }
}

Thread::Thread(const Thread& other)
    :   GenericResource()
    ,   m_shared(other.m_shared)
    ,   m_handle(0)
{
    m_shared->m_refCount.fetch_add(1);
}

Thread::Thread(WinApplication *app, x86::reg32 method, x86::reg32 parameter, x86::reg32 flags)
    :   m_shared(new Shared(app, method, parameter, flags))
    ,   m_handle(0)
{
    reapFinished();
    const bool suspended = (flags & 0x4) != 0;  // CREATE_SUSPENDED
    const x86::reg32 entry = scheduler::resolveEntry(app, method, parameter);
    const pf::cores::Role role = scheduler::threadRole(entry);
    const char* const name = scheduler::threadName(entry);
    m_shared->m_lockFree = scheduler::lockFree(entry);
    m_shared->m_semaphore = new pf::Semaphore("thread_start",suspended ? 0 : 1);
    m_shared->m_thread = new pf::Thread("guest",&run, m_shared, pf::Thread::PriorityNormal, s_hostStackSize,
                                        pf::cores::mask(role));
    {
        pf::LwLockGuard guard(s_tableMutex);
        s_guestControls.emplace_back(new ThreadControl(m_shared->m_threadId));
        ThreadControl* control = s_guestControls.back().get();
        control->holder = role == pf::cores::Game ? pf::perf::HolderGame
                        : role == pf::cores::Io ? pf::perf::HolderIo : pf::perf::HolderHelper;
        control->start = m_shared->m_semaphore;
        control->host = m_shared->m_thread;
        control->entry = entry;
        if (suspended)
        {
            control->startPending = true;
            control->suspendCount.store(1);
            s_suspended.fetch_add(1);
        }
    }
    pf::logInfo("thread 0x%x (%s): entry 0x%08x, %s core %d%s%s", m_shared->m_threadId, name ? name : "unnamed",
                entry, pf::cores::name(role), pf::cores::core(role), suspended ? ", suspended" : "",
                m_shared->m_lockFree ? ", without the execution lock" : "");
    m_shared->m_refCount.fetch_add(1);
    {
        pf::LwLockGuard guard(s_liveMutex);
        s_live.push_back(m_shared);
    }
    m_shared->m_thread->start();
}

Thread::~Thread()
{
    if (m_handle)
        Event::unregisterAlias(m_handle);
    release(m_shared);
    reapFinished();
}

void Thread::registerHandle(x86::reg32 handle)
{
    m_handle = handle;
    Event::registerAlias(handle, m_shared->m_ended);
}

void Thread::release(Shared* shared)
{
    if (shared->m_refCount.fetch_sub(1) != 1)
        return;
    if (shared->m_app)
    {
        pf::LwLockGuard guard(s_tableMutex);
        for (auto it = s_guestControls.begin(); it != s_guestControls.end(); ++it)
        {
            if ((*it)->guestId == shared->m_threadId)
            {
                if ((*it)->suspendCount.load() > 0)
                    s_suspended.fetch_sub(1);
                s_guestControls.erase(it);
                break;
            }
        }
    }
    delete shared;
}

// Joins a host thread taken off the live list, stopping it first if it may still run, and drops the list's
// reference.
void Thread::join(Shared* shared)
{
    if (!shared->m_finished.load())
    {
        shared->m_stop.store(true);
        shared->m_cpu.terminate = true;
        // A thread that was never resumed starts and returns at once.
        shared->m_semaphore->post();
        Event::broadcastTerminate();
        pf::logDebug("Waiting for thread 0x%x", shared->m_threadId);
        Window::wake();
    }
    shared->m_thread->join();
    {
        pf::LwLockGuard guard(s_tableMutex);
        ThreadControl* control = findGuestControl(shared->m_threadId);
        if (control)
            control->host = nullptr;
    }
    delete shared->m_thread;
    shared->m_thread = nullptr;
    release(shared);
}

// Joins the threads that have returned, so their host threads do not wait for shutdown.
void Thread::reapFinished()
{
    std::vector<Shared*> finished;
    {
        pf::LwLockGuard guard(s_liveMutex);
        for (std::vector<Shared*>::iterator it = s_live.begin(); it != s_live.end();)
        {
            if ((*it)->m_finished.load())
            {
                finished.push_back(*it);
                it = s_live.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }
    for (Shared* shared : finished)
        join(shared);
}

void Thread::terminateAll()
{
    for (;;)
    {
        std::vector<Shared*> threads;
        {
            pf::LwLockGuard guard(s_liveMutex);
            threads.swap(s_live);
        }
        if (threads.empty())
            return;
        for (Shared* shared : threads)
            join(shared);
    }
}

void Thread::resume()
{
    if (m_shared->m_semaphore)
        m_shared->m_semaphore->post();
}

// Takes effect at once: the host priority, and the preemption level the execution lock checks.
void Thread::setPriority(int priority)
{
    m_shared->m_priority.store(priority, std::memory_order_relaxed);
    pf::LwLockGuard guard(s_tableMutex);
    ThreadControl* control = findGuestControl(m_shared->m_threadId);
    if (!control)
        return;
    control->priority.store(priority);
    control->level.store(scheduler::levelOf(priority));
    if (control->host)
        control->host->setPriority(scheduler::hostPriorityOf(priority));
}

x86::reg32 Thread::registerHostThread()
{
    const x86::reg32 id = 1 + s_threadCount.fetch_add(1);
    pf::LwLockGuard guard(s_tableMutex);
    s_hostControls.emplace_back(new ThreadControl(id));
    registerEntry(id, s_hostControls.back().get());
    return id;
}

x86::reg32 Thread::currentThreadId()
{
    pf::LwLockGuard guard(s_tableMutex);
    const ThreadIdEntry* entry = findEntry(pf::Thread::currentId());
    return entry ? entry->guestId : 1;
}

// Whoever takes the state off the live list joins it; a thread joined elsewhere is left alone.
void Thread::terminate()
{
    bool owned = false;
    {
        pf::LwLockGuard guard(s_liveMutex);
        std::vector<Shared*>::iterator it = std::find(s_live.begin(), s_live.end(), m_shared);
        if (it != s_live.end())
        {
            s_live.erase(it);
            owned = true;
        }
    }
    if (owned)
        join(m_shared);
}

x86::reg32 Thread::threadId() const
{
    return m_shared->m_threadId;
}

void Thread::sleep(x86::reg32 milliseconds)
{
    pf::sleepMs(milliseconds);
}

int Thread::run(void* data)
{
    Shared* shared = static_cast<Shared*>(data);
    {
        pf::LwLockGuard guard(s_tableMutex);
        ThreadControl* control = findGuestControl(shared->m_threadId);
        registerEntry(shared->m_threadId, control);
        if (pf::perf::enabled() && control && control->holder != pf::perf::HolderHelper)
            pf::perf::trackThread(control->holder == pf::perf::HolderGame ? "game" : "io");
        else if (pf::perf::enabled() && control && shared->m_lockFree)
            pf::perf::trackThread(scheduler::threadName(control->entry) ? scheduler::threadName(control->entry)
                                                                         : "lockfree");
    }
    shared->m_semaphore->wait();
    int result = 0;
    if (!shared->m_stop.load())
        result = shared->m_app->runThread(shared->m_cpu, shared->m_entryPoint, { shared->m_parameter },
                                          !shared->m_lockFree);
    scheduler::setExitCode(shared->m_threadId, x86::reg32(result));
    {
        pf::LwLockGuard guard(s_tableMutex);
        if (ThreadControl* control = findGuestControl(shared->m_threadId))
            control->watch.store(scheduler::WatchEnded, std::memory_order_relaxed);
    }
    unregisterCurrentThread();
    shared->m_ended->set();
    shared->m_finished.store(true);
    return result;
}

namespace scheduler
{

Level levelOf(int windowsPriority)
{
    if (windowsPriority >= 15)
        return LevelTimeCritical;
    if (windowsPriority >= 2)
        return LevelHighest;
    if (windowsPriority == 1)
        return LevelAboveNormal;
    return LevelNormal;
}

// THREAD_PRIORITY_* to host priorities around pf::Thread::PriorityNormal (lower is more urgent). The audio output
// and GXM render threads (PriorityHigh) stay above every guest thread.
int hostPriorityOf(int windowsPriority)
{
    if (windowsPriority >= 15)
        return pf::Thread::PriorityNormal - 24;
    if (windowsPriority <= -15)
        return pf::Thread::PriorityNormal + 28;
    const int clamped = std::max(-2, std::min(2, windowsPriority));
    return pf::Thread::PriorityNormal - 8 * clamped;
}

void setThreadRole(x86::reg32 entryPoint, pf::cores::Role role, const char* name)
{
    s_roles.push_back(RoleEntry{ entryPoint, role, name });
}

pf::cores::Role threadRole(x86::reg32 entryPoint)
{
    for (const RoleEntry& entry : s_roles)
    {
        if (entry.entryPoint == entryPoint)
            return entry.role;
    }
    return pf::cores::Helper;
}

const char* threadName(x86::reg32 entryPoint)
{
    for (const RoleEntry& entry : s_roles)
    {
        if (entry.entryPoint == entryPoint)
            return entry.name;
    }
    return nullptr;
}

void setLockFree(x86::reg32 entryPoint)
{
    s_lockFreeEntries.push_back(entryPoint);
}

bool lockFree(x86::reg32 entryPoint)
{
    return std::find(s_lockFreeEntries.begin(), s_lockFreeEntries.end(), entryPoint) != s_lockFreeEntries.end();
}

void setStartThunk(x86::reg32 thunk, x86::reg32 entryOffset, x86::reg32 parameterOffset)
{
    s_startThunks.push_back(StartThunk{ thunk, entryOffset, parameterOffset });
}

// Follows thunks that start other thunks (a few levels at most).
x86::reg32 resolveEntry(WinApplication* app, x86::reg32 startAddress, x86::reg32 parameter)
{
    for (int level = 0; level < 4; ++level)
    {
        const StartThunk* found = nullptr;
        for (const StartThunk& thunk : s_startThunks)
        {
            if (thunk.thunk == startAddress)
                found = &thunk;
        }
        if (!found || !parameter)
            break;
        startAddress = app->getMemory<x86::reg32>(parameter + found->entryOffset);
        parameter = app->getMemory<x86::reg32>(parameter + found->parameterOffset);
    }
    return startAddress;
}

ThreadControl* currentControl()
{
    pf::LwLockGuard guard(s_tableMutex);
    ThreadIdEntry* entry = findEntry(pf::Thread::currentId());
    if (entry && entry->control)
        return entry->control;
    s_hostControls.emplace_back(new ThreadControl(entry ? entry->guestId : 1));
    ThreadControl* control = s_hostControls.back().get();
    control->hostId = pf::Thread::currentId();
    if (entry)
        entry->control = control;
    else
        registerEntry(1, control);
    return control;
}

Level level(const ThreadControl* control)
{
    return control ? Level(control->level.load(std::memory_order_relaxed)) : LevelNormal;
}

pf::perf::Holder holder(const ThreadControl* control)
{
    if (!control)
        return pf::perf::HolderMain;
    // Host threads at the time-critical level run timer callbacks.
    if (!control->host && control->level.load(std::memory_order_relaxed) == LevelTimeCritical)
        return pf::perf::HolderTimer;
    return control->holder;
}

void setCurrentGameThread()
{
    currentControl()->holder = pf::perf::HolderGame;
    if (pf::perf::enabled())
        pf::perf::trackThread("game");
}

void setCurrentDisplayThread(bool overlap)
{
    currentControl()->holder = overlap ? pf::perf::HolderHelper : pf::perf::HolderGame;
    if (pf::perf::enabled())
        pf::perf::trackThread("display");
}

ThreadControl* setCurrentWorkerThread(x86::reg32 guestId, const char* name)
{
    {
        pf::LwLockGuard guard(s_tableMutex);
        if (!findEntry(pf::Thread::currentId()))
            registerEntry(guestId, nullptr);
    }
    ThreadControl* control = currentControl();
    control->holder = pf::perf::HolderHelper;
    if (pf::perf::enabled())
        pf::perf::trackThread(name);
    return control;
}

x86::reg32 guestId(const ThreadControl* control)
{
    return control ? control->guestId : 1;
}

void raiseCurrentLevel(Level level)
{
    ThreadControl* control = currentControl();
    int current = control->level.load();
    while (current < level && !control->level.compare_exchange_weak(current, level))
    {
    }
}

void setCurrentPriority(int windowsPriority)
{
    ThreadControl* control = currentControl();
    control->priority.store(windowsPriority);
    control->level.store(levelOf(windowsPriority));
    pf::Thread::setCurrentPriority(hostPriorityOf(windowsPriority));
}

int currentPriority()
{
    return currentControl()->priority.load();
}

int priority(x86::reg32 threadId)
{
    pf::LwLockGuard guard(s_tableMutex);
    const ThreadControl* control = findGuestControl(threadId);
    return control ? control->priority.load() : 0;
}

int suspend(x86::reg32 threadId)
{
    pf::LwLockGuard guard(s_tableMutex);
    ThreadControl* control = findGuestControl(threadId);
    if (!control)
        return -1;
    const int previous = control->suspendCount.load();
    if (previous == 0)
        s_suspended.fetch_add(1);
    control->suspendCount.store(previous + 1);
    return previous;
}

int resume(x86::reg32 threadId)
{
    pf::LwLockGuard guard(s_tableMutex);
    ThreadControl* control = findGuestControl(threadId);
    if (!control)
        return -1;
    const int previous = control->suspendCount.load();
    if (previous == 0)
        return 0;
    control->suspendCount.store(previous - 1);
    if (previous == 1)
    {
        s_suspended.fetch_sub(1);
        if (control->startPending)
        {
            control->startPending = false;
            control->start->post();
        }
        else
        {
            control->resumed.post();
        }
    }
    return previous;
}

bool anySuspended()
{
    return s_suspended.load(std::memory_order_relaxed) != 0;
}

bool mustPark(const ThreadControl* control)
{
    return control && control->suspendCount.load() > 0;
}

// Guest thread id -> exit code, under s_tableMutex. Ids are never reused.
static std::map<x86::reg32, x86::reg32> s_exitCodes;

void setExitCode(x86::reg32 threadId, x86::reg32 code)
{
    pf::LwLockGuard guard(s_tableMutex);
    s_exitCodes.insert(std::make_pair(threadId, code));
}

bool exitCode(x86::reg32 threadId, x86::reg32* code)
{
    pf::LwLockGuard guard(s_tableMutex);
    const auto it = s_exitCodes.find(threadId);
    if (it == s_exitCodes.end())
        return false;
    *code = it->second;
    return true;
}

void watchState(ThreadControl* control, WatchState state)
{
    if (control)
        control->watch.store(state, std::memory_order_relaxed);
}

void watchAcquired(ThreadControl* control, const x86::CPU& cpu)
{
    if (!control)
        return;
    control->cpu.store(&cpu, std::memory_order_relaxed);
    control->watch.store(WatchGuest, std::memory_order_relaxed);
}

void watchFree(ThreadControl* control, const x86::CPU& cpu)
{
    if (!control)
        return;
    control->cpu.store(&cpu, std::memory_order_relaxed);
    control->watch.store(WatchFree, std::memory_order_relaxed);
}

void watchReleased(ThreadControl* control, int block, x86::reg32 caller, const void* hostCaller)
{
    if (!control)
        return;
    control->block.store(block, std::memory_order_relaxed);
    control->caller.store(caller, std::memory_order_relaxed);
    control->hostCaller.store(hostCaller, std::memory_order_relaxed);
    control->watch.store(WatchReleased, std::memory_order_relaxed);
}

void setThreadName(x86::reg32 threadId, const char* name)
{
    pf::LwLockGuard guard(s_tableMutex);
    ThreadControl* control = nullptr;
    if (threadId == 0xffffffff)
    {
        const ThreadIdEntry* entry = findEntry(pf::Thread::currentId());
        control = entry ? entry->control : nullptr;
    }
    else
    {
        control = findGuestControl(threadId);
        for (size_t i = 0; !control && i < s_hostControls.size(); ++i)
        {
            if (s_hostControls[i]->guestId == threadId)
                control = s_hostControls[i].get();
        }
    }
    if (control)
        snprintf(control->name, sizeof(control->name), "%s", name);
}

namespace
{

// A copy of a control's watch fields, taken under s_tableMutex.
struct WatchedThread
{
    x86::reg32          guestId;
    int                 hostId;
    x86::reg32          entry;
    char                name[32];
    int                 watch;
    int                 block;
    x86::reg32          caller;
    const void*         hostCaller;
    const x86::CPU*     cpu;
    int                 priority;
    int                 suspendCount;
    bool                startPending;
    bool                owner;
};

const char* watchText(const WatchedThread& thread)
{
    static const char* const blocks[pf::perf::BlockCount] = {
        "released the lock in a shim", "released the lock: Sleep", "released the lock: WaitFor*Object",
        "released the lock: critical section held by another thread", "released the lock: file IO",
        "released the lock: frame slot", "released the lock: GPU wait", "released the lock: preempted",
    };
    switch (thread.watch)
    {
    case WatchGuest:        return "holds the execution lock (guest code or a shim)";
    case WatchLockWait:     return "waits for the execution lock";
    case WatchReleased:     return thread.block >= 0 && thread.block < pf::perf::BlockCount ? blocks[thread.block]
                                                                                            : blocks[0];
    case WatchParked:       return "suspended by SuspendThread";
    case WatchEnded:        return "ended";
    case WatchFree:         return "runs guest code without the execution lock";
    default:                return thread.startPending ? "created suspended, not started" : "no guest code running";
    }
}

// Code addresses of the image's text section on the guest stack above esp: return addresses, most recent first.
int stackCode(WinApplication* app, x86::reg32 esp, x86::reg32* found, int capacity)
{
    const nfsc::layout::ImageSection& text = nfsc::layout::sections[0];
    const x86::reg32 Words = 256;
    if (!MemMap::isGuestRange(esp, Words * 4))
        return 0;
    int count = 0;
    for (x86::reg32 slot = esp; slot < esp + Words * 4 && count < capacity; slot += 4)
    {
        const x86::reg32 value = app->getMemory<x86::reg32>(slot);
        if (value - text.address < text.virtualSize)
            found[count++] = value;
    }
    return count;
}

}

void logThreads(WinApplication* app, const ThreadControl* owner)
{
    // A thread stuck with the table lock must not stop the report.
    bool locked = false;
    for (int attempt = 0; attempt < 100 && !(locked = s_tableMutex.tryLock()); ++attempt)
        pf::sleepMs(10);
    if (!locked)
    {
        pf::logInfo("watchdog: the thread table lock is held, guest threads not listed");
        return;
    }
    std::vector<WatchedThread> threads;
    threads.reserve(s_guestControls.size() + s_hostControls.size());
    for (const std::vector<std::unique_ptr<ThreadControl>>* list : { &s_hostControls, &s_guestControls })
    {
        for (const std::unique_ptr<ThreadControl>& control : *list)
        {
            WatchedThread thread;
            thread.guestId = control->guestId;
            thread.hostId = control->hostId;
            thread.entry = control->entry;
            memcpy(thread.name, control->name, sizeof(thread.name));
            thread.watch = control->watch.load(std::memory_order_relaxed);
            thread.block = control->block.load(std::memory_order_relaxed);
            thread.caller = control->caller.load(std::memory_order_relaxed);
            thread.hostCaller = control->hostCaller.load(std::memory_order_relaxed);
            thread.cpu = control->cpu.load(std::memory_order_relaxed);
            thread.priority = control->priority.load(std::memory_order_relaxed);
            thread.suspendCount = control->suspendCount.load(std::memory_order_relaxed);
            thread.startPending = control->startPending;
            thread.owner = control.get() == owner;
            threads.push_back(thread);
        }
    }
    s_tableMutex.unlock();

    for (const WatchedThread& thread : threads)
    {
        // Registers of a thread that runs or blocked in guest code; the values may be a moment old.
        x86::reg32 esp = 0, lastShim = 0;
        x86::reg32 code[8];
        int codeCount = 0;
        if (thread.cpu && thread.watch != WatchEnded && thread.watch != WatchIdle)
        {
            const volatile x86::CPU* cpu = thread.cpu;
            esp = cpu->esp;
            const x86::reg32 block = cpu->efs;
            if (MemMap::isGuestRange(block, teb::Size))
                lastShim = app->getMemory<x86::reg32>(block + teb::LastShimCall);
            codeCount = stackCode(app, esp, code, 8);
        }
        char stack[8 * 11 + 1] = "";
        for (int i = 0; i < codeCount; ++i)
            snprintf(stack + i * 11, sizeof(stack) - i * 11, " 0x%08x", code[i]);
        pf::logInfo("watchdog: guest thread 0x%x '%s' entry 0x%08x host 0x%x priority %d suspend %d%s: %s; last shim "
                    "call from 0x%08x; last lock release from 0x%08x (host %p); esp 0x%08x stack%s",
                    thread.guestId, thread.name, thread.entry, unsigned(thread.hostId), thread.priority,
                    thread.suspendCount, thread.owner ? " (lock owner)" : "", watchText(thread), lastShim,
                    thread.caller, thread.hostCaller, esp, codeCount ? stack : " -");
    }
}

void park(ThreadControl* control, const x86::CPU& cpu)
{
    control->watch.store(WatchParked, std::memory_order_relaxed);
    pf::logDebug("thread 0x%x suspended", control->guestId);
    // A wake-up left from an earlier resume is harmless: the count is checked again.
    while (!cpu.terminate && control->suspendCount.load() > 0)
        control->resumed.waitFor(s_parkCheckMs);
    pf::logDebug("thread 0x%x resumed", control->guestId);
}

}

}
