#ifndef NFSC_SCENERY_H_
#define NFSC_SCENERY_H_

// Native scenery culling ports (displayports.cpp, switch scenery_native) and their shadow compare (scenery.cpp,
// shadow_scenery): a native plan of each call against the translated body's recorded calls.

#include <native.h>
#include <recomp.h>

#include <atomic>
#include <vector>

namespace nfsc { namespace scenery {

// The CPU whose calls are recorded (the translated body of a shadowed call), and its call log: callee, esp, ecx (0
// for cdecl callees), then the stack arguments.
extern std::atomic<const x86::CPU*> g_recordCpu;
extern std::vector<x86::reg32>* g_recordLog;

// At the entry of a recorded callee: logs the call when cpu is the recorded one and stops recording until the callee
// returns, so the callee's own calls are not logged.
class Recorded
{
public:
    Recorded(x86::reg32 callee, const x86::CPU& cpu, int args, bool thiscall)
    {
        if (g_recordCpu.load(std::memory_order_relaxed) != &cpu)
            return;
        log(callee, cpu, args, thiscall);
        g_recordCpu.store(nullptr, std::memory_order_relaxed);
        m_cpu = &cpu;
    }
    ~Recorded()
    {
        if (m_cpu)
            g_recordCpu.store(m_cpu, std::memory_order_relaxed);
    }
    Recorded(const Recorded&) = delete;
    Recorded& operator=(const Recorded&) = delete;

private:
    static void log(x86::reg32 callee, const x86::CPU& cpu, int args, bool thiscall);

    const x86::CPU* m_cpu = nullptr;
};

// A port's plan of one call: the calls it makes (the visibility tests run, the other callees are only logged) and
// what StuffScenery's inlined leaf functions would write.
struct Plan
{
    std::vector<x86::reg32> calls;
    x86::reg32 model;                       // the model being drawn (0xb69c04)
    x86::reg32 frameCursor;                 // frame allocator: cursor, full flag, overflow bytes
    x86::reg32 frameFull;
    x86::reg32 frameOverflow;
    x86::reg32 modelCounts[8];              // added to the per-kind model and mesh counters
    x86::reg32 modelMeshes[8];
    x86::reg32 argumentSlot;                // StuffScenery's view argument slot, a local of the function
};

// The ports (displayports.cpp): with a plan, nothing is written but the stack below the entry esp. False when
// the thread is to stop, or when a plan cannot follow the translated code (its node stack would leave the frame).
bool treeCull(win32::WinApplication* app, x86::CPU& cpu, Plan* plan);
bool stuffScenery(win32::WinApplication* app, x86::CPU& cpu, Plan* plan);
void treeCullNative(win32::WinApplication* app, x86::CPU& cpu);
void stuffSceneryNative(win32::WinApplication* app, x86::CPU& cpu);
void drawAScenery(win32::WinApplication* app, x86::CPU& cpu);

// GrandSceneryCullInfo::StuffScenery (0x79afa0): the native port or the translated body, by the switches.
void stuffSceneryEntry(win32::WinApplication* app, x86::CPU& cpu);

// Bench summary: the shadow_scenery counts.
void logShadow();

}}

#endif
