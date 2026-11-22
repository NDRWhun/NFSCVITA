// VP6 movie frame decode (guest 0x87f970): the native decoder of vp6.cpp, its frame body run outside the execution
// lock on a decode thread (vp6_core) with reconstruction threads on the other cores, or the translated guest decoder
// (vp6_native=0). shadow_vp6=1 runs both on the same state and compares what they write.
// The colour conversion 0x57dff0 hands whole frames to the movie texture as planes (vp6_gpu=0: native YUV to RGB).

#include "vp6.h"

#include <lib/guest.h>
#include <lib/winapp.h>
#include <native.h>
#include <platform/config.h>
#include <platform/cores.h>
#include <platform/log.h>
#include <platform/perf.h>
#include <platform/sync.h>
#include <platform/time.h>
#include <recomp.h>
#include <winapi/d3d9.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <vector>

namespace nfsc
{

// Translated bodies of the overridden guest functions.
void sub_87f970_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_57dff0_guest(win32::WinApplication* app, x86::CPU& cpu);

namespace
{

constexpr x86::reg32 ResizeBuffers = 0x881996;   // frees and allocates the frame buffers for a new size

bool s_loaded = false;
bool s_native = true;
bool s_shadow = false;
bool s_worker = true;
bool s_gpu = true;
// Core of the decode thread, -1 to decode the frame body on the game's own decode thread.
int s_core = 1;

// The decode thread runs above the guest threads and below the render, audio and timer threads; reconstruction
// threads run just above guest threads of normal priority, so a row they claimed does not wait behind them.
constexpr int DecodePriority = pf::Thread::PriorityHigh + 16;
constexpr int WorkerPriority = pf::Thread::PriorityNormal - 4;
constexpr int MaxWorkers = 3;

// Frame body times since the last log line, in microseconds.
struct BodyTimes
{
    uint64_t setup;         // beginBody
    uint64_t decode;        // decodeRow, all rows
    uint64_t wait;          // the decoding thread waiting for reconstructions or reconstructing rows itself
    uint64_t end;           // endBody
    uint64_t reconstruct;   // reconstructRow on all threads
    uint32_t rows;          // rows of all frames
    uint32_t ownRows;       // rows the decoding thread reconstructed
};

BodyTimes s_times = {};

// Macroblock rows of a frame: the decoding thread decodes them in order and reconstruction threads rebuild the rows
// behind it, in any order; the decoding thread reconstructs rows itself whenever they lag. Counters carry the frame
// number in their upper 16 bits, so a claim from a finished frame cannot take a row of the next one.
class RowPipeline
{
public:
    // Starts one reconstruction thread per core mask.
    void start(const int* masks, int count)
    {
        m_wake = new pf::Semaphore("vp6 rows", 0);
        for (int i = 0; i < count && i < MaxWorkers; ++i)
        {
            m_threads[i] = new pf::Thread("vp6 reconstruction", &entry, this, WorkerPriority, 64 * 1024, masks[i]);
            m_threads[i]->start();
        }
    }

    bool started() const
    {
        return m_wake != nullptr;
    }

    // Decodes and reconstructs all rows of the frame vp6::beginBody started.
    void run(uint32_t rows, BodyTimes& times)
    {
        const uint32_t tag = (++m_frame & 0xffff) << 16;
        for (std::atomic<uint32_t>& d : m_done)
            d.store(0);
        m_reconstructUs.store(0);
        m_decoded.store(tag);
        m_claimed.store(tag);
        uint32_t decodeUs = 0, waitUs = 0, own = 0;
        for (uint32_t row = 0; row < rows; ++row)
        {
            uint32_t start = pf::perf::now();
            if (row >= vp6::RingRows)
            {
                waitDone(tag, row - vp6::RingRows, own);
                const uint32_t now = pf::perf::now();
                waitUs += now - start;
                start = now;
            }
            vp6::decodeRow(row);
            decodeUs += pf::perf::now() - start;
            m_decoded.store(tag | (row + 1));
            m_wake->post();
        }
        const uint32_t start = pf::perf::now();
        while (reconstructNext(own))
        {
        }
        for (uint32_t row = rows > vp6::RingRows ? rows - vp6::RingRows : 0; row < rows; ++row)
            waitDone(tag, row, own);
        times.wait += waitUs + (pf::perf::now() - start);
        times.decode += decodeUs;
        times.reconstruct += m_reconstructUs.load();
        times.ownRows += own;
    }

private:
    static int entry(void* self)
    {
        RowPipeline* p = static_cast<RowPipeline*>(self);
        uint32_t rows = 0;
        for (;;)
        {
            p->m_wake->wait();
            while (p->reconstructNext(rows))
            {
            }
        }
        return 0;
    }

