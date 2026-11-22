#include "display.h"
#include "audioupdate.h"
#include "bench.h"
#include "census.h"
#include "framephases.h"
#include "guestlock.h"
#include "overlap.h"

#include <lib/memmap.h>
#include <lib/scheduler.h>
#include <lib/shimgate.h>
#include <lib/teb.h>
#include <native.h>
#include <platform/config.h>
#include <platform/cores.h>
#include <platform/log.h>
#include <platform/perf.h>
#include <platform/sync.h>
#include <recomp.h>
#include <psp2/kernel/threadmgr.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace nfsc
{

// Translated body of Main_DisplayFrame.
void sub_699230_guest(win32::WinApplication* app, x86::CPU& cpu);

namespace display
{

namespace
{

const x86::reg32 GuestStackSize = 1024 * 1024;
const uint32_t HostStackSize = 1024 * 1024;
// MainLoop's return address from its display call; game flow manager state, 6 while racing.
const x86::reg32 MainLoopDisplayReturn = 0x006b7bae;
const x86::reg32 GameFlowState = 0x00A99BBC;
const x86::reg32 FlowRacing = 6;

inline uint32_t readFpscr()
{
    uint32_t value;
    __asm__ volatile("vmrs %0, fpscr" : "=r"(value));
    return value;
}

inline void writeFpscr(uint32_t value)
{
    __asm__ volatile("vmsr fpscr, %0" : : "r"(value) : "memory");
}

// Priority of the calling host thread.
int currentHostPriority()
{
    SceKernelThreadInfo info;
    info.size = sizeof(info);
    if (sceKernelGetThreadInfo(sceKernelGetThreadId(), &info) < 0)
        return pf::Thread::PriorityNormal;
    return info.currentPriority;
}

int s_overlap = 0;
// Set by the game thread as an overlapped call starts, cleared at the barrier; the display thread sees it set.
std::atomic<bool> s_concurrent{false};
// Guest thread block of the display thread (0 before it exists).
std::atomic<x86::reg32> s_displayBlock{0};
// The Direct3D layer: the display thread holds it for an overlapped call, other threads for each Direct3D call.
pf::LwMutex s_deviceLock("display_device", true);
// Serialises barrier callers.
pf::LwMutex s_barrierLock("display_barrier", true);
// Game thread's waits for display calls in the current MainLoop frame (bench summary).
uint32_t s_gameWaitUs = 0;

// How a shim may run on the display thread without the execution lock.
enum ShimClass : uint8_t
{
    ShimHold,       // takes the execution lock
    ShimFree,       // safe without it
    ShimDevice      // Direct3D: the display thread owns the layer, other threads lock it
};

enum ShimToken
{
    TokenNone,
    TokenHeld,
    TokenDevice,
    TokenHeldDevice     // the audio thread: the Direct3D layer, then the execution lock
};

// Shims that touch no shared host state, or lock it themselves.
const char* const s_freeShims[] = {
    "<msvcr71>memcpy", "<msvcr71>memmove", "<msvcr71>memset", "<msvcr71>memcmp", "<msvcr71>strlen",
    "<msvcr71>strncpy", "<msvcr71>strcmp", "<msvcr71>strncmp", "<msvcr71>strchr", "<msvcr71>strrchr",
    "<msvcr71>strstr", "<msvcr71>strcspn", "<msvcr71>strncat", "<msvcr71>strcoll", "<msvcr71>_strnicmp",
    "<msvcr71>_strcmpi", "<msvcr71>tolower", "<msvcr71>toupper", "<msvcr71>isdigit", "<msvcr71>isspace",
    "<msvcr71>isprint", "<msvcr71>abs", "<msvcr71>floor", "<msvcr71>ceil", "<msvcr71>sqrt", "<msvcr71>cos",
    "<msvcr71>_CIacos", "<msvcr71>_CIasin", "<msvcr71>_CIpow", "<msvcr71>_CIfmod", "<msvcr71>_CIcosh",
    "<msvcr71>_CIsinh", "<msvcr71>_CItanh", "<msvcr71>_isnan", "<msvcr71>atoi", "<msvcr71>strtol",
    "<msvcr71>strtoul", "<msvcr71>sprintf", "<msvcr71>_snprintf", "<msvcr71>vsprintf", "<msvcr71>_vsnprintf",
    "<msvcr71>?vswprintf@@YAHPAGIPBGPAD@Z", "<msvcr71>wcslen", "<msvcr71>wcscpy", "<msvcr71>_mbscpy",
    "<msvcr71>_mbscat", "<msvcr71>sscanf", "<msvcr71>malloc", "<msvcr71>free", "<msvcr71>calloc",
    "<msvcr71>realloc", "<msvcr71>_aligned_malloc", "<msvcr71>_aligned_free", "<msvcr71>_aligned_realloc",
    "<msvcr71>longjmp", "<msvcr71>_setjmp3", "<msvcr71>qsort", "<msvcr71>bsearch",
    "<kernel32>QueryPerformanceCounter", "<kernel32>QueryPerformanceFrequency", "<kernel32>GetTickCount",
    "<kernel32>GetCurrentThreadId", "<kernel32>GetCurrentThread", "<kernel32>GetCurrentProcess",
    "<kernel32>GetCurrentProcessId", "<kernel32>InterlockedExchange", "<kernel32>InterlockedDecrement",
    "<kernel32>InterlockedIncrement", "<kernel32>InterlockedCompareExchange", "<kernel32>InterlockedExchangeAdd",
    "<kernel32>EnterCriticalSection", "<kernel32>LeaveCriticalSection", "<kernel32>TryEnterCriticalSection",
    "<kernel32>Sleep", "<kernel32>SleepEx", "<kernel32>GetLastError",
    "<user32>GetClientRect", "<user32>GetWindowRect", "<user32>GetCursorPos",
    "<winmm>timeGetTime",
    "<d3dx9_30>D3DXVec3Transform", "<d3dx9_30>D3DXMatrixInverse", "<d3dx9_30>D3DXVec4Transform",
    "<d3dx9_30>D3DXMatrixPerspectiveLH", "<d3dx9_30>D3DXMatrixTranslation", "<d3dx9_30>D3DXVec3TransformCoordArray",
    "<d3dx9_30>D3DXVec3Normalize", "<d3dx9_30>D3DXVec3TransformNormal", "<d3dx9_30>D3DXMatrixMultiply",
    "<d3dx9_30>D3DXMatrixOrthoLH", "<d3dx9_30>D3DXMatrixTranspose",
};

ShimClass classOf(const std::string& name)
{
    for (const char* free : s_freeShims)
    {
        if (name == free)
            return ShimFree;
    }
    if (name.compare(0, 9, "IDirect3D") == 0 || name.compare(0, 5, "ID3DX") == 0 ||
        name.compare(0, 6, "<d3d9>") == 0 || name.compare(0, 10, "<d3dx9_30>") == 0)
        return ShimDevice;
    return ShimHold;
}

struct ShimEntry
{
    win32::MethodPtr method;
    ShimClass kind;
    const char* name;   // the registered name (the method table keeps it)
};

// Every import and interface method, sorted by method; others (registered later) take the lock.
std::vector<ShimEntry> s_shimClasses;

void classifyShims(win32::WinApplication* app)
{
    app->forEachMethod([](const std::string& name, win32::MethodPtr method)
    {
        if (!name.empty() && (name[0] == '<' || name[0] == 'I'))
            s_shimClasses.push_back(ShimEntry{ method, classOf(name), name.c_str() });
    });
    std::sort(s_shimClasses.begin(), s_shimClasses.end(), [](const ShimEntry& a, const ShimEntry& b)
              { return reinterpret_cast<uintptr_t>(a.method) < reinterpret_cast<uintptr_t>(b.method); });
}

const ShimEntry* shimEntry(win32::MethodPtr shim)
{
    const auto it = std::lower_bound(s_shimClasses.begin(), s_shimClasses.end(), shim,
                                     [](const ShimEntry& entry, win32::MethodPtr key)
                                     { return reinterpret_cast<uintptr_t>(entry.method) <
                                              reinterpret_cast<uintptr_t>(key); });
    return it != s_shimClasses.end() && it->method == shim ? &*it : nullptr;
}

ShimClass shimClass(win32::MethodPtr shim)
{
    const ShimEntry* entry = shimEntry(shim);
    return entry ? entry->kind : ShimHold;
}

// perf_log: the Direct3D call another thread is in, and since when (under the device lock, so one at a time).
win32::MethodPtr s_deviceShim = nullptr;
uint32_t s_deviceShimStart = 0;

// Shim gate while an overlapped call or an audio update runs: the display thread takes the execution lock for shims
// that need it, other threads take the Direct3D layer for Direct3D calls while a call overlaps; the audio thread takes
// the execution lock for every shim that is not free, and the Direct3D layer first for Direct3D calls. A shim's class
// is looked up once and kept in its slot.
int shimEnter(win32::WinApplication* app, const x86::CPU& cpu, win32::MethodPtr shim, win32::shimgate::Slot& slot)
{
    const bool display = cpu.efs == s_displayBlock.load(std::memory_order_relaxed);
    const bool audio = !display && audioupdate::onAudioThread(cpu);
    if (!display && !audio && !s_concurrent.load(std::memory_order_acquire))
        return TokenNone;
    int kind = slot.load(std::memory_order_relaxed);
    if (kind < 0)
    {
        kind = shimClass(shim);
        slot.store(int8_t(kind), std::memory_order_relaxed);
    }
    if (audio)
    {
        if (kind == ShimFree)
            return TokenNone;
        const bool device = kind == ShimDevice;
        if (device)
            s_deviceLock.lock();
        if (!app->holdContext(cpu))
        {
            if (device)
                s_deviceLock.unlock();
            return TokenNone;
        }
        return device ? TokenHeldDevice : TokenHeld;
    }
    if (display)
    {
        if (kind != ShimHold)
            return TokenNone;
        const uint32_t start = pf::perf::enabled() ? pf::perf::now() : 0;
        if (!app->holdContext(cpu))
            return TokenNone;
        if (pf::perf::enabled())
        {
            pf::perf::overlap(pf::perf::OverlapHolds, 1);
            pf::perf::overlap(pf::perf::OverlapHoldUs, pf::perf::now() - start);
        }
        return TokenHeld;
    }
    if (kind != ShimDevice)
        return TokenNone;
    const uint32_t start = pf::perf::enabled() ? pf::perf::now() : 0;
    const bool waited = !s_deviceLock.tryLock();
    if (waited)
        lockReleasingContext(s_deviceLock, app, cpu, true);
    if (waited && pf::perf::enabled())
    {
        const ShimEntry* entry = shimEntry(shim);
        pf::perf::deviceWait(entry ? entry->name : "?", rc::ld32(cpu.esp), pf::perf::now() - start);
    }
    if (pf::perf::enabled() && !s_deviceShim)
    {
        // perf_log: the time of the call itself, without the wait, for the slow-frame line.
        s_deviceShim = shim;
        s_deviceShimStart = pf::perf::now();
    }
    return TokenDevice;
}

void shimLeave(win32::WinApplication* app, const x86::CPU& cpu, int token)
{
    if (token == TokenHeld || token == TokenHeldDevice)
        app->releaseHeldContext(cpu);
    if (token == TokenDevice && s_deviceShim)
    {
        const ShimEntry* entry = shimEntry(s_deviceShim);
        phases::gameDevice(entry ? entry->name : "?", pf::perf::now() - s_deviceShimStart);
        s_deviceShim = nullptr;
    }
    if (token == TokenDevice || token == TokenHeldDevice)
        s_deviceLock.unlock();
}

// The host thread that runs the game thread's display calls, with its own guest thread block and stack.
class DisplayThread
{
public:
    DisplayThread(win32::WinApplication* app, const x86::CPU& caller, int cpuMask)
        :   m_app(app)
        ,   m_block(win32::teb::Size)
        ,   m_stack(GuestStackSize)
        ,   m_cpu(caller)
        ,   m_fpscr(0)
        ,   m_executionLock(nullptr)
        ,   m_free(false)
        ,   m_stop(false)
        ,   m_go("display_go", 0)
        ,   m_done("display_done", 0)
        ,   m_thread("display", &DisplayThread::entry, this, currentHostPriority(), HostStackSize, cpuMask)
    {
        using namespace win32;
        const x86::reg32 block = m_block.getBlockStart();
        memset(rc::host(block), 0, teb::Size);
        rc::st32(block + teb::ExceptionList, 0xffffffff);
        rc::st32(block + teb::StackBase, stackTop());
        rc::st32(block + teb::StackLimit, m_stack.getBlockStart());
        rc::st32(block + teb::Self, block);
        rc::st32(block + teb::ProcessId, rc::ld32(caller.efs + teb::ProcessId));
        // The game thread's id: code that checks for the main thread sees it.
        rc::st32(block + teb::ThreadId, rc::ld32(caller.efs + teb::ThreadId));
        m_executionLock = app->getMemory<Mutex*>(caller.efs + teb::HostLock);
        app->getMemory<Mutex*>(block + teb::HostLock) = m_executionLock;
        s_displayBlock.store(block);
        m_thread.start();
    }

    ~DisplayThread()
    {
        m_stop = true;
        m_go.post();
        m_thread.join();
        s_displayBlock.store(0);
    }

    // Game thread, holding the execution lock once: runs the call on the display thread and waits for it.
    void call(x86::CPU& cpu)
    {
        using namespace win32;
        const x86::reg32 block = m_block.getBlockStart();
        prepare(cpu, false);
        const uint32_t start = pf::perf::now();
        pf::perf::blocking(pf::perf::BlockOther);
        m_app->unlockContext(cpu);
        m_go.post();
        m_done.wait();
        m_app->lockContext(cpu);
        s_gameWaitUs += pf::perf::now() - start;
        memcpy(rc::host(cpu.efs + teb::TlsSlots), rc::host(block + teb::TlsSlots), 4 * teb::TlsSlotCount);
        rc::st32(cpu.efs + teb::LastError, rc::ld32(block + teb::LastError));
        const x86::reg32 esp = cpu.esp;
        const x86::reg32 efs = cpu.efs;
        const bool terminate = cpu.terminate || m_cpu.terminate;
        cpu = m_cpu;
        cpu.esp = esp + 4;
        cpu.efs = efs;
        cpu.terminate = terminate;
    }

    // Game thread, holding the execution lock: takes the call for the display thread to run without the execution
    // lock; start() runs it, wait() ends it.
    void take(const x86::CPU& cpu)
    {
        prepare(cpu, true);
    }

    void start()
    {
        std::atomic_thread_fence(std::memory_order_seq_cst);
        m_go.post();
    }

    void wait()
    {
        m_done.wait();
        std::atomic_thread_fence(std::memory_order_seq_cst);
    }

    bool terminated() const { return m_cpu.terminate; }

private:
    DisplayThread(const DisplayThread&) = delete;
    DisplayThread& operator=(const DisplayThread&) = delete;

    x86::reg32 stackTop() const
    {
        return m_stack.getBlockStart() + m_stack.getBlockSize();
    }

    // The caller's state, TLS and last error on the display thread's block; free: without the execution lock.
    void prepare(const x86::CPU& cpu, bool free)
    {
        using namespace win32;
        const x86::reg32 block = m_block.getBlockStart();
        memcpy(rc::host(block + teb::TlsSlots), rc::host(cpu.efs + teb::TlsSlots), 4 * teb::TlsSlotCount);
        rc::st32(block + teb::LastError, rc::ld32(cpu.efs + teb::LastError));
        rc::st32(block + teb::ExceptionList, 0xffffffff);
        m_app->getMemory<Mutex*>(block + teb::HostLock) = free ? nullptr : m_executionLock;
        m_free = free;
        m_cpu = cpu;
        m_cpu.efs = block;
        m_cpu.terminate = false;
        // The caller's return address above an empty stack, as after its call instruction.
        m_cpu.esp = stackTop() - 8;
        rc::st32(m_cpu.esp, rc::ld32(cpu.esp));
        m_fpscr = readFpscr();
    }

    static int entry(void* self)
    {
        static_cast<DisplayThread*>(self)->loop();
        return 0;
    }

    void loop()
    {
        using namespace win32;
        // Registered with the game thread's guest id (scheduler::currentControl).
        ThreadControl* const control = scheduler::currentControl();
        scheduler::setCurrentDisplayThread(s_overlap != 0);
        // A waiting display thread makes the game thread give up the execution lock at its next preemption point.
        if (s_overlap)
            scheduler::raiseCurrentLevel(scheduler::LevelAboveNormal);
        m_app->getMemory<ThreadControl*>(m_block.getBlockStart() + teb::HostControl) = control;
        census::displayThreadStarted();
        pf::perf::setDisplayThread(pf::Thread::currentId());
        for (;;)
        {
            m_go.wait();
            std::atomic_thread_fence(std::memory_order_seq_cst);
            if (m_stop)
                break;
            writeFpscr(m_fpscr);
            const uint32_t start = pf::perf::now();
            if (m_free)
            {
                scheduler::watchFree(control, m_cpu);
                s_deviceLock.lock();
                sub_699230_guest(m_app, m_cpu);
                s_deviceLock.unlock();
                if (pf::perf::enabled())
                    pf::perf::overlap(pf::perf::OverlapDisplayUs, pf::perf::now() - start);
            }
            else
            {
                m_app->lockContext(m_cpu);
                sub_699230_guest(m_app, m_cpu);
                m_app->unlockContext(m_cpu);
            }
            bench::sample(bench::SeriesDisplay, pf::perf::now() - start);
            phases::displayDone(pf::perf::now() - start);
            scheduler::watchState(control, scheduler::WatchIdle);
            std::atomic_thread_fence(std::memory_order_seq_cst);
            m_done.post();
        }
    }

    win32::WinApplication*  m_app;
    win32::MemMap           m_block;
    win32::MemMap           m_stack;
    x86::CPU                m_cpu;
    uint32_t                m_fpscr;
    win32::Mutex*           m_executionLock;
    bool                    m_free;
    bool                    m_stop;
    pf::Semaphore           m_go;
    pf::Semaphore           m_done;
    pf::Thread              m_thread;
};

bool s_parallel = false;
int s_cpuMask = pf::Thread::Cpu2;
DisplayThread* s_thread = nullptr;
// A call taken for overlap, started when its MainLoop frame ends (game thread only).
bool s_pending = false;
// perf_log: when the running overlapped call started.
uint32_t s_startUs = 0;

// Game thread: runs the taken call beside it.
void startPending()
{
    if (!s_pending)
        return;
    s_pending = false;
    overlap::starting();
    census::overlapStart();
    if (pf::perf::enabled())
    {
        pf::perf::overlap(pf::perf::OverlapCalls, 1);
        s_startUs = pf::perf::now();
    }
    s_concurrent.store(true, std::memory_order_seq_cst);
    win32::shimgate::g_open.fetch_add(1, std::memory_order_relaxed);
    s_thread->start();
}

// An overlapped call: from MainLoop, in a race frame unless display_overlap=2.
bool overlapCall(const x86::CPU& cpu)
{
    return s_overlap && !cpu.terminate && rc::ld32(cpu.esp) == MainLoopDisplayReturn &&
           (s_overlap == 2 || rc::ld32(GameFlowState) == FlowRacing);
}

// display_overlap=3: the frame's display call started after its first flush; MainLoop's call then returns at once.
bool s_startedEarly = false;
// MainLoop's flag that skips the frame's display call.
const x86::reg32 SkipDisplay = 0x00a995d4;

}

void configure()
{
    s_overlap = std::max(0, std::min(4, pf::Config::getInt("display_overlap", 4)));
    if (s_overlap && census::recordsFrames())
    {
        pf::logError("display: display_overlap is off while display_census 1-3 records frames");
        s_overlap = 0;
    }
    s_parallel = pf::Config::getBool("display_parallel", true) || s_overlap;
    if (s_overlap || audioupdate::enabled())
        win32::shimgate::install(&shimEnter, &shimLeave);
    if (!s_parallel)
        return;
    int core = pf::Config::getInt("display_core", 2);
    if (core < 0 || core > 3 || (core == 3 && !pf::cores::hasCore3()))
        core = 2;
    s_cpuMask = pf::Thread::Cpu0 << core;
    pf::logInfo("display: Main_DisplayFrame runs on its own thread on core %d, %s", core,
                s_overlap == 4 ? "beside the game thread from each race frame's front end update"
                : s_overlap == 3 ? "beside the game thread from the end of each race frame's first flush"
                : s_overlap == 2 ? "beside the game thread's next frame in every frame"
                : s_overlap == 1 ? "beside the game thread's next frame in race frames"
                : "in turn with the game thread");
}

void shutdown()
{
    delete s_thread;
    s_thread = nullptr;
}

bool overlapEnabled()
{
    return s_overlap != 0;
}

void frameEnd(const x86::CPU& cpu)
{
    if (cpu.terminate)
        s_pending = false;
    s_startedEarly = false;
    startPending();
}

void startEarly(win32::WinApplication* app, x86::CPU& cpu, int mode)
{
    if (s_overlap != mode || !s_thread || s_concurrent.load(std::memory_order_relaxed) || s_pending ||
        cpu.terminate || app->contextDepth() != 1 || rc::ld32(GameFlowState) != FlowRacing || rc::ld8(SkipDisplay))
        return;
    if (s_shimClasses.empty())
        classifyShims(app);
    s_thread->take(cpu);
    s_pending = true;
    startPending();
    s_startedEarly = true;
}

uint32_t takeGameWaits()
{
    const uint32_t waits = s_gameWaitUs;
    s_gameWaitUs = 0;
    return waits;
}

bool concurrent()
{
    return s_concurrent.load(std::memory_order_acquire);
}

bool onDisplayThread(const x86::CPU& cpu)
{
    return cpu.efs == s_displayBlock.load(std::memory_order_relaxed);
}

void barrier(win32::WinApplication* app, x86::CPU& cpu, bool early)
{
    if (!s_concurrent.load(std::memory_order_acquire) || onDisplayThread(cpu))
        return;
    const GuestLockGuard guard(s_barrierLock, app, cpu);
    if (!s_concurrent.load(std::memory_order_acquire))
        return;
    if (early && pf::perf::enabled())
        pf::perf::overlap(pf::perf::OverlapEarlyBarriers, 1);
    const uint32_t start = pf::perf::now();
    pf::perf::blocking(pf::perf::BlockOther);
    {
        const ContextRelease release(app, cpu);
        s_thread->wait();
    }
    s_gameWaitUs += pf::perf::now() - start;
    phases::barrier(pf::perf::now() - start);
    if (pf::perf::enabled())
    {
        pf::perf::overlap(pf::perf::OverlapWindowUs, start - s_startUs);
        pf::perf::overlap(pf::perf::OverlapBarrierUs, pf::perf::now() - start);
    }
    closeShimGate();
    s_concurrent.store(false, std::memory_order_seq_cst);
    if (s_thread->terminated())
        cpu.terminate = true;
    census::overlapEnd();
    overlap::drain(app, cpu);
}

void openShimGate(win32::WinApplication* app)
{
    if (s_shimClasses.empty())
        classifyShims(app);
    win32::shimgate::g_open.fetch_add(1, std::memory_order_relaxed);
}

void closeShimGate()
{
    win32::shimgate::g_open.fetch_sub(1, std::memory_order_relaxed);
}

}

namespace native
{

// Main_DisplayFrame: on the display thread with display_parallel=1, else here; with display_overlap the game thread
// returns at once from an overlapped call. A game thread holding the execution lock more than once keeps the call.
void sub_699230(win32::WinApplication* app, x86::CPU& cpu)
{
    if (display::s_startedEarly)
    {
        display::s_startedEarly = false;
        cpu.esp += 4;
        return;
    }
    if (display::s_parallel && app->contextDepth() == 1)
    {
        display::startPending();
        display::barrier(app, cpu, false);
        if (!display::s_thread)
            display::s_thread = new display::DisplayThread(app, cpu, display::s_cpuMask);
        if (display::overlapCall(cpu))
        {
            if (display::s_shimClasses.empty())
                display::classifyShims(app);
            display::s_thread->take(cpu);
            display::s_pending = true;
            cpu.esp += 4;
            return;
        }
        census::displayStart();
        display::s_thread->call(cpu);
        census::displayEnd();
        return;
    }
    census::displayStart();
    const uint32_t start = pf::perf::now();
    sub_699230_guest(app, cpu);
    const uint32_t length = pf::perf::now() - start;
    display::s_gameWaitUs += length;
    bench::sample(bench::SeriesDisplay, length);
    phases::displayDone(length);
    census::displayEnd();
}

}
}
