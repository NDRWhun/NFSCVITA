#include <lib/scheduler.h>
#include <platform/config.h>

// The guest threads of NFSC.exe: start thunks and core roles by entry point. Threads without an entry here run as
// helpers; each sets its own Windows priority, which the scheduler maps onto the execution lock.
namespace win32 { namespace scheduler
{

// RWAudioCore Dac: the audio mixer thread.
static const x86::reg32 MixerEntry = 0x008a800c;
// RealCore 100 Hz tick: waits on a waitable timer, adds 1 to two tick counters only it writes, calls 8 callback
// slots that only a clear writes (always null) and re-arms the timer.
static const x86::reg32 TickEntry = 0x0088cc9c;

void configureGameThreads()
{
    // The process main thread runs the game loop (Application::execute).
    setCurrentGameThread();
    // RealCore thread start: entry at +4, parameter at +8 of the start block.
    setStartThunk(0x0088c86e, 4, 8);
    // EA thread class start: entry at +0x10, parameter at +0x14 of the thread state.
    setStartThunk(0x008daf40, 0x10, 0x14);
    setThreadRole(MixerEntry, pf::cores::Mixer, "mixer");
    // The mixer runs guest code without the execution lock, in parallel with the game thread, unless mixer_parallel=0.
    if (pf::Config::getBool("mixer_parallel", true))
        setLockFree(MixerEntry);
    setThreadRole(TickEntry, pf::cores::Helper, "tick");
    // The tick thread runs without the execution lock unless tick_parallel=0.
    if (pf::Config::getBool("tick_parallel", true))
        setLockFree(TickEntry);
}

}}
