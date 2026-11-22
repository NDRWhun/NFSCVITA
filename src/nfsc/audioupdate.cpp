// MainLoop calls EAXSound::Update (0x521fe0, from 0x6b7b65) after its second event flush: the AEMS bank loader, sound
// cameras, the stream manager, speech, the sound objects' updates (cars, effects, music) and the dynamic mix, which
// pass their parameters to the audio system through the Csis interface under the System lock (critical section
// 0x00bc9a50) the mixer holds for each block service. With audio_parallel the game thread hands the race frame's
// update to the audio thread and goes on with the frame: the update reads what the sim and the frame's updates left
// (car sound states, cameras, timers), and nothing the game thread runs before the barrier writes that (loading
// callbacks, unloads and other audio updates wait for it first). Sound parameters reach the mixer up to one frame
// later than when the game thread ran the update itself.

#include "audioupdate.h"
#include "census.h"
#include "display.h"
#include "guestcall.h"
#include "guestlock.h"
#include "mixsplit.h"

#include <lib/memmap.h>
#include <lib/mutex.h>
#include <lib/scheduler.h>
#include <lib/teb.h>
#include <native.h>
#include <platform/config.h>
#include <platform/cores.h>
#include <platform/log.h>
#include <platform/sync.h>
#include <recomp.h>
#include <winapi/kernel32.h>
#include <psp2/kernel/threadmgr.h>

#include <algorithm>
#include <atomic>
#include <cstring>

