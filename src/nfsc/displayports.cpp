// Native ports of the display call's hot game functions. Each keeps the guest stack frame, the arguments and the
// thiscall object of every call, the memory it writes and the callee-saved registers as the translated code does;
// display_ports=0 runs the translated bodies instead.

#include <native.h>
#include <platform/config.h>
#include <recomp.h>

namespace nfsc
{

// Translated bodies of the overridden guest functions, and the guest functions the ports call.
void sub_79fa60_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_712c90(win32::WinApplication* app, x86::CPU& cpu);
void sub_559bd0(win32::WinApplication* app, x86::CPU& cpu);
void sub_79f9c0(win32::WinApplication* app, x86::CPU& cpu);
void sub_70fb50(win32::WinApplication* app, x86::CPU& cpu);
void sub_79a910(win32::WinApplication* app, x86::CPU& cpu);
void sub_79a960(win32::WinApplication* app, x86::CPU& cpu);
void sub_7110c0(win32::WinApplication* app, x86::CPU& cpu);
void sub_402850(win32::WinApplication* app, x86::CPU& cpu);
void sub_70f850(win32::WinApplication* app, x86::CPU& cpu);
void sub_70f820(win32::WinApplication* app, x86::CPU& cpu);
void sub_87eac0(win32::WinApplication* app, x86::CPU& cpu);
void sub_7b39b0(win32::WinApplication* app, x86::CPU& cpu);
void sub_46efb0(win32::WinApplication* app, x86::CPU& cpu);

namespace
{

int s_native = -1;

bool useNative()
{
    if (s_native < 0)
        s_native = pf::Config::getBool("display_ports", true) ? 1 : 0;
    return s_native != 0;
}

// Guest stack of a port: pushes and calls as the translated code makes them.
struct GuestStack
{
    win32::WinApplication* app;
    x86::CPU& cpu;
    x86::reg32 esp;

    void push(x86::reg32 value)
    {
        esp -= 4;
        rc::st32(esp, value);
    }

    // Calls a guest function with the return address slot below the pushed arguments; false when the thread is to
    // stop. The callee removes its own arguments (stdcall, thiscall); cdecl callers drop them with pop().
    bool call(void (*function)(win32::WinApplication*, x86::CPU&), x86::reg32 returnAddress)
    {
        esp -= 4;
        rc::st32(esp, returnAddress);
        cpu.esp = esp;
        function(app, cpu);
        if (cpu.terminate)
            return false;
        esp = cpu.esp;
        return true;
    }

