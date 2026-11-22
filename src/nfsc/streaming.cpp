// Track section activation. TrackStreamer::SectionLoadedCallback (0x79e780) activates a section as soon as its read
// completes (among ServiceResourceLoading's file callbacks) when it is in the current streaming list, and
// StartLoadingSections (0x7a5590) activates every loaded section of that list during a loading phase, so several
// sections can be activated in one frame. The game's other build can leave them loaded instead
// (NeedsGameStateActivation) and activate one per call of ServiceGameState: the first loaded one of the current
// streaming list (GetSectionToActivate). The PC executable has that call compiled out; section_activate_per_frame=1
// does the same natively. While World_Service calls ServiceGameState, the two calls leave their sections loaded and
// each of those ServiceGameState calls activates one of them. The waits for the end of loading, ServiceGameState
// calls from the loading screens and ServiceNonGameState once the world is no longer serviced activate all of them.

#include "streaming.h"

#include "display.h"
#include "guestcall.h"

#include <platform/config.h>
#include <platform/log.h>
#include <platform/time.h>
#include <recomp.h>

#include <algorithm>
#include <atomic>
#include <vector>

namespace nfsc
{

// Translated bodies of the overridden guest functions.
void sub_79e670_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_7a85e0_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_7a7e30_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_7a7e10_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_79e700_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_7a2a90_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_7a81b0_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_79def0_guest(win32::WinApplication* app, x86::CPU& cpu);

namespace streaming
{

namespace
{

// TheTrackStreamer: sections +0, current streaming sections +0x330 (count +0x32C, at most 256).
const x86::reg32 TrackStreamer = 0x00B70650;
const x86::reg32 TsSections = 0x000;
const x86::reg32 TsCurrentCount = 0x32C;
const x86::reg32 TsCurrent = 0x330;
const x86::reg32 MaxCurrent = 256;
// Section records: in the current streaming list while +0x0B (a bit per streaming position) is set; status +0x0C,
// 3 loaded.
const x86::reg32 SecVisible = 0x0B;
const x86::reg32 SecStatus = 0x0C;
const x86::reg32 StatusLoaded = 3;
// Return addresses of ActivateSection's calls in SectionLoadedCallback and StartLoadingSections, and of
// ServiceGameState's call in World_Service.
const x86::reg32 LoadedCallbackReturn = 0x0079e7bb;
const x86::reg32 StartLoadingReturn = 0x007a560d;
const x86::reg32 WorldServiceReturn = 0x007dcd3e;
// Activations are left for ServiceGameState while World_Service called it this recently.
const uint32_t WorldServiceMs = 250;
// Return addresses of the calls of the section unload (0x79e700) that free the least recently used section:
// HandleMemoryAllocation's two and MakeSpaceInPool's.
const x86::reg32 UnloadReturns[] = { 0x007a70ad, 0x007a7132, 0x007a820c };
// Section record: loaded size +0x54.
const x86::reg32 SecLoadedSize = 0x54;
// TheTrackStreamer's TSMemoryPool +0x73C: total size +0x08, free +0x14. The car pool's size in it (CarLoader).
const x86::reg32 TsMemoryPool = 0x73C;
const x86::reg32 PoolTotal = 0x08;
const x86::reg32 PoolFree = 0x14;
const x86::reg32 CarPoolSize = 0x00B77084;

std::atomic<uint32_t> s_unloads{ 0 };
std::atomic<uint32_t> s_unloadedKb{ 0 };
std::atomic<uint32_t> s_jettisons{ 0 };
std::atomic<uint32_t> s_makeSpace{ 0 };
std::atomic<uint32_t> s_trackPeak{ 0 };
std::atomic<uint32_t> s_waiting{ 0 };

bool s_enabled = false;
// Sections left loaded, in the order they were left, and the section table they belong to.
std::vector<x86::reg32> s_pending;
x86::reg32 s_table = 0;
bool s_worldServiced = false;
uint32_t s_worldServiceMs = 0;
int s_blocking = 0;
// ServiceGameState left its activation while a display call ran beside it.
bool s_owed = false;

bool worldServiced()
{
    return s_worldServiced && pf::ticksMs() - s_worldServiceMs < WorldServiceMs;
}

bool leaves(x86::reg32 caller)
{
    return s_enabled && !s_blocking && (caller == LoadedCallbackReturn || caller == StartLoadingReturn) &&
           worldServiced();
}

void leave(x86::reg32 section)
{
    if (s_pending.empty())
        s_table = rc::ld32(TrackStreamer + TsSections);
    if (std::find(s_pending.begin(), s_pending.end(), section) == s_pending.end())
        s_pending.push_back(section);
    s_waiting.store(uint32_t(s_pending.size()), std::memory_order_relaxed);
}

// Drops the sections no longer left loaded: activated or unloaded since, or of a table that has gone.
void prune()
{
    if (rc::ld32(TrackStreamer + TsSections) != s_table)
    {
        s_pending.clear();
        return;
    }
    s_pending.erase(std::remove_if(s_pending.begin(), s_pending.end(),
                                   [](x86::reg32 s) { return rc::ld32(s + SecStatus) != StatusLoaded; }),
                    s_pending.end());
}

void activate(win32::WinApplication* app, x86::CPU& cpu, x86::reg32 section)
{
    display::barrier(app, cpu);
    callGuest(app, cpu, &sub_79e670_guest, TrackStreamer, { section });
}

// GetSectionToActivate: the first section of the current streaming list that was left loaded.
void activateOne(win32::WinApplication* app, x86::CPU& cpu)
{
    prune();
    const x86::reg32 count = std::min(rc::ld32(TrackStreamer + TsCurrentCount), MaxCurrent);
    for (x86::reg32 i = 0; i < count && !s_pending.empty(); ++i)
    {
        const x86::reg32 section = rc::ld32(TrackStreamer + TsCurrent + 4 * i);
        const auto it = std::find(s_pending.begin(), s_pending.end(), section);
        if (it == s_pending.end())
            continue;
        s_pending.erase(it);
        s_waiting.store(uint32_t(s_pending.size()), std::memory_order_relaxed);
        activate(app, cpu, section);
        return;
    }
    s_waiting.store(uint32_t(s_pending.size()), std::memory_order_relaxed);
}

// Every section left loaded that is still in the current streaming list, in the order they were left.
void activateAll(win32::WinApplication* app, x86::CPU& cpu)
{
    prune();
    std::vector<x86::reg32> sections;
    sections.swap(s_pending);
    s_waiting.store(0, std::memory_order_relaxed);
    for (x86::reg32 section : sections)
    {
        if (!cpu.terminate && rc::ld8(section + SecVisible) && rc::ld32(section + SecStatus) == StatusLoaded)
            activate(app, cpu, section);
    }
}

}

void configure()
{
    s_enabled = pf::Config::getBool("section_activate_per_frame", false);
    if (s_enabled)
        pf::logInfo("streaming: track sections activated one per game frame");
}

Counters counters()
{
    Counters c;
    c.unloads = s_unloads.load(std::memory_order_relaxed);
    c.unloadedKb = s_unloadedKb.load(std::memory_order_relaxed);
    c.jettisons = s_jettisons.load(std::memory_order_relaxed);
    c.makeSpace = s_makeSpace.load(std::memory_order_relaxed);
    c.trackPeakKb = s_trackPeak.load(std::memory_order_relaxed) / 1024;
    c.waiting = s_waiting.load(std::memory_order_relaxed);
    return c;
}

void afterBarrier(win32::WinApplication* app, x86::CPU& cpu)
{
    if (!s_owed)
        return;
    s_owed = false;
    if (!s_pending.empty() && !cpu.terminate)
        activateOne(app, cpu);
}

}

namespace native
{

// TrackStreamer::ActivateSection (thiscall: the section) loads the section's chunks, after the display overlap's
// barrier. With section_activate_per_frame, the calls of SectionLoadedCallback and StartLoadingSections leave the
// section loaded while the world is serviced.
void sub_79e670(win32::WinApplication* app, x86::CPU& cpu)
{
    if (streaming::leaves(rc::ld32(cpu.esp)))
    {
        streaming::leave(rc::ld32(cpu.esp + 4));
        cpu.esp += 8;
        return;
    }
    display::barrier(app, cpu);
    sub_79e670_guest(app, cpu);
}

// TrackStreamer::ServiceGameState; then, from World_Service, one section left loaded is activated (at MainLoop's next
// first flush when a display call runs beside it); from the loading screens, all of them.
void sub_7a85e0(win32::WinApplication* app, x86::CPU& cpu)
{
    const bool world = rc::ld32(cpu.esp) == streaming::WorldServiceReturn;
    sub_7a85e0_guest(app, cpu);
    if (!streaming::s_enabled || cpu.terminate)
        return;
    streaming::s_worldServiced = world;
    if (world)
        streaming::s_worldServiceMs = pf::ticksMs();
    if (streaming::s_pending.empty())
        return;
    if (!world)
        streaming::activateAll(app, cpu);
    else if (display::concurrent())
        streaming::s_owed = true;
    else
        streaming::activateOne(app, cpu);
}

// TrackStreamer::WaitForCurrentLoadingToComplete (BlockUntilLoadingComplete, MakeSpaceInPool, ...) runs the loading
// until every section of the current streaming list is activated: the sections left loaded are activated first, and
// none is left during the wait.
void sub_7a7e30(win32::WinApplication* app, x86::CPU& cpu)
{
    if (!streaming::s_enabled)
    {
        sub_7a7e30_guest(app, cpu);
        return;
    }
    ++streaming::s_blocking;
    streaming::activateAll(app, cpu);
    sub_7a7e30_guest(app, cpu);
    --streaming::s_blocking;
}

// TrackStreamer::ServiceNonGameState (every MainLoop frame and loading screen step): once World_Service no longer
// calls ServiceGameState, the sections left loaded are activated first.
void sub_7a7e10(win32::WinApplication* app, x86::CPU& cpu)
{
    if (streaming::s_enabled && !streaming::s_pending.empty() && !streaming::worldServiced())
        streaming::activateAll(app, cpu);
    sub_7a7e10_guest(app, cpu);
}

// The section unload (thiscall: the section); counted with its size when HandleMemoryAllocation or MakeSpaceInPool
// frees the least recently used section.
void sub_79e700(win32::WinApplication* app, x86::CPU& cpu)
{
    const x86::reg32 caller = rc::ld32(cpu.esp);
    for (x86::reg32 address : streaming::UnloadReturns)
    {
        if (caller != address)
            continue;
        const x86::reg32 section = rc::ld32(cpu.esp + 4);
        if (rc::ld32(section + streaming::SecStatus) == streaming::StatusLoaded)
        {
            streaming::s_unloads.fetch_add(1, std::memory_order_relaxed);
            streaming::s_unloadedKb.fetch_add(rc::ld32(section + streaming::SecLoadedSize) / 1024,
                                              std::memory_order_relaxed);
        }
        break;
    }
    sub_79e700_guest(app, cpu);
}

// Throws a section of the current streaming list out to make room (thiscall: the section), counted.
void sub_7a2a90(win32::WinApplication* app, x86::CPU& cpu)
{
    streaming::s_jettisons.fetch_add(1, std::memory_order_relaxed);
    sub_7a2a90_guest(app, cpu);
}

// TrackStreamer::MakeSpaceInPool(size, jettison), counted; after the display overlap's barrier, since it decides on
// the pool's free memory.
void sub_7a81b0(win32::WinApplication* app, x86::CPU& cpu)
{
    streaming::s_makeSpace.fetch_add(1, std::memory_order_relaxed);
    display::barrier(app, cpu);
    sub_7a81b0_guest(app, cpu);
}

// TSMemoryPool::Malloc; for the Track Streaming pool, the most it held besides the car pool (while the car pool is
// allocated) is kept.
void sub_79def0(win32::WinApplication* app, x86::CPU& cpu)
{
    const x86::reg32 pool = cpu.ecx;
    sub_79def0_guest(app, cpu);
    const x86::reg32 cars = rc::ld32(streaming::CarPoolSize);
    if (!cars || pool != rc::ld32(streaming::TrackStreamer + streaming::TsMemoryPool))
        return;
    const x86::reg32 used = rc::ld32(pool + streaming::PoolTotal) - rc::ld32(pool + streaming::PoolFree);
    if (used < cars)
        return;
    uint32_t peak = streaming::s_trackPeak.load(std::memory_order_relaxed);
    while (used - cars > peak && !streaming::s_trackPeak.compare_exchange_weak(peak, used - cars))
    {
    }
}

}
}
