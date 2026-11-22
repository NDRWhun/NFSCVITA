#include "defer.h"
#include "audioupdate.h"
#include "bench.h"
#include "census.h"
#include "display.h"
#include "framephases.h"
#include "streaming.h"

#include <lib/memmap.h>
#include <native.h>
#include <platform/config.h>
#include <platform/perf.h>
#include <recomp.h>

#include <vector>

namespace nfsc
{

// Translated bodies of the overridden guest functions.
void sub_5612c0_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_6741f0_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_671b20_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_66c2a0_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_66b8c0_guest(win32::WinApplication* app, x86::CPU& cpu);

namespace defer
{

namespace
{

// Update events: vtable, size and the step's time at +8; the base event's vtable.
const x86::reg32 EventTime = 8;
const x86::reg32 EventSize = 12;
const x86::reg32 BaseEventVtable = 0x009ceae4;
// MainLoop's return address from its first flush.
const x86::reg32 MainLoopFirstFlush = 0x006b7aa4;

struct Deferred
{
    win32::MethodPtr body;
    x86::reg32 time;
};

bool s_enabled = false;
bool s_replaying = false;
std::vector<Deferred> s_queue;
// Stands in for the event object while a queued update runs.
win32::MemMap* s_event = nullptr;

// An update event's body: run now, or queued while deferring.
void update(win32::MethodPtr body, win32::WinApplication* app, x86::CPU& cpu)
{
    if (s_enabled && !s_replaying)
    {
        s_queue.push_back(Deferred{ body, rc::ld32(cpu.ecx + EventTime) });
        cpu.esp += 4;
        return;
    }
    census::handlerStart();
    body(app, cpu);
    census::handlerEnd();
}

// Runs the queued updates in order on a copy of the caller's state, below its stack.
void replay(win32::WinApplication* app, x86::CPU& cpu)
{
    if (!s_event)
        s_event = new win32::MemMap(16);
    const x86::reg32 event = s_event->getBlockStart();
    s_replaying = true;
    uint32_t steps = 0;
    for (const Deferred& deferred : s_queue)
        steps += deferred.body == &sub_6741f0_guest;
    bench::sample(bench::SeriesSimSteps, steps);
    for (const Deferred& deferred : s_queue)
    {
        rc::st32(event, BaseEventVtable);
        rc::st32(event + 4, EventSize);
        rc::st32(event + EventTime, deferred.time);
        x86::CPU guest = cpu;
        guest.ecx = event;
        guest.esp = (cpu.esp - 0x100) & ~3u;
        rc::st32(guest.esp, rc::ld32(cpu.esp - 4));
        update(deferred.body, app, guest);
        if (guest.terminate)
        {
            cpu.terminate = true;
            break;
        }
    }
    s_queue.clear();
    s_replaying = false;
}

}

void configure()
{
    s_enabled = pf::Config::getBool("display_defer", true) || display::overlapEnabled();
}

}

namespace native
{

// EventSys flush: runs the queued events, then the updates deferred during it; MainLoop's first flush starts after an
// audio update running beside it (audio_parallel=2), and after it waits for a display call running beside it first,
// activates the track section the last frame's ServiceGameState left (section_activate_per_frame), and may start the
// frame's own display call (display_overlap=3).
void sub_5612c0(win32::WinApplication* app, x86::CPU& cpu)
{
    const bool first = rc::ld32(cpu.esp) == defer::MainLoopFirstFlush;
    if (first)
        audioupdate::firstFlush(app, cpu);
    const uint32_t start = pf::perf::now();
    if (first)
    {
        const audioupdate::PartScope part(pf::perf::AudioLockSim);
        sub_5612c0_guest(app, cpu);
    }
    else
    {
        sub_5612c0_guest(app, cpu);
    }
    if (first)
        phases::sim(pf::perf::now() - start);
    if (first)
        display::barrier(app, cpu, false);
    if (!defer::s_queue.empty() && !cpu.terminate && !display::concurrent())
    {
        const uint32_t replayStart = pf::perf::now();
        defer::replay(app, cpu);
        phases::updates(pf::perf::now() - replayStart);
    }
    if (first && !display::concurrent())
        streaming::afterBarrier(app, cpu);
    if (first)
        display::startEarly(app, cpu, 3);
}

// The sim's world update event (PrepareWorldTimestep, ICEManager::Update, WorldConn::UpdateServices).
void sub_6741f0(win32::WinApplication* app, x86::CPU& cpu)
{
    defer::update(&sub_6741f0_guest, app, cpu);
}

// The sim's render update event (RenderConn::UpdateServices).
void sub_671b20(win32::WinApplication* app, x86::CPU& cpu)
{
    defer::update(&sub_671b20_guest, app, cpu);
}

// The sim's camera update event (CameraAI::Update, Camera::UpdateAll).
void sub_66c2a0(win32::WinApplication* app, x86::CPU& cpu)
{
    defer::update(&sub_66c2a0_guest, app, cpu);
}

// The sim's animation update event: deferred like the others with display_overlap, else run at once.
void sub_66b8c0(win32::WinApplication* app, x86::CPU& cpu)
{
    if (display::overlapEnabled())
        defer::update(&sub_66b8c0_guest, app, cpu);
    else
        sub_66b8c0_guest(app, cpu);
}

}
}
