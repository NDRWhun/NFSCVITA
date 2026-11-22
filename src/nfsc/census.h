#ifndef NFSC_CENSUS_H_
#define NFSC_CENSUS_H_

// display_census=1: which guest memory each part of a race frame writes. In sampled race frames the guest memory in
// use (the image's data section and every guest block) is hashed in 1 KB lines where a part starts and ends: the
// game thread's frame up to the display call ("main"), the display call ("display") and the rest of the frame after
// it ("post"); display_census=2 also separates the sim's world, render and camera updates ("handlers") from "main".
// Lines are mapped to the allocation that holds them (game heap, FastMem block or pool slot, with the guest code that
// allocated it). census.txt in the data folder is rewritten every 8 samples and when the race ends.
// Hashing sees writes only, and also those of threads that run at the same time (audio mixer, file reads). The
// census build (NFSC_CENSUS_ACCESS) also marks the lines the display call's game code reads; display_census=3 there
// records only those reads, without hashing.

#include <lib/winapp.h>

namespace nfsc { namespace census {

enum Kind
{
    HeapBlock,      // bWareMalloc
    FastMemBlock,   // FastMem::Alloc
    PoolSlot,       // a slot pool's slot
    KindCount
};

extern bool g_tracking;

// Reads the settings; before guest code runs. path: the report file.
void configure(const char* path);

// Allocation tracking (only while g_tracking): site is the guest return address of the allocating call, name a
// guest string pointer or a pool object (0: none).
void allocated(Kind kind, x86::reg32 address, x86::reg32 size, x86::reg32 site, x86::reg32 name);
void released(Kind kind, x86::reg32 address);

// Game thread, holding the execution lock: frame part boundaries.
void frameStart();
void displayStart();
void displayEnd();
// Around the sim's update events (display_census=2).
void handlerStart();
void handlerEnd();

// display_census 1-3 records frames (display_overlap stays off then).
bool recordsFrames();
// display_census=4 in the census build, with display_overlap: lines the display thread reads and the game thread
// writes while an overlapped call runs are conflicts; the first ones are logged, all go to census.txt.
// Display thread, once at its start; game thread as an overlapped call starts, and at its barrier.
void displayThreadStarted();
void overlapStart();
void overlapEnd();
// display_census=5 in the census build, with audio_parallel: while an audio update runs on the audio thread, lines the
// audio thread reads and the game thread writes are conflicts, and in every second update lines the game thread reads
// and the audio thread writes; reported as with 4. Audio thread, once at its start; game thread as an update starts
// beside it, and once it has ended.
void audioThreadStarted();
void audioStart();
void audioEnd();

}}

#endif
