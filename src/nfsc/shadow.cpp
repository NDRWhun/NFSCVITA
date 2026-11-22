// Shadow compares of native ports against their translated bodies.

#include "shadow.h"

#include <native.h>
#include <platform/config.h>
#include <platform/log.h>

#include <cstdio>
#include <cstring>

namespace nfsc
{
namespace shadow
{

bool Group::active()
{
    if (on_ < 0)
        on_ = pf::Config::getBool(key_, false) ? 1 : 0;
    return on_ != 0 && !running_;
}

// Logs a difference of the current call while fewer than 16 calls differed before it.
void Group::report(const char* name, const char* what, x86::reg32 guest, x86::reg32 native, int& found)
{
    ++found;
    if (differ_ < 16)
        pf::logInfo("%s: %s call %u: %s translated 0x%08x native 0x%08x", key_, name, unsigned(compared_), what,
                    unsigned(guest), unsigned(native));
}

void Group::call(const char* name, win32::WinApplication* app, x86::CPU& cpu, win32::MethodPtr guest,
                 win32::MethodPtr port, const Region* regions, size_t count, x86::reg32 live, Repeatable repeatable,
                 const void* context, const Region* scratch, size_t scratchCount)
{
    running_ = true;
    std::vector<x86::reg8> before, translated, saved;
    for (size_t k = 0; k < count; ++k)
        before.insert(before.end(), rc::host(regions[k].address), rc::host(regions[k].address) + regions[k].size);
    for (size_t k = 0; k < scratchCount; ++k)
        saved.insert(saved.end(), rc::host(scratch[k].address), rc::host(scratch[k].address) + scratch[k].size);
    x86::CPU reference = cpu;
    guest(app, reference);
    if (repeatable && !reference.terminate && !repeatable(context))
    {
        cpu = reference;
        running_ = false;
        ++kept_;
        return;
    }
    // Every region is read before any is restored: regions may overlap.
    for (size_t k = 0; k < count; ++k)
    {
        const Region& r = regions[k];
        translated.insert(translated.end(), rc::host(r.address), rc::host(r.address) + r.size);
    }
    size_t at = 0;
    for (size_t k = 0; k < scratchCount; ++k)
    {
        memcpy(rc::host(scratch[k].address), saved.data() + at, scratch[k].size);
        at += scratch[k].size;
    }
    at = 0;
    for (size_t k = 0; k < count; ++k)
    {
        memcpy(rc::host(regions[k].address), before.data() + at, regions[k].size);
        at += regions[k].size;
    }
    port(app, cpu);
    running_ = false;
    if (reference.terminate || cpu.terminate)
    {
        cpu.terminate = true;
        return;
    }
    int found = 0;
    const x86::reg32 g[] = { reference.eax, reference.ecx, reference.edx, reference.ebx, reference.esp,
                             reference.ebp, reference.esi, reference.edi };
    const x86::reg32 n[] = { cpu.eax, cpu.ecx, cpu.edx, cpu.ebx, cpu.esp, cpu.ebp, cpu.esi, cpu.edi };
    static const char* const names[] = { "eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi" };
    for (int i = 0; i < 8; ++i)
    {
        if ((live >> i & 1) && g[i] != n[i])
            report(name, names[i], g[i], n[i], found);
    }
    // The x87 status word is not compared: no caller reads its condition codes after a call returns.
    if (reference.fpu.count != cpu.fpu.count)
    {
        report(name, "x87 depth", reference.fpu.count, cpu.fpu.count, found);
    }
    else if (reference.fpu.count != 0)
    {
        x86::reg64 a, b;
        static_assert(sizeof(a) == sizeof(x86::Float), "x87 registers are doubles");
        memcpy(&a, &reference.fpu.st(0), sizeof(a));
        memcpy(&b, &cpu.fpu.st(0), sizeof(b));
        if (a != b)
            report(name, "st0 high word", x86::reg32(a >> 32), x86::reg32(b >> 32), found);
        if (x86::reg32(a) != x86::reg32(b))
            report(name, "st0 low word", x86::reg32(a), x86::reg32(b), found);
    }
    // Memory word by word (a short tail byte by byte); words that are float NaNs in both runs count apart.
    auto isNan = [](x86::reg32 v) { return (v & 0x7f800000u) == 0x7f800000u && (v & 0x7fffffu) != 0; };
    bool nanOnly = false;
    at = 0;
    for (size_t k = 0; k < count; ++k)
    {
        const Region& r = regions[k];
        bool reported = false;
        for (x86::reg32 i = 0; i < r.size && !reported;)
        {
            const x86::reg32 step = r.size - i >= 4 ? 4 : 1;
            x86::reg32 t = 0, v = 0;
            memcpy(&t, &translated[at + i], step);
            memcpy(&v, rc::host(r.address + i), step);
            if (t != v)
            {
                if (step == 4 && isNan(t) && isNan(v))
                {
                    nanOnly = true;
                }
                else
                {
                    char what[48];
                    snprintf(what, sizeof(what), "%s at 0x%08x", step == 4 ? "word" : "byte", unsigned(r.address + i));
                    report(name, what, t, v, found);
                    reported = true;
                }
            }
            i += step;
        }
        at += r.size;
    }
    tally(found, nanOnly);
}

void Group::tally(int found, bool nanOnly)
{
    ++compared_;
    if (found)
        ++differ_;
    else if (nanOnly)
        ++nanOnly_;
}

void Group::log() const
{
    if (on_ > 0 && kept_)
        pf::logInfo("%s: %u calls compared, %u differ, %u in NaN sign or payload only, %u translated only", key_,
                    unsigned(compared_), unsigned(differ_), unsigned(nanOnly_), unsigned(kept_));
    else if (on_ > 0)
        pf::logInfo("%s: %u calls compared, %u differ, %u in NaN sign or payload only", key_, unsigned(compared_),
                    unsigned(differ_), unsigned(nanOnly_));
}

}
}
