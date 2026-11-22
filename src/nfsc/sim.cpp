// Native replacements of hot simulation functions (road navigation, collision): the same double-precision steps
// and float stores as the translated x87 code, guest stack locals as host locals. hot_native=0 runs the translated
// bodies instead.

#include "sim.h"

#include <native.h>
#include <platform/config.h>
#include <platform/log.h>
#include <recomp.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <vector>

namespace nfsc
{

// Translated bodies of the overridden guest functions, and translated functions called from here.
void sub_7f9570_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_7e83e0_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_7fc550_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_403f10(win32::WinApplication* app, x86::CPU& cpu);
void sub_7f8bd0_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_7f9d90_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_7fb180_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_7f8930_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_7e7d40_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_7e7eb0_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_7ecc70(win32::WinApplication* app, x86::CPU& cpu);
void sub_8088e0_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_8078f0(win32::WinApplication* app, x86::CPU& cpu);

namespace native
{
void sub_7ec920(win32::WinApplication* app, x86::CPU& cpu);
}

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

x86::reg32 floatBits(float value)
{
    x86::reg32 bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

// A road spline point (64 bytes): two edge positions (x, z) at +0 and +8, the lateral parameter t at +0x2c.
x86::reg32 roadPoint(x86::reg32 points, x86::reg32 index)
{
    return points + (index << 6);
}

// Position at the point's parameter between its edges: x = t * right + (1 - t) * left, z unrounded; tFirst picks
// the guest's operand order of the z sum.
template< bool tFirst >
void edgeExact(x86::reg32 p, double one, float& x, double& z)
{
    const double t = rc::ldf32(p + 0x2c);
    const double s = one - t;
    x = float(t * double(rc::ldf32(p + 8)) + s * double(rc::ldf32(p)));
    const double a = t * double(rc::ldf32(p + 0xc));
    const double b = s * double(rc::ldf32(p + 4));
    z = tFirst ? a + b : b + a;
}

template< bool tFirst >
void edgePosition(x86::reg32 p, double one, float& x, float& z)
{
    double exact;
    edgeExact<tFirst>(p, one, x, exact);
    z = float(exact);
}

// Byte offset of point i of a road ring buffer (count at +0, start at +4, capacity at +8, 64-byte points from
// +0x10): i modulo count while the buffer is not full, else start + i + 1 modulo the capacity (the guest's idiv).
x86::reg32 ringOffset(x86::reg32 road, x86::reg32 i, x86::reg32 count, x86::reg32 capacity)
{
    if (x86::sreg32(count) < x86::sreg32(capacity))
        return rc::divideSigned(x86::reg32(x86::sreg32(i) >> 31), i, count).remainder << 6;
    const x86::reg32 j = rc::ld32(road + 4) + i + 1;
    return rc::divideSigned(x86::reg32(x86::sreg32(j) >> 31), j, capacity).remainder << 6;
}

// ringOffset of consecutive indices from i: the remainder steps without a divide while the dividend is positive.
struct RingCursor
{
    x86::reg32 ring = 0, dividend = 0, divisor = 0, remainder = 0;
    bool started = false;

    RingCursor() = default;

    RingCursor(x86::reg32 road, x86::reg32 i) : ring(road), started(true)
    {
        const x86::reg32 count = rc::ld32(road), capacity = rc::ld32(road + 8);
        const bool full = x86::sreg32(count) >= x86::sreg32(capacity);
        dividend = full ? rc::ld32(road + 4) + i + 1 : i;
        divisor = full ? capacity : count;
        divide();
    }

    void divide()
    {
        remainder = rc::divideSigned(x86::reg32(x86::sreg32(dividend) >> 31), dividend, divisor).remainder;
    }

    // Guest address of the current point.
    x86::reg32 point() const { return ring + 0x10 + (remainder << 6); }

    void next()
    {
        ++dividend;
        if (x86::sreg32(dividend) > 0 && x86::sreg32(remainder) >= 0 && x86::sreg32(divisor) > 0)
            remainder = remainder + 1 == divisor ? 0 : remainder + 1;
        else
            divide();
    }
};

// Edge vector (right - left) of a road point.
void edgeVector(x86::reg32 p, float& x, float& z)
{
    x = float(double(rc::ldf32(p + 8)) - double(rc::ldf32(p)));
    z = float(double(rc::ldf32(p + 0xc)) - double(rc::ldf32(p + 4)));
}

double lengthSquared(const float v[3])
{
    return (double(v[0]) * double(v[0]) + double(v[2]) * double(v[2])) + double(v[1]) * double(v[1]);
}

// x87 compare whose status word the translated code keeps: applied once, before guest calls and at the end.
struct PendingCompare
{
    double a = 0, b = 0;
    bool pending = false;

    void set(double x, double y)
    {
        a = x;
        b = y;
        pending = true;
    }

    void apply(x86::CPU& cpu)
    {
        if (pending)
            cpu.fpu.compare(a, b);
        pending = false;
    }
};

// Point (x, z) inside the triangle of three 16-byte vertices at tri (x at +0, z at +8), both windings, edges
// included: the guest's three edge tests against zero. last: the final compare operand.
bool inTriangle(x86::reg32 tri, double px, double pz, double zero, double& last)
{
    auto v = [tri](x86::reg32 offset) { return double(rc::ldf32(tri + offset)); };
    const double c1 = (pz - v(0x18)) * (v(0) - v(0x10)) - (px - v(0x10)) * (v(8) - v(0x18));
    last = c1;
    if (c1 <= zero)
    {
        const double c2 = (v(0x10) - v(0x20)) * (pz - v(0x28)) - (v(0x18) - v(0x28)) * (px - v(0x20));
        last = c2;
        if (!(c2 <= zero))
            return false;
        const double c3 = (pz - v(8)) * (v(0x20) - v(0)) - (v(0x28) - v(8)) * (px - v(0));
        last = c3;
        return c3 <= zero;
    }
    const double c2 = (pz - v(0x28)) * (v(0x10) - v(0x20)) - (v(0x18) - v(0x28)) * (px - v(0x20));
    last = c2;
    if (!(c2 >= zero))
        return false;
    const double c3 = (v(0x20) - v(0)) * (pz - v(8)) - (v(0x28) - v(8)) * (px - v(0));
    last = c3;
    return c3 >= zero;
}

int s_roadnav = -1;

bool roadnavNative()
{
    if (s_roadnav < 0)
        s_roadnav = pf::Config::getBool("roadnav_native", true) ? 1 : 0;
    return s_roadnav != 0;
}

int s_roadnav2 = -1;

bool roadnav2Native()
{
    if (s_roadnav2 < 0)
        s_roadnav2 = pf::Config::getBool("roadnav2_native", true) ? 1 : 0;
    return s_roadnav2 != 0;
}

// Road spline system solve (cdecl: system, factors, right-hand side), 0x7ecc70: forward elimination of the
// right-hand side with the reduction's factors, then back substitution, each value stored as a float. Sets eax, ecx
// and edx as the translated body leaves them.
void solveSystem(x86::CPU& cpu, x86::reg32 system, x86::reg32 factors, x86::reg32 rhs)
{
    using x86::sreg32;
    const x86::reg32 rows = rc::ld32(system + 0x550);
    const x86::reg32 stride = rc::ld32(system + 0x554);
    x86::reg32 band = rc::ld32(system + 0x558);
    x86::reg32 eax = rows, ecx = band, edx = 1;
    if (sreg32(rows) > 0)
    {
        const x86::reg32 factorStride = rc::ld32(factors + 0x554) - 1;
        for (x86::reg32 i = 0; sreg32(i) < sreg32(rows); ++i)
        {
            if (sreg32(band) < sreg32(rows))
                ++band;
            const double pivot = rc::ldf32(rhs + 4 * i);
            const x86::reg32 row = factors + 4 * (factorStride * i - 1);
            for (x86::reg32 j = i + 1; sreg32(j) < sreg32(band); ++j)
            {
                const double product = double(rc::ldf32(row + 4 * j)) * pivot;
                rc::stf32(rhs + 4 * j, float(double(rc::ldf32(rhs + 4 * j)) - product));
            }
        }
    }
    for (x86::reg32 i = rows - 1; sreg32(i) >= 0; --i)
    {
        const x86::reg32 terms = edx;
        const x86::reg32 row = system + 4 * (stride * i);
        double sum = rc::ldf32(rhs + 4 * i);
        for (x86::reg32 j = 1; sreg32(j) < sreg32(terms); ++j)
            sum -= double(rc::ldf32(row + 4 * j)) * double(rc::ldf32(rhs + 4 * (i + j)));
        // eax: the unrolled loop's pointer when its groups of four took every term, else the spent count.
        eax = 0;
        if (sreg32(terms - 1) >= 4)
        {
            const x86::reg32 groups = ((terms - 5) >> 2) + 1;
            if (groups * 4 + 1 == terms)
                eax = rhs + 4 * i + 4 + 16 * groups;
        }
        ecx = stride * i;
        if (sreg32(terms) < sreg32(stride))
            edx = terms + 1;
        rc::stf32(rhs + 4 * i, float(sum / double(rc::ldf32(row))));
    }
    cpu.eax = eax;
    cpu.ecx = ecx;
    cpu.edx = edx;
}

// shadow_roadnav=1: each call of a road navigation port runs the translated body on a copy of the CPU, then the
// native one from the same guest memory, and compares the registers, the x87 stack and the memory it writes.
struct ShadowRegion
{
    x86::reg32 address, size;
};

int s_shadow = -1;
bool s_shadowRunning = false;
uint32_t s_shadowCompared = 0, s_shadowDiffer = 0;

bool shadowOn()
{
    if (s_shadow < 0)
        s_shadow = pf::Config::getBool("shadow_roadnav", false) ? 1 : 0;
    return s_shadow != 0 && !s_shadowRunning;
}

// Logs a difference of the current call while fewer than 16 calls differed before it.
void shadowReport(const char* name, const char* what, x86::reg32 guest, x86::reg32 native, int& found)
{
    ++found;
    if (s_shadowDiffer < 16)
        pf::logInfo("shadow_roadnav: %s call %u: %s translated 0x%08x native 0x%08x", name, unsigned(s_shadowCompared),
                    what, unsigned(guest), unsigned(native));
}

// Registers compared, by bit in the order eax, ecx, edx, ebx, esp, ebp, esi, edi, then the x87 status word: all, or
// the ones the callers read.
constexpr x86::reg32 AllRegisters = 0x1ff;
constexpr x86::reg32 LiveEaxEbxEbp = 0x139;
constexpr x86::reg32 CalleeSaved = 0xf8;

// Runs port (the native entry, with the shadow off) after the translated body guest; regions: the guest memory
// either may write.
void shadowCall(const char* name, win32::WinApplication* app, x86::CPU& cpu, win32::MethodPtr guest,
                win32::MethodPtr port, std::initializer_list<ShadowRegion> regions, x86::reg32 live = AllRegisters)
{
    s_shadowRunning = true;
    std::vector<x86::reg8> before, translated;
    for (const ShadowRegion& r : regions)
        before.insert(before.end(), rc::host(r.address), rc::host(r.address) + r.size);
    x86::CPU reference = cpu;
    guest(app, reference);
    size_t at = 0;
    for (const ShadowRegion& r : regions)
    {
        translated.insert(translated.end(), rc::host(r.address), rc::host(r.address) + r.size);
        memcpy(rc::host(r.address), before.data() + at, r.size);
        at += r.size;
    }
    port(app, cpu);
    s_shadowRunning = false;
    if (reference.terminate || cpu.terminate)
    {
        cpu.terminate = true;
        return;
    }
    ++s_shadowCompared;
    int found = 0;
    const x86::reg32 g[] = { reference.eax, reference.ecx, reference.edx, reference.ebx, reference.esp,
                             reference.ebp, reference.esi, reference.edi };
    const x86::reg32 n[] = { cpu.eax, cpu.ecx, cpu.edx, cpu.ebx, cpu.esp, cpu.ebp, cpu.esi, cpu.edi };
    static const char* const names[] = { "eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi" };
    for (int i = 0; i < 8; ++i)
    {
        if ((live >> i & 1) && g[i] != n[i])
            shadowReport(name, names[i], g[i], n[i], found);
    }
    if (reference.fpu.count != cpu.fpu.count)
    {
        shadowReport(name, "x87 depth", reference.fpu.count, cpu.fpu.count, found);
    }
    else if (reference.fpu.count != 0)
    {
        x86::reg64 a, b;
        static_assert(sizeof(a) == sizeof(x86::Float), "x87 registers are doubles");
        memcpy(&a, &reference.fpu.st(0), sizeof(a));
        memcpy(&b, &cpu.fpu.st(0), sizeof(b));
        if (a != b)
            shadowReport(name, "st0 high word", x86::reg32(a >> 32), x86::reg32(b >> 32), found);
        if (x86::reg32(a) != x86::reg32(b))
            shadowReport(name, "st0 low word", x86::reg32(a), x86::reg32(b), found);
    }
    if ((live & 0x100) && reference.fpu.status.word != cpu.fpu.status.word)
        shadowReport(name, "x87 status", reference.fpu.status.word, cpu.fpu.status.word, found);
    at = 0;
    for (const ShadowRegion& r : regions)
    {
        for (x86::reg32 i = 0; i < r.size; ++i)
        {
            if (translated[at + i] != *rc::host(r.address + i))
            {
                char what[48];
                snprintf(what, sizeof(what), "byte at 0x%08x", unsigned(r.address + i));
                shadowReport(name, what, translated[at + i], *rc::host(r.address + i), found);
                break;
            }
        }
        at += r.size;
    }
    if (found)
        ++s_shadowDiffer;
}

}

namespace sim
{

void logShadow()
{
    if (s_shadow > 0)
        pf::logInfo("shadow_roadnav: %u calls compared, %u differ", unsigned(s_shadowCompared),
                    unsigned(s_shadowDiffer));
}

}

namespace native
{

// The tension pass of 0x7f9570 on its arguments; sets eax and the x87 status word as the translated body leaves them.
static void tensionPass(win32::WinApplication* app, x86::CPU& cpu, x86::reg32 points, x86::reg32 count,
                        x86::reg32 a, x86::reg32 b, x86::reg32 first, x86::reg32 end)
{
    using x86::sreg32;
    const double one = rc::ldf32a(0x9c2478);
    const double zero = rc::ldf32a(0x9c248c);
    // Guest stack vectors (x, z, y) by their frame offsets.
    float v20[3], v40[3], v60[3], v70[3], v80[3], v90[3];
    v90[0] = rc::ldf32(a);
    v90[1] = rc::ldf32(a + 4);
    v90[2] = 0.0f;
    v70[0] = rc::ldf32(b);
    v70[1] = rc::ldf32(b + 4);
    v70[2] = 0.0f;
    // Direction into the first point.
    if (first == 0)
    {
        memcpy(v40, v70, sizeof(v40));
    }
    else
    {
        const float* c = v90;
        if (first != 1)
        {
            edgePosition<false>(roadPoint(points, first - 2), one, v80[0], v80[1]);
            v80[2] = 0.0f;
            c = v80;
        }
        float x, z;
        edgePosition<true>(roadPoint(points, first - 1), one, x, z);
        const float dx = float(double(x) - double(c[0]));
        const float dz = float(double(z) - double(c[1]));
        const float dy = float(-double(c[2]));
        v80[0] = dx;
        v80[1] = dz;
        v80[2] = dy;
        memcpy(v40, v80, sizeof(v40));
    }
    const float* c = v90;
    if (first != 0)
    {
        edgePosition<false>(roadPoint(points, first - 1), one, v70[0], v70[1]);
        v70[2] = 0.0f;
        c = v70;
    }
    edgePosition<true>(roadPoint(points, first), one, v80[0], v80[1]);
    v20[0] = float(double(v80[0]) - double(c[0]));
    v20[1] = float(double(v80[1]) - double(c[1]));
    v20[2] = float(-double(c[2]));
    const x86::reg32 last = count - 1;
    if (first == last)
    {
        v70[0] = rc::ldf32(roadPoint(points, first) + 0x10);
        v70[1] = rc::ldf32(roadPoint(points, first) + 0x14);
    }
    else
    {
        edgePosition<true>(roadPoint(points, first), one, v90[0], v90[1]);
        edgePosition<true>(roadPoint(points, first + 1), one, v70[0], v70[1]);
        v70[0] = float(double(v70[0]) - double(v90[0]));
        v70[1] = float(double(v70[1]) - double(v90[1]));
    }
    v70[2] = 0.0f;
    memcpy(v60, v70, sizeof(v60));
    // eax: the bits the guest loads last (fnstsw results in the low word at the last point).
    x86::reg32 eax = 0;
    bool pending = false;
    double compareA = 0, compareB = 0;
    for (x86::reg32 k = first; sreg32(k) < sreg32(end); ++k)
    {
        const x86::reg32 p = roadPoint(points, k);
        v80[0] = float(double(rc::ldf32(p + 8)) - double(rc::ldf32(p)));
        v80[1] = float(double(rc::ldf32(p + 0xc)) - double(rc::ldf32(p + 4)));
        const double l20 = lengthSquared(v20);
        double q = lengthSquared(v40);
        if (k == 0)
        {
            q *= l20;
        }
        else
        {
            for (int i = 0; i < 3; ++i)
                v90[i] = float(double(v20[i]) + double(v40[i]));
            q *= (double(v90[2]) * double(v90[2]) + double(v90[1]) * double(v90[1])) + double(v90[0]) * double(v90[0]);
        }
        const float s1c = float(q * l20);
        const double sx = double(v60[0]) + double(v20[0]);
        const double sz = double(v60[1]) + double(v20[1]);
        const double sy = double(v60[2]) + double(v20[2]);
        float s18 = float(lengthSquared(v60));
        const double ls = (sy * sy + sz * sz) + sx * sx;
        const float s54 = float((ls * double(s18)) * l20);
        auto cross = [](const float u[3], const float v[3]) {
            return float(double(u[1]) * double(v[0]) - double(v[1]) * double(u[0]));
        };
        const float s34 = cross(v40, v20);
        const float s58 = cross(v20, v60);
        const float s50 = cross(v40, v80);
        const float s3c = cross(v20, v80);
        const float s5c = cross(v60, v80);
        const double d1c = s1c, d54 = s54, d34 = s34, d58 = s58, d50 = s50, d3c = s3c, d5c = s5c;
        if (k == last)
        {
            const double num = ((d5c + d3c) * d58) * d1c - (d50 * d34) * d54;
            const double den = ((((d3c + d3c) + d5c) * d5c) * d1c + (d50 * d50) * d54) + (d3c * d3c) * d1c;
            double d = num / den;
            const float t = rc::ldf32(p + 0x2c);
            const float upper = float(one - double(t));
            const float lower = float(-double(t));
            cpu.fpu.compare(d, lower);
            eax = (eax & 0xffff0000u) | cpu.fpu.status.word;
            if (!(d > double(lower)))
                d = lower;
            cpu.fpu.compare(upper, d);
            eax = (eax & 0xffff0000u) | cpu.fpu.status.word;
            if (double(upper) < d)
                d = upper;
            rc::stf32(p + 0x2c, float(d + double(t)));
            pending = false;
        }
        else
        {
            // Direction out of the next point: its spline position, or the next two edge positions.
            float wx, wz;
            const float wy = 0.0f;
            if (k == count - 2)
            {
                wx = rc::ldf32(p + 0x50);
                wz = rc::ldf32(p + 0x54);
            }
            else
            {
                float bx, bz, dx, dz;
                edgePosition<true>(p + 0x40, one, bx, bz);
                edgePosition<false>(roadPoint(points, k + 2), one, dx, dz);
                wx = float(double(dx) - double(bx));
                wz = float(double(dz) - double(bz));
            }
            const double fx = wx, fz = wz, fy = wy;
            const double ax = fx + double(v60[0]);
            const double az = fz + double(v60[1]);
            const double ay = fy + double(v60[2]);
            const double lw = (fy * fy + fz * fz) + fx * fx;
            s18 = float((((ay * ay + az * az) + ax * ax) * lw) * double(s18));
            const double d18 = s18;
            const double turn = double(v80[0]) * fz - double(v80[1]) * fx;
            const double g = d18 * d5c;
            const float s38 = float(d18 * d3c);
            const double d38 = s38;
            double num = (((double(v60[1]) * fx - fz * double(v60[0])) * turn) * d54) * d1c;
            num -= (g * d58) * d1c;
            num -= (d38 * d58) * d1c;
            num += ((d18 * d50) * d34) * d54;
            double h = g * d3c;
            h += h;
            const double e = ((turn * turn) * d54 + d38 * d3c) + h;
            const double den = e * d1c + ((d50 * d50) * d54 + (d5c * d5c) * d1c) * d18;
            const float step = float(-(num / den));
            const float t = rc::ldf32(p + 0x2c);
            const float upper = float(one - double(t));
            double d = -double(t);
            // The translated code keeps the status word of this compare only.
            compareA = step;
            compareB = d;
            pending = true;
            if (double(step) > d)
                d = step;
            if (double(upper) < d)
                d = upper;
            rc::stf32(p + 0x2c, float(d + double(t)));
            const double moveX = double(v80[0]) * d;
            const float moveZ = float(double(v80[1]) * d);
            const float moveY = float(d * zero);
            const float n20[3] = { float(double(v60[0]) - moveX), float(double(v60[1]) - double(moveZ)),
                                   float(double(v60[2]) - double(moveY)) };
            v40[0] = float(moveX + double(v20[0]));
            v40[1] = float(double(moveZ) + double(v20[1]));
            v40[2] = float(double(moveY) + double(v20[2]));
            memcpy(v20, n20, sizeof(v20));
            v60[0] = wx;
            v60[1] = wz;
            v60[2] = wy;
            eax = floatBits(v20[2]);
        }
        app->allowPreemption(cpu);
    }
    if (pending)
        cpu.fpu.compare(compareA, compareB);
    cpu.eax = eax;
}

// Road spline tension pass (cdecl: points, count, start direction, end direction, first, end), 0x7f9570: for each
// point from first to end, moves its lateral parameter towards the curvature minimum of the path through the
// neighbouring edge positions, clamped to the road edges.
void sub_7f9570(win32::WinApplication* app, x86::CPU& cpu)
{
    if (shadowOn())
    {
        const x86::reg32 first = arg(cpu, 4), end = arg(cpu, 5);
        const x86::reg32 span = x86::sreg32(end) > x86::sreg32(first) ? (end - first) << 6 : 0;
        shadowCall("tension pass 0x7f9570", app, cpu, &sub_7f9570_guest, &sub_7f9570,
                   { { cpu.esp, 28 }, { roadPoint(arg(cpu, 0), first), span } });
        return;
    }
    if (!useNative())
    {
        sub_7f9570_guest(app, cpu);
        return;
    }
    tensionPass(app, cpu, arg(cpu, 0), arg(cpu, 1), arg(cpu, 2), arg(cpu, 3), arg(cpu, 4), arg(cpu, 5));
    cpu.esp += 4;
}

// Point (x, z) inside a collision triangle (cdecl: point, triangle), 0x7e83e0: al 1 inside or on an edge.
void sub_7e83e0(win32::WinApplication* app, x86::CPU& cpu)
{
    if (!useNative())
    {
        sub_7e83e0_guest(app, cpu);
        return;
    }
    const x86::reg32 point = arg(cpu, 0);
    const x86::reg32 tri = arg(cpu, 1);
    const double zero = rc::ldf32a(0x9c248c);
    double last;
    const bool inside = inTriangle(tri, rc::ldf32(point), rc::ldf32(point + 8), zero, last);
    cpu.fpu.compare(last, zero);
    cpu.eax = (cpu.eax & 0xffff0000u) | (cpu.fpu.status.word & 0xff00u) | (inside ? 1 : 0);
    cpu.ecx = tri;
    cpu.edx = point;
    cpu.esp += 4;
}

// WWorldPos closest face search (thiscall: world position, face lists, point, keep cached face), 0x7fc550: unless
// the cached face holds the point (and keep is set), finds the face under the point whose height is closest
// below it over all lists, copying it into the position; returns 1 when the cached face was not used.
void sub_7fc550(win32::WinApplication* app, x86::CPU& cpu)
{
    if (!useNative())
    {
        sub_7fc550_guest(app, cpu);
        return;
    }
    const x86::reg32 ret = cpu.esp;
    // The guest frame: the face normal at +0x1c, written by 0x403f10.
    const x86::reg32 frame = ret - 0x28;
    const x86::reg32 self = cpu.ecx;
    const x86::reg32 ebx = cpu.ebx, ebp = cpu.ebp, esi = cpu.esi, savedEdi = cpu.edi;
    x86::reg32 edi = savedEdi;
    const double zero = rc::ldf32a(0x9c248c);
    PendingCompare status;
    const x86::reg32 point = rc::ld32(ret + 8);
    rc::st16(self + 0x32, x86::reg16(rc::ld16(self + 0x32) + 1));
    const float px = rc::ldf32(point);
    const float py = rc::ldf32(point + 4);
    const x86::reg32 flags = rc::ld32(self + 0x30);
    const float pz = rc::ldf32(point + 8);
    x86::reg8 cached = 0;
    if (flags & 1)
    {
        double last;
        cached = inTriangle(self, px, pz, zero, last) ? 1 : 0;
        status.set(last, zero);
    }
    // The guest keeps the best height difference in the point argument slot, the list iterator in the next.
    rc::st32(ret + 8, 0x7e967699);
    auto finish = [&](x86::reg32 ecx, x86::reg32 edx) {
        status.apply(cpu);
        cpu.eax = cached == 0 ? 1 : 0;
        cpu.ecx = (ecx & 0xffffff00u) | cached;
        cpu.edx = edx;
        cpu.ebx = ebx;
        cpu.ebp = ebp;
        cpu.esi = esi;
        cpu.edi = savedEdi;
        cpu.esp = ret + 0x10;
    };
    if (cached && rc::ld8(ret + 0xc))
    {
        finish(self, frame + 0x10);
        return;
    }
    const float height = float(double(py) + double(rc::ldf32(self + 0x34)));
    x86::reg32 lists = rc::ld32(ret + 4);
    rc::st32(self + 0x30, flags & ~1u);
    x86::reg32 it = rc::ld32(lists);
    x86::reg32 listsEnd = rc::ld32(lists + 4);
    rc::st32(ret + 0xc, it);
    if (it == listsEnd)
    {
        finish(lists, listsEnd);
        return;
    }
    const double lowest = rc::ldf32a(0x9c2914);
    bool found = false;
    for (;;)
    {
        if (found && !(rc::ld8(self + 0x2d) & 4))
            break;
        const x86::reg32 list = rc::ld32(it);
        x86::reg32 face = rc::ld32(list);
        if (face != rc::ld32(list + 4))
        {
            for (;;)
            {
                double last;
                const bool inside = inTriangle(face, px, pz, zero, last);
                status.set(last, zero);
                if (inside)
                {
                    // Face normal (thiscall: face, out) into the guest frame.
                    rc::st32(frame - 0xc, frame + 0x1c);
                    status.apply(cpu);
                    cpu.eax = frame + 0x1c;
                    cpu.ecx = face;
                    cpu.edx = frame + 0x10;
                    cpu.ebx = face;
                    cpu.ebp = self;
                    cpu.esi = list;
                    cpu.edi = edi;
                    cpu.esp = frame - 0x10;
                    sub_403f10(app, cpu);
                    if (cpu.terminate)
                        return;
                    const double ny = rc::ldf32(frame + 0x20);
                    status.set(ny, zero);
                    double y = rc::ldf32(face + 4);
                    if (!(ny == zero))
                    {
                        const double dz = (double(pz) - double(rc::ldf32(face + 8))) * double(rc::ldf32(frame + 0x24));
                        const double dx = (double(px) - double(rc::ldf32(face))) * double(rc::ldf32(frame + 0x1c));
                        y = double(rc::ldf32(face + 4)) - (dz + dx) / ny;
                    }
                    const double below = double(height) - y;
                    const double best = rc::ldf32(ret + 8);
                    status.set(below, best);
                    if (below < best && below > lowest)
                    {
                        const x86::reg32 marked = rc::ld32(self + 0x30) | 1;
                        rc::stf32(ret + 8, float(below));
                        rc::st32(self + 0x30, marked);
                        // rep movsd of the 48-byte face.
                        const x86::reg32 step = cpu.flags.df ? 0u - 4u : 4u;
                        x86::reg32 from = face, to = self;
                        for (int n = 0; n < 12; ++n, from += step, to += step)
                            rc::st32(cpu.ees + to, rc::ld32(from));
                        edi = to;
                        const bool more = (rc::ld8(self + 0x2d) & 4) != 0;
                        rc::st32(self + 0x38, rc::ld32(face + 0xc));
                        found = true;
                        if (!more)
                            break;
                    }
                }
                face += 0x30;
                if (face == rc::ld32(list + 4))
                    break;
                app->allowPreemption(cpu);
            }
        }
        it += 4;
        lists = rc::ld32(ret + 4);
        listsEnd = rc::ld32(lists + 4);
        rc::st32(ret + 0xc, it);
        if (it == listsEnd)
            break;
        app->allowPreemption(cpu);
    }
    finish(listsEnd, lists);
}

// Next triangle of a packed collision strip (thiscall: entries, index, origin, triangle), 0x7e7d40: the last two
// vertices move down, the third is the entry's 16-bit coordinates times the scale at 0xa7ad44 plus origin, then the
// entry's 16-bit flags.
void sub_7e7d40(win32::WinApplication* app, x86::CPU& cpu)
{
    if (!useNative())
    {
        sub_7e7d40_guest(app, cpu);
        return;
    }
    const x86::reg32 index = arg(cpu, 0);
    const x86::reg32 origin = arg(cpu, 1);
    const x86::reg32 out = arg(cpu, 2);
    for (x86::reg32 k = 0; k < 12; k += 4)
        rc::st32(out + k, rc::ld32(out + 0x10 + k));
    for (x86::reg32 k = 0; k < 12; k += 4)
        rc::st32(out + 0x10 + k, rc::ld32(out + 0x20 + k));
    const x86::reg32 entry = cpu.ecx + index * 8 + 0x10;
    x86::sreg32 coordinate = 0;
    for (x86::reg32 k = 0; k < 3; ++k)
    {
        coordinate = x86::sreg16(rc::ld16(entry + 2 * k));
        const double scale = rc::ldf32(0xa7ad44);
        rc::stf32(out + 0x20 + 4 * k, float(double(coordinate) * scale + double(rc::ldf32(origin + 4 * k))));
    }
    const x86::reg16 flags = x86::reg16(rc::ld8(entry + 6) | rc::ld8(entry + 7) << 8);
    // The guest builds the flags in the triangle argument slot, over the last coordinate.
    rc::st32(cpu.esp + 12, (x86::reg32(coordinate) & 0xffff0000u) | flags);
    rc::st16(out + 0x2c, flags);
    cpu.eax = out;
    cpu.edx = (index & 0xffff0000u) | flags;
    cpu.esp += 16;
}

// Squared distance (x, z) from a point to a segment (thiscall: segment, point), 0x7e7eb0: the segment's end points
// at +0 and +0x10, its inverse squared length at +0x1c; returned in st0.
void sub_7e7eb0(win32::WinApplication* app, x86::CPU& cpu)
{
    if (!useNative())
    {
        sub_7e7eb0_guest(app, cpu);
        return;
    }
    const x86::reg32 segment = cpu.ecx;
    const x86::reg32 slot = cpu.esp + 4;
    const double w = rc::ldf32(segment + 0x1c);
    const float ax = rc::ldf32(segment), az = rc::ldf32(segment + 8);
    const float bx = rc::ldf32(segment + 0x10), bz = rc::ldf32(segment + 0x18);
    const x86::reg32 point = rc::ld32(slot);
    const float px = rc::ldf32(point);
    const x86::reg32 pzBits = rc::ld32(point + 8);
    const float pz = rc::ldf32(point + 8);
    const float dz = float(double(bz) - double(az));
    const float dx = float(double(bx) - double(ax));
    const float t = float((((double(px) - double(ax)) * double(dx) + (double(pz) - double(az)) * double(dz)) * w) * w);
    rc::stf32(slot, t);
    const double zero = rc::ldf32a(0x9c248c);
    cpu.fpu.compare(t, zero);
    double result;
    auto squared = [](double x, double z) { return x * x + z * z; };
    if (double(t) < zero)
    {
        result = squared(double(ax) - double(px), double(az) - double(pz));
    }
    else
    {
        const double one = rc::ldf32a(0x9c2478);
        cpu.fpu.compare(t, one);
        if (double(t) > one)
        {
            result = squared(double(bx) - double(px), double(bz) - double(pz));
        }
        else
        {
            const double x = (double(dx) * double(t) + double(ax)) - double(px);
            const float z = float((double(dz) * double(t) + double(az)) - double(pz));
            rc::stf32(slot, z);
            result = x * x + double(z) * double(z);
        }
    }
    cpu.eax = (pzBits & 0xffff0000u) | cpu.fpu.status.word;
    cpu.esp += 8;
    cpu.fpu.regs[(cpu.fpu.count + 1) & 7] = result;
    cpu.fpu.count = cpu.fpu.count + 1;
}

// Closest road point passed (thiscall: driver, position, points or 0 for the road ring buffer, count, unused),
// 0x7f8930: the index of the nearest point where the position's side of the point's direction changes (the first
// point counts when the position is behind it), or -1.
void sub_7f8930(win32::WinApplication* app, x86::CPU& cpu)
{
    if (shadowOn())
    {
        shadowCall("closest point passed 0x7f8930", app, cpu, &sub_7f8930_guest, &sub_7f8930, { { cpu.esp, 20 } });
        return;
    }
    if (!useNative())
    {
        sub_7f8930_guest(app, cpu);
        return;
    }
    const x86::reg32 entry = cpu.esp;
    const x86::reg32 self = cpu.ecx;
    const x86::reg32 count = arg(cpu, 2);
    x86::reg32 best = 0xffffffffu;
    x86::reg32 edx = cpu.edx;
    if (x86::sreg32(count) > 0)
    {
        const x86::reg32 position = arg(cpu, 0);
        const float px = rc::ldf32(position), pz = rc::ldf32(position + 8);
        const double zero = rc::ldf32a(0x9c248c);
        float nearest = -1.0f, previous = 0.0f;
        PendingCompare status;
        for (x86::reg32 i = 0; x86::sreg32(i) < x86::sreg32(count); ++i)
        {
            const x86::reg32 array = arg(cpu, 1);
            x86::reg32 p;
            if (array)
            {
                edx = array;
                p = array + (i << 6);
            }
            else
            {
                const x86::reg32 ring = rc::ld32(self + 0xc);
                edx = ringOffset(ring, i, rc::ld32(ring), rc::ld32(ring + 8));
                p = edx + ring + 0x10;
            }
            const float dx = float(double(px) - double(rc::ldf32(p + 0x30)));
            const float dz = float(double(pz) - double(rc::ldf32(p + 0x38)));
            const double dot = double(dz) * double(rc::ldf32(p + 0x14)) + double(dx) * double(rc::ldf32(p + 0x10));
            const double distance2 = double(dx) * double(dx) + double(dz) * double(dz);
            if (i == 0)
            {
                if (dot < zero)
                {
                    nearest = float(distance2);
                    best = 0;
                }
            }
            else
            {
                const double side = dot * double(previous);
                status.set(side, zero);
                if (side < zero && (x86::sreg32(best) < 0 || distance2 < double(nearest)))
                {
                    nearest = float(distance2);
                    best = i;
                }
            }
            previous = float(dot);
            app->allowPreemption(cpu);
        }
        status.apply(cpu);
    }
    cpu.eax = best;
    cpu.ecx = best;
    cpu.edx = edx;
    cpu.esp = entry + 20;
}

// AI best line curvature ahead (thiscall: driver, position, velocity), 0x7fb180: the largest weighted curvature of
// the road ring buffer's edge positions ahead, returned in st0 (0 without a road).
void sub_7fb180(win32::WinApplication* app, x86::CPU& cpu)
{
    if (shadowOn())
    {
        shadowCall("best line curvature 0x7fb180", app, cpu, &sub_7fb180_guest, &sub_7fb180, { { cpu.esp, 12 } });
        return;
    }
    if (!useNative())
    {
        sub_7fb180_guest(app, cpu);
        return;
    }
    const x86::reg32 entry = cpu.esp;
    const x86::reg32 self = cpu.ecx;
    const x86::reg32 depth = cpu.fpu.count;
    auto finish = [&](double result) {
        cpu.esp = entry + 12;
        cpu.fpu.regs[(depth + 1) & 7] = result;
        cpu.fpu.count = depth + 1;
    };
    const x86::reg32 road = rc::ld32(self + 0xc);
    if (road == 0)
    {
        finish(rc::ldf32a(0x9c248c));
        return;
    }
    const x86::reg32 start = rc::ld32(self + 4);
    const x86::reg32 span = rc::ld32(road) - start;
    if (x86::sreg32(span) <= 1)
    {
        cpu.edx = span;
        finish(rc::ldf32a(0x9c248c));
        return;
    }
    const double one = rc::ldf32a(0x9c2478);
    const x86::reg32 position = arg(cpu, 0);
    float px = rc::ldf32(position), pz = rc::ldf32(position + 8);
    x86::reg32 edx = ringOffset(road, start, rc::ld32(road), rc::ld32(road + 8));
    // The guest keeps the largest curvature in the position argument slot, the speed factor in the next.
    rc::st32(entry + 4, 0);
    float cx, cz;
    edgePosition<true>(road + 0x10 + edx, one, cx, cz);
    const x86::reg32 velocity = arg(cpu, 1);
    const double vx = rc::ldf32(velocity), vy = rc::ldf32(velocity + 4), vz = rc::ldf32(velocity + 8);
    const float speed2 = float((vx * vx + vy * vy) + vz * vz);
    rc::stf32(entry + 8, speed2);
    // Registers as the guest leaves them at its square root call.
    cpu.eax = floatBits(speed2);
    cpu.ecx = road;
    const double speed = std::sqrt(double(speed2)) * double(rc::ldf32a(0x9f2e88));
    const float factor = one > speed ? 1.0f : float(speed);
    rc::stf32(entry + 8, factor);
    const x86::reg32 ring = rc::ld32(self + 0xc);
    const x86::reg32 first = rc::ld32(self + 4) + 1;
    const x86::reg32 end = rc::ld32(ring) - 2;
    float largest = 0.0f;
    if (x86::sreg32(first) < x86::sreg32(end))
    {
        const double weight = rc::ldf32a(0x9c2888);
        const x86::reg32 capacity = rc::ld32(ring + 8);
        const x86::reg32 count = rc::ld32(ring);
        float travelled = 0.0f, previous = 0.0f;
        for (x86::reg32 i = first; x86::sreg32(i) < x86::sreg32(end); ++i)
        {
            float nx, nz;
            edgePosition<true>(ring + 0x10 + ringOffset(ring, i, count, capacity), one, nx, nz);
            const double d1 = double(cx) - double(px), d2 = double(cz) - double(pz);
            const double d3 = double(nx) - double(cx), d4 = double(nz) - double(cz);
            const double d5 = double(nx) - double(px), d6 = double(nz) - double(pz);
            const double scale = factor;
            const double divisor = scale > double(travelled) ? scale : double(travelled);
            const float ratio = float(scale / divisor);
            const float chord2 = float(d6 * d6 + d5 * d5);
            const double cross = d4 * d1 - d2 * d3;
            const double norms = ((d4 * d4 + d3 * d3) * (d2 * d2 + d1 * d1)) * double(chord2);
            double curvature = cross / std::sqrt(norms);
            curvature += curvature;
            const double term = (((double(previous) + curvature) * weight) * double(ratio)) * double(ratio);
            if (std::fabs(double(largest)) < std::fabs(term))
                largest = float(term);
            travelled = float(std::sqrt(double(chord2)) * weight + double(travelled));
            previous = float(curvature);
            px = cx;
            pz = cz;
            cx = nx;
            cz = nz;
            edx = floatBits(nz);
            app->allowPreemption(cpu);
        }
        // The guest's following test (zero above a positive value) never clears it.
        cpu.fpu.compare(largest, rc::ldf32a(0x9c248c));
        rc::stf32(entry + 4, largest);
    }
    cpu.edx = edx;
    finish(double(largest) * double(rc::ldf32a(0x9f2e68)));
}

// The smoothing system of the points first..end (rows = end - first, 1 to 68) as 0x7f8bd0 accumulates it: the band
// matrix m (rows x 5) and the right-hand side rhs. Returns edi as the translated body leaves it in its frame.
static x86::reg32 buildSystem(win32::WinApplication* app, x86::CPU& cpu, x86::reg32 points, x86::reg32 count,
                              x86::reg32 a, x86::reg32 b, x86::reg32 first, x86::reg32 end, x86::reg32 frame,
                              float* m, float* rhs)
{
    using x86::sreg32;
    const x86::reg32 rows = end - first;
    const double one = rc::ldf32a(0x9c2478);
    for (x86::reg32 i = 0; i < rows * 5; ++i)
        m[i] = 0.0f;
    auto add = [m](x86::reg32 i, double v) { m[i] = float(v + double(m[i])); };
    auto sub = [rhs](x86::reg32 i, double v) { rhs[i] = float(double(rhs[i]) - v); };
    for (x86::reg32 i = 0; i < rows; ++i)
        rhs[i] = 0.0f;
    const float a0 = rc::ldf32(a), a1 = rc::ldf32(a + 4);
    const float b0 = rc::ldf32(b), b1 = rc::ldf32(b + 4);
    // First point: against the start position and direction, or the two points before it.
    float s10;
    if (first == 0)
    {
        float x;
        double z;
        edgeExact<false>(points, one, x, z);
        const double dx = double(x) - double(a0);
        const double dz = z - double(a1);
        const double ex = double(rc::ldf32(points + 8)) - double(rc::ldf32(points));
        const double ez = double(rc::ldf32(points + 0xc)) - double(rc::ldf32(points + 4));
        const double len = std::sqrt(double(b1) * double(b1) + double(b0) * double(b0));
        const float s14 = float(one / (len * (dx * dx + dz * dz)));
        s10 = float((double(b1) * ex - ez * double(b0)) * double(s14));
        sub(0, ((double(b1) * dx - dz * double(b0)) * double(s14)) * double(s10));
    }
    else
    {
        float cx = a0, cz = a1;
        const float cy = 0.0f;
        if (first != 1)
            edgePosition<true>(roadPoint(points, first - 2), one, cx, cz);
        const x86::reg32 p = roadPoint(points, first);
        float x1;
        double z1;
        edgeExact<true>(p - 0x40, one, x1, z1);
        const double dx = double(x1) - double(cx);
        const double dz = z1 - double(cz);
        const double dy = -double(cy);
        float x2, z2, x3, z3;
        edgePosition<false>(p - 0x40, one, x2, z2);
        edgePosition<true>(p, one, x3, z3);
        const float s20 = float(double(x3) - double(x2));
        const float s24 = float(double(z3) - double(z2));
        const double sx = double(s20) + dx;
        const float s44 = float(double(s24) + dz);
        const float s48 = float(dy);
        float ex, ez;
        edgeVector(p, ex, ez);
        double q = (dy * dy + dx * dx) + dz * dz;
        q *= (sx * sx + double(s48) * double(s48)) + double(s44) * double(s44);
        q *= double(s20) * double(s20) + double(s24) * double(s24);
        const double inv = one / double(float(std::sqrt(q)));
        s10 = float((dz * double(ex) - double(ez) * dx) * inv);
        sub(0, ((dz * double(s20) - double(s24) * dx) * inv) * double(s10));
    }
    add(2, double(s10) * double(s10));
    // edi as the guest leaves it for the solver calls.
    x86::reg32 edi = first;
    for (x86::reg32 c = 0; sreg32(c) < sreg32(rows); ++c)
    {
        const x86::reg32 k = c + first;
        edi = count - 1;
        if (k == edi)
            break;
        float vx = a0, vz = a1;
        const float vy = 0.0f;
        if (k != 0)
            edgePosition<false>(roadPoint(points, k - 1), one, vx, vz);
        const x86::reg32 p = roadPoint(points, k);
        float x0, xa, xb;
        double z0, za, zb;
        edgeExact<true>(p, one, x0, z0);
        const float s60 = float(double(x0) - double(vx));
        const float s64 = float(z0 - double(vz));
        const double y = -double(vy);
        const float s68 = float(y);
        edgeExact<true>(p, one, xa, za);
        edgeExact<true>(p + 0x40, one, xb, zb);
        const float s50 = float(double(xb) - double(xa));
        const float s54 = float(zb - za);
        const float s20 = float(double(s50) + double(s60));
        const float s24 = float(double(s54) + double(s64));
        const float s28 = float(y);
        float w1x = 0.0f, w1z = 0.0f, s40, s44, w2x = 0.0f, w2z = 0.0f;
        if (c != 0)
            edgeVector(p - 0x40, w1x, w1z);
        edgeVector(p, s40, s44);
        if (c != rows - 1)
            edgeVector(p + 0x40, w2x, w2z);
        edi = c == 0 ? frame + 0x100 : frame + 0xd0;
        auto d = [](float v) { return double(v); };
        double q = (d(s20) * d(s20) + d(s28) * d(s28)) + d(s24) * d(s24);
        q *= (d(s60) * d(s60) + d(s68) * d(s68)) + d(s64) * d(s64);
        q *= d(s50) * d(s50) + d(s54) * d(s54);
        const double inv = one / std::sqrt(q);
        const float s18 = float((d(s64) * d(s50) - d(s54) * d(s60)) * inv);
        const float s14 = float((d(s44) * d(s20) - d(s24) * d(s40)) * inv);
        const float s1c = float((d(s54) * d(w1x) - d(w1z) * d(s50)) * inv);
        const float sn = float((d(s64) * d(w2x) - d(w2z) * d(s60)) * inv);
        const float sac = float((d(s44) * d(w2x) - d(w2z) * d(s40)) * inv);
        const float s8c = float((d(w1z) * d(s40) - d(s44) * d(w1x)) * inv);
        const float s3c = float((d(w2z) * d(w1x) - d(w1z) * d(w2x)) * inv);
        if (sreg32(c) > 0)
        {
            const x86::reg32 r = 5 * (c - 1);
            sub(c - 1, d(s1c) * d(s18));
            add(r + 2, d(s1c) * d(s1c));
            add(r + 3, d(s8c) * d(s18) + d(s1c) * d(s14));
            add(r + 4, d(s3c) * d(s18) + d(sn) * d(s1c));
        }
        const x86::reg32 r = 5 * c;
        sub(c, d(s14) * d(s18));
        add(r + 1, d(s8c) * d(s18) + d(s1c) * d(s14));
        add(r + 2, d(s14) * d(s14));
        const double t1 = d(sn) * d(s14) + d(sac) * d(s18);
        add(r + 3, t1);
        if (sreg32(c) < sreg32(rows - 1))
        {
            sub(c + 1, d(sn) * d(s18));
            add(r + 5, d(s3c) * d(s18) + d(sn) * d(s1c));
            add(r + 6, t1);
            add(r + 7, d(sn) * d(sn));
        }
        app->allowPreemption(cpu);
    }
    // Last point: against the two points after it.
    if (sreg32(end) < sreg32(count - 1))
    {
        const x86::reg32 p = roadPoint(points, end);
        float xm, x0, xb, xp;
        double zm, z0, zb, zp;
        edgeExact<true>(p - 0x40, one, xm, zm);
        edgeExact<true>(p, one, x0, z0);
        const float s40 = float(double(x0) - double(xm));
        const double d1 = z0 - zm;
        edgeExact<true>(p, one, xb, zb);
        edgeExact<true>(p + 0x40, one, xp, zp);
        const float s70 = float(double(xp) - double(xb));
        const double d2 = zp - zb;
        const double sx = double(s70) + double(s40);
        const double sz = d2 + d1;
        float s90, s94;
        edgeVector(p - 0x40, s90, s94);
        double q = sx * sx + sz * sz;
        q *= double(s70) * double(s70) + d2 * d2;
        q *= double(s40) * double(s40) + d1 * d1;
        const double inv = one / std::sqrt(q);
        const float st = float((d2 * double(s90) - double(s94) * double(s70)) * inv);
        sub(rows - 1, ((d1 * double(s70) - d2 * double(s40)) * inv) * double(st));
        add(5 * (rows - 1) + 2, double(st) * double(st));
    }
    return edi;
}

// Road spline smoothing system (cdecl: right-hand side, points, count, start position, start direction, first,
// end), 0x7f8bd0: accumulates the curvature terms of the points first..end into a five-band matrix in the guest
// frame and the right-hand side, then reduces (0x7ec920) and solves (0x7ecc70) it.
void sub_7f8bd0(win32::WinApplication* app, x86::CPU& cpu)
{
    using x86::sreg32;
    const x86::reg32 first = arg(cpu, 5);
    const x86::reg32 end = arg(cpu, 6);
    const x86::reg32 rows = end - first;
    if (shadowOn())
    {
        // The right-hand side from the element before it (the last point's term without rows) to the element after
        // the last row (the solve's band with a single row).
        const x86::reg32 written = sreg32(rows) >= 0 && sreg32(rows) <= 68 ? 4 * (rows + 2) : 12;
        shadowCall("smoothing system 0x7f8bd0", app, cpu, &sub_7f8bd0_guest, &sub_7f8bd0,
                   { { cpu.esp, 32 }, { arg(cpu, 0) - 4, written } });
        return;
    }
    // Row counts outside the guest frame's matrix (68 rows of 5) take the translated body.
    if (!useNative() || sreg32(rows) < 1 || sreg32(rows) > 68)
    {
        sub_7f8bd0_guest(app, cpu);
        return;
    }
    const x86::reg32 entry = cpu.esp;
    const x86::reg32 framePointer = entry - 4;
    const x86::reg32 frame = (framePointer & ~0xfu) - 0xbd4 - 12;
    const x86::reg32 out = arg(cpu, 0);
    const x86::reg32 points = arg(cpu, 1);
    const x86::reg32 ebx = cpu.ebx, ebp = cpu.ebp, esi = cpu.esi;
    float m[340], rhs[68];
    const x86::reg32 edi = buildSystem(app, cpu, points, arg(cpu, 2), arg(cpu, 3), arg(cpu, 4), first, end, frame, m,
                                       rhs);
    if (cpu.terminate)
        return;
    for (x86::reg32 i = 0; i < rows; ++i)
        rc::stf32(out + 4 * i, rhs[i]);
    // The matrix (rows, stride 5, half band 2) in the guest frame, then reduce (system, out) and solve (system,
    // reduced, right-hand side).
    for (x86::reg32 i = 0; i < rows * 5; ++i)
        rc::stf32(frame + 0x120 + 4 * i, m[i]);
    rc::st32(frame + 0x670, rows);
    rc::st32(frame + 0x674, 5);
    rc::st32(frame + 0x678, 2);
    rc::st32(frame - 4, frame + 0x680);
    rc::st32(frame - 8, frame + 0x120);
    cpu.eax = frame + 0x120;
    cpu.ebx = points;
    cpu.esp = frame - 12;
    cpu.ebp = framePointer;
    cpu.esi = out;
    cpu.edi = edi;
    native::sub_7ec920(app, cpu);
    if (cpu.terminate)
        return;
    if (roadnavNative())
    {
        solveSystem(cpu, frame + 0x120, frame + 0x680, out);
        cpu.ebx = ebx;
        cpu.ebp = ebp;
        cpu.esi = esi;
        cpu.esp = entry + 4;
        return;
    }
    rc::st32(frame - 12, out);
    rc::st32(frame - 16, frame + 0x680);
    rc::st32(frame - 20, frame + 0x120);
    cpu.ecx = frame + 0x680;
    cpu.edx = frame + 0x120;
    cpu.esp = frame - 24;
    sub_7ecc70(app, cpu);
    if (cpu.terminate)
        return;
    cpu.ebx = ebx;
    cpu.ebp = ebp;
    cpu.esi = esi;
    cpu.esp = entry + 4;
}

// 0x7ec920 on the smoothing system in host memory (stride 5, half band 2): rows 0 and 1 shifted left by 2 and 1,
// then the first column eliminated below each pivot (a zero pivot becomes 1e-20), keeping the factors.
static void reduceHost(float* m, x86::sreg32 rows, float* factors)
{
    m[0] = m[2];
    m[1] = m[3];
    m[2] = m[4];
    m[3] = 0.0f;
    m[4] = 0.0f;
    if (rows > 1)
    {
        m[5] = m[6];
        m[6] = m[7];
        m[7] = m[8];
        m[8] = m[9];
        m[9] = 0.0f;
    }
    x86::sreg32 limit = 2;
    for (x86::sreg32 r = 0; r < rows; ++r)
    {
        float* pivot = m + 5 * r;
        if (limit < rows)
            ++limit;
        if (double(pivot[0]) == 0.0)
            pivot[0] = 1e-20f;
        for (x86::sreg32 s = r + 1; s < limit && s < rows; ++s)
        {
            float* row = m + 5 * s;
            const float f = float(double(row[0]) / double(pivot[0]));
            factors[r + s - 1] = f;
            for (int q = 0; q < 4; ++q)
                row[q] = float(double(row[q + 1]) - double(f) * double(pivot[q + 1]));
            row[4] = 0.0f;
        }
    }
}

// 0x7ecc70 on the host system: forward elimination of rhs with the factors, then back substitution.
static void solveHost(const float* m, x86::sreg32 rows, const float* factors, float* rhs)
{
    x86::sreg32 band = 2;
    for (x86::sreg32 i = 0; i < rows; ++i)
    {
        if (band < rows)
            ++band;
        const double pivot = rhs[i];
        for (x86::sreg32 j = i + 1; j < band && j < rows; ++j)
            rhs[j] = float(double(rhs[j]) - double(factors[i - 1 + j]) * pivot);
    }
    x86::sreg32 terms = 1;
    for (x86::sreg32 i = rows - 1; i >= 0; --i)
    {
        const float* row = m + 5 * i;
        double sum = rhs[i];
        for (x86::sreg32 j = 1; j < terms; ++j)
            sum -= double(row[j]) * double(rhs[i + j]);
        rhs[i] = float(sum / double(row[0]));
        if (terms < 5)
            ++terms;
    }
}

// Road spline smoothing pass (cdecl: points, count, start position, start direction), 0x7f9d90: splits the points
// into runs at pinned parameters (0, 1 or the last point); pinned runs get the tension pass (0x7f9570), free runs
// move along the smoothing system's solution (0x7f8bd0) by the largest step that keeps the parameters in range.
void sub_7f9d90(win32::WinApplication* app, x86::CPU& cpu)
{
    using x86::sreg32;
    const x86::reg32 points = arg(cpu, 0);
    const x86::reg32 count = arg(cpu, 1);
    if (shadowOn())
    {
        shadowCall("smoothing pass 0x7f9d90", app, cpu, &sub_7f9d90_guest, &sub_7f9d90,
                   { { cpu.esp, 20 }, { points, sreg32(count) > 0 ? count << 6 : 0 } }, LiveEaxEbxEbp);
        return;
    }
    if (!useNative() || !roadnav2Native())
    {
        sub_7f9d90_guest(app, cpu);
        return;
    }
    const double zero = rc::ldf32a(0x9c248c);
    auto param = [points](x86::reg32 i) { return roadPoint(points, i) + 0x2c; };
    auto pinned = [&](x86::reg32 i) {
        return double(rc::ldf32(param(i))) == zero || rc::ld32(param(i)) == 0x3f800000u;
    };
    // End of the run from i: a free run ends at the next pinned point, a pinned run at the next free one.
    auto runEnd = [&](x86::reg32 i, bool& free) {
        free = !(pinned(i) || i == count - 1 || pinned(i + 1));
        x86::reg32 k = i + 1;
        if (free)
        {
            while (k != count && !pinned(k))
                ++k;
        }
        else
        {
            while (k != count && (pinned(k) || k == count - 1 || pinned(k + 1)))
                ++k;
        }
        return k;
    };
    // The runs depend only on parameters ahead of the one being moved; free runs longer than the guest's system
    // (68 rows) take the translated body.
    for (x86::reg32 i = 0; sreg32(i) < sreg32(count);)
    {
        bool free;
        const x86::reg32 k = runEnd(i, free);
        if (free && sreg32(k - i) > 68)
        {
            sub_7f9d90_guest(app, cpu);
            return;
        }
        i = k;
    }
    const x86::reg32 entry = cpu.esp;
    const x86::reg32 a = arg(cpu, 2);
    const x86::reg32 b = arg(cpu, 3);
    const double one = rc::ldf32a(0x9c2478);
    const double eps = rc::ldf32a(0x9c28c8);
    const double below = rc::ldf32a(0x9eea80);
    const double top = rc::ldf32a(0x9d756c);
    const double lowest = rc::ldf32a(0x9c2914);
    const double margin = rc::ldf32a(0x9e245c);
    const float initial = rc::ldf32a(0x9f2ea0);
    float m[340], rhs[68], factors[136];
    PendingCompare status;
    bool lastFree = false;
    x86::reg32 high = 0;
    for (x86::reg32 i = 0; sreg32(i) < sreg32(count);)
    {
        bool free;
        const x86::reg32 k = runEnd(i, free);
        lastFree = free;
        if (!free)
        {
            status.apply(cpu);
            tensionPass(app, cpu, points, count, a, b, i, k);
        }
        else
        {
            const sreg32 rows = sreg32(k - i);
            buildSystem(app, cpu, points, count, a, b, i, k, 0, m, rhs);
            if (cpu.terminate)
                return;
            reduceHost(m, rows, factors);
            solveHost(m, rows, factors, rhs);
            double step = initial;
            for (sreg32 j = 0; j < rows; ++j)
            {
                const double v = rhs[j];
                const double t = rc::ldf32(param(i + j));
                double limit;
                if (v > eps)
                {
                    const float inverse = float(one / v);
                    const float room = float((one - t) * double(inverse));
                    if (!(step < double(room)))
                        step = room;
                    limit = inverse;
                }
                else if (v < below)
                {
                    const float room = float(-t / v);
                    if (!(step < double(room)))
                        step = room;
                    limit = lowest / v;
                }
                else
                {
                    continue;
                }
                const float capped = float(limit * margin);
                if (!(step < double(capped)))
                    step = capped;
            }
            for (sreg32 j = 0; j < rows; ++j)
            {
                const x86::reg32 p = param(i + j);
                const float moved = float(step * double(rhs[j]) + double(rc::ldf32(p)));
                rc::stf32(p, moved);
                status.set(moved, eps);
                if (double(moved) < eps)
                {
                    rc::st32(p, 0);
                }
                else
                {
                    status.set(moved, top);
                    if (double(moved) > top)
                        rc::st32(p, 0x3f800000u);
                }
            }
            // eax: the high half of the last loop's point offset, else of the solver's.
            const x86::reg32 tail = x86::reg32(rows) & ~3u;
            high = (rows & 3) ? (tail + i) << 6 : rows >= 5 ? entry - 0x114 + 20 : 0;
        }
        if (cpu.terminate)
            return;
        i = k;
        app->allowPreemption(cpu);
    }
    status.apply(cpu);
    if (lastFree)
        cpu.eax = (high & 0xffff0000u) | cpu.fpu.status.word;
    cpu.esp = entry + 4;
}

// Calls a guest address (method null) or a native entry as the guest calls it with its stack at top: the arguments
// (first lowest) below top, the return address below them; runs on a copy of cpu.
static x86::CPU frameCall(win32::WinApplication* app, const x86::CPU& cpu, x86::reg32 top, x86::reg32 ret,
                          win32::MethodPtr method, x86::reg32 address, x86::reg32 ecx,
                          std::initializer_list<x86::reg32> arguments)
{
    x86::CPU guest = cpu;
    x86::reg32 slot = top - 4 * x86::reg32(arguments.size());
    guest.esp = slot - 4;
    guest.ecx = ecx;
    for (x86::reg32 argument : arguments)
    {
        rc::st32(slot, argument);
        slot += 4;
    }
    rc::st32(guest.esp, ret);
    if (method)
        method(app, guest);
    else
        app->dynamic_call(address, guest);
    return guest;
}

// bNormalize of (x, z), 0x46f690: (1, 0) for a zero length.
static void normalize2(float x, float z, double zero, double one, float& outX, float& outZ)
{
    const float length = float(std::sqrt(double(z) * double(z) + double(x) * double(x)));
    if (double(length) == zero)
    {
        outX = 1.0f;
        outZ = 0.0f;
        return;
    }
    const double inverse = one / double(length);
    outX = float(inverse * double(x));
    outZ = float(inverse * double(z));
}

// WRoadNav::UpdateOccludedPosition (thiscall: navigator, apply cookies, smooth; ret 8), 0x8088e0: finds the road
// point the vehicle passed, interpolates the navigator's position and lane between it and the next, copies the
// road ahead into the frame and either smooths it (tension pass, smoothing pass, parameters written back to the
// road) or searches it for the point where the view of the road is occluded.
void sub_8088e0(win32::WinApplication* app, x86::CPU& cpu)
{
    using x86::sreg32;
    const x86::reg32 self = cpu.ecx;
    const x86::reg32 entry = cpu.esp;
    if (shadowOn())
    {
        const x86::reg32 ring = rc::ld32(self + 0xc);
        const x86::reg32 capacity = ring ? rc::ld32(ring + 8) : 0;
        shadowCall("occluded position 0x8088e0", app, cpu, &sub_8088e0_guest, &sub_8088e0,
                   { { entry, 12 }, { self, 0x16c }, { ring, capacity <= 1024 ? 0x10 + (capacity << 6) : 0 } },
                   CalleeSaved);
        return;
    }
    if (!useNative() || !roadnav2Native())
    {
        sub_8088e0_guest(app, cpu);
        return;
    }
    auto finish = [&]() { cpu.esp = entry + 12; };
    // The guest frame (esp after the prologue): the road copy at +0xb0, the vehicle's vector at +0xa4, the
    // smoothing's start position at +0x70 and direction at +0x2c.
    const x86::reg32 frame = ((entry - 4) & ~0xfu) - 0x11e4 - 12;
    const x86::reg32 points = frame + 0xb0;
    const double zero = rc::ldf32a(0x9c248c);
    const double one = rc::ldf32a(0x9c2478);
    const double eps = rc::ldf32a(0x9c28c8);
    if (rc::ld8(self + 0x5d) == 0)
        return finish();
    const x86::reg32 owner = rc::ld32(self + 0x80);
    rc::st32(self + 0x64, 0);
    rc::st32(self + 0x68, 0);
    rc::st8(self + 0x5e, 0);
    rc::st32(self + 0x6c, 0);
    if (owner == 0)
        return finish();
    // Virtual calls (thiscall, no arguments unless given) returning eax.
    auto method = [&](x86::reg32 object, x86::reg32 offset, x86::reg32 ret,
                      std::initializer_list<x86::reg32> arguments) {
        const x86::CPU r = frameCall(app, cpu, frame, ret, nullptr, rc::ld32(rc::ld32(object) + offset), object,
                                     arguments);
        if (r.terminate)
            cpu.terminate = true;
        return r.eax;
    };
    const x86::reg32 ai = method(owner + 0x44, 4, 0x808925, {});
    if (cpu.terminate || ai == 0)
        return finish();
    const x86::reg32 vehicle = method(ai, 0x54, 0x808934, {});
    if (cpu.terminate || vehicle == 0)
        return finish();
    const x86::reg32 count = rc::ld32(rc::ld32(self + 0xc));
    if (sreg32(count) <= 0)
        return finish();
    method(vehicle, 0x34, 0x808962, { frame + 0xa4 });
    if (cpu.terminate)
        return;
    const x86::reg32 velocity = method(vehicle, 0x24, 0x808969, {});
    if (cpu.terminate)
        return;
    const float v80 = rc::ldf32(velocity), v84 = rc::ldf32(velocity + 4), v88 = rc::ldf32(velocity + 8);
    const x86::reg32 position = method(vehicle, 0x20, 0x80898d, {});
    if (cpu.terminate)
        return;
    const double a4 = rc::ldf32(frame + 0xa4), a8 = rc::ldf32(frame + 0xa8), ac = rc::ldf32(frame + 0xac);
    float px = float(a4 * zero + double(rc::ldf32(position)));
    float py = float(a8 * zero + double(rc::ldf32(position + 4)));
    float pz = float(ac * zero + double(rc::ldf32(position + 8)));
    auto ringPoint = [self](x86::reg32 i) {
        const x86::reg32 ring = rc::ld32(self + 0xc);
        return ring + 0x10 + ringOffset(ring, i, rc::ld32(ring), rc::ld32(ring + 8));
    };

    // The last point passed from the current one on (+8): each point ahead of the position widens the reach by
    // its distance outside the lane; the search stops at the first point beyond the reach behind.
    const bool single = rc::ld32(self + 0x74) == 1;
    const float maxReach = single ? 4.0f : 8.0f;
    const float margin = single ? 1.5f : 1.0f;
    double reach = rc::ldf32a(0x9c5da8);
    float ahead = 0.0f;
    x86::reg32 end = rc::ld32(self + 8);
    RingCursor scan;
    for (; sreg32(end) < sreg32(count); ++end)
    {
        if (!scan.started)
            scan = RingCursor(rc::ld32(self + 0xc), end);
        else
            scan.next();
        const x86::reg32 p = scan.point();
        const float dx = float(double(px) - double(rc::ldf32(p + 0x30)));
        const double dz = double(pz) - double(rc::ldf32(p + 0x38));
        const double along = dz * double(rc::ldf32(p + 0x14)) + double(dx) * double(rc::ldf32(p + 0x10));
        const float alongf = float(along);
        if (along >= zero)
        {
            rc::st32(self + 8, end);
            ahead = alongf;
            const double side = double(dx) * double(rc::ldf32(p + 0x14)) - dz * double(rc::ldf32(p + 0x10));
            const float left = float(double(rc::ldf32(p + 0x20)) - side);
            double outside = side - double(rc::ldf32(p + 0x24));
            if (!(outside > double(left)))
                outside = left;
            outside += double(margin);
            if (zero > outside)
                outside = zero;
            reach = reach + (outside + outside);
            if (double(maxReach) < reach)
                reach = maxReach;
        }
        if (-reach > double(alongf))
            break;
    }

    // The navigator's point between the passed point and the next.
    const x86::reg32 passed = rc::ld32(self + 8);
    const x86::reg32 ps = ringPoint(passed);
    auto ld = [](x86::reg32 address) { return double(rc::ldf32(address)); };
    if (sreg32(passed + 1) < sreg32(count))
    {
        const x86::reg32 pn = ringPoint(passed + 1);
        const float dx = float(double(px) - ld(pn + 0x30));
        const double dz = double(pz) - ld(pn + 0x38);
        const double gap = std::fabs(dz * ld(ps + 0x14) + double(dx) * ld(ps + 0x10)) + double(ahead);
        const float f = gap > eps ? float(double(ahead) / gap) : 0.0f;
        const double u = one - double(f);
        auto mixA = [&](x86::reg32 o) { return float(u * ld(ps + o) + double(f) * ld(pn + o)); };
        auto mixB = [&](x86::reg32 o) { return float(double(f) * ld(pn + o) + u * ld(ps + o)); };
        auto lerp = [&](x86::reg32 o) { return float((ld(pn + o) - ld(ps + o)) * double(f) + ld(ps + o)); };
        rc::stf32(self + 0x10, mixA(0));
        rc::stf32(self + 0x14, mixB(4));
        rc::stf32(self + 0x18, mixA(8));
        rc::stf32(self + 0x1c, mixB(0xc));
        rc::stf32(self + 0x20, mixA(0x10));
        rc::stf32(self + 0x24, mixB(0x14));
        rc::stf32(self + 0x40, lerp(0x30));
        rc::stf32(self + 0x44, lerp(0x34));
        rc::stf32(self + 0x48, lerp(0x38));
        rc::stf32(self + 0x30, lerp(0x20));
        rc::stf32(self + 0x34, lerp(0x24));
        // The direction normalised (0x7e7940) with its length at least the minimum at 0x9f2d68.
        {
            const double x = ld(self + 0x20), z = ld(self + 0x24);
            double length = std::sqrt(double(float(x * x + z * z)));
            const double shortest = rc::ldf32a(0x9f2d68);
            if (length < shortest)
                length = shortest;
            const double inverse = one / length;
            rc::stf32(self + 0x20, float(inverse * x));
            rc::stf32(self + 0x24, float(inverse * z));
        }
        // The lane: the passed point's, or the weighted mix of both when they differ.
        const x86::reg16 laneS = rc::ld16(ps + 0x3e), laneN = rc::ld16(pn + 0x3e);
        const x86::reg32 kindS = laneS & 0x7fff, kindN = laneN & 0x7fff;
        const double scale = rc::ldf32a(0x9c777c);
        float blend;
        if (kindS == kindN)
        {
            rc::st16(self + 0x4e, laneS);
            const double ws = double(rc::ld16(ps + 0x3c)) * scale, wn = double(rc::ld16(pn + 0x3c)) * scale;
            blend = float((wn - ws) * double(f) + ws);
        }
        else
        {
            const x86::reg32 table = rc::ld32(0xb77ecc);
            const double width = rc::ldf32a(0xb77f58);
            const float sn = float(double(rc::ld16(table + kindN * 0x16 + 4)) * width);
            const float ss = float(double(rc::ld16(table + kindS * 0x16 + 4)) * width);
            const float wn = float(double(rc::ld16(pn + 0x3c)) * scale * double(sn));
            const double d = (one - double(rc::ld16(ps + 0x3c)) * scale) * double(ss);
            const float ws = float(d);
            const double r = double(f) - double(ws) / (d + double(wn));
            if (r < zero)
            {
                rc::st16(self + 0x4e, laneS);
                blend = float((r / double(ss)) * double(ws) + one);
            }
            else
            {
                rc::st16(self + 0x4e, laneN);
                blend = float((r / double(sn)) * double(wn));
            }
        }
        // 0x7ea530: the lane fraction clamped to 0..1 as a 16-bit fixed-point value at +0x4c.
        double fraction = zero;
        if (double(blend) > zero)
        {
            fraction = blend;
            if (one < double(blend))
                fraction = one;
        }
        rc::st16(self + 0x4c, x86::reg16(x86::truncInteger64(fraction * double(rc::ldf32a(0x9c7780)) +
                                                              double(rc::ldf32a(0x9c2888)))));
    }
    else
    {
        rc::st32(self + 0x20, rc::ld32(ps + 0x10));
        rc::st32(self + 0x24, rc::ld32(ps + 0x14));
        rc::st32(self + 0x30, rc::ld32(ps + 0x20));
        rc::st32(self + 0x34, rc::ld32(ps + 0x24));
        rc::st16(self + 0x4e, rc::ld16(ps + 0x3e));
        const double f = ahead;
        rc::stf32(self + 0x10, float(f * ld(ps + 0x10) + ld(ps)));
        rc::stf32(self + 0x14, float(f * ld(ps + 0x14) + ld(ps + 4)));
        rc::stf32(self + 0x18, float(f * ld(ps + 0x10) + ld(ps + 8)));
        rc::stf32(self + 0x1c, float(f * ld(ps + 0x14) + ld(ps + 0xc)));
        rc::stf32(self + 0x40, float(f * ld(ps + 0x10) + ld(ps + 0x30)));
        rc::stf32(self + 0x48, float(f * ld(ps + 0x14) + ld(ps + 0x38)));
        rc::st32(self + 0x44, rc::ld32(ps + 0x34));
    }
    // Distance outside the lane at +0x50.
    const float dx = float(double(px) - ld(self + 0x40));
    const double dz = double(pz) - ld(self + 0x48);
    const float side = float(double(dx) * ld(self + 0x24) - dz * ld(self + 0x20));
    double outside = ld(self + 0x30) - double(side);
    const double other = double(side) - ld(self + 0x34);
    if (other > outside)
        outside = float(other);
    rc::stf32(self + 0x50, float(outside + ld(self + 0x84)));
    if (sreg32(end) >= sreg32(count))
        return finish();

    // The road from the end of the search on into the frame, with an empty point after it.
    memset(rc::host(points + (count << 6)), 0, 64);
    RingCursor copy;
    for (x86::reg32 k = end; sreg32(k) < sreg32(count); ++k)
    {
        if (!copy.started)
            copy = RingCursor(rc::ld32(self + 0xc), k);
        else
            copy.next();
        memcpy(rc::host(points + (k << 6)), rc::host(copy.point()), 64);
    }
    if (rc::ld8(entry + 4))
    {
        const float heading = float(double(v80) * ld(self + 0x24) - double(v88) * ld(self + 0x20));
        const x86::CPU r = frameCall(app, cpu, frame, 0x8093ba, &sub_8078f0, 0, self,
                                     { points + (end << 6), count - end, floatBits(side), floatBits(heading),
                                       rc::ld32(entry + 8) });
        if (r.terminate)
        {
            cpu.terminate = true;
            return;
        }
    }
    px = float(double(v80) * zero + double(px));
    py = float(double(v84) * zero + double(py));
    pz = float(double(v88) * zero + double(pz));

    if (rc::ld8(entry + 8))
    {
        // Smoothing: start position and direction from the vehicle, then the road's parameters written back.
        const x86::reg32 r1 = method(vehicle, 0x20, 0x80940f, {});
        if (cpu.terminate)
            return;
        const x86::reg32 startZ = rc::ld32(r1 + 8);
        const x86::reg32 r2 = method(vehicle, 0x20, 0x80941d, {});
        if (cpu.terminate)
            return;
        const x86::reg32 startX = rc::ld32(r2);
        const x86::reg32 r3 = method(vehicle, 0x24, 0x809432, {});
        if (cpu.terminate)
            return;
        x86::reg32 dirZ = rc::ld32(r3 + 8);
        const x86::reg32 r4 = method(vehicle, 0x24, 0x809440, {});
        if (cpu.terminate)
            return;
        double dirX = rc::ldf32(r4);
        auto bitsFloat = [](x86::reg32 bits) {
            float value;
            memcpy(&value, &bits, sizeof(value));
            return double(value);
        };
        double length = std::sqrt(dirX * dirX + bitsFloat(dirZ) * bitsFloat(dirZ));
        if (length <= double(rc::ldf32a(0x9c2938)))
        {
            dirZ = rc::ld32(frame + 0xac);
            dirX = a4;
            length = std::sqrt(a4 * a4 + ac * ac);
            if (length < eps)
            {
                dirZ = 0;
                dirX = one;
                length = one;
            }
        }
        const float inverse = float(one / length);
        rc::st32(frame + 0x70, startX);
        rc::st32(frame + 0x74, startZ);
        rc::stf32(frame + 0x2c, float(double(inverse) * dirX));
        rc::stf32(frame + 0x30, float(bitsFloat(dirZ) * double(inverse)));
        const x86::reg32 next = end + 1;
        const x86::reg32 first = sreg32(count) < sreg32(next) ? count : next;
        const x86::reg32 passes = rc::ld32(self + 0x54);
        rc::st32(self + 4, first);
        rc::st32(self + 0x54, passes + 1);
        const x86::reg32 rest = count - first;
        const x86::reg32 from = points + (first << 6);
        x86::CPU r = frameCall(app, cpu, frame, 0x80952f, &sub_7f9570, 0, frame + 0x70,
                               { from, rest, frame + 0x70, frame + 0x2c, 0, rest });
        if (!r.terminate && (passes & 1))
            r = frameCall(app, r, frame, 0x80954b, &sub_7f9d90, 0, r.ecx, { from, rest, frame + 0x70, frame + 0x2c });
        if (r.terminate)
        {
            cpu.terminate = true;
            return;
        }
        cpu.fpu.status = r.fpu.status;
        // Each copied point's parameter: its smoothed position projected onto the road point's edge vector.
        RingCursor road;
        for (x86::reg32 s = next; sreg32(s) < sreg32(count); ++s)
        {
            if (!road.started)
                road = RingCursor(rc::ld32(self + 0xc), s);
            else
                road.next();
            const x86::reg32 q = points + (s << 6);
            const x86::reg32 p = road.point();
            const double a = ld(q + 8) - ld(q);
            const double b = ld(q + 0xc) - ld(q + 4);
            const double c = ld(p + 8) - ld(p);
            const double d = ld(p + 0xc) - ld(p + 4);
            const double e = ld(q) - ld(p);
            const float g = float(ld(q + 4) - ld(p + 4));
            const double n = (((a * c) + (d * b)) * ld(q + 0x2c) + (e * c)) + (double(g) * d);
            const double result = n / ((c * c) + (d * d));
            float t = float(zero);
            if (result > zero)
            {
                t = float(result);
                if (one < double(t))
                    t = float(one);
            }
            rc::stf32(p + 0x2c, t);
        }
        rc::st32(self + 0x6c, 0);
        rc::st8(self + 0x5e, 0);
        rc::st32(self + 0x64, 0);
        rc::st32(self + 0x68, 0);
        for (x86::reg32 o = 0; o < 12; o += 4)
            rc::st32(self + 0x160 + o, rc::ld32(self + 0x94 + o));
        for (x86::reg32 o = 0; o < 12; o += 4)
            rc::st32(self + 0x154 + o, rc::ld32(self + 0x94 + o));
        return finish();
    }

    // Occlusion: the two edges' extreme bearings seen from the position; the search ends where they cross.
    float leftX = 0.0f, leftZ = 0.0f, rightX = 0.0f, rightZ = 0.0f;
    x86::reg32 lastLeft = end, lastRight = end, open = end, chosen = end;
    int which = 0;
    bool leftSide = false;
    for (x86::reg32 k = end; sreg32(k) <= sreg32(count); ++k)
    {
        const x86::reg32 q = points + (k << 6);
        const float lx = float(ld(q) - double(px));
        const double lz = ld(q + 4) - double(pz);
        const float rx = float(ld(q + 8) - double(px));
        const float rz = float(ld(q + 0xc) - double(pz));
        if (k == end)
        {
            rightZ = float(lz);
            rightX = lx;
            leftX = rx;
            leftZ = rz;
        }
        else
        {
            const bool last = k == count;
            const x86::reg32 centre = last ? self + 0x94 : q + 0x30;
            const float cx = float(ld(centre) - double(px));
            const float cz = float(ld(centre + 8) - double(pz));
            if (!last)
            {
                if (double(rightX) * lz - double(rightZ) * double(lx) < zero)
                {
                    rightZ = float(lz);
                    rightX = lx;
                    lastRight = k;
                }
                if (double(rz) * double(leftX) - double(leftZ) * double(rx) > zero)
                {
                    leftX = rx;
                    leftZ = rz;
                    lastLeft = k;
                }
            }
            if (double(rightZ) * double(cx) - double(cz) * double(rightX) < zero)
            {
                which = 1;
                chosen = lastRight;
                leftSide = true;
            }
            else if (double(leftZ) * double(cx) - double(cz) * double(leftX) > zero)
            {
                which = 2;
                chosen = lastLeft;
                leftSide = false;
            }
            else
            {
                open = k;
            }
        }
        if (double(leftZ) * double(rightX) - double(rightZ) * double(leftX) > zero)
            break;
        app->allowPreemption(cpu);
    }
    if (sreg32(open) < sreg32(count) && which != 0)
    {
        const float vx = which == 1 ? rightX : leftX;
        const float vz = which == 1 ? rightZ : leftZ;
        const x86::reg32 q = points + (chosen << 6);
        const float dy = float(ld(q + 0x34) - double(py));
        const x86::reg32 flags = rc::ld32(q + 0x28);
        rc::stf32(self + 0x154, float(double(vx) + double(px)));
        rc::stf32(self + 0x158, float(double(dy) + double(py)));
        rc::stf32(self + 0x15c, float(double(vz) + double(pz)));
        const x86::reg32 sign = leftSide ? 0xffffffffu : 1u;
        const x86::reg32 left = (flags & 1) ? 0 : sign;
        const x86::reg32 right = (flags & 1) ? sign : 0;
        rc::st32(self + 0x64, left);
        rc::st32(self + 0x68, right);
        rc::st8(self + 0x5e, right != 0 && (flags & 2) ? 1 : 0);
        if (rc::ld8(self + 0x5d) != 0 && (left != 0 || right != 0))
        {
            const float width = float(std::fabs(ld(q + 0x24) - ld(q + 0x20)));
            const float ox = rc::ldf32(self + 0x154), oz = rc::ldf32(self + 0x15c);
            const float d0 = float(double(ox) - double(px)), d1 = float(double(oz) - double(pz));
            float n1x, n1z, n2x, n2z, n3x, n3z;
            normalize2(d0, d1, zero, one, n1x, n1z);
            normalize2(float(double(ox) - ld(self + 0x94)), float(double(oz) - ld(self + 0x9c)), zero, one, n2x,
                       n2z);
            const double distance = std::sqrt(double(d1) * double(d1) + double(d0) * double(d0));
            const float tx = float(float(double(n2x) * distance) + double(ox));
            const float tz = float(float(double(n2z) * distance) + double(oz));
            normalize2(float(double(n1x) + double(n2x)), float(double(n1z) + double(n2z)), zero, one, n3x, n3z);
            const double reachOut = (double(tz) - double(oz)) * double(n3z) + (double(tx) - double(ox)) * double(n3x);
            float offset = reachOut < double(width) ? float(reachOut) : width;
            if (rc::ld32(self + 0x68) != 0)
            {
                // 0x401fb0: (value - 0) / range clamped to 0..1, 0 for a range up to 0x9c1760.
                const double h = double(n1z) * double(v88) + double(n1x) * double(v80);
                const float range = float(h + h);
                const float value = float(h - ld(self + 0x6c));
                double ratio = zero;
                const double span = double(range) - 0.0;
                if (span > double(rc::ldf32a(0x9c1760)))
                {
                    const float r = float((double(value) - 0.0) / span);
                    if (!(double(r) < one))
                        ratio = one;
                    else if (!(zero > double(r)))
                        ratio = r;
                }
                const double scaled = (ratio + ratio) * double(offset);
                offset = float(scaled);
                if (!(scaled < double(width)))
                    offset = width;
            }
            rc::st32(self + 0x164, rc::ld32(self + 0x158));
            rc::stf32(self + 0x160, float(double(n3x) * double(offset) + ld(self + 0x154)));
            rc::stf32(self + 0x168, float(double(n3z) * double(offset) + ld(self + 0x15c)));
        }
        else
        {
            for (x86::reg32 o = 0; o < 12; o += 4)
                rc::st32(self + 0x160 + o, rc::ld32(self + 0x94 + o));
        }
    }
    if (rc::ld32(self + 0x68) == 0)
    {
        rc::st32(self + 0x6c, 0);
        rc::st8(self + 0x5e, 0);
    }
    finish();
}

}
}