namespace nfsc
{

// Translated bodies of the overridden guest functions.
void sub_521fe0_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_521860_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_8cf74b_guest(win32::WinApplication* app, x86::CPU& cpu);
// Csis::System::Lock (through its override) and Unlock.
void sub_8cf74b(win32::WinApplication* app, x86::CPU& cpu);
void sub_8cf75d(win32::WinApplication* app, x86::CPU& cpu);

namespace audioupdate
{

namespace
{

const x86::reg32 GuestStackSize = 512 * 1024;
const uint32_t HostStackSize = 1024 * 1024;
// MainLoop's return address from its audio update; game flow manager state, 6 while racing.
const x86::reg32 MainLoopAudioReturn = 0x006b7b6a;
const x86::reg32 GameFlowState = 0x00A99BBC;
const x86::reg32 FlowRacing = 6;
// IsSoundEnabled.
const x86::reg32 SoundEnabled = 0x00a631b8;
// The update's rare branches, which stay on the game thread: the in-game reinitialisation (dynamic mix map, sound
// objects, speech purge, music restart) and the countdown revving start (posts a Hermes message).
const x86::reg32 InGameReinit = 0x00a8ba74;
const x86::reg32 CountdownRevs = 0x00a8ba75;
// The update's early outs (skipped frames, a one-frame skip, a pending state), the dt it needs (at least the float
// at 0x9c248c) and the flag of its speech deduction before the bank loader.
const x86::reg32 SkipFrames = 0x00a8ba2c;
const x86::reg32 SkipOnce = 0x00a8ba28;
const x86::reg32 Pending = 0x00a8ba94;
const x86::reg32 MinimumDt = 0x009c248c;
const x86::reg32 SpeechFlags = 0x00a8bae4;
const x86::reg32 SpeechDeduce = 0x20;
// EAXAemsManager, the update's bank loader.
const x86::reg32 AemsManager = 0x00a8c148;
// Joylog::ReplayingFlag and CapturingFlag: the audio update reads and records joylog data in the frame's order.
const x86::reg32 JoylogReplaying = 0x00a99660;
const x86::reg32 JoylogCapturing = 0x00a99664;
// Csis::System's critical section, the audio System lock (Csis::System::Lock 0x8cf74b; the System's lock hooks
// 0x89cb10/0x89cb15 take it too).
const x86::reg32 AudioSystemLock = 0x00bc9a50;
// The EA mutex's lock count (0x8d8640 adds one per take, 0x8d8700 takes one off).
const x86::reg32 AudioLockCount = 0x20;
// The game's code, for the first game caller on a waiting game thread's stack.
const x86::reg32 GameCodeStart = 0x00401000;
const x86::reg32 GameCodeEnd = 0x00860000;
// The libraries after it (audio, Csis, EA core), and the EA mutex's return from EnterCriticalSection.
const x86::reg32 LibraryCodeEnd = 0x00990000;
const x86::reg32 EaMutexReturn = 0x008d865e;
const int CallerScanWords = 96;
// Polls for the audio thread's next event before a kernel wait (the hand-over usually comes within microseconds).
const int EventSpinTries = 2048;

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

// CPU time of the calling host thread in microseconds.
uint64_t threadCpuUs()
{
    SceKernelThreadInfo info;
    info.size = sizeof(info);
    if (sceKernelGetThreadInfo(sceKernelGetThreadId(), &info) < 0)
        return 0;
    return info.runClocks;
}

int s_mode = 0;
// Set by the game thread as an update starts beside it, cleared once it has ended.
std::atomic<bool> s_concurrent{false};
// Guest thread block of the audio thread (0 before it exists).
std::atomic<x86::reg32> s_audioBlock{0};
// Game thread only: running a call for the audio thread; its host thread id.
bool s_serving = false;
int s_gameThread = 0;
// Game thread only (perf_log): what it is doing.
pf::perf::AudioLockPart s_part = pf::perf::AudioLockOther;
// audio_lock_batch: the update holds the audio System lock from start to end (taken once, the inner takes nest).
bool s_batch = false;
// Set by the game thread before it starts an update whose bank loader it ran itself; the audio thread skips it.
bool s_aemsRan = false;

// The host thread holding the batched lock (0: none), and whether a wait of its gave the lock up meanwhile.
std::atomic<int> s_batchOwner{0};
bool s_batchReleased = false;

// Takes the audio System lock for a whole update (counted for perf_log), and gives it back.
uint32_t batchLock(win32::WinApplication* app, x86::CPU& cpu)
{
    const bool timed = pf::perf::enabled();
    const uint32_t start = timed ? pf::perf::now() : 0;
    callGuest(app, cpu, &sub_8cf74b, cpu.ecx, {});
    s_batchOwner.store(pf::Thread::currentId(), std::memory_order_relaxed);
    const uint32_t taken = timed ? pf::perf::now() : 0;
    if (timed)
        pf::perf::audio(pf::perf::AudioBatchWaitUs, taken - start);
    return taken;
}

void batchUnlock(win32::WinApplication* app, x86::CPU& cpu, uint32_t taken)
{
    s_batchOwner.store(0, std::memory_order_relaxed);
    callGuest(app, cpu, &sub_8cf75d, cpu.ecx, {});
    if (pf::perf::enabled())
        pf::perf::audio(pf::perf::AudioBatchHeldUs, pf::perf::now() - taken);
}

// The host mutex behind the audio System lock's critical section.
win32::Mutex* audioSystemMutex()
{
    return *reinterpret_cast<win32::Mutex**>(rc::host(AudioSystemLock));
}

// Wait hook: while the batch's owner blocks (another lock, Sleep, an object) and only the batch holds the audio
// System lock (the EA mutex's count is 1), the lock is free for the others: inside the update the game code may wait
// for a thread that needs it (the stream service takes its channel lock, then this one; the update polls the channel
// lock with Sleep). It is taken back after the wait, the execution lock released while that waits.
void batchWait(win32::WinApplication* app, const x86::CPU& cpu, bool waiting)
{
    if (s_batchOwner.load(std::memory_order_relaxed) != pf::Thread::currentId())
        return;
    if (waiting)
    {
        if (rc::ld32(AudioSystemLock + AudioLockCount) != 1)
            return;
        s_batchReleased = true;
        audioSystemMutex()->unlock();
        if (pf::perf::enabled())
            pf::perf::audio(pf::perf::AudioBatchReleases, 1);
        return;
    }
    if (!s_batchReleased)
        return;
    s_batchReleased = false;
    win32::Mutex* const mutex = audioSystemMutex();
    if (mutex->tryLock())
        return;
    const uint32_t start = pf::perf::enabled() ? pf::perf::now() : 0;
    {
        const ContextRelease release(app, cpu);
        mutex->lock();
    }
    if (pf::perf::enabled())
        pf::perf::audio(pf::perf::AudioBatchWaitUs, pf::perf::now() - start);
}

// The host thread that runs the audio updates, with its own guest thread block and stack. Events go one way at a
// time: the audio thread posts a call for the game thread (and waits for it to run) or the update's end, and the
// game thread takes each in next().
class AudioThread
{
public:
    AudioThread(win32::WinApplication* app, const x86::CPU& game, int priority, int cpuMask)
        :   m_app(app)
        ,   m_block(win32::teb::Size)
        ,   m_stack(GuestStackSize)
        ,   m_guestId(rc::ld32(game.efs + win32::teb::ThreadId))
        ,   m_cpu(game)
        ,   m_fpscr(0)
        ,   m_call(nullptr)
        ,   m_callCpu(game)
        ,   m_batchHeld(false)
        ,   m_batchTaken(0)
        ,   m_callPosted(false)
        ,   m_ended(false)
        ,   m_stop(false)
        ,   m_ready("audio_ready", 0)
        ,   m_go("audio_go", 0)
        ,   m_event("audio_event", 0)
        ,   m_served("audio_served", 0)
        ,   m_thread("audio_update", &AudioThread::entry, this, priority, HostStackSize, cpuMask)
    {
        using namespace win32;
        const x86::reg32 block = m_block.getBlockStart();
        memset(rc::host(block), 0, teb::Size);
        rc::st32(block + teb::ExceptionList, 0xffffffff);
        rc::st32(block + teb::StackBase, stackTop());
        rc::st32(block + teb::StackLimit, m_stack.getBlockStart());
        rc::st32(block + teb::Self, block);
        rc::st32(block + teb::ProcessId, rc::ld32(game.efs + teb::ProcessId));
        // The game thread's id: code that checks for the main thread sees it.
        rc::st32(block + teb::ThreadId, m_guestId);
        app->getMemory<Mutex*>(block + teb::HostLock) = nullptr;
        s_audioBlock.store(block);
        m_thread.start();
        // Registered (scheduler, census marker) before its first update.
        m_ready.wait();
    }

