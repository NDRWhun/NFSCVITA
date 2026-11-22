#ifndef PLATFORM_GXM_RENDER_H_
#define PLATFORM_GXM_RENDER_H_

// Direct3D frames on SceGxm (platform/render.h): GPU memory, resources, frame slots (game thread) and the
// drawing of a frame's scenes (render thread). Used by gxm_device.cpp.

#include <platform/render.h>
#include <platform/sync.h>
#include <psp2/gxm.h>
#include <psp2/kernel/sysmem.h>
#include <atomic>
#include <cstdint>
#include <map>
#include <unordered_map>
#include <vector>

namespace pf { namespace gxm {

// Sub-allocates GPU-mapped memory blocks (first fit, coalescing free list); game thread only.
class GpuHeap
{
public:
    GpuHeap(const char* name, SceKernelMemBlockType type, uint32_t chunkSize, uint32_t attributes);
    ~GpuHeap();
    void* allocate(uint32_t size, uint32_t alignment);   // null when no block can be added
    void free(void* pointer);

private:
    struct Chunk
    {
        SceUID                          uid;
        uint8_t*                        base;
        uint32_t                        size;
        std::map<uint32_t, uint32_t>    holes;      // offset -> size
    };
    bool addChunk(uint32_t minimum);

    const char*                         m_name;
    SceKernelMemBlockType               m_type;
    uint32_t                            m_chunkSize;
    uint32_t                            m_attributes;
    std::vector<Chunk*>                 m_chunks;
    std::unordered_map<void*, std::pair<Chunk*, uint32_t>> m_used;   // pointer -> chunk, size
};

// GPU-read memory (MAIN_NC) for what frames in flight read: chunks are reused once every frame that used them is
// finished. One thread uses a pool.
class FramePool
{
public:
    typedef bool (*Finished)(void* context, uint32_t frameIndex);

    FramePool(const char* name, uint32_t chunkSize) : m_name(name), m_chunkSize(chunkSize) {}
    ~FramePool();
    // The frame the next allocations belong to; the current chunk is read by it too.
    void begin(uint32_t frameIndex, Finished finished, void* context);
    void* allocate(uint32_t size, uint32_t alignment);
    // A whole free chunk of at least minimum bytes, which becomes the current one.
    void* take(uint32_t minimum, uint32_t* size);

private:
    struct Chunk
    {
        SceUID      uid;
        uint8_t*    base;
        uint32_t    size, used, lastUse;
    };
    int freeChunk(uint32_t minimum);

    const char*         m_name;
    uint32_t            m_chunkSize;
    std::vector<Chunk>  m_chunks;
    int                 m_current = -1;
    uint32_t            m_frame = 0;
    Finished            m_finished = nullptr;
    void*               m_context = nullptr;
};

// Cached host memory for the CPU-side words of precomputed objects; valid until reset (the next frame).
class HostArena
{
public:
    ~HostArena();
    void reset() { m_block = 0; m_used = 0; }
    void* allocate(uint32_t size);

private:
    enum { BlockSize = 64 * 1024 };
    std::vector<uint8_t*>   m_blocks;
    size_t                  m_block = 0;
    uint32_t                m_used = 0;
};

struct Texture : render::TextureStorage
{
    void*               data;
    uint32_t            bytes;
    uint32_t            faceStride;
    uint32_t            stride;         // row bytes of a linear texture (a 2D target with one level, unless tiled)
    SceGxmTexture       texture;
    std::vector<SceGxmColorSurface> surfaces;   // targets: face * levels + level
};

struct Depth : render::DepthStorage
{
    void*                       data;
    void*                       stencil;
    SceGxmDepthStencilSurface   surface;
};

struct Buffer : render::BufferStorage
{
};

class FrameRenderer
{
public:
    enum
    {
        Slots = 3
    };

    bool init(SceGxmContext* context, SceGxmShaderPatcher* patcher, volatile unsigned int* notifications);
    void shutdown();

