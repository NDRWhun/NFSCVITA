#include <platform/perf.h>
#include <platform/log.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/power.h>
#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>

namespace pf { namespace perf {

bool g_enabled = false;
int g_nextBlock = BlockOther;
std::atomic<int> g_lockHolder{0};

int currentThread()
{
    return sceKernelGetThreadId();
}

namespace
{

const uint32_t LogIntervalUs = 5000000;

std::atomic<uint32_t> s_counters[CounterCount];
std::atomic<Reporter> s_reporter{nullptr};
std::atomic<FrameObserver> s_frameObserver{nullptr};
std::atomic<GpuObserver> s_gpuObserver{nullptr};
std::atomic<uint32_t> s_preemptCount{0};
std::atomic<uint32_t> s_preemptSum{0};
std::atomic<uint32_t> s_preemptMax{0};
std::atomic<uint32_t> s_gpuBusyCount{0};
std::atomic<uint32_t> s_gpuBusySum{0};

double msPerFrame(uint32_t microseconds, uint32_t frames)
{
    return double(microseconds) / 1000.0 / double(frames);
}

std::atomic<uint32_t> s_blocked[BlockCount];
std::atomic<uint32_t> s_blockCount{0};
std::atomic<uint32_t> s_relock{0};
std::atomic<uint32_t> s_held[HolderCount];
// Helper lock time by guest thread id; the last slot collects higher ids.
const uint32_t HelperIds = 32;
std::atomic<uint32_t> s_helperHeld[HelperIds];

// Audio mixer: its guest thread id, blocks rendered and the block service split since the last log line.
std::atomic<uint32_t> s_mixerThread{0};
std::atomic<uint32_t> s_mixerBlocks{0};
std::atomic<uint32_t> s_mixerWorkerUs{0};
std::atomic<uint32_t> s_mixerWaitUs{0};
std::atomic<uint32_t> s_mixerInlineUs{0};

// Timer callbacks by guest address; written by the timer service thread.
struct TimerStats
{
    std::atomic<uint32_t> callback;
    std::atomic<uint32_t> count;
    std::atomic<uint32_t> us;
    std::atomic<uint32_t> max;
};
const int TimerCount = 4;
TimerStats s_timers[TimerCount];

// Threads whose CPU time is logged.
struct Tracked
{
    std::atomic<const char*> name;
    SceUID thread;
    uint64_t lastClocks;    // updated by the log
};
const int TrackedCount = 10;
Tracked s_tracked[TrackedCount];
std::atomic<int> s_trackedCount{0};

std::atomic<uint32_t> s_overlap[OverlapCount];
std::atomic<int> s_displayThread{0};
std::atomic<uint32_t> s_displayWaits[DisplayWaitCount];
// The display thread's waits of the current frame (that thread only).
uint32_t s_frameDisplayWaits[DisplayWaitCount];

// Waits by key (a critical section's guest address, a shim name) with the first caller and owner seen; the busiest
// is logged.
struct KeyedWait
{
    std::atomic<uint32_t> key;
    std::atomic<uint32_t> caller;
    std::atomic<int> owner;
    std::atomic<uint32_t> us;
    std::atomic<uint32_t> count;
};
const int KeyedWaitCount = 8;
KeyedWait s_csWaits[KeyedWaitCount];
std::atomic<CsWaitObserver> s_csWaitObserver{nullptr};

// Audio update counters, and the game thread's audio System lock waits by part and by calling game code.
std::atomic<uint32_t> s_audio[AudioCount];
std::atomic<uint32_t> s_audioLockCount[AudioLockPartCount];
std::atomic<uint32_t> s_audioLockUs[AudioLockPartCount];
std::atomic<uint32_t> s_audioLockTakes[AudioLockPartCount];
KeyedWait s_audioLockCallers[KeyedWaitCount];
const char* const s_audioLockPartNames[AudioLockPartCount] = { "sim", "audio update", "other" };

// The log name of a tracked host thread, or null.
const char* trackedName(int thread)
{
    const int tracked = std::min(s_trackedCount.load(std::memory_order_acquire), TrackedCount);
    for (int i = 0; i < tracked; ++i)
    {
        if (s_tracked[i].thread == thread)
            return s_tracked[i].name.load(std::memory_order_acquire);
    }
    return nullptr;
}

KeyedWait s_deviceWaits[KeyedWaitCount];

void keyedWait(KeyedWait* waits, uint32_t key, uint32_t caller, int owner, uint32_t microseconds)
{
    for (int i = 0; i < KeyedWaitCount; ++i)
    {
        KeyedWait& wait = waits[i];
        uint32_t current = wait.key.load(std::memory_order_relaxed);
        if (current == 0 && wait.key.compare_exchange_strong(current, key, std::memory_order_relaxed))
        {
            wait.caller.store(caller, std::memory_order_relaxed);
            current = key;
        }
        if (current == key)
        {
            if (owner)
                wait.owner.store(owner, std::memory_order_relaxed);
            wait.us.fetch_add(microseconds, std::memory_order_relaxed);
            wait.count.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
}

// The busiest keyed wait since the last call (all are reset), or null.
KeyedWait* busiestWait(KeyedWait* waits, uint32_t& us, uint32_t& count)
{
    KeyedWait* busiest = nullptr;
    us = 0;
    count = 0;
    for (int i = 0; i < KeyedWaitCount; ++i)
    {
        const uint32_t waited = waits[i].us.exchange(0, std::memory_order_relaxed);
        const uint32_t times = waits[i].count.exchange(0, std::memory_order_relaxed);
        if (waited > us)
        {
            busiest = &waits[i];
            us = waited;
            count = times;
        }
    }
    return busiest;
}

const char* const s_blockNames[BlockCount] = { "other", "sleep", "wait", "cs", "file", "swap", "gpu", "preempt" };
const char* const s_holderNames[HolderCount] = { "game", "timer", "io", "helper", "main" };

void raiseMax(std::atomic<uint32_t>& max, uint32_t value)
{
    uint32_t current = max.load(std::memory_order_relaxed);
    while (value > current && !max.compare_exchange_weak(current, value, std::memory_order_relaxed))
    {
    }
}

struct Line
{
    char text[1024] = {};
    int length = 0;

    __attribute__((format(printf, 2, 3))) void append(const char* format, ...)
    {
        if (length >= int(sizeof(text)))
            return;
        va_list args;
        va_start(args, format);
        length += vsnprintf(text + length, sizeof(text) - length, format, args);
        va_end(args);
    }
};

// Second log line, per frame: where the game thread's time without the execution lock went, who held the lock,
// CPU time of the tracked threads and the timer callbacks.
void logBlocking(uint32_t elapsed, uint32_t frames)
{
    Line line;
    uint32_t blocked[BlockCount];
    uint32_t total = 0;
    for (int i = 0; i < BlockCount; ++i)
    {
        blocked[i] = s_blocked[i].exchange(0, std::memory_order_relaxed);
        total += blocked[i];
    }
    line.append("perf: game blocked %.1f ms (%.1f releases, relock %.1f):", msPerFrame(total, frames),
                double(s_blockCount.exchange(0, std::memory_order_relaxed)) / frames,
                msPerFrame(s_relock.exchange(0, std::memory_order_relaxed), frames));
    for (int i = 0; i < BlockCount; ++i)
        line.append(" %s %.1f", s_blockNames[i], msPerFrame(blocked[i], frames));
    line.append(" | lock held:");
    for (int i = 0; i < HolderCount; ++i)
        line.append(" %s %.1f", s_holderNames[i],
                    msPerFrame(s_held[i].exchange(0, std::memory_order_relaxed), frames));
    uint32_t helpers[HelperIds];
    for (uint32_t i = 0; i < HelperIds; ++i)
        helpers[i] = s_helperHeld[i].exchange(0, std::memory_order_relaxed);
    const uint32_t mixerThread = s_mixerThread.load(std::memory_order_relaxed);
    const uint32_t mixerHeld = mixerThread ? helpers[std::min(mixerThread, HelperIds - 1)] : 0;
    line.append(" (");
    for (int n = 0; n < 3; ++n)
    {
        const uint32_t* busiest = std::max_element(helpers, helpers + HelperIds);
        if (*busiest == 0)
            break;
        line.append("%sthread 0x%x %.1f", n ? ", " : "", unsigned(busiest - helpers), msPerFrame(*busiest, frames));
        helpers[busiest - helpers] = 0;
    }
    line.append(")");
    const uint32_t blocks = s_mixerBlocks.exchange(0, std::memory_order_relaxed);
    const uint32_t workerUs = s_mixerWorkerUs.exchange(0, std::memory_order_relaxed);
    const uint32_t waitUs = s_mixerWaitUs.exchange(0, std::memory_order_relaxed);
    const uint32_t inlineUs = s_mixerInlineUs.exchange(0, std::memory_order_relaxed);
    if (mixerThread)
    {
        const double seconds = double(elapsed) / 1000000.0;
        line.append(" | mixer %.0f blocks/s held %.0f ms/s %.0f us/block split %.0f ms/s wait %.0f ms/s inline %.0f ms/s",
                    blocks / seconds, mixerHeld / 1000.0 / seconds, blocks ? double(mixerHeld) / blocks : 0.0,
                    workerUs / 1000.0 / seconds, waitUs / 1000.0 / seconds, inlineUs / 1000.0 / seconds);
    }
    line.append(" | cpu");
    // The display thread's CPU time over the interval: a running thread's run time is only brought up to date when
    // it leaves the CPU, so readings around each call miss its current time slice.
    uint32_t displayCpu = 0;
    const int tracked = std::min(s_trackedCount.load(std::memory_order_acquire), TrackedCount);
    for (int i = 0; i < tracked; ++i)
    {
        Tracked& thread = s_tracked[i];
        const char* name = thread.name.load(std::memory_order_acquire);
        SceKernelThreadInfo info;
        info.size = sizeof(info);
        if (!name || sceKernelGetThreadInfo(thread.thread, &info) < 0)
            continue;
        const uint64_t clocks = info.runClocks;
        line.append(" %s %.1f", name, msPerFrame(uint32_t(clocks - thread.lastClocks), frames));
        if (thread.thread == s_displayThread.load(std::memory_order_relaxed))
            displayCpu = uint32_t(clocks - thread.lastClocks);
        thread.lastClocks = clocks;
    }
    line.append(" | timers");
    for (TimerStats& timer : s_timers)
    {
        const uint32_t callback = timer.callback.load(std::memory_order_relaxed);
        if (!callback)
            break;
        const uint32_t count = timer.count.exchange(0, std::memory_order_relaxed);
        const uint32_t us = timer.us.exchange(0, std::memory_order_relaxed);
        const uint32_t max = timer.max.exchange(0, std::memory_order_relaxed);
        line.append(" %08x %.0f/s %.2f max %.1f ms", unsigned(callback), double(count) * 1000000.0 / double(elapsed),
                    count ? double(us) / 1000.0 / double(count) : 0.0, double(max) / 1000.0);
    }
    uint32_t keyedUs = 0, keyedCount = 0;
    if (const KeyedWait* cs = busiestWait(s_csWaits, keyedUs, keyedCount))
    {
        const int owner = cs->owner.load(std::memory_order_relaxed);
        const char* ownerName = trackedName(owner);
        line.append(" | cs wait most 0x%08x (first caller 0x%08x, owner %s",
                    unsigned(cs->key.load(std::memory_order_relaxed)),
                    unsigned(cs->caller.load(std::memory_order_relaxed)), ownerName ? ownerName : "thread");
        if (!ownerName)
            line.append(" 0x%x", unsigned(owner));
        line.append(") %.1f/frame %.2f ms", double(keyedCount) / frames, msPerFrame(keyedUs, frames));
    }
    if (const KeyedWait* device = busiestWait(s_deviceWaits, keyedUs, keyedCount))
    {
        line.append(" | device wait most %s (first caller 0x%08x) %.1f/frame %.2f ms",
                    reinterpret_cast<const char*>(uintptr_t(device->key.load(std::memory_order_relaxed))),
                    unsigned(device->caller.load(std::memory_order_relaxed)), double(keyedCount) / frames,
                    msPerFrame(keyedUs, frames));
    }
    uint32_t overlap[OverlapCount];
    for (int i = 0; i < OverlapCount; ++i)
        overlap[i] = s_overlap[i].exchange(0, std::memory_order_relaxed);
    if (overlap[OverlapCalls])
    {
        const double perFrame = 1.0 / double(frames);
        line.append(" | overlap %.2f/frame: window %.1f barrier %.1f display %.1f ms, display holds %.1f/frame %.2f ms "
                    "lock waits %.1f/frame %.2f ms, game lock waits %.1f/frame %.2f ms device waits %.1f/frame %.2f "
                    "ms, "
                    "lock-free cs waits %.1f/frame %.2f ms, game-state locks %.0f/frame, early barriers %u",
                    overlap[OverlapCalls] * perFrame, msPerFrame(overlap[OverlapWindowUs], frames),
                    msPerFrame(overlap[OverlapBarrierUs], frames), msPerFrame(overlap[OverlapDisplayUs], frames),
                    overlap[OverlapHolds] * perFrame, msPerFrame(overlap[OverlapHoldUs], frames),
                    overlap[OverlapDisplayWaits] * perFrame, msPerFrame(overlap[OverlapDisplayWaitUs], frames),
                    overlap[OverlapGameWaits] * perFrame, msPerFrame(overlap[OverlapGameWaitUs], frames),
                    overlap[OverlapDeviceWaits] * perFrame, msPerFrame(overlap[OverlapDeviceWaitUs], frames),
                    overlap[OverlapFreeCsWaits] * perFrame, msPerFrame(overlap[OverlapFreeCsWaitUs], frames),
                    overlap[OverlapStateLocks] * perFrame, unsigned(overlap[OverlapEarlyBarriers]));
    }
    uint32_t displayWaits[DisplayWaitCount];
    for (int i = 0; i < DisplayWaitCount; ++i)
        displayWaits[i] = s_displayWaits[i].exchange(0, std::memory_order_relaxed);
    if (overlap[OverlapCalls])
    {
        // The display call's wall time = its CPU time + the measured waits + the rest (preempted or unmeasured).
        const uint32_t measured = displayCpu + overlap[OverlapHoldUs] +
                                  overlap[OverlapDisplayWaitUs] + displayWaits[DisplaySlot] +
                                  displayWaits[DisplaySubmit] + displayWaits[DisplayGpu] + displayWaits[DisplayCs];
        const uint32_t wall = overlap[OverlapDisplayUs];
        logInfo("%s", line.text);
        logInfo("perf: display blocked: call %.1f ms = cpu %.1f + frame slot %.1f + submit %.1f + gpu query %.1f + "
                "cs %.1f + execution lock %.1f + port locks %.1f + preempted or unmeasured %.1f",
                msPerFrame(wall, frames), msPerFrame(displayCpu, frames),
                msPerFrame(displayWaits[DisplaySlot], frames), msPerFrame(displayWaits[DisplaySubmit], frames),
                msPerFrame(displayWaits[DisplayGpu], frames), msPerFrame(displayWaits[DisplayCs], frames),
                msPerFrame(overlap[OverlapHoldUs], frames), msPerFrame(overlap[OverlapDisplayWaitUs], frames),
                msPerFrame(wall > measured ? wall - measured : 0, frames));
        return;
    }
    logInfo("%s", line.text);
}

// Last log line, per frame, when the audio update ran or the game thread took the audio System lock: the update's
// thread and the game thread's waits for it, then the lock's takes and waits by part and the game code that waited
// most.
void logAudio(uint32_t frames)
{
    uint32_t audio[AudioCount];
    for (int i = 0; i < AudioCount; ++i)
        audio[i] = s_audio[i].exchange(0, std::memory_order_relaxed);
    uint32_t count[AudioLockPartCount];
    uint32_t us[AudioLockPartCount];
    uint32_t takes[AudioLockPartCount];
    uint32_t totalCount = 0, totalUs = 0, totalTakes = 0;
    for (int i = 0; i < AudioLockPartCount; ++i)
    {
        count[i] = s_audioLockCount[i].exchange(0, std::memory_order_relaxed);
        us[i] = s_audioLockUs[i].exchange(0, std::memory_order_relaxed);
        takes[i] = s_audioLockTakes[i].exchange(0, std::memory_order_relaxed);
        totalCount += count[i];
        totalUs += us[i];
        totalTakes += takes[i];
    }
    uint32_t callerUs[KeyedWaitCount], callerCount[KeyedWaitCount], callers[KeyedWaitCount], vias[KeyedWaitCount];
    for (int i = 0; i < KeyedWaitCount; ++i)
    {
        callers[i] = s_audioLockCallers[i].key.exchange(0, std::memory_order_relaxed);
        vias[i] = s_audioLockCallers[i].caller.load(std::memory_order_relaxed);
        callerUs[i] = s_audioLockCallers[i].us.exchange(0, std::memory_order_relaxed);
        callerCount[i] = s_audioLockCallers[i].count.exchange(0, std::memory_order_relaxed);
    }
    if (!audio[AudioAsync] && !audio[AudioSync] && !totalCount && !totalTakes)
        return;
    const double perFrame = 1.0 / double(frames);
    Line line;
    line.append("perf: audio update async %.2f/frame sync %.2f/frame: thread %.1f ms (cpu %.1f), hand-over %.2f ms "
                "(%.2f calls/frame run for it), barrier %.2f ms, early barriers %u, lock batch wait %.2f held %.2f ms "
                "released %.1f/frame | game audio lock takes %.1f/frame (%u beside the audio thread), waits %.1f/frame %.2f ms:",
                audio[AudioAsync] * perFrame, audio[AudioSync] * perFrame, msPerFrame(audio[AudioThreadUs], frames),
                msPerFrame(audio[AudioThreadCpuUs], frames), msPerFrame(audio[AudioHandoffUs], frames),
                audio[AudioServed] * perFrame, msPerFrame(audio[AudioBarrierUs], frames),
                unsigned(audio[AudioEarlyBarriers]), msPerFrame(audio[AudioBatchWaitUs], frames),
                msPerFrame(audio[AudioBatchHeldUs], frames), audio[AudioBatchReleases] * perFrame,
                totalTakes * perFrame, unsigned(audio[AudioInterleaved]),
                totalCount * perFrame, msPerFrame(totalUs, frames));
    for (int i = 0; i < AudioLockPartCount; ++i)
        line.append(" %s %.1f/frame %.2f ms (takes %.1f)", s_audioLockPartNames[i], count[i] * perFrame,
                    msPerFrame(us[i], frames), takes[i] * perFrame);
    line.append(" | most from");
    for (int n = 0; n < 3; ++n)
    {
        const uint32_t* busiest = std::max_element(callerUs, callerUs + KeyedWaitCount);
        if (*busiest == 0)
            break;
        const int i = int(busiest - callerUs);
        line.append(" 0x%08x (via 0x%08x) %.1f/frame %.2f ms", unsigned(callers[i]), unsigned(vias[i]),
                    callerCount[i] * perFrame, msPerFrame(callerUs[i], frames));
        callerUs[i] = 0;
    }
    logInfo("%s", line.text);
}

// Render thread only (set by init() before that thread starts).
uint32_t s_intervalStart = 0;
uint32_t s_frames = 0;
uint32_t s_drawCalls = 0;
uint32_t s_triangles = 0;
uint32_t s_peakTriangles = 0;
uint32_t s_lastPresent = 0;
uint32_t s_frameMax = 0;

}

void init(bool enabled)
{
    g_enabled = enabled;
    s_intervalStart = now();
    logInfo("perf: timing log %s (perf_log), main thread priority %d affinity 0x%x, clocks arm %d bus %d gpu %d xbar %d MHz",
            enabled ? "on" : "off", sceKernelGetThreadCurrentPriority(),
            sceKernelGetThreadCpuAffinityMask(sceKernelGetThreadId()),
            scePowerGetArmClockFrequency(), scePowerGetBusClockFrequency(),
            scePowerGetGpuClockFrequency(), scePowerGetGpuXbarClockFrequency());
    // A profiler maps sampled PCs to the ELF with this (load address minus the symbol's ELF address).
    if (enabled)
        logInfo("perf: pf::perf::init at %p", reinterpret_cast<void*>(&init));
}

uint32_t now()
{
    return sceKernelGetProcessTimeLow();
}

void add(Counter counter, uint32_t microseconds)
{
    s_counters[counter].fetch_add(microseconds, std::memory_order_relaxed);
}

void overlap(Overlap counter, uint32_t value)
{
    s_overlap[counter].fetch_add(value, std::memory_order_relaxed);
}

void setDisplayThread(int thread)
{
    s_displayThread.store(thread, std::memory_order_relaxed);
}

void displayWait(DisplayWait wait, uint32_t microseconds)
{
    if (currentThread() == s_displayThread.load(std::memory_order_relaxed))
    {
        s_displayWaits[wait].fetch_add(microseconds, std::memory_order_relaxed);
        s_frameDisplayWaits[wait] += microseconds;
    }
}

uint32_t takeDisplayFrameWait(DisplayWait wait)
{
    const uint32_t waited = s_frameDisplayWaits[wait];
    s_frameDisplayWaits[wait] = 0;
    return waited;
}

std::atomic<uint32_t> s_textureCalls[TextureCallCount];
std::atomic<uint32_t> s_textureUs[TextureCallCount];

void textureCall(TextureCall call, uint32_t microseconds)
{
    s_textureCalls[call].fetch_add(1, std::memory_order_relaxed);
    s_textureUs[call].fetch_add(microseconds, std::memory_order_relaxed);
}

TextureTimer::TextureTimer(TextureCall call)
    : m_call(call), m_start(0), m_on(enabled())
{
    if (m_on)
        m_start = now();
}

TextureTimer::~TextureTimer()
{
    if (m_on)
        textureCall(m_call, now() - m_start);
}

void takeTextureCalls(uint32_t counts[TextureCallCount], uint32_t microseconds[TextureCallCount])
{
    for (int i = 0; i < TextureCallCount; ++i)
    {
        counts[i] = s_textureCalls[i].exchange(0, std::memory_order_relaxed);
        microseconds[i] = s_textureUs[i].exchange(0, std::memory_order_relaxed);
    }
}

void criticalSectionWait(uint32_t section, uint32_t caller, uint32_t stack, int owner, uint32_t microseconds)
{
    keyedWait(s_csWaits, section, caller, owner, microseconds);
    if (CsWaitObserver observer = s_csWaitObserver.load(std::memory_order_acquire))
        observer(section, stack, microseconds);
}

void setCsWaitObserver(CsWaitObserver observer)
{
    s_csWaitObserver.store(observer, std::memory_order_release);
}

void audio(Audio counter, uint32_t value)
{
    s_audio[counter].fetch_add(value, std::memory_order_relaxed);
}

void audioLockWait(AudioLockPart part, uint32_t caller, uint32_t via, uint32_t microseconds)
{
    s_audioLockCount[part].fetch_add(1, std::memory_order_relaxed);
    s_audioLockUs[part].fetch_add(microseconds, std::memory_order_relaxed);
    if (caller)
        keyedWait(s_audioLockCallers, caller, via, 0, microseconds);
}

void audioLockTaken(AudioLockPart part)
{
    s_audioLockTakes[part].fetch_add(1, std::memory_order_relaxed);
}

void deviceWait(const char* shim, uint32_t caller, uint32_t microseconds)
{
    keyedWait(s_deviceWaits, uint32_t(reinterpret_cast<uintptr_t>(shim)), caller, 0, microseconds);
}

void preemptWait(uint32_t microseconds)
{
    s_preemptCount.fetch_add(1, std::memory_order_relaxed);
    s_preemptSum.fetch_add(microseconds, std::memory_order_relaxed);
    raiseMax(s_preemptMax, microseconds);
}

void gpuBusy(uint32_t microseconds)
{
    s_gpuBusyCount.fetch_add(1, std::memory_order_relaxed);
    s_gpuBusySum.fetch_add(microseconds, std::memory_order_relaxed);
    if (GpuObserver observer = s_gpuObserver.load(std::memory_order_acquire))
        observer(microseconds);
}

void setGpuObserver(GpuObserver observer)
{
    s_gpuObserver.store(observer, std::memory_order_release);
}

void gameBlocked(Block block, uint32_t microseconds, uint32_t relockUs)
{
    s_blocked[block].fetch_add(microseconds, std::memory_order_relaxed);
    s_blockCount.fetch_add(1, std::memory_order_relaxed);
    s_relock.fetch_add(relockUs, std::memory_order_relaxed);
}

void trackThread(const char* name)
{
    const int index = s_trackedCount.fetch_add(1, std::memory_order_relaxed);
    if (index >= TrackedCount)
        return;
    Tracked& thread = s_tracked[index];
    thread.thread = sceKernelGetThreadId();
    SceKernelThreadInfo info;
    info.size = sizeof(info);
    thread.lastClocks = sceKernelGetThreadInfo(thread.thread, &info) < 0 ? 0 : info.runClocks;
    thread.name.store(name, std::memory_order_release);
}

void held(Holder holder, uint32_t microseconds)
{
    s_held[holder].fetch_add(microseconds, std::memory_order_relaxed);
}

void helperHeld(uint32_t threadId, uint32_t microseconds)
{
    s_helperHeld[std::min<uint32_t>(threadId, HelperIds - 1)].fetch_add(microseconds, std::memory_order_relaxed);
}

void mixerBlock(uint32_t threadId)
{
    s_mixerThread.store(threadId, std::memory_order_relaxed);
    s_mixerBlocks.fetch_add(1, std::memory_order_relaxed);
}

void mixerSplit(uint32_t workerUs, uint32_t waitUs, uint32_t inlineUs)
{
    s_mixerWorkerUs.fetch_add(workerUs, std::memory_order_relaxed);
    s_mixerWaitUs.fetch_add(waitUs, std::memory_order_relaxed);
    s_mixerInlineUs.fetch_add(inlineUs, std::memory_order_relaxed);
}

void timerFired(uint32_t callback, uint32_t microseconds)
{
    for (TimerStats& timer : s_timers)
    {
        uint32_t current = timer.callback.load(std::memory_order_relaxed);
        if (current == 0)
        {
            timer.callback.store(callback, std::memory_order_relaxed);
            current = callback;
        }
        if (current == callback)
        {
            timer.count.fetch_add(1, std::memory_order_relaxed);
            timer.us.fetch_add(microseconds, std::memory_order_relaxed);
            raiseMax(timer.max, microseconds);
            return;
        }
    }
}

void framePresented(uint32_t drawCalls, uint32_t triangles)
{
    const FrameObserver observer = s_frameObserver.load(std::memory_order_acquire);
    if (!enabled())
    {
        if (observer)
            observer(now());
        return;
    }
    ++s_frames;
    s_drawCalls += drawCalls;
    s_triangles += triangles;
    if (triangles > s_peakTriangles)
        s_peakTriangles = triangles;
    const uint32_t time = now();
    if (s_lastPresent && time - s_lastPresent > s_frameMax)
        s_frameMax = time - s_lastPresent;
    s_lastPresent = time;
    if (observer)
        observer(time);
    const uint32_t elapsed = time - s_intervalStart;
    if (elapsed < LogIntervalUs)
        return;

    uint32_t sums[CounterCount];
    for (int i = 0; i < CounterCount; ++i)
        sums[i] = s_counters[i].exchange(0, std::memory_order_relaxed);
    const uint32_t frames = s_frames;
    const double frame = msPerFrame(elapsed, frames);
    const double wait = msPerFrame(sums[SlotWait], frames);
    const double gil = msPerFrame(sums[GilWait], frames);
    const double game = frame > wait + gil ? frame - wait - gil : 0.0;
    const uint32_t preempts = s_preemptCount.exchange(0, std::memory_order_relaxed);
    const uint32_t preemptSum = s_preemptSum.exchange(0, std::memory_order_relaxed);
    const uint32_t preemptMax = s_preemptMax.exchange(0, std::memory_order_relaxed);
    const uint32_t gpuBusyCount = s_gpuBusyCount.exchange(0, std::memory_order_relaxed);
    const uint32_t gpuBusySum = s_gpuBusySum.exchange(0, std::memory_order_relaxed);
    logInfo("perf: %.1f fps | frame %.1f ms (max %.1f) = game %.1f (preempted %.1f) + wait %.1f (fence %.1f) + gil %.1f"
            " | render %.1f (queue %.1f) | gpu %.1f flip %.1f busy %.1f (%u%%) | %u calls %u tris (peak %u)"
            " | preempt %.0f/s wait %.2f max %.1f ms",
            double(frames) * 1000000.0 / double(elapsed), frame, double(s_frameMax) / 1000.0, game,
            msPerFrame(sums[Preempted], frames), wait,
            msPerFrame(sums[FenceWait], frames), gil,
            msPerFrame(sums[Render], frames), msPerFrame(sums[QueueWait], frames),
            msPerFrame(sums[GpuLatency], frames), msPerFrame(sums[FlipWait], frames),
            gpuBusyCount ? msPerFrame(gpuBusySum, gpuBusyCount) : 0.0, gpuBusyCount * 100 / frames,
            s_drawCalls / frames, s_triangles / frames, s_peakTriangles,
            double(preempts) * 1000000.0 / double(elapsed), preempts ? msPerFrame(preemptSum, preempts) : 0.0,
            double(preemptMax) / 1000.0);
    logBlocking(elapsed, frames);
    logAudio(frames);
    if (Reporter reporter = s_reporter.load(std::memory_order_acquire))
        reporter(elapsed);
    s_intervalStart = time;
    s_frames = 0;
    s_frameMax = 0;
    s_drawCalls = 0;
    s_triangles = 0;
    s_peakTriangles = 0;
}

void setReporter(Reporter reporter)
{
    s_reporter.store(reporter, std::memory_order_release);
}

void setFrameObserver(FrameObserver observer)
{
    s_frameObserver.store(observer, std::memory_order_release);
}

}}
