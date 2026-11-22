#ifndef SSE_H_
#define SSE_H_

// SSE registers and the single-precision operations the recompiler emits for them, with the results of an x86 CPU
// running with the default MXCSR (round to nearest, no flush to zero): a NaN operand is returned quiet (the first
// one if both are NaN), an invalid operation gives the indefinite 0xffc00000, conversions out of range give the
// integer indefinite 0x80000000.

#include <x86.h>
#include <cmath>
#include <cstring>

namespace x86
{

union alignas(16) Xmm
{
    float f[4];
    reg32 u[4];
    reg64 q[2];
};

namespace sse
{

inline float fromBits(reg32 bits)
{
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

inline reg32 toBits(float f)
{
    reg32 bits;
    memcpy(&bits, &f, 4);
    return bits;
}

inline Xmm load128(const void* p)
{
    Xmm x;
    memcpy(&x, p, 16);
    return x;
}

inline void store128(void* p, const Xmm& x)
{
    memcpy(p, &x, 16);
}

// The NaN result of an operation whose result r is NaN.
__attribute__((cold, noinline)) inline float nanResult(float a, float b)
{
    if (a != a)
        return fromBits(toBits(a) | 0x00400000u);
    if (b != b)
        return fromBits(toBits(b) | 0x00400000u);
    return fromBits(0xffc00000u);
}

inline float checked(float r, float a, float b)
{
    return __builtin_expect(r != r, 0) ? nanResult(a, b) : r;
}

inline float add(float a, float b) { return checked(a + b, a, b); }
inline float sub(float a, float b) { return checked(a - b, a, b); }
inline float mul(float a, float b) { return checked(a * b, a, b); }
inline float div(float a, float b) { return checked(a / b, a, b); }

inline float sqrt(float a)
{
    if (a != a)
        return fromBits(toBits(a) | 0x00400000u);
    if (a < 0.0f)
        return fromBits(0xffc00000u);
    return std::sqrt(a);
}

// minps/maxps: the second operand when either is NaN or both are zeros.
inline float min(float a, float b) { return a < b ? a : b; }
inline float max(float a, float b) { return a > b ? a : b; }

// cvtss2si / cvtps2pi (round to nearest even) and cvttss2si / cvttps2pi (truncate).
inline reg32 toInt(float a)
{
    if (!(a >= -2147483648.0f && a < 2147483648.0f))
        return 0x80000000u;
    return reg32(sreg32(std::nearbyint(a)));
}

inline reg32 toIntTruncate(float a)
{
    if (!(a > -2147483904.0f && a < 2147483648.0f))
        return 0x80000000u;
    return reg32(sreg32(a));
}

// cmpps predicates 0-7: eq, lt, le, unord, neq, nlt, nle, ord.
inline reg32 compare(float a, float b, int predicate)
{
    bool r;
    switch (predicate & 7)
    {
    case 0:  r = a == b; break;
    case 1:  r = a < b; break;
    case 2:  r = a <= b; break;
    case 3:  r = a != a || b != b; break;
    case 4:  r = !(a == b); break;
    case 5:  r = !(a < b); break;
    case 6:  r = !(a <= b); break;
    default: r = a == a && b == b; break;
    }
    return r ? 0xffffffffu : 0u;
}

}

}

#endif /* !SSE_H_ */
