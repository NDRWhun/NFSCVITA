// Native bodies of the sound library functions the bank routines call (table 0xa7d6c0). Each takes the guest
// address of its stack argument, returns eax and computes in the translated code's double steps and float stores.
// banklib_native=0 runs the translated functions instead; shadow_banklib=1 compares every call with them.

#include "bankcode.h"
#include "shadow.h"

#include <lib/winapp.h>
#include <platform/config.h>
#include <recomp.h>
#include <x86.h>

namespace nfsc
{

void sub_897ba8(win32::WinApplication* app, x86::CPU& cpu);
void sub_897bb7(win32::WinApplication* app, x86::CPU& cpu);
void sub_897e88(win32::WinApplication* app, x86::CPU& cpu);
void sub_897eca(win32::WinApplication* app, x86::CPU& cpu);
void sub_897fda(win32::WinApplication* app, x86::CPU& cpu);
void sub_898014(win32::WinApplication* app, x86::CPU& cpu);
void sub_89805f(win32::WinApplication* app, x86::CPU& cpu);
void sub_898090(win32::WinApplication* app, x86::CPU& cpu);
void sub_89819f(win32::WinApplication* app, x86::CPU& cpu);
void sub_89832d(win32::WinApplication* app, x86::CPU& cpu);
void sub_89834b(win32::WinApplication* app, x86::CPU& cpu);
void sub_8983b8(win32::WinApplication* app, x86::CPU& cpu);
void sub_8983f4(win32::WinApplication* app, x86::CPU& cpu);
void sub_898423(win32::WinApplication* app, x86::CPU& cpu);
void sub_898440(win32::WinApplication* app, x86::CPU& cpu);
void sub_898487(win32::WinApplication* app, x86::CPU& cpu);
void sub_89849f(win32::WinApplication* app, x86::CPU& cpu);
void sub_8984ec(win32::WinApplication* app, x86::CPU& cpu);
void sub_8985d6(win32::WinApplication* app, x86::CPU& cpu);
void sub_898675(win32::WinApplication* app, x86::CPU& cpu);
void sub_89869f(win32::WinApplication* app, x86::CPU& cpu);
void sub_8986b5(win32::WinApplication* app, x86::CPU& cpu);

namespace bankcode
{

namespace
{

// 0: translated functions, 1: native bodies, 2: native bodies compared with the translated functions.
int s_mode = 1;

shadow::Group s_shadow("shadow_banklib");

// Registers a caller of these functions reads back: eax, esp and the callee-saved ones.
constexpr x86::reg32 LiveRegisters = 0xf9;

// Guest state of the random generator 0x89e1ec.
constexpr x86::reg32 RandomState = 0xbbf420;
constexpr x86::reg32 RandomStateSize = 0x18;
// Mixer tick period in seconds (float), set by the sound system.
constexpr x86::reg32 TickPeriod = 0xbbee68;
// 16-bit quarter sine table read forwards and backwards by 0x89e28d.
constexpr x86::reg32 SineForward = 0xa7d848;
constexpr x86::reg32 SineBackward = 0xa7da48;

using Body = x86::reg32 (*)(const x86::CPU& cpu, x86::reg32 p);
using Regions = size_t (*)(x86::reg32 p, shadow::Region* out);

inline x86::sreg32 s32(x86::reg32 value)
{
    return x86::sreg32(value);
}

inline double fild(x86::reg32 address)
{
    return double(s32(rc::ld32(address)));
}

// fistp in the guest's rounding mode.
inline x86::reg32 fist(const x86::CPU& cpu, double value)
{
    return x86::reg32(x86::fistInteger<x86::sreg32>(x86::roundIntegral(value, cpu.fpu.control.rc)));
}

// 0x8979a4: eax clamped to [lo, hi].
inline x86::reg32 clamp(x86::reg32 value, x86::reg32 lo, x86::reg32 hi)
{
    if (s32(value) < s32(lo))
        return lo;
    return s32(value) > s32(hi) ? hi : value;
}

// 0x89e1ec: multiply-with-carry style generator over five words and a counter.
x86::reg32 random()
{
    x86::reg32 ecx = rc::ld32(0xbbf430);
    x86::reg32 esi = rc::ld32(0xbbf434);
    x86::reg32 eax = ecx + esi;
    x86::reg32 edx = (eax < esi || eax < ecx) ? 1 : 0;
    ecx = rc::ld32(0xbbf42c);
    edx += ecx;
    rc::st32(0xbbf430, eax);
    eax += edx;
    edx = rc::ld32(0xbbf428);
    ecx = x86::reg32(eax < ecx) + edx;
    rc::st32(0xbbf42c, eax);
    eax += ecx;
    const x86::reg32 word424 = rc::ld32(0xbbf424);
    ecx = x86::reg32(eax < edx) + word424;
    rc::st32(0xbbf428, eax);
    eax += ecx;
    ecx = x86::reg32(eax < word424);
    rc::st32(0xbbf424, eax);
    eax += ecx;
    rc::st32(0xbbf420, rc::ld32(0xbbf420) + eax);
    eax = rc::ld32(0xbbf420);
    ++esi;
    rc::st32(0xbbf434, esi);
    if (esi != 0)
        return eax;
    for (x86::reg32 word = 0xbbf430; word >= 0xbbf424; word -= 4)
    {
        const x86::reg32 next = rc::ld32(word) + 1;
        rc::st32(word, next);
        if (next != 0)
            return eax;
    }
    ++eax;
    rc::st32(0xbbf420, eax);
    return eax;
}

// 0x89e28d: signed 16-bit sine of a 10-bit phase.
x86::reg32 sine(x86::reg32 phase)
{
    const x86::reg32 quadrant = (phase >> 8) & 3;
    const x86::reg32 index = phase & 0xff;
    const x86::reg32 value = rc::ld16(quadrant & 1 ? SineBackward - index * 2 : SineForward + index * 2);
    return quadrant & 2 ? 0u - value : value;
}

// 0x897ba8: takes the word at +0x10, clearing it.
x86::reg32 take10(const x86::CPU&, x86::reg32 p)
{
    const x86::reg32 value = rc::ld32(p + 0x10);
    rc::st32(p + 0x10, 0);
    return value;
}

// 0x897bb7: the word at +0x14.
x86::reg32 get14(const x86::CPU&, x86::reg32 p)
{
    return rc::ld32(p + 0x14);
}

// 0x897e88: +0x14 when inside [+0, +4]; else the counter +8 steps by the signed byte +0xc while +0x10 > 0,
// wrapping past either end.
x86::reg32 stepper(const x86::CPU&, x86::reg32 p)
{
    const x86::reg32 value = rc::ld32(p + 0x14);
    const x86::reg32 lo = rc::ld32(p);
    if (s32(value) >= s32(lo) && s32(value) <= s32(rc::ld32(p + 4)))
        return value;
    if (s32(rc::ld32(p + 0x10)) > 0)
    {
        rc::st32(p + 8, rc::ld32(p + 8) + x86::reg32(x86::sreg32(x86::sreg8(rc::ld8(p + 0xc)))));
        const x86::reg32 current = rc::ld32(p + 8);
        const x86::reg32 hi = rc::ld32(p + 4);
        if (s32(current) > s32(hi))
            rc::st32(p + 8, lo);
        else if (s32(current) < s32(lo))
            rc::st32(p + 8, hi);
    }
    return rc::ld32(p + 8);
}

// 0x897eca: +8, or with +0xc set a new +8 = +0 + random % +4.
x86::reg32 randomPick(const x86::CPU&, x86::reg32 p)
{
    if (rc::ld32(p + 0xc) == 0)
        return rc::ld32(p + 8);
    const x86::reg32 r = random();
    const x86::reg32 value = rc::ld32(p) + rc::divideUnsigned(0, r, rc::ld32(p + 4)).remainder;
    rc::st32(p + 8, value);
    return value;
}

// 0x897fda: 1 once when +0x14 enters [+0, +4]; leaving [+8, +0xc] rearms.
x86::reg32 trigger(const x86::CPU&, x86::reg32 p)
{
    const x86::reg32 value = rc::ld32(p + 0x14);
    if (s32(value) >= s32(rc::ld32(p)) && s32(value) <= s32(rc::ld32(p + 4)))
    {
        if (rc::ld8(p + 0x10) == 0)
        {
            rc::st8(p + 0x10, 1);
            rc::st8(p + 0x11, 1);
            return 1;
        }
    }
    else if (s32(value) >= s32(rc::ld32(p + 8)) && s32(value) <= s32(rc::ld32(p + 0xc)))
    {
        rc::st8(p + 0x10, 0);
    }
    rc::st8(p + 0x11, 0);
    return 0;
}

// 0x898014: timer float +0 counts up by the tick period to the integer +0xc, then fires once (1, timer -1).
x86::reg32 timer(const x86::CPU&, x86::reg32 p)
{
    if (rc::ld32(p + 8) != 0)
        rc::stf32(p, 0.0f);
    if (!(double(rc::ldf32(p)) >= 0.0))
    {
        rc::st8(p + 4, 0);
        return 0;
    }
    if (fild(p + 0xc) <= double(rc::ldf32(p)))
    {
        rc::st32(p, 0xbf800000u);
        rc::st8(p + 4, 1);
        return 1;
    }
    rc::stf32(p, float(double(rc::ldf32(TickPeriod)) + double(rc::ldf32(p))));
    rc::st8(p + 4, 0);
    return 0;
}

// 0x89805f: +4 = the value paired with the first set flag of a list (count byte +2, flags at +word0).
x86::reg32 firstSet(const x86::CPU&, x86::reg32 p)
{
    const x86::reg32 flags = p + rc::ld16(p);
    for (x86::reg32 i = 0; s32(i) < s32(rc::ld8(p + 2)); ++i)
    {
        if (rc::ld32(flags + i * 4) != 0)
        {
            rc::st32(p + 4, rc::ld32(p + i * 4 + 8));
            break;
        }
    }
    return rc::ld32(p + 4);
}

// 0x898090: 1 if any of the count byte's words from +4 is set.
x86::reg32 anySet(const x86::CPU&, x86::reg32 p)
{
    const x86::reg32 count = rc::ld8(p);
    for (x86::reg32 i = 0; i < count; ++i)
    {
        if (rc::ld32(p + 4 + i * 4) != 0)
            return 1;
    }
    return 0;
}

// 0x89819f: table lookup of the input +0xc when it changed: the table (+0) holds bytes, words or dwords for
// [lo, hi], scaled by its float and linearly interpolated unless the scale is 1; the result is kept at +8.
x86::reg32 table(const x86::CPU& cpu, x86::reg32 p)
{
    const x86::reg32 input = rc::ld32(p + 0xc);
    if (input == rc::ld32(p + 4))
        return rc::ld32(p + 8);
    const x86::reg32 t = rc::ld32(p);
    rc::st32(p + 4, input);
    const x86::reg32 lo = rc::ld32(t + 4);
    const x86::reg32 index = clamp(input, lo, rc::ld32(t + 8)) - lo;
    const x86::reg8 type = rc::ld8(t);
    x86::reg32 result;
    if (double(rc::ldf32(t + 0xc)) == 1.0)
    {
        if (type == 2)
            result = x86::reg32(x86::sreg32(x86::sreg16(rc::ld16(t + index * 2 + 0x10))));
        else if (type == 1)
            result = x86::reg32(x86::sreg32(x86::sreg8(rc::ld8(t + index + 0x10))));
        else
            result = rc::ld32(t + index * 4 + 0x10);
    }
    else
    {
        const double scaled = double(s32(index)) * double(rc::ldf32(t + 0xc));
        const float position = float(scaled);
        const x86::reg32 i = fist(cpu, double(float(scaled - 0.5)));
        const double fraction = double(position) - double(s32(i));
        x86::reg32 j = i + 1;
        const x86::reg32 count = rc::ld16(t + 2);
        if (!(s32(j) < s32(count)))
            j = count - 1;
        double a, b;
        if (type == 2)
        {
            a = double(x86::sreg16(rc::ld16(t + i * 2 + 0x10)));
            b = double(x86::sreg16(rc::ld16(t + j * 2 + 0x10)));
        }
        else if (type == 1)
        {
            a = double(x86::sreg8(rc::ld8(t + i + 0x10)));
            b = double(x86::sreg8(rc::ld8(t + j + 0x10)));
        }
        else
        {
            a = fild(t + i * 4 + 0x10);
            b = fild(t + j * 4 + 0x10);
        }
        double value = b - a;
        value *= fraction;
        value += a;
        result = fist(cpu, double(float(value)));
    }
    rc::st32(p + 8, result);
    return result;
}

// 0x89832d: the n-th word from +8 for n = +4 in [1, count byte].
x86::reg32 select(const x86::CPU&, x86::reg32 p)
{
    const x86::reg32 n = rc::ld32(p + 4);
    if (s32(n) > 0 && s32(n) <= s32(rc::ld8(p)))
        return rc::ld32(p + n * 4 + 4);
    return 0;
}

// 0x89834b: routes the value +8 to output slot n = +4 (clearing the previous slot, index word +2); returns +0xc.
x86::reg32 route(const x86::CPU&, x86::reg32 p)
{
    const x86::reg32 previous = x86::reg32(x86::sreg32(x86::sreg16(rc::ld16(p + 2))));
    rc::st32(p + previous * 4 + 8, 0);
    const x86::reg32 n = rc::ld32(p + 4);
    if (s32(n) > 0 && s32(n) <= s32(rc::ld8(p)))
    {
        rc::st32(p + n * 4 + 8, rc::ld32(p + 8));
        rc::st16(p + 2, x86::reg16(n));
    }
    return rc::ld32(p + 0xc);
}

// 0x8983b8: signed maximum of the count byte's words from +4.
x86::reg32 maximum(const x86::CPU&, x86::reg32 p)
{
    x86::reg32 value = rc::ld32(p + 4);
    const x86::reg32 count = rc::ld8(p);
    for (x86::reg32 i = 1; i < count; ++i)
    {
        const x86::reg32 v = rc::ld32(p + 4 + i * 4);
        if (s32(v) > s32(value))
            value = v;
    }
    return value;
}

// 0x8983f4: product of the count byte's integers from +8, times the float +4, rounded through a float.
x86::reg32 product(const x86::CPU& cpu, x86::reg32 p)
{
    const x86::reg32 count = rc::ld8(p);
    double value = fild(p + 8);
    for (x86::reg32 i = 1; i < count; ++i)
        value *= fild(p + 8 + i * 4);
    value *= double(rc::ldf32(p + 4));
    return fist(cpu, double(float(value)));
}

// 0x898423: +8 times +4 times the float +0, rounded through a float.
x86::reg32 scale(const x86::CPU& cpu, x86::reg32 p)
{
    double value = fild(p + 8) * fild(p + 4);
    value *= double(rc::ldf32(p));
    return fist(cpu, double(float(value)));
}

// 0x898440: sum of the count byte's words from +4.
x86::reg32 sum(const x86::CPU&, x86::reg32 p)
{
    x86::reg32 value = rc::ld32(p + 4);
    const x86::reg32 count = rc::ld8(p);
    for (x86::reg32 i = 1; i < count; ++i)
        value += rc::ld32(p + 4 + i * 4);
    return value;
}

// 0x898487: +0 / +4 (0 for a zero divisor).
x86::reg32 quotient(const x86::CPU&, x86::reg32 p)
{
    const x86::reg32 divisor = rc::ld32(p + 4);
    if (divisor == 0)
        return 0;
    const x86::reg32 dividend = rc::ld32(p);
    return rc::divideSigned(x86::reg32(s32(dividend) >> 31), dividend, divisor).quotient;
}

// 0x89849f: +0 % +4 (0 for a zero divisor).
x86::reg32 remainder(const x86::CPU&, x86::reg32 p)
{
    const x86::reg32 divisor = rc::ld32(p + 4);
    if (divisor == 0)
        return 0;
    const x86::reg32 dividend = rc::ld32(p);
    return rc::divideSigned(x86::reg32(s32(dividend) >> 31), dividend, divisor).remainder;
}

// 0x8984ec: oscillator (+0: 0 sine, 1 square, 2 saw, else triangle) of amplitude +0xc and period +8 ticks; the
// phase +4 wraps below 1 and advances by one tick.
x86::reg32 oscillator(const x86::CPU& cpu, x86::reg32 p)
{
    const x86::reg32 period = rc::ld32(p + 8);
    if (s32(period) <= 0)
        return 0;
    const float step = float(double(rc::ldf32(TickPeriod)) / double(s32(period)));
    const double amplitude = fild(p + 0xc);
    const float amplitudeFloat = float(amplitude);
    if (1.0 <= double(rc::ldf32(p + 4)))
    {
        double phase = rc::ldf32(p + 4);
        do
            phase -= 1.0;
        while (phase >= 1.0);
        rc::stf32(p + 4, float(phase));
    }
    const x86::reg8 shape = rc::ld8(p);
    double value;
    if (shape == 0)
    {
        const float angle = float(double(rc::ldf32(p + 4)) * 1024.0);
        value = double(s32(sine(fist(cpu, double(angle)))));
        value *= double(amplitudeFloat);
        value *= 0x1p-16;
    }
    else if (shape == 1)
    {
        value = double(rc::ldf32(p + 4)) >= 0.5 ? amplitude : 0.0;
    }
    else if (shape == 2)
    {
        value = amplitude * double(rc::ldf32(p + 4));
    }
    else if (double(rc::ldf32(p + 4)) < 0.5)
    {
        value = amplitude * double(rc::ldf32(p + 4));
        value += value;
    }
    else
    {
        value = 1.0 - double(rc::ldf32(p + 4));
        value *= amplitude;
        value += value;
    }
    const float output = float(value);
    rc::stf32(p + 4, float(double(step) + double(rc::ldf32(p + 4))));
    return fist(cpu, double(output));
}

// 0x8985d6: the float +0 glides to the integer target +0x18 in +0x10 ticks (rate +4, scaled by +0x14), stopping
// at the target; returns it rounded.
x86::reg32 glide(const x86::CPU& cpu, x86::reg32 p)
{
    const x86::reg32 targetWord = rc::ld32(p + 0x18);
    const double target = double(s32(targetWord));
    if (target == double(rc::ldf32(p)))
        return targetWord;
    if (targetWord != rc::ld32(p + 8) || rc::ld32(p + 0x10) != rc::ld32(p + 0xc))
    {
        const x86::reg32 ticks = rc::ld32(p + 0x10);
        rc::st32(p + 8, targetWord);
        rc::st32(p + 0xc, ticks);
        if (s32(ticks) <= 0)
        {
            rc::stf32(p, float(target));
            return targetWord;
        }
        const double distance = target - double(rc::ldf32(p));
        rc::stf32(p + 4, float(distance));
        double rate = distance / double(s32(ticks));
        rate *= double(rc::ldf32(TickPeriod));
        rate *= double(rc::f32(0x39800000u));
        rc::stf32(p + 4, float(rate));
    }
    double value = fild(p + 0x14) * double(rc::ldf32(p + 4));
    value += double(rc::ldf32(p));
    rc::stf32(p, float(value));
    const double rate = rc::ldf32(p + 4);
    bool overshoot;
    if (!(rate >= 0.0))
        overshoot = !(value >= target) != __builtin_isunordered(value, target);
    else
        overshoot = value > target;
    if (overshoot)
        rc::stf32(p, float(target));
    return fist(cpu, double(rc::ldf32(p)));
}

// 0x898675: sum of the count byte's words from +8, at most +4.
x86::reg32 sumAtMost(const x86::CPU&, x86::reg32 p)
{
    x86::reg32 value = rc::ld32(p + 8);
    const x86::reg32 count = rc::ld8(p);
    for (x86::reg32 i = 1; i < count; ++i)
        value += rc::ld32(p + 8 + i * 4);
    const x86::reg32 limit = rc::ld32(p + 4);
    return s32(value) > s32(limit) ? limit : value;
}

// 0x89869f: +4 - +8, at least +0.
x86::reg32 differenceAtLeast(const x86::CPU&, x86::reg32 p)
{
    const x86::reg32 value = rc::ld32(p + 4) - rc::ld32(p + 8);
    const x86::reg32 limit = rc::ld32(p);
    return s32(value) < s32(limit) ? limit : value;
}

// 0x8986b5: +8 * +4, at most +0.
x86::reg32 productAtMost(const x86::CPU&, x86::reg32 p)
{
    const x86::reg32 value = rc::ld32(p + 8) * rc::ld32(p + 4);
    const x86::reg32 limit = rc::ld32(p);
    return s32(value) > s32(limit) ? limit : value;
}

size_t noRegions(x86::reg32, shadow::Region*)
{
    return 0;
}

template <x86::reg32 Offset, x86::reg32 Size>
size_t fields(x86::reg32 p, shadow::Region* out)
{
    out[0] = { p + Offset, Size };
    return 1;
}

template <x86::reg32 Offset, x86::reg32 Size>
size_t fieldsAndRandom(x86::reg32 p, shadow::Region* out)
{
    out[0] = { p + Offset, Size };
    out[1] = { RandomState, RandomStateSize };
    return 2;
}

size_t routeRegions(x86::reg32 p, shadow::Region* out)
{
    out[0] = { p + 2, 2 };
    out[1] = { p + x86::reg32(x86::sreg32(x86::sreg16(rc::ld16(p + 2)))) * 4 + 8, 4 };
    const x86::reg32 n = rc::ld32(p + 4);
    if (s32(n) <= 0 || s32(n) > s32(rc::ld8(p)))
        return 2;
    out[2] = { p + n * 4 + 8, 4 };
    return 3;
}

// The native body as a guest function: argument at esp + 4, result in eax, return address popped.
template <Body F>
void asMethod(win32::WinApplication*, x86::CPU& cpu)
{
    cpu.eax = F(cpu, rc::ld32(cpu.esp + 4));
    cpu.esp += 4;
}

// The translated function G called as the routine's call instruction would: its CPU's esp at the return slot.
[[gnu::cold, gnu::noinline]] x86::reg32 translated(win32::WinApplication* app, x86::CPU& cpu, x86::reg32 args, x86::reg32 address,
                      win32::MethodPtr guest)
{
    const x86::reg32 esp = cpu.esp;
    cpu.esp = args - 4;
    cpu.ip = address;
    app->allowPreemption(cpu);
    guest(app, cpu);
    cpu.esp = esp;
    return cpu.eax;
}

template <Body F, Regions R>
[[gnu::cold, gnu::noinline]] x86::reg32 compared(const char* name, win32::WinApplication* app, x86::CPU& cpu, x86::reg32 args,
                    x86::reg32 address, win32::MethodPtr guest)
{
    if (!s_shadow.active())
        return F(cpu, rc::ld32(args));
    x86::CPU call = cpu;
    call.esp = args - 4;
    call.ip = address;
    shadow::Region regions[3];
    const size_t count = R(rc::ld32(args), regions);
    // Some of the functions store into their argument slot, which the routines never read again.
    const shadow::Region slot = { args, 4 };
    s_shadow.call(name, app, call, guest, &asMethod<F>, regions, count, LiveRegisters, nullptr, nullptr, &slot, 1);
    return call.eax;
}

template <Body F, Regions R>
inline x86::reg32 run(const char* name, win32::WinApplication* app, x86::CPU& cpu, x86::reg32 args,
                      x86::reg32 address, win32::MethodPtr guest)
{
    if (__builtin_expect(s_mode == 1, 1))
        return F(cpu, rc::ld32(args));
    if (s_mode == 0)
        return translated(app, cpu, args, address, guest);
    return compared<F, R>(name, app, cpu, args, address, guest);
}

}

// Not inlined: a routine's call sites stay one call each.
#define NFSC_BANK_LIBRARY(address, body, regions)                                                                  \
    [[gnu::noinline]] x86::reg32 lib_##address(win32::WinApplication* app, x86::CPU& cpu, x86::reg32 args)                           \
    {                                                                                                              \
        return run<&body, &regions>("0x" #address, app, cpu, args, 0x##address, &sub_##address);                  \
    }

NFSC_BANK_LIBRARY(897ba8, take10, (fields<0x10, 4>))
NFSC_BANK_LIBRARY(897bb7, get14, noRegions)
NFSC_BANK_LIBRARY(897e88, stepper, (fields<8, 4>))
NFSC_BANK_LIBRARY(897eca, randomPick, (fieldsAndRandom<8, 4>))
NFSC_BANK_LIBRARY(897fda, trigger, (fields<0x10, 2>))
NFSC_BANK_LIBRARY(898014, timer, (fields<0, 5>))
NFSC_BANK_LIBRARY(89805f, firstSet, (fields<4, 4>))
NFSC_BANK_LIBRARY(898090, anySet, noRegions)
NFSC_BANK_LIBRARY(89819f, table, (fields<4, 8>))
NFSC_BANK_LIBRARY(89832d, select, noRegions)
NFSC_BANK_LIBRARY(89834b, route, routeRegions)
NFSC_BANK_LIBRARY(8983b8, maximum, noRegions)
NFSC_BANK_LIBRARY(8983f4, product, noRegions)
NFSC_BANK_LIBRARY(898423, scale, noRegions)
NFSC_BANK_LIBRARY(898440, sum, noRegions)
NFSC_BANK_LIBRARY(898487, quotient, noRegions)
NFSC_BANK_LIBRARY(89849f, remainder, noRegions)
NFSC_BANK_LIBRARY(8984ec, oscillator, (fields<4, 4>))
NFSC_BANK_LIBRARY(8985d6, glide, (fields<0, 0x10>))
NFSC_BANK_LIBRARY(898675, sumAtMost, noRegions)
NFSC_BANK_LIBRARY(89869f, differenceAtLeast, noRegions)
NFSC_BANK_LIBRARY(8986b5, productAtMost, noRegions)

#undef NFSC_BANK_LIBRARY

void installLibrary()
{
    s_mode = pf::Config::getBool("banklib_native", true) ? (pf::Config::getBool("shadow_banklib", false) ? 2 : 1)
                                                         : 0;
}

void logShadow()
{
    s_shadow.log();
}

}

}
