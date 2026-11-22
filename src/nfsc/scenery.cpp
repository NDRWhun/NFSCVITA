// Switches of the scenery culling ports and shadow_scenery: a native plan of each call compared with the translated
// body's recorded calls, registers and writes; the translated results are kept.

#include "scenery.h"

#include <platform/config.h>
#include <platform/log.h>

#include <cstdio>

namespace nfsc
{

// Translated bodies of the overridden guest functions.
void sub_79fdb0_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_79afa0_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_559bd0_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_7472d0_guest(win32::WinApplication* app, x86::CPU& cpu);

namespace scenery
{

std::atomic<const x86::CPU*> g_recordCpu{ nullptr };
std::vector<x86::reg32>* g_recordLog = nullptr;

void Recorded::log(x86::reg32 callee, const x86::CPU& cpu, int args, bool thiscall)
{
    std::vector<x86::reg32>& calls = *g_recordLog;
    calls.push_back(callee);
    calls.push_back(cpu.esp);
    calls.push_back(thiscall ? cpu.ecx : 0);
    for (int i = 0; i < args; ++i)
        calls.push_back(rc::ld32(cpu.esp + 4 + 4 * x86::reg32(i)));
}

namespace
{

// Frame allocator, drawn model and per-kind counters StuffScenery writes through its inlined leaf functions.
const x86::reg32 DrawingModel = 0x00b69c04;
const x86::reg32 FrameCursor = 0x00a8ff34;
const x86::reg32 FrameFull = 0x00a8ff44;
const x86::reg32 FrameOverflow = 0x00a8ff48;
const x86::reg32 ModelCounts = 0x00b42960;
const x86::reg32 ModelMeshes = 0x00b42e48;
const int Kinds = 8;
const unsigned MaxReports = 16;

int s_native = -1;
int s_shadow = -1;

bool nativeOn()
{
    if (s_native < 0)
        s_native = pf::Config::getBool("scenery_native", false) ? 1 : 0;
    return s_native != 0;
}

bool shadowOn()
{
    if (s_shadow < 0)
        s_shadow = pf::Config::getBool("shadow_scenery", false) ? 1 : 0;
    return s_shadow != 0;
}

struct Counts
{
    const char* name;
    unsigned compared;
    unsigned differ;
    unsigned unplanned;
    unsigned reports;
};

Counts s_treeCull = { "TreeCull", 0, 0, 0, 0 };
Counts s_stuffScenery = { "StuffScenery", 0, 0, 0, 0 };

// The plan and the recording of the call being compared (one display call at a time).
Plan s_plan;
std::vector<x86::reg32> s_recorded;

// One difference of the compared call; the first MaxReports of each function are logged.
void report(Counts& counts, unsigned& found, const char* what)
{
    ++found;
    if (counts.reports >= MaxReports)
        return;
    ++counts.reports;
    pf::logInfo("shadow_scenery: %s call %u: %s", counts.name, counts.compared, what);
}

void compareWord(Counts& counts, unsigned& found, const char* what, x86::reg32 planned, x86::reg32 translated)
{
    if (planned == translated)
        return;
    char text[96];
    snprintf(text, sizeof(text), "%s native 0x%08x translated 0x%08x", what, unsigned(planned), unsigned(translated));
    report(counts, found, text);
}

void compareCalls(Counts& counts, unsigned& found)
{
    const std::vector<x86::reg32>& a = s_plan.calls;
    const std::vector<x86::reg32>& b = s_recorded;
    size_t k = 0;
    while (k < a.size() && k < b.size() && a[k] == b[k])
        ++k;
    if (k == a.size() && k == b.size())
        return;
    char text[256];
    int used = snprintf(text, sizeof(text), "call log word %u of %u/%u: native", unsigned(k), unsigned(a.size()),
                        unsigned(b.size()));
    for (size_t i = k; i < a.size() && i < k + 6 && used < 200; ++i)
        used += snprintf(text + used, sizeof(text) - size_t(used), " %08x", unsigned(a[i]));
    used += snprintf(text + used, sizeof(text) - size_t(used), " translated");
    for (size_t i = k; i < b.size() && i < k + 6 && used < 240; ++i)
        used += snprintf(text + used, sizeof(text) - size_t(used), " %08x", unsigned(b[i]));
    report(counts, found, text);
}

void compareRegisters(Counts& counts, unsigned& found, const x86::CPU& planned, const x86::CPU& translated)
{
    compareWord(counts, found, "ebx", planned.ebx, translated.ebx);
    compareWord(counts, found, "ebp", planned.ebp, translated.ebp);
    compareWord(counts, found, "esi", planned.esi, translated.esi);
    compareWord(counts, found, "edi", planned.edi, translated.edi);
    compareWord(counts, found, "esp", planned.esp, translated.esp);
}

// Runs the translated body with the calls of its recorded callees logged into s_recorded.
void recordTranslated(void (*body)(win32::WinApplication*, x86::CPU&), win32::WinApplication* app, x86::CPU& cpu)
{
    s_recorded.clear();
    g_recordLog = &s_recorded;
    g_recordCpu.store(&cpu, std::memory_order_relaxed);
    body(app, cpu);
    g_recordCpu.store(nullptr, std::memory_order_relaxed);
}

void shadowTreeCull(win32::WinApplication* app, x86::CPU& cpu)
{
    s_plan.calls.clear();
    x86::CPU planned = cpu;
    const bool complete = treeCull(app, planned, &s_plan);
    if (planned.terminate)
    {
        cpu.terminate = true;
        return;
    }
    recordTranslated(&sub_79fdb0_guest, app, cpu);
    if (cpu.terminate)
        return;
    if (!complete)
    {
        ++s_treeCull.unplanned;
        return;
    }
    ++s_treeCull.compared;
    unsigned found = 0;
    compareCalls(s_treeCull, found);
    compareRegisters(s_treeCull, found, planned, cpu);
    if (found)
        ++s_treeCull.differ;
}

void shadowStuffScenery(win32::WinApplication* app, x86::CPU& cpu)
{
    const x86::reg32 esp0 = cpu.esp;
    x86::reg32 counts[Kinds], meshes[Kinds];
    for (int k = 0; k < Kinds; ++k)
    {
        counts[k] = rc::ld32(ModelCounts + 4 * x86::reg32(k));
        meshes[k] = rc::ld32(ModelMeshes + 4 * x86::reg32(k));
        s_plan.modelCounts[k] = 0;
        s_plan.modelMeshes[k] = 0;
    }
    s_plan.calls.clear();
    s_plan.model = rc::ld32(DrawingModel);
    s_plan.frameCursor = rc::ld32(FrameCursor);
    s_plan.frameFull = rc::ld32(FrameFull);
    s_plan.frameOverflow = rc::ld32(FrameOverflow);
    s_plan.argumentSlot = rc::ld32(esp0 + 4);
    x86::CPU planned = cpu;
    const bool complete = stuffScenery(app, planned, &s_plan);
    if (planned.terminate)
    {
        cpu.terminate = true;
        return;
    }
    recordTranslated(&sub_79afa0_guest, app, cpu);
    if (cpu.terminate)
        return;
    if (!complete)
    {
        ++s_stuffScenery.unplanned;
        return;
    }
    ++s_stuffScenery.compared;
    unsigned found = 0;
    compareCalls(s_stuffScenery, found);
    compareRegisters(s_stuffScenery, found, planned, cpu);
    compareWord(s_stuffScenery, found, "view argument slot", s_plan.argumentSlot, rc::ld32(esp0 + 4));
    compareWord(s_stuffScenery, found, "drawn model", s_plan.model, rc::ld32(DrawingModel));
    compareWord(s_stuffScenery, found, "frame cursor", s_plan.frameCursor, rc::ld32(FrameCursor));
    compareWord(s_stuffScenery, found, "frame full", s_plan.frameFull, rc::ld32(FrameFull));
    compareWord(s_stuffScenery, found, "frame overflow", s_plan.frameOverflow, rc::ld32(FrameOverflow));
    for (int k = 0; k < Kinds; ++k)
    {
        compareWord(s_stuffScenery, found, "model count", s_plan.modelCounts[k],
                    rc::ld32(ModelCounts + 4 * x86::reg32(k)) - counts[k]);
        compareWord(s_stuffScenery, found, "mesh count", s_plan.modelMeshes[k],
                    rc::ld32(ModelMeshes + 4 * x86::reg32(k)) - meshes[k]);
    }
    if (found)
        ++s_stuffScenery.differ;
}

// Shadowed only outside a recording (the recorded translated body of another shadowed call).
bool shadowNow()
{
    return shadowOn() && !g_recordCpu.load(std::memory_order_relaxed);
}

}

void stuffSceneryEntry(win32::WinApplication* app, x86::CPU& cpu)
{
    if (shadowNow())
        shadowStuffScenery(app, cpu);
    else if (nativeOn())
        stuffScenery(app, cpu, nullptr);
    else
        sub_79afa0_guest(app, cpu);
}

void logShadow()
{
    if (s_shadow <= 0)
        return;
    for (const Counts* c : { &s_treeCull, &s_stuffScenery })
        pf::logInfo("shadow_scenery: %s %u calls compared, %u differ, %u not planned (node stack overflow)", c->name,
                    c->compared, c->differ, c->unplanned);
}

}

namespace native
{

// ScenerySectionHeader::TreeCull (0x79fdb0).
void sub_79fdb0(win32::WinApplication* app, x86::CPU& cpu)
{
    if (scenery::shadowNow())
        scenery::shadowTreeCull(app, cpu);
    else if (scenery::nativeOn())
        scenery::treeCull(app, cpu, nullptr);
    else
        sub_79fdb0_guest(app, cpu);
}

// ScenerySectionHeader::DrawAScenery (0x79fa60, thiscall: index, cull info, visibility state; ret 0xc).
void sub_79fa60(win32::WinApplication* app, x86::CPU& cpu)
{
    scenery::Recorded recorded(0x79fa60, cpu, 3, true);
    scenery::drawAScenery(app, cpu);
}

// eView::GetVisibleState (0x559bd0, thiscall: box min, box max, matrix; ret 0xc), a jump to 0x71b630.
void sub_559bd0(win32::WinApplication* app, x86::CPU& cpu)
{
    scenery::Recorded recorded(0x559bd0, cpu, 3, true);
    sub_559bd0_guest(app, cpu);
}

// StuffScenery's light context set-up (0x7472d0, cdecl: context, light database, matrix, camera, eye, view, solid
// flags).
void sub_7472d0(win32::WinApplication* app, x86::CPU& cpu)
{
    scenery::Recorded recorded(0x7472d0, cpu, 7, false);
    sub_7472d0_guest(app, cpu);
}

}
}
