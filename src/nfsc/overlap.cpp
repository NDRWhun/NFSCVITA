// Around a display call running beside the game thread (display_overlap): events the display thread posts wait for
// the barrier in a buffer of its own, and code that loads or unloads data waits for the barrier first (and for an audio
// update running beside the game thread, audio_parallel).

#include "audioupdate.h"
#include "display.h"
#include "framephases.h"
#include "guestcall.h"
#include "guestlock.h"
#include "overlap.h"

#include <lib/memmap.h>
#include <native.h>
#include <platform/log.h>
#include <platform/perf.h>
#include <recomp.h>

#include <cstring>
#include <vector>

namespace nfsc
{

// Translated bodies of the overridden guest functions.
void sub_561590_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_6b68d0_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_725b40_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_5bb510_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_726090_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_6b7be0_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_45e690_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_45b050_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_453d80_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_45ba20_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_7d9320_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_7da310_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_7e3b40_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_7e3be0_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_7bf630_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_7a7060_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_7a2a20_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_7e3580_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_7e4700_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_6ad8f0_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_759510_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_55bf70_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_79f150_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_750e30_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_55bb60_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_45d830_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_699d20_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_6a8ad0_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_7a2ed0_guest(win32::WinApplication* app, x86::CPU& cpu);

namespace native
{
void sub_561590(win32::WinApplication* app, x86::CPU& cpu);
}

namespace overlap
{

namespace
{

// EventSys queues by index (base, end, read position).
const x86::reg32 EventQueues = 0x00a95230;
const x86::reg32 EventChunkSize = 64 * 1024;
// ServiceResourceLoading's queued and newly added load completion callbacks.
const x86::reg32 LoadCallbacks = 0x00a995dc;
const x86::reg32 NewLoadCallbacks = 0x00a995e4;
// MainLoop's return address from FEManager::Update.
const x86::reg32 MainLoopFrontEndUpdate = 0x006b7b4a;
// The queued file list: count, first entry (its status at +0x40: 2 done, 3 failed), joylog playback flag.
const x86::reg32 QueuedFileCount = 0x00a99698;
const x86::reg32 QueuedFileFirst = 0x00a99a94;
const x86::reg32 QueuedFileStatus = 0x40;
const x86::reg32 JoylogPlayback = 0x00a62ecc;

pf::LwMutex s_eventLock("event_alloc", true);

struct PostedEvent
{
    x86::reg32 queue;
    x86::reg32 size;
    x86::reg32 address;
};

// The display thread's events of the running call, in order, in guest chunks of their own.
std::vector<win32::MemMap*> s_eventChunks;
size_t s_chunk = 0;
x86::reg32 s_chunkUsed = 0;
std::vector<PostedEvent> s_posted;
// The engine's two deferred Direct3D release passes, skipped while a display call ran.
bool s_releasesWaiting = false;
bool s_frameReleasesWaiting = false;

// Room for an event in the display thread's chunks, 16-byte aligned as in a queue.
x86::reg32 postedEvent(x86::reg32 size, x86::reg32 queue, x86::reg32 caller)
{
    const x86::reg32 rounded = (size + 15) & ~15u;
    if (rounded > EventChunkSize)
        x86::guestTrap(caller, "display thread event larger than its buffer");
    if (s_chunk < s_eventChunks.size() && s_chunkUsed + rounded > EventChunkSize)
    {
        ++s_chunk;
        s_chunkUsed = 0;
    }
    if (s_chunk == s_eventChunks.size())
        s_eventChunks.push_back(new win32::MemMap(EventChunkSize));
    const x86::reg32 address = s_eventChunks[s_chunk]->getBlockStart() + s_chunkUsed;
    s_chunkUsed += rounded;
    s_posted.push_back(PostedEvent{ queue, size, address });
    return address;
}

}

void drain(win32::WinApplication* app, x86::CPU& cpu)
{
    for (const PostedEvent& event : s_posted)
    {
        const x86::CPU guest = callGuest(app, cpu, &native::sub_561590, cpu.ecx, { event.size, event.queue });
        if (guest.eax)
            memcpy(rc::host(guest.eax), rc::host(event.address), event.size);
    }
    s_posted.clear();
    s_chunk = 0;
    s_chunkUsed = 0;
    if (s_releasesWaiting && !cpu.terminate)
        callGuest(app, cpu, &sub_725b40_guest, cpu.ecx, {});
    if (s_frameReleasesWaiting && !cpu.terminate)
        callGuest(app, cpu, &sub_726090_guest, cpu.ecx, {});
    s_releasesWaiting = false;
    s_frameReleasesWaiting = false;
    drainFrees(app, cpu);
}

void beforeQueuedFileCallbacks(win32::WinApplication* app, x86::CPU& cpu)
{
    if (!display::concurrent() || !rc::ld32(QueuedFileCount))
        return;
    const x86::reg32 status = rc::ld32(rc::ld32(QueuedFileFirst) + QueuedFileStatus);
    if (status == 2 || status == 3 || rc::ld8(JoylogPlayback))
        display::barrier(app, cpu);
}

}

namespace native
{

// EventSys event allocation in a queue (cdecl: size, queue index): the display thread's events of an overlapped call
// go to its own buffer until the barrier, the others are allocated under a lock.
void sub_561590(win32::WinApplication* app, x86::CPU& cpu)
{
    const x86::reg32 size = rc::ld32(cpu.esp + 4);
    const x86::reg32 queue = rc::ld32(cpu.esp + 8);
    if (size && display::concurrent() && display::onDisplayThread(cpu))
    {
        cpu.eax = overlap::postedEvent(size, queue, rc::ld32(cpu.esp));
        cpu.ecx = rc::ld32(overlap::EventQueues + 4 * queue);
        cpu.esp += 4;
        return;
    }
    const ConcurrentLockGuard guard(overlap::s_eventLock, app, cpu);
    sub_561590_guest(app, cpu);
}

// After a running audio update and a running display call; on the audio thread they run on the game thread.
#define NFSC_AFTER_BARRIER(address)                                                             \
    void sub_##address(win32::WinApplication* app, x86::CPU& cpu)                               \
    {                                                                                           \
        if (audioupdate::handOver(app, cpu, &sub_##address))                                    \
            return;                                                                             \
        audioupdate::barrier(app, cpu);                                                         \
        display::barrier(app, cpu);                                                             \
        sub_##address##_guest(app, cpu);                                                        \
    }

// Character and NIS animation the sim advances (it rewrites the poses the display call skins with):
// CAnimScene::UpdateTime, CAnimCtrl::UpdateAnimPose, CAnimCtrl::AdvanceAnimTime,
// CBasicCharacterAnimEntity::UpdateTimeStep.
NFSC_AFTER_BARRIER(45e690)
NFSC_AFTER_BARRIER(45b050)
NFSC_AFTER_BARRIER(453d80)
NFSC_AFTER_BARRIER(45ba20)
// Vehicle render connections, which the display call walks and renders: the VehicleRenderConn constructor (joins the
// list), the VehicleRenderConn and CarRenderConn destructors (HeliRenderConn's tail-jumps to the first) and OnLoaded.
NFSC_AFTER_BARRIER(7da310)
NFSC_AFTER_BARRIER(7e3b40)
NFSC_AFTER_BARRIER(7e3be0)
NFSC_AFTER_BARRIER(7bf630)
// Streaming decisions read the pools' free memory, which frees deferred to the barrier would understate (sections and
// cars thrown out to make room that is already there): TrackStreamer::HandleMemoryAllocation, CarLoader::
// MakeSpaceInCarMemoryPool and MakeSpaceInPool (TrackStreamer::MakeSpaceInPool is in streaming.cpp).
// TrackStreamer::UnactivateSection unloads the chunks of a section the display call may be drawing (ActivateSection,
// which loads them, is in streaming.cpp).
NFSC_AFTER_BARRIER(7a7060)
NFSC_AFTER_BARRIER(7e3580)
NFSC_AFTER_BARRIER(7e4700)
NFSC_AFTER_BARRIER(7a2a20)
// LoadChunks: every chunk load (scenery, solids, texture packs, lights...), also from ResourceFile's own service
// without queued callbacks. UnloadChunks waits for rendering itself (0x732840).
NFSC_AFTER_BARRIER(6ad8f0)
// From the census: AcidEffect's constructor (joins the emitter system's effect list the display draws), eModel::UnInit
// (also from AI and car code outside chunk unloads) and the scenery hierarchy helper 0x79f150.
NFSC_AFTER_BARRIER(759510)
NFSC_AFTER_BARRIER(55bf70)
NFSC_AFTER_BARRIER(79f150)
// From the race census: AcidEffect's destructor, eStreamPackLoader::UnloadStreamingEntry, CAnimScene::UnBindToGame.
NFSC_AFTER_BARRIER(750e30)
NFSC_AFTER_BARRIER(55bb60)
NFSC_AFTER_BARRIER(45d830)
// Scenery groups and the world's topology changed by race and career events (barriers, NIS world animations):
// EnableBarrierSceneryGroup, RedoTopologyAndSceneryGroups and the per-section group refresh 0x7a2ed0.
NFSC_AFTER_BARRIER(699d20)
NFSC_AFTER_BARRIER(6a8ad0)
NFSC_AFTER_BARRIER(7a2ed0)
// UnloadResourceFile, World_Shutdown.
NFSC_AFTER_BARRIER(6b7be0)
NFSC_AFTER_BARRIER(7d9320)

#undef NFSC_AFTER_BARRIER

// ServiceResourceLoading: after the barriers when it has load completion callbacks to run (counts at 0xa995dc and
// 0xa995e4); starting file reads needs no wait. On the audio thread it runs on the game thread.
void sub_6b68d0(win32::WinApplication* app, x86::CPU& cpu)
{
    if (audioupdate::handOver(app, cpu, &sub_6b68d0))
        return;
    if (rc::ld32(overlap::LoadCallbacks) || rc::ld32(overlap::NewLoadCallbacks))
    {
        audioupdate::barrier(app, cpu);
        display::barrier(app, cpu);
    }
    const uint32_t start = pf::perf::now();
    sub_6b68d0_guest(app, cpu);
    phases::loading(pf::perf::now() - start);
}

// FEManager::Update; MainLoop's call (display_overlap=4) starts the frame's display call after it.
void sub_5bb510(win32::WinApplication* app, x86::CPU& cpu)
{
    const bool mainLoop = rc::ld32(cpu.esp) == overlap::MainLoopFrontEndUpdate;
    // The front end and HUD objects (FEMinList nodes, visibility) change here while FEngine::Render walks them.
    display::barrier(app, cpu);
    const uint32_t start = pf::perf::now();
    sub_5bb510_guest(app, cpu);
    phases::frontEnd(pf::perf::now() - start);
    if (mainLoop)
        display::startEarly(app, cpu, 4);
}

// The engine's passes that release up to three Direct3D objects queued for release (lists at 0xb1dbf8 and 0xb1eab0):
// while a display call runs beside the game thread they run at the barrier instead.
void sub_725b40(win32::WinApplication* app, x86::CPU& cpu)
{
    if (display::concurrent() && !display::onDisplayThread(cpu))
    {
        overlap::s_releasesWaiting = true;
        cpu.esp += 4;
        return;
    }
    sub_725b40_guest(app, cpu);
}

void sub_726090(win32::WinApplication* app, x86::CPU& cpu)
{
    if (display::concurrent() && !display::onDisplayThread(cpu))
    {
        overlap::s_frameReleasesWaiting = true;
        cpu.esp += 4;
        return;
    }
    sub_726090_guest(app, cpu);
}

// The engine's empty wait for the end of rendering, called before chunks unload, streaming sections and entries are
// freed, car skins and logos are rewritten and the front end's cars go: the barriers of the audio update and the
// display call.
void sub_732840(win32::WinApplication* app, x86::CPU& cpu)
{
    if (audioupdate::handOver(app, cpu, &sub_732840))
        return;
    audioupdate::barrier(app, cpu);
    display::barrier(app, cpu);
    cpu.esp += 4;
}

}
}