    ~AudioThread()
    {
        m_stop.store(true, std::memory_order_seq_cst);
        m_go.post();
        m_thread.join();
        s_audioBlock.store(0);
    }

    // Game thread: runs EAXSound::Update on the audio thread with the caller's registers, its this and its argument
    // above a copy of its return address.
    void start(const x86::CPU& cpu)
    {
        using namespace win32;
        const x86::reg32 block = m_block.getBlockStart();
        memcpy(rc::host(block + teb::TlsSlots), rc::host(cpu.efs + teb::TlsSlots), 4 * teb::TlsSlotCount);
        rc::st32(block + teb::LastError, rc::ld32(cpu.efs + teb::LastError));
        rc::st32(block + teb::ExceptionList, 0xffffffff);
        m_cpu = cpu;
        m_cpu.efs = block;
        m_cpu.terminate = false;
        m_cpu.esp = stackTop() - 16;
        rc::st32(m_cpu.esp, rc::ld32(cpu.esp));
        rc::st32(m_cpu.esp + 4, rc::ld32(cpu.esp + 4));
        m_fpscr = readFpscr();
        std::atomic_thread_fence(std::memory_order_seq_cst);
        m_go.post();
    }

    // Game thread, holding the execution lock once: waits for the audio thread's next event (the lock released
    // unless it comes within a short poll). Runs a call the audio thread handed over and returns false, or returns
    // true once the update has ended.
    bool next(win32::WinApplication* app, x86::CPU& cpu)
    {
        bool ready = false;
        for (int i = 0; i < EventSpinTries && !ready; ++i)
        {
            ready = m_callPosted.load(std::memory_order_acquire) || m_ended.load(std::memory_order_acquire);
            if (!ready)
                __asm__ volatile("yield");
        }
        if (ready)
        {
            m_event.wait();
        }
        else
        {
            pf::perf::blocking(pf::perf::BlockOther);
            const ContextRelease release(app, cpu);
            m_event.wait();
        }
        std::atomic_thread_fence(std::memory_order_seq_cst);
        if (m_callPosted.exchange(false, std::memory_order_acq_rel))
        {
            runCall(app, cpu);
            m_served.post();
            return false;
        }
        m_ended.store(false, std::memory_order_relaxed);
        return true;
    }

    // Audio thread: the game thread runs function on this state; returns with its result.
    // The batched audio System lock is given up meanwhile: the call may take it on the game thread.
    void handOver(x86::CPU& cpu, win32::MethodPtr function)
    {
        if (m_batchHeld)
            batchUnlock(m_app, cpu, m_batchTaken);
        m_call = function;
        m_callCpu = cpu;
        m_callPosted.store(true, std::memory_order_seq_cst);
        m_event.post();
        m_served.wait();
        std::atomic_thread_fence(std::memory_order_seq_cst);
        cpu = m_callCpu;
        if (m_batchHeld)
            m_batchTaken = batchLock(m_app, cpu);
    }

    bool terminated() const { return m_cpu.terminate; }

private:
    AudioThread(const AudioThread&) = delete;
    AudioThread& operator=(const AudioThread&) = delete;

