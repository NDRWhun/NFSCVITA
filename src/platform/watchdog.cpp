#include <platform/watchdog.h>
#include <platform/config.h>
#include <platform/log.h>
#include <platform/sync.h>
#include <platform/time.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <algorithm>
#include <cstdarg>
#include <cstdio>

// SceLibKernel exports these two without a declaration in the SDK headers.
namespace
{
struct LwMutexInfo
{
    SceSize     size;
    SceUID      id;
    char        name[32];
    int         attr;
    void*       work;
    int         initCount;
    int         currentCount;
    SceUID      currentOwnerId;
    SceUInt32   numWaitThreads;
};
static_assert(sizeof(LwMutexInfo) == 0x40, "SceKernelLwMutexInfo size");
}
extern "C" int sceKernelGetLwMutexInfoById(SceUID lwMutexId, LwMutexInfo* info);
extern "C" int sceKernelGetCondInfo(SceUID condId, SceKernelCondInfo* info);

namespace pf { namespace watchdog {

std::atomic<uint32_t> g_frames{0};

namespace
{

const int MaxThreads = 64;
std::atomic<int> s_threads[MaxThreads];
std::atomic<Reporter> s_reporter{nullptr};
uint32_t s_limitMs = 5000;
// CPU time is sampled twice this far apart to show which threads run.
const uint32_t SampleMs = 500;

struct Text
{
    char* text;
    size_t size;
    size_t length;

