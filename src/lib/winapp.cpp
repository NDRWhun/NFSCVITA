#include <lib/winapp.h>
#include <lib/foreigncode.h>
#include <lib/mutex.h>
#include <lib/event.h>
#include <lib/library.h>
#include <lib/scheduler.h>
#include <lib/teb.h>
#include <lib/thread.h>
#include <lib/timer.h>
#include <winapi/types.h>
#include <x86.h>

#include <winapi/com.h>
#include <winapi/kernel32.h>
#include <platform/log.h>
#include <platform/perf.h>
#include <platform/sync.h>
#include <platform/time.h>
#include <platform/watchdog.h>
#include <psp2/kernel/threadmgr.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <vector>

namespace win32
{

static x86::reg32 s_resourceIndex;
static std::atomic<int> s_resourceCount{0};

// Words of the guest thread block (fs) used by the host; guest code only uses fs:[0].
static const x86::reg32 ThreadLockOffset     = teb::HostLock;
static const x86::reg32 ThreadControlOffset  = teb::HostControl;

// How long a thread of a raised level waits awake for the execution lock before it blocks, and how long a thread
// that yielded it waits for such a thread to take it before queueing again.
static const uint64_t PreemptSpinUs = 500;

// Threads waiting for the execution lock per preemption level above normal (m_preemptWaiters is their sum).
static std::atomic<int> s_levelWaiters[scheduler::LevelCount];

// A normal-level thread waiting this long for the execution lock makes its holder yield at a preemption point.
static const uint32_t StarvedWaitUs = 50000;
// Normal-level threads waiting longer than StarvedWaitUs (counted in m_preemptWaiters too).
static std::atomic<int> s_starvedWaiters{0};

// True while a thread holds the execution lock: spinning waiters read it instead of calling the kernel.
static std::atomic<bool> s_lockHeld{false};
// Control of the thread holding the execution lock, for the hang watchdog.
static std::atomic<const ThreadControl*> s_lockOwner{nullptr};
// The application the hang watchdog reports on.
static WinApplication* s_watchApp = nullptr;
// Spin iterations between two clock reads of the spin loops.
static const uint32_t SpinClockMask = 15;

// Thread blocks and guest stacks of returned runThread calls, reused: timer callbacks run about 125 times a second,
// and finding 1 MB in the guest memory map takes a long first-fit search.
struct ThreadMemory
{
    MemMap* block;
    MemMap* stack;
};
static std::vector<ThreadMemory> s_threadMemory;
static pf::LwMutex s_threadMemoryLock("thread_memory");

// Holds one thread block and stack for the duration of a runThread call.
class ThreadMemoryLease
{
public:
    ThreadMemoryLease()
    {
        pf::LwLockGuard guard(s_threadMemoryLock);
        if (s_threadMemory.empty())
        {
            m_memory = ThreadMemory{ new MemMap(teb::Size), new MemMap(1024 * 1024) };
        }
        else
        {
            m_memory = s_threadMemory.back();
            s_threadMemory.pop_back();
        }
    }

    ~ThreadMemoryLease()
    {
        pf::LwLockGuard guard(s_threadMemoryLock);
        s_threadMemory.push_back(m_memory);
    }

