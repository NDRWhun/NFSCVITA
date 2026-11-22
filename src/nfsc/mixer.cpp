// Native kernels of the audio mixer (RWAudioCore Dac thread): the per-voice filters 0x89f363 and 0x89f4f6 in the
// same double-precision steps as their x87 code, the gain accumulation 0x8ad305, the scale 0x8ad12e and the clamp
// 0x8b074c with NEON.
// mixer_native=0 runs the translated kernels instead. The block render 0x8a74dd counts mixer blocks for perf_log.

#include <lib/thread.h>
#include <lib/winapp.h>
#include <native.h>
#include <platform/config.h>
#include <platform/perf.h>
#include <recomp.h>

#include <arm_neon.h>
#include <atomic>
#include <cstring>

namespace nfsc
{

// Translated bodies of the overridden guest functions.
void sub_89f363_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_89f4f6_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_8ad305_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_8ad12e_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_8b074c_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_8a74dd_guest(win32::WinApplication* app, x86::CPU& cpu);

namespace
{

// Anti-denormal offset of 0x89f4f6 (guest float at 0xa03bb8).
constexpr x86::reg32 DenormalOffset = 0xa03bb8;

// mixer_native, read once; relaxed, as the block service's worker thread reads it too.
std::atomic<int> s_native{-1};

bool useNative()
{
    int enabled = s_native.load(std::memory_order_relaxed);
    if (enabled < 0)
    {
        enabled = pf::Config::getBool("mixer_native", true) ? 1 : 0;
        s_native.store(enabled, std::memory_order_relaxed);
    }
    return enabled != 0;
}

x86::reg32 arg(const x86::CPU& cpu, int index)
{
    return rc::ld32(cpu.esp + 4 + 4 * index);
}

float argFloat(const x86::CPU& cpu, int index)
{
    return rc::ldf32(cpu.esp + 4 + 4 * index);
}

bool aligned4(x86::reg32 a, x86::reg32 b)
{
    return ((a | b) & 3) == 0;
}

}

namespace native
{

// Filter stage (cdecl: count, f, q, g, h, z, in, a, out, p, accumulate); returns the last output in st0.
// out[i] = in[i] - z*f - q*a[i+1], z = out[i]; p[i] = (g*a[i] + a[i+1])*h, added to p[i] when accumulate is set.
void sub_89f363(win32::WinApplication* app, x86::CPU& cpu)
{
    if (!useNative())
    {
        sub_89f363_guest(app, cpu);
        return;
    }
    const x86::sreg32 count = x86::sreg32(arg(cpu, 0));
    const double f = argFloat(cpu, 1);
    const double q = argFloat(cpu, 2);
    const double g = argFloat(cpu, 3);
    const double h = argFloat(cpu, 4);
    double z = argFloat(cpu, 5);
    const x86::reg32 in = arg(cpu, 6);
    const x86::reg32 a = arg(cpu, 7);
    const x86::reg32 out = arg(cpu, 8);
    const x86::reg32 p = arg(cpu, 9);
    const bool accumulate = arg(cpu, 10) != 0;
    for (x86::sreg32 i = 0; i < count; ++i)
    {
        const x86::reg32 o = 4 * x86::reg32(i);
        double t = double(rc::ldf32(in + o)) - z * f;
        t -= q * double(rc::ldf32(a + o + 4));
        rc::stf32(out + o, float(t));
        double u = g * double(rc::ldf32(a + o)) + double(rc::ldf32(a + o + 4));
        u *= h;
        if (accumulate)
            u += double(rc::ldf32(p + o));
        rc::stf32(p + o, float(u));
        z = rc::ldf32(out + o);
    }
    cpu.fpu.push(x86::Float(z));
    cpu.esp += 4;
}

// Filter stage (cdecl: count, f, g, in, low, band, out, accumulate) with the anti-denormal offset c:
// low[i] = (low[i] + c) - c; band[i] = in[i] - f*low[i], then (band[i] + c) - c = v; out[i] = (v*f + low[i])*g,
// added to out[i] when accumulate is set.
void sub_89f4f6(win32::WinApplication* app, x86::CPU& cpu)
{
    if (!useNative())
    {
        sub_89f4f6_guest(app, cpu);
        return;
    }
    const x86::sreg32 count = x86::sreg32(arg(cpu, 0));
    const double f = argFloat(cpu, 1);
    const double g = argFloat(cpu, 2);
    const x86::reg32 in = arg(cpu, 3);
    const x86::reg32 low = arg(cpu, 4);
    const x86::reg32 band = arg(cpu, 5);
    const x86::reg32 out = arg(cpu, 6);
    const bool accumulate = arg(cpu, 7) != 0;
    const double c = rc::ldf32(DenormalOffset);
    for (x86::sreg32 i = 0; i < count; ++i)
    {
        const x86::reg32 o = 4 * x86::reg32(i);
        double l = double(rc::ldf32(low + o)) + c;
        l -= c;
        rc::stf32(low + o, float(l));
        const double lf = rc::ldf32(low + o);
        rc::stf32(band + o, float(double(rc::ldf32(in + o)) - f * lf));
        double v = double(rc::ldf32(band + o)) + c;
        v -= c;
        rc::stf32(band + o, float(v));
        double r = (v * f + double(rc::ldf32(low + o))) * g;
        if (accumulate)
            r += double(rc::ldf32(out + o));
        rc::stf32(out + o, float(r));
    }
    cpu.esp += 4;
}

// Gain accumulation (cdecl: destination, source, gain, count): destination[i] += gain * source[i].
void sub_8ad305(win32::WinApplication* app, x86::CPU& cpu)
{
    if (!useNative())
    {
        sub_8ad305_guest(app, cpu);
        return;
    }
    const x86::reg32 destination = arg(cpu, 0);
    const x86::reg32 source = arg(cpu, 1);
    const float gain = argFloat(cpu, 2);
    const x86::reg32 count = arg(cpu, 3);
    x86::reg32 i = 0;
    // Element-wise order is kept unless the ranges overlap by less than four floats.
    const x86::sreg32 distance = x86::sreg32(destination - source);
    if (aligned4(destination, source) && (distance == 0 || distance >= 16 || distance <= -16))
    {
        float* d = reinterpret_cast<float*>(rc::host(destination));
        const float* s = reinterpret_cast<const float*>(rc::host(source));
        const float32x4_t g = vdupq_n_f32(gain);
        // Sixteen at a time, all loads first, unless the destination lies less than sixteen floats above the source.
        if (distance <= 0 || distance >= 64)
        {
            for (; i + 16 <= count; i += 16)
            {
                const float32x4_t s0 = vld1q_f32(s + i), s1 = vld1q_f32(s + i + 4);
                const float32x4_t s2 = vld1q_f32(s + i + 8), s3 = vld1q_f32(s + i + 12);
                const float32x4_t d0 = vld1q_f32(d + i), d1 = vld1q_f32(d + i + 4);
                const float32x4_t d2 = vld1q_f32(d + i + 8), d3 = vld1q_f32(d + i + 12);
                vst1q_f32(d + i, vmlaq_f32(d0, s0, g));
                vst1q_f32(d + i + 4, vmlaq_f32(d1, s1, g));
                vst1q_f32(d + i + 8, vmlaq_f32(d2, s2, g));
                vst1q_f32(d + i + 12, vmlaq_f32(d3, s3, g));
            }
        }
        for (; i + 4 <= count; i += 4)
            vst1q_f32(d + i, vmlaq_f32(vld1q_f32(d + i), vld1q_f32(s + i), g));
    }
    for (; i < count; ++i)
    {
        const x86::reg32 o = 4 * i;
        rc::stf32(destination + o, float(double(gain) * double(rc::ldf32(source + o)) + double(rc::ldf32(destination + o))));
    }
    cpu.esp += 4;
}

// Scale (cdecl: destination, source, gain, count): destination[i] = gain * source[i].
void sub_8ad12e(win32::WinApplication* app, x86::CPU& cpu)
{
    if (!useNative())
    {
        sub_8ad12e_guest(app, cpu);
        return;
    }
    const x86::reg32 destination = arg(cpu, 0);
    const x86::reg32 source = arg(cpu, 1);
    const float gain = argFloat(cpu, 2);
    const x86::reg32 count = arg(cpu, 3);
    x86::reg32 i = 0;
    // Element-wise order is kept unless the ranges overlap by less than four floats.
    const x86::sreg32 distance = x86::sreg32(destination - source);
    if (aligned4(destination, source) && (distance == 0 || distance >= 16 || distance <= -16))
    {
        float* d = reinterpret_cast<float*>(rc::host(destination));
        const float* s = reinterpret_cast<const float*>(rc::host(source));
        const float32x4_t g = vdupq_n_f32(gain);
        // Sixteen at a time, all loads first, unless the destination lies less than sixteen floats above the source.
        if (distance <= 0 || distance >= 64)
        {
            for (; i + 16 <= count; i += 16)
            {
                const float32x4_t s0 = vld1q_f32(s + i), s1 = vld1q_f32(s + i + 4);
                const float32x4_t s2 = vld1q_f32(s + i + 8), s3 = vld1q_f32(s + i + 12);
                vst1q_f32(d + i, vmulq_f32(s0, g));
                vst1q_f32(d + i + 4, vmulq_f32(s1, g));
                vst1q_f32(d + i + 8, vmulq_f32(s2, g));
                vst1q_f32(d + i + 12, vmulq_f32(s3, g));
            }
        }
        for (; i + 4 <= count; i += 4)
            vst1q_f32(d + i, vmulq_f32(vld1q_f32(s + i), g));
    }
    for (; i < count; ++i)
    {
        const x86::reg32 o = 4 * i;
        rc::stf32(destination + o, float(double(gain) * double(rc::ldf32(source + o))));
    }
    cpu.esp += 4;
}

// Clamp (cdecl: buffer, low, high, count): values below low become low, values above high become high (bit copies
// of the arguments); NaN stays.
void sub_8b074c(win32::WinApplication* app, x86::CPU& cpu)
{
    if (!useNative())
    {
        sub_8b074c_guest(app, cpu);
        return;
    }
    const x86::reg32 buffer = arg(cpu, 0);
    const float low = argFloat(cpu, 1);
    const float high = argFloat(cpu, 2);
    const x86::reg32 count = arg(cpu, 3);
    x86::reg32 i = 0;
    if ((buffer & 3) == 0)
    {
        float* b = reinterpret_cast<float*>(rc::host(buffer));
        const float32x4_t lo = vdupq_n_f32(low);
        const float32x4_t hi = vdupq_n_f32(high);
        for (; i + 4 <= count; i += 4)
        {
            const float32x4_t x = vld1q_f32(b + i);
            const uint32x4_t below = vcltq_f32(x, lo);
            const uint32x4_t above = vcgtq_f32(x, hi);
            vst1q_f32(b + i, vbslq_f32(below, lo, vbslq_f32(above, hi, x)));
        }
    }
    for (; i < count; ++i)
    {
        const x86::reg32 a = buffer + 4 * i;
        const float x = rc::ldf32(a);
        if (x < low)
            rc::stf32(a, low);
        else if (x > high)
            rc::stf32(a, high);
    }
    cpu.esp += 4;
}

// Block render of the DAC thread (one 256-frame block per call): the guest function, counted for perf_log.
void sub_8a74dd(win32::WinApplication* app, x86::CPU& cpu)
{
    static x86::reg32 s_thread = 0;
    if (pf::perf::enabled() && !s_thread)
        s_thread = win32::Thread::currentThreadId();
    sub_8a74dd_guest(app, cpu);
    if (pf::perf::enabled())
        pf::perf::mixerBlock(s_thread);
}

}
}