    // Claims and reconstructs the next decoded row, counted in rows; false when there is none.
    bool reconstructNext(uint32_t& rows)
    {
        for (;;)
        {
            uint32_t claimed = m_claimed.load();
            const uint32_t decoded = m_decoded.load();
            if (((claimed ^ decoded) >> 16) != 0 || (claimed & 0xffff) >= (decoded & 0xffff))
                return false;
            if (m_claimed.compare_exchange_weak(claimed, claimed + 1))
            {
                const uint32_t row = claimed & 0xffff;
                const uint32_t start = pf::perf::now();
                vp6::reconstructRow(row);
                m_reconstructUs.fetch_add(pf::perf::now() - start);
                m_done[row % vp6::RingRows].store((claimed & 0xffff0000u) | (row + 1));
                ++rows;
                return true;
            }
        }
    }

    // Returns once row is reconstructed, reconstructing other rows meanwhile (counted in rows).
    void waitDone(uint32_t tag, uint32_t row, uint32_t& rows)
    {
        const uint32_t want = tag | (row + 1);
        while (m_done[row % vp6::RingRows].load() != want)
        {
            if (!reconstructNext(rows))
                pf::sleepUs(50);
        }
    }

    pf::Thread* m_threads[MaxWorkers] = {};
    pf::Semaphore* m_wake = nullptr;
    uint32_t m_frame = 0;
    std::atomic<uint32_t> m_decoded{ 0 };
    std::atomic<uint32_t> m_claimed{ 0 };
    std::atomic<uint32_t> m_reconstructUs{ 0 };
    std::atomic<uint32_t> m_done[vp6::RingRows] = {};
};

RowPipeline s_pipeline;
// The native frame state is one per process: decodes of two movies at once take turns.
pf::LwMutex* s_bodyMutex = nullptr;

// decodeBody, one decode at a time, with the reconstruction threads when the rows are independent.
void decodeFrameBody(x86::reg32 instance, x86::reg32 data, x86::reg32 argumentSlot)
{
    pf::LwLockGuard guard(*s_bodyMutex);
    uint32_t start = pf::perf::now();
    bool independent;
    const uint32_t rows = vp6::beginBody(instance, data, argumentSlot, independent);
    uint32_t now = pf::perf::now();
    s_times.setup += now - start;
    s_times.rows += rows;
    if (s_pipeline.started() && independent)
        s_pipeline.run(rows, s_times);
    else
    {
        for (uint32_t row = 0; row < rows; ++row)
        {
            start = pf::perf::now();
            vp6::decodeRow(row);
            now = pf::perf::now();
            vp6::reconstructRow(row);
            s_times.decode += now - start;
            s_times.reconstruct += pf::perf::now() - now;
        }
        s_times.ownRows += rows;
    }
    start = pf::perf::now();
    vp6::endBody();
    s_times.end += pf::perf::now() - start;
}

// A thread that runs decodeFrameBody for the game's decode thread, which waits meanwhile.
class BodyThread
{
public:
    void start(int mask)
    {
        m_go = new pf::Semaphore("vp6 body", 0);
        m_finished = new pf::Semaphore("vp6 body done", 0);
        m_thread = new pf::Thread("vp6 decode", &entry, this, DecodePriority, 64 * 1024, mask);
        m_thread->start();
    }

    void run(x86::reg32 instance, x86::reg32 data, x86::reg32 argumentSlot)
    {
        m_instance = instance;
        m_data = data;
        m_argumentSlot = argumentSlot;
        m_go->post();
        m_finished->wait();
    }

private:
    static int entry(void* self)
    {
        BodyThread* p = static_cast<BodyThread*>(self);
        for (;;)
        {
            p->m_go->wait();
            decodeFrameBody(p->m_instance, p->m_data, p->m_argumentSlot);
            p->m_finished->post();
        }
        return 0;
    }

