// The game's small-block allocator (FastMem, 0xa99720) and its slot pools under host locks, so threads that run
// guest code at the same time can allocate. The common paths are native and leave memory and registers as the
// translated code does; every other path runs the translated body under the same lock. alloc_native=0 runs the
// translated bodies for every call, still locked. While a display call runs beside the game thread, frees wait for
// the barrier, so neither thread sees memory the other freed taken again.

#include "audioupdate.h"
#include "census.h"
#include "display.h"
#include "guestcall.h"
#include "guestlock.h"
#include "overlap.h"

#include <native.h>
#include <platform/config.h>
#include <recomp.h>

#include <cstring>
#include <vector>

namespace nfsc
{

// Translated bodies of the overridden guest functions.
void sub_60ba70_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_609e80_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_60a020_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_60a150_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_60d520_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_46f400_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_46f420_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_477740_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_4777a0_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_46f470_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_4770c0_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_477210_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_477540_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_4775b0_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_477860_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_4778b0_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_477b80_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_477c10_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_476d70_guest(win32::WinApplication* app, x86::CPU& cpu);

namespace
{

// Recursive: the locked translated bodies call each other.
pf::LwMutex s_fastMemLock("fastmem", true);
pf::LwMutex s_slotPoolLock("slot_pools", true);

int s_native = -1;

bool useNative()
{
    if (s_native < 0)
        s_native = pf::Config::getBool("alloc_native", true) ? 1 : 0;
    return s_native != 0;
}

typedef GuestLockGuard AllocatorGuard;

x86::reg32 arg(const x86::CPU& cpu, int index)
{
    return rc::ld32(cpu.esp + 4 + 4 * index);
}

// rep stosd / rep stosb of zeros at es:address, forward.
void zero(const x86::CPU& cpu, x86::reg32 address, x86::reg32 bytes)
{
    memset(rc::host(cpu.ees + address), 0, bytes);
}

// FastMem bucket list head of a block size of at most 0x400 bytes (16-byte steps).
x86::reg32 bucket(x86::reg32 fastMem, x86::reg32 size)
{
    return fastMem + ((size - 1) >> 4) * 4;
}

// FastMem::Alloc(size, name), thiscall: a zeroed block of the size's bucket. Larger sizes, an empty bucket and a
// backward direction flag take the translated body.
void fastMemAlloc(win32::WinApplication* app, x86::CPU& cpu)
{
    const AllocatorGuard guard(s_fastMemLock, app, cpu);
    const x86::reg32 size = arg(cpu, 0);
    const x86::reg32 head = size <= 0x400 ? bucket(cpu.ecx, size) : 0;
    const x86::reg32 block = head ? rc::ld32(head) : 0;
    if (!block || cpu.flags.df || !useNative())
    {
        sub_60ba70_guest(app, cpu);
        return;
    }
    rc::st32(head, rc::ld32(block));
    zero(cpu, block, size);
    cpu.eax = block;
    cpu.ecx = 0;
    cpu.edx = block;
    cpu.esp += 12;
}

// SlotPool::Malloc, thiscall: the first free slot, zeroed when the pool's flag 2 says so, or 0. An empty pool that
// may expand and a backward direction flag take the translated body.
void slotPoolMalloc(win32::WinApplication* app, x86::CPU& cpu)
{
    const AllocatorGuard guard(s_slotPoolLock, app, cpu);
    const x86::reg32 pool = cpu.ecx;
    const x86::reg32 slot = rc::ld32(pool + 0x10);
    const x86::reg8 flags = rc::ld8(pool + 0x14);
    if ((!slot && (flags & 1)) || cpu.flags.df || !useNative())
    {
        sub_477740_guest(app, cpu);
        return;
    }
    if (slot)
    {
        rc::st32(pool + 0x10, rc::ld32(slot));
        x86::reg32 count = rc::ld32(pool + 0x18) + 1;
        rc::st32(pool + 0x18, count);
        if (flags & 2)
        {
            const x86::sreg32 words = x86::sreg32(rc::ld32(pool + 0x28)) >> 2;
            if (words > 0)
            {
                zero(cpu, slot, x86::reg32(words) * 4);
                count = 0;
            }
            else
            {
                count = x86::reg32(words);
            }
        }
        cpu.ecx = count;
    }
    cpu.eax = slot;
    cpu.edx = slot;
    cpu.esp += 4;
}

// display_census: a slot taken from a pool.
void trackSlot(x86::reg32 slot, x86::reg32 pool, x86::reg32 site)
{
    if (census::g_tracking && slot)
        census::allocated(census::PoolSlot, slot, rc::ld32(pool + 0x28), site, pool);
}

// FastMem's global allocator and its 32-byte bucket.
const x86::reg32 FastMem = 0x00a99720;
const x86::reg32 FastMemBucket32 = 0x00a99724;
// Nodes one deferred list release may hold before the list counts as broken.
const uint32_t MaxListNodes = 1u << 20;

enum FreeKind : uint8_t
{
    FreeBucket,     // a block to the front of the FastMem bucket list at a: a = head, b = block
    FreeSlot,       // a = pool, b = slot
    FreeHeap        // b = bFree block
};

struct DeferredFree
{
    FreeKind kind;
    x86::reg32 a;
    x86::reg32 b;
};

// Frees of both threads while a display call runs beside the game thread, in order.
std::vector<DeferredFree> s_deferred;
pf::LwMutex s_deferredLock("deferred_frees");

bool deferring()
{
    return display::concurrent();
}

void deferFree(FreeKind kind, x86::reg32 a, x86::reg32 b)
{
    pf::LwLockGuard guard(s_deferredLock);
    s_deferred.push_back(DeferredFree{ kind, a, b });
}

}

namespace overlap
{

bool deferHeapFree(x86::reg32 block)
{
    if (!block || !deferring())
        return false;
    deferFree(FreeHeap, 0, block);
    return true;
}

void drainFrees(win32::WinApplication* app, x86::CPU& cpu)
{
    std::vector<DeferredFree> frees;
    {
        pf::LwLockGuard guard(s_deferredLock);
        frees.swap(s_deferred);
    }
    for (const DeferredFree& free : frees)
    {
        switch (free.kind)
        {
        case FreeBucket:
        {
            const AllocatorGuard guard(s_fastMemLock, app, cpu);
            rc::st32(free.b, rc::ld32(free.a));
            rc::st32(free.a, free.b);
            break;
        }
        case FreeSlot:
        {
            const AllocatorGuard guard(s_slotPoolLock, app, cpu);
            const x86::reg32 count = rc::ld32(free.a + 0x18);
            rc::st32(free.b, rc::ld32(free.a + 0x10));
            rc::st32(free.a + 0x18, count - 1);
            rc::st32(free.a + 0x10, free.b);
            break;
        }
        case FreeHeap:
            callGuest(app, cpu, &sub_476d70_guest, cpu.ecx, { free.b });
            break;
        }
        if (cpu.terminate)
            break;
    }
}

}

namespace native
{

// FastMem::Alloc.
void sub_60ba70(win32::WinApplication* app, x86::CPU& cpu)
{
    const x86::reg32 site = rc::ld32(cpu.esp);
    const x86::reg32 size = arg(cpu, 0);
    fastMemAlloc(app, cpu);
    if (census::g_tracking && cpu.eax)
        census::allocated(census::FastMemBlock, cpu.eax, size, site, 0);
}

// FastMem::Free(block, size, name), thiscall: the block goes to the front of its bucket; larger sizes take the
// translated body.
void sub_609e80(win32::WinApplication* app, x86::CPU& cpu)
{
    const x86::reg32 block = arg(cpu, 0);
    const x86::reg32 size = arg(cpu, 1);
    if (deferring() && size <= 0x400 && useNative())
    {
        if (census::g_tracking)
            census::released(census::FastMemBlock, block);
        deferFree(FreeBucket, bucket(cpu.ecx, size), block);
        cpu.eax = (size - 1) >> 4;
        cpu.edx = block;
        cpu.esp += 16;
        return;
    }
    const AllocatorGuard guard(s_fastMemLock, app, cpu);
    if (census::g_tracking)
        census::released(census::FastMemBlock, block);
    if (size > 0x400 || !useNative())
    {
        sub_609e80_guest(app, cpu);
        return;
    }
    const x86::reg32 head = bucket(cpu.ecx, size);
    rc::st32(block, rc::ld32(head));
    rc::st32(head, block);
    cpu.eax = (size - 1) >> 4;
    cpu.edx = block;
    cpu.esp += 16;
}

// FastMem::CreateBlock(bucket): fills a bucket from the current block.
void sub_60a020(win32::WinApplication* app, x86::CPU& cpu)
{
    const AllocatorGuard guard(s_fastMemLock, app, cpu);
    sub_60a020_guest(app, cpu);
}

// Frees an object's FastMem buffer (thiscall: 16-byte units at +0, block at +8) into its bucket; larger buffers go to
// the general allocator.
void sub_60a150(win32::WinApplication* app, x86::CPU& cpu)
{
    const x86::reg32 block = rc::ld32(cpu.ecx + 8);
    const x86::reg32 bytes = rc::ld32(cpu.ecx) << 4;
    if (deferring() && block && bytes && bytes <= 0x400)
    {
        const x86::reg32 head = bucket(FastMem, bytes);
        deferFree(FreeBucket, head, block);
        cpu.eax = (bytes - 1) >> 4;
        cpu.ecx = rc::ld32(head);
        cpu.edx = block;
        cpu.esp += 4;
        return;
    }
    const AllocatorGuard guard(s_fastMemLock, app, cpu);
    sub_60a150_guest(app, cpu);
}

// Returns a spline's node list (thiscall: list head at +0x64) to FastMem's 32-byte bucket.
void sub_60d520(win32::WinApplication* app, x86::CPU& cpu)
{
    const x86::reg32 list = cpu.ecx + 0x64;
    if (deferring() && rc::ld32(list) != list)
    {
        x86::reg32 last = 0;
        uint32_t count = 0;
        for (x86::reg32 node = rc::ld32(list); node != list; node = rc::ld32(node))
        {
            if (++count > MaxListNodes)
                x86::guestTrap(rc::ld32(cpu.esp), "spline node list without end");
            deferFree(FreeBucket, FastMemBucket32, node);
            last = node;
        }
        cpu.eax = list;
        cpu.ecx = last;
        cpu.edx = list;
        cpu.esp += 4;
        return;
    }
    const AllocatorGuard guard(s_fastMemLock, app, cpu);
    sub_60d520_guest(app, cpu);
}

// bMalloc(SlotPool*): the first free slot or 0, without expanding.
void sub_46f400(win32::WinApplication* app, x86::CPU& cpu)
{
    const AllocatorGuard guard(s_slotPoolLock, app, cpu);
    const x86::reg32 pool = arg(cpu, 0);
    const x86::reg32 site = rc::ld32(cpu.esp);
    if (!useNative())
    {
        sub_46f400_guest(app, cpu);
        trackSlot(cpu.eax, pool, site);
        return;
    }
    const x86::reg32 slot = rc::ld32(pool + 0x10);
    if (slot)
    {
        const x86::reg32 next = rc::ld32(slot);
        rc::st32(pool + 0x18, rc::ld32(pool + 0x18) + 1);
        rc::st32(pool + 0x10, next);
        cpu.edx = next;
    }
    cpu.eax = slot;
    cpu.ecx = pool;
    cpu.esp += 4;
    trackSlot(slot, pool, site);
}

// bFree(SlotPool*, slot): the slot goes to the front of the free list.
void sub_46f420(win32::WinApplication* app, x86::CPU& cpu)
{
    if (deferring() && useNative())
    {
        const x86::reg32 pool = arg(cpu, 0);
        const x86::reg32 slot = arg(cpu, 1);
        if (census::g_tracking)
            census::released(census::PoolSlot, slot);
        deferFree(FreeSlot, pool, slot);
        cpu.eax = pool;
        cpu.ecx = rc::ld32(pool + 0x18) - 1;
        cpu.edx = slot;
        cpu.esp += 4;
        return;
    }
    const AllocatorGuard guard(s_slotPoolLock, app, cpu);
    if (census::g_tracking)
        census::released(census::PoolSlot, arg(cpu, 1));
    if (!useNative())
    {
        sub_46f420_guest(app, cpu);
        return;
    }
    const x86::reg32 pool = arg(cpu, 0);
    const x86::reg32 slot = arg(cpu, 1);
    const x86::reg32 count = rc::ld32(pool + 0x18);
    rc::st32(slot, rc::ld32(pool + 0x10));
    rc::st32(pool + 0x18, count - 1);
    rc::st32(pool + 0x10, slot);
    cpu.eax = pool;
    cpu.ecx = count - 1;
    cpu.edx = slot;
    cpu.esp += 4;
}

// SlotPool::Malloc.
void sub_477740(win32::WinApplication* app, x86::CPU& cpu)
{
    const x86::reg32 pool = cpu.ecx;
    const x86::reg32 site = rc::ld32(cpu.esp);
    slotPoolMalloc(app, cpu);
    trackSlot(cpu.eax, pool, site);
}

// SlotPool list allocation of several slots.
void sub_4777a0(win32::WinApplication* app, x86::CPU& cpu)
{
    const AllocatorGuard guard(s_slotPoolLock, app, cpu);
    sub_4777a0_guest(app, cpu);
}

// SlotPool::FlushSlotPool: after an audio update and a display call running beside the caller (on the game thread when
// the audio thread calls it).
void sub_46f470(win32::WinApplication* app, x86::CPU& cpu)
{
    if (audioupdate::handOver(app, cpu, &sub_46f470))
        return;
    audioupdate::barrier(app, cpu);
    display::barrier(app, cpu);
    const AllocatorGuard guard(s_slotPoolLock, app, cpu);
    sub_46f470_guest(app, cpu);
}

// SlotPool::CleanupExpandedSlotPools: after an audio update and a display call running beside the caller.
void sub_4770c0(win32::WinApplication* app, x86::CPU& cpu)
{
    if (audioupdate::handOver(app, cpu, &sub_4770c0))
        return;
    audioupdate::barrier(app, cpu);
    display::barrier(app, cpu);
    const AllocatorGuard guard(s_slotPoolLock, app, cpu);
    sub_4770c0_guest(app, cpu);
}

// SlotPoolManager::CleanupExpandedSlotPools: after an audio update and a display call running beside the caller.
void sub_477210(win32::WinApplication* app, x86::CPU& cpu)
{
    if (audioupdate::handOver(app, cpu, &sub_477210))
        return;
    audioupdate::barrier(app, cpu);
    display::barrier(app, cpu);
    const AllocatorGuard guard(s_slotPoolLock, app, cpu);
    sub_477210_guest(app, cpu);
}

// SlotPool::NewSlotPool.
void sub_477540(win32::WinApplication* app, x86::CPU& cpu)
{
    const AllocatorGuard guard(s_slotPoolLock, app, cpu);
    sub_477540_guest(app, cpu);
}

// SlotPool::ExpandSlotPool.
void sub_4775b0(win32::WinApplication* app, x86::CPU& cpu)
{
    const AllocatorGuard guard(s_slotPoolLock, app, cpu);
    sub_4775b0_guest(app, cpu);
}

// SlotPoolManager::NewSlotPool.
void sub_477860(win32::WinApplication* app, x86::CPU& cpu)
{
    const AllocatorGuard guard(s_slotPoolLock, app, cpu);
    sub_477860_guest(app, cpu);
}

// SlotPoolManager::DeleteSlotPool: after an audio update and a display call running beside the caller.
void sub_4778b0(win32::WinApplication* app, x86::CPU& cpu)
{
    if (audioupdate::handOver(app, cpu, &sub_4778b0))
        return;
    audioupdate::barrier(app, cpu);
    display::barrier(app, cpu);
    const AllocatorGuard guard(s_slotPoolLock, app, cpu);
    sub_4778b0_guest(app, cpu);
}

// bNewSlotPool: a new pool joins the manager's list.
void sub_477b80(win32::WinApplication* app, x86::CPU& cpu)
{
    const AllocatorGuard guard(s_slotPoolLock, app, cpu);
    sub_477b80_guest(app, cpu);
}

// Creates the shared 12-byte slot pool and joins it to the manager's list.
void sub_477c10(win32::WinApplication* app, x86::CPU& cpu)
{
    const AllocatorGuard guard(s_slotPoolLock, app, cpu);
    sub_477c10_guest(app, cpu);
}

}
}
