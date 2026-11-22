#ifndef PLATFORM_PERF_H_
#define PLATFORM_PERF_H_

#include <atomic>
#include <cstdint>

namespace pf { namespace perf {

// Time sums in microseconds, reported per frame and reset with every log line.
enum Counter
{
    SlotWait,       // game thread waiting for a free frame slot, FenceWait included
    FenceWait,      // game thread waiting for the GPU to finish reading a frame slot
    GilWait,        // game thread re-taking the execution lock after a frame present
    Render,         // render thread building and queueing a frame, QueueWait included
    QueueWait,      // render thread blocked in sceGxmDisplayQueueAddEntry
    GpuLatency,     // sceGxmEndScene until the display callback runs, frames queued ahead included
    FlipWait,       // display callback setting the frame buffer and waiting for vblank
    Preempted,      // the game thread giving the execution lock to preempting threads, until it has it again
    CounterCount
};

// Why the game thread gave up the execution lock.
enum Block
{
    BlockOther,
    BlockSleep,     // Sleep
    BlockWait,      // WaitForSingleObject / WaitForMultipleObjects
    BlockCritSec,   // EnterCriticalSection held by another thread
    BlockFile,      // ReadFile, WriteFile, MapViewOfFile
    BlockSwap,      // Flip / Present: frame slot wait
    BlockGpu,       // a query waiting for the GPU
    BlockPreempt,   // preemption point, a higher-level thread took the lock
    BlockCount
};

// Kinds of threads holding the execution lock.
enum Holder
{
    HolderGame,
    HolderTimer,    // timer callbacks
    HolderIo,       // file reading threads
    HolderHelper,   // other guest threads
    HolderMain,     // message loop
    HolderCount
};

// Display calls running beside the game thread (display_overlap): counts and microsecond sums per log line.
enum Overlap
{
    OverlapCalls,           // overlapped display calls started
    OverlapWindowUs,        // game thread: from a call's start to its barrier
    OverlapBarrierUs,       // game thread waiting at the barrier
    OverlapDisplayUs,       // display thread running the call
    OverlapHolds,           // display thread taking the execution lock for a shim
    OverlapHoldUs,
    OverlapDisplayWaits,    // display thread waiting for a host lock the port takes for game state
    OverlapDisplayWaitUs,
    OverlapGameWaits,       // other threads waiting for such a lock
    OverlapGameWaitUs,
    OverlapDeviceWaits,     // other threads waiting for the Direct3D layer the display thread holds
    OverlapDeviceWaitUs,
    OverlapFreeCsWaits,     // threads running without the execution lock waiting in EnterCriticalSection
    OverlapFreeCsWaitUs,
    OverlapStateLocks,      // game-state lock acquisitions while a call overlaps
    OverlapEarlyBarriers,   // loading, unloading or freeing code waited for a running call before the barrier
    OverlapCount
};

// The per-frame audio update on a thread of its own (audio_parallel): counts and microsecond sums per log line.
enum Audio
{
    AudioAsync,             // MainLoop audio updates run on the audio thread
    AudioSync,              // MainLoop audio updates run on the game thread
    AudioThreadUs,          // audio thread running its updates
    AudioThreadCpuUs,       // audio thread's CPU time in them
    AudioHandoffUs,         // game thread at the hand-over, until the update's game-thread part has run
    AudioServed,            // calls the game thread ran for the audio thread
    AudioBarrierUs,         // game thread waiting for a running update at a barrier
    AudioEarlyBarriers,     // barriers before the next frame's own one
    AudioInterleaved,       // game thread taking the audio System lock while an update runs on the audio thread
    AudioBatchWaitUs,       // audio_lock_batch: waiting for the audio System lock before an update
    AudioBatchHeldUs,       // holding it through the update
    AudioBatchReleases,     // given up while the update blocked on something else
    AudioCount
};

// What the game thread was doing when it waited for the audio System lock.
enum AudioLockPart
{
    AudioLockSim,           // MainLoop's first event flush: the sim steps
    AudioLockUpdate,        // the audio update, or its part that runs on the game thread
    AudioLockOther,
    AudioLockPartCount
};

extern bool g_enabled;
extern int g_nextBlock;
// Host thread id of the execution lock's holder, 0 while the lock is free.
extern std::atomic<int> g_lockHolder;

// Main thread, before any other thread starts: sets the switch and logs start-up facts.
void init(bool enabled);
inline bool enabled() { return g_enabled; }
// Host thread id of the calling thread.
int currentThread();
// The thread holding the execution lock names the reason of its next release (read and reset by the release);
// threads running without the lock are ignored.
inline void blocking(Block block)
{
    if (currentThread() == g_lockHolder.load(std::memory_order_relaxed))
        g_nextBlock = block;
}
// Game thread: the execution lock was given up for a reason from release to re-acquisition, relockUs of it waiting
// for the lock.
void gameBlocked(Block block, uint32_t microseconds, uint32_t relockUs);
// The calling thread's CPU time is logged under this name (a string literal); a few threads at most.
void trackThread(const char* name);
// Time a thread of a kind held the execution lock.
void held(Holder holder, uint32_t microseconds);
// Time a helper thread held the execution lock, by guest thread id (the busiest are logged).
void helperHeld(uint32_t threadId, uint32_t microseconds);
// Audio mixer thread (guest thread id): one 256-frame block rendered.
void mixerBlock(uint32_t threadId);
// Audio mixer thread, per block service handed to its worker: the worker's time on it, the mixer thread's wait for
// it and the time the mixer thread ran it itself (the worker had not started it).
void mixerSplit(uint32_t workerUs, uint32_t waitUs, uint32_t inlineUs);
// Timer service: one timer callback ran for this long, waiting for the lock included.
void timerFired(uint32_t callback, uint32_t microseconds);
// Process time in microseconds, wrapping; the difference of two readings is a duration.
uint32_t now();
// Any thread.
void add(Counter counter, uint32_t microseconds);
void overlap(Overlap counter, uint32_t value);

// Where the display thread waits (counted only on the thread named by setDisplayThread).
enum DisplayWait
{
    DisplaySlot,        // a free frame slot for recording (render thread and GPU behind)
    DisplaySubmit,      // handing a recorded frame over
    DisplayGpu,         // a query waiting for the GPU to finish a frame
    DisplayCs,          // a guest critical section
    DisplayWaitCount
};
void setDisplayThread(int thread);
void displayWait(DisplayWait wait, uint32_t microseconds);
// Display thread: its waits of a kind since the last call (per frame).
uint32_t takeDisplayFrameWait(DisplayWait wait);

// Texture creation in the Direct3D layer (any thread): count and time since the last take.
enum TextureCall
{
    TextureCreate,      // IDirect3DDevice9::CreateTexture
    TextureLock,        // IDirect3DTexture9::LockRect
    TextureUnlock,      // IDirect3DTexture9::UnlockRect (conversion and upload)
    TextureCallCount
};
void textureCall(TextureCall call, uint32_t microseconds);
void takeTextureCalls(uint32_t counts[TextureCallCount], uint32_t microseconds[TextureCallCount]);
// Times the enclosing scope as one texture call (perf_log only).
class TextureTimer
{
public:
    explicit TextureTimer(TextureCall call);
    ~TextureTimer();
    TextureTimer(const TextureTimer&) = delete;
    TextureTimer& operator=(const TextureTimer&) = delete;

private:
    TextureCall m_call;
    uint32_t m_start;
    bool m_on;
};
// A thread holding the execution lock waited for a guest critical section (guest address) entered from caller with
// the guest stack pointer stack, held by the host thread owner (0: unknown).
void criticalSectionWait(uint32_t section, uint32_t caller, uint32_t stack, int owner, uint32_t microseconds);
// Called by criticalSectionWait (set before guest code runs).
typedef void (*CsWaitObserver)(uint32_t section, uint32_t stack, uint32_t microseconds);
void setCsWaitObserver(CsWaitObserver observer);
void audio(Audio counter, uint32_t value);
// The game thread waited for the audio System lock (the guest critical section that the game's sound calls and the
// mixer share); caller: the first game code return address on its stack (0: none found), via: the last library
// return address below it (0: none).
void audioLockWait(AudioLockPart part, uint32_t caller, uint32_t via, uint32_t microseconds);
// The game thread took the audio System lock.
void audioLockTaken(AudioLockPart part);
// The game thread waited for the Direct3D layer the display thread holds, in the shim named (a string that lives on)
// called from caller.
void deviceWait(const char* shim, uint32_t caller, uint32_t microseconds);
// Any thread: time a preempting thread (timer callback, raised guest priority) waited for the execution lock.
void preemptWait(uint32_t microseconds);
// Display callback: GPU time of one frame, from the later of its sceGxmEndScene and the previous frame's
// completion until its own completion; only reported for frames whose completion time is known.
void gpuBusy(uint32_t microseconds);
// Called on the display callback's thread at each gpuBusy with its time.
typedef void (*GpuObserver)(uint32_t microseconds);
void setGpuObserver(GpuObserver observer);
// Render thread, after each game frame it queued; logs one line about every 5 s.
void framePresented(uint32_t drawCalls, uint32_t triangles);
// Called on the render thread after each log line with the interval length in microseconds.
typedef void (*Reporter)(uint32_t intervalUs);
void setReporter(Reporter reporter);
// Called on the render thread at each framePresented with its now() time.
typedef void (*FrameObserver)(uint32_t presentUs);
void setFrameObserver(FrameObserver observer);

}}

#endif
