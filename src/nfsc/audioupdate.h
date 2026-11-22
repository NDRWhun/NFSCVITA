#ifndef NFSC_AUDIOUPDATE_H_
#define NFSC_AUDIOUPDATE_H_

// The game's per-frame audio update (EAXSound::Update 0x521fe0, MainLoop's call) on a guest thread of its own with
// audio_parallel=1 or 2, beside the rest of the race frame: up to the start of the next MainLoop frame (1), or up to
// the start of its first event flush, after its timers, input and sim event (2). The game thread waits for it at that
// barrier, and earlier where it would change what the update uses (other audio updates, load completions, unloads).
// EAXAemsManager::Update, the sound bank loader inside it, runs on the game thread at the hand-over, before the rest
// of the update starts (on the update's usual path only its pause timing comes before it). audio_lock_batch=1 (the
// default) makes MainLoop's race-frame update, on either thread, take the audio System lock once and hold it to its
// end; it is free while the update blocks on something else.

#include <lib/winapp.h>
#include <platform/perf.h>

namespace nfsc { namespace audioupdate {

// Reads the settings; before guest code runs and before display::configure.
void configure();
// Stops the audio thread; after the game loop has returned.
void shutdown();

bool enabled();
// True while an update runs beside the game thread; changes only on the game thread.
bool concurrent();
bool onAudioThread(const x86::CPU& cpu);
// Game thread: waits for an update running beside it (execution lock released), running the calls it hands over
// meanwhile; does nothing on other threads, with no update running, or while the game thread runs a call for it.
// early: not the next frame's own barrier (counted).
void barrier(win32::WinApplication* app, x86::CPU& cpu, bool early = true);
// Audio thread: runs a guest function on the game thread instead, at its next hand-over or barrier, and returns as
// that function does; false on other threads (the caller runs it itself).
bool handOver(win32::WinApplication* app, x86::CPU& cpu, win32::MethodPtr function);
// Game thread, for MainLoop's barriers: before its frame (mode 1) and before its first event flush (mode 2).
void frameStart(win32::WinApplication* app, x86::CPU& cpu);
void firstFlush(win32::WinApplication* app, x86::CPU& cpu);

// Game thread, perf_log: what it is doing, for its waits for the audio System lock.
class PartScope
{
public:
    explicit PartScope(pf::perf::AudioLockPart part);
    ~PartScope();

private:
    PartScope(const PartScope&) = delete;
    PartScope& operator=(const PartScope&) = delete;
    const pf::perf::AudioLockPart m_previous;
};

}}

#endif