    x86::reg32 stackTop() const
    {
        return m_stack.getBlockStart() + m_stack.getBlockSize();
    }

    // The handed-over call on the game thread's block, which holds the execution lock; the audio thread's stack.
    void runCall(win32::WinApplication* app, x86::CPU& cpu)
    {
        const PartScope part(pf::perf::AudioLockUpdate);
        x86::CPU guest = m_callCpu;
        const x86::reg32 efs = guest.efs;
        guest.efs = cpu.efs;
        s_serving = true;
        m_call(app, guest);
        s_serving = false;
        guest.efs = efs;
        if (guest.terminate)
            cpu.terminate = true;
        m_callCpu = guest;
        if (pf::perf::enabled())
            pf::perf::audio(pf::perf::AudioServed, 1);
    }

    static int entry(void* self)
    {
        static_cast<AudioThread*>(self)->loop();
        return 0;
    }

    void loop()
    {
        using namespace win32;
        ThreadControl* const control = scheduler::setCurrentWorkerThread(m_guestId, "audio");
        // Its shims that need the execution lock make the game thread give it up at its next preemption point.
        scheduler::raiseCurrentLevel(scheduler::LevelAboveNormal);
        m_app->getMemory<ThreadControl*>(m_block.getBlockStart() + teb::HostControl) = control;
        census::audioThreadStarted();
        m_ready.post();
        for (;;)
        {
            m_go.wait();
            std::atomic_thread_fence(std::memory_order_seq_cst);
            if (m_stop.load(std::memory_order_seq_cst))
                break;
            writeFpscr(m_fpscr);
            const bool timed = pf::perf::enabled();
            const uint32_t start = timed ? pf::perf::now() : 0;
            const uint64_t cpuStart = timed ? threadCpuUs() : 0;
            scheduler::watchFree(control, m_cpu);
            m_batchHeld = s_batch;
            if (m_batchHeld)
                m_batchTaken = batchLock(m_app, m_cpu);
            sub_521fe0_guest(m_app, m_cpu);
            if (m_batchHeld)
                batchUnlock(m_app, m_cpu, m_batchTaken);
            m_batchHeld = false;
            scheduler::watchState(control, scheduler::WatchIdle);
            if (timed)
            {
                pf::perf::audio(pf::perf::AudioThreadUs, pf::perf::now() - start);
                pf::perf::audio(pf::perf::AudioThreadCpuUs, uint32_t(threadCpuUs() - cpuStart));
            }
            m_ended.store(true, std::memory_order_seq_cst);
            m_event.post();
        }
    }

