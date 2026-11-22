#include "census.h"
#include "overlap.h"

#include <lib/memmap.h>
#include <native.h>
#include <platform/config.h>
#include <platform/log.h>
#include <platform/sync.h>
#include <recomp.h>
#include <nfsc_layout.h>
#include <psp2/kernel/sysmem.h>
#include <winapi/msvcr71.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

#ifdef NFSC_CENSUS_ACCESS
namespace rc
{
x86::reg8* g_censusReads = nullptr;
x86::reg8* g_censusWrites = nullptr;
uint32_t g_censusReader = 0;
uint32_t g_censusWriter = 0;
// display_census=4: per line, the host code (generated function) that first read or wrote it in an overlapped call.
uint32_t* g_censusReaderPcs = nullptr;
uint32_t* g_censusWriterPcs = nullptr;

namespace
{
bool s_censusMarking = false;
}

__attribute__((noinline)) bool censusMarking()
{
    return s_censusMarking;
}

__attribute__((noinline)) void censusMarkRead(x86::reg32 address)
{
    if (x86::reg8* const map = g_censusReads)
    {
        const uint32_t reader = g_censusReader;
        if (!reader || censusThread() == reader)
        {
            map[address >> 10] = 1;
            if (uint32_t* const pcs = g_censusReaderPcs)
            {
                if (!pcs[address >> 10])
                    pcs[address >> 10] = uint32_t(reinterpret_cast<uintptr_t>(__builtin_return_address(0)));
            }
        }
    }
}

__attribute__((noinline)) void censusMarkWrite(x86::reg32 address)
{
    if (x86::reg8* const map = g_censusWrites)
    {
        if (censusThread() == g_censusWriter)
        {
            map[address >> 10] = 1;
            if (uint32_t* const pcs = g_censusWriterPcs)
            {
                if (!pcs[address >> 10])
                    pcs[address >> 10] = uint32_t(reinterpret_cast<uintptr_t>(__builtin_return_address(0)));
            }
        }
    }
}

void setCensusMarking(bool on)
{
    s_censusMarking = on;
}
}
#endif

