// Native replacements of hot game functions: the same double-precision steps and float stores as the translated x87
// code, the same register results. hot_native=0 runs the translated bodies instead.

#include "hot.h"

#include <native.h>
#include <platform/config.h>
#include <recomp.h>

#include <cmath>
#include <cstring>

namespace nfsc
{

// Translated bodies of the overridden guest functions.
void sub_70f0b0_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_71b630_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_46fb20_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_46fcc0_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_7e9a60_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_605910_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_7384c0_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_46cad0_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_558360_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_7e7c30_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_79f9c0_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_699420_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_7ec920_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_401710_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_71a350_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_7381a0_guest(win32::WinApplication* app, x86::CPU& cpu);

namespace
{

int s_native = -1;

bool useNative()
{
    if (s_native < 0)
        s_native = pf::Config::getBool("hot_native", true) ? 1 : 0;
    return s_native != 0;
}

x86::reg32 arg(const x86::CPU& cpu, int index)
{
    return rc::ld32(cpu.esp + 4 + 4 * index);
}

float argFloat(const x86::CPU& cpu, int index)
{
    return rc::ldf32(cpu.esp + 4 + 4 * index);
}

// PF after an instruction with this 32-bit result: even parity of its low byte.
bool parityFlag(x86::reg32 value)
{
    return !x86::parity8(x86::reg8(value));
}

// rep movsd then rep movsb of count bytes, forward: memmove unless the destination starts inside the source.
void copyForward(x86::reg32 to, x86::reg32 from, x86::reg32 count)
{
    if (to <= from || to - from >= count)
    {
        memmove(rc::host(to), rc::host(from), count);
        return;
    }
    x86::reg32 i = 0;
    for (; i + 4 <= count; i += 4)
        rc::st32(to + i, rc::ld32(from + i));
    for (; i < count; ++i)
        rc::st8(to + i, rc::ld8(from + i));
}

// Registers the chunk splitter leaves for its caller, as the translated code tracks them.
struct SplitState
{
    x86::reg32 ecx;
    x86::reg32 edx;
    bool pf;
};

// SplitPermTempChunks: copies the chunks of [chunk, chunk + size) whose temporary flag equals flag's low byte to
// out + offset (each aligned to 128 bytes after a padding chunk), descending into container chunks; returns the
// new offset. slots: the guest esp of the outermost call, whose argument slots the guest reuses (0 below it).
x86::reg32 splitChunks(x86::reg32 flag, x86::reg32 chunk, x86::reg32 size, x86::reg32 out, x86::reg32 offset,
                       x86::reg32 depth, x86::reg32 slots, SplitState& s)
{
    constexpr x86::reg32 TempMask = 0xa62b74;
    ++depth;
    if (slots)
        rc::st32(slots + 0x18, depth);
    // The guest's nesting check: a write to address 0.
    if (x86::sreg32(depth) > 10)
        rc::st32(0, 0);
    const x86::reg32 end = chunk + size;
    if (slots)
        rc::st32(slots + 8, end);
    if (chunk >= end)
        return offset;
    const x86::reg8 want = x86::reg8(flag);
    x86::reg32 at = offset;
    x86::reg32 c = chunk;
    do
    {
        const x86::reg8 temp = x86::reg8((rc::ld32(c) == 0xb3320000u ? 0xffu : 0u) & rc::ld32(TempMask));
        s.pf = parityFlag(temp);
        if (slots)
            rc::st8(slots + 0x14, temp);
        if (temp == want)
        {
            x86::reg32 pad = (c - at) & 0x7f;
            if (pad != 0 && x86::sreg32(pad) <= 8)
                pad += 0x80;
            s.pf = parityFlag(out);
            if (out)
            {
                s.pf = parityFlag(pad);
                if (pad)
                {
                    rc::st32(out + at + 4, pad - 8);
                    rc::st32(out + at, 0);
                    memset(rc::host(out + at + 8), 0x11, pad - 8);
                    s.edx = pad - 8;
                    s.pf = parityFlag((pad - 8) & 3);
                }
            }
            at += pad;
        }
        bool descend = false;
        if (x86::sreg32(rc::ld32(c)) < 0)
        {
            s.pf = parityFlag(temp);
            descend = temp == 0;
        }
        if (descend)
        {
            s.edx = depth;
            const x86::reg32 inner = splitChunks(flag, c + 8, rc::ld32(c + 4), out, at + 8, depth, 0, s);
            if (inner != at + 8)
            {
                s.pf = parityFlag(out);
                if (out)
                {
                    rc::st32(out + at, rc::ld32(c));
                    s.edx = inner - at - 8;
                    rc::st32(out + at + 4, s.edx);
                }
                at = inner;
            }
        }
        else if (temp == want)
        {
            s.pf = parityFlag(out);
            if (out)
            {
                const x86::reg32 count = rc::ld32(c + 4) + 8;
                s.edx = count;
                copyForward(out + at, c, count);
                s.pf = parityFlag(count & 3);
            }
            at = rc::ld32(c + 4) + at + 8;
        }
        s.ecx = rc::ld32(c + 4);
        c = c + s.ecx + 8;
    } while (c < end);
    return at;
}

}

// Per axis, the smaller of each product pair (one product rounded to float) goes to a, the larger to b.
void transformBox(x86::reg32 m, float a[3], float b[3], double& lastA, double& lastB)
{
    double lo[3], hi[3];
    for (int j = 0; j < 3; ++j)
        lo[j] = hi[j] = rc::ldf32(m + 0x30 + 4 * j);
    for (int i = 0; i < 3; ++i)
    {
        for (int j = 0; j < 3; ++j)
        {
            const double mij = rc::ldf32(m + 16 * i + 4 * j);
            const double p = double(a[i]) * mij;
            const float q = float(double(b[i]) * mij);
            if (p < double(q))
            {
                lo[j] += p;
                hi[j] += q;
            }
            else
            {
                lo[j] += q;
                hi[j] += p;
            }
            lastA = p;
            lastB = q;
        }
    }
    for (int j = 0; j < 3; ++j)
    {
        a[j] = float(lo[j]);
        b[j] = float(hi[j]);
    }
}

int viewBoxTest(x86::reg32 view, x86::reg32 pa, x86::reg32 pb, x86::reg32 m, ViewBoxDetail* detail)
{
    float a[3], b[3];
    for (int k = 0; k < 3; ++k)
    {
        a[k] = rc::ldf32(pa + 4 * k);
        b[k] = rc::ldf32(pb + 4 * k);
    }
    ViewBoxDetail local;
    ViewBoxDetail& out = detail ? *detail : local;
    out.transformed = m != 0;
    if (m)
        transformBox(m, a, b, out.lastA, out.lastB);
    float c[3], e[3];
    for (int k = 0; k < 3; ++k)
        c[k] = float((double(b[k]) + double(a[k])) * 0.5);
    for (int k = 0; k < 3; ++k)
        e[k] = float(double(b[k]) - double(c[k]));
    const x86::reg32 planes = rc::ld32(view) + 0x140;
    bool crossing = false;
    for (int k = 0; k < 6; ++k)
    {
        const x86::reg32 p = planes + 16 * k;
        const double px = rc::ldf32(p), py = rc::ldf32(p + 4), pz = rc::ldf32(p + 8), pw = rc::ldf32(p + 12);
        double r = std::fabs(py) * double(e[1]) + std::fabs(pz) * double(e[2]);
        r += std::fabs(px) * double(e[0]);
        double d = double(c[1]) * py + double(c[2]) * pz;
        d += double(c[0]) * px;
        d += pw;
        const double front = d + r;
        if (front < 0.0)
        {
            out.compare = front;
            out.plane = k + 1;
            return 0;
        }
        const double back = d - r;
        if (back < 0.0)
            crossing = true;
        if (k == 5)
            out.compare = back;
    }
    out.plane = 7;
    return crossing ? 1 : 2;
}

namespace native
{

// Box transform (cdecl: matrix, a, b), 0x70f0b0.
void sub_70f0b0(win32::WinApplication* app, x86::CPU& cpu)
{
    if (!useNative())
    {
        sub_70f0b0_guest(app, cpu);
        return;
    }
    const x86::reg32 m = arg(cpu, 0);
    const x86::reg32 pa = arg(cpu, 1);
    const x86::reg32 pb = arg(cpu, 2);
    float a[3], b[3];
    for (int k = 0; k < 3; ++k)
    {
        a[k] = rc::ldf32(pa + 4 * k);
        b[k] = rc::ldf32(pb + 4 * k);
    }
    double lastA, lastB;
    transformBox(m, a, b, lastA, lastB);
    // The guest keeps its float product in the matrix argument slot; the status word goes through ax.
    rc::stf32(cpu.esp + 4, float(lastB));
    for (int k = 0; k < 3; ++k)
        rc::stf32(pa + 4 * k, a[k]);
    for (int k = 0; k < 3; ++k)
        rc::stf32(pb + 4 * k, b[k]);
    cpu.fpu.compare(lastA, lastB);
    cpu.eax = (m & 0xffff0000u) | cpu.fpu.status.word;
    cpu.flags.pf = !(lastA < lastB);
    cpu.ecx = pa + 12;
    cpu.esp += 4;
}

// View frustum test of a box (thiscall: view, a, b, matrix or 0), 0x71b630: 0 outside one of the six planes at
// [view]+0x140, 2 inside all, 1 otherwise. The box corners a, b are transformed first when the matrix is given.
void sub_71b630(win32::WinApplication* app, x86::CPU& cpu)
{
    if (!useNative())
    {
        sub_71b630_guest(app, cpu);
        return;
    }
    ViewBoxDetail detail;
    const int result = viewBoxTest(cpu.ecx, arg(cpu, 0), arg(cpu, 1), arg(cpu, 2), &detail);
    if (detail.transformed)
        cpu.flags.pf = !(detail.lastA < detail.lastB);
    cpu.fpu.compare(detail.compare, 0.0);
    const x86::reg32 planes = rc::ld32(cpu.ecx) + 0x140;
    cpu.eax = x86::reg32(result);
    cpu.ecx = planes + 4 + 16 * x86::reg32(detail.plane < 7 ? detail.plane - 1 : 6);
    cpu.edx = x86::reg32(detail.plane);
    cpu.esp += 16;
}

// Grows the 2D box min..max to hold p (cdecl: min, max, p), 0x46fb20.
void sub_46fb20(win32::WinApplication* app, x86::CPU& cpu)
{
    if (!useNative())
    {
        sub_46fb20_guest(app, cpu);
        return;
    }
    const x86::reg32 lo = arg(cpu, 0);
    const x86::reg32 hi = arg(cpu, 1);
    const x86::reg32 p = arg(cpu, 2);
    const double px = rc::ldf32(p), py = rc::ldf32(p + 4);
    const float lx = rc::ldf32(lo), ly = rc::ldf32(lo + 4);
    const float hx = rc::ldf32(hi), hy = rc::ldf32(hi + 4);
    const float nlx = double(lx) < px ? lx : float(px);
    const float nly = double(ly) < py ? ly : float(py);
    const float nhx = double(hx) > px ? hx : float(px);
    const float nhy = double(hy) > py ? hy : float(py);
    // The guest keeps its last corner value in the p argument slot.
    rc::stf32(cpu.esp + 12, hy);
    rc::stf32(lo, nlx);
    rc::stf32(lo + 4, nly);
    rc::stf32(hi, nhx);
    rc::stf32(hi + 4, nhy);
    cpu.fpu.compare(hy, py);
    cpu.flags.pf = !(double(hy) <= py);
    cpu.esp += 4;
}

// True when p is inside the 2D box min..max grown by r (cdecl: min, max, p, r), 0x46fcc0.
void sub_46fcc0(win32::WinApplication* app, x86::CPU& cpu)
{
    if (!useNative())
    {
        sub_46fcc0_guest(app, cpu);
        return;
    }
    const x86::reg32 lo = arg(cpu, 0);
    const x86::reg32 hi = arg(cpu, 1);
    const x86::reg32 p = arg(cpu, 2);
    const double r = argFloat(cpu, 3);
    cpu.ecx = p;
    cpu.eax = 0;
    cpu.esp += 4;
    double a = double(rc::ldf32(lo)) - r;
    double b = rc::ldf32(p);
    cpu.fpu.compare(a, b);
    if (a > b)
        return;
    cpu.edx = hi;
    a = r + double(rc::ldf32(hi));
    cpu.fpu.compare(a, b);
    if (a < b)
        return;
    a = double(rc::ldf32(lo + 4)) - r;
    b = rc::ldf32(p + 4);
    cpu.fpu.compare(a, b);
    if (a > b)
        return;
    a = r + double(rc::ldf32(hi + 4));
    cpu.fpu.compare(a, b);
    if (a < b)
        return;
    cpu.eax = 1;
}

// 2D bounds (x, z) of three points (thiscall: out, a, b, c), 0x7e9a60: out = min x, min z, max x, max z; returns out.
void sub_7e9a60(win32::WinApplication* app, x86::CPU& cpu)
{
    if (!useNative())
    {
        sub_7e9a60_guest(app, cpu);
        return;
    }
    const x86::reg32 out = cpu.ecx;
    const x86::reg32 p0 = arg(cpu, 0), p1 = arg(cpu, 1), p2 = arg(cpu, 2);
    float v = 0;
    double lastA = 0, lastB = 0, prevA = 0, prevB = 0;
    // Smallest or largest of c, b, a at offset, in the guest's comparison order.
    auto pick = [&](x86::reg32 offset, bool largest) {
        float m = rc::ldf32(p2 + offset);
        const x86::reg32 points[2] = { p1, p0 };
        for (const x86::reg32 p : points)
        {
            v = rc::ldf32(p + offset);
            prevA = lastA;
            prevB = lastB;
            lastA = v;
            lastB = m;
            if (largest ? double(v) > double(m) : double(v) < double(m))
                m = v;
        }
        return m;
    };
    rc::stf32(out, pick(0, false));
    rc::stf32(out + 4, pick(8, false));
    rc::stf32(out + 8, pick(0, true));
    rc::stf32(out + 12, pick(8, true));
    // The guest keeps its last point value in the c argument slot.
    rc::stf32(cpu.esp + 12, v);
    // The translated code keeps the status word of the second-last compare.
    cpu.fpu.compare(prevA, prevB);
    cpu.eax = out;
    cpu.esp += 16;
}

// VU0_MATRIX4_mult: 4x4 matrix product out = a * b (cdecl: a, b, out), 0x605910.
void sub_605910(win32::WinApplication* app, x86::CPU& cpu)
{
    if (!useNative())
    {
        sub_605910_guest(app, cpu);
        return;
    }
    const x86::reg32 pa = arg(cpu, 0);
    const x86::reg32 pb = arg(cpu, 1);
    const x86::reg32 out = arg(cpu, 2);
    double b[16];
    for (int k = 0; k < 16; ++k)
        b[k] = rc::ldf32(pb + 4 * k);
    float r[16];
    for (int i = 0; i < 4; ++i)
    {
        const x86::reg32 row = pa + 16 * i;
        const double a0 = rc::ldf32(row), a1 = rc::ldf32(row + 4), a2 = rc::ldf32(row + 8), a3 = rc::ldf32(row + 12);
        r[4 * i] = float(((b[12] * a3 + b[4] * a1) + b[8] * a2) + a0 * b[0]);
        r[4 * i + 1] = float(((a0 * b[1] + a3 * b[13]) + b[9] * a2) + a1 * b[5]);
        r[4 * i + 2] = float(((b[2] * a0 + a3 * b[14]) + b[10] * a2) + a1 * b[6]);
        r[4 * i + 3] = float(((b[15] * a3 + b[7] * a1) + b[11] * a2) + a0 * b[3]);
    }
    for (int k = 0; k < 16; ++k)
        rc::stf32(out + 4 * k, r[k]);
    cpu.eax = pa + 0x48;
    cpu.ecx = 0;
    cpu.edx = cpu.esp + 8;
    cpu.esp += 4;
}

// Irradiance::UpdateCoeffs (thiscall: coefficients, colour, scale, x, y, z), 0x7384c0: adds the colour times scale,
// weighted by the spherical harmonics basis of the direction, to the ten four-channel coefficients.
void sub_7384c0(win32::WinApplication* app, x86::CPU& cpu)
{
    if (!useNative())
    {
        sub_7384c0_guest(app, cpu);
        return;
    }
    const x86::reg32 colour = arg(cpu, 0);
    const double s = argFloat(cpu, 1);
    const double x = argFloat(cpu, 2), y = argFloat(cpu, 3), z = argFloat(cpu, 4);
    float c[4];
    for (int j = 0; j < 4; ++j)
        c[j] = rc::ldf32(colour + 4 * j);
    // Basis constants (read-only data) and per-band weights (game data).
    double k[11], g[10];
    for (int i = 0; i < 11; ++i)
        k[i] = rc::ldf32(0x9ea954 + 4 * i);
    for (int i = 0; i < 10; ++i)
        g[i] = rc::ldf32(0xa6b928 + 4 * i);
    const double zz = (z * z) * double(rc::ldf32(0x9ca2c4)) - double(rc::ldf32(0x9c2478));
    const double xy = x * x - y * y;
    for (int j = 0; j < 4; ++j)
    {
        const x86::reg32 e = cpu.ecx + 4 * j;
        const double t = s * double(c[j]);
        const double u = t * k[9];
        const float l14 = float(y * u), l18 = float(z * u), l1c = float(u * x);
        const double v = t * k[8];
        const double vx = x * v;
        const float l20 = float(y * vx);
        const float l24 = float((v * y) * z);
        const float l2c = float(vx * z);
        const float l28 = float((t * k[7]) * zz);
        const float l30 = float((t * k[6]) * xy);
        rc::stf32(e, float((((k[10] * t) * k[5]) - double(l28) * k[4]) * g[0] + double(rc::ldf32(e))));
        const double w = double(l30) * k[3];
        rc::stf32(e + 0x10, float(g[1] * w + double(rc::ldf32(e + 0x10))));
        rc::stf32(e + 0x20, float(double(rc::ldf32(e + 0x20)) - w * g[2]));
        rc::stf32(e + 0x30, float((double(l28) * k[2]) * g[3] + double(rc::ldf32(e + 0x30))));
        rc::stf32(e + 0x40, float((double(l20) * k[1]) * g[4] + double(rc::ldf32(e + 0x40))));
        rc::stf32(e + 0x50, float((double(l2c) * k[1]) * g[5] + double(rc::ldf32(e + 0x50))));
        rc::stf32(e + 0x60, float((double(l24) * k[3]) * g[6] + double(rc::ldf32(e + 0x60))));
        rc::stf32(e + 0x70, float((double(l1c) * k[0]) * g[7] + double(rc::ldf32(e + 0x70))));
        rc::stf32(e + 0x80, float((double(l14) * k[0]) * g[8] + double(rc::ldf32(e + 0x80))));
        rc::stf32(e + 0x90, float((double(l18) * k[0]) * g[9] + double(rc::ldf32(e + 0x90))));
    }
    cpu.flags.pf = true;
    cpu.esp += 24;
}

// CRC-32 of data, then of the length's bytes, inverted (cdecl: data, length, crc), 0x46cad0.
void sub_46cad0(win32::WinApplication* app, x86::CPU& cpu)
{
    if (!useNative())
    {
        sub_46cad0_guest(app, cpu);
        return;
    }
    constexpr x86::reg32 Table = 0x9c6268;
    const x86::reg32 data = arg(cpu, 0);
    const x86::sreg32 length = x86::sreg32(arg(cpu, 1));
    x86::reg32 crc = arg(cpu, 2);
    cpu.esp += 4;
    if (length <= 0)
    {
        cpu.eax = ~crc;
        cpu.edx = data;
        cpu.flags.pf = parityFlag(x86::reg32(length));
        return;
    }
    const x86::reg8* p = rc::host(data);
    for (x86::sreg32 i = 0; i < length; ++i)
        crc = rc::ld32(Table + 4 * ((crc >> 24 ^ p[i]) & 0xff)) ^ crc << 8;
    x86::reg32 index = 0;
    for (x86::sreg32 n = length; n > 0; n >>= 8)
    {
        index = (crc >> 24 ^ x86::reg32(n)) & 0xff;
        crc = rc::ld32(Table + 4 * index) ^ crc << 8;
    }
    cpu.eax = ~crc;
    cpu.edx = index;
    cpu.flags.pf = true;
}

// Binary search for key in count entries of stride bytes at base, the key at offset in each entry; returns the
// first entry with the key or 0 (cdecl: key, base, count, offset, stride), 0x558360.
void sub_558360(win32::WinApplication* app, x86::CPU& cpu)
{
    if (!useNative())
    {
        sub_558360_guest(app, cpu);
        return;
    }
    const x86::reg32 key = arg(cpu, 0);
    const x86::reg32 base = arg(cpu, 1);
    const x86::reg32 count = arg(cpu, 2);
    const x86::reg32 offset = arg(cpu, 3);
    const x86::reg32 stride = arg(cpu, 4);
    cpu.esp += 4;
    cpu.eax = 0;
    if (base == 0 || x86::sreg32(count) <= 0 || offset + 4 > stride)
    {
        cpu.flags.pf = true;
        return;
    }
    x86::reg32 lo = 0, hi = count - 1, mid = 0, entry = 0, value = 0;
    for (;;)
    {
        const x86::reg32 sum = lo + hi;
        mid = x86::reg32(x86::sreg32(sum - x86::reg32(x86::sreg32(sum) >> 31)) >> 1);
        entry = mid * stride + base;
        value = rc::ld32(entry + offset);
        if (key < value)
            hi = mid - 1;
        else if (key > value)
            lo = mid + 1;
        else
            break;
        if (x86::sreg32(lo) > x86::sreg32(hi))
        {
            cpu.ecx = mid;
            cpu.edx = value;
            cpu.flags.pf = true;
            return;
        }
    }
    // Back to the first entry with the key; the translated code takes PF from the count tests only.
    cpu.flags.pf = parityFlag(mid);
    if (x86::sreg32(mid) > 0)
    {
        x86::reg32 at = entry + offset - stride;
        while (rc::ld32(at) == key)
        {
            entry -= stride;
            at -= stride;
            --mid;
            cpu.flags.pf = parityFlag(mid);
            if (x86::sreg32(mid) <= 0)
                break;
        }
        value = at;
    }
    cpu.eax = entry;
    cpu.ecx = mid;
    cpu.edx = value;
}

// Triangle of a packed collision entry (thiscall: entries, index, origin, out), 0x7e7c30: three vertices of 16-bit
// coordinates times the scale at 0xa7ad44 plus origin, as vectors with w = 0, then the entry's 16-bit flags.
void sub_7e7c30(win32::WinApplication* app, x86::CPU& cpu)
{
    if (!useNative())
    {
        sub_7e7c30_guest(app, cpu);
        return;
    }
    const x86::reg32 entry = cpu.ecx + arg(cpu, 0) * 8;
    const x86::reg32 origin = arg(cpu, 1);
    const x86::reg32 out = arg(cpu, 2);
    static const x86::reg32 offsets[9] = { 0, 2, 4, 8, 10, 12, 16, 18, 20 };
    x86::sreg32 coordinate = 0;
    for (int k = 0; k < 9; ++k)
    {
        coordinate = x86::sreg16(rc::ld16(entry + offsets[k]));
        const double scale = rc::ldf32(0xa7ad44);
        const x86::reg32 to = out + 16 * (k / 3) + 4 * (k % 3);
        const float value = float(double(coordinate) * scale + double(rc::ldf32(origin + 4 * (k % 3))));
        if (k == 2 || k == 5)
            rc::st32(to + 4, 0);
        rc::stf32(to, value);
    }
    const x86::reg8 low = rc::ld8(entry + 22), high = rc::ld8(entry + 23);
    const x86::reg16 flags = x86::reg16(low | high << 8);
    // The guest builds the flags in the index argument slot, over the last coordinate.
    rc::st32(cpu.esp + 4, (x86::reg32(coordinate) & 0xffff0000u) | flags);
    rc::st16(out + 0x2c, flags);
    cpu.eax = ((entry + 16) & 0xffffff00u) | high;
    cpu.edx = (origin & 0xffff0000u) | flags;
    cpu.flags.pf = true;
    cpu.esp += 16;
}

// Distance attenuation of a directed object (cdecl: object, position, radius, distance out), 0x79f9c0: 0 when the
// position is more than radius behind the object's plane; otherwise stores the distance and returns, truncated,
// the factor at object+0x40 scaled by radius / (distance - radius) when that is above radius.
void sub_79f9c0(win32::WinApplication* app, x86::CPU& cpu)
{
    if (!useNative())
    {
        sub_79f9c0_guest(app, cpu);
        return;
    }
    const x86::reg32 object = arg(cpu, 0);
    const x86::reg32 position = arg(cpu, 1);
    const double radius = argFloat(cpu, 2);
    const x86::reg32 distanceOut = arg(cpu, 3);
    const double vx = double(rc::ldf32(position)) - double(rc::ldf32(object + 0x20));
    const double vy = double(rc::ldf32(position + 4)) - double(rc::ldf32(object + 0x24));
    const double vz = double(rc::ldf32(position + 8)) - double(rc::ldf32(object + 0x28));
    double d = vz * double(rc::ldf32(object + 0x38)) + vy * double(rc::ldf32(object + 0x34));
    d += vx * double(rc::ldf32(object + 0x30));
    const double behind = -radius;
    cpu.fpu.compare(behind, d);
    cpu.esp += 4;
    if (behind > d)
    {
        cpu.eax = 0;
        return;
    }
    double squared = vx * vx + vz * vz;
    squared += vy * vy;
    const double distance = std::sqrt(squared);
    rc::stf32(distanceOut, float(distance));
    const float beyond = float(distance - radius);
    cpu.fpu.compare(beyond, radius);
    double factor = rc::ldf32(object + 0x40);
    if (double(beyond) > radius)
        factor = (radius / double(beyond)) * double(rc::ldf32(object + 0x40));
    // _ftol2 (native in this build): truncated to 64 bits in edx:eax.
    cpu.edx_eax = x86::reg64(x86::truncInteger64(factor));
    cpu.ecx = object;
}

// SplitPermTempChunks (cdecl: flag, chunk, size, out, offset, depth), 0x699420: splits loaded chunks into the
// permanent or temporary buffer.
void sub_699420(win32::WinApplication* app, x86::CPU& cpu)
{
    if (!useNative())
    {
        sub_699420_guest(app, cpu);
        return;
    }
    SplitState s = { cpu.ecx, cpu.edx, cpu.flags.pf != 0 };
    const x86::reg32 chunk = arg(cpu, 1);
    const x86::reg32 size = arg(cpu, 2);
    const bool empty = chunk >= chunk + size;
    cpu.eax = splitChunks(arg(cpu, 0), chunk, size, arg(cpu, 3), arg(cpu, 4), arg(cpu, 5), cpu.esp, s);
    if (!empty)
    {
        cpu.ecx = s.ecx;
        cpu.edx = s.edx;
        cpu.flags.pf = s.pf;
    }
    cpu.esp += 4;
}

// Road spline system reduction (cdecl: system, out), 0x7ec920: on a row-major float matrix with its rows at +0x550,
// row stride at +0x554 and size at +0x558, shifts row i left by size - i and clears the vacated end, then
// eliminates the first column below each pivot (a zero pivot becomes 1e-20), storing the factors in out.
void sub_7ec920(win32::WinApplication* app, x86::CPU& cpu)
{
    if (!useNative())
    {
        sub_7ec920_guest(app, cpu);
        return;
    }
    using x86::sreg32;
    const x86::reg32 m = arg(cpu, 0);
    const x86::reg32 out = arg(cpu, 1);
    const x86::reg32 slot = cpu.esp + 4;
    auto stride = [m]() { return rc::ld32(m + 0x554); };
    auto at = [m](x86::reg32 index) { return m + 4 * index; };
    const x86::reg32 size = rc::ld32(m + 0x558);
    rc::st32(out + 0x550, rc::ld32(m + 0x550));
    rc::st32(out + 0x554, size);
    rc::st32(out + 0x558, size - 1);
    const x86::reg32 cols = rc::ld32(m + 0x554);
    x86::reg32 shift = rc::ld32(m + 0x558);
    if (sreg32(shift) > 0)
    {
        // The guest keeps its row countdown in the system argument slot.
        x86::reg32 count = shift;
        rc::st32(slot, count);
        x86::reg32 clearFrom = cols - shift;
        x86::reg32 top = shift;
        for (x86::reg32 i = 0;;)
        {
            x86::reg32 j = top - i;
            if (sreg32(cols - j) >= 4)
            {
                do
                {
                    for (x86::reg32 k = 0; k < 4; ++k)
                    {
                        const x86::reg32 row = stride() * i;
                        const x86::reg32 v = rc::ld32(at(row + j) + 4 * k);
                        rc::st32(at(row - shift + j) + 4 * k, v);
                    }
                    j += 4;
                } while (sreg32(j) < sreg32(cols - 3));
            }
            for (; sreg32(j) < sreg32(cols); ++j)
            {
                const x86::reg32 row = stride() * i;
                rc::st32(at(row - shift + j), rc::ld32(at(row + j)));
            }
            x86::reg32 c = clearFrom;
            if (sreg32(count) >= 4)
            {
                do
                {
                    for (x86::reg32 k = 0; k < 4; ++k)
                        rc::st32(at(stride() * i + c) + 4 * k, 0);
                    c += 4;
                } while (sreg32(c) < sreg32(cols - 3));
            }
            for (; sreg32(c) < sreg32(cols); ++c)
                rc::st32(at(stride() * i + c), 0);
            ++i;
            --shift;
            rc::st32(slot, --count);
            top = rc::ld32(m + 0x558);
            ++clearFrom;
            if (sreg32(i) >= sreg32(top))
                break;
        }
    }
    x86::reg32 rows = rc::ld32(m + 0x550);
    x86::reg32 limit = rc::ld32(m + 0x558);
    for (x86::reg32 r = 0; sreg32(r) < sreg32(rows); rows = rc::ld32(m + 0x550))
    {
        const x86::reg32 pivot = at(stride() * r);
        const float p = rc::ldf32(pivot);
        if (sreg32(limit) < sreg32(rows))
            ++limit;
        if (double(p) == 0.0)
            rc::st32(pivot, 0x1e3ce508);
        const x86::reg32 next = r + 1;
        for (x86::reg32 s = next; sreg32(s) < sreg32(limit); ++s)
        {
            const float f = float(double(rc::ldf32(at(stride() * s))) / double(rc::ldf32(at(stride() * r))));
            rc::stf32(slot, f);
            rc::stf32(out + 4 * ((rc::ld32(out + 0x554) - 1) * r + s) - 4, f);
            x86::reg32 k = 1;
            auto eliminate = [&](x86::reg32 q) {
                const x86::reg32 sr = stride();
                const x86::reg32 to = at(sr * s + k) + 4 * q;
                const double v = double(rc::ldf32(to)) - double(f) * double(rc::ldf32(at(sr * r + k) + 4 * q));
                rc::stf32(to - 4, float(v));
            };
            if (sreg32(cols - 1) >= 4)
            {
                do
                {
                    for (x86::reg32 q = 0; q < 4; ++q)
                        eliminate(q);
                    k += 4;
                } while (sreg32(k) < sreg32(cols - 3));
            }
            for (; sreg32(k) < sreg32(cols); ++k)
                eliminate(0);
            rc::st32(at(stride() * s + cols) - 4, 0);
        }
        r = next;
    }
    cpu.eax = rows;
    cpu.esp += 4;
}

// Vector (x, y, z) of v with w (cdecl: out, v, w), 0x401710; returns out.
void sub_401710(win32::WinApplication* app, x86::CPU& cpu)
{
    if (!useNative())
    {
        sub_401710_guest(app, cpu);
        return;
    }
    const x86::reg32 out = arg(cpu, 0);
    const x86::reg32 v = arg(cpu, 1);
    const x86::reg32 x = rc::ld32(v), y = rc::ld32(v + 4), z = rc::ld32(v + 8);
    const x86::reg32 w = arg(cpu, 2);
    rc::st32(out, x);
    rc::st32(out + 4, y);
    rc::st32(out + 8, z);
    rc::st32(out + 12, w);
    cpu.eax = out;
    cpu.edx = out;
    cpu.esp += 4;
}

// std::rotate of 8-byte elements (cdecl: first, middle, last), 0x71a350: the cycle algorithm, one cycle per
// greatest common divisor step of the two lengths.
void sub_71a350(win32::WinApplication* app, x86::CPU& cpu)
{
    if (!useNative())
    {
        sub_71a350_guest(app, cpu);
        return;
    }
    using x86::sreg32;
    const x86::reg32 first = arg(cpu, 0);
    const x86::reg32 middle = arg(cpu, 1);
    const x86::reg32 last = arg(cpu, 2);
    const x86::reg32 shift = x86::reg32(sreg32(middle - first) >> 3);
    const x86::reg32 count = x86::reg32(sreg32(last - first) >> 3);
    cpu.esp += 4;
    cpu.ecx = count;
    cpu.flags.pf = parityFlag(shift);
    x86::reg32 cycles = count;
    if (shift)
    {
        x86::reg32 a = count, b = shift, r;
        do
        {
            r = x86::reg32(sreg32(a) % sreg32(b));
            a = b;
            b = r;
        } while (r);
        cycles = a;
        cpu.edx = 0;
        cpu.flags.pf = true;
    }
    if (sreg32(cycles) >= sreg32(count))
        return;
    cpu.flags.pf = parityFlag(cycles);
    if (sreg32(cycles) <= 0)
        return;
    x86::reg32 start = first + cycles * 8;
    x86::reg32 hole = 0;
    x86::reg32 lo = 0;
    for (x86::reg32 n = cycles; n; )
    {
        lo = rc::ld32(start);
        const x86::reg32 hi = rc::ld32(start + 4);
        x86::reg32 from = start + shift * 8;
        hole = start;
        if (from == last)
            from = first;
        while (from != start)
        {
            rc::st32(hole, rc::ld32(from));
            rc::st32(hole + 4, rc::ld32(from + 4));
            const x86::reg32 left = x86::reg32(sreg32(last - from) >> 3);
            hole = from;
            from = sreg32(shift) >= sreg32(left) ? first + (shift - left) * 8 : from + shift * 8;
        }
        rc::st32(hole + 4, hi);
        start -= 8;
        rc::st32(hole, lo);
        // The guest counts the cycles down in the middle argument slot.
        rc::st32(cpu.esp + 4, --n);
    }
    cpu.ecx = lo;
    cpu.edx = hole;
}

// QueryLightHelper (cdecl: query, info, light, x, y, z), 0x7381a0: for an active light whose range holds the
// point, the attenuation (1 inside the inner radius, falling to 0 at the range), then by light type: a colour into
// the info's list or nearest light, or the light and its attenuation into the query's list.
void sub_7381a0(win32::WinApplication* app, x86::CPU& cpu)
{
    if (!useNative())
    {
        sub_7381a0_guest(app, cpu);
        return;
    }
    const x86::reg32 query = arg(cpu, 0);
    const x86::reg32 info = arg(cpu, 1);
    const x86::reg32 light = arg(cpu, 2);
    const x86::reg32 slot = cpu.esp + 12;
    const double x = argFloat(cpu, 3), y = argFloat(cpu, 4), z = argFloat(cpu, 5);
    const x86::reg8 active = rc::ld8(light + 7);
    cpu.esp += 4;
    if (active != 1)
    {
        cpu.eax = (cpu.eax & 0xffffff00u) | active;
        return;
    }
    const double dx = std::fabs(x - double(rc::ldf32(light + 0x10)));
    const double dy = std::fabs(y - double(rc::ldf32(light + 0x14)));
    const double dz = std::fabs(z - double(rc::ldf32(light + 0x18)));
    const double range = rc::ldf32(light + 0x1c);
    const float range2 = float(range * range);
    const float distance2 = float((dz * dz + dy * dy) + dx * dx);
    // eax carries the status words of the guest's fnstsw.
    auto status = [&cpu](x86::reg32 high, double a, double b) {
        cpu.fpu.compare(a, b);
        return (high & 0xffff0000u) | cpu.fpu.status.word;
    };
    x86::reg32 eax = status(cpu.eax, distance2, range2);
    if (!(double(distance2) < double(range2)))
    {
        eax = rc::ld32(0xb42ee0);
        if (eax == 0)
        {
            cpu.eax = 0;
            return;
        }
    }
    const x86::reg8 type = rc::ld8(light + 4);
    const x86::reg8 kind = rc::ld8(light + 5);
    const double inner = double(rc::ldf32(light + 0x30)) * double(rc::ldf32(light + 0x30));
    // The guest keeps the attenuation in the light argument slot.
    float attenuation = 1.0f;
    rc::stf32(slot, attenuation);
    eax = status(eax, distance2, inner);
    if (double(distance2) > inner)
    {
        attenuation = float(double(rc::ldf32(0x9c2478)) - (double(distance2) - inner) / (double(range2) - inner));
        rc::stf32(slot, attenuation);
    }
    if (type == 0)
    {
        const x86::reg32 colour = rc::ld32(light + 0xc);
        const double c0 = double(colour & 0xff);
        const float c1 = float(colour >> 8 & 0xff);
        const float c2 = float(colour >> 16 & 0xff);
        const x86::reg32 c3 = rc::ld32(light + 0x2c);
        const double a = attenuation;
        if (kind == 2)
        {
            const x86::reg32 entry = rc::ld32(info + 0xb4);
            if (entry >= rc::ld32(info + 0xb8))
            {
                cpu.eax = entry;
                return;
            }
            rc::stf32(entry, float(c0 * a));
            rc::stf32(rc::ld32(info + 0xb4) + 4, float(double(c1) * a));
            const x86::reg32 third = rc::ld32(info + 0xb4);
            rc::stf32(third + 8, float(double(c2) * a));
            float c3f;
            memcpy(&c3f, &c3, sizeof(c3f));
            rc::stf32(rc::ld32(info + 0xb4) + 12, float(double(c3f) * a));
            eax = status(third, a, 0.0);
            if (!(a > 0.0))
            {
                cpu.eax = eax;
                return;
            }
            const x86::reg32 next = rc::ld32(info + 0xb4) + 16;
            const float total = float(a + double(rc::ldf32(info + 0xbc)));
            rc::st32(info + 0xb4, next);
            rc::stf32(info + 0xbc, total);
            cpu.eax = next;
            return;
        }
        eax = status(c3, distance2, rc::ldf32(info + 0xc0));
        if (!(double(distance2) < double(rc::ldf32(info + 0xc0))))
        {
            cpu.eax = eax;
            return;
        }
        x86::reg32 c1Bits, c2Bits, d2Bits;
        memcpy(&c1Bits, &c1, 4);
        memcpy(&c2Bits, &c2, 4);
        memcpy(&d2Bits, &distance2, 4);
        rc::stf32(rc::ld32(info + 0xb0), float(c0));
        rc::st32(rc::ld32(info + 0xb0) + 4, c1Bits);
        rc::st32(rc::ld32(info + 0xb0) + 8, c2Bits);
        rc::st32(rc::ld32(info + 0xb0) + 12, c3);
        rc::st32(info + 0xc0, d2Bits);
        cpu.eax = c3;
        return;
    }
    if (type == 2 || type == 3 || type == 5)
    {
        cpu.eax = info;
        const x86::reg32 n = rc::ld32(info + 0xc8);
        if (x86::sreg32(n) >= 24)
        {
            const x86::reg32 overflow = type == 2 ? 0xb4cf90 : 0xb4cf8c;
            rc::st32(overflow, rc::ld32(overflow) + 1);
            return;
        }
        rc::st32(query + n * 4, light);
        rc::stf32(query + rc::ld32(info + 0xc8) * 4 + 0x60, attenuation);
        rc::st32(info + 0xc8, rc::ld32(info + 0xc8) + 1);
        return;
    }
    cpu.eax = eax;
}

}
}
