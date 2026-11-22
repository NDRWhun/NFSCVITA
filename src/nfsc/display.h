#ifndef NFSC_DISPLAY_H_
#define NFSC_DISPLAY_H_

// Main_DisplayFrame (0x699230) on a guest thread of its own. With display_parallel=1 the game thread hands each
// display call to that thread and waits for it, so the frame still runs in order: the display thread takes the
// execution lock as the game thread would, with a copy of its registers, x87 state, host FPU mode, thread id, TLS
// and last error, which return to the game thread with the call. display_core picks its core (default 2).
// display_overlap=1 (race frames; 2: all frames) runs call N without the execution lock from the end of its MainLoop
// frame beside the game thread's next frame, up to the barrier after that frame's sim flush; display_overlap=3 starts
// it right after its own frame's sim flush and deferred updates instead.

#include <lib/winapp.h>

namespace nfsc { namespace display {

// Reads the settings; before guest code runs.
void configure();
// Stops the display thread; after the game loop has returned, before the application goes.
void shutdown();

bool overlapEnabled();
// Game thread after each MainLoop frame: starts the display call the frame took for overlap.
void frameEnd(const x86::CPU& cpu);
// Game thread: with display_overlap=mode starts the frame's display call here, beside the rest of the frame (3: after
// MainLoop's first flush, its barrier and the deferred updates; 4: after MainLoop's front end update).
void startEarly(win32::WinApplication* app, x86::CPU& cpu, int mode);
// Game thread: microseconds it waited for display calls since the last call.
uint32_t takeGameWaits();
// True while a display call runs beside the game thread.
bool concurrent();
bool onDisplayThread(const x86::CPU& cpu);
// Waits for a display call running beside the caller (execution lock released meanwhile), then runs what waited for
// it; does nothing on the display thread or when no call runs. early: not the frame's own barrier (counted).
void barrier(win32::WinApplication* app, x86::CPU& cpu, bool early = true);
// Game thread: the shim gate checks shim calls from the time a thread starts running guest code without the
// execution lock beside it (the display call, the audio update) until it has ended; one close per open.
void openShimGate(win32::WinApplication* app);
void closeShimGate();

}}

#endif
