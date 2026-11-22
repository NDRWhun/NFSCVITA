#include <lib/heap.h>
#include <lib/memmap.h>
#include <lib/mutex.h>
#include <platform/log.h>
#include <nfsc_layout.h>
#include <psp2/kernel/sysmem.h>
#include <cstdlib>
#include <cstring>
#include <malloc.h>
#include <vector>

namespace win32
{

// Guest heap, thread stacks and shim buffers.
static const x86::reg32 s_simulatedMemory = NFSC_GUEST_HEAP_MIB*1024*1024;
// One byte per 4 KB block of the simulated memory, page-aligned.
static const x86::reg32 s_blockMapSize = (s_simulatedMemory / 4096 + 4095) & ~4095u;
// End of the highest image section (the generated layout), page-aligned.
static const x86::reg32 s_sectionLimit = (nfsc::layout::imageEnd + 4095) & ~4095u;
// Layout: sections, block map, simulated memory.
static const x86::reg32 s_arenaSize = s_sectionLimit + s_blockMapSize + s_simulatedMemory;
static_assert(s_arenaSize % 4096 == 0, "memblock sizes are whole pages");

x86::reg8* MemMap::s_blocks = nullptr;
x86::reg32 MemMap::s_blockCount = 0;
x86::reg32 MemMap::s_blockSize = 0;
x86::reg32 MemMap::s_sectionSize = 0;
x86::reg32 MemMap::s_addressOffset = 0;
Mutex*     MemMap::s_lock = nullptr;
std::vector<MemMap*> MemMap::s_memMaps;

static SceUID s_arenaBlock = -1;

// The guest address space is one zeroed memblock; pages are always accessible.
// Runs before main, so failure is reported by MemMap::init once logging is up.
static x86::reg8* allocArena()
{
    s_arenaBlock = sceKernelAllocMemBlock("guest",SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_RW, s_arenaSize, nullptr);
    if (s_arenaBlock < 0)
        return nullptr;
    void* base = nullptr;
    sceKernelGetMemBlockBase(s_arenaBlock, &base);
    memset(base, 0, s_arenaSize);
    return static_cast<x86::reg8*>(base);
}

static void freeArena()
{
    if (s_arenaBlock >= 0)
    {
        sceKernelFreeMemBlock(s_arenaBlock);
        s_arenaBlock = -1;
    }
}

x86::reg8* const g_guestBase = allocArena();

// Blocks in use and the highest count so far; a new peak is logged every PeakLogStep blocks.
static x86::reg32 s_usedBlocks = 0;
static x86::reg32 s_peakBlocks = 0;
// Every block below this one is in use.
static x86::reg32 s_firstFree = 0;

// First block in [b, end) whose map byte is 0 (free), or end; map bytes are 0 or 1 and the map is word-aligned.
static x86::reg32 nextFree(const x86::reg8* map, x86::reg32 b, x86::reg32 end)
{
    while (b < end && (b & 3))
    {
        if (!map[b])
            return b;
        ++b;
    }
    while (b + 4 <= end)
    {
        uint32_t word;
        memcpy(&word, map + b, 4);
        if (word != 0x01010101u)
            break;
        b += 4;
    }
    while (b < end && map[b])
        ++b;
    return b;
}

// First block in [b, end) whose map byte is 1 (used), or end.
static x86::reg32 nextUsed(const x86::reg8* map, x86::reg32 b, x86::reg32 end)
{
    while (b < end && (b & 3))
    {
        if (map[b])
            return b;
        ++b;
    }
    while (b + 4 <= end)
    {
        uint32_t word;
        memcpy(&word, map + b, 4);
        if (word)
            break;
        b += 4;
    }
    while (b < end && !map[b])
        ++b;
    return b;
}
static const x86::reg32 PeakLogStep = 8 * 1024 * 1024 / 4096;

static void (*s_useLog)() = nullptr;

void MemMap::setUseLog(void (*log)())
{
    s_useLog = log;
}

// Guest memory in use and its parts, the host heap and the free user memory.
static void logUse(const char* what, x86::reg32 blocks, x86::reg32 size)
{
    const struct mallinfo host = mallinfo();
    SceKernelFreeMemorySizeInfo free;
    free.size = sizeof(free);
    sceKernelGetFreeMemorySize(&free);
    pf::logInfo("guest memory %s %u MB of %u MB (block of %u bytes); guest heap %u KB used of %u KB; host heap %u KB "
                "used of %u KB; free user %u KB", what, blocks / 256, s_simulatedMemory >> 20, size,
                heap::used() / 1024, heap::held() / 1024, unsigned(host.uordblks) / 1024,
                unsigned(host.arena) / 1024, free.size_user / 1024);
    if (s_useLog)
        s_useLog();
}

void MemMap::logInUse()
{
    s_lock->lock();
    const x86::reg32 used = s_usedBlocks;
    s_lock->unlock();
    logUse("in use", used, 0);
}

MemMap::MemMap(x86::reg32 size)
    :   m_block(x86::reg32(-1))
    ,   m_blockCount(size / 4096 + (size % 4096 != 0))
{
    RC_ASSERT(m_blockCount >= 1);
    if (m_blockCount > s_blockCount)
        pf::fatal("guest memory exhausted (requested %u bytes)", size);
    s_lock->lock();
    // First fit: from the lowest free block, each candidate run ends at its first used block.
    s_firstFree = nextFree(s_blocks, s_firstFree, s_blockCount);
    for (x86::reg32 b = s_firstFree; b + m_blockCount <= s_blockCount; /* nothing */)
    {
        const x86::reg32 used = nextUsed(s_blocks, b, b + m_blockCount);
        if (used == b + m_blockCount)
        {
            m_block = b;
            break;
        }
        b = nextFree(s_blocks, used + 1, s_blockCount);
    }
    if (m_block == x86::reg32(-1))
    {
        const x86::reg32 used = s_usedBlocks;
        s_lock->unlock();
        logUse("in use", used, size);
        pf::fatal("guest memory exhausted (requested %u bytes)", size);
    }
    for (x86::reg32 s = m_block; s < m_block + m_blockCount; ++s)
    {
        s_blocks[s] = 1;
    }
    if (m_block == s_firstFree)
        s_firstFree = m_block + m_blockCount;
    s_memMaps.push_back(this);
    s_usedBlocks += m_blockCount;
    const bool newStep = s_usedBlocks / PeakLogStep > s_peakBlocks / PeakLogStep;
    if (s_usedBlocks > s_peakBlocks)
        s_peakBlocks = s_usedBlocks;
    const x86::reg32 peak = s_peakBlocks;
    s_lock->unlock();
    if (newStep)
        logUse("peak", peak, size);
}

MemMap::~MemMap()
{
    s_lock->lock();
    for (std::vector<MemMap*>::iterator it = s_memMaps.begin(); it != s_memMaps.end(); ++it)
    {
        if (*it == this)
        {
            s_memMaps.erase(it);
            break;
        }
    }
    for (x86::reg32 s = m_block; s < m_block + m_blockCount; ++s)
    {
        s_blocks[s] = 0;
    }
    if (m_block < s_firstFree)
        s_firstFree = m_block;
    s_usedBlocks -= m_blockCount;
    s_lock->unlock();
}

// Null for an address outside every block.
MemMap* MemMap::findBlock(x86::reg32 memIndex)
{
    if (memIndex < s_addressOffset || memIndex >= s_addressOffset + s_simulatedMemory)
        return nullptr;
    MemMap* result = nullptr;
    s_lock->lock();
    for (std::vector<MemMap*>::iterator it = s_memMaps.begin(); it != s_memMaps.end(); ++it)
    {
        if ((*it)->getBlockStart() <= memIndex && (*it)->getBlockStart() + (*it)->getBlockSize() > memIndex)
        {
            RC_ASSERT(!result);
            result = *it;
            break;
        }
    }
    s_lock->unlock();
    return result;
}

bool MemMap::isGuestRange(x86::reg32 address, x86::reg32 size)
{
    return x86::reg64(address) + size <= x86::reg64(s_addressOffset) + s_simulatedMemory;
}

x86::reg32 MemMap::peakBytes()
{
    s_lock->lock();
    const x86::reg32 peak = s_peakBlocks;
    s_lock->unlock();
    return peak * 4096;
}

x86::reg32 MemMap::totalBytes()
{
    return s_blockCount * 4096;
}

x86::reg32 MemMap::endAddress()
{
    return s_addressOffset + s_blockCount * 4096;
}

void MemMap::usedRanges(std::vector<std::pair<x86::reg32, x86::reg32>>& ranges)
{
    ranges.clear();
    s_lock->lock();
    for (x86::reg32 b = nextUsed(s_blocks, 0, s_blockCount); b < s_blockCount; )
    {
        const x86::reg32 end = nextFree(s_blocks, b, s_blockCount);
        ranges.push_back(std::make_pair(s_addressOffset + b * 4096, s_addressOffset + end * 4096));
        b = nextUsed(s_blocks, end, s_blockCount);
    }
    s_lock->unlock();
}

void MemMap::init(x86::reg32 baseAddress, const std::vector<Section>& sections)
{
    if (!g_guestBase)
        pf::fatal("guest memory allocation of %u bytes failed: 0x%08x", s_arenaSize, unsigned(s_arenaBlock));
    SceKernelFreeMemorySizeInfo free;
    free.size = sizeof(free);
    sceKernelGetFreeMemorySize(&free);
    pf::logInfo("guest memory: %u bytes at %p; free user %u KB, cdram %u KB, phycont %u KB", s_arenaSize,
                static_cast<void*>(g_guestBase), free.size_user / 1024, free.size_cdram / 1024,
                free.size_phycont / 1024);
    s_blockCount = s_simulatedMemory / 4096;
    s_blockSize = s_blockMapSize;
    x86::reg32 maxEnd = 0;
    for (std::vector<Section>::const_iterator it = sections.begin(); it != sections.end(); ++it)
    {
        x86::reg32 end = it->baseAddress + it->size;
        if (end > maxEnd)
            maxEnd = end;
    }
    s_sectionSize = (maxEnd + 4095) & ~4095u;
    if (s_sectionSize > s_sectionLimit)
        pf::fatal("image sections end at 0x%08x, past the guest arena limit 0x%08x", s_sectionSize, s_sectionLimit);
    s_blocks = g_guestBase + s_sectionSize;
    s_addressOffset = s_sectionSize + s_blockSize;
    for (std::vector<Section>::const_iterator it = sections.begin(); it != sections.end(); ++it)
    {
        RC_ASSERT(it->baseAddress >= baseAddress);
        RC_ASSERT(it->baseAddress + it->size <= s_sectionSize);
        if (it->data)
        {
            memcpy(g_guestBase + it->baseAddress, it->data, it->size);
        }
    }
    s_lock = new Mutex("MemMap");
}

void MemMap::fini()
{
    std::vector<MemMap*> memMapsCopy(s_memMaps);
    for (std::vector<MemMap*>::iterator it = memMapsCopy.begin(); it != memMapsCopy.end(); ++it)
    {
        delete *it;
    }
    RC_ASSERT(s_memMaps.empty());
    s_blocks = nullptr;
    s_firstFree = 0;
    s_blockCount = 0;
    s_blockSize = 0;
    s_addressOffset = 0;
    delete s_lock;
    s_lock = nullptr;
    freeArena();
}

void MemMap::fillDebugGraph(x86::reg16* graph)
{
    s_lock->lock();
    const x86::reg32 blocksPerPixel = (s_blockCount+639)/640;
    for (x86::reg32 b = 0; b < s_blockCount; b += blocksPerPixel)
    {
        x86::reg16 c = 0;
        for (x86::reg32 b1 = b; b1 < b+blocksPerPixel; ++b1)
        {
            if (s_blocks[b1])
                c++;
        }
        x86::reg16 r = (0x1f*c/blocksPerPixel) << (5+6);
        x86::reg16 g = (0x3f*(blocksPerPixel-c)/blocksPerPixel) << 5;
        graph[b/blocksPerPixel + 640*0] = r|g;
        graph[b/blocksPerPixel + 640*1] = r|g;
        graph[b/blocksPerPixel + 640*2] = r|g;
    }
    s_lock->unlock();
}

void MemMap::fillDebugGraph(x86::reg32* graph)
{
    s_lock->lock();
    const x86::reg32 blocksPerPixel = (s_blockCount+639)/640;
    for (x86::reg32 b = 0; b < s_blockCount; b += blocksPerPixel)
    {
        x86::reg16 c = 0;
        for (x86::reg32 b1 = b; b1 < b+blocksPerPixel; ++b1)
        {
            if (s_blocks[b1])
                c++;
        }
        x86::reg32 r = (0xff*c/blocksPerPixel) << 24;
        x86::reg32 g = (0xff*(blocksPerPixel-c)/blocksPerPixel) << 16;
        graph[b/blocksPerPixel + 640*0] = r|g;
        graph[b/blocksPerPixel + 640*1] = r|g;
        graph[b/blocksPerPixel + 640*2] = r|g;
    }
    s_lock->unlock();
}

}
