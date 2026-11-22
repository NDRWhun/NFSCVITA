// Native replacements of the per-car simulation step: the same double-precision steps and float stores as the
// translated x87 code, leaf callees inlined, other callees called as guest code on the guest's stack.

#include "vehicle.h"
#include "shadow.h"

#include <native.h>
#include <platform/config.h>
#include <recomp.h>

#include <cmath>
#include <cstring>
#include <vector>

namespace nfsc
{

// Translated bodies of the overridden guest functions, and translated functions called from here.
void sub_6c73a0_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_817090_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_7fc550(win32::WinApplication* app, x86::CPU& cpu);
void sub_813d20(win32::WinApplication* app, x86::CPU& cpu);
void sub_403f10(win32::WinApplication* app, x86::CPU& cpu);
void sub_469bc0(win32::WinApplication* app, x86::CPU& cpu);
void sub_6c1ae0(win32::WinApplication* app, x86::CPU& cpu);
void sub_468cc0(win32::WinApplication* app, x86::CPU& cpu);
void sub_431700_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_417c10(win32::WinApplication* app, x86::CPU& cpu);
void sub_4299a0(win32::WinApplication* app, x86::CPU& cpu);
void sub_40bb40(win32::WinApplication* app, x86::CPU& cpu);
void sub_409a30(win32::WinApplication* app, x86::CPU& cpu);

namespace
{

double ld(x86::reg32 address)
{
    return rc::ldf32(address);
}

x86::reg32 floatBits(float value)
{
    x86::reg32 bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
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

// Virtual call of the method at offset of the object's table (thiscall: object), its return address at esp; the
// table in eax or edx as the guest loads it.
bool virtualCall(win32::WinApplication* app, x86::CPU& cpu, x86::reg32 object, x86::reg32 offset, x86::reg32 esp,
                 x86::reg32 ret, bool tableInEax)
{
    const x86::reg32 table = rc::ld32(object);
    cpu.ecx = object;
    if (tableInEax)
        cpu.eax = table;
    else
        cpu.edx = table;
    rc::st32(esp, ret);
    cpu.esp = esp;
    app->dynamic_call(rc::ld32(table + offset), cpu);
    return !cpu.terminate;
}

// The st0 result of a guest call made at site with the x87 depth at depth, popped.
double popResult(x86::CPU& cpu, x86::reg32 depth, x86::reg32 site)
{
    RC_FPU_CALL(cpu, depth + 1, site);
    const double value = cpu.fpu.regs[(depth + 1) & 7];
    cpu.fpu.count = depth;
    return value;
}

// Vector scaled to unit length (0x411d40): zero when its squared length is at most the epsilon.
void normalize(float v[3])
{
    const double x = v[0], y = v[1], z = v[2];
    const float length2 = float((x * x + y * y) + z * z);
    double scale = 0.0;
    if (double(length2) > double(rc::ldf32a(0x9c1760)))
    {
        const double length = std::sqrt(double(length2));
        scale = length == 0.0 ? 0.0 : 1.0 / length;
    }
    v[0] = float(scale * x);
    v[1] = float(scale * y);
    v[2] = float(scale * z);
}

// Cross product a x b in the guest's operand order (0x401d10).
void cross(const float a[3], const float b[3], float out[3])
{
    const float x = float(double(b[2]) * double(a[1]) - double(b[1]) * double(a[2]));
    const float y = float(double(b[0]) * double(a[2]) - double(a[0]) * double(b[2]));
    const float z = float(double(a[0]) * double(b[1]) - double(b[0]) * double(a[1]));
    out[0] = x;
    out[1] = y;
    out[2] = z;
}

// x clamped to high, then raised to low (0x402290); the last compare's status word is the guest's.
double clampLowHigh(x86::CPU& cpu, float x, float low, float high)
{
    const double a = double(x) < double(high) ? double(x) : double(high);
    cpu.fpu.compare(double(low), a);
    return double(low) > a ? double(low) : a;
}

// x raised to low, then clamped to high (0x4010e0).
double clampHighLow(float x, float low, float high)
{
    const double a = double(x) > double(low) ? double(x) : double(low);
    return double(high) < a ? double(high) : a;
}

// x clamped to -limit..limit (0x402250).
double clampSymmetric(float x, float limit)
{
    const double a = double(x) < double(limit) ? double(x) : double(limit);
    const double low = double(-limit);
    return low > a ? low : a;
}

// The runtime's asin (_CIasin) of an argument in range.
double arcSine(double x)
{
    return std::isnan(x) ? x : std::asin(x);
}

// Average::Record (thiscall: average, value), 0x6a2050: a ring of the last values, their sum and mean.
void averageRecord(x86::reg32 a, x86::reg32 bits)
{
    float value;
    memcpy(&value, &bits, sizeof(value));
    const x86::reg8 count = rc::ld8(a + 6);
    if (count < rc::ld8(a + 5))
        rc::st8(a + 6, x86::reg8(count + 1));
    const x86::reg32 slot = rc::ld32(a + 0x10) + 4 * x86::reg32(rc::ld8(a + 7));
    const double sum = double(value) + ld(a + 8);
    rc::stf32(a + 8, float(sum));
    rc::stf32(a + 8, float(sum - ld(slot)));
    rc::st32(slot, bits);
    rc::stf32(a + 0xc, float(ld(a + 8) / double(rc::ld8(a + 6))));
    const x86::reg8 next = x86::reg8(rc::ld8(a + 7) + 1);
    rc::st8(a + 7, next < rc::ld8(a + 5) ? next : 0);
}

// PidError::Record (thiscall: error, value, time step, no derivative, no integral) with both flags clear, 0x6a2370:
// the error and its previous value, then the averages of the time step, the integral and the derivative.
void pidRecord(x86::reg32 pid, float error, x86::reg32 dtBits)
{
    float dt;
    memcpy(&dt, &dtBits, sizeof(dt));
    rc::st32(pid + 0x80, rc::ld32(pid + 0x7c));
    const double delta = double(error) - ld(pid + 0x80);
    rc::st32(pid + 0x7c, floatBits(error));
    const float integral = float((0.5 * delta + ld(pid + 0x80)) * double(dt));
    const float derivative = float(delta / double(dt));
    averageRecord(pid, dtBits);
    averageRecord(pid + 0x28, floatBits(integral));
    averageRecord(pid + 0x50, floatBits(derivative));
}

// A PID gain (0x4039d0): the count at +0x2e times +0x30 over +0x78 times +8, 0 without a count.
double pidGain(x86::reg32 pid)
{
    const x86::reg8 count = rc::ld8(pid + 0x2e);
    if (!count)
        return 0.0;
    return (double(count) * ld(pid + 0x30)) / (ld(pid + 0x78) * ld(pid + 8));
}

// Table::GetValue (thiscall: table, x), 0x6a1900: linear interpolation in evenly spaced values (count +0, start +4,
// inverse step +0xc, values +0x10).
double tableValue(x86::reg32 table, float x)
{
    const double position = (double(x) - ld(table + 4)) * ld(table + 0xc);
    const float stored = float(position);
    const x86::reg32 values = rc::ld32(table + 0x10);
    const x86::sreg32 i = x86::sreg32(x86::reg32(x86::truncInteger64(position)));
    if (i < 0 || double(stored) < 0.0)
        return ld(values);
    const x86::reg32 count = rc::ld32(table);
    if (i >= x86::sreg32(count - 1))
        return ld(values + 4 * count - 4);
    double base = double(i);
    if (base > double(stored))
        base -= 1.0;
    const double t = double(stored) - base;
    const x86::reg32 at = values + 4 * x86::reg32(i);
    return (1.0 - t) * ld(at) + t * ld(at + 4);
}

// Piecewise-linear table of (x, y) pairs (thiscall: table, x), 0x6a1ce0: pairs at [+0], count at +4.
double pairValue(x86::reg32 table, float x)
{
    const x86::reg32 count = rc::ld32(table + 4);
    const x86::reg32 pairs = rc::ld32(table);
    if (x86::sreg32(count) <= 1)
        return ld(pairs + 4);
    const double v = x;
    if (!(v > ld(pairs) || std::isnan(v)))
        return ld(pairs + 4);
    if (!(v < ld(pairs + 8 * count - 8) || std::isnan(v)))
        return ld(pairs + 8 * count - 4);
    for (x86::reg32 k = 0; k < count - 1; ++k)
    {
        const x86::reg32 p = pairs + 8 * k;
        if (v < ld(p) || std::isnan(v) || !(v < ld(p + 8)))
            continue;
        const double dy = ld(p + 0xc) - ld(p + 4);
        const double dx = ld(p + 8) - ld(p);
        const double adx = dx < 0.0 ? -dx : dx;
        if (!(adx > double(rc::ldf32a(0x9cb5d0))))
            return dy * 0.5 + ld(p + 4);
        return dy * ((v - ld(p)) / dx) + ld(p + 4);
    }
    return ld(pairs + 4);
}

// A port group's switches: <name>_native (default on) and shadow_<name>.
struct Switch
{
    const char* key;
    int on = -1;

    bool native()
    {
        if (on < 0)
            on = pf::Config::getBool(key, true) ? 1 : 0;
        return on != 0;
    }
};

Switch s_wheel{ "wheel_native" };
shadow::Group s_wheelShadow("shadow_wheel");
Switch s_steering{ "steering_native" };
shadow::Group s_steeringShadow("shadow_steering");

// WWorldPos::Update (thiscall: position, point, ground plane out, keep cached face, collider, keep face) with its
// return address at entry: the face under the point (the collider's face lists, else the world's), the plane's
// normal and the point's height above it. Sets eax (al: a face is held), ecx and edx as the translated body leaves
// them; ebx, ebp, esi and edi come back as given.
bool worldPosUpdate(win32::WinApplication* app, x86::CPU& cpu, x86::reg32 self, x86::reg32 point, x86::reg32 plane,
                    x86::reg32 keep, x86::reg32 collider, x86::reg8 keepFace, x86::reg32 entry)
{
    const x86::reg32 ebx = cpu.ebx, ebp = cpu.ebp, esi = cpu.esi, edi = cpu.edi;
    const x86::reg32 had = rc::ld8(self + 0x30) & 1;
    // Closest face search (thiscall: position, face lists or 0, point, keep cached face; ret 0xc).
    const x86::reg32 s = entry - 0x1c;
    rc::st32(s + 4, collider ? collider + 0x5c : 0);
    rc::st32(s + 8, point);
    rc::st32(s + 0xc, keep);
    cpu.ecx = self;
    cpu.ebx = (ebx & 0xffffff00u) | had;
    cpu.ebp = point;
    cpu.esi = self;
    if (collider)
    {
        cpu.eax = collider + 0x5c;
        if (!guestCall(app, cpu, &sub_7fc550, s, 0x8170b8))
            return false;
    }
    else
    {
        cpu.eax = 0;
        cpu.edx = keep;
        if (!guestCall(app, cpu, &sub_813d20, s, 0x8170c9))
            return false;
    }
    x86::reg32 eax = cpu.eax, ecx = cpu.ecx;
    const x86::reg8 found = x86::reg8(eax);
    auto finish = [&](x86::reg8 result) {
        cpu.eax = (eax & 0xffffff00u) | result;
        cpu.ecx = ecx;
        cpu.ebx = ebx;
        cpu.ebp = ebp;
        cpu.esi = esi;
        cpu.edi = edi;
        cpu.esp = entry + 0x18;
    };
    if (!(rc::ld32(self + 0x30) & 1))
    {
        ecx = rc::ld32(0xb4d920);
        rc::st32(self + 0x38, ecx);
        ecx = (ecx & 0xffffff00u) | keepFace;
        if (!keepFace || !had)
        {
            cpu.edx = plane;
            rc::st32(plane + 0xc, 0xbf800000u);
            finish(0);
            return true;
        }
        rc::st32(self + 0x30, rc::ld32(self + 0x30) | 1);
    }
    if (found)
    {
        if (rc::ld32(self + 0x30) & 1)
        {
            // Face plane normal (thiscall: face, normal out; ret 4).
            rc::st32(entry - 0x14, plane);
            cpu.eax = eax;
            cpu.ecx = self;
            cpu.ebx = (ebx & 0xffffff00u) | had;
            cpu.ebp = point;
            cpu.esi = self;
            cpu.edi = plane;
            if (!guestCall(app, cpu, &sub_403f10, entry - 0x18, 0x8170ff))
                return false;
            eax = cpu.eax;
            ecx = cpu.ecx;
        }
        else
        {
            rc::st32(plane + 8, 0);
            rc::st32(plane, 0);
            rc::st32(plane + 4, 0x3f800000u);
        }
        rc::st32(plane + 0xc, 0);
    }
    const double h = ((ld(self + 8) - ld(point + 8)) * ld(plane + 8) + (ld(self + 4) - ld(point + 4)) * ld(plane + 4)) +
                     (ld(self) - ld(point)) * ld(plane);
    rc::stf32(plane + 0xc, float(h));
    finish(1);
    return true;
}

}

namespace vehicle
{

void logShadow()
{
    s_wheelShadow.log();
    s_steeringShadow.log();
}

}

namespace native
{

// WWorldPos::Update (thiscall: position, point, ground plane out, keep cached face, collider, keep face; ret
// 0x14), 0x817090.
void sub_817090(win32::WinApplication* app, x86::CPU& cpu)
{
    const x86::reg32 entry = cpu.esp;
    if (s_wheelShadow.active())
    {
        // A call without a collider is not repeated: the world face search keeps its temporary lists in allocated
        // blocks, and a second search can store pointers into other blocks.
        const x86::reg32 collider = rc::ld32(entry + 0x10);
        auto repeatable = [](const void* context) { return *static_cast<const x86::reg32*>(context) != 0; };
        s_wheelShadow.call("WWorldPos::Update 0x817090", app, cpu, &sub_817090_guest, &sub_817090,
                           { { cpu.ecx, 0x3c }, { rc::ld32(entry + 8), 0x10 }, { entry, 0x18 } }, 0xfb, repeatable,
                           &collider);
        return;
    }
    if (!s_wheel.native())
    {
        sub_817090_guest(app, cpu);
        return;
    }
    worldPosUpdate(app, cpu, cpu.ecx, rc::ld32(entry + 4), rc::ld32(entry + 8), rc::ld32(entry + 0xc),
                   rc::ld32(entry + 0x10), rc::ld8(entry + 0x14), entry);
}

// Wheel::UpdatePosition (thiscall: wheel, angular velocity, linear velocity, body matrix, centre of mass, time
// step, radius, probe, collider, maximum probe depth; ret 0x24), 0x6c73a0: the wheel's world position and contact
// point velocity, the ground probe distance, the world position update (WWorldPos::Update inlined) and the ground
// surface (SimSurface and Wheel::UpdateSurface inlined unless the wheel's surface must change).
void sub_6c73a0(win32::WinApplication* app, x86::CPU& cpu)
{
    const x86::reg32 entry = cpu.esp;
    const x86::reg32 w = cpu.ecx;
    if (s_wheelShadow.active())
    {
        // The wheel and the argument area (the guest keeps temporaries there). A translated run that changed the
        // wheel's surface or deleted the surface collection it held is not repeated (reference counts outside),
        // nor one without a collider (the world face search's temporary lists come from the allocator).
        struct Surface
        {
            x86::reg32 wheel, before, collider;
        };
        const Surface surface{ w, rc::ld32(w + 0x9c), rc::ld32(entry + 0x20) };
        auto repeatable = [](const void* context) {
            const Surface& s = *static_cast<const Surface*>(context);
            if (!s.collider || rc::ld32(s.wheel + 0x9c) != s.before)
                return false;
            const x86::reg32 held = rc::ld32(s.wheel + 0x38) ? rc::ld32(s.wheel + 0x38) : rc::ld32(0xb4d868);
            return held == 0 || rc::ld16(held + 8) != 0;
        };
        s_wheelShadow.call("Wheel::UpdatePosition 0x6c73a0", app, cpu, &sub_6c73a0_guest, &sub_6c73a0,
                           { { w, 0xb0 }, { entry, 0x28 } }, 0xfb, repeatable, &surface);
        return;
    }
    if (!s_wheel.native())
    {
        sub_6c73a0_guest(app, cpu);
        return;
    }
    const x86::reg32 ebx = cpu.ebx, ebp = cpu.ebp, esi = cpu.esi, edi = cpu.edi;
    const x86::reg32 angular = rc::ld32(entry + 4), linear = rc::ld32(entry + 8), m = rc::ld32(entry + 0xc);
    const x86::reg32 centre = rc::ld32(entry + 0x10);
    const double dt = ld(entry + 0x14);
    // The exception frame.
    const x86::reg32 handler = rc::ld32(cpu.efs);
    rc::st32(entry - 4, 0xffffffffu);
    rc::st32(entry - 8, 0x9a6708);
    rc::st32(entry - 0xc, handler);
    rc::st32(cpu.efs, entry - 0xc);
    // Local position rotated by the body matrix (0x401b50), then the world position.
    {
        const double x = ld(w + 0x6c), y = ld(w + 0x70), z = ld(w + 0x74);
        rc::stf32(w + 0x7c, float((z * ld(m + 0x20) + y * ld(m + 0x10)) + x * ld(m)));
        rc::stf32(w + 0x80, float((z * ld(m + 0x24) + y * ld(m + 0x14)) + x * ld(m + 4)));
        rc::stf32(w + 0x84, float((z * ld(m + 0x28) + y * ld(m + 0x18)) + x * ld(m + 8)));
    }
    rc::stf32(w + 0x4c, float(ld(m + 0x30) + ld(w + 0x7c)));
    rc::stf32(w + 0x50, float(ld(m + 0x34) + ld(w + 0x80)));
    rc::stf32(w + 0x54, float(ld(m + 0x38) + ld(w + 0x84)));
    // Contact point velocity: angular velocity x (offset from the centre of mass) + linear velocity.
    const double r0 = ld(w + 0x7c) - ld(centre), r1 = ld(w + 0x80) - ld(centre + 4);
    const double r2 = ld(w + 0x84) - ld(centre + 8);
    const double a0 = ld(angular), a1 = ld(angular + 4), a2 = ld(angular + 8);
    const float cx = float(r2 * a1 - r1 * a2);
    const float cy = float(r0 * a2 - r2 * a0);
    const float cz = float(r1 * a0 - r0 * a1);
    const float vx = float(double(cx) + ld(linear));
    const float vy = float(double(cy) + ld(linear + 4));
    const float vz = float(double(cz) + ld(linear + 8));
    rc::stf32(w + 0x8c, vx);
    rc::stf32(w + 0x90, vy);
    rc::stf32(w + 0x94, vz);
    // Probe distance: the downward travel, the horizontal travel and the radius, at most half the maximum.
    const double down = -(double(vy) * dt);
    const float fall = down > 0.0 ? float(down) : 0.0f;
    rc::stf32(entry + 0xc, fall);
    const float horizontal2 = float(double(vz) * double(vz) + double(vx) * double(vx));
    rc::stf32(entry + 0x10, horizontal2);
    const double probe = (std::sqrt(double(horizontal2)) * dt + double(fall)) + ld(entry + 0x18);
    const float limit = float(ld(entry + 0x24) * 0.5);
    rc::stf32(entry + 0x14, limit);
    rc::stf32(w + 0x34, probe < double(limit) ? float(probe) : limit);
    // The keep-cached-face argument as the guest pushes it: the status word of the compression test in its low
    // half, the squared horizontal travel's high half above.
    const double compression = ld(w + 0x78);
    cpu.fpu.compare(compression, 0.0);
    const x86::reg8 keepCached = compression > 0.0 && rc::ld8(entry + 0x1c) != 0 ? 1 : 0;
    const x86::reg32 keep = ((floatBits(horizontal2) & 0xffff0000u) | x86::reg16(cpu.fpu.status.word)) & 0xffffff00u;
    cpu.ebx = w + 0x4c;
    cpu.ebp = m;
    cpu.esi = w;
    cpu.edi = w + 0x7c;
    cpu.edx = w + 0x3c;
    if (!worldPosUpdate(app, cpu, w, w + 0x4c, w + 0x3c, keep | keepCached, rc::ld32(entry + 0x20), 1, entry - 0x50))
        return;
    const x86::reg8 result = x86::reg8(cpu.eax);
    // SimSurface of the position's surface collection, in the guest frame (Attrib::Instance: collection, layout,
    // message port, flags).
    const x86::reg32 surface = entry - 0x1c;
    const x86::reg32 collection = rc::ld32(w + 0x38);
    x86::reg32 layout = 0, flags = 0;
    if (collection)
    {
        flags = rc::ld32(collection + 0x1c) == 0 ? 1 : 0;
        layout = rc::ld32(collection + 0x18);
        rc::st16(collection + 8, x86::reg16(rc::ld16(collection + 8) + 1));
    }
    rc::st32(surface, collection);
    rc::st32(surface + 4, layout ? layout : 0xa989d8);
    rc::st32(surface + 8, 0);
    rc::st32(surface + 0xc, flags);
    if (!collection)
    {
        // Attrib::Instance::Change to the default surface (thiscall: instance, collection; ret 4).
        rc::st32(entry - 0x58, rc::ld32(0xb4d868));
        cpu.eax = surface;
        cpu.ecx = surface;
        if (!guestCall(app, cpu, &sub_469bc0, entry - 0x5c, 0x75d814))
            return;
    }
    rc::st32(entry - 4, 0);
    // Wheel::UpdateSurface: a wheel on a surface keeps it and takes its grip; otherwise the guest changes it.
    const double grip = ld(w + 0xac);
    if (grip > 0.0 || std::isnan(grip))
    {
        if (rc::ld32(surface) == rc::ld32(w + 0x9c))
            rc::st32(w + 0xac, rc::ld32(rc::ld32(surface + 4) + 0xf8));
    }
    else
    {
        rc::st32(entry - 0x3c, surface);
        cpu.eax = surface;
        cpu.ecx = w;
        cpu.ebx = ((w + 0x4c) & 0xffffff00u) | result;
        if (!guestCall(app, cpu, &sub_6c1ae0, entry - 0x40, 0x6c7556))
            return;
    }
    rc::st32(entry - 4, 0xffffffffu);
    // ~SimSurface: the collection's reference released, deleted at zero.
    x86::reg32 eax = 0;
    const x86::reg32 held = rc::ld32(surface);
    if (held)
    {
        const x86::reg16 count = rc::ld16(held + 8);
        if (count > 1)
        {
            rc::st16(held + 8, x86::reg16(count - 1));
            eax = held;
        }
        else
        {
            // Attrib::Database::Delete (cdecl: collection, database).
            rc::st16(held + 8, 0);
            const x86::reg32 database = rc::ld32(0xa8499c);
            cpu.edx = rc::ld32(database + 4) + 0x44;
            cpu.ecx = database;
            cpu.eax = held;
            rc::st32(entry - 0x44, held);
            rc::st32(entry - 0x40, cpu.edx);
            if (!guestCall(app, cpu, &sub_468cc0, entry - 0x48, 0x4698a1))
                return;
            eax = cpu.eax;
        }
    }
    rc::st32(cpu.efs, handler);
    cpu.eax = (eax & 0xffffff00u) | result;
    cpu.ecx = handler;
    cpu.ebx = ebx;
    cpu.ebp = ebp;
    cpu.esi = esi;
    cpu.edi = edi;
    cpu.esp = entry + 0x28;
}

// AIVehiclePid::OnSteering (thiscall: AI, time step; ret 4), 0x431700: the heading errors towards the target point
// (two PID error records), the steering controller's gains and inputs, and the clamped steering. The leaf maths
// (vector helpers, clamps, asin, PID records, gain tables) is inlined; virtual methods, the controller update and
// the gain sums run as guest code.
void sub_431700(win32::WinApplication* app, x86::CPU& cpu)
{
    const x86::reg32 entry = cpu.esp;
    const x86::reg32 self = cpu.ecx;
    if (s_steeringShadow.active())
    {
        // The AI, its PID errors with their averages' value rings, the steering controller with its averaging
        // windows (object, value ring, time ring); eax ebx esp ebp esi edi.
        std::vector<shadow::Region> regions = { { self, 0x200 }, { entry, 8 } };
        for (const x86::reg32 at : { 0x1ccu, 0x1d0u })
        {
            const x86::reg32 object = rc::ld32(self + at);
            if (!object)
                continue;
            regions.push_back({ object, 0xc0 });
            for (x86::reg32 average = 0; average < 0x78; average += 0x28)
            {
                if (rc::ld32(object + average + 0x10))
                    regions.push_back({ rc::ld32(object + average + 0x10), 4 * x86::reg32(rc::ld8(object + average + 5)) });
            }
        }
        const x86::reg32 control = rc::ld32(self + 0x1f4);
        if (control)
        {
            regions.push_back({ control, 0xc0 });
            const x86::reg32 windows[] = { rc::ld32(control + 0x64), rc::ld32(control + 0x68), rc::ld32(control + 0x6c),
                                           control + 0x74 };
            for (const x86::reg32 window : windows)
            {
                if (!window)
                    continue;
                const x86::reg32 size = 4 * x86::reg32(rc::ld8(window + 5));
                regions.push_back({ window, 0x34 });
                for (const x86::reg32 ring : { rc::ld32(window + 0x10), rc::ld32(window + 0x30) })
                {
                    if (ring)
                        regions.push_back({ ring, size });
                }
            }
        }
        // A translated run that left the time step argument unchanged took AIVehicle::OnSteering's path (its own
        // guest state outside these regions) or returned before the heading errors: it is not repeated.
        struct Argument
        {
            x86::reg32 at, before;
        };
        const Argument argument{ entry + 4, rc::ld32(entry + 4) };
        auto repeatable = [](const void* context) {
            const Argument& a = *static_cast<const Argument*>(context);
            return rc::ld32(a.at) != a.before;
        };
        s_steeringShadow.call("AIVehiclePid::OnSteering 0x431700", app, cpu, &sub_431700_guest, &sub_431700, regions,
                              0xf9, repeatable, &argument);
        return;
    }
    if (!s_steering.native())
    {
        sub_431700_guest(app, cpu);
        return;
    }
    const x86::reg32 ebx = cpu.ebx, ebp = cpu.ebp, esi = cpu.esi, edi = cpu.edi;
    const x86::reg32 depth = cpu.fpu.count;
    const x86::reg32 ai = self + 0x44;
    const x86::reg32 dtBits = rc::ld32(entry + 4);
    auto finish = [&]() {
        cpu.ebx = ebx;
        cpu.ebp = ebp;
        cpu.esi = esi;
        cpu.edi = edi;
        cpu.esp = entry + 8;
        cpu.fpu.count = depth;
    };
    cpu.esi = self;
    cpu.edi = ai;
    if (!virtualCall(app, cpu, ai, 0x14, entry - 0x4c, 0x431712, true))
        return;
    if (x86::reg8(cpu.eax))
    {
        // AIVehicle::OnSteering (thiscall: AI, time step; ret 4).
        rc::st32(entry - 0x4c, dtBits);
        cpu.ecx = self;
        cpu.edx = dtBits;
        if (!guestCall(app, cpu, &sub_417c10, entry - 0x50, 0x431b7f))
            return;
        cpu.esi = esi;
        cpu.edi = edi;
        cpu.esp = entry + 8;
        cpu.fpu.count = depth;
        return;
    }
    if (!virtualCall(app, cpu, ai, 0x18, entry - 0x4c, 0x431721, false))
        return;
    if (!(cpu.eax & 1))
    {
        finish();
        return;
    }
    const x86::reg32 target = rc::ld32(self + 0x100);
    if (!target)
    {
        cpu.eax = 0;
        finish();
        return;
    }
    rc::st32(self + 0x108, 0);
    rc::st32(self + 0x1d8, 0);
    cpu.ebx = 0;
    if (!virtualCall(app, cpu, ai, 4, entry - 0x54, 0x431752, true))
        return;
    const x86::reg32 vehicle = cpu.eax;
    cpu.ebp = vehicle;
    if (!virtualCall(app, cpu, vehicle, 0x54, entry - 0x54, 0x43175c, false))
        return;
    const x86::reg32 body = cpu.eax;
    cpu.edi = body;
    if (!virtualCall(app, cpu, body, 0x30, entry - 0x54, 0x431765, true))
        return;
    const float speed = float(popResult(cpu, depth, 0x431762));
    const double held = ld(self + 0x94);
    cpu.fpu.compare(held, 0.0);
    if (held == 0.0)
    {
        cpu.fpu.compare(double(speed), 1.0);
        if (double(speed) < 1.0)
        {
            cpu.eax = (cpu.eax & 0xffff0000u) | x86::reg16(cpu.fpu.status.word);
            finish();
            return;
        }
    }
    // Direction to the target point, the body's forward direction and its velocity, flattened and normalised.
    if (!virtualCall(app, cpu, vehicle, 0x60, entry - 0x54, 0x43179b, false))
        return;
    const x86::reg32 position = cpu.eax;
    float toTarget[3] = { float(ld(self + 0x98) - ld(position)), 0.0f, float(ld(self + 0xa0) - ld(position + 8)) };
    normalize(toTarget);
    const x86::reg32 forwardAt = entry - 0x30;
    rc::st32(entry - 0x54, forwardAt);
    if (!virtualCall(app, cpu, body, 0x34, entry - 0x58, 0x4317d8, false))
        return;
    float forward[3] = { rc::ldf32(forwardAt), 0.0f, rc::ldf32(forwardAt + 8) };
    normalize(forward);
    if (!virtualCall(app, cpu, body, 0x24, entry - 0x54, 0x4317f7, true))
        return;
    const x86::reg32 velocityAt = cpu.eax;
    float velocity[3] = { rc::ldf32(velocityAt), 0.0f, rc::ldf32(velocityAt + 8) };
    if (double(speed) > double(rc::ldf32a(0x9c2938)))
        normalize(velocity);
    // Heading: forward blended towards the velocity by speed (0..1), normalised.
    const float blend = float(clampLowHigh(cpu, speed, 0.0f, 1.0f));
    const double rest = 1.0 - double(blend);
    float heading[3];
    for (int k = 0; k < 3; ++k)
        heading[k] = float(double(forward[k]) * rest);
    for (int k = 0; k < 3; ++k)
        heading[k] = float(double(blend) * double(velocity[k]) + double(heading[k]));
    normalize(heading);
    // Heading errors: asin of the cross products' y, recorded in the two PID errors.
    float side[3];
    cross(forward, toTarget, side);
    const float error1 = float(arcSine(clampSymmetric(side[1], 1.0f)));
    const x86::reg32 pid1 = rc::ld32(self + 0x1cc);
    pidRecord(pid1, error1, dtBits);
    cross(heading, toTarget, side);
    const float error2 = float(arcSine(clampSymmetric(side[1], 1.0f)));
    pidRecord(rc::ld32(self + 0x1d0), error2, dtBits);
    const x86::reg32 pid2 = rc::ld32(self + 0x1d0);
    const x86::reg32 first = rc::ld32(self + 0x1cc);
    const float error = float(ld(pid2 + 0x7c) * 0.0 + ld(first + 0x7c));
    rc::stf32(entry + 4, error);
    const float gain2 = float(pidGain(pid2) * 0.0);
    const float gain = float(clampHighLow(float(pidGain(first) + double(gain2)), -0.5f, 0.5f));
    const double integral = clampHighLow(float(ld(pid2 + 0x5c) * 0.0 + ld(first + 0x5c)), -10.0f, 10.0f);
    const double product = double(error) * integral;
    cpu.fpu.compare(product, 0.0);
    const double magnitude = std::fabs(integral);
    const float signedIntegral = float(product >= 0.0 ? magnitude : -magnitude);
    // Steering controller inputs and update (thiscall: controller, error, time step, limit; ret 0xc).
    x86::reg32 control = rc::ld32(self + 0x1f4);
    rc::st32(control + 0xac, floatBits(error));
    rc::st32(rc::ld32(self + 0x1f4) + 0xb0, floatBits(gain));
    rc::stf32(rc::ld32(self + 0x1f4) + 0xb4, float(integral));
    const double scale = rc::ldf32a(0x9c2474);
    const double tableGain = pairValue(0xa83c6c, float(std::fabs(double(error) * scale)));
    rc::stf32(entry - 0x5c, float(double(signedIntegral) * scale - tableGain));
    rc::st32(entry - 0x58, dtBits);
    rc::st32(entry - 0x54, 0xc7c34f80u);
    cpu.ecx = rc::ld32(self + 0x1f4);
    cpu.ebx = dtBits;
    cpu.ebp = first;
    cpu.edi = pid2;
    if (!guestCall(app, cpu, &sub_4299a0, entry - 0x60, 0x431a20))
        return;
    if (double(speed) < double(rc::ldf32a(0x9c18cc)))
    {
        const float r1 = float(tableValue(0xa4c380, speed));
        const float r2 = float(tableValue(0xa4c358, speed));
        const float r3 = float(tableValue(0xa4c36c, speed));
        rc::stf32(entry + 4, r2);
        rc::stf32(rc::ld32(self + 0x1f4) + 0x58, r2);
        rc::stf32(rc::ld32(self + 0x1f4) + 0x5c, r1);
        rc::stf32(rc::ld32(self + 0x1f4) + 0x60, r3);
        cpu.edi = floatBits(speed);
    }
    // The target's turning radius and the controller's output give the steering, scaled by the AI.
    if (!virtualCall(app, cpu, target, 0x6c, entry - 0x54, 0x431a8a, false))
        return;
    const float radius = float(popResult(cpu, depth, 0x431a87));
    rc::stf32(entry + 4, radius);
    cpu.ecx = rc::ld32(self + 0x1f4);
    if (!guestCall(app, cpu, &sub_40bb40, entry - 0x54, 0x431a99))
        return;
    const double output = popResult(cpu, depth, 0x431a94);
    const float steer = float(output / (double(radius) * double(rc::ldf32a(0x9c1758))));
    rc::stf32(entry + 4, steer);
    rc::st32(entry - 0x54, floatBits(steer));
    cpu.ecx = self;
    if (!guestCall(app, cpu, &sub_409a30, entry - 0x58, 0x431ab5))
        return;
    const float factor = float(popResult(cpu, depth, 0x431ab0));
    const x86::reg32 other = rc::ld32(self + 0xfc);
    rc::st8(self + 0xbd, 0);
    float result = steer;
    bool reversing = false;
    if (other)
    {
        if (!virtualCall(app, cpu, other, 0x14, entry - 0x54, 0x431acf, false))
            return;
        reversing = x86::reg8(cpu.eax) != 0;
    }
    if (reversing)
    {
        if (!virtualCall(app, cpu, ai, 8, entry - 0x54, 0x431adc, true))
            return;
        if (!virtualCall(app, cpu, cpu.eax, 0xa4, entry - 0x54, 0x431ae6, false))
            return;
        const double g = popResult(cpu, depth, 0x431ae0);
        cpu.fpu.compare(g, 0.0);
        if (g < 0.0)
        {
            cpu.fpu.compare(double(steer), 0.0);
            result = double(steer) < 0.0 ? 1.0f : -1.0f;
        }
        else
        {
            result = 0.0f;
        }
    }
    else
    {
        if (!virtualCall(app, cpu, self + 0x4c, 0x1c, entry - 0x54, 0x431b2b, true))
            return;
        const double h = popResult(cpu, depth, 0x431b28);
        cpu.fpu.compare(0.0, h);
        if (h == 0.0)
            result = float(double(factor) * double(steer));
    }
    rc::stf32(entry + 4, result);
    const x86::reg32 high = cpu.eax & 0xffff0000u;
    const float clamped = float(clampLowHigh(cpu, result, -1.0f, 1.0f));
    rc::stf32(self + 0x108, clamped);
    rc::stf32(self + 0x1d8, clamped);
    cpu.eax = high | x86::reg16(cpu.fpu.status.word);
    finish();
}

}
}
