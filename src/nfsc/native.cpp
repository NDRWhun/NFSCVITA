// Native replacements of guest functions (tools/recompiler/generate.py NATIVE_OVERRIDES). Each one does what the
// guest function does, including its ret: on entry cpu.esp points at the return address slot.

#include "audioupdate.h"
#include "bench.h"
#include "census.h"
#include "display.h"
#include "framephases.h"

#include <lib/winapp.h>
#include <platform/config.h>
#include <platform/log.h>
#include <platform/perf.h>

#include <cmath>
#include <cstring>
#include <fpu.h>
#include <native.h>
#include <psp2/kernel/clib.h>
#include <recomp.h>
#include <vector>

namespace nfsc
{

// Translated bodies of the overridden guest functions.
void sub_43c670_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_6b5650_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_6b7930_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_8d3750_guest(win32::WinApplication* app, x86::CPU& cpu);

namespace
{

// One load and store of n bytes, in the guest's order.
inline void step(uint8_t*& to, const uint8_t*& from, uint32_t n)
{
    uint8_t v[8];
    memcpy(v, from, n);
    memcpy(to, v, n);
    to += n;
    from += n;
}

// 0x8d3750's copy, step for step: 8-byte steps below 0x200 bytes; from 0x200 on, the destination aligned to 8 when it
// is not aligned to 4, then 32-byte, 8-byte, 4, 2 and 1-byte steps. Only overlaps with the destination above the
// source depend on the steps.
void copySteps(x86::reg32 destination, x86::reg32 source, uint32_t count)
{
    uint8_t* const start = rc::host(destination);
    uint8_t* to = start;
    const uint8_t* from = rc::host(source);
    if (count < 0x200)
    {
        for (; count >= 8; count -= 8)
        {
            step(to, from, 4);
            step(to, from, 4);
        }
        for (; count; --count)
            step(to, from, 1);
        return;
    }
    if (destination & 3)
    {
        for (uint32_t n = 1; n <= 4; n <<= 1)
        {
            if ((destination + uint32_t(to - start)) & n)
            {
                step(to, from, n);
                count -= n;
            }
        }
    }
    for (; count >= 32; count -= 32)
        for (int i = 0; i < 8; ++i)
            step(to, from, 4);
    for (; count >= 8; count -= 8)
    {
        step(to, from, 4);
        step(to, from, 4);
    }
    for (uint32_t n = 4; n; n >>= 1)
    {
        if (count >= n)
        {
            step(to, from, n);
            count -= n;
        }
    }
}

// The copy of 0x8d3750 between guest addresses; the system memmove, not the byte loop clang's memmove call reaches.
void copy(x86::reg32 destination, x86::reg32 source, x86::reg32 count)
{
    if (destination > source && destination - source < count)
        copySteps(destination, source, count);
    else
        sceClibMemmove(rc::host(destination), rc::host(source), count);
}

// shadow_memcpy=1: 0x8d3750 runs translated and native on the same memory and compares the bytes and registers.
int s_memcpySetting = -1;
int s_memcpyCompared = 0;
int s_memcpyDiffer = 0;

void copyShadow(win32::WinApplication* app, x86::CPU& cpu, x86::reg32 destination, x86::reg32 source,
                x86::reg32 count)
{
    // The buffers and the rewritten guest memory need the execution lock, also on the parallel mixer thread.
    const win32::HoldContext hold(*app, cpu);
    static std::vector<uint8_t> before, guest;
    // The destination, or both ranges when they overlap.
    const bool overlap = destination < source ? source - destination < count : destination - source < count;
    const x86::reg32 low = overlap && source < destination ? source : destination;
    const uint32_t span = overlap ? (destination > source ? destination : source) + count - low : count;
    before.assign(rc::host(low), rc::host(low) + span);
    x86::CPU guestCpu = cpu;
    sub_8d3750_guest(app, guestCpu);
    guest.assign(rc::host(low), rc::host(low) + span);
    memcpy(rc::host(low), before.data(), span);
    copy(destination, source, count);
    ++s_memcpyCompared;
    uint32_t bad = 0, first = 0;
    for (uint32_t k = 0; k < span; ++k)
    {
        if (guest[k] != rc::host(low)[k] && bad++ == 0)
            first = k;
    }
    const bool registers = guestCpu.eax != destination + count || guestCpu.edx != source + count || guestCpu.ecx != 0
                        || guestCpu.ebx != cpu.ebx || guestCpu.esi != cpu.esi || guestCpu.edi != cpu.edi
                        || guestCpu.ebp != cpu.ebp || guestCpu.esp != cpu.esp + 4;
    if (bad || registers)
    {
        ++s_memcpyDiffer;
        pf::logInfo("shadow_memcpy: call %d (0x%08x <- 0x%08x, %u bytes): %u bytes differ (first 0x%08x)%s",
                    s_memcpyCompared, destination, source, count, bad, low + first,
                    registers ? ", registers differ" : "");
    }
    if (s_memcpyCompared % 10000 == 0)
        pf::logInfo("shadow_memcpy: %d calls compared, %d differ", s_memcpyCompared, s_memcpyDiffer);
    memcpy(rc::host(low), guest.data(), span);
    cpu = guestCpu;
}

}

namespace native
{

// _ftol2: st0 popped and converted to a 64-bit integer in edx:eax, truncating toward zero (it rounds with the
// current mode and corrects by the sign of the difference); NaN and values out of range give the integer
// indefinite.
void sub_87eac0(win32::WinApplication*, x86::CPU& cpu)
{
    const double value = double(cpu.fpu.pop());
    cpu.edx_eax = x86::reg64(x86::truncInteger64(value));
    cpu.esp += 4;
}

// _chkstk / _alloca_probe: moves esp down by eax bytes below the return address and returns there with eax = the
// return address (the guest pushes it again before its ret); ecx is kept. The guest touches each page on the way;
// guest stacks here are committed memory.
void sub_87eca0(win32::WinApplication*, x86::CPU& cpu)
{
    const x86::reg32 slot = cpu.esp;
    const x86::reg32 size = cpu.eax;
    cpu.eax = rc::ld32(slot);
    cpu.esp = slot + 4 - size;
}

// SeedRandomNumber: the guest function, then the benchmark's fixed seed.
void sub_6b5650(win32::WinApplication* app, x86::CPU& cpu)
{
    sub_6b5650_guest(app, cpu);
    bench::seedRandom();
}

// AIVehicleHuman::Update (this in ecx): the roam benchmark's target and reset for the player car's AI, then the guest
// function.
void sub_43c670(win32::WinApplication* app, x86::CPU& cpu)
{
    bench::humanUpdate(app, cpu);
    sub_43c670_guest(app, cpu);
}

// MainLoop: the census frame start, the end of an audio update running beside it (audio_parallel=1), the guest frame,
// the overlapped display call it took, then the benchmark's state check.
void sub_6b7930(win32::WinApplication* app, x86::CPU& cpu)
{
    census::frameStart();
    const uint32_t start = pf::perf::now();
    audioupdate::frameStart(app, cpu);
    sub_6b7930_guest(app, cpu);
    display::frameEnd(cpu);
    const uint32_t busy = pf::perf::now() - start - display::takeGameWaits();
    bench::sample(bench::SeriesGame, busy);
    phases::gameDone(busy);
    bench::frame();
}

// memcpy of the movie and audio libraries (cdecl: destination, source, count): returns eax = destination + count,
// edx = source + count, ecx = 0; ebx, esi, edi and ebp are kept.
void sub_8d3750(win32::WinApplication* app, x86::CPU& cpu)
{
    const x86::reg32 destination = rc::ld32(cpu.esp + 4);
    const x86::reg32 source = rc::ld32(cpu.esp + 8);
    const x86::reg32 count = rc::ld32(cpu.esp + 12);
    if (s_memcpySetting < 0)
        s_memcpySetting = pf::Config::getBool("shadow_memcpy", false) ? 1 : 0;
    if (s_memcpySetting)
    {
        copyShadow(app, cpu, destination, source, count);
        return;
    }
    copy(destination, source, count);
    cpu.eax = destination + count;
    cpu.edx = source + count;
    cpu.ecx = 0;
    cpu.esp += 4;
}

}
}