    // Game thread.
    render::Frame* beginFrame(uint32_t width, uint32_t height);
    // The slot of a frame begun; the device queues it for the render thread.
    void submitted(render::Frame* frame, uint32_t vblanks);
    uint32_t completedFrame();
    void waitFrame(uint32_t index);
    render::TextureStorage* backBufferTexture(render::Frame* frame);
    render::TextureStorage* createTexture(const render::TextureDesc& desc);
    void writeTexture(render::TextureStorage* texture, uint32_t face, uint32_t level, const void* data, uint32_t pitch);
    void readTexture(const render::TextureStorage* texture, uint32_t face, uint32_t level, void* data, uint32_t pitch);
    void writePlanes(render::TextureStorage* texture, const render::Planes& planes);
    void copyTexture(render::TextureStorage* to, const render::TextureStorage* from);
    render::DepthStorage* createDepth(uint32_t width, uint32_t height);
    render::BufferStorage* createBuffer(uint32_t size);
    void release(render::TextureStorage* texture);
    void release(render::DepthStorage* depth);
    void release(render::BufferStorage* buffer);

    // Render thread. Back buffer scenes go to the display surface when the frame's back buffer has the display
    // size, or when stretchDirect (the back buffer fills the display, no gamma) and none of them uses depth;
    // else to the slot's offscreen target. presentScene(): the caller must add a scene that shows the frame
    // (the offscreen target scaled, or black when nothing drew the display); that scene then writes the done
    // notification, else the frame's last scene does. Both hold after drawFrame.
    bool presentScene(const render::Frame& frame) const;
    const SceGxmTexture* offscreen(const render::Frame& frame) const;
    // The frame's gamma ramp as a 256 x 1 table (red, green, blue in the colour channels); null for the identity.
    const SceGxmTexture* gammaRamp(const render::Frame& frame);
    void drawFrame(const render::Frame& frame, const SceGxmColorSurface* display, SceGxmSyncObject* displaySync,
                   bool stretchDirect);
    SceGxmNotification* doneNotification(const render::Frame& frame);
    uint32_t vblanks(const render::Frame& frame) const;

private:
    struct Slot
    {
        render::Frame                   frame;
        std::vector<render::Scene>      scenes;
        std::vector<render::DrawCall>   draws;
        std::vector<render::Viewport>   viewports;
        std::vector<float>              constants;
        void*                           vertices = nullptr;
        SceGxmNotification              done = {};
        bool                            inFlight = false;
        uint32_t                        vblanks = 2;
        Texture*                        offscreen = nullptr;    // back buffer when not the display size
        bool                            forceOffscreen = false; // back buffer sampled by the frame
        bool                            direct = false;         // back buffer scenes drawn to the display
        Texture*                        ramp = nullptr;         // gamma ramp table of the frame
    };
    // Per scene of the frame being drawn (render thread).
    struct ScenePlan
    {
        bool        depthUsed;      // cleared or tested by a draw
        bool        skipped;        // only colour clears: drawn at the start of scene carriedTo instead
        uint32_t    carriedTo;
        // Where it draws.
        const SceGxmColorSurface*   color;
        SceGxmSyncObject*           sync;
        uint32_t                    width, height;
        float                       scaleX, scaleY;
        uint32_t                    firstOrder, orderCount;     // its draws in m_order
        uint32_t                    firstJob, jobCount;
    };
    void planFrame(const render::Frame& frame, bool stretchDirect);
    // Moves the alpha-tested draws of a scene's order after the opaque draws that follow them up to the next draw
    // that is not depth-tested LESS/LESSEQUAL with depth write, full colour mask and no blend (setting draw_sort).
    void sortScene(const render::Frame& frame, uint32_t scene);
    // draw_sort=2, before that: opaque draws ahead of the earlier depth-tested draws that write no depth.
    void opaqueFirst(const render::Frame& frame, const render::Target& target, uint32_t* order, uint32_t count);
    // perf_log: HSR flush transitions (alpha-tested to opaque or blended draw) before and after sortScene.
    struct SortStats
    {
        uint32_t    frames = 0;
        uint64_t    draws = 0, discards = 0, before = 0, after = 0;     // largest scene of each frame
        uint64_t    allBefore = 0, allAfter = 0;                        // every scene
        uint32_t    largest = 0, largestDiscards = 0, largestBefore = 0, largestAfter = 0;  // this frame
    };
    // The slot's offscreen back buffer at this size (game thread).
    void prepareOffscreen(Slot& slot, uint32_t width, uint32_t height);
    struct Pending
    {
        int         kind;           // 0 texture, 1 depth, 2 buffer
        void*       object;
        uint32_t    after;          // freed once this frame is finished
    };
    struct VertexProgram
    {
        SceGxmShaderPatcherId           id = nullptr;
        const SceGxmProgram*            program = nullptr;
        std::vector<const SceGxmProgramParameter*> fields;
        uint32_t                        uniformBytes = 0;   // default uniform buffer
    };
    struct FragmentProgram
    {
        SceGxmShaderPatcherId           id = nullptr;
        const SceGxmProgram*            program = nullptr;
        std::vector<const SceGxmProgramParameter*> fields;
        uint32_t                        uniformBytes = 0;
        unsigned int                    units[render::MaxSamplers] = {};
        bool                            owned = false;      // registered here (not a copy of another entry)
    };
    enum
    {
        Variants = 1 + render::FactorCount      // plain, alpha test, the blend factor outputs
    };

