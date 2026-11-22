#ifndef NFSC_BENCH_H_
#define NFSC_BENCH_H_

// Benchmarks (bench=1), started by the game's skip-front-end options with the player's car under AI control.
// bench_mode=race: a fixed quick race, measured from the race start to the player's finish. bench_mode=roam: free
// roam in the city with traffic and cops, the AI driving to far road nodes, measured for bench_seconds from the first
// movement. Settings in docs/settings.md.

#include <cstdint>

namespace win32 { class WinApplication; }
namespace x86 { struct CPU; }

namespace nfsc { namespace bench {

// Main thread, before guest code runs: reads the settings and sets the game's skip-front-end options.
void configure();

// After SeedRandomNumber (0x6B5650): replaces the seed with a fixed one.
void seedRandom();

// After each MainLoop (0x6B7930) frame: logs state changes, starts and ends the measurement, quits after the summary
// with bench_exit=1.
void frame();

// Before AIVehicleHuman::Update (0x43C670), this in ecx: in the roam benchmark, gives the player car's AI its next
// target and resets the car when it is stuck.
void humanUpdate(win32::WinApplication* app, x86::CPU& cpu);

// Per-frame values the summary reports while measuring; one writing thread per series.
enum Series
{
    SeriesDisplay,      // display thread: one display call, microseconds
    SeriesGame,         // game thread: one MainLoop frame without its waits for the display call, microseconds
    SeriesSimSteps,     // game thread: the sim's world update events of one frame (one per sim frame, any steps)
    SeriesGpu,          // display callback: GPU time of one frame (perf::gpuBusy), microseconds
    SeriesCount
};
void sample(Series series, uint32_t value);

}}

#endif /* !NFSC_BENCH_H_ */