    pf::Thread* m_thread = nullptr;
    pf::Semaphore* m_go = nullptr;
    pf::Semaphore* m_finished = nullptr;
    x86::reg32 m_instance = 0, m_data = 0, m_argumentSlot = 0;
};

BodyThread s_bodyThread;
// Serialises the callers of s_bodyThread.
pf::LwMutex* s_bodyThreadMutex = nullptr;

// The frame body on the decode thread, or on the calling thread without one.
void runFrameBody(x86::reg32 instance, x86::reg32 data, x86::reg32 argumentSlot)
{
    if (s_core < 0)
    {
        decodeFrameBody(instance, data, argumentSlot);
        return;
    }
    pf::LwLockGuard guard(*s_bodyThreadMutex);
    s_bodyThread.run(instance, data, argumentSlot);
}

// Decode time statistics, logged every StatFrames frames.
constexpr int StatFrames = 300;
int s_statCount = 0;
uint64_t s_statSum = 0;
uint32_t s_statMax = 0;
uint64_t s_headerSum = 0;
// Conversion statistics, logged with them: time of each 0x57dff0 call and the frames handed to the GPU as planes.
int s_convertCount = 0;
int s_convertGpu = 0;
uint64_t s_convertSum = 0;
uint32_t s_convertMax = 0;

// Shadow comparison state (decodes, then conversions).
int s_compared = 0;
int s_differ = 0;
int s_skipped = 0;
int s_reported = 0;
int s_convertCompared = 0;
int s_convertDiffer = 0;

void loadSettings()
{
    s_loaded = true;
    s_native = pf::Config::getBool("vp6_native", true);
    s_shadow = pf::Config::getBool("shadow_vp6", false);
    s_worker = pf::Config::getBool("vp6_worker", true);
    s_gpu = pf::Config::getBool("vp6_gpu", true);
    s_core = pf::Config::getInt("vp6_core", 1);
    const int cores = pf::cores::hasCore3() ? 4 : 3;
    if (s_core >= cores)
        pf::logInfo("vp6: vp6_core=%d is not an application core; the game's decode thread decodes", s_core);
    if (s_core < 0 || s_core >= cores)
        s_core = -1;
    s_bodyMutex = new pf::LwMutex("vp6 frame");
    vp6::setMemoryBase(uintptr_t(win32::g_guestBase));
    char where[64] = "";
    if (s_native || s_shadow)
    {
        if (s_core >= 0)
        {
            s_bodyThreadMutex = new pf::LwMutex("vp6 body");
            s_bodyThread.start(pf::Thread::Cpu0 << s_core);
        }
        // One reconstruction thread on each other core.
        const int decodeCore = s_core >= 0 ? s_core : pf::cores::core(pf::cores::Helper);
        int masks[MaxWorkers];
        int workers = 0;
        for (int core = 0; core < cores && s_worker; ++core)
        {
            if (core != decodeCore && workers < MaxWorkers)
            {
                masks[workers++] = pf::Thread::Cpu0 << core;
                const size_t used = strlen(where);
                snprintf(where + used, sizeof(where) - used, "%s%d", workers == 1 ? "" : ",", core);
            }
        }
        if (workers != 0)
            s_pipeline.start(masks, workers);
    }
    if (!s_native && !s_shadow)
        pf::logInfo("vp6: guest decoder, CPU colour conversion");
    else
    {
        char decoder[32] = "the game's decode thread";
        if (s_core >= 0)
            snprintf(decoder, sizeof(decoder), "a thread on core %d", s_core);
        pf::logInfo("vp6: native decoder on %s%s%s%s, %s colour conversion", decoder,
                    where[0] ? ", reconstruction threads on cores " : "", where,
                    s_shadow ? ", compared with the guest (shadow_vp6)" : "",
                    s_gpu && s_native && !s_shadow ? "GPU" : "CPU");
    }
}

void recordTime(uint32_t us)
{
    s_statSum += us;
    s_statMax = std::max(s_statMax, us);
    if (++s_statCount == StatFrames)
    {
        // Averages per frame in milliseconds.
        const double n = s_statCount * 1000.0;
        char stages[192] = "";
        if (s_native)
            snprintf(stages, sizeof(stages), " (header %.2f, setup %.2f, rows %.1f, waiting %.1f, end %.2f; "
                     "reconstruction %.1f ms on all threads, %u of %u rows on the decoding thread)",
                     double(s_headerSum) / n, double(s_times.setup) / n, double(s_times.decode) / n,
                     double(s_times.wait) / n, double(s_times.end) / n, double(s_times.reconstruct) / n,
                     s_times.ownRows, s_times.rows);
        pf::logInfo("vp6: %d frames, %s decode avg %.1f ms, max %.1f ms%s; %d conversions avg %.2f ms, max %.2f ms, "
                    "%d as planes for the GPU", s_statCount, s_native ? "native" : "guest", double(s_statSum) / n,
                    s_statMax / 1000.0, stages, s_convertCount,
                    s_convertCount ? double(s_convertSum) / s_convertCount / 1000.0 : 0.0, s_convertMax / 1000.0,
                    s_convertGpu);
        s_statCount = 0;
        s_statSum = 0;
        s_statMax = 0;
        s_headerSum = 0;
        s_times = BodyTimes{};
        s_convertCount = 0;
        s_convertGpu = 0;
        s_convertSum = 0;
        s_convertMax = 0;
    }
}

// Low word of the guest's time stamp counter (CPU::rdtsc).
uint32_t timestamp()
{
    return uint32_t(pf::ticksUs() * x86::TimestampMhz);
}

// 0x87f85f's decode time statistics: the last time in TSC ticks per [0x164], and its running average.
void storeDecodeTime(x86::reg32 instance, uint32_t start)
{
    const uint32_t mhz = rc::ld32(instance + vp6::InstCpuMhz);
    if (mhz == 0)
        return;
    const uint32_t ticks = (timestamp() - start) / mhz;
    rc::st32(instance + vp6::InstDecodeTime, ticks);
    const uint32_t average = rc::ld32(instance + vp6::InstDecodeTime + 4);
    rc::st32(instance + vp6::InstDecodeTime + 4, average == 0 ? ticks : (average * 7 + ticks) >> 3);
}

// 0x881996 on a copy of the caller's state.
void resizeBuffers(win32::WinApplication* app, x86::CPU& cpu, x86::reg32 instance)
{
    x86::CPU copy = cpu;
    copy.esp -= 4;
    rc::st32(copy.esp, instance);
    native::callGuest(app, copy, ResizeBuffers);
    if (copy.terminate)
        cpu.terminate = true;
}

// The native decode of one frame; the header may call the guest, the rest runs without the execution lock unless
// locked is set. Returns the decode time in microseconds, waiting for the lock excluded.
uint32_t decodeNative(win32::WinApplication* app, x86::CPU& cpu, x86::reg32 instance, x86::reg32 data, bool locked)
{
    const x86::reg32 entry = cpu.esp;
    const uint32_t start = timestamp();
    const uint32_t begin = pf::perf::now();
    uint32_t quantiser;
    if (vp6::headerBegin(instance, data, quantiser))
        resizeBuffers(app, cpu, instance);
    vp6::headerEnd(instance, quantiser);
    s_headerSum += pf::perf::now() - begin;
    // 0x87f85f passes 0x882a93 a stack slot 0x24 bytes below 0x87f970's return address.
    if (!locked)
        app->unlockContext(cpu);
    runFrameBody(instance, data, entry - 0x24);
    const uint32_t elapsed = pf::perf::now() - begin;
    if (!locked)
        app->lockContext(cpu);
    storeDecodeTime(instance, start);
    return elapsed;
}

// Guest memory a frame decode reads or writes, by decoder instance.
struct Region
{
    x86::reg32 address;
    uint32_t size;
    const char* name;
};

std::vector<Region> decoderRegions(x86::reg32 instance)
{
    auto ld = [](x86::reg32 a) { return rc::ld32(a); };
    std::vector<Region> r;
    r.push_back({ instance, vp6::InstanceSize, "instance" });
    const uint32_t frame = ld(instance + vp6::InstFrameSize) + 2 * ld(instance + vp6::InstChromaSize)
                         + ld(instance + 0x154);
    static const char* const frames[3] = { "frame0", "frame1", "frame2" };
    for (int i = 0; i < 3; ++i)
        r.push_back({ (ld(instance + 0x1ac + 4 * i) + 0x1f) & ~0x1fu, frame, frames[i] });
    const uint32_t blocksX = ld(instance + 0x178);
    r.push_back({ ld(instance + 0x10), 0x300, "coefficients" });
    r.push_back({ ld(instance + 0xc4), (blocksX + 8) * 16, "above Y" });
    r.push_back({ ld(instance + 0xc8), ((blocksX >> 1) + 8) * 16, "above U" });
    r.push_back({ ld(instance + 0xcc), ((blocksX >> 1) + 8) * 16, "above V" });
    r.push_back({ ld(instance + 0x34c), ld(instance + 0x1a0), "mode map" });
    r.push_back({ ld(instance + 0x354), ld(instance + 0x1a0) * 4, "vector map" });
    r.push_back({ ld(instance + 0x1c8), 128, "residual" });
    r.push_back({ ld(instance + 0x1d0), 128, "scratch" });
    r.push_back({ ld(instance + 0x1d8), 128, "prediction" });
    const x86::reg32 quant = ld(instance + 0xf4);
    r.push_back({ quant, 0x60, "quantiser" });
    r.push_back({ ld(quant + 0x50), 128, "dequant Y" });
    r.push_back({ ld(quant + 0x54), 128, "dequant UV" });
    r.push_back({ ld(instance + 0x148), 0x20, "border" });
    r.push_back({ 0xbbbeb0, 4, "argument slot" });
    return r;
}

// Instance bytes not compared: the decode time (a measurement) and the upper bytes of the Huffman tree child words,
// which the guest fills from its uninitialised stack and never reads.
bool ignoredInstanceByte(uint32_t offset)
{
    if (offset >= vp6::InstDecodeTime && offset < vp6::InstDecodeTime + 8)
        return true;
    auto inTree = [offset](uint32_t base, uint32_t nodes) {
        if (offset < base || offset >= base + 12 * nodes)
            return false;
        const uint32_t b = (offset - base) % 12;
        return (b >= 1 && b <= 3) || (b >= 5 && b <= 7);
    };
    for (uint32_t p = 0; p < 2; ++p)
        if (inTree(0x364 + 0x90 * p, 11) || inTree(0x21ac + 0xa8 * p, 8))
            return true;
    for (uint32_t t = 0; t < 36; ++t)
        if (inTree(0x4fc + 0x90 * t, 11))
            return true;
    return false;
}

void save(const std::vector<Region>& regions, std::vector<uint8_t>& to)
{
    size_t size = 0;
    for (const Region& r : regions)
        size += r.size;
    to.resize(size);
    size_t at = 0;
    for (const Region& r : regions)
    {
        memcpy(to.data() + at, rc::host(r.address), r.size);
        at += r.size;
    }
}

void restore(const std::vector<Region>& regions, const std::vector<uint8_t>& from)
{
    size_t at = 0;
    for (const Region& r : regions)
    {
        memcpy(rc::host(r.address), from.data() + at, r.size);
        at += r.size;
    }
}

// Compares the guest and native results of saved regions; returns the differing bytes, the first ones logged.
// instance: the first region is a decoder instance, compared without its ignored bytes.
int compareRegions(const std::vector<Region>& regions, const std::vector<uint8_t>& guest, const char* what, int call,
                   bool instance)
{
    int bad = 0;
    size_t at = 0;
    for (const Region& r : regions)
    {
        const uint8_t* n = rc::host(r.address);
        for (uint32_t k = 0; k < r.size; ++k)
        {
            if (guest[at + k] == n[k] || (instance && at == 0 && ignoredInstanceByte(k)))
                continue;
            if (s_reported < 24)
            {
                ++s_reported;
                pf::logInfo("%s: call %d %s+0x%x (0x%08x): guest %02x native %02x", what, call, r.name, k,
                            r.address + k, guest[at + k], n[k]);
            }
            ++bad;
        }
        at += r.size;
    }
    return bad;
}

// Runs the guest decoder, then the native one on the same state, and compares every region; the guest result and
// registers stay.
void decodeShadow(win32::WinApplication* app, x86::CPU& cpu, x86::reg32 instance, x86::reg32 data)
{
    static std::vector<uint8_t> before, guest;
    const std::vector<Region> regions = decoderRegions(instance);
    save(regions, before);
    // A size change reallocates the buffers: such a frame is decoded by the guest alone.
    uint32_t quantiser;
    const bool resize = vp6::headerBegin(instance, data, quantiser);
    restore(regions, before);
    x86::CPU guestCpu = cpu;
    sub_87f970_guest(app, guestCpu);
    if (resize)
    {
        ++s_skipped;
        pf::logInfo("shadow_vp6: frame with a new size decoded by the guest only (%d so far)", s_skipped);
        cpu = guestCpu;
        return;
    }
    save(regions, guest);
    restore(regions, before);
    decodeNative(app, cpu, instance, data, true);
    ++s_compared;
    const int bad = compareRegions(regions, guest, "shadow_vp6", s_compared, true);
    if (bad)
        ++s_differ;
    if (bad || s_compared % StatFrames == 0)
        pf::logInfo("shadow_vp6: %d calls compared, %d differ%s", s_compared, s_differ,
                    bad ? " (this call differs)" : "");
    restore(regions, guest);
    cpu = guestCpu;
}

// The format 1 conversion of 0x57dff0: the native result, with the guest's side effects. With gpu, a frame for a
// whole locked texture goes to the texture as planes instead and the locked pixels stay unwritten.
void convertNative(x86::CPU& cpu, const uint32_t args[9], bool gpu)
{
    vp6::Rows rows;
    bool convert;
    cpu.eax = vp6::prepareConversion(args, rows, convert);
    if (convert)
    {
        const pf::render::Planes planes{ rc::host(rows.y), rc::host(rows.u), rc::host(rows.v), rows.yStride,
                                         rows.yStride >> 1, rows.width, rows.height };
        const bool streamed = gpu && rows.destination == args[6] && int32_t(rows.yStride) > 0
                           && win32::d3d9::streamPlanes(rows.destination, rows.pitch, planes);
        if (streamed)
            ++s_convertGpu;
        else
            vp6::convertRows(rows);
        rc::st32(cpu.esp + 0x24, args[5]);
    }
    cpu.esp += 4;
}

// Runs the guest conversion, then the native one, and compares the pixels, tables, flag and result.
void convertShadow(win32::WinApplication* app, x86::CPU& cpu, const uint32_t args[9])
{
    static std::vector<uint8_t> before, guest;
    std::vector<Region> regions;
    const int32_t height = int32_t(args[5]), pitch = int32_t(args[7]);
    if (height > 0 && pitch > 0)
        regions.push_back({ args[6], uint32_t(height - 1) * uint32_t(pitch) + args[4] * 4 + 4, "pixels" });
    regions.push_back({ vp6::ConvertTables, vp6::ConvertTablesSize, "tables" });
    regions.push_back({ vp6::ConvertOrderFlag, 4, "order" });
    regions.push_back({ cpu.esp + 0x24, 4, "row count" });
    save(regions, before);
    x86::CPU guestCpu = cpu;
    sub_57dff0_guest(app, guestCpu);
    save(regions, guest);
    restore(regions, before);
    x86::CPU nativeCpu = cpu;
    convertNative(nativeCpu, args, false);
    ++s_convertCompared;
    // The one caller (0x55ad76) overwrites eax right after the call, so only memory is compared.
    const int bad = compareRegions(regions, guest, "shadow_vp6 conversion", s_convertCompared, false);
    if (bad)
        ++s_convertDiffer;
    if (bad || s_convertCompared % StatFrames == 0)
        pf::logInfo("shadow_vp6: %d conversions compared, %d differ%s", s_convertCompared, s_convertDiffer,
                    bad ? " (this call differs)" : "");
    restore(regions, guest);
    cpu = guestCpu;
}

}

namespace native
{

// 0x87f970: decodes one VP6 frame (cdecl: decoder instance, frame data); returns 0.
void sub_87f970(win32::WinApplication* app, x86::CPU& cpu)
{
    if (!s_loaded)
        loadSettings();
    const x86::reg32 instance = rc::ld32(cpu.esp + 4);
    const x86::reg32 data = rc::ld32(cpu.esp + 8);
    if (s_shadow)
    {
        decodeShadow(app, cpu, instance, data);
        return;
    }
    if (!s_native)
    {
        const uint32_t start = pf::perf::now();
        sub_87f970_guest(app, cpu);
        recordTime(pf::perf::now() - start);
        return;
    }
    recordTime(decodeNative(app, cpu, instance, data, false));
    cpu.eax = 0;
    cpu.esp += 4;
}

// 0x57dff0: converts a decoded movie frame to the pixels of a locked texture (cdecl, nine arguments, the last the
// pixel format); format 1 is native, the others run the guest.
void sub_57dff0(win32::WinApplication* app, x86::CPU& cpu)
{
    if (!s_loaded)
        loadSettings();
    uint32_t args[9];
    for (int i = 0; i < 9; ++i)
        args[i] = rc::ld32(cpu.esp + 4 + 4 * x86::reg32(i));
    if (args[8] != 1 || !(s_native || s_shadow))
    {
        sub_57dff0_guest(app, cpu);
        return;
    }
    const uint32_t start = pf::perf::now();
    if (s_shadow)
        convertShadow(app, cpu, args);
    else
        convertNative(cpu, args, s_gpu);
    const uint32_t elapsed = pf::perf::now() - start;
    s_convertSum += elapsed;
    s_convertMax = std::max(s_convertMax, elapsed);
    ++s_convertCount;
}

}
}
