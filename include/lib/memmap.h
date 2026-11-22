#ifndef LIB_MEMMAP_H
#define LIB_MEMMAP_H

#include <x86.h>
#include <vector>

namespace win32
{

class Mutex;

// Host address of guest address 0: one memblock allocated by a static initialiser in memmap.cpp.
// Const, so compiled code need not reload it after guest stores. No static initialiser may touch
// guest memory.
extern x86::reg8* const g_guestBase;

struct Section
{
    const x86::reg8*    data;
    x86::reg32          baseAddress;
    x86::reg32          size;
};

class MemMap
{
public:
    MemMap(x86::reg32 size);
    ~MemMap();

    x86::reg32 getBlockStart() const  { return s_addressOffset + m_block * 4096; }
    x86::reg32 getBlockSize() const   { return m_blockCount * 4096; }

    static void init(x86::reg32 baseAddress, const std::vector<Section>& sections);
    static void fini();
    static MemMap* findBlock(x86::reg32 memIndex);
    // True when [address, address + size) lies inside the guest address space.
    static bool isGuestRange(x86::reg32 address, x86::reg32 size);
    // Highest guest memory in use so far and the guest memory size, in bytes.
    static x86::reg32 peakBytes();
    static x86::reg32 totalBytes();
    // End of the guest address space (sections, block map and simulated memory).
    static x86::reg32 endAddress();
    // The runs of blocks in use now, as [start, end) guest addresses in ascending order.
    static void usedRanges(std::vector<std::pair<x86::reg32, x86::reg32>>& ranges);
    // Called after every guest memory log line (peak steps and exhaustion), on the allocating thread.
    static void setUseLog(void (*log)());
    // Logs the guest memory in use now (and calls the use log).
    static void logInUse();

    static void fillDebugGraph(x86::reg16* graph);
    static void fillDebugGraph(x86::reg32* graph);
private:
    x86::reg32 m_block;
    x86::reg32 m_blockCount;

private:
    static x86::reg8*           s_blocks;
    static x86::reg32           s_blockCount;
    static x86::reg32           s_blockSize;
    static x86::reg32           s_sectionSize;
    static x86::reg32           s_addressOffset;
    static Mutex*               s_lock;
    static std::vector<MemMap*> s_memMaps;
};

}

#endif // MEMMAP_H
