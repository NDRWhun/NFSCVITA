#ifndef RECOMP_H_
#define RECOMP_H_

// Support code for the generated game code (tools/recompiler): guest memory loads and stores.

#include <x86.h>
#include <lib/memmap.h>
#include <csetjmp>
#include <cstring>

namespace win32
{
class WinApplication;
}

namespace rc
{

// Host pointer of a guest address.
inline x86::reg8* host(x86::reg32 address)
{
    return win32::g_guestBase + address;
}

// setjmp/longjmp: a setjmp call in generated code saves a host jump buffer, keyed by the guest jmp_buf, and the x87
// depth; longJump resumes there after the guest longjmp restored the registers into cpu (src/lib/jump.cpp).
jmp_buf& jumpBuffer(win32::WinApplication* app, x86::CPU& cpu, x86::reg32 guestBuffer);
[[noreturn]] void longJump(win32::WinApplication* app, x86::CPU& cpu, x86::reg32 guestBuffer);

// div/idiv r/m32: edx:eax by a 32-bit divisor.
struct Division
{
    x86::reg32 quotient;
    x86::reg32 remainder;
};

// Wide dividend with a 32-bit quotient (hi < d): a double estimate within 1 of the quotient, corrected by the
// remainder. vcvt saturates where the estimate rounds up to 2^32.
inline Division divideWide(x86::reg32 hi, x86::reg32 lo, x86::reg32 d)
{
    const double estimate = (double(hi) * 0x1p32 + double(lo)) / double(d);
    float bits;
    asm("vcvt.u32.f64 %0, %P1" : "=t"(bits) : "w"(estimate));
    x86::reg32 q;
    memcpy(&q, &bits, sizeof(q));
    const x86::reg64 n = (x86::reg64(hi) << 32) | lo;
    x86::sreg64 r = x86::sreg64(n - x86::reg64(q) * d);
    if (r < 0)
    {
        --q;
        r += d;
    }
    else if (x86::reg64(r) >= d)
    {
        ++q;
        r -= d;
    }
    return {q, x86::reg32(r)};
}

// A zero divisor or a quotient that does not fit (#DE on x86): the 64-bit libgcc division, out of line so the
// compiler does not turn a known zero divisor into a trap.
[[gnu::noinline, gnu::cold]] inline Division divideUnsignedWide(x86::reg32 hi, x86::reg32 lo, x86::reg32 d)
{
    const x86::reg64 n = (x86::reg64(hi) << 32) | lo;
    return {x86::reg32(n / d), x86::reg32(n % d)};
}

[[gnu::noinline, gnu::cold]] inline Division divideSignedWide(x86::reg32 hi, x86::reg32 lo, x86::reg32 d)
{
    const x86::sreg64 n = x86::sreg64((x86::reg64(hi) << 32) | lo);
    return {x86::reg32(n / x86::sreg32(d)), x86::reg32(n % x86::sreg32(d))};
}

// Results as the 64-bit C++ division gives them.
inline Division divideUnsigned(x86::reg32 hi, x86::reg32 lo, x86::reg32 d)
{
    if (hi == 0 && d != 0)
        return {lo / d, lo % d};
    if (hi < d)
        return divideWide(hi, lo, d);
    return divideUnsignedWide(hi, lo, d);
}

// A sign-extended dividend (cdq) takes the 32-bit division, except for the divisors 0 and -1.
inline Division divideSigned(x86::reg32 hi, x86::reg32 lo, x86::reg32 d)
{
    if (hi == x86::reg32(x86::sreg32(lo) >> 31) && d + 1u > 1u)
        return {x86::reg32(x86::sreg32(lo) / x86::sreg32(d)), x86::reg32(x86::sreg32(lo) % x86::sreg32(d))};
    return divideSignedWide(hi, lo, d);
}

#ifdef NFSC_CENSUS_ACCESS
// Census build (src/nfsc/census.cpp): generated code marks each 1 KB guest line it reads in g_censusReads and writes
// in g_censusWrites while they are set, on the host thread whose marker (censusThread) is g_censusReader (0: any)
// and g_censusWriter.
extern x86::reg8* g_censusReads;
extern x86::reg8* g_censusWrites;
extern uint32_t g_censusReader;
extern uint32_t g_censusWriter;

// The host thread's user thread register.
inline uint32_t censusThread()
{
    uint32_t value;
    __asm__("mrc p15, 0, %0, c13, c0, 3" : "=r"(value));
    return value;
}

// True when a census mode marks accesses; fixed before guest code runs, so a function reads it once. Each access then
// costs a test and an out-of-line call that checks the maps and the thread.
bool censusMarking() __attribute__((const));
void censusMarkRead(x86::reg32 address) __attribute__((cold));
void censusMarkWrite(x86::reg32 address) __attribute__((cold));

inline void censusRead(x86::reg32 address)
{
    if (__builtin_expect(censusMarking(), 0))
        censusMarkRead(address);
}

inline void censusWrite(x86::reg32 address)
{
    if (__builtin_expect(censusMarking(), 0))
        censusMarkWrite(address);
}
#else
inline void censusRead(x86::reg32) {}
inline void censusWrite(x86::reg32) {}
#endif

// A float or double of the given bits (constants of read-only guest data).
constexpr float f32(x86::reg32 bits)    { return __builtin_bit_cast(float, bits); }
constexpr double f64(x86::reg64 bits)   { return __builtin_bit_cast(double, bits); }

template< typename T >
inline T load(x86::reg32 address)
{
    censusRead(address);
    T value;
    memcpy(&value, win32::g_guestBase + address, sizeof(T));
    return value;
}

template< typename T >
inline void store(x86::reg32 address, T value)
{
    censusWrite(address);
    memcpy(win32::g_guestBase + address, &value, sizeof(T));
}

inline x86::reg8 ld8(x86::reg32 address)    { return load<x86::reg8>(address); }
inline x86::reg16 ld16(x86::reg32 address)  { return load<x86::reg16>(address); }
inline x86::reg32 ld32(x86::reg32 address)  { return load<x86::reg32>(address); }
inline x86::reg64 ld64(x86::reg32 address)  { return load<x86::reg64>(address); }
inline float ldf32(x86::reg32 address)      { return load<float>(address); }
inline double ldf64(x86::reg32 address)     { return load<double>(address); }

inline void st8(x86::reg32 address, x86::reg8 value)    { store(address, value); }
inline void st16(x86::reg32 address, x86::reg16 value)  { store(address, value); }
inline void st32(x86::reg32 address, x86::reg32 value)  { store(address, value); }
inline void st64(x86::reg32 address, x86::reg64 value)  { store(address, value); }
inline void stf32(x86::reg32 address, float value)      { store(address, value); }
inline void stf64(x86::reg32 address, double value)     { store(address, value); }

// Floats at 4-byte aligned guest addresses (guest memory starts on a page): the VFP loads and stores them
// directly, without the core register a possibly unaligned access needs.
typedef float AlignedFloat __attribute__((aligned(4), may_alias));

inline float ldf32a(x86::reg32 address)
{
    censusRead(address);
    return *reinterpret_cast<const AlignedFloat*>(win32::g_guestBase + address);
}

inline void stf32a(x86::reg32 address, float value)
{
    censusWrite(address);
    *reinterpret_cast<AlignedFloat*>(win32::g_guestBase + address) = value;
}

// Serialises locked instructions on misaligned addresses, which host atomics cannot access (src/lib/cpu.cpp).
void lockMisaligned();
void unlockMisaligned();

// Locked instructions (lock prefix, xchg with memory): sequentially consistent host atomics, full barriers as on x86.
// Each returns the old value.
template< typename T >
inline T lockedExchange(x86::reg32 address, T value)
{
    censusRead(address);
    if (__builtin_expect((address & (sizeof(T) - 1)) == 0, 1))
        return __atomic_exchange_n(reinterpret_cast<T*>(host(address)), value, __ATOMIC_SEQ_CST);
    lockMisaligned();
    const T old = load<T>(address);
    store(address, value);
    unlockMisaligned();
    return old;
}

template< typename T >
inline T lockedAdd(x86::reg32 address, T value)
{
    censusRead(address);
    if (__builtin_expect((address & (sizeof(T) - 1)) == 0, 1))
        return __atomic_fetch_add(reinterpret_cast<T*>(host(address)), value, __ATOMIC_SEQ_CST);
    lockMisaligned();
    const T old = load<T>(address);
    store(address, T(old + value));
    unlockMisaligned();
    return old;
}

template< typename T >
inline T lockedCompareExchange(x86::reg32 address, T expected, T desired)
{
    censusRead(address);
    if (__builtin_expect((address & (sizeof(T) - 1)) == 0, 1))
    {
        __atomic_compare_exchange_n(reinterpret_cast<T*>(host(address)), &expected, desired, false, __ATOMIC_SEQ_CST,
                                    __ATOMIC_SEQ_CST);
        return expected;
    }
    lockMisaligned();
    const T old = load<T>(address);
    if (old == expected)
        store(address, desired);
    unlockMisaligned();
    return old;
}

}

#endif /* !RECOMP_H_ */