    // A patched program and what its precomputed objects need.
    struct VertexEntry
    {
        SceGxmVertexProgram*    program = nullptr;
        uint8_t                 source = 0;         // render::VertexProgram
        uint32_t                drawSize = 0, stateSize = 0, uniformBytes = 0;
    };
    struct FragmentEntry
    {
        SceGxmFragmentProgram*  program = nullptr;
        uint32_t                stateSize = 0, uniformBytes = 0;
    };
    struct DrawPrograms
    {
        const VertexEntry*      vertex;
        const FragmentEntry*    fragment;
    };
    // What a context was last given within a scene or command list.
    struct ContextState
    {
        render::DrawState       last;
        bool                    valid = false;
        uint32_t                vertexConstants = 0, fragmentConstants = 0;
        uint16_t                viewport = 0xffff;
        const VertexEntry*      vertex = nullptr;
        const FragmentEntry*    fragment = nullptr;
        float                   scaleX = 1.0f, scaleY = 1.0f;   // back buffer to display, this scene
        bool                    depth = true;                   // the scene has a depth surface

        void reset(float x, float y, bool d)
        {
            valid = false;
            viewport = 0xffff;
            scaleX = x;
            scaleY = y;
            depth = d;
        }
    };
    // A thread that records draws: its context and the memory of its precomputed objects.
    struct Recorder
    {
        SceGxmContext*          context = nullptr;
        ContextState            state;
        FramePool               data{"gxm_draws", 512 * 1024};
        HostArena               words;
    };
    // A thread recording command lists: a deferred context and the pools its memory callbacks draw from.
    struct Worker
    {
        FrameRenderer*          owner = nullptr;
        Recorder                recorder;
        FramePool               vdm{"gxm_list_vdm", 64 * 1024};
        FramePool               vertex{"gxm_list_vertex", 256 * 1024};
        FramePool               fragment{"gxm_list_fragment", 64 * 1024};
        void*                   hostMem = nullptr;
        uint32_t                frame = 0;      // frame its pools were begun for
        Thread*                 thread = nullptr;
    };
    // A range of a scene's draws recorded into one command list.
    struct Job
    {
        uint32_t                scene, begin, end;  // m_order range
        bool                    ok;
        SceGxmCommandList       list;
    };

