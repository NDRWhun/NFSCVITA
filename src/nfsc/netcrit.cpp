// The net library's NetCrit (owner thread, recursion count, busy flag, then a CRITICAL_SECTION the original takes
// while it owns the flag): native, so a thread that waits for another thread's NetCrit releases the execution lock
// and sleeps instead of trying again with it held. The owner may need the execution lock to leave.

#include "guestlock.h"

#include <lib/teb.h>
#include <native.h>
#include <platform/time.h>
#include <recomp.h>

namespace nfsc
{

namespace
{

// The library's own NetCrit, used when the argument is null; fields of a NetCrit.
const x86::reg32 DefaultCrit = 0x00bcbf68;
const x86::reg32 CritOwner = 0x0;
const x86::reg32 CritCount = 0x4;
const x86::reg32 CritBusy = 0x8;
// The wait between two tries while another thread owns it, as the original's Sleep(1).
const uint32_t RetryUs = 1000;

x86::reg32 crit(const x86::CPU& cpu)
{
    const x86::reg32 address = rc::ld32(cpu.esp + 4);
    return address ? address : DefaultCrit;
}

x86::reg32 threadId(const x86::CPU& cpu)
{
    return rc::ld32(cpu.efs + win32::teb::ThreadId);
}

// Takes the busy flag for the calling thread (recursion when it already owns it); false when another thread owns it.
bool tryTake(const x86::CPU& cpu, x86::reg32 address)
{
    const x86::reg32 self = threadId(cpu);
    if (rc::ld32(address + CritOwner) != self)
    {
        if (__atomic_exchange_n(reinterpret_cast<uint32_t*>(rc::host(address + CritBusy)), 1u, __ATOMIC_SEQ_CST))
            return false;
        rc::st32(address + CritOwner, self);
    }
    rc::st32(address + CritCount, rc::ld32(address + CritCount) + 1);
    return true;
}

}

namespace native
{

// NetCritEnter(crit) (cdecl).
void sub_94c850(win32::WinApplication* app, x86::CPU& cpu)
{
    const x86::reg32 address = crit(cpu);
    while (!tryTake(cpu, address) && !cpu.terminate)
    {
        const ContextRelease release(app, cpu);
        pf::sleepUs(RetryUs);
    }
    cpu.esp += 4;
}

// NetCritTry(crit) (cdecl): 1 when taken.
void sub_94c800(win32::WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    cpu.eax = tryTake(cpu, crit(cpu)) ? 1 : 0;
    cpu.esp += 4;
}

// NetCritLeave(crit) (cdecl).
void sub_94c8f0(win32::WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    const x86::reg32 address = crit(cpu);
    const x86::reg32 count = rc::ld32(address + CritCount);
    if (count > 1)
    {
        rc::st32(address + CritCount, count - 1);
    }
    else
    {
        rc::st32(address + CritOwner, 0);
        rc::st32(address + CritCount, 0);
        __atomic_store_n(reinterpret_cast<uint32_t*>(rc::host(address + CritBusy)), 0u, __ATOMIC_SEQ_CST);
    }
    cpu.esp += 4;
}

}
}
