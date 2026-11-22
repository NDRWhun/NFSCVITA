#ifndef NFSC_FRAMEPHASES_H_
#define NFSC_FRAMEPHASES_H_

// perf_log: where the slow frames' time went. The display call's passes and the game thread's frame parts are timed
// per frame; frames whose display call or game thread (its waits for the display call excluded) take over 33.4 ms
// are kept, and every 5 s one line gives how many there were and the worst three of each with their breakdown.
// Further lines give chunk loads by id, the game thread's Direct3D calls and the solid/texture pack loaders' parts.

#include <cstdint>

namespace nfsc { namespace phases {

// Display thread, after each display call: its length.
void displayDone(uint32_t microseconds);
// Game thread, after each MainLoop frame: its length without the waits for the display call.
void gameDone(uint32_t microseconds);
// Game thread: the frame's sim flush, deferred updates and barrier wait.
void sim(uint32_t microseconds);
void updates(uint32_t microseconds);
void barrier(uint32_t microseconds);
void frontEnd(uint32_t microseconds);
void loading(uint32_t microseconds);
// Game thread: one texture creation from a TextureInfo (0x725c80), for the chunk loaders' line.
void textureCreation(uint32_t microseconds);
// Game thread (or another thread than the display's): one Direct3D call of a display overlap, by its shim name.
void gameDevice(const char* shim, uint32_t microseconds);

}}

#endif
