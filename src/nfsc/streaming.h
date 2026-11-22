#ifndef NFSC_STREAMING_H_
#define NFSC_STREAMING_H_

// Track streaming: with section_activate_per_frame=1 the sections whose reads finish are activated one per game
// frame, as the game's other build does from TrackStreamer::ServiceGameState; the streamer's memory use and the
// calls that make room in it are counted (streaming.cpp).

#include <lib/winapp.h>

#include <cstdint>

namespace nfsc { namespace streaming {

// Totals since start for the perf_log pool line (any thread).
struct Counters
{
    uint32_t unloads;       // least recently used sections unloaded to make room
    uint32_t unloadedKb;
    uint32_t jettisons;     // sections of the current streaming list thrown out to make room (0x7a2a90)
    uint32_t makeSpace;     // TrackStreamer::MakeSpaceInPool calls
    uint32_t trackPeakKb;   // most of the Track Streaming pool in use besides the car pool
    uint32_t waiting;       // sections left loaded for ServiceGameState
};
Counters counters();

// Reads the settings; before guest code runs.
void configure();
// Game thread at MainLoop's first flush, after the frame's barrier: the activation that ServiceGameState left while
// a display call ran beside it.
void afterBarrier(win32::WinApplication* app, x86::CPU& cpu);

}}

#endif /* !NFSC_STREAMING_H_ */
