// Native ports of the display call's hot game functions. Each keeps the guest stack frame, the arguments and the
// thiscall object of every call, the memory it writes and the callee-saved registers as the translated code does;
// display_ports=0 runs the translated DrawAScenery instead, scenery_native (scenery.cpp) picks the other two.

#include "scenery.h"

#include <native.h>
#include <platform/config.h>
#include <recomp.h>

namespace nfsc
{

// Translated bodies of the overridden guest functions, and the guest functions the ports call.
void sub_79fa60_guest(win32::WinApplication* app, x86::CPU& cpu);
namespace native
{
void sub_559bd0(win32::WinApplication* app, x86::CPU& cpu);
void sub_79fa60(win32::WinApplication* app, x86::CPU& cpu);
void sub_7472d0(win32::WinApplication* app, x86::CPU& cpu);
void sub_729320(win32::WinApplication* app, x86::CPU& cpu);
}
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

// GrandSceneryCullInfo::StuffScenery data; its leaf callees 0x558aa0, 0x402800 and 0x73a4b0 are inlined.
const x86::reg32 ConstLightDistance = 0x009ee7f0;   // squared distance under which a model gets its own lights
const x86::reg32 DrawingModel = 0x00b69c04;
const x86::reg32 LightingOff = 0x00ab0a98;
const x86::reg32 IdentityMatrix = 0x00b1def0;
const x86::reg32 WorldLights = 0x00a6cfe0;
const x86::reg32 FrameCursor = 0x00a8ff34;
const x86::reg32 FrameLimit = 0x00a8ff38;
const x86::reg32 FrameFull = 0x00a8ff44;
const x86::reg32 FrameOverflow = 0x00a8ff48;
const x86::reg32 ModelCounts = 0x00b42960;
const x86::reg32 ModelMeshes = 0x00b42e48;
const x86::reg32 LightContextSize = 0x1d0;

// Logs a call of the plan: callee, esp at its entry, its object (0 for cdecl), then its stack arguments.
void logCall(scenery::Plan& plan, x86::reg32 callee, x86::reg32 esp, x86::reg32 object,
             std::initializer_list<x86::reg32> args)
{
    plan.calls.push_back(callee);
    plan.calls.push_back(esp);
    plan.calls.push_back(object);
    plan.calls.insert(plan.calls.end(), args);
}

// What StuffScenery's inlined leaf functions write: to guest memory, or to the plan.
class StuffWrites
{
public:
    explicit StuffWrites(scenery::Plan* plan) : m_plan(plan) {}

    x86::reg32 model() const
    {
        return m_plan ? m_plan->model : rc::ld32(DrawingModel);
    }

    void setModel(x86::reg32 model)
    {
        if (m_plan)
            m_plan->model = model;
        else
            rc::st32(DrawingModel, model);
    }

    // 0x402800: bytes rounded up to 16 from the frame block, 0 (and the overflow counted) when they do not fit.
    x86::reg32 allocate(x86::reg32 bytes)
    {
        const x86::reg32 size = (bytes + 0xf) & ~0xfu;
        const x86::reg32 cursor = m_plan ? m_plan->frameCursor : rc::ld32(FrameCursor);
        const x86::reg32 end = cursor + size;
        if (end < rc::ld32(FrameLimit))
        {
            if (m_plan)
                m_plan->frameCursor = end;
            else
                rc::st32(FrameCursor, end);
            return cursor;
        }
        if (m_plan)
        {
            m_plan->frameFull = 1;
            m_plan->frameOverflow += size;
        }
        else
        {
            rc::st32(FrameFull, 1);
            rc::st32(FrameOverflow, rc::ld32(FrameOverflow) + size);
        }
        return 0;
    }

    // 0x73a4b0: one model of the kind drawn, with its mesh count.
    void count(x86::reg32 kind, x86::reg32 model)
    {
        const x86::reg32 solid = rc::ld32(model + 0xc);
        if (!solid)
            return;
        const x86::reg32 meshes = x86::reg32(x86::sreg32(x86::sreg16(rc::ld16(solid + 0x14))));
        if (m_plan)
        {
            ++m_plan->modelCounts[kind & 7];
            m_plan->modelMeshes[kind & 7] += meshes;
            return;
        }
        rc::st32(ModelCounts + kind * 4, rc::ld32(ModelCounts + kind * 4) + 1);
        rc::st32(ModelMeshes + kind * 4, rc::ld32(ModelMeshes + kind * 4) + meshes);
    }

private:
    scenery::Plan* m_plan;
};

// 0x558aa0 (thiscall: model): whether the model's solid takes per-model lights.
bool usesLights(x86::reg32 model)
{
    const x86::reg32 solid = rc::ld32(model + 0xc);
    if (!solid)
        return false;
    const x86::reg16 flags = rc::ld16(solid + 0xe);
    return (flags & 0x40) || (flags & 0x800);
}

}

