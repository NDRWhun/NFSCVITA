#include <native.h>
#include <platform/config.h>
#include <recomp.h>

#include <algorithm>
#include <atomic>
#include <cstring>

namespace nfsc
{

void sub_536c40_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_537310_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_536bf0_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_8abbb2_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_8b70b0_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_8aa340_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_8ab884_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_8983f4_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_898423_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_8b6d00_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_8aa220_guest(win32::WinApplication* app, x86::CPU& cpu);

namespace
{

// mixer_native, read once; a relaxed flag instead of a guarded static (no barrier per call).
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

void runGuest(void (*function)(win32::WinApplication*, x86::CPU&), win32::WinApplication* app, x86::CPU& cpu)
{
    x86::CPU guest = cpu;
    function(app, guest);
    cpu = guest;
}

// Guest ranges [a, a + an) and [b, b + bn) share a byte; a range past the end of the address space overlaps all.
bool overlaps(x86::reg32 a, uint64_t an, x86::reg32 b, uint64_t bn)
{
    const uint64_t aEnd = uint64_t(a) + an;
    const uint64_t bEnd = uint64_t(b) + bn;
    return aEnd > 0x100000000ull || bEnd > 0x100000000ull || (a < bEnd && b < aEnd);
}

template <typename T>
T get(const x86::reg8* base, x86::reg32 address)
{
    T value;
    memcpy(&value, base + address, sizeof(value));
    return value;
}

template <typename T>
void put(x86::reg8* base, x86::reg32 address, T value)
{
    memcpy(base + address, &value, sizeof(value));
}

// fist to 16 bits of a float rounded to nearest even (rounding control 0): 0x8000 for |x| >= 32768, infinities and
// NaN. The magnitude, clamped to 32768, is rounded on its bits: mantissa shifted down, ties to even.
x86::reg16 nearest16(x86::reg32 bits)
{
    const x86::reg32 magnitude = std::min<x86::reg32>(bits & 0x7fffffff, 0x47000000);
    const x86::reg32 exponent = magnitude >> 23;
    x86::reg32 rounded = 0;
    if (exponent >= 126)
    {
        const x86::reg32 shift = 150 - exponent;
        const x86::reg32 mantissa = (magnitude & 0x7fffff) | 0x800000;
        const x86::reg32 half = 1u << (shift - 1);
        const x86::reg32 rest = mantissa & ((half << 1) - 1);
        rounded = mantissa >> shift;
        rounded += rest > half || (rest == half && (rounded & 1));
    }
    return x86::reg16(bits >> 31 ? 0u - rounded : rounded);
}

typedef x86::reg32 Lanes __attribute__((vector_size(16)));
typedef x86::sreg32 SignedLanes __attribute__((vector_size(16)));
typedef x86::reg16 HalfLanes __attribute__((vector_size(8)));

// 0x537310's 16-bit output of four float samples: nearest16 in the same steps lane by lane, 0x8000 saturated to
// 0x7fff for a positive sign bit.
HalfLanes saturated16(Lanes bits)
{
    const Lanes clampBits = Lanes{} + 0x47000000;
    Lanes magnitude = bits & 0x7fffffff;
    magnitude = magnitude < clampBits ? magnitude : clampBits;
    const Lanes exponent = magnitude >> 23;
    const Lanes valid = Lanes(exponent >= 126);
    const Lanes shift = valid ? 150 - exponent : Lanes{} + 24;
    const Lanes mantissa = (magnitude & 0x7fffff) | 0x800000;
    const Lanes half = Lanes{} + 1 << (shift - 1);
    const Lanes rest = mantissa & ((half << 1) - 1);
    Lanes rounded = mantissa >> shift;
    rounded -= Lanes(rest > half) | (Lanes(rest == half) & -(rounded & 1));
    rounded &= valid;
    const Lanes sign = Lanes(SignedLanes(bits) >> 31);
    const Lanes value = ((rounded ^ sign) - sign) & 0xffff;
    const Lanes output = value == 0x8000 ? (bits >> 31) + 0x7fff : value;
    return __builtin_convertvector(output, HalfLanes);
}

// 0x536c40's body: decodes the engine-audio block named in the argument slot (which receives the second header
// sample) into the object's 32 samples, leaving eax, ecx and edx as the guest does.
void decodeBlock(x86::reg32 object, x86::reg32 slot, x86::reg32& eax, x86::reg32& ecx, x86::reg32& edx)
{
    const x86::reg32 block = rc::ld32(slot);
    rc::st32(object + 0x2c, block);
    const x86::reg32 source = rc::ld32(object + 0x24) + block * 19;
    const x86::sreg32 first = x86::sreg32(x86::sreg8(rc::ld8(source + 1))) * 256
                              + (rc::ld8(source) & 0xf0);
    const x86::sreg32 second = x86::sreg32(x86::sreg8(rc::ld8(source + 3))) * 256
                               + (rc::ld8(source + 2) & 0xf0);
    rc::stf32(object + 0x30, float(first));
    rc::stf32(object + 0x34, float(second));
    rc::st32(slot, x86::reg32(second));
    const x86::reg32 predictor = rc::ld8(source) & 15;
    const double a = rc::ldf32(0xa5b654 + predictor * 4);
    const double b = rc::ldf32(0xa5b664 + predictor * 4);
    const x86::reg32 shift = (rc::ld8(source + 2) & 15) * 16;
    eax = object + 0xb0;
    ecx = shift;
    edx = source + 20;
    if (!overlaps(object + 0x30, 0x80, source, 19) && !overlaps(object + 0x30, 0x80, 0xa5b654, 0x424)
        && !overlaps(object + 0x30, 8, slot, 4))
    {
        // The samples are apart from the block, the tables and the argument slot: the last two stay in registers.
        x86::reg8* const base = rc::host(0);
        float x = float(first);
        float y = float(second);
        for (x86::reg32 pair = 0; pair < 15; ++pair)
        {
            const x86::reg32 out = object + 0x38 + pair * 8;
            const x86::reg32 packed = get<x86::reg8>(base, source + 4 + pair);
            const double high = get<float>(base, 0xa5b678 + (shift + (packed >> 4)) * 4);
            const double low = get<float>(base, 0xa5b678 + (shift + (packed & 15)) * 4);
            const x86::reg32 phase = pair % 5;
            const double even = phase == 1 ? (a * double(y) + high) + b * double(x)
                                           : (b * double(x) + high) + a * double(y);
            const double odd = phase < 2 ? (b * double(y) + low) + even * a : (even * a + low) + b * double(y);
            x = float(even);
            y = float(odd);
            put(base, out, x);
            put(base, out + 4, y);
        }
        return;
    }
    for (x86::reg32 pair = 0; pair < 15; ++pair)
    {
        const x86::reg32 out = object + 0x38 + pair * 8;
        const x86::reg32 packed = rc::ld8(source + 4 + pair);
        const double high = rc::ldf32(0xa5b678 + (shift + (packed >> 4)) * 4);
        const double low = rc::ldf32(0xa5b678 + (shift + (packed & 15)) * 4);
        const double x = rc::ldf32(out - 8);
        const double y = rc::ldf32(out - 4);
        const x86::reg32 phase = pair % 5;
        const double even = phase == 1 ? (a * y + high) + b * x : (b * x + high) + a * y;
        rc::stf32(out, float(even));
        const double odd = phase < 2 ? (b * y + low) + even * a : (even * a + low) + b * y;
        rc::stf32(out + 4, float(odd));
    }
}

}

namespace native
{

// Multiplies two integer audio controls and rounds their scaled float result to an integer.
void sub_898423(win32::WinApplication* app, x86::CPU& cpu)
{
    if (!useNative())
    {
        runGuest(sub_898423_guest, app, cpu);
        return;
    }
    const x86::reg32 controls = rc::ld32(cpu.esp + 4);
    double value = double(x86::sreg32(rc::ld32(controls + 8))) * double(x86::sreg32(rc::ld32(controls + 4)));
    value *= double(rc::ldf32(controls));
    rc::stf32(cpu.esp + 4, float(value));
    cpu.eax = x86::reg32(x86::fistInteger<x86::sreg32>(
        x86::roundIntegral(double(rc::ldf32(cpu.esp + 4)), cpu.fpu.control.rc)));
    cpu.esp += 4;
}

// Multiplies integer audio controls and rounds their scaled float result to an integer.
void sub_8983f4(win32::WinApplication* app, x86::CPU& cpu)
{
    if (!useNative())
    {
        runGuest(sub_8983f4_guest, app, cpu);
        return;
    }
    const x86::reg32 controls = rc::ld32(cpu.esp + 4);
    const x86::reg32 count = rc::ld8(controls);
    double value = x86::sreg32(rc::ld32(controls + 8));
    for (x86::reg32 i = 1; i < count; ++i)
        value *= double(x86::sreg32(rc::ld32(controls + 8 + i * 4)));
    value *= double(rc::ldf32(controls + 4));
    rc::stf32(cpu.esp + 4, float(value));
    cpu.eax = x86::reg32(x86::fistInteger<x86::sreg32>(
        x86::roundIntegral(double(rc::ldf32(cpu.esp + 4)), cpu.fpu.control.rc)));
    if (count > 1)
        cpu.ecx = controls + 8 + count * 4;
    cpu.edx = count > 1 ? 0 : count;
    cpu.esp += 4;
}

// Computes the engine sample index using the guest integer rounding and fractional sign.
void sub_536bf0(win32::WinApplication* app, x86::CPU& cpu)
{
    if (!useNative())
    {
        runGuest(sub_536bf0_guest, app, cpu);
        return;
    }
    const double value = rc::ldf32(cpu.esp + 4);
    const x86::sreg32 integer = x86::fistInteger<x86::sreg32>(x86::roundIntegral(value, cpu.fpu.control.rc));
    const float fraction = float(value - double(integer));
    x86::reg32 bits;
    memcpy(&bits, &fraction, sizeof(bits));
    cpu.eax = x86::reg32(integer) + x86::reg32(x86::sreg32(bits) >> 31);
    cpu.esp += 4;
}

// Decodes four-lane EA-XA packets into the mixer's floating-point sample buffer.
void sub_8ab884(win32::WinApplication* app, x86::CPU& cpu)
{
    const x86::reg32 state = rc::ld32(cpu.esp + 4);
    if (!useNative() || x86::sreg32(rc::ld32(state + 0x38)) <= 0 || rc::ld8(0xbc6e88))
    {
        runGuest(sub_8ab884_guest, app, cpu);
        return;
    }
    const x86::reg32 descriptor = rc::ld32(cpu.esp + 8);
    const x86::reg32 packets = rc::ld8(state + 0x2e);
    x86::reg32 ecx = cpu.ecx;
    x86::reg32 edx = cpu.edx;
    for (x86::reg32 packet = 0; packet < packets; ++packet)
    {
        const x86::reg32 source = rc::ld32(state + 0x34);
        const x86::reg32 destination = rc::ld32(descriptor + 4) + rc::ld16(descriptor + 14) * packet * 4;
        if (!overlaps(destination, 512, source, 76) && !overlaps(destination, 512, 0xa04330, 0x60)
            && !overlaps(destination, 512, 0x9e6224, 4))
        {
            // Nothing the packet reads lies in its output: the four lanes run side by side from a copy of the packet.
            x86::reg8 raw[76];
            memcpy(raw, rc::host(source), sizeof(raw));
            x86::reg8* const base = rc::host(0);
            const double gain = rc::ldf32(0x9e6224);
            double a[4], b[4], scale[4];
            float older[4], newer[4];
            for (x86::reg32 lane = 0; lane < 4; ++lane)
            {
                const x86::reg8* header = raw + lane * 4;
                a[lane] = get<float>(base, 0xa04330 + (header[0] & 15) * 8);
                b[lane] = get<float>(base, 0xa04334 + (header[0] & 15) * 8);
                scale[lane] = get<float>(base, 0xa04350 + (header[2] & 15) * 4);
                const x86::sreg32 first = x86::sreg32(x86::sreg8(header[1])) * 256 + (header[0] & 0xf0);
                const x86::sreg32 second = x86::sreg32(x86::sreg8(header[3])) * 256 + (header[2] & 0xf0);
                older[lane] = float(double(first) * gain);
                newer[lane] = float(double(second) * gain);
                put(base, destination + lane * 128, older[lane]);
                put(base, destination + lane * 128 + 4, newer[lane]);
            }
            for (x86::reg32 pair = 0; pair < 15; ++pair)
            {
                for (x86::reg32 lane = 0; lane < 4; ++lane)
                {
                    const x86::reg32 packed = raw[16 + pair * 4 + lane];
                    const double low = double(x86::sreg32(packed << 28)) * scale[lane];
                    const double high = double(x86::sreg32((packed >> 4) << 28)) * scale[lane];
                    const double previous = newer[lane];
                    const double even = (high + double(older[lane]) * b[lane]) + previous * a[lane];
                    const double odd = (even * a[lane] + previous * b[lane]) + low;
                    older[lane] = float(even);
                    newer[lane] = float(odd);
                    const x86::reg32 out = destination + lane * 128 + 8 + pair * 8;
                    put(base, out, older[lane]);
                    put(base, out + 4, newer[lane]);
                }
            }
            rc::st32(state + 0x34, rc::ld32(state + 0x34) + 76);
            ecx = destination + 0x274;
            edx = source + 76;
            continue;
        }
        double a[4], b[4], scale[4];
        for (x86::reg32 lane = 0; lane < 4; ++lane)
        {
            const x86::reg32 header = source + lane * 4;
            const x86::reg32 predictor = rc::ld8(header) & 15;
            a[lane] = rc::ldf32(0xa04330 + predictor * 8);
            b[lane] = rc::ldf32(0xa04334 + predictor * 8);
            scale[lane] = rc::ldf32(0xa04350 + (rc::ld8(header + 2) & 15) * 4);
            const x86::sreg32 first = x86::sreg32(x86::sreg8(rc::ld8(header + 1))) * 256
                                      + (rc::ld8(header) & 0xf0);
            const x86::sreg32 second = x86::sreg32(x86::sreg8(rc::ld8(header + 3))) * 256
                                       + (rc::ld8(header + 2) & 0xf0);
            const double gain = rc::ldf32(0x9e6224);
            rc::stf32(destination + lane * 128, float(double(first) * gain));
            rc::stf32(destination + lane * 128 + 4, float(double(second) * gain));
        }
        for (x86::reg32 pair = 0; pair < 15; ++pair)
        {
            for (x86::reg32 lane = 0; lane < 4; ++lane)
            {
                const x86::reg32 packed = rc::ld8(source + 16 + pair * 4 + lane);
                const double low = double(x86::sreg32(packed << 28)) * scale[lane];
                const double high = double(x86::sreg32((packed >> 4) << 28)) * scale[lane];
                const x86::reg32 out = destination + lane * 128 + 8 + pair * 8;
                const double previous = rc::ldf32(out - 4);
                const double even = (high + double(rc::ldf32(out - 8)) * b[lane]) + previous * a[lane];
                rc::stf32(out, float(even));
                const double odd = (even * a[lane] + previous * b[lane]) + low;
                rc::stf32(out + 4, float(odd));
            }
        }
        rc::st32(state + 0x34, rc::ld32(state + 0x34) + 76);
        ecx = destination + 0x274;
        edx = source + 76;
    }
    rc::st32(state + 0x38, rc::ld32(state + 0x38) - 128);
    cpu.eax = 128;
    cpu.ecx = ecx;
    cpu.edx = edx;
    cpu.esp += 4;
}

// Reads engine samples across compressed blocks and saturates them to signed 16-bit output.
void sub_537310(win32::WinApplication* app, x86::CPU& cpu)
{
    if (!useNative())
    {
        runGuest(sub_537310_guest, app, cpu);
        return;
    }
    const x86::reg32 stack = cpu.esp;
    const x86::reg32 object = cpu.ecx;
    const x86::reg32 start = rc::ld32(stack + 4);
    if (x86::sreg32(start) < 0)
    {
        cpu.esp += 16;
        return;
    }
    const x86::reg32 count = rc::ld32(stack + 8);
    const x86::reg32 length = rc::ld32(object + 0x14);
    if (x86::sreg32(start + count - 1) >= x86::sreg32(length))
    {
        cpu.ecx = length;
        cpu.esp += 16;
        return;
    }
    x86::reg32 ecx = rc::ld32(object + 0x2c);
    x86::reg32 decodedEax = 0;
    x86::reg32 decodedEdx = 0;
    bool decodedBlock = false;
    // The call of 0x536c40 with its argument slot below the caller's stack.
    auto decode = [&](x86::reg32 block, x86::reg32 callStack)
    {
        rc::st32(callStack + 4, block);
        decodeBlock(object, callStack + 4, decodedEax, ecx, decodedEdx);
        decodedBlock = true;
    };
    if (start / 32 != ecx)
        decode(start / 32, stack - 20);
    x86::reg32 offset = start & 31;
    const x86::reg32 destination = rc::ld32(stack + 12);
    const x86::reg32 data = rc::ld32(object + 0x24);
    const uint64_t firstBlock = start / 32;
    const uint64_t lastBlock = (uint64_t(start) + count) / 32;
    // Rounding to nearest, and the output away from everything the samples and block decodes read or write.
    if (cpu.fpu.control.rc == 0 && x86::sreg32(count) > 0 && !overlaps(destination, uint64_t(count) * 2, stack - 32, 48)
        && !overlaps(destination, uint64_t(count) * 2, object, 0xb0)
        && !overlaps(destination, uint64_t(count) * 2, 0xa5b654, 0x424)
        && uint64_t(data) + lastBlock * 19 + 19 <= 0x100000000ull
        && !overlaps(destination, uint64_t(count) * 2, x86::reg32(data + firstBlock * 19),
                     (lastBlock - firstBlock) * 19 + 19))
    {
        x86::reg8* const base = rc::host(0);
        const x86::reg32 high = x86::reg32(rc::ld16(stack + 10)) << 16;
        x86::reg32 bits = 0;
        x86::reg16 value = 0;
        auto convert = [&](x86::reg32 i, x86::reg32 sample)
        {
            bits = sample;
            value = nearest16(bits);
            ecx = value == 0x8000 ? (bits >> 31) + 0x7fff : high | value;
            put(base, destination + i * 2, x86::reg16(ecx));
        };
        // Whether a sample came after the last block decode: then ecx holds that sample's result.
        bool converted = false;
        for (x86::reg32 i = 0; i < count;)
        {
            // The samples left in the decoded block, four side by side; ecx and the stack slots follow the last one.
            const x86::reg32 end = i + std::min(count - i, 32 - offset);
            for (; i + 4 <= end; i += 4, offset += 4)
            {
                const Lanes samples = get<Lanes>(base, object + 0x30 + offset * 4);
                put(base, destination + i * 2, saturated16(samples));
                bits = samples[3];
                converted = true;
            }
            for (; i < end; ++i, ++offset)
            {
                convert(i, get<x86::reg32>(base, object + 0x30 + offset * 4));
                converted = true;
            }
            if (offset == 32)
            {
                decode(rc::ld32(object + 0x2c) + 1, stack - 24);
                offset = 0;
                converted = false;
            }
        }
        value = nearest16(bits);
        if (converted)
            ecx = value == 0x8000 ? (bits >> 31) + 0x7fff : high | value;
        rc::st32(stack + 4, bits);
        rc::st16(stack + 8, value);
        if (decodedBlock)
        {
            cpu.eax = decodedEax;
            cpu.edx = decodedEdx;
        }
        cpu.ecx = ecx;
        cpu.esp += 16;
        return;
    }
    for (x86::sreg32 i = 0; i < x86::sreg32(count); ++i)
    {
        const x86::reg32 bits = rc::ld32(object + 0x30 + offset * 4);
        rc::st32(stack + 4, bits);
        const double sample = rc::ldf32(stack + 4);
        const x86::reg16 value = x86::reg16(x86::fistInteger<x86::sreg16>(
            x86::roundIntegral(sample, cpu.fpu.control.rc)));
        rc::st16(stack + 8, value);
        ecx = rc::ld32(stack + 8);
        if (value == 0x8000)
            ecx = (bits >> 31) + 0x7fff;
        rc::st16(destination + x86::reg32(i) * 2, x86::reg16(ecx));
        if (++offset == 32)
        {
            decode(rc::ld32(object + 0x2c) + 1, stack - 24);
            offset = 0;
        }
    }
    if (decodedBlock)
    {
        cpu.eax = decodedEax;
        cpu.edx = decodedEdx;
    }
    cpu.ecx = ecx;
    cpu.esp += 16;
}

// Converts signed 16-bit samples to floats from the end of the buffer.
void sub_8aa340(win32::WinApplication* app, x86::CPU& cpu)
{
    x86::reg32 count = rc::ld32(cpu.esp + 4);
    if (!useNative() || x86::sreg32(count) <= 0)
    {
        runGuest(sub_8aa340_guest, app, cpu);
        return;
    }
    const x86::reg32 source = rc::ld32(cpu.esp + 8);
    const x86::reg32 destination = rc::ld32(cpu.esp + 12);
    while (count & 7)
    {
        --count;
        rc::stf32(destination + count * 4, float(x86::sreg16(rc::ld16(source + count * 2))));
    }
    while (count)
    {
        count -= 8;
        float samples[8];
        for (x86::reg32 i = 0; i < 8; ++i)
            samples[i] = float(x86::sreg16(rc::ld16(source + (count + i) * 2)));
        rc::stf32(destination + count * 4, samples[0]);
        for (x86::reg32 i = 6; i != 0; --i)
            rc::stf32(destination + (count + i) * 4, samples[i]);
        rc::stf32(destination + (count + 7) * 4, samples[7]);
    }
    cpu.esp += 4;
}

// Interpolates mono samples with a 32-bit fractional source position.
void sub_8b70b0(win32::WinApplication* app, x86::CPU& cpu)
{
    const x86::reg32 count = rc::ld32(cpu.esp + 4);
    if (!useNative() || x86::sreg32(count) < 0)
    {
        runGuest(sub_8b70b0_guest, app, cpu);
        return;
    }
    const x86::reg32 source = rc::ld32(cpu.esp + 8);
    const x86::reg32 destination = rc::ld32(cpu.esp + 12);
    const x86::reg32 indexSlot = rc::ld32(cpu.esp + 16);
    const x86::reg32 phaseSlot = rc::ld32(cpu.esp + 20);
    const x86::reg32 stepInteger = rc::ld32(cpu.esp + 24);
    const x86::reg32 stepFraction = rc::ld32(cpu.esp + 28);
    x86::reg32 index = rc::ld32(indexSlot);
    x86::reg32 phase = rc::ld32(phaseSlot);
    rc::st32(0xa7f190, phase);
    const uint64_t lastIndex = uint64_t(index) + uint64_t(count - 1) * stepInteger
                               + ((uint64_t(phase) + uint64_t(count - 1) * stepFraction) >> 32);
    const uint64_t readStart = uint64_t(source) + uint64_t(index) * 4;
    const uint64_t readEnd = uint64_t(source) + lastIndex * 4 + 8;
    // Without a wrap of the source index, and with the source, output and phase scratch apart, every load comes
    // before any store it could see: four samples are computed side by side and the scratch written once.
    if (count > 0 && readEnd <= 0x100000000ull
        && !overlaps(x86::reg32(readStart), readEnd - readStart, destination, uint64_t(count) * 4)
        && !overlaps(x86::reg32(readStart), readEnd - readStart, 0xa7f190, 12)
        && !overlaps(destination, uint64_t(count) * 4, 0xa7f190, 12))
    {
        x86::reg8* const base = rc::host(0);
        const double high = double(x86::sreg32(rc::ld32(0xa7f194))) * 0x1p32;
        const double scale = rc::ldf32(0xa7f198);
        x86::reg32 i = 0;
        for (; i + 4 <= count; i += 4)
        {
            double left[4], right[4], fraction[4];
            for (x86::reg32 k = 0; k < 4; ++k)
            {
                left[k] = get<float>(base, source + index * 4);
                right[k] = get<float>(base, source + index * 4 + 4);
                fraction[k] = high + double(phase);
                const x86::reg32 next = phase + stepFraction;
                index += stepInteger + (next < phase ? 1 : 0);
                phase = next;
            }
            for (x86::reg32 k = 0; k < 4; ++k)
            {
                double sample = right[k] - left[k];
                sample *= fraction[k];
                sample *= scale;
                sample += left[k];
                put(base, destination + (i + k) * 4, float(sample));
            }
        }
        for (; i < count; ++i)
        {
            const double left = get<float>(base, source + index * 4);
            double sample = double(get<float>(base, source + index * 4 + 4)) - left;
            sample *= high + double(phase);
            sample *= scale;
            sample += left;
            const x86::reg32 next = phase + stepFraction;
            index += stepInteger + (next < phase ? 1 : 0);
            phase = next;
            put(base, destination + i * 4, float(sample));
        }
        rc::st32(0xa7f190, phase);
        rc::st32(phaseSlot, phase);
        rc::st32(indexSlot, index);
        cpu.esp += 4;
        return;
    }
    for (x86::reg32 i = 0; i < count; ++i)
    {
        const double left = rc::ldf32(source + index * 4);
        double sample = double(rc::ldf32(source + index * 4 + 4)) - left;
        sample *= x86::int64ToDouble(x86::sreg64(rc::ld64(0xa7f190)));
        sample *= double(rc::ldf32(0xa7f198));
        sample += left;
        const x86::reg32 next = phase + stepFraction;
        index += stepInteger + (next < phase ? 1 : 0);
        phase = next;
        rc::st32(0xa7f190, phase);
        rc::stf32(destination + i * 4, float(sample));
    }
    rc::st32(phaseSlot, phase);
    rc::st32(indexSlot, index);
    cpu.esp += 4;
}

// Interpolates mono samples with a 16-bit fractional source position.
void sub_8abbb2(win32::WinApplication* app, x86::CPU& cpu)
{
    if (!useNative())
    {
        runGuest(sub_8abbb2_guest, app, cpu);
        return;
    }
    const x86::reg32 count = rc::ld32(cpu.esp + 4);
    const x86::reg32 source = rc::ld32(cpu.esp + 8);
    const x86::reg32 destination = rc::ld32(cpu.esp + 12);
    const x86::reg32 indexSlot = rc::ld32(cpu.esp + 16);
    const x86::reg32 phaseSlot = rc::ld32(cpu.esp + 20);
    const x86::reg32 step = rc::ld32(cpu.esp + 24);
    x86::reg32 index = rc::ld32(indexSlot);
    x86::reg32 phase = rc::ld16(phaseSlot + 2);
    const double scale = rc::ldf32(0xa04388);
    const uint64_t lastIndex = uint64_t(index) + ((uint64_t(phase) + uint64_t(count - 1) * step) >> 16);
    const uint64_t readStart = uint64_t(source) + uint64_t(index) * 4;
    const uint64_t readEnd = uint64_t(source) + lastIndex * 4 + 8;
    // A step below 2^32 - 2^16 cannot wrap the phase. With the source, output and phase argument slot apart,
    // every load comes before any store it could see: four samples are computed side by side.
    if (count > 0 && count < 0x10000000 && step <= 0xffff0000 && readEnd <= 0x100000000ull
        && !overlaps(x86::reg32(readStart), readEnd - readStart, destination, uint64_t(count) * 4)
        && !overlaps(x86::reg32(readStart), readEnd - readStart, cpu.esp + 20, 4)
        && !overlaps(destination, uint64_t(count) * 4, cpu.esp + 20, 4))
    {
        x86::reg8* const base = rc::host(0);
        x86::reg32 slot = 0;
        x86::reg32 i = 0;
        for (; i + 4 <= count; i += 4)
        {
            double left[4], right[4], fraction[4];
            for (x86::reg32 k = 0; k < 4; ++k)
            {
                slot = phase;
                left[k] = get<float>(base, source + index * 4);
                right[k] = get<float>(base, source + index * 4 + 4);
                fraction[k] = double(phase) * scale;
                phase += step;
                index += phase >> 16;
                phase &= 0xffff;
            }
            for (x86::reg32 k = 0; k < 4; ++k)
                put(base, destination + (i + k) * 4, float((right[k] - left[k]) * fraction[k] + left[k]));
        }
        for (; i < count; ++i)
        {
            slot = phase;
            const double left = get<float>(base, source + index * 4);
            const double difference = double(get<float>(base, source + index * 4 + 4)) - left;
            put(base, destination + i * 4, float(difference * (double(phase) * scale) + left));
            phase += step;
            index += phase >> 16;
            phase &= 0xffff;
        }
        rc::st32(cpu.esp + 20, slot);
        rc::st32(phaseSlot, phase << 16);
        rc::st32(indexSlot, index);
        cpu.edx = index;
        cpu.esp += 28;
        return;
    }
    for (x86::reg32 i = 0; i < count; ++i)
    {
        rc::st32(cpu.esp + 20, phase);
        const double left = rc::ldf32(source + index * 4);
        const double difference = double(rc::ldf32(source + index * 4 + 4)) - left;
        const double fraction = double(phase) * scale;
        const double sample = difference * fraction + left;
        rc::stf32(destination + i * 4, float(sample));
        phase += step;
        index += phase >> 16;
        phase &= 0xffff;
    }
    rc::st32(phaseSlot, phase << 16);
    rc::st32(indexSlot, index);
    cpu.edx = index;
    cpu.esp += 28;
}

// One-pole filter (cdecl: state, count, in, out): z = z*state[1] + state[2]*in[i], out[i] = z; state[0] = z.
void sub_8b6d00(win32::WinApplication* app, x86::CPU& cpu)
{
    const x86::reg32 state = rc::ld32(cpu.esp + 4);
    const x86::reg32 count = rc::ld32(cpu.esp + 8);
    const x86::reg32 in = rc::ld32(cpu.esp + 12);
    const x86::reg32 out = rc::ld32(cpu.esp + 16);
    // The guest loop runs once for a count of zero; the coefficients stay outside the output.
    if (!useNative() || count == 0 || count >= 0x10000000 || overlaps(out, uint64_t(count) * 4, state + 4, 8))
    {
        runGuest(sub_8b6d00_guest, app, cpu);
        return;
    }
    x86::reg8* const base = rc::host(0);
    const double a = get<float>(base, state + 4);
    const double b = get<float>(base, state + 8);
    double z = get<float>(base, state);
    for (x86::reg32 i = 0; i < count; ++i)
    {
        z *= a;
        double input = b;
        input *= double(get<float>(base, in + i * 4));
        z += input;
        put(base, out + i * 4, float(z));
    }
    put(base, state, float(z));
    cpu.esp += 4;
}

// Gain accumulation from the end (cdecl: count, gain, in, out): out[i] += gain*in[i], the count modulo four one at a
// time, then groups of four.
void sub_8aa220(win32::WinApplication* app, x86::CPU& cpu)
{
    x86::reg32 count = rc::ld32(cpu.esp + 4);
    if (!useNative() || x86::sreg32(count) <= 0)
    {
        runGuest(sub_8aa220_guest, app, cpu);
        return;
    }
    const double gain = rc::ldf32(cpu.esp + 8);
    const x86::reg32 in = rc::ld32(cpu.esp + 12);
    const x86::reg32 out = rc::ld32(cpu.esp + 16);
    x86::reg8* const base = rc::host(0);
    while (count & 3)
    {
        const x86::reg32 o = (count - 1) * 4;
        put(base, out + o, float(double(get<float>(base, in + o)) * gain + double(get<float>(base, out + o))));
        --count;
    }
    // Four products, then the four sums, stored in the guest's order (first, second, fourth, third).
    while (count)
    {
        const x86::reg32 o = (count - 4) * 4;
        const double p0 = double(get<float>(base, in + o)) * gain;
        const double p1 = double(get<float>(base, in + o + 4)) * gain;
        const double p2 = double(get<float>(base, in + o + 8)) * gain;
        const double p3 = double(get<float>(base, in + o + 12)) * gain;
        const double s0 = p0 + double(get<float>(base, out + o));
        const double s1 = p1 + double(get<float>(base, out + o + 4));
        const double s2 = p2 + double(get<float>(base, out + o + 8));
        const double s3 = p3 + double(get<float>(base, out + o + 12));
        put(base, out + o, float(s0));
        put(base, out + o + 4, float(s1));
        put(base, out + o + 12, float(s3));
        put(base, out + o + 8, float(s2));
        count -= 4;
    }
    cpu.esp += 4;
}

// Decodes one 19-byte engine-audio block into 32 floating-point samples.
void sub_536c40(win32::WinApplication* app, x86::CPU& cpu)
{
    if (!useNative())
    {
        runGuest(sub_536c40_guest, app, cpu);
        return;
    }
    decodeBlock(cpu.ecx, cpu.esp + 4, cpu.eax, cpu.ecx, cpu.edx);
    cpu.esp += 8;
}

}

}
