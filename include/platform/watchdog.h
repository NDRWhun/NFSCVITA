#ifndef PLATFORM_WATCHDOG_H_
#define PLATFORM_WATCHDOG_H_

#include <atomic>
#include <cstddef>
#include <cstdint>

// Hang watchdog: a thread of its own checks once a second that the game thread presents frames. After a stall of
// watchdog_seconds (5 by default, watchdog=0 turns it off) it logs, once per stall, the kernel state of every host
// thread, then the guest view from the reporter, and logs again when frames come back.
namespace pf { namespace watchdog
{

extern std::atomic<uint32_t> g_frames;

// Main thread, after the config is loaded; registers the calling thread.
void start();
// Game thread: a frame was presented.
inline void progress() { g_frames.fetch_add(1, std::memory_order_relaxed); }
// Host threads the report lists (pf::Thread registers its own).
void addThread(int threadId);
void removeThread(int threadId);

// Guest view of the report: runs on the watchdog thread after the host threads are logged.
typedef void (*Reporter)();
void setReporter(Reporter reporter);

// Kernel state of a host thread on one line: name, status, priority, core, what it waits for (object kind, name
// and owner) and its CPU time. Usable by the reporter.
void describeThread(int threadId, char* text, size_t size);

}}

#endif