    win32::WinApplication*      m_app;
    win32::MemMap               m_block;
    win32::MemMap               m_stack;
    const x86::reg32            m_guestId;
    // The audio thread's guest state; the hang watchdog reads it.
    x86::CPU                    m_cpu;
    uint32_t                    m_fpscr;
    win32::MethodPtr            m_call;
    x86::CPU                    m_callCpu;
    // Audio thread only: the update holds the batched audio System lock, taken at this time.
    bool                        m_batchHeld;
    uint32_t                    m_batchTaken;
    std::atomic<bool>           m_callPosted;
    std::atomic<bool>           m_ended;
    std::atomic<bool>           m_stop;
    pf::Semaphore               m_ready;
    pf::Semaphore               m_go;
    pf::Semaphore               m_event;
    pf::Semaphore               m_served;
    pf::Thread                  m_thread;
};

AudioThread* s_thread = nullptr;

// With core 3 the audio thread has it at the mixer worker's priority (the game thread's before the worker exists):
// at a lower one the worker's block services took core 3 from it for most of each update. Equal priorities do not
// preempt each other, so once running it keeps the core until it blocks. Without core 3 the audio thread runs on
// cores 0-2 below the game and display threads, in their idle time and while the game thread waits for it.
AudioThread* createThread(win32::WinApplication* app, const x86::CPU& cpu)
{
    const bool core3 = pf::cores::hasCore3();
    const int mask = core3 ? pf::Thread::Cpu3 : pf::Thread::CpuAll;
    const int worker = mixsplit::workerPriority();
    const int priority = !core3 ? win32::scheduler::hostPriorityOf(0) + 1
                       : worker ? worker : sceKernelGetThreadCurrentPriority();
    pf::logInfo("audio: the race frames' audio update runs on its own thread on core%s, priority %d, until %s%s",
                core3 ? " 3" : "s 0-2", priority,
                s_mode == 2 ? "the next frame's first event flush" : "the next frame starts",
                s_batch ? ", holding the audio System lock throughout" : "");
    return new AudioThread(app, cpu, priority, mask);
}

// MainLoop's update in a race frame that takes the update's usual path (none of its early outs or rare branches, no
// speech deduction before the bank loader), outside joylog capture and replay.
bool startable(win32::WinApplication* app, const x86::CPU& cpu)
{
    if (!s_mode || cpu.terminate || rc::ld32(cpu.esp) != MainLoopAudioReturn || app->contextDepth() != 1 ||
        rc::ld32(GameFlowState) != FlowRacing || !rc::ld32(SoundEnabled) || rc::ld32(SkipFrames) ||
        rc::ld8(SkipOnce) || rc::ld32(Pending) || rc::ld8(InGameReinit) || rc::ld8(CountdownRevs) ||
        (rc::ld8(SpeechFlags) & SpeechDeduce) || rc::ld32(JoylogReplaying) || rc::ld32(JoylogCapturing))
        return false;
    // The update's `dt >= minimum` test (false for a NaN).
    return rc::ldf32(cpu.esp + 4) >= rc::ldf32(MinimumDt);
}

// Game thread, the update ended: the flag goes and the census window closes.
void ended(x86::CPU& cpu)
{
    s_concurrent.store(false, std::memory_order_seq_cst);
    s_aemsRan = false;
    display::closeShimGate();
    census::audioEnd();
    if (s_thread->terminated())
        cpu.terminate = true;
}

// Game thread: runs the update's bank loader (EAXAemsManager::Update, its first call on the usual path; before it the
// update only times its pause states), then starts the rest of the update beside it.
void start(win32::WinApplication* app, x86::CPU& cpu)
{
    if (!s_thread)
    {
        s_gameThread = pf::Thread::currentId();
        s_thread = createThread(app, cpu);
    }
    const bool timed = pf::perf::enabled();
    const uint32_t begin = timed ? pf::perf::now() : 0;
    {
        const PartScope part(pf::perf::AudioLockUpdate);
        const x86::CPU guest = callGuest(app, cpu, &sub_521860_guest, AemsManager, {});
        if (guest.terminate)
            return;
    }
    s_aemsRan = true;
    display::openShimGate(app);
    s_concurrent.store(true, std::memory_order_seq_cst);
    census::audioStart();
    s_thread->start(cpu);
    if (timed)
    {
        pf::perf::audio(pf::perf::AudioAsync, 1);
        pf::perf::audio(pf::perf::AudioHandoffUs, pf::perf::now() - begin);
    }
}

// A return address that follows a call instruction: call rel32, call [mem32] / [reg+disp32], call [reg+disp8],
// call reg / [reg].
bool afterCall(x86::reg32 value)
{
    if (value < GameCodeStart + 8 || value >= LibraryCodeEnd)
        return false;
    const uint8_t* code = rc::host(value);
    return code[-5] == 0xe8 || (code[-6] == 0xff && (code[-5] & 0x38) == 0x10) ||
           (code[-3] == 0xff && (code[-2] & 0x38) == 0x10) || (code[-2] == 0xff && (code[-1] & 0x38) == 0x10);
}

// Game thread waiting for the audio System lock (perf_log): its part, the first game code that called in and the
// library code it called (the last library return address below it; the EA mutex's own is skipped).
void lockWaited(uint32_t section, uint32_t stack, uint32_t microseconds)
{
    if (section != AudioSystemLock || pf::Thread::currentId() != s_gameThread)
        return;
    x86::reg32 caller = 0, via = 0;
    for (int i = 1; i < CallerScanWords && !caller; ++i)
    {
        const x86::reg32 value = rc::ld32(stack + 4 * i);
        if (!afterCall(value))
            continue;
        if (value < GameCodeEnd)
            caller = value;
        else if (value != EaMutexReturn)
            via = value;
    }
    pf::perf::audioLockWait(s_part, caller, via, microseconds);
}

}

void configure()
{
    s_mode = std::max(0, std::min(2, pf::Config::getInt("audio_parallel", 0)));
    s_batch = pf::Config::getBool("audio_lock_batch", true);
    if (s_batch)
        win32::kernel32::setWaitHook(&batchWait);
    // perf_log: the game thread's waits for the audio System lock by part, with or without the audio thread.
    if (pf::perf::enabled())
    {
        s_gameThread = pf::Thread::currentId();
        pf::perf::setCsWaitObserver(&lockWaited);
    }
}

void shutdown()
{
    // An update still running may wait for the game thread, which has stopped: the process ends with it.
    if (s_concurrent.load(std::memory_order_acquire))
        return;
    delete s_thread;
    s_thread = nullptr;
}

bool enabled()
{
    return s_mode != 0;
}

bool concurrent()
{
    return s_concurrent.load(std::memory_order_acquire);
}

bool onAudioThread(const x86::CPU& cpu)
{
    return cpu.efs == s_audioBlock.load(std::memory_order_relaxed);
}

void barrier(win32::WinApplication* app, x86::CPU& cpu, bool early)
{
    if (!s_concurrent.load(std::memory_order_acquire) || s_serving || pf::Thread::currentId() != s_gameThread)
        return;
    const bool timed = pf::perf::enabled();
    const uint32_t start = timed ? pf::perf::now() : 0;
    while (!s_thread->next(app, cpu))
    {
    }
    ended(cpu);
    if (timed)
    {
        pf::perf::audio(pf::perf::AudioBarrierUs, pf::perf::now() - start);
        if (early)
            pf::perf::audio(pf::perf::AudioEarlyBarriers, 1);
    }
}

bool handOver(win32::WinApplication*, x86::CPU& cpu, win32::MethodPtr function)
{
    if (!onAudioThread(cpu))
        return false;
    s_thread->handOver(cpu, function);
    return true;
}

void frameStart(win32::WinApplication* app, x86::CPU& cpu)
{
    if (s_mode == 1)
        barrier(app, cpu, false);
}

void firstFlush(win32::WinApplication* app, x86::CPU& cpu)
{
    barrier(app, cpu, s_mode != 2);
}

PartScope::PartScope(pf::perf::AudioLockPart part)
    :   m_previous(s_part)
{
    s_part = part;
}

PartScope::~PartScope()
{
    s_part = m_previous;
}

}

namespace native
{

// EAXSound::Update(float) (thiscall): MainLoop's call in a race frame runs on the audio thread with audio_parallel,
// the others here once a running update has ended.
void sub_521fe0(win32::WinApplication* app, x86::CPU& cpu)
{
    using namespace audioupdate;
    if (onAudioThread(cpu))
    {
        sub_521fe0_guest(app, cpu);
        return;
    }
    barrier(app, cpu);
    if (startable(app, cpu))
    {
        start(app, cpu);
        cpu.esp += 8;
        return;
    }
    const bool mainLoop = rc::ld32(cpu.esp) == MainLoopAudioReturn;
    const PartScope part(pf::perf::AudioLockUpdate);
    // audio_lock_batch: MainLoop's update holds the audio System lock throughout here too.
    const bool batch = s_batch && mainLoop && app->contextDepth() == 1 && !cpu.terminate &&
                       rc::ld32(GameFlowState) == FlowRacing;
    const uint32_t taken = batch ? batchLock(app, cpu) : 0;
    sub_521fe0_guest(app, cpu);
    if (batch)
        batchUnlock(app, cpu, taken);
    if (mainLoop && pf::perf::enabled())
        pf::perf::audio(pf::perf::AudioSync, 1);
}

// Csis::System::Lock, also the audio System's lock hook: perf_log counts the game thread's takes by part, and those
// while an update runs on the audio thread (their order against the update's calls is not the game's).
void sub_8cf74b(win32::WinApplication* app, x86::CPU& cpu)
{
    using namespace audioupdate;
    if (pf::perf::enabled() && pf::Thread::currentId() == s_gameThread)
    {
        pf::perf::audioLockTaken(s_part);
        if (s_concurrent.load(std::memory_order_relaxed) && !s_serving)
            pf::perf::audio(pf::perf::AudioInterleaved, 1);
    }
    sub_8cf74b_guest(app, cpu);
}

// EAXAemsManager::Update (thiscall): sound bank loads and unloads (the file queue, joylog, track streamer memory) run
// on the game thread.
void sub_521860(win32::WinApplication* app, x86::CPU& cpu)
{
    using namespace audioupdate;
    // The game thread ran it before it started this update.
    if (s_aemsRan && cpu.ecx == AemsManager && onAudioThread(cpu))
    {
        s_aemsRan = false;
        cpu.esp += 4;
        return;
    }
    if (handOver(app, cpu, &sub_521860_guest))
        return;
    sub_521860_guest(app, cpu);
}

}
}
