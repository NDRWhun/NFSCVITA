#ifndef LIB_SCHEDULER_H_
#define LIB_SCHEDULER_H_

// Scheduling of the host threads that run guest code under the execution lock: Windows priorities, preemption,
// SuspendThread/ResumeThread and the core each guest thread runs on.

#include <lib/winapp.h>
#include <platform/cores.h>
#include <platform/perf.h>

namespace win32
{

// Per host thread; the thread block's teb::HostControl word points to it while guest code runs.
struct ThreadControl;

namespace scheduler
{

// A thread waiting for the execution lock makes the owner yield at its next preemption point when its level is
// higher, as a higher-priority thread preempts on Windows.
enum Level
{
    LevelNormal,        // THREAD_PRIORITY_NORMAL and below
    LevelAboveNormal,
    LevelHighest,
    LevelTimeCritical,  // also the winmm timer callbacks
    LevelCount
};

Level levelOf(int windowsPriority);
// Host thread priority for a Windows thread priority.
int hostPriorityOf(int windowsPriority);

// Core role and log name of the guest threads started at an entry point; others get pf::cores::Helper. Before
// guest code runs.
void setThreadRole(x86::reg32 entryPoint, pf::cores::Role role, const char* name = nullptr);
pf::cores::Role threadRole(x86::reg32 entryPoint);
const char* threadName(x86::reg32 entryPoint);
// Guest threads started at an entry point run guest code without the execution lock (WinApplication::runThread):
// everything they reach must synchronise on its own, and calls that need the lock take a HoldContext. Before guest
// code runs.
void setLockFree(x86::reg32 entryPoint);
bool lockFree(x86::reg32 entryPoint);
// A start routine that calls the real entry point stored in its parameter block at entryOffset, with the
// parameter stored at parameterOffset (the CRT's _beginthreadex thunk). Before guest code runs.
void setStartThunk(x86::reg32 thunk, x86::reg32 entryOffset, x86::reg32 parameterOffset);
// The entry point a CreateThread start routine and parameter lead to.
x86::reg32 resolveEntry(WinApplication* app, x86::reg32 startAddress, x86::reg32 parameter);
// The threads of NFSC.exe (roles, names, start thunks).
void configureGameThreads();

// Control of the calling host thread; one that is not a guest thread gets a normal-level control on first use.
ThreadControl* currentControl();
Level level(const ThreadControl* control);
// Kind of thread, for the execution lock accounting of perf_log.
pf::perf::Holder holder(const ThreadControl* control);
// The calling host thread runs the game loop: perf_log accounts it as the game thread. Before guest code runs.
void setCurrentGameThread();
// The calling host thread runs the game's display call for the game thread: accounted as the game thread, or as a
// helper when it runs beside the game thread (overlap), its CPU time logged as "display".
void setCurrentDisplayThread(bool overlap);
// The calling host thread runs work of guest thread guestId without the execution lock: it reports that id, is
// accounted as a helper and its CPU time is logged under name (a string literal). Returns its control.
ThreadControl* setCurrentWorkerThread(x86::reg32 guestId, const char* name);
// Guest thread id of a control (1, the main thread, for none).
x86::reg32 guestId(const ThreadControl* control);
// Raises (never lowers) the level of the calling host thread, e.g. for a service thread running callbacks.
void raiseCurrentLevel(Level level);
// SetThreadPriority on the calling thread (GetCurrentThread pseudo handle).
void setCurrentPriority(int windowsPriority);
int currentPriority();
// Windows priority of a guest thread (0 if unknown).
int priority(x86::reg32 threadId);

// SuspendThread / ResumeThread by guest thread id: the previous suspend count, or -1 for an unknown thread. A
// suspended thread stops before it next takes the execution lock; the thread holding the lock calls these, so the
// target never runs guest code while suspended. A thread running without the lock stops only at its next HoldContext.
int suspend(x86::reg32 threadId);
int resume(x86::reg32 threadId);
// True while any thread is suspended (cheap check for the lock path).
bool anySuspended();
// True when the thread should stop at its lock point.
bool mustPark(const ThreadControl* control);
// Blocks a thread that released the execution lock until it is resumed or cpu terminates.
void park(ThreadControl* control, const x86::CPU& cpu);

// Exit code of a guest thread (GetExitCodeThread): set by ExitThread or by the thread's return value, whichever
// comes first. False while the thread runs.
void setExitCode(x86::reg32 threadId, x86::reg32 code);
bool exitCode(x86::reg32 threadId, x86::reg32* code);

// What a thread does with the execution lock, for the hang watchdog; set at its lock transitions.
enum WatchState
{
    WatchIdle,          // no guest code yet, or between guest calls
    WatchGuest,         // holds the lock: guest code or a shim
    WatchLockWait,      // waits for the lock
    WatchReleased,      // a shim gave the lock up (the pf::perf::Block says why)
    WatchParked,        // stopped by SuspendThread
    WatchEnded,
    WatchFree           // runs guest code without the execution lock
};
void watchState(ThreadControl* control, WatchState state);
void watchAcquired(ThreadControl* control, const x86::CPU& cpu);
void watchFree(ThreadControl* control, const x86::CPU& cpu);
// caller: guest return address of the shim; hostCaller: the host code that released the lock.
void watchReleased(ThreadControl* control, int block, x86::reg32 caller, const void* hostCaller);
// Name a guest thread gave itself (the debugger naming exception); 0xffffffff names the calling thread.
void setThreadName(x86::reg32 threadId, const char* name);
// Logs every thread that ran guest code: state, last shim call, last lock release and the code addresses on its
// guest stack. owner: the control holding the execution lock, or null.
void logThreads(WinApplication* app, const ThreadControl* owner);

}

}

#endif
