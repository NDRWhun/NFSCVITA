#include <platform/log.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

namespace pf
{

// s_logPath is read and cleared under s_logMutex, which stays until the process exits so that no logger can use a
// deleted mutex. The path is copied: callers may pass a temporary.
static char        s_logPathBuffer[256];
static const char* s_logPath = nullptr;
static SceUID      s_logMutex = -1;
static int         s_logFile = -1;
static bool        s_console = false;

static const SceUInt LockTimeoutUs = 2000000;
static std::atomic<bool> s_lockTimedOut{false};

// The file stays open so logging needs no new descriptor when the process has none left; writes are unbuffered, so
// nothing is lost if the process dies. A failed write reopens the file once.
static void writeText(const char* text, int length)
{
    if (s_console)
        sceClibPrintf("%s", text);
    if (!s_logPath)
        return;
    for (int attempt = 0; attempt < 2; ++attempt)
    {
        if (s_logFile < 0)
            s_logFile = open(s_logPath, O_WRONLY | O_CREAT | O_APPEND, 0666);
        if (s_logFile < 0 || write(s_logFile, text, length) == length)
            return;
        close(s_logFile);
        s_logFile = -1;
    }
}

static void writeLine(const char* level, const char* fmt, va_list args)
{
    char buffer[1024];
    int prefix = snprintf(buffer, sizeof(buffer), "[%s] ", level);
    int len = vsnprintf(buffer + prefix, sizeof(buffer) - prefix - 1, fmt, args);
    if (len < 0)
        return;
    len += prefix;
    if (len > int(sizeof(buffer)) - 2)
        len = int(sizeof(buffer)) - 2;
    buffer[len++] = '\n';
    buffer[len] = 0;

    // A writer stuck with the lock does not stop the others: after the timeout the line is written without it.
    SceUInt timeout = LockTimeoutUs;
    const bool lockable = s_logMutex >= 0 && !s_lockTimedOut.load(std::memory_order_relaxed);
    const bool locked = lockable && sceKernelLockMutex(s_logMutex, 1, &timeout) >= 0;
    if (lockable && !locked && !s_lockTimedOut.exchange(true))
    {
        SceKernelMutexInfo info;
        info.size = sizeof(info);
        const int owner = sceKernelGetMutexInfo(s_logMutex, &info) >= 0 ? info.currentOwnerId : 0;
        char note[128];
        const int noteLength = snprintf(note, sizeof(note), "[error] log: lock held by host thread 0x%x for 2 s, "
                                        "lines are written without it from now on\n", unsigned(owner));
        writeText(note, std::min(noteLength, int(sizeof(note)) - 1));
    }
    writeText(buffer, len);
    if (locked)
        sceKernelUnlockMutex(s_logMutex, 1);
}

void logInit(const char* logFilePath)
{
    s_logMutex = sceKernelCreateMutex("log",0, 0, nullptr);
    if (logFilePath)
    {
        snprintf(s_logPathBuffer, sizeof(s_logPathBuffer), "%s", logFilePath);
        s_logPath = s_logPathBuffer;
    }
    if (s_logPath)
        s_logFile = open(s_logPath, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0666);
}

void logSetConsole(bool enabled)
{
    s_console = enabled;
}

int logMutexId()
{
    return s_logMutex;
}

void logShutdown()
{
    if (s_logMutex >= 0)
        sceKernelLockMutex(s_logMutex, 1, nullptr);
    s_logPath = nullptr;
    if (s_logFile >= 0)
        close(s_logFile);
    s_logFile = -1;
    if (s_logMutex >= 0)
        sceKernelUnlockMutex(s_logMutex, 1);
}

void logDebug(const char* fmt, ...)
{
#ifdef NFSC_LOG_DEBUG
    va_list args;
    va_start(args, fmt);
    writeLine("debug", fmt, args);
    va_end(args);
#else
    (void)fmt;
#endif
}

void logInfo(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    writeLine("info", fmt, args);
    va_end(args);
}

void logError(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    writeLine("error", fmt, args);
    va_end(args);
}

void fatal(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    writeLine("fatal", fmt, args);
    va_end(args);
    logShutdown();
    sceKernelExitProcess(1);
    for (;;) {}
}

}
