// NotifySolidLoader (0x55dc20): after a solid list loads, every model still waiting for its solid (the list at
// 0xa901a0) is offered to eModel::ConnectSolid (0x55c830) with the list's header; a model whose solid is in it moves
// to that solid's model list. The translated function makes a full ConnectSolid call per waiting model, each with two
// QueryPerformanceCounter calls once the solid name's bucket has any solid loaded, so a notify costs about
// (waiting models) x (a guest call with two timer calls): thousands of models per loaded solid list in free roam.
// The native walk does the same per model with a few loads: the bucket count, then the binary search of the list's
// solid table (0x558360's steps), and the same list moves in the same order. The functions' time statistics
// (0xa8ff74-0xa8ff88, 0xa8ffa8), which nothing reads, are not kept.

#include "solids.h"

#include <platform/config.h>
#include <recomp.h>

namespace nfsc
{

// Translated body of the overridden guest function.
void sub_55dc20_guest(win32::WinApplication* app, x86::CPU& cpu);

namespace solids
{

namespace
{

// Set while chunks load inside a loading pass that connects the models afterwards (the other branch of 0x55dc20).
const x86::reg32 DeferConnections = 0x00a995e8;
// The models waiting for their solid: list head (next, previous) at 0xa901a0. Models: next +0, previous +4, solid
// name hash +8, connected solid +0x0C.
const x86::reg32 WaitingModels = 0x00a901a0;
const x86::reg32 ModelNext = 0x00;
const x86::reg32 ModelPrevious = 0x04;
const x86::reg32 ModelHash = 0x08;
const x86::reg32 ModelSolid = 0x0C;
// Loaded solids per name hash bucket (hash & 0x1fff), one byte each.
const x86::reg32 SolidBuckets = 0x00a901ac;
const x86::reg32 BucketMask = 0x1fff;
// Solid list header: solid count +0x0C, solid table +0x74 (8-byte entries: name hash, solid). Solid: its model list
// (next, previous) at +0x88.
const x86::reg32 HeaderCount = 0x0C;
const x86::reg32 HeaderTable = 0x74;
const x86::reg32 TableStride = 8;
const x86::reg32 SolidModels = 0x88;

int s_native = -1;

// 0x558360 with offset 0 and stride 8: the first entry holding key in the table, or 0.
x86::reg32 findEntry(x86::reg32 key, x86::reg32 base, x86::reg32 count)
{
    if (base == 0 || x86::sreg32(count) <= 0)
        return 0;
    x86::reg32 lo = 0, hi = count - 1, mid, entry;
    for (;;)
    {
        const x86::reg32 sum = lo + hi;
        mid = x86::reg32(x86::sreg32(sum - x86::reg32(x86::sreg32(sum) >> 31)) >> 1);
        entry = mid * TableStride + base;
        const x86::reg32 value = rc::ld32(entry);
        if (key < value)
            hi = mid - 1;
        else if (key > value)
            lo = mid + 1;
        else
            break;
        if (x86::sreg32(lo) > x86::sreg32(hi))
            return 0;
    }
    while (x86::sreg32(mid) > 0 && rc::ld32(entry - TableStride) == key)
    {
        entry -= TableStride;
        --mid;
    }
    return entry;
}

// eModel::ConnectSolid(hash, header) for a header: the solid of that name in the header's list, or 0.
x86::reg32 connectSolid(x86::reg32 hash, x86::reg32 header)
{
    if (!rc::ld8(SolidBuckets + (hash & BucketMask)))
        return 0;
    const x86::reg32 entry = findEntry(hash, rc::ld32(header + HeaderTable), rc::ld32(header + HeaderCount));
    return entry ? rc::ld32(entry + 4) : 0;
}

// Appends node to the list whose head (next, previous) is at head.
void append(x86::reg32 head, x86::reg32 node)
{
    const x86::reg32 tail = rc::ld32(head + ModelPrevious);
    rc::st32(tail + ModelNext, node);
    rc::st32(head + ModelPrevious, node);
    rc::st32(node + ModelPrevious, tail);
    rc::st32(node + ModelNext, head);
}

}

void notifySolidLoader(win32::WinApplication* app, x86::CPU& cpu)
{
    if (s_native < 0)
        s_native = pf::Config::getBool("solid_notify_native", true) ? 1 : 0;
    const x86::reg32 header = rc::ld32(cpu.esp + 4);
    if (!s_native || !header || rc::ld32(DeferConnections))
    {
        sub_55dc20_guest(app, cpu);
        return;
    }
    x86::reg32 node = rc::ld32(WaitingModels + ModelNext);
    while (node != WaitingModels)
    {
        const x86::reg32 next = rc::ld32(node + ModelNext);
        const x86::reg32 hash = rc::ld32(node + ModelHash);
        if (hash)
        {
            const x86::reg32 solid = connectSolid(hash, header);
            if (solid != rc::ld32(node + ModelSolid))
            {
                const x86::reg32 after = rc::ld32(node + ModelNext);
                const x86::reg32 before = rc::ld32(node + ModelPrevious);
                rc::st32(before + ModelNext, after);
                rc::st32(after + ModelPrevious, before);
                append(solid ? solid + SolidModels : WaitingModels, node);
                rc::st32(node + ModelSolid, solid);
            }
        }
        node = next;
    }
    cpu.esp += 4;
}

}
}
