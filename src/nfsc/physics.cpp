// Native replacements of the car physics step (suspension and tire forces): the same double-precision steps and
// float stores as the translated x87 code, leaf callees inlined, other callees called as guest code.
// physics_native=0 runs the translated bodies instead.

#include "physics.h"
#include "shadow.h"

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
void sub_6fca30_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_6f0880(win32::WinApplication* app, x86::CPU& cpu);
void sub_6c73a0(win32::WinApplication* app, x86::CPU& cpu);
void sub_87eb42(win32::WinApplication* app, x86::CPU& cpu);

namespace
{

int s_native = -1;

bool useNative()
{
    if (s_native < 0)
        s_native = pf::Config::getBool("physics_native", true) ? 1 : 0;
    return s_native != 0;
}

double ld(x86::reg32 address)
{
    return rc::ldf32(address);
}

double constant(x86::reg32 address)
{
    return rc::ldf32a(address);
}

void st(x86::reg32 address, double value)
{
    rc::stf32(address, float(value));
}

double rounded(double value)
{
    return double(float(value));
}

x86::reg32 floatBitsOf(float value)
{
    x86::reg32 bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

float bitsFloat(x86::reg32 bits)
{
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

// Calls a guest function with its return address at esp.
bool guestCall(win32::WinApplication* app, x86::CPU& cpu, win32::MethodPtr function, x86::reg32 esp,
               x86::reg32 ret)
{
    rc::st32(esp, ret);
    cpu.esp = esp;
    function(app, cpu);
    return !cpu.terminate;
}

// Virtual call through the method at offset of the object's table, ecx the object.
bool virtualCall(win32::WinApplication* app, x86::CPU& cpu, x86::reg32 object, x86::reg32 offset, x86::reg32 esp,
                 x86::reg32 ret)
{
    const x86::reg32 table = rc::ld32(object);
    cpu.ecx = object;
    cpu.edx = table;
    rc::st32(esp, ret);
    cpu.esp = esp;
    app->dynamic_call(rc::ld32(table + offset), cpu);
    return !cpu.terminate;
}

// The last x87 compare whose status word the translated code keeps: applied before guest calls and at the end.
struct Status
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

// Vector (x, y, z) scaled to unit length (0x411d40): zero when its squared length is at most the epsilon.
void normalize(float v[3])
{
    const double x = v[0], y = v[1], z = v[2];
    const float length2 = float((x * x + y * y) + z * z);
    double scale = constant(0x9c248c);
    if (double(length2) > constant(0x9c1760))
    {
        const double length = std::sqrt(double(length2));
        scale = length == constant(0x9c248c) ? constant(0x9c248c) : constant(0x9c2478) / length;
    }
    v[0] = float(scale * x);
    v[1] = float(scale * y);
    v[2] = float(scale * z);
}

// (x - low) / (high - low) clamped to 0..1, 0 for an empty range (0x401fb0).
double ramp(Status& fsw, float x, float low, float high)
{
    const double range = double(high) - double(low);
    fsw.set(range, constant(0x9c1760));
    if (!(range > constant(0x9c1760)))
        return constant(0x9c248c);
    const float t = float((double(x) - double(low)) / range);
    fsw.set(t, constant(0x9c2478));
    if (!(double(t) < constant(0x9c2478)))
        return constant(0x9c2478);
    fsw.set(constant(0x9c248c), t);
    if (constant(0x9c248c) > double(t))
        return constant(0x9c248c);
    return t;
}

// The value whose magnitude is smaller when both have the same sign, else a (0x6e6640).
double smaller(Status& fsw, float a, float b)
{
    fsw.set(double(a) * double(b), constant(0x9c248c));
    if (!(double(a) * double(b) > constant(0x9c248c)))
        return a;
    fsw.set(a, b);
    if (double(a) > constant(0x9c248c))
        return double(a) < double(b) ? a : b;
    return double(a) > double(b) ? a : b;
}

// 16-bit angle of (x, y) from the arctangent table at 0xa4da60 (0x46dfd0).
x86::sreg16 angle(Status& fsw, float x, float y)
{
    using x86::sreg32;
    const double zero = constant(0x9c248c);
    x86::reg32 quadrant = 0;
    fsw.set(x, zero);
    if (double(x) < zero)
    {
        quadrant = 1;
        x = float(-double(x));
    }
    double ay = y;
    fsw.set(ay, zero);
    if (ay < zero)
    {
        quadrant ^= 3;
        ay = -ay;
    }
    auto entry = [](double ratio) {
        const x86::reg32 i = x86::reg32(x86::truncInteger64(ratio * constant(0x9c5834)));
        const x86::reg32 at = 0xa4da60u + x86::reg32(sreg32(i) >> 8) * 2;
        const x86::reg32 low = rc::ld16(at);
        const x86::reg32 step = x86::reg32(sreg32((x86::reg32(rc::ld16(at + 2)) - low) * (i & 0xff)) >> 8);
        return std::pair<x86::reg32, x86::reg32>(low, step);
    };
    x86::reg32 r;
    fsw.set(x, ay);
    if (double(x) > ay)
    {
        const auto e = entry(ay / double(x));
        r = e.first + e.second;
    }
    else
    {
        fsw.set(ay, x);
        if (ay > double(x))
        {
            const auto e = entry(double(x) / ay);
            r = 0x4000u - e.second - e.first;
        }
        else
        {
            fsw.set(ay, zero);
            r = ay == zero ? 0 : 0x2000u;
        }
    }
    switch (quadrant)
    {
    case 0:
        break;
    case 3:
        r = 0u - r;
        break;
    case 1:
        r = 0x8000u - r;
        break;
    default:
        r = r + 0x8000u;
        break;
    }
    return x86::sreg16(x86::reg16(r));
}

// Slip angle of the lateral and forward speeds (0x6063e0): components below the epsilon count as zero.
double slipAngle(Status& fsw, float lateral, float forward)
{
    const double zero = constant(0x9c248c);
    const double epsilon = constant(0x9d77f8);
    auto magnitude = [zero](float v) { return double(v) < zero ? -double(v) : double(v); };
    fsw.set(forward, zero);
    fsw.set(magnitude(forward), epsilon);
    const float y = magnitude(forward) > epsilon ? forward : 0.0f;
    fsw.set(lateral, zero);
    const float x = magnitude(lateral) > epsilon ? lateral : 0.0f;
    return double(angle(fsw, y, x)) * constant(0x9c2670);
}

// SuspensionSimple::Tire::UpdateLoaded (thiscall: tire, lateral speed, forward speed, forward speed, load, time
// step, lateral factor), 0x6efe10: returns the lateral force.
double tireUpdateLoaded(x86::CPU& cpu, Status& fsw, x86::reg32 w, float lateral, float speed, float speed2,
                        float load, float dt, float factor)
{
    const double zero = constant(0x9c248c);
    const double one = constant(0x9c2478);
    auto abs = [zero](double v) { return v < zero ? -v : v; };
    fsw.set(ld(w + 0xd0), zero);
    if (ld(w + 0xd0) <= zero && rc::ld8(w + 0x108) == 0)
        st(w + 0xcc, double(speed) / ld(w + 0xc0));
    const float accel = float((double(speed) - ld(w + 0xf0)) / double(dt));
    rc::stf32(w + 0xf0, speed);
    rc::stf32(w + 0xe8, lateral);
    fsw.set(load, zero);
    rc::stf32(w + 0xd0, double(load) > zero ? load : float(zero));
    rc::st32(w + 0x104, 0x3f800000u);
    fsw.set(ld(w + 0xc8), zero);
    const bool driven = ld(w + 0xc8) > zero && rc::ld32(w + 0x100) == 1;
    rc::st8(w + 0x108, driven ? 1 : 0);
    bool turning = false;
    fsw.set(ld(w + 0xc8) + ld(w + 0xc4), zero);
    if (ld(w + 0xc8) + ld(w + 0xc4) > zero)
    {
        turning = true;
        double torque;
        fsw.set(speed, zero);
        fsw.set(abs(speed), one);
        if (abs(speed) < one)
        {
            const double k = constant(0x9e4ee0) / ld(w + 0xc0);
            torque = ((k * ld(w + 0xc8)) * double(speed)) * double(load) +
                     ((k * ld(w + 0xc4)) * double(speed)) * double(load);
        }
        else
        {
            const x86::reg32 specs = rc::ld32(rc::ld32(w + 0xf8) + 4);
            const double brake = ((ld(specs + rc::ld32(w + 0x100) * 4 + 8) * constant(0x9e39d8)) * ld(w + 0xc4)) *
                                 constant(0x9e4f38);
            const double handBrake = ((ld(specs + 0x10) * constant(0x9e39d8)) * ld(w + 0xc8)) * constant(0x9e4f38);
            torque = brake + handBrake;
            fsw.set(ld(w + 0xcc), zero);
            if (ld(w + 0xcc) > zero)
                torque = -torque;
        }
        st(w + 0xdc, torque + ld(w + 0xdc));
    }
    const float forward = double(speed) < zero ? float(-double(speed)) : speed;
    st(w + 0xfc, slipAngle(fsw, lateral, forward));
    if (rc::ld8(w + 0x108) == 0 && rc::ld8(w + 0x109) == 0)
        rc::st32(w + 0xe4, 0);
    else
        st(w + 0xe4, ld(w + 0xcc) * ld(w + 0xc0) - double(speed));
    bool clipped = false;
    float clip = 1.0f;
    {
        fsw.set(ld(w + 0xe4), zero);
        const double slip = abs(ld(w + 0xe4));
        if (rc::ld8(w + 0x109))
            fsw.set(slip, ld(w + 0x114));
        if (rc::ld8(w + 0x109) && slip > ld(w + 0x114))
        {
            clipped = true;
            clip = float(ld(w + 0x114) / slip);
            st(w + 0x104, double(clip) * ld(w + 0x104));
        }
    }
    const double slip = ld(w + 0xe4);
    const float mag2 = float(slip * slip + double(lateral) * double(lateral));
    const float mag = float(std::sqrt(double(mag2)));
    const float angleAbs = float(abs(ld(w + 0xfc)));
    const x86::reg32 tuning = rc::ld32(w + 0xf4);
    const x86::reg32 index = rc::ld32(w + 0x100);
    const x86::reg32 specs = rc::ld32(tuning + 4);
    const double lateralForce = ((((ramp(fsw, angleAbs, 0.0f, bitsFloat(0x3d088890)) *
                                    ld(specs + index * 4 + 0x70)) * ld(w + 0x118)) * ld(w + 0xe0)) * ld(w + 0xd0)) *
                                constant(0x9e514c);
    st(w + 0xd4, lateralForce);
    fsw.set(ld(w + 0xfc), zero);
    if (ld(w + 0xfc) > zero)
        st(w + 0xd4, -lateralForce);
    const bool locked = rc::ld8(w + 0x108) != 0;
    if (locked)
        fsw.set(mag, one);
    if (locked && double(mag) > one)
    {
        const double grip = ld(specs + index * 4 + 0x58);
        st(w + 0xd8, (((grip * ld(w + 0xe0)) * ld(w + 0xe4)) * ld(w + 0xd0)) / (double(mag) * constant(0x9e4f2c)));
        st(w + 0xd4, ((((ld(w + 0xe0) / double(mag)) * grip) * double(lateral)) * ld(w + 0xd0)) * constant(0x9e5958));
    }
    else
    {
        const double longitudinal = ld(w + 0xdc) / ld(w + 0xc0);
        if (clipped)
        {
            const double r = ramp(fsw, speed2, bitsFloat(0x4156930c), bitsFloat(0x41b2cfdf));
            const float limit = float((((((r * constant(0x9e562c) + constant(0x9d8b78)) *
                                          ld(specs + index * 4 + 0x58)) * ld(w + 0xe0)) / double(mag)) *
                                       ld(w + 0xd0)) * ld(w + 0xe4));
            st(w + 0xd8, smaller(fsw, limit, float(longitudinal)));
        }
        else
        {
            st(w + 0xd8, longitudinal);
        }
    }
    if (rc::ld8(w + 0x109) && !clipped && !locked)
        st(w + 0xd8, (((ld(w + 0x110) * ld(w + 0xc0) - double(accel)) / ld(w + 0xc0)) * constant(0x9e4f38)) +
                         ld(w + 0xd8));
    const float sum2 = float(ld(w + 0xd8) * ld(w + 0xd8) + ld(w + 0xd4) * ld(w + 0xd4));
    const double limit = ((((ld(w + 0xc4) * constant(0x9e4fd4)) + one) * ld(specs + index * 4 + 0x48)) *
                          ld(w + 0xe0)) * ld(w + 0xd0);
    const float limitf = float(limit);
    fsw.set(limit * double(limitf), sum2);
    if (limit * double(limitf) < double(sum2))
        fsw.set(sum2, constant(0x9e4e9c));
    if (limit * double(limitf) < double(sum2) && double(sum2) > constant(0x9e4e9c))
    {
        const double r = double(limitf) / std::sqrt(double(sum2));
        st(w + 0x104, r * ld(w + 0x104));
        st(w + 0xd4, r * ld(w + 0xd4));
        st(w + 0xd8, r * ld(w + 0xd8));
        clip = float((r * r) * double(clip));
    }
    fsw.set(speed, zero);
    fsw.set(abs(speed), one);
    if (abs(speed) > one)
    {
        const x86::reg32 table = rc::ld32(rc::ld32(w + 0xf4) + 4);
        const double s = cpu.fpu.sin(ld(w + 0xfc) * constant(0x9e4e94));
        st(w + 0xd8, ld(w + 0xd8) - ((s / ld(table + rc::ld32(w + 0x100) * 4 + 0x70)) * ld(w + 0xd4)) *
                                        double(factor));
    }
    else
    {
        fsw.set(lateral, zero);
        fsw.set(abs(lateral), one);
        const double m = abs(lateral) < one ? abs(lateral) : one;
        st(w + 0xd4, m * ld(w + 0xd4));
    }
    if (!turning && rc::ld32(w + 0xec) > 1)
    {
        const x86::reg32 owner = rc::ld32(w + 0xa0);
        st(w + 0xd8, ld(owner + 0xec) * ld(w + 0xd8));
        st(w + 0xd4, ld(owner + 0xf0) * ld(w + 0xd4));
    }
    if (rc::ld8(w + 0x108))
    {
        const double result = ld(w + 0xd4);
        rc::st32(w + 0x110, 0);
        rc::st32(w + 0xcc, 0);
        return result;
    }
    if (rc::ld8(w + 0x109))
        fsw.set(clip, one);
    if (rc::ld8(w + 0x109) && double(clip) < one)
    {
        const x86::reg32 owner = rc::ld32(w + 0xa0);
        const double u = ((ld(w + 0xdc) - ld(w + 0xd8) * ld(w + 0xc0)) + ld(w + 0x10c)) * constant(0x9c2888);
        st(w + 0x10c, u);
        const double v1 = (u - (ld(owner + 0xe4) * ld(w + 0xcc)) * constant(0x9e4f2c)) * constant(0x9c27b0);
        const double v2 = v1 - (double(clip) * ld(w + 0xe4)) / (double(dt) * ld(w + 0xc0));
        st(w + 0x110, v2);
        const double v3 = ((double(accel) / ld(w + 0xc0)) - v2) * double(clip) + v2;
        st(w + 0x110, v3);
        st(w + 0xcc, v3 * double(dt) + ld(w + 0xcc));
        return ld(w + 0xd4);
    }
    st(w + 0xcc, ld(w + 0xf0) / ld(w + 0xc0));
    return ld(w + 0xd4);
}

// Tire state of an unloaded wheel (thiscall: tire, time step), 0x6e1480.
void tireUnloaded(Status& fsw, x86::reg32 w, float dt)
{
    fsw.set(ld(w + 0xc8), constant(0x9c248c));
    rc::st32(w + 0x104, 0);
    rc::st32(w + 0xd0, 0);
    rc::st32(w + 0xe4, 0);
    rc::st32(w + 0xfc, 0);
    const bool driven = ld(w + 0xc8) > constant(0x9c248c) && rc::ld32(w + 0x100) == 1;
    rc::st8(w + 0x108, driven ? 1 : 0);
    if (driven)
        rc::st32(w + 0xcc, 0);
    else
        st(w + 0xcc, (ld(w + 0xdc) / constant(0xa63594)) * double(dt) + ld(w + 0xcc));
    rc::st32(w + 0xd4, 0);
    rc::st32(w + 0xd8, 0);
    rc::st32(w + 0x10c, 0);
    rc::st32(w + 0x110, 0);
}

// shadow_physics=1: physics port calls compared with their translated bodies.
shadow::Group s_shadow("shadow_physics");
using ShadowRegion = shadow::Region;

bool shadowOn()
{
    return s_shadow.active();
}

// The registers the translated bodies write back for their callers: edx, ebx, esp, esi, edi.
constexpr x86::reg32 WrittenBack = 0xdc;

// Runs port (the native entry, with the shadow off) after the translated body guest; regions: the guest memory
// either may write, restored before the native run; repeatable: whether the native run may follow.
void shadowCall(const char* name, win32::WinApplication* app, x86::CPU& cpu, win32::MethodPtr guest,
                win32::MethodPtr port, const std::vector<ShadowRegion>& regions, shadow::Group::Repeatable repeatable,
                const void* context)
{
    s_shadow.call(name, app, cpu, guest, port, regions, WrittenBack, repeatable, context);
}

}

namespace physics
{

void logShadow()
{
    s_shadow.log();
}

}

namespace native
{

// SuspensionSimple::DoWheelForces (thiscall: suspension, chassis state), 0x6fca30: per wheel, the wheel position
// (guest), the suspension compression, spring and damper load, the tire forces (Tire::UpdateLoaded inlined) and
// the force on the wheel; then the summed force and torque on the rigid body and the bottom-out push.
void sub_6fca30(win32::WinApplication* app, x86::CPU& cpu)
{
    if (shadowOn())
    {
        // The suspension, its wheels, the chassis state and the rigid body's force accumulators.
        const x86::reg32 self = cpu.ecx;
        std::vector<ShadowRegion> regions = { { self, 0x120 }, { rc::ld32(cpu.esp + 4), 0xe0 } };
        for (x86::reg32 i = 0; i < 4; ++i)
            regions.push_back({ rc::ld32(self + 0x110 + 4 * i), 0x120 });
        const x86::reg32 body = rc::ld32(self + 0xcc);
        if (body)
            regions.push_back({ rc::ld32(rc::ld32(body + 0x30)), 0x70 });
        // A translated run whose wheel position updates changed a wheel's surface or deleted the surface collection
        // a wheel held is not repeated (reference counts outside these regions).
        struct Surfaces
        {
            x86::reg32 wheel[4], before[4];
        } surfaces;
        for (x86::reg32 i = 0; i < 4; ++i)
        {
            surfaces.wheel[i] = rc::ld32(self + 0x110 + 4 * i);
            surfaces.before[i] = rc::ld32(surfaces.wheel[i] + 0x9c);
        }
        auto repeatable = [](const void* context) {
            const Surfaces& s = *static_cast<const Surfaces*>(context);
            for (int i = 0; i < 4; ++i)
            {
                if (rc::ld32(s.wheel[i] + 0x9c) != s.before[i])
                    return false;
                const x86::reg32 held = rc::ld32(s.wheel[i] + 0x38) ? rc::ld32(s.wheel[i] + 0x38) : rc::ld32(0xb4d868);
                if (held && rc::ld16(held + 8) == 0)
                    return false;
            }
            return true;
        };
        shadowCall("SuspensionSimple::DoWheelForces 0x6fca30", app, cpu, &sub_6fca30_guest, &sub_6fca30, regions,
                   repeatable, &surfaces);
        return;
    }
    if (!useNative())
    {
        sub_6fca30_guest(app, cpu);
        return;
    }
    const x86::reg32 entry = cpu.esp;
    // The guest frame: esp after the prologue's pushes.
    const x86::reg32 s = entry - 0x134;
    const x86::reg32 self = cpu.ecx;
    const x86::reg32 state = rc::ld32(entry + 4);
    const x86::reg32 ebx = cpu.ebx, ebp = cpu.ebp, esi = cpu.esi, edi = cpu.edi;
    const x86::reg32 depth = cpu.fpu.count;
    const double zero = constant(0x9c248c);
    const double one = constant(0x9c2478);
    Status fsw;
    // Ground contact points of the axles (thiscall: suspension, state, front out, rear out) into the guest frame.
    for (x86::reg32 k = 0; k < 12; k += 4)
    {
        rc::st32(s + 0x14 + k, 0);
        rc::st32(s + 0x64 + k, 0);
    }
    rc::st32(s - 0xc, state);
    rc::st32(s - 8, s + 0x14);
    rc::st32(s - 4, s + 0x64);
    cpu.eax = s + 0x64;
    cpu.ebp = 0;
    cpu.esi = state;
    cpu.edi = self;
    if (!guestCall(app, cpu, &sub_6f0880, s - 0x10, 0x6fca73))
        return;
    float contact[4][3];
    for (int k = 0; k < 3; ++k)
    {
        contact[0][k] = rc::ldf32(s + 0x64 + 4 * k);
        contact[1][k] = rc::ldf32(s + 0x14 + 4 * k);
        contact[2][k] = rc::ldf32(state + 0x20 + 4 * k);
        contact[3][k] = contact[2][k];
    }
    x86::reg32 wheels[4];
    for (int i = 0; i < 4; ++i)
        wheels[i] = rc::ld32(self + 0x110 + 4 * i);
    rc::st32(wheels[2] + 0xc8, rc::ld32(state + 0x7c));
    rc::st32(wheels[3] + 0xc8, rc::ld32(state + 0x7c));
    for (int i = 0; i < 4; ++i)
        rc::st32(wheels[i] + 0xc4, rc::ld32(state + 0x5c));
    // The guest's two copies of the rear wheels' test (both branches store the same value).
    for (int copy = 0; copy < 2; ++copy)
    {
        fsw.set(ld(state + 0x4c), constant(0x9c93fc));
        if (ld(state + 0x4c) > constant(0x9c93fc))
        {
            fsw.set(ld(state + 0x5c), constant(0x9c2888));
            if (copy == 0 && ld(state + 0x5c) > constant(0x9c2888))
                fsw.set(ld(state + 0x48) * constant(0x9c583c), constant(0x9e4f38));
        }
    }
    float factor = 0.5f;
    if (rc::ld16(state + 0xca) == 3)
    {
        const x86::reg32 object = rc::ld32(self + 0xdc);
        double value = zero;
        if (object)
        {
            cpu.eax = rc::ld32(object);
            cpu.ebx = self + 0x110;
            cpu.fpu.count = depth;
            fsw.apply(cpu);
            if (!virtualCall(app, cpu, object, 4, s - 4, 0x6fcb85))
                return;
            RC_FPU_CALL(cpu, depth + 1, 0x6fcb82u);
            value = cpu.fpu.regs[(depth + 1) & 7];
            cpu.fpu.count = depth;
        }
        factor = float(value * constant(0x9e6088) + constant(0x9e5164));
    }
    // Per-axle tuning, scaled.
    const x86::reg32 tuning = rc::ld32(self + 0xb0);
    const float progression[2] = { rc::ldf32(tuning), rc::ldf32(tuning + 4) };
    const double k52e8 = constant(0x9e52e8), k87f0 = constant(0x9c87f0);
    const float rebound[2] = { float(ld(tuning + 0x28) * k52e8), float(ld(tuning + 0x2c) * k52e8) };
    const float bump[2] = { float(ld(tuning + 0x20) * k52e8), float(ld(tuning + 0x24) * k52e8) };
    const float bottom[2] = { float(ld(tuning + 0x40) * k87f0), float(ld(tuning + 0x44) * k87f0) };
    const float exponent[2] = { float(one - ld(tuning + 0x30)), float(one - ld(tuning + 0x34)) };
    const float spring[2] = { float(ld(tuning + 0x38) * k52e8), float(ld(tuning + 0x3c) * k52e8) };
    const float travel[2] = { float(ld(tuning + 8) * k87f0), float(ld(tuning + 0xc) * k87f0) };
    const float ride[2] = { float(ld(tuning + 0x10) * k87f0), float(ld(tuning + 0x14) * k87f0) };
    float antiRoll[4];
    {
        const double front = ld(tuning + 0x48) * k52e8;
        const double rear = ld(tuning + 0x4c) * k52e8;
        const double x = (ld(wheels[0] + 0x78) - ld(wheels[1] + 0x78)) * front;
        antiRoll[0] = float(x);
        antiRoll[1] = float(-x);
        antiRoll[2] = float((ld(wheels[2] + 0x78) - ld(wheels[3] + 0x78)) * rear);
        antiRoll[3] = float(-double(antiRoll[2]));
    }
    float bottomOut = 0.0f;
    x86::reg32 grounded = 0;
    bool anyGrounded = false;
    for (x86::reg32 i = 0; i < 4; ++i)
    {
        const x86::reg32 w = rc::ld32(self + 0x110 + 4 * i);
        const x86::reg32 axle = i >> 1;
        // Wheel::UpdatePosition (thiscall: wheel, 9 arguments; ret 0x24).
        rc::st32(s - 4, floatBitsOf(float(ld(state + 0x84) + ld(state + 0x84))));
        rc::st32(s - 8, rc::ld32(state + 0xd8));
        rc::st32(s - 0xc, 1);
        rc::st32(s - 0x10, rc::ld32(w + 0xc0));
        rc::st32(s - 0x14, rc::ld32(state + 0xc0));
        rc::st32(s - 0x18, state + 0xb0);
        rc::st32(s - 0x1c, state);
        rc::st32(s - 0x20, state + 0x50);
        rc::st32(s - 0x24, state + 0x60);
        cpu.eax = state + 0x60;
        cpu.ecx = w;
        cpu.edx = rc::ld32(state + 0xc0);
        cpu.ebx = axle;
        cpu.ebp = s + 0xf8 + 0x10 * i;
        cpu.esi = state;
        cpu.edi = w;
        cpu.fpu.count = depth;
        fsw.apply(cpu);
        if (!guestCall(app, cpu, &sub_6c73a0, s - 0x28, 0x6fce59))
            return;
        const float* e = contact[i];
        const double wx = ld(w + 0x3c), wy = ld(w + 0x40), wz = ld(w + 0x44);
        float n[3] = { float(double(e[2]) * wy - double(e[1]) * wz), float(double(e[0]) * wz - double(e[2]) * wx),
                       float(double(e[1]) * wx - double(e[0]) * wy) };
        normalize(n);
        const double d = (wy * ld(state + 0x14) + wx * ld(state + 0x10)) + ld(state + 0x18) * wz;
        double c;
        fsw.set(d, one);
        if (!(d < one))
        {
            c = one;
        }
        else
        {
            const float fd = float(d);
            fsw.set(zero, fd);
            c = zero > double(fd) ? zero : double(fd);
        }
        double t = c * double(ride[axle]) + ld(w + 0x48);
        const float limit = travel[axle];
        fsw.set(ld(w + 0x78), zero);
        if (ld(w + 0x78) == zero)
        {
            const double x = t - double(limit);
            fsw.set(bottomOut, x);
            bottomOut = float(double(bottomOut) > x ? double(bottomOut) : x);
        }
        fsw.set(t, zero);
        if (!(t > zero))
            t = zero;
        float compression = float(t);
        fsw.set(t, limit);
        if (t > double(limit))
        {
            const double x = t - double(limit);
            bottomOut = float(double(bottomOut) > x ? double(bottomOut) : x);
            compression = limit;
        }
        fsw.set(compression, constant(0x9e4e9c));
        if (double(compression) > constant(0x9e4e9c) && c > constant(0xa63230))
        {
            ++grounded;
            const double dt = ld(state + 0xc0);
            double v = (double(compression) - ld(w + 0x78)) / dt;
            const float vf = float(v);
            fsw.set(bottom[axle], constant(0x9e4e9c));
            if (double(bottom[axle]) > constant(0x9e4e9c))
                fsw.set(exponent[axle], one);
            if (double(bottom[axle]) > constant(0x9e4e9c) && double(exponent[axle]) < one)
            {
                fsw.set(v, zero);
                const double av = v < zero ? -v : v;
                fsw.set(av, bottom[axle]);
                if (av > double(bottom[axle]))
                {
                    // pow(|v| / bottom, exponent) through the runtime's import.
                    cpu.fpu.regs[(depth + 1) & 7] = double(float(av)) / double(bottom[axle]);
                    cpu.fpu.regs[(depth + 2) & 7] = double(exponent[axle]);
                    cpu.fpu.count = depth + 2;
                    fsw.apply(cpu);
                    if (!guestCall(app, cpu, &sub_87eb42, s - 4, 0x6fd06d))
                        return;
                    RC_FPU_CALL(cpu, depth + 1, 0x6fd068u);
                    v = double(cpu.fpu.regs[(depth + 1) & 7]) * double(bottom[axle]);
                    cpu.fpu.count = depth;
                    fsw.set(vf, zero);
                    if (!(double(vf) > zero))
                        v = -v;
                }
            }
            const float springForce = float(((double(compression) * double(progression[axle]) + one) *
                                             double(spring[axle])) * double(compression));
            double damping = v * double(v > zero ? rebound[axle] : bump[axle]);
            const double damperLimit = (ld(rc::ld32(self + 0xb0) + 0x5c) * ld(state + 0xac)) * constant(0x9c2bec);
            fsw.set(damping, damperLimit);
            if (damping > damperLimit)
                damping = zero;
            const double total = (damping + double(antiRoll[i])) + double(springForce);
            const float load = total > zero ? float(total) : 0.0f;
            const float lateral = float((double(n[2]) * ld(w + 0x94) + double(n[1]) * ld(w + 0x90)) +
                                        double(n[0]) * ld(w + 0x8c));
            const float speed = rc::ldf32(state + 0x48);
            const float result = float(tireUpdateLoaded(cpu, fsw, w, lateral, speed, speed, load,
                                                        rc::ldf32(state + 0xc0), factor));
            double q = (double(lateral) / ld(state + 0xc0)) * (ld(state + 0xac) * constant(0x9c2d68));
            fsw.set(q, zero);
            if (q < zero)
                q = -q;
            const float negative = float(-q);
            fsw.set(result, q);
            double grip = double(result) < q ? double(result) : q;
            if (double(negative) > grip)
                grip = negative;
            const float g[3] = { float(double(n[0]) * grip), float(double(n[1]) * grip), float(double(n[2]) * grip) };
            float m[3] = { float(double(n[1]) * wz - double(n[2]) * wy), float(double(n[2]) * wx - double(n[0]) * wz),
                           float(double(n[0]) * wy - double(n[1]) * wx) };
            normalize(m);
            const double up = ld(w + 0xd8);
            const float my = float(double(m[1]) * up);
            const float mz = float(double(m[2]) * up);
            const float lx = float(double(load) * ld(state + 0x10));
            const float ly = float(double(load) * ld(state + 0x14));
            const float lz = float(double(load) * ld(state + 0x18));
            const float z = float(double(mz) + double(g[2]));
            st(w + 0x5c, ((double(m[0]) * up) + double(g[0])) + double(lx));
            st(w + 0x60, (double(my) + double(g[1])) + double(ly));
            st(w + 0x64, double(z) + double(lz));
            anyGrounded = true;
        }
        else
        {
            for (x86::reg32 k = 0; k < 12; k += 4)
                rc::st32(w + 0x5c + k, rc::ld32(0x9d780c + k));
            tireUnloaded(fsw, w, rc::ldf32(state + 0xc0));
        }
        fsw.set(compression, zero);
        if (double(compression) == zero)
            st(w + 0x68, ld(state + 0xc0) + ld(w + 0x68));
        else
            rc::st32(w + 0x68, 0);
        rc::stf32(w + 0x78, double(compression) > zero ? compression : float(zero));
        app->allowPreemption(cpu);
    }
    // edx and ebx as the translated code leaves them.
    x86::reg32 edx = self + 0x120, ebxNow = 1;
    if (anyGrounded)
    {
        // The wheel forces and their torques about the centre of mass, summed.
        float force[3], torque[3];
        for (int k = 0; k < 3; ++k)
            force[k] = torque[k] = rc::ldf32(0x9d780c + 4 * k);
        for (x86::reg32 i = 0; i < 4; ++i)
        {
            const x86::reg32 w = rc::ld32(self + 0x110 + 4 * i);
            const double px = ld(w + 0x6c);
            const double py = rounded((ld(w + 0x70) + ld(w + 0x78)) - double(ride[i >> 1]));
            const double pz = ld(w + 0x74);
            const float x = float(((pz * ld(state + 0x20) + px * ld(state)) + py * ld(state + 0x10)) +
                                  ld(state + 0x30));
            const float y = float(((pz * ld(state + 0x24) + px * ld(state + 4)) + py * ld(state + 0x14)) +
                                  ld(state + 0x34));
            const float z = float(((px * ld(state + 8) + py * ld(state + 0x18)) + pz * ld(state + 0x28)) +
                                  ld(state + 0x38));
            rc::stf32(w + 0x4c, x);
            rc::stf32(w + 0x50, y);
            rc::stf32(w + 0x54, z);
            ebxNow = floatBitsOf(x);
            const double rx = rounded((double(x) - ld(state + 0xb0)) - ld(state + 0x30));
            const double ry = (double(y) - ld(state + 0xb4)) - ld(state + 0x34);
            const double rz = rounded(double(z) - ld(state + 0xb8)) - ld(state + 0x38);
            const double fx = ld(w + 0x5c), fy = ld(w + 0x60), fz = ld(w + 0x64);
            const float tx = float(ry * fz - rz * fy);
            const float ty = float(rz * fx - rx * fz);
            const float tz = float(rx * fy - ry * fx);
            force[0] = float(double(force[0]) + fx);
            force[1] = float(double(force[1]) + fy);
            force[2] = float(double(force[2]) + fz);
            torque[0] = float(double(tx) + double(torque[0]));
            torque[1] = float(double(ty) + double(torque[1]));
            torque[2] = float(double(tz) + double(torque[2]));
            app->allowPreemption(cpu);
        }
        const double d = (double(torque[2]) * ld(state + 0x18) + double(torque[1]) * ld(state + 0x14)) +
                         double(torque[0]) * ld(state + 0x10);
        const double e = d * ld(rc::ld32(self + 0xa0) + 0x88) - d;
        torque[0] = float(e * ld(state + 0x10) + double(torque[0]));
        torque[1] = float(e * ld(state + 0x14) + double(torque[1]));
        torque[2] = float(e * ld(state + 0x18) + double(torque[2]));
        // Rigid body force and torque (virtual, thiscall: body, force, torque; ret 8).
        for (int k = 0; k < 3; ++k)
        {
            rc::stf32(s + 0x70 + 4 * k, force[k]);
            rc::stf32(s + 0x48 + 4 * k, torque[k]);
        }
        rc::st32(s - 8, s + 0x70);
        rc::st32(s - 4, s + 0x48);
        cpu.eax = s + 0x70;
        cpu.ebx = ebxNow;
        cpu.ebp = s + 0x138;
        cpu.esi = state;
        cpu.edi = self;
        cpu.fpu.count = depth;
        fsw.apply(cpu);
        if (!virtualCall(app, cpu, rc::ld32(self + 0xcc), 0x84, s - 0xc, 0x6fd5ef))
            return;
        edx = cpu.edx;
    }
    if (double(bottomOut) > zero)
    {
        for (x86::reg32 i = 0; i < 4; ++i)
        {
            const x86::reg32 w = rc::ld32(self + 0x110 + 4 * i);
            st(w + 0x50, double(bottomOut) + ld(w + 0x50));
        }
        // Rigid body bottom-out push (virtual, thiscall: body, amount; ret 4).
        rc::stf32(s - 4, bottomOut);
        cpu.eax = floatBitsOf(bottomOut);
        cpu.ebx = ebxNow;
        cpu.ebp = s + 0x138;
        cpu.esi = state;
        cpu.edi = self;
        cpu.fpu.count = depth;
        fsw.apply(cpu);
        if (!virtualCall(app, cpu, rc::ld32(self + 0xcc), 0x7c, s - 8, 0x6fd656))
            return;
        edx = cpu.edx;
    }
    if (grounded != 0 && rc::ld32(self + 0xf8) == 0)
    {
        {
            st(state + 0x94, ld(state + 0x94) * constant(0x9c2888));
            const double v0 = ld(state + 0x90), v1 = ld(state + 0x94), v2 = ld(state + 0x98);
            st(state + 0x60, (v1 * ld(state + 0x10) + v2 * ld(state + 0x20)) + v0 * ld(state));
            st(state + 0x64, (v2 * ld(state + 0x24) + v1 * ld(state + 0x14)) + v0 * ld(state + 4));
            st(state + 0x68, (v2 * ld(state + 0x28) + v1 * ld(state + 0x18)) + v0 * ld(state + 8));
            // Rigid body linear velocity (virtual, thiscall: body, velocity; ret 4).
            rc::st32(s - 4, state + 0x60);
            cpu.eax = state + 0x60;
            cpu.ebx = grounded;
            cpu.ebp = s + 0x138;
            cpu.esi = state;
            cpu.edi = self;
            cpu.fpu.count = depth;
            fsw.apply(cpu);
            if (!virtualCall(app, cpu, rc::ld32(self + 0xcc), 0x64, s - 8, 0x6fd6fd))
                return;
            edx = cpu.edx;
        }
    }
    rc::st32(self + 0xf8, grounded);
    cpu.edx = edx;
    cpu.ebx = ebx;
    cpu.ebp = ebp;
    cpu.esi = esi;
    cpu.edi = edi;
    cpu.fpu.count = depth;
    cpu.esp = entry + 8;
    fsw.apply(cpu);
}

}
}