    void freePending();
    void destroy(int kind, void* object);
    const VertexEntry* vertexProgram(uint8_t program, uint8_t layout);
    // half: the scene's target stores half floats (the program writes HALF4, without blending).
    const FragmentEntry* fragmentProgram(const render::DrawState& state, bool half);
    SceGxmRenderTarget* renderTarget(uint32_t width, uint32_t height);
    static bool finished(void* self, uint32_t frameIndex);
    // Patched programs of every draw (render thread; creates them on first use).
    void preparePrograms(const render::Frame& frame);
    void draw(Recorder& recorder, const render::Frame& frame, uint32_t index);
    void drawPrecomputed(Recorder& recorder, const render::Frame& frame, uint32_t index);
    void setFixedState(ContextState& c, SceGxmContext* context, const render::Frame& frame, const render::DrawCall& d,
                       bool first);
    SceGxmTexture samplerTexture(const render::DrawState& s, uint32_t i) const;
    void upload(void* buffer, const render::UniformField* fields, uint8_t count,
                const std::vector<const SceGxmProgramParameter*>& parameters, const float* constants);
    // Command lists (setting gxm_lists): workers on the render core, core 3 and core 2 record the frame's scenes in
    // ranges; the immediate context executes the lists in order.
    void initLists();
    void shutdownLists();
    bool createWorker(Worker& worker);
    static int workerEntry(void* worker);
    static void* listMemory(FramePool& pool, SceSize requested, SceSize* size);
    static void* vdmMemory(void* worker, SceSize requested, SceSize* size);
    static void* vertexMemory(void* worker, SceSize requested, SceSize* size);
    static void* fragmentMemory(void* worker, SceSize requested, SceSize* size);
    bool recordLists(const render::Frame& frame);
    void runJobs(Worker& worker);
    bool recordJob(Worker& worker, const render::Frame& frame, const Job& job);

    SceGxmContext*          m_context = nullptr;
    SceGxmShaderPatcher*    m_patcher = nullptr;
    GpuHeap*                m_textureHeap = nullptr;
    GpuHeap*                m_bufferHeap = nullptr;
    Slot                    m_slots[Slots];
    uint32_t                m_lastIndex = 0;
    uint32_t                m_completed = 0;
    std::vector<Pending>    m_pending;
    SceUID                  m_sequenceUid = -1;
    uint16_t*               m_sequence = nullptr;       // 0..65535
    SceUID                  m_arenaUid = -1;
    Texture*                m_black = nullptr;          // read by samplers without a texture
    bool                    m_tiledTargets = true;      // setting tiled_targets (0: 2D targets of one level linear)

    VertexProgram           m_vertexPrograms[render::VpCount];
    FragmentProgram         m_fragmentPrograms[render::FpCount][Variants];
    std::unordered_map<uint32_t, VertexEntry>              m_vertexCache;
    std::unordered_map<uint64_t, FragmentEntry>            m_fragmentCache;
    std::map<uint32_t, SceGxmRenderTarget*>                 m_targets;
    std::vector<SceUID>                                    m_targetBlocks;

    // Render thread (and workers, read only while they record).
    std::atomic<bool>           m_precomputed{true};    // precomputed states and draws (setting gxm_precomputed)
    Recorder                    m_main;                 // the immediate context
    std::vector<DrawPrograms>   m_programs;             // per draw of the frame being drawn
    std::vector<uint8_t>        m_halfTarget;           // per draw: its scene's target stores half floats
    std::vector<ScenePlan>      m_plan;
    std::vector<uint32_t>       m_order;                // draw indices of the scenes, carried clears first
    int                         m_drawSort = 1;         // setting draw_sort
    bool                        m_trilinear = true;     // setting trilinear (0: the nearest level, filtered)
    std::vector<uint32_t>       m_held;                 // sortScene: alpha-tested draws not placed yet
    std::vector<uint32_t>       m_opaque;               // opaqueFirst: opaque draws not placed yet
    SortStats                   m_sortStats;

    // Command lists. m_workers[0] is the render thread's own deferred context.
    std::vector<Worker*>        m_workers;
    Mutex*                      m_jobLock = nullptr;
    CondVar*                    m_jobsDone = nullptr;
    Semaphore*                  m_wake = nullptr;
    std::vector<Job>            m_jobs;                 // guarded by m_jobLock while workers run
    const render::Frame*        m_jobFrame = nullptr;
    uint32_t                    m_nextJob = 0, m_doneJobs = 0;
    bool                        m_stop = false;
};

}}

#endif
