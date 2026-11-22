// Game state that both the sim and the display call change (Attrib reference counts and collections, bRandom, shared
// strings, joylog, map icons): while a display call runs beside the game thread, each override runs the translated
// body under one recursive host lock. The display thread draws bRandom numbers from a seed of its own then.

#include "display.h"
#include "guestcall.h"
#include "guestlock.h"
#include "overlap.h"

#include <lib/memmap.h>
#include <native.h>
#include <recomp.h>

namespace nfsc
{

namespace
{

pf::LwMutex s_gameStateLock("game_state", true);

// bRandom's global seed; the display thread's copy, taken as each overlapped call starts.
const x86::reg32 RandomSeed = 0x00a4d3d0;
win32::MemMap* s_displaySeed = nullptr;

}

namespace overlap
{

void starting()
{
    if (!s_displaySeed)
        s_displaySeed = new win32::MemMap(16);
    rc::st32(s_displaySeed->getBlockStart(), rc::ld32(RandomSeed));
}

}

#define NFSC_GAME_STATE_LOCKED(address)                                                         \
    void sub_##address##_guest(win32::WinApplication* app, x86::CPU& cpu);                    \
    namespace native                                                                            \
    {                                                                                           \
    void sub_##address(win32::WinApplication* app, x86::CPU& cpu)                               \
    {                                                                                           \
        const ConcurrentLockGuard guard(s_gameStateLock, app, cpu);                             \
        sub_##address##_guest(app, cpu);                                                        \
    }                                                                                           \
    }

// Attrib: every function that changes a collection's reference count (the 16-bit word at +8) or frees one.
NFSC_GAME_STATE_LOCKED(461340)
NFSC_GAME_STATE_LOCKED(461380)
NFSC_GAME_STATE_LOCKED(461b50)
NFSC_GAME_STATE_LOCKED(465a30)
NFSC_GAME_STATE_LOCKED(465a80)
NFSC_GAME_STATE_LOCKED(467370)
NFSC_GAME_STATE_LOCKED(4681c0)
NFSC_GAME_STATE_LOCKED(4682b0)
NFSC_GAME_STATE_LOCKED(468880)
NFSC_GAME_STATE_LOCKED(468cc0)
NFSC_GAME_STATE_LOCKED(4691d0)
NFSC_GAME_STATE_LOCKED(469270)
NFSC_GAME_STATE_LOCKED(4693d0)
NFSC_GAME_STATE_LOCKED(469610)
NFSC_GAME_STATE_LOCKED(469870)
NFSC_GAME_STATE_LOCKED(469a20)
NFSC_GAME_STATE_LOCKED(469bc0)
NFSC_GAME_STATE_LOCKED(469c40)
NFSC_GAME_STATE_LOCKED(469cb0)
NFSC_GAME_STATE_LOCKED(469d10)
NFSC_GAME_STATE_LOCKED(469d50)
NFSC_GAME_STATE_LOCKED(46ac80)
NFSC_GAME_STATE_LOCKED(46ad10)
NFSC_GAME_STATE_LOCKED(46ad80)
NFSC_GAME_STATE_LOCKED(46b3c0)
// bRandom(int, seed*), bRandom(float, seed*).
NFSC_GAME_STATE_LOCKED(46daa0)
NFSC_GAME_STATE_LOCKED(46dae0)
// bSharedStringPool::Allocate, Free.
NFSC_GAME_STATE_LOCKED(471c80)
NFSC_GAME_STATE_LOCKED(471ec0)
// Joylog::GetData (two forms), GetSignedData, AddData (two forms), AddOrGetData (unsigned, string, wide string).
NFSC_GAME_STATE_LOCKED(6b3b30)
NFSC_GAME_STATE_LOCKED(6b3bc0)
NFSC_GAME_STATE_LOCKED(6b3b90)
NFSC_GAME_STATE_LOCKED(6b3c50)
NFSC_GAME_STATE_LOCKED(6b3c80)
NFSC_GAME_STATE_LOCKED(6b3d00)
NFSC_GAME_STATE_LOCKED(6b3db0)
NFSC_GAME_STATE_LOCKED(6b3ee0)
// GManager::AllocIcon.
NFSC_GAME_STATE_LOCKED(626f90)
// GetTextureInfo (moves the pack it found to the front of the texture pack list at 0xa90084).
NFSC_GAME_STATE_LOCKED(55cfd0)
// WCollisionMgr::GetInstanceListGuts (two forms; they mark the instances they visit).
NFSC_GAME_STATE_LOCKED(8127b0)
NFSC_GAME_STATE_LOCKED(812a70)
// VisibleSectionManager::FindDrivableSection(int) (moves the section it found to the front of the drivable list).
NFSC_GAME_STATE_LOCKED(7a0590)
// TrackStreamer::DetermineStreamingSections (its own inlined copy of that search moves the section to the front).
NFSC_GAME_STATE_LOCKED(7a4680)
// VisibleSectionManager::ActivateOverlay (another inlined copy of that search).
NFSC_GAME_STATE_LOCKED(7a3160)
// Walks of that list: FindDrivableSection(bVector3) (the display call's light flares), FindClosestBoundary.
NFSC_GAME_STATE_LOCKED(7a3290)
NFSC_GAME_STATE_LOCKED(7a03e0)

#undef NFSC_GAME_STATE_LOCKED

void sub_46db30_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_46db70_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_46daa0_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_46dae0_guest(win32::WinApplication* app, x86::CPU& cpu);

namespace
{

// bRandom of the global seed on the display thread of an overlapped call: the seeded form (cdecl: range, seed*) with
// the display thread's seed, returning as the global form (cdecl: range).
bool displayRandom(win32::WinApplication* app, x86::CPU& cpu, win32::MethodPtr seeded)
{
    if (!display::concurrent() || !display::onDisplayThread(cpu))
        return false;
    const x86::CPU guest = callGuest(app, cpu, seeded, cpu.ecx, { rc::ld32(cpu.esp + 4),
                                                                   s_displaySeed->getBlockStart() });
    const x86::reg32 esp = cpu.esp;
    const bool terminate = cpu.terminate;
    cpu = guest;
    cpu.esp = esp + 4;
    cpu.terminate = terminate;
    return true;
}

}

namespace native
{

// bRandom(int).
void sub_46db30(win32::WinApplication* app, x86::CPU& cpu)
{
    if (displayRandom(app, cpu, &sub_46daa0_guest))
        return;
    const ConcurrentLockGuard guard(s_gameStateLock, app, cpu);
    sub_46db30_guest(app, cpu);
}

// bRandom(float).
void sub_46db70(win32::WinApplication* app, x86::CPU& cpu)
{
    if (displayRandom(app, cpu, &sub_46dae0_guest))
        return;
    const ConcurrentLockGuard guard(s_gameStateLock, app, cpu);
    sub_46db70_guest(app, cpu);
}

}

}
