// Native front end object update: FEPackage::UpdateObject with its groups, script tracks and key interpolation in
// one pass; its other callees run as guest calls. feng_native=0 runs the translated code.

#include "feng.h"

#include <native.h>
#include <platform/config.h>
#include <platform/log.h>
#include <recomp.h>

#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <vector>

namespace nfsc
{

// Translated body of the overridden function and the guest functions called from here.
void sub_5fcdc0_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_5f7b90(win32::WinApplication* app, x86::CPU& cpu);
void sub_5f2e40(win32::WinApplication* app, x86::CPU& cpu);
void sub_5f1720(win32::WinApplication* app, x86::CPU& cpu);
void sub_5e93b0(win32::WinApplication* app, x86::CPU& cpu);
void sub_5f3800(win32::WinApplication* app, x86::CPU& cpu);
void sub_5f06a0(win32::WinApplication* app, x86::CPU& cpu);

namespace
{

// Frame counter, the last frame an object was updated in and the updates in that frame.
const x86::reg32 FrameCounter = 0x00ab0a78;
const x86::reg32 UpdateFrame = 0x00a980a0;
const x86::reg32 UpdateRepeats = 0x00a9809c;
// FEngine's flag mask (all bits set while it forces flags on).
const x86::reg32 FlagMask = 0x00a60da8;
// Constants at 0x9c2be4, 0x9c248c, 0x9c2478 and 0x9c2888.
const float KeySpanMinimum = 9.999999747378752e-06f;
const float Zero = 0.0f;
const float One = 1.0f;
const float Half = 0.5f;

// Object flags.
const x86::reg32 FlagScriptRunning = 0x800000;
const x86::reg32 FlagAnimating = 0x1000000;
const x86::reg32 FlagsDirtyAny = 0x1c00000;
const x86::reg32 FlagDirty = 0x2000000;
const x86::reg32 FlagForceTracks = 0x400000;

int s_native = -1;

bool fengNative()
{
    if (s_native < 0)
        s_native = pf::Config::getBool("feng_native", true) ? 1 : 0;
    return s_native != 0;
}

x86::reg32 floatBits(float value)
{
    x86::reg32 bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

// Track: type +0, size +1, interpolation +2, action +3, length and output word offset +4, base key +8, delta keys
// +0x2c; key node: next +4, previous +8, time +0xc, value +0x10.
x86::reg32 trackLength(x86::reg32 track)
{
    return x86::reg32(x86::sreg32(rc::ld32(track + 4) << 8) >> 8);
}

x86::reg32 trackOutput(x86::reg32 data, x86::reg32 track)
{
    return data + 4 * x86::reg32(x86::sreg32(int8_t(rc::ld8(track + 7))));
}

// First delta key: the list head at +0x30, through the reference chain when +0x2c is set.
x86::reg32 firstKey(x86::reg32 track)
{
    if (!rc::ld8(track + 0x2c))
        return rc::ld32(track + 0x30);
    x86::reg32 ref = rc::ld32(track + 0x30);
    while (rc::ld8(ref + 4))
        ref = rc::ld32(ref + 8);
    return rc::ld32(ref + 8);
}

// First key at or after time, else the last one, from node.
x86::reg32 keyFrom(x86::reg32 node, x86::reg32 time)
{
    while (rc::ld32(node + 4) && x86::sreg32(rc::ld32(node + 0xc)) < x86::sreg32(time))
        node = rc::ld32(node + 4);
    return node;
}

// 0x5ea190: the key for time, the base key for negative times or without delta keys.
x86::reg32 findKey(x86::reg32 track, x86::reg32 time)
{
    if (x86::sreg32(time) <= -1)
        return track + 8;
    const x86::reg32 head = firstKey(track);
    return head ? keyFrom(head, time) : track + 8;
}

// 0x5ea1e0: the delta key for time, 0 without delta keys.
x86::reg32 findDeltaKey(x86::reg32 track, x86::reg32 time)
{
    const x86::reg32 head = firstKey(track);
    return head ? keyFrom(head, time) : 0;
}

int s_shadow = -1;
bool s_shadowRunning = false;
uint32_t s_shadowCompared = 0, s_shadowDiffer = 0;

bool shadowOn()
{
    if (s_shadow < 0)
        s_shadow = pf::Config::getBool("shadow_feng", false) ? 1 : 0;
    return s_shadow != 0 && !s_shadowRunning;
}

// Logs a difference of the current call while fewer than 16 calls differed before it.
void shadowReport(const char* what, x86::reg32 guest, x86::reg32 native, int& found)
{
    ++found;
    if (s_shadowDiffer < 16)
        pf::logInfo("shadow_feng: object update call %u: %s translated 0x%08x native 0x%08x",
                    unsigned(s_shadowCompared), what, unsigned(guest), unsigned(native));
}

struct ShadowRegion
{
    x86::reg32 address, size;
};

// Registers UpdateObject leaves: eax and edx the final flags (eax with the dirty bit), ecx its first stack slot.
struct Result
{
    x86::reg32 eax, ecx, edx;
};

// One FEPackage's object updates (FEPackage::Update, UpdateGroup and UpdateObject callers).
struct Update
{
    win32::WinApplication* app;
    x86::CPU& cpu;
    x86::reg32 package;

    // Calls a guest function on a copy of the CPU with esp at the guest's call, the return address and the stack
    // arguments written there; false when the thread is to stop.
    bool call(win32::MethodPtr function, x86::reg32 esp, x86::reg32 returnAddress, x86::reg32 ecx,
              std::initializer_list<x86::reg32> arguments)
    {
        x86::reg32 slot = esp + 4;
        for (x86::reg32 argument : arguments)
        {
            rc::st32(slot, argument);
            slot += 4;
        }
        rc::st32(esp, returnAddress);
        x86::CPU guest = cpu;
        guest.ecx = ecx;
        guest.esp = esp;
        function(app, guest);
        cpu.fpu = guest.fpu;
        if (guest.terminate)
        {
            cpu.terminate = true;
            return false;
        }
        return true;
    }

    // FEPackage::IssueScriptMessages (0x5f7b90, thiscall: engine, object, script, from, to) from UpdateObject.
    bool messages(x86::reg32 esp0, x86::reg32 returnAddress, x86::reg32 object, x86::reg32 script, x86::reg32 from,
                  x86::reg32 to)
    {
        return call(&sub_5f7b90, esp0 - 0x2c, returnAddress, package,
                    { rc::ld32(package + 0x30), object, script, from, to });
    }

    void copy(x86::reg32 from, x86::reg32 to, x86::reg32 size);
    void step(x86::reg32 track, x86::reg32 time, x86::reg32 out);
    void addKey(x86::reg32 track, x86::reg32 value, x86::reg32 out);
    void baseValue(x86::reg32 track, x86::reg32 out);
    bool blend(x86::reg32 track, x86::reg32 from, x86::reg32 to, float t, x86::reg32 out, x86::reg32 esp);
    bool linear(x86::reg32 track, x86::reg32 time, x86::reg32 out, x86::reg32 esp);
    bool interpolate(x86::reg32 track, x86::reg32 time, x86::reg32 out, x86::reg32 esp);
    bool advance(x86::reg32 track, x86::reg32 time, x86::reg32 out, x86::reg32 esp);
    bool tracks(x86::reg32 object, x86::reg32 script, x86::reg32 esp);
    bool group(x86::reg32 object, x86::reg32 tick, x86::reg32 esp);
    bool object(x86::reg32 object, x86::reg32 tick, x86::reg32 esp0, Result& result);
    bool child(x86::reg32 object, x86::reg32 tick, x86::reg32 esp0, x86::reg32 returnAddress, Result& result);
    bool shadowEligible(x86::reg32 object);
    bool shadowObject(x86::reg32 object, x86::reg32 tick, x86::reg32 esp0, Result& result);
};

// rep movsd then rep movsb of size bytes into es:to.
void Update::copy(x86::reg32 from, x86::reg32 to, x86::reg32 size)
{
    const bool down = cpu.flags.df != 0;
    for (x86::reg32 n = size >> 2; n; --n)
    {
        rc::st32(cpu.ees + to, rc::ld32(from));
        from += down ? 0u - 4u : 4u;
        to += down ? 0u - 4u : 4u;
    }
    for (x86::reg32 n = size & 3; n; --n)
    {
        rc::st8(cpu.ees + to, rc::ld8(from));
        from += down ? 0u - 1u : 1u;
        to += down ? 0u - 1u : 1u;
    }
}

// Stepped track (0x5f0a10, cdecl: track, time, out): the value of the key in effect at time; past the length the
// base key or the one after it.
void Update::step(x86::reg32 track, x86::reg32 time, x86::reg32 out)
{
    const x86::reg32 length = trackLength(track);
    const x86::reg32 size = rc::ld8(track + 1);
    x86::reg32 from;
    if (x86::sreg32(time) <= x86::sreg32(length))
    {
        const x86::reg32 node = findKey(track, time);
        const x86::reg32 previous = rc::ld32(node + 8);
        from = previous && x86::sreg32(rc::ld32(node + 0xc)) > x86::sreg32(time) ? previous : node;
    }
    else
    {
        const x86::reg32 node = findKey(track, length - time);
        const x86::reg32 next = rc::ld32(node + 4);
        from = next && x86::sreg32(rc::ld32(node + 0xc)) < x86::sreg32(time) ? next : node;
    }
    copy(from + 0x10, out, size);
}

// The base value plus a key value by parameter type (0x5f5373 table): integer, float, 2 and 3 floats, quaternion
// product, 4 integers.
void Update::addKey(x86::reg32 track, x86::reg32 value, x86::reg32 out)
{
    const x86::reg32 base = track + 0x18;
    auto f = [](x86::reg32 address) { return double(rc::ldf32(address)); };
    switch (rc::ld8(track))
    {
    case 1:
        rc::st32(out, rc::ld32(base) + rc::ld32(value));
        break;
    case 2:
        rc::stf32(out, float(f(base) + f(value)));
        break;
    case 3:
        rc::stf32(out, float(f(base) + f(value)));
        rc::stf32(out + 4, float(f(value + 4) + f(base + 4)));
        break;
    case 4:
        rc::stf32(out, float(f(base) + f(value)));
        rc::stf32(out + 4, float(f(value + 4) + f(base + 4)));
        rc::stf32(out + 8, float(f(value + 8) + f(base + 8)));
        break;
    case 5:
    {
        // 0x5e8580: value * base, then copied out.
        const x86::reg32 a = value, b = base;
        const double x1 = f(a + 4) * f(b + 8) - f(b + 4) * f(a + 8);
        const double x2 = f(b) * f(a + 8) - f(a) * f(b + 8);
        const float z = float(f(b + 4) * f(a) - f(a + 4) * f(b));
        const float rx = float((f(a + 0xc) * f(b) + f(b + 0xc) * f(a)) + x1);
        const float ry = float((f(b + 0xc) * f(a + 4) + f(b + 4) * f(a + 0xc)) + x2);
        const float rz = float((f(a + 0xc) * f(b + 8) + f(b + 0xc) * f(a + 8)) + double(z));
        const double w = f(b + 0xc) * f(a + 0xc);
        const float rw = float(w - ((f(b) * f(a) + f(b + 4) * f(a + 4)) + f(b + 8) * f(a + 8)));
        rc::stf32(out, rx);
        rc::stf32(out + 4, ry);
        rc::stf32(out + 8, rz);
        rc::stf32(out + 0xc, rw);
        break;
    }
    case 6:
        rc::st32(out + 8, rc::ld32(value + 8) + rc::ld32(base + 8));
        rc::st32(out + 4, rc::ld32(value + 4) + rc::ld32(base + 4));
        rc::st32(out, rc::ld32(base) + rc::ld32(value));
        rc::st32(out + 0xc, rc::ld32(value + 0xc) + rc::ld32(base + 0xc));
        break;
    }
}

// The base key's value by parameter type (0x5f5428 table).
void Update::baseValue(x86::reg32 track, x86::reg32 out)
{
    const x86::reg32 base = track + 0x18;
    switch (rc::ld8(track))
    {
    case 1:
    case 2:
        rc::st32(out, rc::ld32(base));
        break;
    case 3:
    {
        const x86::reg32 y = rc::ld32(base + 4), x = rc::ld32(base);
        rc::st32(out, x);
        rc::st32(out + 4, y);
        break;
    }
    case 4:
    {
        const x86::reg32 x = rc::ld32(base), y = rc::ld32(base + 4), z = rc::ld32(base + 8);
        rc::st32(out, x);
        rc::st32(out + 4, y);
        rc::st32(out + 8, z);
        break;
    }
    case 5:
    case 6:
    {
        const x86::reg32 w = rc::ld32(base + 0xc), y = rc::ld32(base + 4), z = rc::ld32(base + 8);
        const x86::reg32 x = rc::ld32(base);
        rc::st32(out, x);
        rc::st32(out + 0xc, w);
        rc::st32(out + 4, y);
        rc::st32(out + 8, z);
        break;
    }
    }
}

// _ftol2 of (b - a) * t + 0.5 in double, plus the base and a.
x86::reg32 blendInteger(x86::reg32 a, x86::reg32 b, float t, x86::reg32 base)
{
    const double v = double(x86::sreg32(b - a)) * double(t) + double(Half);
    const x86::reg32 rounded = x86::reg32(x86::truncInteger64(v));
    return rounded + rc::ld32(base) + a;
}

// Base value plus the keys' values blended by t (0x5f5440 table); quaternions slerp in the guest (0x5f06a0).
bool Update::blend(x86::reg32 track, x86::reg32 from, x86::reg32 to, float t, x86::reg32 out, x86::reg32 esp)
{
    const x86::reg32 base = track + 0x18;
    const x86::reg32 a = from + 0x10, b = to + 0x10;
    auto f = [](x86::reg32 address) { return double(rc::ldf32(address)); };
    const double s = t;
    switch (rc::ld8(track))
    {
    case 1:
    {
        const x86::reg32 va = rc::ld32(a), vb = rc::ld32(b);
        rc::st32(out, blendInteger(va, vb, t, base));
        break;
    }
    case 2:
    {
        const double x = f(a);
        rc::stf32(out, float(((f(b) - x) * s + x) + f(base)));
        break;
    }
    case 3:
        rc::stf32(out, float(((f(b) - f(a)) * s + f(a)) + f(base)));
        rc::stf32(out + 4, float(((f(b + 4) - f(a + 4)) * s + f(base + 4)) + f(a + 4)));
        break;
    case 4:
        rc::stf32(out, float(((f(b) - f(a)) * s + f(a)) + f(base)));
        rc::stf32(out + 4, float(((f(b + 4) - f(a + 4)) * s + f(base + 4)) + f(a + 4)));
        rc::stf32(out + 8, float(((f(b + 8) - f(a + 8)) * s + f(base + 8)) + f(a + 8)));
        break;
    case 5:
        return call(&sub_5f06a0, esp - 0x40, 0x5f5328, floatBits(t), { a, b, floatBits(t), base, out });
    case 6:
        for (x86::reg32 k = 0; k < 16; k += 4)
        {
            const x86::reg32 va = rc::ld32(a + k), vb = rc::ld32(b + k);
            const double v = double(x86::sreg32(vb - va)) * s + double(Half);
            const x86::reg32 rounded = x86::reg32(x86::truncInteger64(v));
            rc::st32(out + k, rounded + rc::ld32(base + k) + rc::ld32(a + k));
        }
        break;
    }
    return true;
}

// Interpolated track (0x5f4fc0, cdecl: track, time, out; esp at its entry): finds the keys around time for the
// action (0 hold at the end, 1 loop, 2 ping-pong), their blend factor, then the base value plus the blend.
bool Update::linear(x86::reg32 track, x86::reg32 time, x86::reg32 out, x86::reg32 esp)
{
    using x86::sreg32;
    x86::reg32 from = 0, to = 0;
    float t = 0.0f;
    // Blend factor of time - start over span, 0 for spans up to 1e-5.
    auto factor = [&t](x86::reg32 span, x86::reg32 offset)
    {
        const double d = double(sreg32(span));
        t =d > double(KeySpanMinimum) ? float(double(sreg32(offset)) / d) : 0.0f;
    };
    if (firstKey(track))
    {
        switch (rc::ld8(track + 3) & 0x7f)
        {
        case 0:
            to = findDeltaKey(track, time);
            if (!to)
                break;
            from = rc::ld32(to + 8);
            if (!from)
            {
                t = One;
                break;
            }
            {
                const x86::reg32 end = rc::ld32(to + 0xc);
                if (sreg32(time) >= sreg32(end))
                {
                    t = One;
                    break;
                }
                const x86::reg32 start = rc::ld32(from + 0xc);
                factor(end - start, time - start);
            }
            break;
        case 1:
        {
            to = findDeltaKey(track, time);
            if (!to)
                break;
            const x86::reg32 end = rc::ld32(to + 0xc);
            if (sreg32(end) < sreg32(time))
            {
                from = to;
                to = firstKey(track);
                const x86::reg32 start = rc::ld32(from + 0xc);
                factor(trackLength(track) + rc::ld32(to + 0xc) - start, time - start);
            }
            else if (end == time)
            {
                from = to;
                t = 0.0f;
            }
            else
            {
                from = rc::ld32(to + 8);
                if (from)
                {
                    const x86::reg32 start = rc::ld32(from + 0xc);
                    factor(end - start, time - start);
                }
                else
                {
                    const x86::reg32 length = trackLength(track);
                    from = findKey(track, length);
                    const x86::reg32 tail = length - rc::ld32(from + 0xc);
                    factor(tail + end, tail + time);
                }
            }
            break;
        }
        case 2:
        {
            const x86::reg32 length = trackLength(track);
            to = findDeltaKey(track, sreg32(time) > sreg32(length) ? length + length - time : time);
            break;
        }
        }
    }
    if (!to)
    {
        baseValue(track, out);
        return true;
    }
    if (double(t) == double(Zero))
    {
        addKey(track, from + 0x10, out);
        return true;
    }
    if (double(t) == double(One))
    {
        addKey(track, to + 0x10, out);
        return true;
    }
    return blend(track, from, to, t, out, esp);
}

// A track by its interpolation (0 stepped, 1 and 3 interpolated); esp at the interpolator's entry.
bool Update::interpolate(x86::reg32 track, x86::reg32 time, x86::reg32 out, x86::reg32 esp)
{
    switch (rc::ld8(track + 2))
    {
    case 0:
        step(track, time, out);
        return true;
    case 1:
    case 3:
        return linear(track, time, out, esp);
    }
    return true;
}

// 0x5f9570 (cdecl: track, time, out; esp at its entry): a track not done yet, then done when it has no delta keys.
bool Update::advance(x86::reg32 track, x86::reg32 time, x86::reg32 out, x86::reg32 esp)
{
    if (rc::ld8(track + 3) & 0x80)
        return true;
    if (!interpolate(track, time, out, esp - 0x14))
        return false;
    if (!firstKey(track))
        rc::st8(track + 3, rc::ld8(track + 3) | 0x80);
    return true;
}

// FEPackage::UpdateObjectTracks (0x5fa2d0, thiscall: object, script; esp at its entry): every track of the script
// at its time into the object's data, the first track (output at +0) again first; running and animating flags.
bool Update::tracks(x86::reg32 object, x86::reg32 script, x86::reg32 esp)
{
    const x86::reg32 data = rc::ld32(object + 0x2c);
    x86::reg32 track = rc::ld32(script + 0x20);
    const x86::reg32 time = rc::ld32(script + 0x10);
    const x86::reg32 inner = esp - 0x20;
    const bool first = track && !(rc::ld32(track + 4) & 0xff000000);
    if (rc::ld8(package + 0x14))
    {
        if (first)
        {
            const x86::reg32 flags = rc::ld32(object + 0x1c);
            if (rc::ld8(track + 3) & 0x80)
                rc::st32(object + 0x1c, flags & (rc::ld32(FlagMask) | ~FlagScriptRunning));
            else
                rc::st32(object + 0x1c, flags | FlagScriptRunning);
            if (!advance(track, time, data, inner))
                return false;
        }
        else
        {
            rc::st32(object + 0x1c, rc::ld32(object + 0x1c) & (rc::ld32(FlagMask) | ~FlagScriptRunning));
        }
        x86::reg32 done = 0x80;
        const x86::reg32 count = rc::ld8(script + 0x1c);
        if (rc::ld32(rc::ld32(object + 0x2c) + 0xc) && count)
        {
            for (x86::reg32 n = count; n; --n, track += 0x38)
            {
                done &= rc::ld8(track + 3);
                if (!advance(track, time, trackOutput(data, track), inner))
                    return false;
            }
        }
        if (done)
            rc::st32(object + 0x1c, rc::ld32(object + 0x1c) & (rc::ld32(FlagMask) | ~FlagAnimating));
        else
            rc::st32(object + 0x1c, rc::ld32(object + 0x1c) | FlagAnimating);
    }
    else
    {
        if (first && !interpolate(track, time, data, inner))
            return false;
        const x86::reg32 count = rc::ld8(script + 0x1c);
        if (rc::ld32(rc::ld32(object + 0x2c) + 0xc) && count)
        {
            for (x86::reg32 n = count; n; --n, track += 0x38)
            {
                if (!interpolate(track, time, trackOutput(data, track), inner))
                    return false;
            }
        }
    }
    const x86::reg32 flags = rc::ld32(object + 0x1c);
    if (flags & FlagsDirtyAny)
        rc::st32(object + 0x1c, flags | FlagDirty);
    return true;
}

// FEPackage::UpdateGroup (0x5eca20, thiscall: group, tick; esp at its entry): each child with the group's dirty
// flags, updated.
bool Update::group(x86::reg32 object, x86::reg32 tick, x86::reg32 esp)
{
    Result ignored;
    for (x86::reg32 node = rc::ld32(object + 0x64); node; node = rc::ld32(node + 4))
    {
        rc::st32(node + 0x1c, rc::ld32(node + 0x1c) | (rc::ld32(object + 0x1c) & 0x3c00000));
        if (!child(node, tick, esp - 0x1c, 0x5eca4e, ignored))
            return false;
        app->allowPreemption(cpu);
    }
    return true;
}

// FEPackage::UpdateObject (0x5fcdc0, thiscall: object, tick; esp0 at its entry): advances the current script by
// the package's tick with its end action and messages, the tracks, then the object type's own update.
bool Update::object(x86::reg32 object, x86::reg32 tick, x86::reg32 esp0, Result& result)
{
    using x86::sreg32;
    const x86::reg32 frame = rc::ld32(FrameCounter);
    if (rc::ld32(UpdateFrame) == frame)
    {
        rc::st32(UpdateRepeats, rc::ld32(UpdateRepeats) + 1);
    }
    else
    {
        rc::st32(UpdateRepeats, 0);
        rc::st32(UpdateFrame, frame);
    }
    const x86::reg32 entryFlags = rc::ld32(object + 0x1c);
    if (entryFlags & FlagsDirtyAny)
        rc::st32(object + 0x1c, entryFlags | FlagDirty);
    else
        rc::st32(object + 0x1c, (rc::ld32(FlagMask) | ~FlagDirty) & entryFlags);
    const x86::reg32 increment = rc::ld32(package + 0x34);
    x86::reg32 script = rc::ld32(object + 0x54);
    const x86::reg32 previous = rc::ld32(script + 0x10);
    const x86::reg32 length = rc::ld32(script + 0xc);
    rc::st32(esp0 + 4, previous);
    rc::st32(script + 0x10, previous + increment);
    if (sreg32(previous + increment) < 0)
        rc::st32(script + 0x10, 0);
    const x86::reg32 current = rc::ld32(script + 0x10);
    const bool executing = rc::ld8(package + 0x14) != 0;
    x86::reg32 slot = package;
    if (sreg32(current) >= sreg32(length))
    {
        if (executing && rc::ld32(script + 0x18))
        {
            if (!tracks(object, script, esp0 - 0x20))
                return false;
            const x86::reg32 rest = rc::ld32(script + 0x10) - length;
            slot = rest;
            rc::st32(script + 0x10, 0);
            if (rc::ld32(script + 0x24) && !messages(esp0, 0x5fce7e, object, script, previous, length))
                return false;
            script = rc::ld32(script + 0x18);
            rc::st32(object + 0x54, script);
            if (script && !call(&sub_5f2e40, esp0 - 0x18, 0x5fce93, object, {}))
                return false;
            const x86::reg32 events = rc::ld32(script + 0x24);
            rc::st32(script + 0x10, rest);
            if (events && !messages(esp0, 0x5fcfe4, object, script, 0, rest))
                return false;
        }
        else if (executing)
        {
            switch (rc::ld32(script + 0x14) & 3)
            {
            case 0:
                if (rc::ld32(script + 0x24) && !messages(esp0, 0x5fcf39, object, script, previous, length))
                    return false;
                rc::st32(script + 0x10, rc::ld32(script + 0xc) + 1);
                break;
            case 1:
            {
                if (sreg32(length) <= 0)
                {
                    rc::st32(script + 0x10, 0);
                    break;
                }
                if (rc::ld32(script + 0x24) && !messages(esp0, 0x5fcef5, object, script, previous, length))
                    return false;
                const x86::reg32 time = rc::ld32(script + 0x10);
                const x86::reg32 wrapped = rc::divideSigned(x86::reg32(sreg32(time) >> 31), time,
                                                            rc::ld32(script + 0xc)).remainder;
                rc::st32(script + 0x10, wrapped);
                if (rc::ld32(script + 0x24) && !messages(esp0, 0x5fcf15, object, script, 0, wrapped))
                    return false;
                if (!call(&sub_5f2e40, esp0 - 0x18, 0x5fcf1c, object, {}))
                    return false;
                break;
            }
            case 2:
                if (sreg32(length) <= 0)
                    rc::st32(script + 0x10, 0);
                else
                    rc::st32(script + 0x10, rc::divideSigned(x86::reg32(sreg32(current) >> 31), current,
                                                             length + length).remainder);
                break;
            }
        }
    }
    else if (executing && rc::ld32(script + 0x24))
    {
        switch (rc::ld32(script + 0x14) & 3)
        {
        case 0:
            if (!messages(esp0, 0x5fcfe4, object, script, previous, current))
                return false;
            break;
        case 1:
            if (sreg32(current) >= sreg32(previous))
            {
                if (!messages(esp0, 0x5fcfe4, object, script, previous, current))
                    return false;
            }
            else if (!messages(esp0, 0x5fcfbf, object, script, previous, length) ||
                     !messages(esp0, 0x5fcfe4, object, script, 0, rc::ld32(script + 0x10)))
            {
                return false;
            }
            break;
        case 2:
            if (sreg32(previous) < sreg32(length))
            {
                if (sreg32(current) < sreg32(length))
                {
                    if (!messages(esp0, 0x5fcfe4, object, script, previous, current))
                        return false;
                }
                else if (!messages(esp0, 0x5fcf8b, object, script, previous, length) ||
                         !messages(esp0, 0x5fcfe4, object, script, length, rc::ld32(script + 0x10) - length))
                {
                    return false;
                }
            }
            else if (sreg32(current) >= sreg32(length))
            {
                if (!messages(esp0, 0x5fcfe4, object, script, previous - length, current - length))
                    return false;
            }
            else if (!messages(esp0, 0x5fcfbf, object, script, previous - length, 0) ||
                     !messages(esp0, 0x5fcfe4, object, script, 0, rc::ld32(script + 0x10)))
            {
                return false;
            }
            break;
        }
    }
    // Tracks again unless the script stood at its end since the last update.
    if (!rc::ld8(package + 0x14) || previous != rc::ld32(script + 0x10) || previous != rc::ld32(script + 0xc) + 1 ||
        (rc::ld32(object + 0x1c) & FlagForceTracks))
    {
        if (!tracks(object, script, esp0 - 0x20))
            return false;
    }
    switch (rc::ld32(object + 0x18))
    {
    case 4:
        if (!call(&sub_5f1720, esp0 - 0x1c, 0x5fd03e, object, { floatBits(float(x86::sreg32(tick))) }))
            return false;
        break;
    case 5:
        if (!group(object, tick, esp0 - 0x20))
            return false;
        break;
    case 6:
        if (!call(&sub_5e93b0, esp0 - 0x1c, 0x5fd04f, object, { floatBits(float(x86::sreg32(tick))) }))
            return false;
        break;
    case 7:
        if (rc::ld8(package + 0x14))
            rc::st32(object + 0x5c, rc::ld32(object + 0x5c) + tick);
        break;
    }
    const x86::reg32 flags = rc::ld32(object + 0x1c);
    if ((flags & 0x20000) && (flags & 0x1400000) && !call(&sub_5f3800, esp0 - 0x1c, 0x5fd07d, package, { object }))
        return false;
    if (rc::ld8(package + 0x14) == 1 && previous == rc::ld32(script + 0x10) && previous == rc::ld32(script + 0xc) + 1)
    {
        const x86::reg32 settled = rc::ld32(object + 0x1c);
        if (!(settled & FlagForceTracks))
            rc::st32(object + 0x1c, (rc::ld32(FlagMask) | ~(FlagScriptRunning | FlagAnimating)) & settled);
    }
    const x86::reg32 mask = rc::ld32(FlagMask) | ~FlagForceTracks;
    const x86::reg32 final = rc::ld32(object + 0x1c) & mask;
    rc::st32(object + 0x1c, final);
    result.edx = final;
    result.eax = final;
    if (final & FlagsDirtyAny)
    {
        result.eax = final | FlagDirty;
        rc::st32(object + 0x1c, result.eax);
    }
    result.ecx = slot;
    return true;
}

// An UpdateObject call of the native code, compared with the translated one under shadow_feng.
bool Update::child(x86::reg32 object, x86::reg32 tick, x86::reg32 esp0, x86::reg32 returnAddress, Result& result)
{
    if (shadowOn() && shadowEligible(object))
    {
        rc::st32(esp0, returnAddress);
        rc::st32(esp0 + 4, object);
        rc::st32(esp0 + 8, tick);
        return shadowObject(object, tick, esp0, result);
    }
    return this->object(object, tick, esp0, result);
}

// Objects whose update makes no guest call: no script messages, chain or loop end, not a list, group or movie, no
// mouse offsets, no quaternion slerp.
bool Update::shadowEligible(x86::reg32 object)
{
    const x86::reg32 type = rc::ld32(object + 0x18);
    const x86::reg32 script = rc::ld32(object + 0x54);
    if ((type >= 4 && type <= 6) || !script || (rc::ld32(object + 0x1c) & 0x20000))
        return false;
    if (rc::ld32(script + 0x24) || rc::ld32(script + 0x18) || (rc::ld32(script + 0x14) & 3) == 1)
        return false;
    const x86::reg32 list = rc::ld32(script + 0x20);
    const x86::reg32 count = rc::ld8(script + 0x1c);
    if (!list && count)
        return false;
    // The first track runs without a track count too.
    for (x86::reg32 k = 0; k < (list && !count ? 1 : count); ++k)
    {
        const x86::reg32 track = list + k * 0x38;
        const x86::reg32 interpolation = rc::ld8(track + 2);
        if (rc::ld8(track) == 5 && (interpolation == 1 || interpolation == 3))
            return false;
    }
    return true;
}

// Runs the translated UpdateObject on a copy of the CPU, then the native one from the same guest memory, and
// compares registers, the x87 depth and the memory the update writes.
bool Update::shadowObject(x86::reg32 object, x86::reg32 tick, x86::reg32 esp0, Result& result)
{
    const x86::reg32 script = rc::ld32(object + 0x54);
    const x86::reg32 data = rc::ld32(object + 0x2c);
    const x86::reg32 list = rc::ld32(script + 0x20);
    std::vector<ShadowRegion> regions = { { UpdateRepeats, 8 }, { object + 0x1c, 4 }, { script + 0x10, 4 },
                                          { esp0 + 4, 4 } };
    if (rc::ld32(object + 0x18) == 7)
        regions.push_back({ object + 0x5c, 4 });
    const x86::reg32 count = list && !rc::ld8(script + 0x1c) ? 1 : rc::ld8(script + 0x1c);
    if (count)
    {
        regions.push_back({ list, count * 0x38 });
        x86::sreg32 low = 0x7fffffff, high = -0x7fffffff;
        for (x86::reg32 k = 0; k < count; ++k)
        {
            const x86::reg32 track = list + k * 0x38;
            const x86::sreg32 offset = 4 * x86::sreg32(int8_t(rc::ld8(track + 7)));
            const x86::sreg32 size = rc::ld8(track + 1) > 16 ? rc::ld8(track + 1) : 16;
            low = offset < low ? offset : low;
            high = offset + size > high ? offset + size : high;
        }
        regions.push_back({ data + x86::reg32(low), x86::reg32(high - low) });
    }
    std::vector<x86::reg8> before, translated;
    for (const ShadowRegion& r : regions)
        before.insert(before.end(), rc::host(r.address), rc::host(r.address) + r.size);
    s_shadowRunning = true;
    x86::CPU reference = cpu;
    reference.ecx = package;
    reference.esp = esp0;
    sub_5fcdc0_guest(app, reference);
    size_t at = 0;
    for (const ShadowRegion& r : regions)
    {
        translated.insert(translated.end(), rc::host(r.address), rc::host(r.address) + r.size);
        memcpy(rc::host(r.address), before.data() + at, r.size);
        at += r.size;
    }
    const bool finished = this->object(object, tick, esp0, result);
    s_shadowRunning = false;
    if (reference.terminate || !finished)
    {
        cpu.terminate = true;
        return false;
    }
    ++s_shadowCompared;
    int found = 0;
    const x86::reg32 g[] = { reference.eax, reference.ecx, reference.edx, reference.ebx, reference.esp,
                             reference.ebp, reference.esi, reference.edi };
    const x86::reg32 n[] = { result.eax, result.ecx, result.edx, cpu.ebx, esp0 + 0xc, cpu.ebp, cpu.esi, cpu.edi };
    static const char* const names[] = { "eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi" };
    for (int i = 0; i < 8; ++i)
    {
        if (g[i] != n[i])
            shadowReport(names[i], g[i], n[i], found);
    }
    if (reference.fpu.count != cpu.fpu.count)
        shadowReport("x87 depth", reference.fpu.count, cpu.fpu.count, found);
    at = 0;
    for (const ShadowRegion& r : regions)
    {
        for (x86::reg32 i = 0; i < r.size; ++i)
        {
            if (translated[at + i] != *rc::host(r.address + i))
            {
                char what[48];
                snprintf(what, sizeof(what), "byte at 0x%08x", unsigned(r.address + i));
                shadowReport(what, translated[at + i], *rc::host(r.address + i), found);
                break;
            }
        }
        at += r.size;
    }
    if (found)
        ++s_shadowDiffer;
    return true;
}

}

namespace feng
{

void logShadow()
{
    if (s_shadow > 0)
        pf::logInfo("shadow_feng: %u calls compared, %u differ", unsigned(s_shadowCompared), unsigned(s_shadowDiffer));
}

}

namespace native
{

// FEPackage::UpdateObject (0x5fcdc0, thiscall: object, tick; ret 8).
void sub_5fcdc0(win32::WinApplication* app, x86::CPU& cpu)
{
    if (!fengNative())
    {
        sub_5fcdc0_guest(app, cpu);
        return;
    }
    const x86::reg32 esp0 = cpu.esp;
    Update update{ app, cpu, cpu.ecx };
    Result result;
    const x86::reg32 object = rc::ld32(esp0 + 4);
    if (!update.child(object, rc::ld32(esp0 + 8), esp0, rc::ld32(esp0), result))
        return;
    cpu.eax = result.eax;
    cpu.ecx = result.ecx;
    cpu.edx = result.edx;
    cpu.esp = esp0 + 0xc;
}

}
}
