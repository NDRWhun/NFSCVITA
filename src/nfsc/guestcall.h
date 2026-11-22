#ifndef NFSC_GUESTCALL_H_
#define NFSC_GUESTCALL_H_

#include <lib/winapp.h>
#include <recomp.h>

#include <initializer_list>

namespace nfsc
{

// Runs a guest function on a copy of the caller's state below its stack, with ecx and the stack arguments (first
// lowest) above the caller's return address; returns the copy, a terminate request passes to the caller.
inline x86::CPU callGuest(win32::WinApplication* app, x86::CPU& cpu, win32::MethodPtr function, x86::reg32 ecx,
                          std::initializer_list<x86::reg32> arguments)
{
    x86::CPU guest = cpu;
    guest.ecx = ecx;
    guest.esp = ((cpu.esp - 0x100) & ~3u) - 4 * x86::reg32(arguments.size()) - 4;
    x86::reg32 slot = guest.esp + 4;
    for (x86::reg32 argument : arguments)
    {
        rc::st32(slot, argument);
        slot += 4;
    }
    rc::st32(guest.esp, rc::ld32(cpu.esp));
    function(app, guest);
    if (guest.terminate)
        cpu.terminate = true;
    return guest;
}

}

#endif