    const MemMap& block() const { return *m_memory.block; }
    const MemMap& stack() const { return *m_memory.stack; }

private:
    ThreadMemoryLease(const ThreadMemoryLease&) = delete;
    ThreadMemoryLease& operator=(const ThreadMemoryLease&) = delete;
    ThreadMemory m_memory;
};

static void freeThreadMemory()
{
    pf::LwLockGuard guard(s_threadMemoryLock);
    for (const ThreadMemory& memory : s_threadMemory)
    {
        delete memory.stack;
        delete memory.block;
    }
    s_threadMemory.clear();
}

// The method table doubles while methods register; about 43,100 methods end in 131,072 slots.
static const x86::reg32 s_initialMethodSlotBits = 10;

static ForeignCodeResolver s_foreignCodeResolver = nullptr;

void setForeignCodeResolver(ForeignCodeResolver resolver)
{
    s_foreignCodeResolver = resolver;
}

static ForeignCodeObserver s_foreignCodeObserver = nullptr;

void setForeignCodeObserver(ForeignCodeObserver observer)
{
    s_foreignCodeObserver = observer;
}

void foreignCodeChanged(x86::reg32 address, x86::reg32 size)
{
    if (s_foreignCodeObserver)
        s_foreignCodeObserver(address, size);
}

[[noreturn]] static void missingMethod(x86::reg32 address)
{
    pf::logError("dynamic_call: no method registered at guest address 0x%08x", address);
    __builtin_trap();
}

// Empty slots hold address 0, so only a call to guest address 0 reaches this.
static void callAddressZero(WinApplication* application, x86::CPU& cpu)
{
    pf::logError("dynamic_call: call to guest address 0 returning to 0x%08x",
                 x86::reg32(application->getMemory<x86::reg32>(cpu.esp)));
    missingMethod(0);
}

GenericResource::GenericResource()
    :   m_resourceIndex(0)
{
    s_resourceCount.fetch_add(1);
}

GenericResource::~GenericResource()
{
    RC_ASSERT(m_resourceIndex == 0);
    s_resourceCount.fetch_sub(1);
}

void GenericResource::setResourceIndex(x86::reg32 resourceIndex)
{
    RC_ASSERT(!resourceIndex || !m_resourceIndex);
    m_resourceIndex = resourceIndex;
}

x86::reg32 GenericResource::getResourceIndex() const
{
    return m_resourceIndex;
}


WinApplication::WinApplication(const char* appName, x86::reg32 baseAddress, const std::vector<Section>& sections)
    :   m_appName(nullptr)
    ,   m_appNameW(nullptr)
    ,   m_env(nullptr)
    ,   m_executionContext(new Mutex("ExecutionContext", Mutex::KernelPriority))
    ,   m_preemptWaiters(0)
    ,   m_contextDepth(0)
    ,   m_resourceContext(new Mutex("Resource"))
    ,   m_methodSlots((x86::reg32(1) << s_initialMethodSlotBits) + 1, MethodSlot{0, &callAddressZero})
    ,   m_methodShift(32 - s_initialMethodSlotBits)
    ,   m_cpu{}
{
    MemMap::init(baseAddress, sections);
    m_appName = new MemMap(4096);
    m_appNameW = new MemMap(4096);
    m_env = new MemMap(4096);
    kernel32::registerModules(this);
    scheduler::configureGameThreads();
    Event::init();
    Timer::init();
    com::InitialiseComMemory(this);
    s_watchApp = this;
    pf::watchdog::setReporter(&WinApplication::watchReport);
    strcpy(&getMemory<char>(m_appName->getBlockStart()), appName);
    getMemory<char>(m_env->getBlockStart()) = 0;
    WCHAR* wchar = &getMemory<WCHAR>(m_appNameW->getBlockStart());
    for (const char* appNameW = appName; *appNameW; ++wchar, ++appNameW)
    {
        *wchar = WCHAR(*appNameW);
    }
    *wchar = 0;
}

WinApplication::~WinApplication()
{
    pf::watchdog::setReporter(nullptr);
    // Timer callbacks and guest threads stop first so no guest code runs while resources are freed.
    Timer::fini();
    Thread::terminateAll();
    freeThreadMemory();
    delete m_appNameW;
    delete m_appName;
    // Highest handles first: later objects depend on earlier ones (sound buffers on their device).
    m_resourceContext->lock();
    while (!m_resources.empty())
    {
        std::map<x86::reg32, GenericResource*>::iterator it = --m_resources.end();
        GenericResource* resource = it->second;
        m_resources.erase(it);
        resource->setResourceIndex(0);
        delete resource;
    }
    m_resourceContext->unlock();
    Event::fini();
    MemMap::fini();
    delete m_resourceContext;
    delete m_executionContext;
    RC_ASSERT(s_resourceCount.load() == 0);
}

void WinApplication::setEnvironment(std::initializer_list<const char*> variables)
{
    char* block = &getMemory<char>(m_env->getBlockStart());
    const char* const end = block + m_env->getBlockSize() - 1;
    for (const char* variable : variables)
    {
        const size_t length = strlen(variable) + 1;
        if (block + length >= end)
            break;
        memcpy(block, variable, length);
        block += length;
    }
    *block = 0;
}

x86::reg32 WinApplication::allocateResourceFixed(GenericResource *resource, x86::reg32 fixedHandle)
{
    RC_ASSERT((fixedHandle < 0x20000) || (fixedHandle > 0x20000 + 0x30000));
    m_resourceContext->lock();
    resource->setResourceIndex(fixedHandle);
    bool inserted = m_resources.insert(std::make_pair(fixedHandle, resource)).second;
    RC_ASSERT(inserted);
    m_resourceContext->unlock();
    return fixedHandle;
}

x86::reg32 WinApplication::allocateResource(GenericResource *resource)
{
    m_resourceContext->lock();
    x86::reg32 resourceIndex = ++s_resourceIndex + 0x20000;
    resource->setResourceIndex(resourceIndex);
    bool inserted = m_resources.insert(std::make_pair(resourceIndex, resource)).second;
    RC_ASSERT(inserted);
    m_resourceContext->unlock();
    return resourceIndex;
}

GenericResource* WinApplication::getResource(x86::reg32 resource)
{
    GenericResource* result = nullptr;
    m_resourceContext->lock();
    std::map<x86::reg32, GenericResource*>::iterator it = m_resources.find(resource);
    if (it != m_resources.end())
    {
        result = it->second;
    }
    else
    {
        result = nullptr;
    }
    m_resourceContext->unlock();
    return result;
}

// Deleted outside the resource lock: a destructor may wait for a thread that needs it.
bool WinApplication::freeResource(x86::reg32 resource)
{
    m_resourceContext->lock();
    std::map<x86::reg32, GenericResource*>::iterator it = m_resources.find(resource);
    if (it == m_resources.end())
    {
        m_resourceContext->unlock();
        return false;
    }
    GenericResource* object = it->second;
    m_resources.erase(it);
    m_resourceContext->unlock();
    object->setResourceIndex(0);
    delete object;
    return true;
}

void WinApplication::registerMethod(x86::reg32 pointer, Method method)
{
    RC_ASSERT(pointer != 0);
    RC_ASSERT(m_methodNames.find(pointer) == m_methodNames.end());
    m_methodNames[pointer] = std::move(method.name);
    if (m_methodNames.size() * 2 > methodSlotCount())
        growMethodTable();
    insertMethod(pointer, method.method);
}

MethodPtr WinApplication::replaceMethod(x86::reg32 pointer, MethodPtr method)
{
    const MethodPtr previous = findMethod(pointer);
    insertMethod(pointer, method);
    return previous;
}

const char* WinApplication::getMethodName(x86::reg32 address) const
{
    std::unordered_map<x86::reg32, std::string>::const_iterator it = m_methodNames.find(address);
    return it != m_methodNames.end() ? it->second.c_str() : nullptr;
}

// Recent table lookups past the second probe: the address in the low word, the method in the high word (0 when the
// table has no method there). Cleared when a method is stored.
static const x86::reg32 s_findCacheBits = 8;
static std::atomic<uint64_t> s_findCache[1u << s_findCacheBits];
static std::atomic<bool> s_findCacheUsed{false};

static void clearFindCache()
{
    s_findCacheUsed.store(false, std::memory_order_relaxed);
    for (std::atomic<uint64_t>& entry : s_findCache)
        entry.store(0, std::memory_order_relaxed);
}

static void storeFindCache(std::atomic<uint64_t>& entry, uint64_t value)
{
    s_findCacheUsed.store(true, std::memory_order_relaxed);
    entry.store(value, std::memory_order_relaxed);
}

// The foreign code resolver's method for an address without a registered one; traps without one.
static MethodPtr findForeign(x86::reg32 address)
{
    // Not cached: foreign code comes and goes with the data that holds it.
    if (const MethodPtr foreign = s_foreignCodeResolver ? s_foreignCodeResolver(address) : nullptr)
        return foreign;
    missingMethod(address);
}

MethodPtr WinApplication::findDisplacedMethod(const MethodSlot* home, x86::reg32 address) const
{
    return home[1].address == address ? home[1].method : findMethod(address);
}

MethodPtr WinApplication::findMethod(x86::reg32 address) const
{
    // Middle bits of the hash: addresses of one probe run share its top bits.
    const x86::reg32 index = (address * MethodHashMultiplier) >> 12 & ((1u << s_findCacheBits) - 1);
    std::atomic<uint64_t>& cached = s_findCache[index];
    const uint64_t entry = cached.load(std::memory_order_relaxed);
    if (address && x86::reg32(entry) == address)
    {
        const MethodPtr method = reinterpret_cast<MethodPtr>(uintptr_t(entry >> 32));
        return method ? method : findForeign(address);
    }
    const x86::reg32 mask = methodSlotCount() - 1;
    for (x86::reg32 i = methodSlot(address); ; i = (i + 1) & mask)
    {
        const MethodSlot& slot = m_methodSlots[i];
        if (slot.address == address)
        {
            if (address)
                storeFindCache(cached, uint64_t(reinterpret_cast<uintptr_t>(slot.method)) << 32 | address);
            return slot.method;
        }
        if (slot.address == 0)
        {
            storeFindCache(cached, address);
            return findForeign(address);
        }
    }
}

// Stores at the first slot holding the address or empty.
void WinApplication::insertMethod(x86::reg32 address, MethodPtr method)
{
    const x86::reg32 mask = methodSlotCount() - 1;
    x86::reg32 i = methodSlot(address);
    while (m_methodSlots[i].address != 0 && m_methodSlots[i].address != address)
        i = (i + 1) & mask;
    m_methodSlots[i] = MethodSlot{address, method};
    if (s_findCacheUsed.load(std::memory_order_relaxed))
        clearFindCache();
}

void WinApplication::growMethodTable()
{
    std::vector<MethodSlot> slots(methodSlotCount() * 2 + 1, MethodSlot{0, &callAddressZero});
    slots.swap(m_methodSlots);
    --m_methodShift;
    for (std::vector<MethodSlot>::const_iterator it = slots.begin(); it != slots.end(); ++it)
    {
        if (it->address)
            insertMethod(it->address, it->method);
    }
}

int WinApplication::runThread(x86::CPU& cpu, x86::reg32 entryPoint, std::initializer_list<x86::reg32> arguments,
                              bool threadLock, bool preempts)
{
    if (!m_cpu.terminate)
    {
        const ThreadMemoryLease memory;
        // A new thread block is zeroed, as on Windows.
        const x86::reg32 block = memory.block().getBlockStart();
        const x86::reg32 stackTop = memory.stack().getBlockStart() + memory.stack().getBlockSize();
        memset(&getMemory<x86::reg8>(block), 0, teb::Size);
        cpu.init(block, entryPoint);
        getMemory<x86::reg32>(block + teb::ExceptionList) = 0xffffffff;
        getMemory<x86::reg32>(block + teb::StackBase) = stackTop;
        getMemory<x86::reg32>(block + teb::StackLimit) = memory.stack().getBlockStart();
        getMemory<x86::reg32>(block + teb::Self) = block;
        getMemory<x86::reg32>(block + teb::ProcessId) = teb::GuestProcessId;
        getMemory<x86::reg32>(block + teb::ThreadId) = Thread::currentThreadId();
        getMemory<Mutex*>(block + ThreadLockOffset) = threadLock ? m_executionContext : nullptr;
        if (preempts)
            scheduler::raiseCurrentLevel(scheduler::LevelAboveNormal);
        ThreadControl* const control = scheduler::currentControl();
        getMemory<ThreadControl*>(block + ThreadControlOffset) = control;
        if (!threadLock)
            scheduler::watchFree(control, cpu);
        {
            LockContext ctx(*this, cpu);
            // A return address slot with the arguments above it, below 4 spare bytes.
            cpu.esp = stackTop - 8 - 4 * x86::reg32(arguments.size());
            x86::reg32 slot = cpu.esp + 4;
            for (x86::reg32 argument : arguments)
            {
                getMemory<x86::reg32>(slot) = argument;
                slot += 4;
            }
            getMemory<x86::reg32>(cpu.esp) = 0;
            dynamic_call(entryPoint, cpu);
        }
        scheduler::watchState(control, scheduler::WatchIdle);
        return int(cpu.eax);
    }
    else
    {
        return 0;
    }
}

// Takes the lock, counting a waiter above the normal level so the owner yields to it (allowPreemption).
static void acquire(Mutex* executionContext, scheduler::Level level, std::atomic<int>& preemptWaiters)
{
    if (level == scheduler::LevelNormal)
    {
        SceUInt timeout = StarvedWaitUs;
        if (sceKernelLockMutex(executionContext->kernelId(), 1, &timeout) == 0)
            return;
        s_starvedWaiters.fetch_add(1, std::memory_order_relaxed);
        preemptWaiters.fetch_add(1, std::memory_order_relaxed);
        executionContext->lock();
        preemptWaiters.fetch_sub(1, std::memory_order_relaxed);
        s_starvedWaiters.fetch_sub(1, std::memory_order_relaxed);
        return;
    }
    const bool timed = pf::perf::enabled();
    const uint32_t start = timed ? pf::perf::now() : 0;
    s_levelWaiters[level].fetch_add(1, std::memory_order_relaxed);
    preemptWaiters.fetch_add(1, std::memory_order_relaxed);
    // The owner gives the lock up at its next preemption point, usually within microseconds: a waiter that stays
    // awake takes it without a wake-up.
    const uint64_t spinEnd = pf::ticksUs() + PreemptSpinUs;
    bool owned = executionContext->tryLock();
    for (uint32_t spin = 1; !owned; ++spin)
    {
        if (!s_lockHeld.load(std::memory_order_relaxed))
            owned = executionContext->tryLock();
        if (!owned && (spin & SpinClockMask) == 0 && pf::ticksUs() >= spinEnd)
            break;
    }
    if (!owned)
        executionContext->lock();
    preemptWaiters.fetch_sub(1, std::memory_order_relaxed);
    s_levelWaiters[level].fetch_sub(1, std::memory_order_relaxed);
    if (timed)
        pf::perf::preemptWait(pf::perf::now() - start);
}

// perf_log accounting of the execution lock. The holder fields change only with the lock held, the game fields only
// on the game thread.
static uint32_t s_heldSince = 0;
static bool     s_gameReleased = false;
static uint32_t s_gameReleasedAt = 0;
static int      s_gameBlock = pf::perf::BlockOther;

// A thread suspended by SuspendThread stops here, without the lock, each time it would take it.
void WinApplication::lockContext(const x86::CPU& cpu)
{
    Mutex* executionContext = getMemory<Mutex*>(cpu.efs + ThreadLockOffset);
    if (!executionContext)
        return;
    ThreadControl* control = getMemory<ThreadControl*>(cpu.efs + ThreadControlOffset);
    const scheduler::Level level = scheduler::level(control);
    const bool timed = pf::perf::enabled();
    const uint32_t start = timed ? pf::perf::now() : 0;
    scheduler::watchState(control, scheduler::WatchLockWait);
    acquire(executionContext, level, m_preemptWaiters);
    ++m_contextDepth;
    s_lockHeld.store(true, std::memory_order_relaxed);
    while (scheduler::anySuspended() && m_contextDepth == 1 && scheduler::mustPark(control) && !cpu.terminate)
    {
        --m_contextDepth;
        s_lockHeld.store(false, std::memory_order_relaxed);
        executionContext->unlock();
        scheduler::park(control, cpu);
        scheduler::watchState(control, scheduler::WatchLockWait);
        acquire(executionContext, level, m_preemptWaiters);
        ++m_contextDepth;
        s_lockHeld.store(true, std::memory_order_relaxed);
    }
    if (m_contextDepth == 1)
    {
        s_lockOwner.store(control, std::memory_order_relaxed);
        pf::perf::g_lockHolder.store(pf::Thread::currentId(), std::memory_order_relaxed);
    }
    scheduler::watchAcquired(control, cpu);
    if (timed && m_contextDepth == 1)
    {
        const uint32_t acquired = pf::perf::now();
        s_heldSince = acquired;
        if (s_gameReleased && scheduler::holder(control) == pf::perf::HolderGame)
        {
            s_gameReleased = false;
            pf::perf::gameBlocked(pf::perf::Block(s_gameBlock), acquired - s_gameReleasedAt, acquired - start);
        }
    }
}

void WinApplication::unlockContext(const x86::CPU& cpu)
{
    Mutex* executionContext = getMemory<Mutex*>(cpu.efs + ThreadLockOffset);
    if (executionContext)
    {
        ThreadControl* const control = getMemory<ThreadControl*>(cpu.efs + ThreadControlOffset);
        if (m_contextDepth == 1)
        {
            s_lockOwner.store(nullptr, std::memory_order_relaxed);
            scheduler::watchReleased(control, pf::perf::g_nextBlock, getMemory<x86::reg32>(cpu.esp),
                                     __builtin_return_address(0));
        }
        if (pf::perf::enabled() && m_contextDepth == 1)
        {
            const uint32_t released = pf::perf::now();
            const pf::perf::Holder holder = scheduler::holder(control);
            pf::perf::held(holder, released - s_heldSince);
            if (holder == pf::perf::HolderHelper)
                pf::perf::helperHeld(scheduler::guestId(control), released - s_heldSince);
            if (holder == pf::perf::HolderGame)
            {
                s_gameReleased = true;
                s_gameReleasedAt = released;
                s_gameBlock = pf::perf::g_nextBlock;
            }
        }
        pf::perf::g_nextBlock = pf::perf::BlockOther;
        if (--m_contextDepth == 0)
        {
            s_lockHeld.store(false, std::memory_order_relaxed);
            pf::perf::g_lockHolder.store(0, std::memory_order_relaxed);
        }
        executionContext->unlock();
    }
}

// The thread block names the execution lock while the hold lasts, so shims inside it release and retake the lock.
bool WinApplication::holdContext(const x86::CPU& cpu)
{
    if (getMemory<Mutex*>(cpu.efs + ThreadLockOffset))
        return false;
    getMemory<Mutex*>(cpu.efs + ThreadLockOffset) = m_executionContext;
    lockContext(cpu);
    return true;
}

void WinApplication::releaseHeldContext(const x86::CPU& cpu)
{
    unlockContext(cpu);
    getMemory<Mutex*>(cpu.efs + ThreadLockOffset) = nullptr;
    scheduler::watchFree(getMemory<ThreadControl*>(cpu.efs + ThreadControlOffset), cpu);
}

// Hang watchdog samples of the threads running guest code, at their preemption points: the host thread, the host code
// address in the translated function and the guest esp, distinct addresses only; a slot is read once marked ready.
static const int SampleMax = 8;
static std::atomic<bool> s_sampling{false};
static std::atomic<int> s_sampleClaimed{0};
static std::atomic<uint32_t> s_samplePoints{0};
static std::atomic<bool> s_sampleReady[SampleMax];
static const void* s_sampleHost[SampleMax];
static x86::reg32 s_sampleEsp[SampleMax];
static int s_sampleThread[SampleMax];

static void samplePreemptionPoint(const x86::CPU& cpu, const void* host)
{
    s_samplePoints.fetch_add(1, std::memory_order_relaxed);
    const int claimed = std::min(s_sampleClaimed.load(std::memory_order_relaxed), SampleMax);
    for (int i = 0; i < claimed; ++i)
    {
        if (s_sampleReady[i].load(std::memory_order_acquire) && s_sampleHost[i] == host)
            return;
    }
    const int slot = s_sampleClaimed.fetch_add(1, std::memory_order_relaxed);
    if (slot >= SampleMax)
        return;
    s_sampleHost[slot] = host;
    s_sampleEsp[slot] = cpu.esp;
    s_sampleThread[slot] = pf::Thread::currentId();
    s_sampleReady[slot].store(true, std::memory_order_release);
}

// The part of yieldContext past its checks, out of line so that their early return saves no registers.
__attribute__((noinline)) static void yieldToHigher(WinApplication& app, const x86::CPU& cpu)
{
    const ThreadControl* const control = app.getMemory<ThreadControl*>(cpu.efs + ThreadControlOffset);
    const int level = scheduler::level(control);
    auto higherWaits = [level]()
    {
        if (s_starvedWaiters.load(std::memory_order_relaxed) != 0)
            return true;
        for (int waiting = level + 1; waiting < scheduler::LevelCount; ++waiting)
        {
            if (s_levelWaiters[waiting].load(std::memory_order_relaxed) != 0)
                return true;
        }
        return false;
    };
    if (!higherWaits())
        return;
    // Only the game thread's yields are timed.
    const bool timed = pf::perf::enabled() && scheduler::holder(control) == pf::perf::HolderGame;
    const uint32_t start = timed ? pf::perf::now() : 0;
    pf::perf::blocking(pf::perf::BlockPreempt);
    app.unlockContext(cpu);
    // A waiter that spins for the lock takes it now; this thread must not take it back first.
    const uint64_t spinEnd = pf::ticksUs() + PreemptSpinUs;
    for (uint32_t spin = 1; higherWaits(); ++spin)
    {
        if ((spin & SpinClockMask) == 0 && pf::ticksUs() >= spinEnd)
            break;
    }
    app.lockContext(cpu);
    if (timed)
        pf::perf::add(pf::perf::Preempted, pf::perf::now() - start);
}

#if defined(__arm__)
extern "C" void rcYieldFromStub(win32::WinApplication* app, const x86::CPU* cpu, const void* host)
{
    win32::WinApplication::yieldFromStub(app, cpu, host);
}

// rc_yield_stub(r0 app, r1 cpu): yieldContext keeping r0-r3, r12, d0-d7, d16-d31 and the FPSCR, on a stack aligned
// to 8 bytes (r4 holds the caller's stack pointer); the caller's return address goes along for the watchdog samples.
asm(R"(
    .text
    .p2align 2
    .global rc_yield_stub
    .type rc_yield_stub, %function
    .thumb
    .thumb_func
rc_yield_stub:
    push {r0-r4, r12, lr}
    mov r4, sp
    bic r12, r4, #7
    mov sp, r12
    vpush {d0-d7}
    vpush {d16-d31}
    vmrs r2, fpscr
    push {r2, r3}
    mov r2, lr
    bl rcYieldFromStub
    pop {r2, r3}
    vmsr fpscr, r2
    vpop {d16-d31}
    vpop {d0-d7}
    mov sp, r4
    pop {r0-r4, r12, pc}
    .size rc_yield_stub, .-rc_yield_stub
)");
#endif

// Releases the execution lock and queues for it again when a thread of a higher level or a starved one waits. The lock
// orders its waiters by host priority, which follows the level, so the waiting thread of the highest priority runs
// next, until it blocks or yields itself. Threads without the lock and a lock held more than once do not yield.
__attribute__((always_inline)) inline void WinApplication::yieldContextFrom(const x86::CPU& cpu, const void* host)
{
    if (s_sampling.load(std::memory_order_relaxed))
        samplePreemptionPoint(cpu, host);
    if (getMemory<Mutex*>(cpu.efs + ThreadLockOffset) != m_executionContext || m_contextDepth != 1)
        return;
    yieldToHigher(*this, cpu);
}

__attribute__((noinline)) void WinApplication::yieldContext(const x86::CPU& cpu)
{
    yieldContextFrom(cpu, __builtin_return_address(0));
}

void WinApplication::yieldFromStub(WinApplication* app, const x86::CPU* cpu, const void* host)
{
    app->yieldContextFrom(*cpu, host);
}

void WinApplication::terminate()
{
    m_cpu.terminate = true;
}

// Watchdog thread: where the threads running guest code are, from their preemption points over SampleWaitMs;
// meanwhile a raised waiter count sends each allowPreemption call to yieldContext. Code addresses beside watchReport's.
static void logGuestSamples(std::atomic<int>& preemptWaiters, const void* anchor)
{
    const int SampleWaitMs = 300;
    for (std::atomic<bool>& ready : s_sampleReady)
        ready.store(false, std::memory_order_relaxed);
    s_sampleClaimed.store(0, std::memory_order_relaxed);
    s_samplePoints.store(0, std::memory_order_relaxed);
    s_sampling.store(true, std::memory_order_seq_cst);
    preemptWaiters.fetch_add(1, std::memory_order_seq_cst);
    for (int waited = 0; waited < SampleWaitMs && s_sampleClaimed.load(std::memory_order_relaxed) < SampleMax;
         waited += 10)
        pf::sleepMs(10);
    preemptWaiters.fetch_sub(1, std::memory_order_seq_cst);
    s_sampling.store(false, std::memory_order_seq_cst);
    char text[SampleMax * 56 + 1] = "";
    int length = 0;
    int count = 0;
    for (int i = 0; i < SampleMax; ++i)
    {
        if (!s_sampleReady[i].load(std::memory_order_acquire))
            continue;
        ++count;
        length += snprintf(text + length, sizeof(text) - length, " thread 0x%x host %p esp 0x%08x;",
                           unsigned(s_sampleThread[i]), s_sampleHost[i], s_sampleEsp[i]);
    }
    pf::logInfo("watchdog: guest code reached %u preemption point(s) in %d ms%s%s (watchReport at %p)",
                s_samplePoints.load(std::memory_order_relaxed), SampleWaitMs, count ? ":" : ": native code or shims",
                text, anchor);
}

// Watchdog thread; the lock fields are read without the lock, so they may be a moment old.
void WinApplication::watchReport()
{
    WinApplication* app = s_watchApp;
    if (!app)
        return;
    const ThreadControl* owner = s_lockOwner.load(std::memory_order_relaxed);
    SceKernelMutexInfo info;
    info.size = sizeof(info);
    const bool known = sceKernelGetMutexInfo(app->m_executionContext->kernelId(), &info) >= 0;
    pf::logInfo("watchdog: execution lock %s by guest thread 0x%x (kernel owner host 0x%x, %d waiting), depth %d, "
                "%d preempting thread(s) waiting", s_lockHeld.load(std::memory_order_relaxed) ? "held" : "free",
                owner ? unsigned(scheduler::guestId(owner)) : 0u, known ? unsigned(info.currentOwnerId) : 0u,
                known ? info.numWaitThreads : -1, app->m_contextDepth,
                app->m_preemptWaiters.load(std::memory_order_relaxed));
    logGuestSamples(app->m_preemptWaiters, reinterpret_cast<const void*>(&WinApplication::watchReport));
    scheduler::logThreads(app, owner);
    Event::logWaiters();
    MemMap::logInUse();
}

}