    void pop(x86::reg32 bytes)
    {
        esp += bytes;
    }
};

// ScenerySectionHeader::DrawAScenery (thiscall: scenery index, cull info, visibility check) fields.
const x86::reg32 ConstOne = 0x009ee7c8;        // 1.0f
const x86::reg32 ConstDistanceBias = 0x009ee7cc;
const x86::reg32 ConstNearDistance = 0x009ee7d0;
const x86::reg32 ConstLodRatio = 0x009ee7dc;
const x86::reg32 ConstWindScale = 0x009ee7a4;

}

namespace native
{

// ScenerySectionHeader::DrawAScenery (0x79fa60, thiscall, ret 0xc): culls one scenery instance of the section
// against the cull info's view and level of detail and appends its model to the cull info's draw list.
void sub_79fa60(win32::WinApplication* app, x86::CPU& cpu)
{
    if (!useNative())
    {
        sub_79fa60_guest(app, cpu);
        return;
    }
    const x86::reg32 esp0 = cpu.esp;
    const x86::reg32 header = cpu.ecx;
    const x86::reg32 index = rc::ld32(esp0 + 4);
    const x86::reg32 cull = rc::ld32(esp0 + 8);
    const x86::reg32 checkSlot = esp0 + 0xc;
    const x86::reg32 ebx0 = cpu.ebx, esi0 = cpu.esi, edi0 = cpu.edi, ebp0 = cpu.ebp;
    // push ebp; mov ebp, esp; and esp, -16; sub esp, 0x54; push ebx, esi, edi.
    rc::st32(esp0 - 4, ebp0);
    const x86::reg32 ebp = esp0 - 4;
    x86::reg32 frame = (ebp & ~15u) - 0x54;
    rc::st32(frame - 4, ebx0);
    rc::st32(frame - 8, esi0);
    rc::st32(frame - 12, edi0);
    frame -= 12;
    GuestStack stack{ app, cpu, frame };
    // Locals of the translated frame.
    const x86::reg32 modelSlot = frame + 0x14;
    const x86::reg32 lodSlot = frame + 0x18;
    const x86::reg32 distanceSlot = frame + 0x1c;

    const x86::reg32 info = rc::ld32(header + 0x20) + index * 0x60;
    cpu.ebp = ebp;
    cpu.esi = info;
    cpu.edi = cull;
    const auto finish = [&]()
    {
        cpu.ebx = ebx0;
        cpu.esi = esi0;
        cpu.edi = edi0;
        cpu.ebp = ebp0;
        cpu.esp = esp0 + 0x10;
    };

    const x86::reg32 bit = rc::ld32(cull + 0x44);
    rc::st32(lodSlot, header);
    if (x86::sreg32(bit) >= 0)
    {
        const x86::reg32 row = x86::reg32(x86::sreg32(x86::sreg16(rc::ld16(info + 0x1c))) << 7);
        const x86::reg32 byteIndex = row + x86::reg32(x86::sreg32(bit) >> 3);
        if (rc::ld8(byteIndex + rc::ld32(header + 0x34)) & x86::reg8(1u << (bit & 7)))
        {
            finish();
            return;
        }
    }
    x86::reg32 flags = rc::ld32(info + 0xc);
    if (flags & 0x100)
    {
        if (!stack.call(&sub_712c90, 0x79fabb))
            return;
        if (!x86::reg8(cpu.eax))
        {
            finish();
            return;
        }
    }
    if ((flags & 0x8000000) || (flags & 0x80))
        flags |= 0x8000080;
    if (((flags ^ 0xffffffc0u) & rc::ld32(cull + 4)) & 0x80000ff)
    {
        finish();
        return;
    }
    if (rc::ld32(checkSlot) == 1)
    {
        stack.push(0);
        stack.push(info + 0x10);
        stack.push(info);
        cpu.ecx = rc::ld32(cull);
        if (!stack.call(&sub_559bd0, 0x79fafd))
            return;
        rc::st32(checkSlot, cpu.eax);
        if (!cpu.eax)
        {
            finish();
            return;
        }
    }
    const x86::reg32 instances = rc::ld32(rc::ld32(lodSlot) + 0x18);
    const x86::reg32 instance = instances + x86::reg32(x86::sreg32(x86::sreg16(rc::ld16(info + 0x54)))) * 0x48;
    cpu.ebx = instance;
    stack.push(distanceSlot);
    stack.push(instances);
    {
        x86::Float distance = x86::Float(rc::ldf32(instance + 0x38));
        distance += x86::Float(x86::Float(rc::ldf32a(ConstOne)));
        rc::stf32(stack.esp, float(distance));
    }
    stack.push(info + 0x20);
    stack.push(cull);
    cpu.eax = info + 0x20;
    if (!stack.call(&sub_79f9c0, 0x79fb35))
        return;
    stack.pop(0x10);
    x86::reg32 lod = cpu.eax;
    rc::st32(lodSlot, lod);
    if (x86::sreg32(lod) <= 1)
    {
        finish();
        return;
    }
    const x86::reg32 infoFlags = rc::ld32(info + 0xc);
    if (infoFlags & 0x2000000)
    {
        lod += 0xa;
        rc::st32(lodSlot, lod);
    }
    const x86::reg32 cullFlags = rc::ld32(cull + 4);
    rc::st32(modelSlot, 0);
    // Model choice: 0 the high-detail model at +0x30, 1 the low one at +0x34, 2 the one at +0x28, 3 none.
    enum { High, Low, Third, None, Ratio } choice = None;
    if (cullFlags & 0x3000)
    {
        if (infoFlags & 0x200)
            choice = x86::sreg32(lod) < 0x20 ? None : High;
        else if (x86::sreg32(lod) < 0x20)
            choice = None;
        else
            choice = (infoFlags & 0x1000100) ? Third : Low;
    }
    else if (cullFlags & 0x40)
    {
        choice = x86::sreg32(lod) < 0x20 ? None : High;
    }
    else
    {
        if (!stack.call(&sub_70fb50, 0x79fba8))
            return;
        const x86::sreg32 detail = x86::sreg32(cpu.eax);
        lod = rc::ld32(lodSlot);
        if (detail >= 3)
            choice = x86::sreg32(lod) < 0x17 ? None : High;
        else
            choice = x86::sreg32(lod) < 0x12 ? None : Ratio;
    }
    if (choice == Ratio)
    {
        choice = None;
        bool ratioDone = false;
        const x86::reg32 model = rc::ld32(instance + 0x28);
        const x86::reg32 solid = model ? rc::ld32(model + 0xc) : 0;
        if (solid && x86::sreg16(rc::ld16(solid + 0x14)) >= 0x28)
        {
            x86::Float size = x86::Float(rc::ldf32(solid + 0x9c));
            if (__builtin_isless(size, x86::Float(rc::ldf32a(ConstOne))))
                size = x86::Float(rc::ldf32a(ConstOne));
            const x86::Float ratio = x86::Float(x86::sreg32(rc::ld32(lodSlot))) / x86::Float(size);
            const x86::Float near = x86::Float(rc::ldf32(distanceSlot));
            if (!__builtin_isless(near, x86::Float(rc::ldf32a(ConstNearDistance))))
            {
                if (!__builtin_isgreaterequal(ratio, x86::Float(rc::ldf32a(ConstLodRatio))))
                {
                    choice = High;
                    ratioDone = true;
                }
            }
        }
        if (!ratioDone)
        {
            if (!stack.call(&sub_7110c0, 0x79fc7d))
                return;
            choice = cpu.eax != 2 ? High : Third;
        }
    }
    if (choice == High)
        rc::st32(modelSlot, rc::ld32(instance + 0x30));
    else if (choice == Low)
        rc::st32(modelSlot, rc::ld32(instance + 0x34));
    else if (choice == Third)
        rc::st32(modelSlot, rc::ld32(instance + 0x28));
    stack.push(instance);
    if (!stack.call(&sub_79a910, 0x79fbc3))
        return;
    stack.pop(4);
    if (cpu.eax)
    {
        stack.push(info);
        stack.push(instance);
        stack.push(cull);
        if (!stack.call(&sub_79a960, 0x79fbd2))
            return;
        stack.pop(0xc);
    }
    const x86::reg32 model = rc::ld32(modelSlot);
    if (!model)
    {
        finish();
        return;
    }
    if (rc::ld32(info + 0xc) & 0x400)
    {
        const x86::reg32 entry = rc::ld32(cull + 0xc);
        if (entry >= rc::ld32(cull + 0x10))
        {
            finish();
            return;
        }
        rc::st32(cull + 0xc, entry + 0xc);
        rc::st32(entry, model + rc::ld32(checkSlot));
        rc::st32(entry + 4, 0);
        rc::st32(entry + 8, info);
        finish();
        return;
    }
    stack.push(1);
    if (!stack.call(&sub_402850, 0x79fc99))
        return;
    const x86::reg32 matrix = cpu.eax;
    stack.pop(4);
    if (!matrix)
    {
        finish();
        return;
    }
    cpu.ebx = matrix;
    stack.push(matrix);
    cpu.ecx = info;
    if (!stack.call(&sub_70f850, 0x79fcae))
        return;
    stack.push(matrix + 0x30);
    cpu.ecx = info;
    if (!stack.call(&sub_70f820, 0x79fcb9))
        return;
    if ((rc::ld32(info + 0xc) & rc::ld32(cull + 4)) & 0x100)
    {
        x86::Float z = x86::Float(rc::ldf32(matrix + 0x38));
        z += x86::Float(x86::Float(rc::ldf32a(ConstDistanceBias)));
        rc::stf32(matrix + 0x38, float(z));
    }
    if (rc::ld32(cull + 4) & 0x1000)
    {
        x86::Float x = x86::Float(rc::ldf32(matrix + 0x28));
        x = -x;
        rc::stf32(matrix + 0x28, float(x));
    }
    const x86::reg32 entry = rc::ld32(cull + 0xc);
    rc::st32(distanceSlot, entry);
    if (entry >= rc::ld32(cull + 0x10))
    {
        finish();
        return;
    }
    rc::st32(cull + 0xc, entry + 0xc);
    rc::st32(entry, model + rc::ld32(checkSlot));
    if ((rc::ld32(info + 0xc) & 0x4000) && (rc::ld32(cull + 4) & 0x8000))
    {
        // Wind sway: a rotation by the angle (scaled +0x30, whole degrees mod 360) of the view applied to the matrix.
        x86::Float angle = x86::Float(rc::ldf32(matrix + 0x30));
        stack.push(matrix);
        angle *= x86::Float(x86::Float(rc::ldf32a(ConstWindScale)));
        stack.esp -= 4;
        cpu.esp = stack.esp;
        cpu.fpu.regs[(cpu.fpu.count + 1) & 7] = angle;
        cpu.fpu.count = cpu.fpu.count + 1;
        sub_87eac0(app, cpu);
        if (cpu.terminate)
            return;
        stack.esp += 4;
        const x86::reg32 whole = cpu.eax;
        const rc::Division degrees = rc::divideSigned(x86::reg32(x86::sreg32(whole) >> 31), whole, 0x168);
        stack.push(degrees.remainder);
        const x86::reg32 rotation = stack.esp + 0x28;
        stack.push(rotation);
        stack.push(rc::ld32(cull));
        stack.esp -= 4;
        cpu.esp = stack.esp;
        sub_7b39b0(app, cpu);
        if (cpu.terminate)
            return;
        stack.esp += 4;
        const x86::reg32 rotated = stack.esp + 0x30;
        stack.push(rotated);
        stack.push(matrix);
        stack.push(matrix);
        stack.esp -= 4;
        cpu.esp = stack.esp;
        sub_46efb0(app, cpu);
        if (cpu.terminate)
            return;
        stack.esp += 4;
        const x86::reg32 reloaded = rc::ld32(distanceSlot);
        stack.pop(0x1c);
        rc::st32(reloaded + 4, matrix);
        rc::st32(reloaded + 8, info);
        finish();
        return;
    }
    rc::st32(entry + 4, matrix);
    rc::st32(entry + 8, info);
    finish();
}

}
}
