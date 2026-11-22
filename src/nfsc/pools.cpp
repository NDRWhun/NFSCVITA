#include "pools.h"
#include "streaming.h"

#include <lib/memmap.h>
#include <platform/config.h>
#include <platform/log.h>
#include <platform/perf.h>
#include <recomp.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace nfsc { namespace pools {

namespace
{

// The 16 pool objects (0x00A84D00, 100 bytes each): +0 name, +0x2C size, +0x30 allocated, +0x34 most allocated.
const x86::reg32 PoolObjects = 0x00A84D00;
const x86::reg32 PoolStride = 100;
const int PoolCount = 16;

// The car pool's size in KB (65000). LoadGlobalChunks (0x6b6e00) reserves the Track Streaming block for the track
// sections, this size and the frame buffers at boot; it and the front end and in-game loaders (0x6b8030, 0x6bbdf0)
// size the car pool inside that block from it.
const x86::reg32 CarPoolKb = 0x00A62C48;
const int GameCarPoolKb = 65000;

// TheTrackStreamer: sections out of memory +0x28, its TSMemoryPool +0x73C (total size +0x08, free +0x14, node list
// head +0x28; nodes: next +0, size +0x0C, allocated +0x10, at most 192). The car pool's size inside it (CarLoader).
const x86::reg32 TrackStreamer = 0x00B70650;
const x86::reg32 TsOutOfMemory = 0x28;
const x86::reg32 TsMemoryPool = 0x73C;
const x86::reg32 PoolTotal = 0x08;
const x86::reg32 PoolFree = 0x14;
const x86::reg32 PoolNodes = 0x28;
const x86::reg32 NodeSize = 0x0C;
const x86::reg32 NodeAllocated = 0x10;
const int MaxNodes = 192;
const x86::reg32 CarPoolSize = 0x00B77084;

x86::reg32 read32(x86::reg32 address)
{
    x86::reg32 value;
    memcpy(&value, win32::g_guestBase + address, 4);
    return value;
}

// The largest free block of the pool; read while the game thread may change it, so the walk is bounded.
x86::reg32 largestFree(x86::reg32 pool)
{
    x86::reg32 largest = 0;
    const x86::reg32 head = pool + PoolNodes;
    x86::reg32 node = read32(head);
    for (int i = 0; i <= MaxNodes && node != head && win32::MemMap::isGuestRange(node, NodeAllocated + 1); ++i)
    {
        if (!win32::g_guestBase[node + NodeAllocated])
            largest = std::max(largest, read32(node + NodeSize));
        node = read32(node);
    }
    return largest;
}

// One line: name size/allocated/peak in KB for every pool with a size.
void logPools()
{
    char line[1024];
    int length = snprintf(line, sizeof(line), "game pools (KB size/used/peak):");
    for (int i = 0; i < PoolCount && length < int(sizeof(line)); ++i)
    {
        const x86::reg32 pool = PoolObjects + x86::reg32(i) * PoolStride;
        const x86::reg32 size = read32(pool + 0x2C);
        if (!size)
            continue;
        char name[24] = "?";
        const x86::reg32 namePtr = read32(pool);
        if (namePtr && win32::MemMap::isGuestRange(namePtr, sizeof(name)))
        {
            memcpy(name, win32::g_guestBase + namePtr, sizeof(name) - 1);
            name[sizeof(name) - 1] = 0;
            for (char* c = name; *c; ++c)
                *c = *c >= 0x20 && *c < 0x7F ? *c : '?';
        }
        length += snprintf(line + length, sizeof(line) - size_t(length), " %d %s %u/%u/%u;", i, name, size / 1024,
                           read32(pool + 0x30) / 1024, read32(pool + 0x34) / 1024);
    }
    const x86::reg32 trackPool = read32(TrackStreamer + TsMemoryPool);
    if (trackPool && length < int(sizeof(line)))
    {
        const x86::reg32 cars = read32(CarPoolSize);
        const x86::reg32 total = read32(trackPool + PoolTotal);
        const x86::reg32 used = total - read32(trackPool + PoolFree);
        const streaming::Counters c = streaming::counters();
        length += snprintf(line + length, sizeof(line) - size_t(length),
                           " track %u/%u/%u, largest free %u, cars %u, out of memory %u; unloads %u (%u KB), "
                           "jettisons %u, make space %u, activations waiting %u",
                           (total - cars) / 1024, used >= cars ? (used - cars) / 1024 : 0, c.trackPeakKb,
                           largestFree(trackPool) / 1024, cars / 1024, read32(TrackStreamer + TsOutOfMemory),
                           c.unloads, c.unloadedKb, c.jettisons, c.makeSpace, c.waiting);
    }
    pf::logInfo("%s", line);
}

// Every perf_log interval: the guest memory in use and the pools.
void report(uint32_t)
{
    win32::MemMap::logInUse();
}

}

void install()
{
    const int carPool = std::min(std::max(pf::Config::getInt("car_pool_kb", 40000), 20000), GameCarPoolKb);
    rc::st32(CarPoolKb, x86::reg32(carPool));
    pf::logInfo("pools: car pool %d KB (the game's %d KB)", carPool, GameCarPoolKb);
    win32::MemMap::setUseLog(&logPools);
    if (pf::perf::enabled())
        pf::perf::setReporter(&report);
}

}}
