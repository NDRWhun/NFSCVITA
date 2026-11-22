#ifndef NFSC_OVERLAP_H_
#define NFSC_OVERLAP_H_

// What a display call running beside the game thread (display_overlap) needs from the rest of the port: work that
// waits for the barrier, and per-call state of the display thread.

#include <lib/winapp.h>

namespace nfsc { namespace overlap {

// Game thread, holding the execution lock, as a display call starts beside it (gamestate.cpp).
void starting();
// Game thread at the barrier, after the display call: its events go to their queues (overlap.cpp), then the deferred
// frees run.
void drain(win32::WinApplication* app, x86::CPU& cpu);

// Frees asked for while a display call runs beside the game thread wait for the barrier (allocators.cpp). True when
// the heap block's free was queued.
bool deferHeapFree(x86::reg32 block);
void drainFrees(win32::WinApplication* app, x86::CPU& cpu);

// Game thread, before CheckQueuedFileCallbacks: waits for a running display call when the first finished queued file
// has its callback to run (section, stream pack and car loads).
void beforeQueuedFileCallbacks(win32::WinApplication* app, x86::CPU& cpu);

}}

#endif
