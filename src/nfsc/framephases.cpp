#include "framephases.h"
#include "display.h"
#include "overlap.h"
#include "solids.h"

#include <native.h>
#include <recomp.h>
#include <platform/log.h>
#include <platform/perf.h>
#include <platform/sync.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>

namespace nfsc
{

// Translated bodies of the timed guest functions.
void sub_72eba0_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_72e0f0_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_72e460_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_72e350_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_7275a0_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_72e5b0_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_729770_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_730cb0_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_79afa0_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_7d1c40_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_72bc30_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_74e350_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_7103c0_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_714db0_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_7300d0_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_750c30_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_750d40_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_7a4ec0_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_7dcd20_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_6adcc0_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_6b4760_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_557f30_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_69ede0_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_6b5d30_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_699320_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_55c240_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_55eed0_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_55c830_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_55dd60_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_7473a0_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_560400_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_55f6c0_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_55dfc0_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_55ebd0_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_55cd60_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_55caa0_guest(win32::WinApplication* app, x86::CPU& cpu);

namespace phases
{

namespace
{

const uint32_t SlowUs = 33400;
const uint32_t LineUs = 5000000;
const int Worst = 3;

// Display call parts: the 3D frame (inclusive) and the passes inside it, the front end/HUD layer, the GPU query wait.
// After them, parts inside those (inclusive, any pass): scenery culling, world models, list sorting, the 3D frame's
// set-up calls, the HUD layer's 2D call and light flare pools.
enum DisplayPart { Frame3d, Reflection, EnvFace, Mirror, Depth, MainView, Post, Hud, GpuQuery, SceneryCull,
                   WorldModels, ListSort, PreDisplay, Setup1, Setup2, Hud2d, HudFlares, DisplayPartCount };
const char* const s_displayNames[DisplayPartCount] = {
    "3d", "reflection", "envmap", "mirror", "depth", "main", "post", "hud", "gpuquery", "scenerycull",
    "worldmodels", "listsort", "predisplay", "setup7103c0", "setup714db0", "hud7300d0", "hudflares" };
// Game frame parts; the loading parts nest (inclusive): resource file service (decompression and its chunks),
// queued file callbacks (section loads and their chunks), chunk loads, table fix-ups, decompression, file reads.
enum GamePart { Sim, Updates, Events, World, FrontEnd, Loading, Barrier, FileService, FileCallbacks, Chunks, FixUps,
                Decompress, FileReads, Direct3d, GamePartCount };
const char* const s_gameNames[GamePartCount] = {
    "sim", "updates", "events", "world", "frontend", "loading", "barrier", "fileservice", "filecallbacks", "chunks",
    "fixups", "decompress", "filereads", "d3d" };

// Direct3D call time of the game thread by shim over the interval (while a display call overlaps).
const int DeviceShims = 16;
const char* s_deviceName[DeviceShims];
uint32_t s_deviceUs[DeviceShims];
uint32_t s_deviceCount[DeviceShims];
// Parts that sit inside others and are left out of the frame's sum.
bool nested(int part)
{
    return part == Barrier || part >= FileService;
}

// Inside the solid list and texture pack loaders (inclusive, game thread), over the interval.
enum LoaderPart { SolidHeaders, Solids, SolidNotify, ConnectSolid, FixTextureTable, LightMaterial, PackHeaders,
                  AssignTextures, CreateTexture, TextureNotify, SolidTextureNotify, TextureAnims, VramData,
                  LoaderPartCount };
const char* const s_loaderNames[LoaderPartCount] = {
    "solidheaders", "solids", "solidnotify", "connectsolid", "fixtexturetable", "lightmaterial", "packheaders",
    "assigntextures", "createtexture", "texturenotify", "solidtexturenotify", "textureanims", "vramdata" };
uint32_t s_loaderUs[LoaderPartCount];
uint32_t s_loaderCount[LoaderPartCount];

// Chunk load time by chunk id over the interval (game thread only).
const int ChunkIds = 32;
uint32_t s_chunkId[ChunkIds];
uint32_t s_chunkUs[ChunkIds];
uint32_t s_chunkCount[ChunkIds];

struct DisplayRecord
{
    uint32_t total = 0;
    uint32_t parts[DisplayPartCount] = {};
};

struct GameRecord
{
    uint32_t total = 0;
    uint32_t parts[GamePartCount] = {};
};

// The current frame's parts (each array written by its own thread).
uint32_t s_display[DisplayPartCount];
uint32_t s_game[GamePartCount];

// The interval's counts and worst frames; the display side under the lock.
pf::LwMutex s_lock("frame_phases");
uint32_t s_displayFrames = 0, s_displaySlow = 0;
DisplayRecord s_displayWorst[Worst];
uint32_t s_gameFrames = 0, s_gameSlow = 0;
GameRecord s_gameWorst[Worst];
uint32_t s_lineStart = 0;

template< typename R >
void keepWorst(R (&worst)[Worst], const R& record)
{
    int lowest = 0;
    for (int i = 1; i < Worst; ++i)
    {
        if (worst[i].total < worst[lowest].total)
            lowest = i;
    }
    if (record.total > worst[lowest].total)
        worst[lowest] = record;
}

void appendf(char* text, size_t size, size_t& length, const char* format, ...) __attribute__((format(printf, 4, 5)));

void appendf(char* text, size_t size, size_t& length, const char* format, ...)
{
    if (length >= size)
        return;
    va_list args;
    va_start(args, format);
    const int n = vsnprintf(text + length, size - length, format, args);
    va_end(args);
    if (n > 0)
        length += size_t(n);
}

double ms(uint32_t us)
{
    return double(us) / 1000.0;
}

// Game thread: one line per interval.
void logLine()
{
    DisplayRecord display[Worst];
    uint32_t displayFrames, displaySlow;
    {
        pf::LwLockGuard guard(s_lock);
        std::copy(s_displayWorst, s_displayWorst + Worst, display);
        displayFrames = s_displayFrames;
        displaySlow = s_displaySlow;
        s_displayFrames = s_displaySlow = 0;
        std::fill(s_displayWorst, s_displayWorst + Worst, DisplayRecord());
    }
    char text[1000];
    size_t length = 0;
    appendf(text, sizeof(text), length, "perf: slow frames: display %u of %u, game %u of %u |", displaySlow,
            displayFrames, s_gameSlow, s_gameFrames);
    std::sort(display, display + Worst, [](const DisplayRecord& a, const DisplayRecord& b) { return a.total > b.total; });
    for (const DisplayRecord& r : display)
    {
        if (!r.total)
            break;
        uint32_t passes = 0;
        for (int p = Reflection; p <= Post; ++p)
            passes += r.parts[p];
        const uint32_t inner = r.parts[Frame3d] > passes ? r.parts[Frame3d] - passes : 0;
        const uint32_t counted = r.parts[Frame3d] + r.parts[Hud] + r.parts[GpuQuery];
        appendf(text, sizeof(text), length, " display %.1f =", ms(r.total));
        for (int p = Reflection; p <= Post; ++p)
        {
            if (r.parts[p])
                appendf(text, sizeof(text), length, " %s %.1f", s_displayNames[p], ms(r.parts[p]));
        }
        appendf(text, sizeof(text), length, " 3d-other %.1f hud %.1f gpuquery %.1f rest %.1f (inside:", ms(inner),
                ms(r.parts[Hud]), ms(r.parts[GpuQuery]), ms(r.total > counted ? r.total - counted : 0));
        for (int p = SceneryCull; p < DisplayPartCount; ++p)
        {
            if (r.parts[p] >= 100)
                appendf(text, sizeof(text), length, " %s %.1f", s_displayNames[p], ms(r.parts[p]));
        }
        appendf(text, sizeof(text), length, ");");
    }
    std::sort(s_gameWorst, s_gameWorst + Worst, [](const GameRecord& a, const GameRecord& b) { return a.total > b.total; });
    for (const GameRecord& r : s_gameWorst)
    {
        if (!r.total)
            break;
        uint32_t counted = 0;
        appendf(text, sizeof(text), length, " game %.1f =", ms(r.total));
        for (int p = 0; p < GamePartCount; ++p)
        {
            if (!nested(p))
                counted += r.parts[p];
            if (r.parts[p])
                appendf(text, sizeof(text), length, " %s %.1f", s_gameNames[p], ms(r.parts[p]));
        }
        appendf(text, sizeof(text), length, " rest %.1f;", ms(r.total > counted ? r.total - counted : 0));
    }
    pf::logInfo("%s", text);
    // The chunk ids that took longest to load over the interval.
    length = 0;
    text[0] = 0;
    for (int n = 0; n < 6; ++n)
    {
        int best = -1;
        for (int i = 0; i < ChunkIds; ++i)
        {
            if (s_chunkUs[i] && (best < 0 || s_chunkUs[i] > s_chunkUs[best]))
                best = i;
        }
        if (best < 0)
            break;
        appendf(text, sizeof(text), length, "%s chunk 0x%08x %u in %.1f ms", n ? "," : "perf: chunk loads:",
                unsigned(s_chunkId[best]), unsigned(s_chunkCount[best]), ms(s_chunkUs[best]));
        s_chunkUs[best] = 0;
    }
    std::fill(s_chunkId, s_chunkId + ChunkIds, 0u);
    std::fill(s_chunkUs, s_chunkUs + ChunkIds, 0u);
    std::fill(s_chunkCount, s_chunkCount + ChunkIds, 0u);
    // The game thread's Direct3D calls that took longest.
    for (int n = 0; n < 5; ++n)
    {
        int best = -1;
        for (int i = 0; i < DeviceShims; ++i)
        {
            if (s_deviceUs[i] && (best < 0 || s_deviceUs[i] > s_deviceUs[best]))
                best = i;
        }
        if (best < 0)
            break;
        appendf(text, sizeof(text), length, "%s %s %u in %.1f ms", n ? "," : (length ? " | game d3d:" :
                "perf: chunk loads: none | game d3d:"), s_deviceName[best], unsigned(s_deviceCount[best]),
                ms(s_deviceUs[best]));
        s_deviceUs[best] = 0;
    }
    std::fill(s_deviceName, s_deviceName + DeviceShims, nullptr);
    std::fill(s_deviceUs, s_deviceUs + DeviceShims, 0u);
    std::fill(s_deviceCount, s_deviceCount + DeviceShims, 0u);
    if (length)
        pf::logInfo("%s", text);
    length = 0;
    for (int p = 0; p < LoaderPartCount; ++p)
    {
        if (s_loaderCount[p])
            appendf(text, sizeof(text), length, "%s %s %u in %.1f ms", length ? "," : "perf: chunk loaders:",
                    s_loaderNames[p], unsigned(s_loaderCount[p]), ms(s_loaderUs[p]));
    }
    std::fill(s_loaderUs, s_loaderUs + LoaderPartCount, 0u);
    std::fill(s_loaderCount, s_loaderCount + LoaderPartCount, 0u);
    // The Direct3D layer's texture calls of any thread (creation, lock, unlock with conversion and upload).
    static const char* const textureNames[pf::perf::TextureCallCount] = { "d3dcreate", "d3dlock", "d3dunlock" };
    uint32_t textureCounts[pf::perf::TextureCallCount];
    uint32_t textureUs[pf::perf::TextureCallCount];
    pf::perf::takeTextureCalls(textureCounts, textureUs);
    for (int c = 0; c < pf::perf::TextureCallCount; ++c)
    {
        if (textureCounts[c])
            appendf(text, sizeof(text), length, "%s %s %u in %.1f ms", length ? "," : "perf: chunk loaders:",
                    textureNames[c], unsigned(textureCounts[c]), ms(textureUs[c]));
    }
    if (length)
        pf::logInfo("%s", text);
    s_gameFrames = s_gameSlow = 0;
    std::fill(s_gameWorst, s_gameWorst + Worst, GameRecord());
}

void addDisplay(DisplayPart part, uint32_t us)
{
    s_display[part] += us;
}

void addGame(GamePart part, uint32_t us)
{
    s_game[part] += us;
}

void addLoader(LoaderPart part, uint32_t us)
{
    s_loaderUs[part] += us;
    ++s_loaderCount[part];
}

void addChunk(uint32_t id, uint32_t us)
{
    for (int i = 0; i < ChunkIds; ++i)
    {
        if (s_chunkId[i] == id || (!s_chunkId[i] && !s_chunkUs[i]))
        {
            s_chunkId[i] = id;
            s_chunkUs[i] += us;
            ++s_chunkCount[i];
            return;
        }
    }
}

}

void textureCreation(uint32_t microseconds)
{
    addLoader(CreateTexture, microseconds);
}

void displayDone(uint32_t microseconds)
{
    if (!pf::perf::enabled())
        return;
    DisplayRecord record;
    record.total = microseconds;
    s_display[GpuQuery] = pf::perf::takeDisplayFrameWait(pf::perf::DisplayGpu);
    std::copy(s_display, s_display + DisplayPartCount, record.parts);
    std::fill(s_display, s_display + DisplayPartCount, 0u);
    pf::LwLockGuard guard(s_lock);
    ++s_displayFrames;
    if (microseconds > SlowUs)
    {
        ++s_displaySlow;
        keepWorst(s_displayWorst, record);
    }
}

void gameDone(uint32_t microseconds)
{
    if (!pf::perf::enabled())
        return;
    GameRecord record;
    record.total = microseconds;
    std::copy(s_game, s_game + GamePartCount, record.parts);
    std::fill(s_game, s_game + GamePartCount, 0u);
    ++s_gameFrames;
    if (microseconds > SlowUs)
    {
        ++s_gameSlow;
        keepWorst(s_gameWorst, record);
    }
    const uint32_t now = pf::perf::now();
    if (!s_lineStart)
        s_lineStart = now;
    if (now - s_lineStart >= LineUs)
    {
        s_lineStart = now;
        logLine();
    }
}

void gameDevice(const char* shim, uint32_t microseconds)
{
    if (!pf::perf::enabled())
        return;
    addGame(Direct3d, microseconds);
    for (int i = 0; i < DeviceShims; ++i)
    {
        if (s_deviceName[i] == shim || !s_deviceName[i])
        {
            s_deviceName[i] = shim;
            s_deviceUs[i] += microseconds;
            ++s_deviceCount[i];
            return;
        }
    }
}

void sim(uint32_t microseconds) { addGame(Sim, microseconds); }
void updates(uint32_t microseconds) { addGame(Updates, microseconds); }
void barrier(uint32_t microseconds) { addGame(Barrier, microseconds); }
void frontEnd(uint32_t microseconds) { addGame(FrontEnd, microseconds); }
void loading(uint32_t microseconds) { addGame(Loading, microseconds); }

}

namespace native
{

#define NFSC_DISPLAY_TIMED(address, part)                                                       \
    void sub_##address(win32::WinApplication* app, x86::CPU& cpu)                               \
    {                                                                                           \
        if (!pf::perf::enabled())                                                               \
        {                                                                                       \
            sub_##address##_guest(app, cpu);                                                    \
            return;                                                                             \
        }                                                                                       \
        const uint32_t start = pf::perf::now();                                                 \
        sub_##address##_guest(app, cpu);                                                        \
        phases::addDisplay(phases::part, pf::perf::now() - start);                              \
    }

#define NFSC_GAME_TIMED(address, part)                                                          \
    void sub_##address(win32::WinApplication* app, x86::CPU& cpu)                               \
    {                                                                                           \
        if (!pf::perf::enabled())                                                               \
        {                                                                                       \
            sub_##address##_guest(app, cpu);                                                    \
            return;                                                                             \
        }                                                                                       \
        const uint32_t start = pf::perf::now();                                                 \
        sub_##address##_guest(app, cpu);                                                        \
        phases::addGame(phases::part, pf::perf::now() - start);                                 \
    }

// The 3D frame and its passes: road reflection, a car environment map face, mirror/PIP views, the motion blur depth
// pass, the main view, the post chain; the front end/HUD layer.
NFSC_DISPLAY_TIMED(72eba0, Frame3d)
NFSC_DISPLAY_TIMED(72e0f0, Reflection)
NFSC_DISPLAY_TIMED(72e460, EnvFace)
NFSC_DISPLAY_TIMED(72e350, Mirror)
NFSC_DISPLAY_TIMED(7275a0, Depth)
NFSC_DISPLAY_TIMED(72e5b0, MainView)
NFSC_DISPLAY_TIMED(729770, Post)
NFSC_DISPLAY_TIMED(730cb0, Hud)
// GrandSceneryCullInfo::StuffScenery, WorldModel::Render, the draw list sort, Render::PreDisplay, two set-up calls of
// the 3D frame, the HUD layer's 2D call and its two light flare pool calls.
NFSC_DISPLAY_TIMED(79afa0, SceneryCull)
NFSC_DISPLAY_TIMED(7d1c40, WorldModels)
NFSC_DISPLAY_TIMED(72bc30, ListSort)
NFSC_DISPLAY_TIMED(74e350, PreDisplay)
NFSC_DISPLAY_TIMED(7103c0, Setup1)
NFSC_DISPLAY_TIMED(714db0, Setup2)
NFSC_DISPLAY_TIMED(7300d0, Hud2d)
NFSC_DISPLAY_TIMED(750c30, HudFlares)
NFSC_DISPLAY_TIMED(750d40, HudFlares)
// emProcessAllEvents, World_Service.
NFSC_GAME_TIMED(7a4ec0, Events)
NFSC_GAME_TIMED(7dcd20, World)
// Loading: ResourceFile service (decompress, load its chunks), queued file callbacks, eFixUpTables, LZDecompress,
// StartQueuedFileReading.
NFSC_GAME_TIMED(6adcc0, FileService)

// CheckQueuedFileCallbacks: after the display overlap's barrier when a finished file's callback runs.
void sub_6b4760(win32::WinApplication* app, x86::CPU& cpu)
{
    overlap::beforeQueuedFileCallbacks(app, cpu);
    if (!pf::perf::enabled())
    {
        sub_6b4760_guest(app, cpu);
        return;
    }
    const uint32_t start = pf::perf::now();
    sub_6b4760_guest(app, cpu);
    phases::addGame(phases::FileCallbacks, pf::perf::now() - start);
}
NFSC_GAME_TIMED(557f30, FixUps)
NFSC_GAME_TIMED(69ede0, Decompress)
NFSC_GAME_TIMED(6b5d30, FileReads)

#define NFSC_LOADER_TIMED(address, part)                                                        \
    void sub_##address(win32::WinApplication* app, x86::CPU& cpu)                               \
    {                                                                                           \
        if (!pf::perf::enabled() || display::onDisplayThread(cpu))                              \
        {                                                                                       \
            sub_##address##_guest(app, cpu);                                                    \
            return;                                                                             \
        }                                                                                       \
        const uint32_t start = pf::perf::now();                                                 \
        sub_##address##_guest(app, cpu);                                                        \
        phases::addLoader(phases::part, pf::perf::now() - start);                               \
    }

// Solid lists: InternalLoaderSolidHeaderChunks, InternalLoaderSolidChunks, eModel::ConnectSolid,
// eSolid::FixTextureTable, elGetLightMaterial (NotifySolidLoader below).
NFSC_LOADER_TIMED(55c240, SolidHeaders)
NFSC_LOADER_TIMED(55eed0, Solids)
// eModel::ConnectSolid also runs outside chunk loads (eModel::Init for cars and props): after the display overlap's
// barrier, since it changes a model the display call may be drawing.
void sub_55c830(win32::WinApplication* app, x86::CPU& cpu)
{
    display::barrier(app, cpu);
    if (!pf::perf::enabled() || display::onDisplayThread(cpu))
    {
        sub_55c830_guest(app, cpu);
        return;
    }
    const uint32_t start = pf::perf::now();
    sub_55c830_guest(app, cpu);
    phases::addLoader(phases::ConnectSolid, pf::perf::now() - start);
}
NFSC_LOADER_TIMED(55dd60, FixTextureTable)
NFSC_LOADER_TIMED(7473a0, LightMaterial)
// Texture packs: InternalLoadTexturePackHeaderChunks, TexturePack::AssignTextureData (the texture creation is the
// rest of it), eTexture::NotifyTexturePackLoading, eSolidNotifyTextureLoading, InitTextureAnimations,
// InternalLoadVRAMDataChunks.
NFSC_LOADER_TIMED(560400, PackHeaders)
NFSC_LOADER_TIMED(55f6c0, AssignTextures)
NFSC_LOADER_TIMED(55dfc0, TextureNotify)
NFSC_LOADER_TIMED(55ebd0, SolidTextureNotify)
NFSC_LOADER_TIMED(55cd60, TextureAnims)
NFSC_LOADER_TIMED(55caa0, VramData)

#undef NFSC_LOADER_TIMED

// NotifySolidLoader (solids.cpp), timed.
void sub_55dc20(win32::WinApplication* app, x86::CPU& cpu)
{
    if (!pf::perf::enabled() || display::onDisplayThread(cpu))
    {
        solids::notifySolidLoader(app, cpu);
        return;
    }
    const uint32_t start = pf::perf::now();
    solids::notifySolidLoader(app, cpu);
    phases::addLoader(phases::SolidNotify, pf::perf::now() - start);
}

// One chunk's load (cdecl: chunk), timed by chunk id.
void sub_699320(win32::WinApplication* app, x86::CPU& cpu)
{
    if (!pf::perf::enabled())
    {
        sub_699320_guest(app, cpu);
        return;
    }
    const uint32_t id = rc::ld32(rc::ld32(cpu.esp + 4));
    const uint32_t start = pf::perf::now();
    sub_699320_guest(app, cpu);
    const uint32_t spent = pf::perf::now() - start;
    phases::addGame(phases::Chunks, spent);
    phases::addChunk(id, spent);
}

#undef NFSC_DISPLAY_TIMED
#undef NFSC_GAME_TIMED

}
}