namespace scenery
{

// ScenerySectionHeader::DrawAScenery (0x79fa60, thiscall, ret 0xc): culls one scenery instance of the section
// against the cull info's view and level of detail and appends its model to the cull info's draw list.
void drawAScenery(win32::WinApplication* app, x86::CPU& cpu)
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

// ScenerySectionHeader::TreeCull (0x79fdb0, thiscall: cull info; ret 4): walks the scenery tree with node and state
// stacks in the frame; nodes in state 1 are tested against the view, scenery children go to DrawAScenery.
bool treeCull(win32::WinApplication* app, x86::CPU& cpu, Plan* plan)
{
    const x86::reg32 esp0 = cpu.esp;
    // sub esp, 0x14c; push ebx, ebp, esi, edi.
    const x86::reg32 frame = esp0 - 0x15c;
    rc::st32(frame + 0xc, cpu.ebx);
    rc::st32(frame + 8, cpu.ebp);
    rc::st32(frame + 4, cpu.esi);
    rc::st32(frame, cpu.edi);
    x86::reg32 header = cpu.ecx;
    rc::st32(frame + 0x18, header);
    rc::st32(frame + 0x5c, rc::ld32(header + 0x2c));
    rc::st8(frame + 0x1c, 1);
    const x86::reg32 stackBottom = frame + 0x5c;
    x86::reg32 nodes = frame + 0x60;
    x86::reg32 states = frame + 0x1d;
    GuestStack stack{ app, cpu, frame };
    do
    {
        x86::reg8 state = rc::ld8(states - 1);
        const x86::reg32 node = rc::ld32(nodes - 4);
        nodes -= 4;
        --states;
        cpu.ebp = node;
        cpu.esi = nodes;
        cpu.edi = states;
        if (state == 1)
        {
            const x86::reg32 cull = rc::ld32(frame + 0x160);
            stack.esp = frame;
            stack.push(0);
            stack.push(node + 0x10);
            stack.push(node);
            cpu.ecx = rc::ld32(cull);
            if (plan)
                logCall(*plan, 0x559bd0, stack.esp - 4, cpu.ecx, { node, node + 0x10, 0 });
            if (!stack.call(&native::sub_559bd0, 0x79fdf6))
                return false;
            header = rc::ld32(frame + 0x18);
            state = x86::reg8(cpu.eax);
        }
        if (state && x86::sreg16(rc::ld16(node + 0xc)) > 0)
        {
            x86::reg32 child = node + 0x1c;
            x86::sreg32 k = 0;
            do
            {
                const x86::sreg32 index = x86::sreg16(rc::ld16(child));
                if (index >= 0)
                {
                    stack.esp = frame;
                    stack.push(state);
                    const x86::reg32 cull = rc::ld32(frame + 0x160);
                    stack.push(cull);
                    stack.push(x86::reg32(index));
                    cpu.ecx = header;
                    cpu.ebx = state;
                    if (plan)
                        logCall(*plan, 0x79fa60, stack.esp - 4, header, { x86::reg32(index), cull, state });
                    else if (!stack.call(&native::sub_79fa60, 0x79fe37))
                        return false;
                    header = rc::ld32(frame + 0x18);
                }
                else
                {
                    // A plan stops where the translated code would write the node stack over its return address.
                    if (plan && nodes >= esp0)
                        return false;
                    rc::st32(nodes, rc::ld32(header + 0x2c) - x86::reg32(index) * 0x30);
                    nodes += 4;
                    rc::st8(states, state);
                    ++states;
                }
                child += 2;
                ++k;
            } while (k < x86::sreg16(rc::ld16(node + 0xc)));
        }
    } while (nodes != stackBottom);
    cpu.edi = rc::ld32(frame);
    cpu.esi = rc::ld32(frame + 4);
    cpu.ebp = rc::ld32(frame + 8);
    cpu.ebx = rc::ld32(frame + 0xc);
    cpu.ecx = header;
    cpu.esp = esp0 + 8;
    return true;
}

void treeCullNative(win32::WinApplication* app, x86::CPU& cpu)
{
    treeCull(app, cpu, nullptr);
}

// GrandSceneryCullInfo::StuffScenery (0x79afa0, thiscall: view, flags; ret 8): submits the view's culled models
// (0x729320) with their render flags; models with own lights near the eye get a light context.
bool stuffScenery(win32::WinApplication* app, x86::CPU& cpu, Plan* plan)
{
    const x86::reg32 esp0 = cpu.esp;
    const x86::reg32 grand = cpu.ecx;
    const x86::reg32 ebx0 = cpu.ebx, esi0 = cpu.esi, edi0 = cpu.edi, ebp0 = cpu.ebp;
    const x86::reg32 view = rc::ld32(esp0 + 4);
    const x86::reg32 flags = rc::ld32(esp0 + 8);
    const x86::reg32 eye = rc::ld32(view + 0x40) + 0x40;
    x86::reg32 exclude = 0, required = 0, include = 0;
    if (flags & 1)
        exclude = 0x1000;
    if (flags & 0x400)
        include = 0x100000;
    if (flags & 0x80)
        exclude |= 0x100;
    if (flags & 0x10)
        include = 0x2000;
    else if (flags & 8)
        required = 0x2000;
    const bool lightingFlag = (flags & 0x100) != 0;
    if (lightingFlag || (flags & 0x2200))
        include = 0x10000;
    if (flags & 0x800)
        required |= 0x400000;
    if (flags & 0x1000)
        include = 0x1000000;
    // The view argument slot holds the include mask from here on.
    if (plan)
        plan->argumentSlot = include;
    else
        rc::st32(esp0 + 4, include);
    StuffWrites writes(plan);
    const auto finish = [&]()
    {
        cpu.ebx = ebx0;
        cpu.esi = esi0;
        cpu.edi = edi0;
        cpu.ebp = ebp0;
        cpu.esp = esp0 + 0xc;
    };
    const x86::sreg32 count = x86::sreg32(rc::ld32(grand + 0x3c0));
    x86::reg32 info = grand;
    x86::sreg32 i = 0;
    while (i < count && rc::ld32(info) != view)
    {
        ++i;
        info += 0x50;
    }
    if (i >= count)
    {
        finish();
        return true;
    }
    x86::reg32 entry = rc::ld32(info + 8);
    if (entry >= rc::ld32(info + 0xc))
    {
        finish();
        return true;
    }
    // sub esp, 0x14; push ebx, esi, ebp, edi.
    const x86::reg32 frame = esp0 - 0x24;
    GuestStack stack{ app, cpu, frame };
    cpu.ebx = view;
    do
    {
        cpu.ebp = entry;
        x86::reg32 sceneryFlags = rc::ld32(rc::ld32(entry + 8) + 0xc);
        bool skip = (sceneryFlags & 0x80000) && rc::ld32(LightingOff);
        if (!skip && (sceneryFlags & 0x40000) && !rc::ld32(LightingOff) && !(sceneryFlags & 0x100))
            skip = true;
        if (!skip)
        {
            const x86::reg32 word = rc::ld32(entry);
            const x86::reg32 model = word & ~3u;
            writes.setModel(model);
            sceneryFlags = rc::ld32(rc::ld32(entry + 8) + 0xc);
            x86::reg32 bits = exclude;
            if (sceneryFlags & 0x10000)
                bits |= 0x200;
            if (sceneryFlags & 0x200)
                bits |= 0x2000;
            if (sceneryFlags & 0x1000000)
                bits |= 0x100000;
            if (sceneryFlags & 0x100)
                bits |= 0x1020000;
            if (sceneryFlags & 0x400000)
                bits |= 0x40000;
            if (sceneryFlags & 0x20000)
                bits |= 0x800000;
            if (sceneryFlags & 0x80000)
                bits |= 0x400000;
            const bool bit26 = (sceneryFlags >> 26) & 1;
            if (flags & 0x200)
            {
                if ((sceneryFlags & 0x100000) && !bit26)
                    bits |= 0x10000;
                if ((sceneryFlags & 0x200000) && !bit26)
                    bits |= 0x10000;
                if (sceneryFlags & 0x40000000)
                    bits |= 0x10000;
            }
            if ((flags & 0x2000) && (sceneryFlags & 0x40000000))
                bits |= 0x10000;
            if (lightingFlag && (sceneryFlags & 0x40300000))
                bits |= 0x10000;
            if ((word & 3) == 2)
                bits |= 4;
            const bool unmasked = !include && !required;
            const bool notExcluded = required && !(required & bits);
            if (unmasked || (bits & include) || notExcluded)
            {
                cpu.esi = bits;
                bool draw = true;
                x86::reg32 drawModel, matrix, light = 0;
                if (!rc::ld32(entry + 4))
                {
                    drawModel = model;
                    matrix = IdentityMatrix;
                }
                else if (!usesLights(model))
                {
                    drawModel = writes.model();
                    matrix = rc::ld32(entry + 4);
                }
                else
                {
                    const x86::reg32 m = rc::ld32(entry + 4);
                    x86::Float dx = x86::Float(rc::ldf32(m + 0x30));
                    dx -= x86::Float(x86::Float(rc::ldf32(eye)));
                    x86::Float dy = x86::Float(rc::ldf32(m + 0x34));
                    dy -= x86::Float(x86::Float(rc::ldf32(eye + 4)));
                    x86::Float dz = x86::Float(rc::ldf32(m + 0x38));
                    dz -= x86::Float(x86::Float(rc::ldf32(eye + 8)));
                    x86::Float sum = dz;
                    sum *= x86::Float(dz);
                    x86::Float term = dy;
                    term *= x86::Float(dy);
                    sum += term;
                    term = dx;
                    term *= x86::Float(dx);
                    sum += term;
                    cpu.fpu.compare(sum, x86::Float(rc::ldf32a(ConstLightDistance)));
                    const x86::reg8 near = x86::reg8(x86::reg8(cpu.fpu.status.word >> 8) & 5);
                    if (!x86::parity8(near))
                    {
                        drawModel = writes.model();
                        matrix = rc::ld32(entry + 4);
                        light = rc::ld32(view + 0x4c);
                    }
                    else
                    {
                        stack.esp = frame;
                        stack.push(LightContextSize);
                        const x86::reg32 context = writes.allocate(LightContextSize);
                        cpu.edi = context;
                        if (!context)
                        {
                            draw = false;
                            drawModel = matrix = 0;
                        }
                        else
                        {
                            const x86::reg32 solid = rc::ld32(writes.model() + 0xc);
                            const x86::reg32 solidFlags = rc::ld16(solid + 0xe);
                            const x86::reg32 camera = rc::ld32(view + 0x40);
                            const x86::reg32 modelMatrix = rc::ld32(entry + 4);
                            stack.esp = frame;
                            stack.push(solidFlags);
                            stack.push(view);
                            stack.push(eye);
                            stack.push(camera);
                            stack.push(modelMatrix);
                            stack.push(WorldLights);
                            stack.push(context);
                            if (plan)
                                logCall(*plan, 0x7472d0, stack.esp - 4, 0,
                                        { context, WorldLights, modelMatrix, camera, eye, view, solidFlags });
                            else if (!stack.call(&native::sub_7472d0, 0x79b2a3))
                                return false;
                            matrix = rc::ld32(entry + 4);
                            drawModel = writes.model();
                            light = context;
                            bits &= ~0x200u;
                            cpu.esi = bits;
                        }
                    }
                }
                if (draw)
                {
                    stack.esp = frame;
                    stack.push(0);
                    stack.push(0);
                    stack.push(bits);
                    stack.push(light);
                    stack.push(matrix);
                    stack.push(drawModel);
                    cpu.ecx = view;
                    if (plan)
                        logCall(*plan, 0x729320, stack.esp - 4, view, { drawModel, matrix, light, bits, 0, 0 });
                    else if (!stack.call(&native::sub_729320, 0x79b2ed))
                        return false;
                }
                const x86::reg32 viewId = rc::ld32(view + 8);
                x86::reg32 kind = 4;
                if (viewId == 4 || viewId == 5)
                    kind = 5;
                else if (x86::sreg32(viewId) >= 0x16 && x86::sreg32(viewId) <= 0x1b)
                    kind = 3;
                else if (viewId == 3)
                    kind = 6;
                writes.count(kind, writes.model());
                writes.setModel(0);
            }
        }
        entry += 0xc;
    } while (entry < rc::ld32(info + 0xc));
    finish();
    return true;
}

void stuffSceneryNative(win32::WinApplication* app, x86::CPU& cpu)
{
    stuffScenery(app, cpu, nullptr);
}

}
}