namespace nfsc
{

// Translated bodies of the overridden guest functions.
void sub_476b20_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_476d70_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_477230_guest(win32::WinApplication* app, x86::CPU& cpu);

namespace census
{

bool g_tracking = false;

namespace
{

const x86::reg32 LineShift = 10;
const x86::reg32 LineBytes = 1u << LineShift;

enum Phase
{
    PhaseMain,
    PhaseHandlers,
    PhaseDisplay,
    PhasePost,
    PhaseCount,
    PhaseNone = PhaseCount
};
const char* const s_phaseNames[PhaseCount] = { "main", "handlers", "display", "post" };
const char* const s_kindNames[KindCount] = { "heap", "fastmem", "slot" };

// Game flow manager state; 6: racing.
const x86::reg32 GameFlowState = 0x00A99BBC;
const x86::reg32 FlowRacing = 6;
// display_census=3: the display call's reads only, without hashing (census build).
const int ModeReads = 3;
// display_census=4: conflicts of overlapped display calls (census build, display_overlap).
const int ModeOverlap = 4;
// display_census=5: conflicts of audio updates on the audio thread (census build, audio_parallel).
const int ModeAudio = 5;
#ifdef NFSC_CENSUS_ACCESS
// Conflicting lines logged when first seen.
const uint32_t LoggedConflicts = 64;
// Overlapped calls between two report rewrites.
const uint32_t OverlapReportEvery = 60;
#endif
// A slot pool's name.
const x86::reg32 PoolName = 0x0c;
// Samples between two report rewrites during a race.
const uint32_t ReportEvery = 8;

int s_mode = 0;
size_t s_lines = 0;
uint32_t s_interval = 120;
std::string s_path;

uint32_t s_frame = 0;
uint32_t s_lastSample = 0;
uint32_t s_samples = 0;
bool s_sampling = false;
bool s_closePost = false;
bool s_wasRacing = false;
int s_handlerDepth = 0;

// Per 1 KB line of the guest address space: hash, the boundary that hashed it last, the phases that changed it in
// this sample, and the samples in which each phase changed it (saturating).
std::vector<uint32_t> s_hash;
std::vector<uint16_t> s_epoch;
std::vector<uint8_t> s_sampleMask;
std::vector<uint8_t> s_counts[PhaseCount];
uint16_t s_currentEpoch = 0;
std::vector<std::pair<x86::reg32, x86::reg32>> s_ranges;

#ifdef NFSC_CENSUS_ACCESS
// Census build: the lines the display call's game code read in this sample, and the samples that read each line
// (saturating).
std::vector<uint8_t> s_displayReads;
std::vector<uint8_t> s_readCounts;
// display_census=4, in a memory block of their own (not the host heap): lines the display thread read and the game
// thread wrote during the overlapped call, and the overlapped calls in which each line was a conflict (saturating).
uint8_t* s_overlapReads = nullptr;
uint8_t* s_mainWrites = nullptr;
uint8_t* s_conflicts = nullptr;
// display_census=4, in a second block when it fits: per line, the guest code that asked for the last malloc block
// starting there (the code address above malloc's caller, the game's operator new).
x86::reg32* s_mallocSites = nullptr;
uint32_t* s_readerPcs = nullptr;
uint32_t* s_writerPcs = nullptr;

// msvcr71 observer: a malloc block's lines get its allocating code.
void mallocAllocated(x86::reg32 block, x86::reg32 size, x86::reg32 site, x86::reg32 via)
{
    const x86::reg32 code = via ? via : site;
    const size_t last = std::min<size_t>((block + std::max<x86::reg32>(size, 1) - 1) >> LineShift, s_lines - 1);
    for (size_t line = block >> LineShift; line <= last; ++line)
        s_mallocSites[line] = code;
}
uint32_t s_displayMarker = 0;
uint32_t s_audioMarker = 0;
uint32_t s_conflictWindows = 0;
uint32_t s_logged = 0;
bool s_overlapActive = false;
#endif
uint32_t s_windows = 0;

// Generated code marks its reads while the display call records (census build).
void startPhase(Phase phase)
{
#ifdef NFSC_CENSUS_ACCESS
    rc::g_censusReads = phase == PhaseDisplay ? s_displayReads.data() : nullptr;
#else
    (void)phase;
#endif
}

// Adds this sample's display reads to the counts (census build).
void finishReads()
{
#ifdef NFSC_CENSUS_ACCESS
    for (size_t line = 0; line < s_lines; ++line)
    {
        if (s_displayReads[line] && s_readCounts[line] < 255)
            ++s_readCounts[line];
    }
    std::fill(s_displayReads.begin(), s_displayReads.end(), uint8_t(0));
#endif
}

struct Allocation
{
    x86::reg32 size;
    x86::reg32 site;
    x86::reg32 name;
};
typedef std::map<x86::reg32, Allocation> AllocationMap;
AllocationMap s_allocations[KindCount];
pf::LwMutex s_allocationLock("census");

inline uint32_t rotate(uint32_t value, int bits)
{
    return (value << bits) | (value >> (32 - bits));
}

// Four interleaved multiply-xor lanes over the line's words.
uint32_t hashLine(x86::reg32 address)
{
    const uint32_t* words = reinterpret_cast<const uint32_t*>(rc::host(address));
    uint32_t a = 0x9e3779b9u, b = 0x85ebca6bu, c = 0xc2b2ae35u, d = 0x27d4eb2fu;
    for (x86::reg32 i = 0; i < LineBytes / 4; i += 4)
    {
        a = (a ^ words[i]) * 0x01000193u;
        b = (b ^ words[i + 1]) * 0x01000193u;
        c = (c ^ words[i + 2]) * 0x01000193u;
        d = (d ^ words[i + 3]) * 0x01000193u;
    }
    return a ^ rotate(b, 8) ^ rotate(c, 16) ^ rotate(d, 24);
}

// The image's data section and the guest blocks in use.
void collectRanges()
{
    win32::MemMap::usedRanges(s_ranges);
    for (const nfsc::layout::ImageSection& section : nfsc::layout::sections)
    {
        if (strcmp(section.name, ".data") == 0)
            s_ranges.push_back(std::make_pair(section.address & ~(LineBytes - 1),
                                              (section.address + section.virtualSize + LineBytes - 1) &
                                              ~(LineBytes - 1)));
    }
}

// Hashes every line in use; a line hashed at the previous boundary whose hash changed was written by the phase that
// ends here. Then the next phase starts.
void boundary(Phase ending, Phase next)
{
    startPhase(PhaseNone);
    if (++s_currentEpoch == 0)
    {
        std::fill(s_epoch.begin(), s_epoch.end(), uint16_t(0));
        s_currentEpoch = 1;
    }
    const uint16_t previous = uint16_t(s_currentEpoch - 1);
    const uint8_t bit = ending == PhaseNone ? 0 : uint8_t(1u << ending);
    collectRanges();
    for (const std::pair<x86::reg32, x86::reg32>& range : s_ranges)
    {
        const x86::reg32 end = std::min<x86::reg32>(range.second >> LineShift, x86::reg32(s_hash.size()));
        for (x86::reg32 line = range.first >> LineShift; line < end; ++line)
        {
            const uint32_t hash = hashLine(line << LineShift);
            if (bit && s_epoch[line] == previous && s_hash[line] != hash)
                s_sampleMask[line] |= bit;
            s_hash[line] = hash;
            s_epoch[line] = s_currentEpoch;
        }
    }
    startPhase(next);
}

void finishSample()
{
    for (size_t line = 0; line < s_sampleMask.size(); ++line)
    {
        const uint8_t mask = s_sampleMask[line];
        if (!mask)
            continue;
        for (int phase = 0; phase < PhaseCount; ++phase)
        {
            if ((mask & (1u << phase)) && s_counts[phase][line] < 255)
                ++s_counts[phase][line];
        }
    }
    finishReads();
    ++s_samples;
}

// Phases whose hashes saw a line change in any sample (bits 0-3), and in the census build bit 4 when the display
// call's code read it.
uint8_t lineMask(size_t line)
{
    uint8_t mask = 0;
    for (int phase = 0; phase < PhaseCount && s_mode != ModeReads; ++phase)
    {
        if (s_counts[phase][line])
            mask |= uint8_t(1u << phase);
    }
#ifdef NFSC_CENSUS_ACCESS
    if (s_readCounts[line])
        mask |= 0x10;
#endif
    return mask;
}

// Samples in which a phase changed a line, and in which the display call read it (census build).
uint8_t lineCount(size_t line, int phase)
{
    return s_mode != ModeReads ? s_counts[phase][line] : 0;
}

uint8_t lineReads(size_t line)
{
#ifdef NFSC_CENSUS_ACCESS
    return s_readCounts[line];
#else
    (void)line;
    return 0;
#endif
}

// The allocation of a kind that holds an address, or null.
const AllocationMap::value_type* holder(Kind kind, x86::reg32 address)
{
    const AllocationMap& map = s_allocations[kind];
    AllocationMap::const_iterator it = map.upper_bound(address);
    if (it == map.begin())
        return nullptr;
    --it;
    return address - it->first < std::max<x86::reg32>(it->second.size, 1) ? &*it : nullptr;
}

// A printable guest string of at most 40 characters, or "".
std::string guestString(x86::reg32 address)
{
    std::string text;
    if (!address || !win32::MemMap::isGuestRange(address, 1))
        return text;
    for (x86::reg32 i = 0; i < 40 && win32::MemMap::isGuestRange(address + i, 1); ++i)
    {
        const char c = char(rc::ld8(address + i));
        if (!c)
            break;
        text += c >= 0x20 && c < 0x7f ? c : '?';
    }
    return text;
}

// Where a line lies: the game heap block, guest block or image section, as a grouping key and a description.
x86::reg32 container(x86::reg32 address, char* text, size_t size)
{
    if (const AllocationMap::value_type* heap = holder(HeapBlock, address))
    {
        const std::string name = guestString(heap->second.name);
        snprintf(text, size, "heap 0x%08x+0x%x site 0x%08x%s%s%s", heap->first, heap->second.size, heap->second.site,
                 name.empty() ? "" : " \"", name.c_str(), name.empty() ? "" : "\"");
        return heap->first;
    }
#ifdef NFSC_CENSUS_ACCESS
    if (s_mallocSites && s_mallocSites[address >> LineShift])
    {
        const x86::reg32 site = s_mallocSites[address >> LineShift];
        snprintf(text, size, "malloc block from 0x%08x", site);
        return site;
    }
#endif
    for (const nfsc::layout::ImageSection& section : nfsc::layout::sections)
    {
        if (address - section.address < section.virtualSize)
        {
            snprintf(text, size, "image %s", section.name);
            return section.address;
        }
    }
    if (const win32::MemMap* block = win32::MemMap::findBlock(address))
    {
        snprintf(text, size, "guest block 0x%08x+0x%x", block->getBlockStart(), block->getBlockSize());
        return block->getBlockStart();
    }
    snprintf(text, size, "unallocated");
    return 0;
}

// The allocation sites of FastMem blocks and pool slots in [start, end), most frequent first.
void smallSites(FILE* file, x86::reg32 start, x86::reg32 end)
{
    for (int kind = FastMemBlock; kind <= PoolSlot; ++kind)
    {
        std::map<std::pair<x86::reg32, x86::reg32>, uint32_t> sites;
        const AllocationMap& map = s_allocations[kind];
        for (AllocationMap::const_iterator it = map.lower_bound(start); it != map.end() && it->first < end; ++it)
            ++sites[std::make_pair(it->second.site, kind == PoolSlot ? it->second.name : 0)];
        if (sites.empty())
            continue;
        std::vector<std::pair<uint32_t, std::pair<x86::reg32, x86::reg32>>> sorted;
        for (const auto& site : sites)
            sorted.push_back(std::make_pair(site.second, site.first));
        std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        fprintf(file, " | %s", s_kindNames[kind]);
        for (size_t i = 0; i < sorted.size() && i < 4; ++i)
        {
            const x86::reg32 pool = sorted[i].second.second;
            const std::string name = pool ? guestString(rc::ld32(pool + PoolName)) : std::string();
            fprintf(file, " 0x%08x x%u", sorted[i].second.first, sorted[i].first);
            if (pool)
                fprintf(file, " (pool 0x%08x \"%s\")", pool, name.c_str());
        }
        if (sorted.size() > 4)
            fprintf(file, " +%u sites", unsigned(sorted.size() - 4));
    }
}

void writeReportFile();

// The report's own guest reads are not marked.
void writeReport()
{
#ifdef NFSC_CENSUS_ACCESS
    x86::reg8* const reads = rc::g_censusReads;
    startPhase(PhaseNone);
    writeReportFile();
    rc::g_censusReads = reads;
#else
    writeReportFile();
#endif
}

void writeReportFile()
{
    FILE* file = fopen(s_path.c_str(), "w");
    if (!file)
    {
        pf::logError("census: cannot write %s", s_path.c_str());
        return;
    }
    pf::LwLockGuard guard(s_allocationLock);
    // Per phase: lines written, of them also written by display, and read by the display call's code.
    uint64_t bytes[PhaseCount] = {};
    uint64_t withDisplay[PhaseCount] = {};
    uint64_t readByDisplay[PhaseCount] = {};
#ifdef NFSC_CENSUS_ACCESS
    uint64_t read = 0;
#endif
    for (size_t line = 0; line < s_lines; ++line)
    {
        const uint8_t mask = lineMask(line);
#ifdef NFSC_CENSUS_ACCESS
        read += (mask & 0x10) ? LineBytes : 0;
#endif
        for (int phase = 0; phase < PhaseCount; ++phase)
        {
            if (mask & (1u << phase))
            {
                bytes[phase] += LineBytes;
                if (phase != PhaseDisplay && (mask & (1u << PhaseDisplay)))
                    withDisplay[phase] += LineBytes;
                if (phase != PhaseDisplay && (mask & 0x10))
                    readByDisplay[phase] += LineBytes;
            }
        }
    }
    fprintf(file, "# display census: %u sampled race frames, one every %u frames, mode %d; 1 KB lines\n",
            s_samples, s_interval, s_mode);
    if (s_mode != ModeReads)
    {
        fprintf(file, "# phases: main = game thread from the frame start to the display call%s; display = the "
                "display call; post = game thread after the display call\n",
                s_mode == 2 ? " except the sim's update events" : "");
    }
    if (s_mode == 2)
        fprintf(file, "# handlers = the sim's world, render and camera update events\n");
#ifdef NFSC_CENSUS_ACCESS
    const bool access = true;
    fprintf(file, "# census build: reads = samples in which the display call's game code (generated code and native "
            "ports) read a line; host code (shims, renderer) is not seen; %llu KB read\n",
            (unsigned long long)(read / 1024));
#else
    const bool access = false;
    fprintf(file, "# writes only (the display call's reads are seen in the census build, display_census=3)\n");
#endif
    if (s_mode != ModeReads)
    {
        fprintf(file, "# hashes see the writes of threads running at the same time (audio mixer, file reads) in the "
                "phase they fall in\n");
        for (int phase = 0; phase < PhaseCount; ++phase)
        {
            fprintf(file, "# %s: %llu KB written, %llu KB of it also written by display", s_phaseNames[phase],
                    (unsigned long long)(bytes[phase] / 1024),
                    (unsigned long long)(phase == PhaseDisplay ? 0 : withDisplay[phase] / 1024));
            if (access && phase != PhaseDisplay)
                fprintf(file, ", %llu KB read by display", (unsigned long long)(readByDisplay[phase] / 1024));
            fputc('\n', file);
        }
    }
    fprintf(file, "# tracked allocations now: heap %u, fastmem %u, slot %u\n",
            unsigned(s_allocations[HeapBlock].size()), unsigned(s_allocations[FastMemBlock].size()),
            unsigned(s_allocations[PoolSlot].size()));
    fprintf(file, "# start end bytes | samples whose hashes saw main handlers display post write |%s where | small "
            "allocations\n", access ? " samples the display call read |" : "");
    char where[160];
    size_t line = 0;
    while (line < s_lines)
    {
        const uint8_t mask = lineMask(line);
        if (!mask)
        {
            ++line;
            continue;
        }
        const x86::reg32 key = container(x86::reg32(line << LineShift), where, sizeof(where));
        uint8_t maxCount[PhaseCount] = {};
        uint8_t maxReads = 0;
        size_t end = line;
        char next[160];
        while (end < s_lines && lineMask(end) == mask &&
               (end == line || container(x86::reg32(end << LineShift), next, sizeof(next)) == key))
        {
            for (int phase = 0; phase < PhaseCount; ++phase)
                maxCount[phase] = std::max(maxCount[phase], lineCount(end, phase));
            maxReads = std::max(maxReads, lineReads(end));
            ++end;
        }
        const x86::reg32 start = x86::reg32(line << LineShift);
        const x86::reg32 stop = x86::reg32(end << LineShift);
        fprintf(file, "0x%08x 0x%08x %u | %u %u %u %u | ", start, stop - 1, stop - start, maxCount[PhaseMain],
                maxCount[PhaseHandlers], maxCount[PhaseDisplay], maxCount[PhasePost]);
        if (access)
            fprintf(file, "%u | ", maxReads);
        fputs(where, file);
        smallSites(file, start, stop);
        fputc('\n', file);
        line = end;
    }
    fclose(file);
    pf::logInfo("census: %u samples written to %s", s_samples, s_path.c_str());
}

// display_census=4 and 5: the conflicting lines, grouped by container, with the overlapped calls that saw them.
void writeConflicts()
{
#ifdef NFSC_CENSUS_ACCESS
    FILE* file = fopen(s_path.c_str(), "w");
    if (!file)
    {
        pf::logError("census: cannot write %s", s_path.c_str());
        return;
    }
    pf::LwLockGuard guard(s_allocationLock);
    if (s_mode == ModeAudio)
        fprintf(file, "# audio update conflicts: %u updates on the audio thread, %u with conflicts; 1 KB lines the "
                "audio thread's game code read and the game thread's game code wrote during the same update (even "
                "updates), or the other way round (odd updates; writer: audio thread, reader: game thread) (host code "
                "is not seen)\n", s_windows, s_conflictWindows);
    else
        fprintf(file, "# display overlap conflicts: %u overlapped calls, %u with conflicts; 1 KB lines the display "
                "thread's game code read and the game thread's game code wrote during the same call (host code is not "
                "seen)\n", s_windows, s_conflictWindows);
    fprintf(file, "# host code: rc::censusMarking at 0x%08x (writer and reader: first host code to write or read the "
            "region's first line in an overlapped call)\n",
            unsigned(reinterpret_cast<uintptr_t>(&rc::censusMarking)));
    fprintf(file, "# start end bytes | calls with a conflict | where | small allocations | writer reader\n");
    char where[160];
    char next[160];
    size_t line = 0;
    while (line < s_lines)
    {
        if (!s_conflicts[line])
        {
            ++line;
            continue;
        }
        const x86::reg32 key = container(x86::reg32(line << LineShift), where, sizeof(where));
        uint8_t most = 0;
        size_t end = line;
        while (end < s_lines && s_conflicts[end] &&
               (end == line || container(x86::reg32(end << LineShift), next, sizeof(next)) == key))
        {
            most = std::max(most, s_conflicts[end]);
            ++end;
        }
        const x86::reg32 start = x86::reg32(line << LineShift);
        const x86::reg32 stop = x86::reg32(end << LineShift);
        fprintf(file, "0x%08x 0x%08x %u | %u | %s", start, stop - 1, stop - start, unsigned(most), where);
        smallSites(file, start, stop);
        if (s_writerPcs)
            fprintf(file, " | writer 0x%08x reader 0x%08x", unsigned(s_writerPcs[line]), unsigned(s_readerPcs[line]));
        fputc('\n', file);
        line = end;
    }
    fclose(file);
    pf::logInfo("census: overlap conflicts of %u calls written to %s", s_windows, s_path.c_str());
#endif
}

}

void configure(const char* path)
{
    s_mode = std::max(0, std::min(ModeAudio, pf::Config::getInt("display_census", 0)));
#ifndef NFSC_CENSUS_ACCESS
    if (s_mode >= ModeReads)
    {
        pf::logError("census: display_census=%d needs the census build (tools/build.ps1 -Census)", s_mode);
        s_mode = 0;
    }
#endif
    if (s_mode == ModeOverlap && pf::Config::getInt("display_overlap", 4) == 0)
    {
        pf::logError("census: display_census=4 needs display_overlap=1-4");
        s_mode = 0;
    }
    if (s_mode == ModeAudio && pf::Config::getInt("audio_parallel", 0) == 0)
    {
        pf::logError("census: display_census=5 needs audio_parallel=1 or 2");
        s_mode = 0;
    }
    if (!s_mode)
        return;
    s_interval = x86::reg32(std::max(1, pf::Config::getInt("display_census_interval", 120)));
    s_path = path;
    s_lines = win32::MemMap::endAddress() >> LineShift;
#ifdef NFSC_CENSUS_ACCESS
    if (s_mode == ModeOverlap || s_mode == ModeAudio)
    {
        const SceSize bytes = SceSize((3 * s_lines + 0xfffff) & ~size_t(0xfffff));
        const SceUID block = sceKernelAllocMemBlock("census_overlap", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, bytes, nullptr);
        void* base = nullptr;
        if (block < 0 || sceKernelGetMemBlockBase(block, &base) < 0)
        {
            pf::logError("census: no memory for the overlap tables (0x%08x)", unsigned(block));
            s_mode = 0;
            return;
        }
        memset(base, 0, 3 * s_lines);
        s_overlapReads = static_cast<uint8_t*>(base);
        s_mainWrites = s_overlapReads + s_lines;
        s_conflicts = s_mainWrites + s_lines;
        // Accessing code and malloc sites, when the memory is there.
        const SceSize namesBytes = SceSize((12 * s_lines + 0xfffff) & ~size_t(0xfffff));
        const SceUID names = sceKernelAllocMemBlock("census_names", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, namesBytes,
                                                    nullptr);
        void* namesBase = nullptr;
        if (names >= 0 && sceKernelGetMemBlockBase(names, &namesBase) >= 0)
        {
            memset(namesBase, 0, 12 * s_lines);
            s_readerPcs = static_cast<uint32_t*>(namesBase);
            s_writerPcs = s_readerPcs + s_lines;
            s_mallocSites = s_writerPcs + s_lines;
            win32::msvcr71::setHeapObserver(&mallocAllocated, nullptr);
        }
        else
        {
            pf::logError("census: no memory for the conflicts' code and malloc sites (0x%08x)", unsigned(names));
        }
        rc::setCensusMarking(true);
        g_tracking = true;
        pf::logInfo("census: %s conflicts (mode %d), %u lines of %u bytes, report %s",
                    s_mode == ModeAudio ? "audio update" : "overlap", s_mode, unsigned(s_lines), LineBytes, path);
        return;
    }
#endif
    if (s_mode != ModeReads)
    {
        s_hash.assign(s_lines, 0);
        s_epoch.assign(s_lines, 0);
        s_sampleMask.assign(s_lines, 0);
        for (std::vector<uint8_t>& counts : s_counts)
            counts.assign(s_lines, 0);
    }
#ifdef NFSC_CENSUS_ACCESS
    s_displayReads.assign(s_lines, 0);
    s_readCounts.assign(s_lines, 0);
    rc::setCensusMarking(true);
#endif
    g_tracking = true;
    pf::logInfo("census: on (mode %d), a race frame every %u frames, %u lines of %u bytes, report %s", s_mode,
                s_interval, unsigned(s_lines), LineBytes, path);
}

void allocated(Kind kind, x86::reg32 address, x86::reg32 size, x86::reg32 site, x86::reg32 name)
{
    pf::LwLockGuard guard(s_allocationLock);
    s_allocations[kind][address] = Allocation{ size, site, name };
}

void released(Kind kind, x86::reg32 address)
{
    pf::LwLockGuard guard(s_allocationLock);
    s_allocations[kind].erase(address);
}

void frameStart()
{
    if (!s_mode)
        return;
    ++s_frame;
    const bool racing = rc::ld32(GameFlowState) == FlowRacing;
    if (s_mode == ModeOverlap || s_mode == ModeAudio)
    {
        if (s_wasRacing && !racing && s_windows)
            writeConflicts();
        s_wasRacing = racing;
        return;
    }
    const bool sample = racing && s_frame - s_lastSample >= s_interval;
    if (s_mode == ModeReads)
    {
        if (s_wasRacing && !racing && s_samples)
            writeReport();
        s_wasRacing = racing;
        s_sampling = sample;
        if (sample)
            s_lastSample = s_frame;
        return;
    }
    if (s_closePost)
    {
        boundary(PhasePost, sample ? PhaseMain : PhaseNone);
        finishSample();
        s_closePost = false;
        if (s_samples % ReportEvery == 0)
            writeReport();
    }
    else if (sample)
    {
        boundary(PhaseNone, PhaseMain);
    }
    if (s_wasRacing && !racing && s_samples)
        writeReport();
    s_wasRacing = racing;
    s_sampling = sample;
    if (sample)
    {
        s_lastSample = s_frame;
        std::fill(s_sampleMask.begin(), s_sampleMask.end(), uint8_t(0));
    }
}

void displayStart()
{
    if (!s_sampling)
        return;
    if (s_mode == ModeReads)
        startPhase(PhaseDisplay);
    else
        boundary(PhaseMain, PhaseDisplay);
}

void displayEnd()
{
    if (!s_sampling)
        return;
    s_sampling = false;
    if (s_mode == ModeReads)
    {
        startPhase(PhaseNone);
        finishReads();
        if (++s_samples % (ReportEvery * 8) == 0)
            writeReport();
        return;
    }
    boundary(PhaseDisplay, PhasePost);
    s_closePost = true;
}

void handlerStart()
{
    if (s_sampling && s_mode == 2 && s_handlerDepth++ == 0)
        boundary(PhaseMain, PhaseHandlers);
}

void handlerEnd()
{
    if (s_sampling && s_mode == 2 && --s_handlerDepth == 0)
        boundary(PhaseHandlers, PhaseMain);
}

bool recordsFrames()
{
    return s_mode >= 1 && s_mode <= ModeReads;
}

namespace
{

#ifdef NFSC_CENSUS_ACCESS
// Marks the reads of one thread and the writes of another until endWindow.
void startWindow(uint32_t reader, uint32_t writer)
{
    rc::g_censusReader = reader;
    rc::g_censusWriter = writer;
    rc::g_censusReaderPcs = s_readerPcs;
    rc::g_censusWriterPcs = s_writerPcs;
    rc::g_censusWrites = s_mainWrites;
    rc::g_censusReads = s_overlapReads;
    s_overlapActive = true;
}

// The lines read and written in the window are its conflicts.
void endWindow()
{
    if (!s_overlapActive)
        return;
    s_overlapActive = false;
    rc::g_censusReads = nullptr;
    rc::g_censusWrites = nullptr;
    rc::g_censusReaderPcs = nullptr;
    rc::g_censusWriterPcs = nullptr;
    ++s_windows;
    bool any = false;
    char where[160];
    for (size_t line = 0; line < s_lines; ++line)
    {
        if (!s_overlapReads[line] || !s_mainWrites[line])
            continue;
        any = true;
        if (s_conflicts[line] == 0 && s_logged < LoggedConflicts)
        {
            ++s_logged;
            {
                pf::LwLockGuard guard(s_allocationLock);
                container(x86::reg32(line << LineShift), where, sizeof(where));
            }
            pf::logInfo("census: %s conflict in %s %u: line 0x%08x, %s", s_mode == ModeAudio ? "audio" : "overlap",
                        s_mode == ModeAudio ? "update" : "call", s_windows, x86::reg32(line << LineShift), where);
        }
        if (s_conflicts[line] < 0xff)
            ++s_conflicts[line];
    }
    if (any)
        ++s_conflictWindows;
    memset(s_overlapReads, 0, s_lines);
    memset(s_mainWrites, 0, s_lines);
    if (s_windows % OverlapReportEvery == 0)
        writeConflicts();
}
#endif

}

void displayThreadStarted()
{
#ifdef NFSC_CENSUS_ACCESS
    s_displayMarker = rc::censusThread();
#endif
}

void overlapStart()
{
#ifdef NFSC_CENSUS_ACCESS
    if (s_mode != ModeOverlap)
        return;
    const uint32_t mainMarker = rc::censusThread();
    if (!s_displayMarker || mainMarker == s_displayMarker)
    {
        pf::logError("census: the game and display threads share a thread marker (0x%08x); no conflict marks",
                     mainMarker);
        s_mode = 0;
        return;
    }
    startWindow(s_displayMarker, mainMarker);
#endif
}

void overlapEnd()
{
#ifdef NFSC_CENSUS_ACCESS
    if (s_mode == ModeOverlap)
        endWindow();
#endif
}

void audioThreadStarted()
{
#ifdef NFSC_CENSUS_ACCESS
    s_audioMarker = rc::censusThread();
#endif
}

void audioStart()
{
#ifdef NFSC_CENSUS_ACCESS
    if (s_mode != ModeAudio)
        return;
    const uint32_t mainMarker = rc::censusThread();
    if (!s_audioMarker || mainMarker == s_audioMarker)
    {
        pf::logError("census: the game and audio threads share a thread marker (0x%08x); no conflict marks",
                     mainMarker);
        s_mode = 0;
        return;
    }
    if (s_windows % 2 == 0)
        startWindow(s_audioMarker, mainMarker);
    else
        startWindow(mainMarker, s_audioMarker);
#endif
}

void audioEnd()
{
#ifdef NFSC_CENSUS_ACCESS
    if (s_mode == ModeAudio)
        endWindow();
#endif
}

}

namespace native
{

// bWareMalloc(size, name, ...): the block is tracked with the guest code that asked for it.
void sub_476b20(win32::WinApplication* app, x86::CPU& cpu)
{
    if (!census::g_tracking)
    {
        sub_476b20_guest(app, cpu);
        return;
    }
    const x86::reg32 size = rc::ld32(cpu.esp + 4);
    const x86::reg32 name = rc::ld32(cpu.esp + 8);
    const x86::reg32 site = rc::ld32(cpu.esp);
    sub_476b20_guest(app, cpu);
    if (cpu.eax)
        census::allocated(census::HeapBlock, cpu.eax, size, site, name);
}

// bMalloc(size, flags): its block is tracked with bMalloc's caller instead.
void sub_477230(win32::WinApplication* app, x86::CPU& cpu)
{
    if (!census::g_tracking)
    {
        sub_477230_guest(app, cpu);
        return;
    }
    const x86::reg32 size = rc::ld32(cpu.esp + 4);
    const x86::reg32 site = rc::ld32(cpu.esp);
    sub_477230_guest(app, cpu);
    if (cpu.eax)
        census::allocated(census::HeapBlock, cpu.eax, size, site, 0);
}

// bFree(block): waits for the barrier while a display call runs beside the game thread.
void sub_476d70(win32::WinApplication* app, x86::CPU& cpu)
{
    if (census::g_tracking)
        census::released(census::HeapBlock, rc::ld32(cpu.esp + 4));
    if (overlap::deferHeapFree(rc::ld32(cpu.esp + 4)))
    {
        cpu.esp += 4;
        return;
    }
    sub_476d70_guest(app, cpu);
}

}
}