    __attribute__((format(printf, 2, 3))) void append(const char* format, ...)
    {
        if (length + 1 >= size)
            return;
        va_list args;
        va_start(args, format);
        const int n = vsnprintf(text + length, size - length, format, args);
        va_end(args);
        if (n > 0)
            length = std::min(size - 1, length + size_t(n));
    }
};

const char* statusName(SceUInt32 status)
{
    if (status & SCE_THREAD_RUNNING)    return "running";
    if (status & SCE_THREAD_READY)      return "ready";
    if (status & SCE_THREAD_STANDBY)    return "standby";
    if (status & SCE_THREAD_WAITING)    return "waiting";
    if (status & SCE_THREAD_SUSPENDED)  return "suspended";
    if (status & SCE_THREAD_DORMANT)    return "dormant";
    if (status & SCE_THREAD_STAGNANT)   return "stagnant";
    if (status & SCE_THREAD_DELETED)    return "deleted";
    if (status & SCE_THREAD_DEAD)       return "dead";
    return "unknown";
}

// Kind, name and state of the kernel object a thread waits on.
void describeObject(SceUID id, Text& out)
{
    SceKernelMutexInfo mutex;
    mutex.size = sizeof(mutex);
    if (sceKernelGetMutexInfo(id, &mutex) >= 0)
    {
        out.append("mutex '%s' owner 0x%x count %d waiters %d", mutex.name, unsigned(mutex.currentOwnerId),
                   mutex.currentCount, mutex.numWaitThreads);
        return;
    }
    LwMutexInfo lw;
    lw.size = sizeof(lw);
    if (sceKernelGetLwMutexInfoById(id, &lw) >= 0)
    {
        out.append("lw mutex '%s' owner 0x%x count %d waiters %u", lw.name, unsigned(lw.currentOwnerId),
                   lw.currentCount, unsigned(lw.numWaitThreads));
        return;
    }
    SceKernelSemaInfo sema;
    sema.size = sizeof(sema);
    if (sceKernelGetSemaInfo(id, &sema) >= 0)
    {
        out.append("semaphore '%s' count %d of %d waiters %d", sema.name, sema.currentCount, sema.maxCount,
                   sema.numWaitThreads);
        return;
    }
    SceKernelCondInfo cond;
    cond.size = sizeof(cond);
    if (sceKernelGetCondInfo(id, &cond) >= 0)
    {
        out.append("condition '%s' of mutex 0x%x waiters %d", cond.name, unsigned(cond.mutexId), cond.numWaitThreads);
        return;
    }
    out.append("no mutex, semaphore or condition");
}

bool runClocks(int threadId, uint64_t* clocks)
{
    SceKernelThreadInfo info;
    info.size = sizeof(info);
    if (sceKernelGetThreadInfo(threadId, &info) < 0)
        return false;
    *clocks = info.runClocks;
    return true;
}

// before: CPU time of an earlier sample, or null.
void describe(int threadId, char* text, size_t size, const uint64_t* before)
{
    Text out{ text, size, 0 };
    text[0] = 0;
    SceKernelThreadInfo info;
    info.size = sizeof(info);
    const int result = sceKernelGetThreadInfo(threadId, &info);
    if (result < 0)
    {
        out.append("0x%x: no thread info (0x%08x)", unsigned(threadId), unsigned(result));
        return;
    }
    out.append("0x%x '%s' %s, priority %d, core %d (mask 0x%x), cpu %llu ms", unsigned(threadId), info.name,
               statusName(info.status), info.currentPriority, info.currentCpuId, unsigned(info.currentCpuAffinityMask),
               static_cast<unsigned long long>(info.runClocks / 1000));
    if (before)
        out.append(" (+%llu ms in %u ms)", static_cast<unsigned long long>((info.runClocks - *before) / 1000),
                   unsigned(SampleMs));
    if (info.status & SCE_THREAD_WAITING)
    {
        out.append(", waits (type 0x%x) on 0x%x: ", unsigned(info.waitType), unsigned(info.waitId));
        if (info.waitId > 0)
            describeObject(info.waitId, out);
        else
            out.append("no object");
    }
}

void report(uint32_t stalledMs, uint32_t frames)
{
    logInfo("watchdog: no frame from the game thread for %.1f s (%u frames so far); thread states follow",
            double(stalledMs) / 1000.0, unsigned(frames));
    SceKernelMutexInfo logLock;
    logLock.size = sizeof(logLock);
    if (sceKernelGetMutexInfo(logMutexId(), &logLock) >= 0 && logLock.currentOwnerId > 0)
        logInfo("watchdog: log lock held by host thread 0x%x", unsigned(logLock.currentOwnerId));
    SceKernelFreeMemorySizeInfo free;
    free.size = sizeof(free);
    if (sceKernelGetFreeMemorySize(&free) >= 0)
        logInfo("watchdog: free user %u KB, cdram %u KB, phycont %u KB", unsigned(free.size_user) / 1024,
                unsigned(free.size_cdram) / 1024, unsigned(free.size_phycont) / 1024);

    int ids[MaxThreads];
    uint64_t clocks[MaxThreads];
    bool sampled[MaxThreads];
    int count = 0;
    const int self = sceKernelGetThreadId();
    for (std::atomic<int>& slot : s_threads)
    {
        const int id = slot.load(std::memory_order_acquire);
        if (id > 0 && id != self)
        {
            ids[count] = id;
            sampled[count] = runClocks(id, &clocks[count]);
            ++count;
        }
    }
    sleepMs(SampleMs);
    char text[512];
    for (int i = 0; i < count; ++i)
    {
        describe(ids[i], text, sizeof(text), sampled[i] ? &clocks[i] : nullptr);
        logInfo("watchdog: host thread %s", text);
    }
    if (Reporter reporter = s_reporter.load(std::memory_order_acquire))
        reporter();
    logInfo("watchdog: end of report");
}

int run(void*)
{
    uint32_t lastFrames = 0;
    uint64_t lastChange = ticksUs();
    bool reported = false;
    for (;;)
    {
        sleepMs(1000);
        const uint32_t frames = g_frames.load(std::memory_order_relaxed);
        const uint64_t now = ticksUs();
        const uint32_t stalledMs = uint32_t((now - lastChange) / 1000);
        if (frames != lastFrames)
        {
            if (reported)
                logInfo("watchdog: the game thread presents frames again after %.1f s", double(stalledMs) / 1000.0);
            lastFrames = frames;
            lastChange = now;
            reported = false;
            continue;
        }
        // Start-up loads before the first frame are not watched.
        if (frames == 0 || reported || stalledMs < s_limitMs)
            continue;
        reported = true;
        report(stalledMs, frames);
    }
    return 0;
}

}

void start()
{
    addThread(sceKernelGetThreadId());
    if (!Config::getBool("watchdog", true))
        return;
    s_limitMs = uint32_t(std::max(Config::getInt("watchdog_seconds", 5), 2)) * 1000;
    // Lives until the process exits.
    Thread* thread = new Thread("watchdog", &run, nullptr, Thread::PriorityHigh, 64 * 1024, Thread::CpuAll);
    thread->start();
    logInfo("watchdog: on, report after %u s without a frame", unsigned(s_limitMs / 1000));
}

void addThread(int threadId)
{
    for (std::atomic<int>& slot : s_threads)
    {
        int expected = 0;
        if (slot.compare_exchange_strong(expected, threadId, std::memory_order_acq_rel))
            return;
    }
}

void removeThread(int threadId)
{
    for (std::atomic<int>& slot : s_threads)
    {
        int expected = threadId;
        if (slot.compare_exchange_strong(expected, 0, std::memory_order_acq_rel))
            return;
    }
}

void setReporter(Reporter reporter)
{
    s_reporter.store(reporter, std::memory_order_release);
}

void describeThread(int threadId, char* text, size_t size)
{
    describe(threadId, text, size, nullptr);
}

}}
