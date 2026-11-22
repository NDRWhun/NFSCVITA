// IDirect3DDevice9: the device state machine on the game thread. Draws and clears are recorded into the frame
// of platform/render.h; Present hands the frame to the device (render thread on the Vita).

#include <winapi/d3d9_internal.h>
#include <winapi/d3d9_direct.h>
#include <platform/config.h>
#include <platform/gxm.h>
#include <platform/log.h>
#include <platform/perf.h>
#include <platform/watchdog.h>
#include <algorithm>
#include <cmath>
#include <cstddef>

namespace win32 { namespace d3d9
{

namespace
{

pf::gxm::Device& gpu()
{
    return pf::gxm::Device::get();
}

// First words of the fixed state's fields.
enum : uint32_t
{
    StageStateWords = offsetof(FixedState, stageStates) / 4,
    SamplerStateWords = offsetof(FixedState, samplerStates) / 4,
    TransformWords = offsetof(FixedState, transforms) / 4,
    ViewportWords = offsetof(FixedState, viewport) / 4
};

// stamp_pairs (default 1): the constant stamps are written two registers per step with a single last one; 0: one
// loop over the registers.
bool stampPairs()
{
    static const bool s_pairs = pf::Config::getBool("stamp_pairs", true);
    return s_pairs;
}

void writeStamps(uint64_t* stamps, uint64_t stamp, uint32_t count)
{
    if (!stampPairs())
    {
        for (uint32_t i = 0; i < count; ++i)
            stamps[i] = stamp;
        return;
    }
    uint32_t i = 0;
#if defined(__clang__)
#pragma clang loop vectorize(disable)
#endif
    for (; i + 2 <= count; i += 2)
    {
        stamps[i] = stamp;
        stamps[i + 1] = stamp;
    }
    if (i < count)
        stamps[i] = stamp;
}

// The exponent of a ramp that is the same power curve in all three channels (within half an 8-bit step), else 0.
float rampExponent(const WORD ramp[3][256])
{
    if (memcmp(ramp[0], ramp[1], sizeof(ramp[0])) != 0 || memcmp(ramp[0], ramp[2], sizeof(ramp[0])) != 0
        || ramp[0][128] == 0 || ramp[0][128] == 0xffff)
        return 0.0f;
    const float exponent = logf(ramp[0][128] / 65535.0f) / logf(128.0f / 255.0f);
    if (!(exponent > 0.125f && exponent < 8.0f))
        return 0.0f;
    for (int i = 0; i < 256; ++i)
    {
        if (fabsf(powf(i / 255.0f, exponent) * 65535.0f - float(ramp[0][i])) > 128.0f)
            return 0.0f;
    }
    return exponent;
}

enum : DWORD
{
    D3DRS_ZENABLE = 7, D3DRS_ZWRITEENABLE = 14, D3DRS_ALPHATESTENABLE = 15, D3DRS_SRCBLEND = 19,
    D3DRS_DESTBLEND = 20, D3DRS_CULLMODE = 22, D3DRS_ZFUNC = 23, D3DRS_ALPHAREF = 24, D3DRS_ALPHAFUNC = 25,
    D3DRS_ALPHABLENDENABLE = 27, D3DRS_FOGENABLE = 28, D3DRS_TEXTUREFACTOR = 60, D3DRS_LIGHTING = 137,
    D3DRS_COLORWRITEENABLE = 168, D3DRS_BLENDOP = 171, D3DRS_BLENDFACTOR = 193,

    D3DTS_VIEW = 2, D3DTS_PROJECTION = 3, D3DTS_TEXTURE0 = 16, D3DTS_WORLD = 256,

    D3DSAMP_ADDRESSU = 1, D3DSAMP_ADDRESSV = 2, D3DSAMP_MAGFILTER = 5, D3DSAMP_MINFILTER = 6, D3DSAMP_MIPFILTER = 7,

    D3DCLEAR_TARGET = 1, D3DCLEAR_ZBUFFER = 2,

    D3DPT_TRIANGLELIST = 4, D3DPT_TRIANGLESTRIP = 5, D3DPT_TRIANGLEFAN = 6
};

static_assert(sizeof(D3DVIEWPORT9) % 4 == 0 && sizeof(render::Viewport) % 4 == 0, "viewports are whole words");

const D3DMATRIX s_identity = { { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 } };

// Perf builds: game thread time spent in the Draw* methods, logged every 300 frames. One draw in DrawSampleMask + 1
// is timed; the time per draw is the mean of the timed ones.
const uint32_t DrawSampleMask = 7;
uint64_t s_drawUs = 0;
uint32_t s_drawCalls = 0, s_drawTimed = 0, s_drawFrames = 0;

struct DrawTimer
{
    uint32_t start;
    DrawTimer()
        :   start(__builtin_expect(pf::perf::enabled(), 0) && (s_drawCalls++ & DrawSampleMask) == 0 ? pf::perf::now()
                                                                                                     : 0)
    {
    }
    ~DrawTimer()
    {
        if (__builtin_expect(start == 0, 1))
            return;
        s_drawUs += pf::perf::now() - start;
        ++s_drawTimed;
    }
};

void drawFrameDone()
{
    if (!pf::perf::enabled() || ++s_drawFrames < 300)
        return;
    const double perDraw = s_drawTimed ? double(s_drawUs) / double(s_drawTimed) : 0.0;
    pf::logInfo("d3d9: draws on the game thread: %u us/frame, %u draws/frame, %.2f us/draw (1 in %u timed)",
                uint32_t(perDraw * s_drawCalls / s_drawFrames), s_drawCalls / s_drawFrames, perDraw,
                unsigned(DrawSampleMask + 1));
    s_drawUs = 0;
    s_drawCalls = s_drawTimed = s_drawFrames = 0;
}

void color(DWORD argb, float out[4])
{
    out[0] = float((argb >> 16) & 255) / 255.0f;
    out[1] = float((argb >> 8) & 255) / 255.0f;
    out[2] = float(argb & 255) / 255.0f;
    out[3] = float(argb >> 24) / 255.0f;
}

// Inputs of the vertex shader by primitive count.
uint32_t vertexCount(DWORD type, UINT primitives)
{
    switch (type)
    {
    case D3DPT_TRIANGLELIST:  return primitives * 3;
    case D3DPT_TRIANGLESTRIP:
    case D3DPT_TRIANGLEFAN:   return primitives + 2;
    default:                  return 0;
    }
}

uint8_t primitive(DWORD type)
{
    return type == D3DPT_TRIANGLESTRIP ? render::PrimitiveStrip
         : type == D3DPT_TRIANGLEFAN ? render::PrimitiveFan : render::PrimitiveTriangles;
}

}

const render::TextureStorage* storageOf(Object* object)
{
    if (!object)
        return nullptr;
    if (object->kind() == KindVolumeTexture)
        return static_cast<VolumeTexture*>(object)->storage();
    if (object->kind() != KindTexture && object->kind() != KindCubeTexture)
        return nullptr;
    return static_cast<const Texture*>(object)->storage();
}

// ---- Construction --------------------------------------------------------------------------------------------------

Device::Device(WinApplication* app, Direct3D* d3d, const D3DPRESENT_PARAMETERS& params, HWND window, DWORD flags)
    :   Object(KindDevice)
    ,   m_app(app)
    ,   m_d3d(d3d)
    ,   m_params(params)
    ,   m_window(window)
    ,   m_flags(flags)
{
    d3d->addRef();
    memset(&m_state, 0, sizeof(m_state));
    // frame_rate=30 (default) shows each frame for two vblanks, 60 for one.
    m_vblanks = pf::Config::getInt("frame_rate", 30) >= 60 ? 1 : 2;
    for (int c = 0; c < 3; ++c)
        for (int i = 0; i < 256; ++i)
            m_gammaRamp[c][i] = WORD(i * 257);
    for (UINT& frequency : m_streamFrequency)
        frequency = 1;
    resetBackBuffer(params);
    defaults();
}

Device::~Device()
{
    hold(m_vertexShader, static_cast<Shader*>(nullptr));
    hold(m_pixelShader, static_cast<Shader*>(nullptr));
    hold(m_state.declaration, static_cast<VertexDeclaration*>(nullptr));
    hold(m_state.stream, static_cast<Buffer*>(nullptr));
    hold(m_state.indices, static_cast<Buffer*>(nullptr));
    hold(m_state.target, static_cast<Surface*>(nullptr));
    hold(m_state.depth, static_cast<Surface*>(nullptr));
    for (Object*& texture : m_state.textures)
        hold(texture, static_cast<Object*>(nullptr));
    for (VertexDeclaration* d : m_fvfDeclarations)
        delete d;
    delete m_backBuffer;
    delete m_autoDepth;
    m_d3d->release();
}

template< typename T >
void Device::hold(T*& slot, T* object)
{
    if (slot == object)
        return;
    if (object)
        object->addRef();
    if (slot)
        slot->release();
    slot = object;
}

void Device::resetBackBuffer(const D3DPRESENT_PARAMETERS& params)
{
    m_params = params;
    if (!m_params.BackBufferWidth || !m_params.BackBufferHeight)
    {
        m_params.BackBufferWidth = pf::gxm::DisplayWidth;
        m_params.BackBufferHeight = pf::gxm::DisplayHeight;
    }
    if (m_state.target == m_backBuffer)
        hold(m_state.target, static_cast<Surface*>(nullptr));
    if (m_state.depth == m_autoDepth)
        hold(m_state.depth, static_cast<Surface*>(nullptr));
    // The device's references; a surface the game still holds lives until its last Release.
    if (m_backBuffer)
        m_backBuffer->release();
    if (m_autoDepth)
        m_autoDepth->release();
    m_autoDepth = nullptr;
    const uint32_t w = m_params.BackBufferWidth, h = m_params.BackBufferHeight;
    m_backBuffer = new Surface(this, Surface::RoleBackBuffer, w, h, D3DFMT_A8R8G8B8);
    publish(m_app, m_backBuffer);
    if (m_params.EnableAutoDepthStencil)
    {
        m_autoDepth = new Surface(this, Surface::RoleDepth, w, h, m_params.AutoDepthStencilFormat);
        m_autoDepth->setDepth(gpu().createDepth(w, h));
        publish(m_app, m_autoDepth);
    }
    pf::logInfo("d3d9: back buffer %ux%u%s", w, h, m_autoDepth ? " with depth" : "");
}

void Device::defaults()
{
    DWORD* rs = m_state.renderStates;
    memset(rs, 0, sizeof(m_state.renderStates));
    rs[D3DRS_ZENABLE] = m_autoDepth ? 1 : 0;
    rs[8] = 3;                      // FILLMODE solid
    rs[9] = 2;                      // SHADEMODE gouraud
    rs[D3DRS_ZWRITEENABLE] = 1;
    rs[16] = 1;                     // LASTPIXEL
    rs[D3DRS_SRCBLEND] = 2;         // ONE
    rs[D3DRS_DESTBLEND] = 1;        // ZERO
    rs[D3DRS_CULLMODE] = 3;         // CCW
    rs[D3DRS_ZFUNC] = 4;            // LESSEQUAL
    rs[D3DRS_ALPHAFUNC] = 8;        // ALWAYS
    rs[D3DRS_TEXTUREFACTOR] = 0xffffffffu;
    rs[D3DRS_LIGHTING] = 1;
    rs[D3DRS_COLORWRITEENABLE] = 0xf;
    rs[D3DRS_BLENDOP] = 1;
    rs[D3DRS_BLENDFACTOR] = 0xffffffffu;
    for (DWORD s = 0; s < MaxStages; ++s)
    {
        DWORD* tss = m_state.stageStates[s];
        memset(tss, 0, sizeof(m_state.stageStates[s]));
        tss[D3DTSS_COLOROP] = s == 0 ? D3DTOP_MODULATE : D3DTOP_DISABLE;
        tss[D3DTSS_COLORARG1] = D3DTA_TEXTURE;
        tss[D3DTSS_COLORARG2] = D3DTA_CURRENT;
        tss[D3DTSS_ALPHAOP] = s == 0 ? D3DTOP_SELECTARG1 : D3DTOP_DISABLE;
        tss[D3DTSS_ALPHAARG1] = D3DTA_TEXTURE;
        tss[D3DTSS_ALPHAARG2] = D3DTA_CURRENT;
        tss[D3DTSS_TEXCOORDINDEX] = s;
    }
    for (DWORD s = 0; s < MaxSamplerStages; ++s)
    {
        DWORD* ss = m_state.samplerStates[s];
        memset(ss, 0, sizeof(m_state.samplerStates[s]));
        ss[D3DSAMP_ADDRESSU] = ss[D3DSAMP_ADDRESSV] = ss[3] = 1;   // WRAP
        ss[D3DSAMP_MAGFILTER] = ss[D3DSAMP_MINFILTER] = 1;         // POINT
        ss[10] = 1;                                                 // MAXANISOTROPY
    }
    for (D3DMATRIX& m : m_state.transforms)
        m = s_identity;
    hold(m_state.target, m_backBuffer);
    hold(m_state.depth, m_autoDepth);
    m_state.viewport = D3DVIEWPORT9{ 0, 0, m_params.BackBufferWidth, m_params.BackBufferHeight, 0.0f, 1.0f };
    logAllState();
    m_sceneOpen = false;
    m_stateDirty = true;
}

// ---- Recording -----------------------------------------------------------------------------------------------------

render::Frame* Device::frame(WinApplication* app, x86::CPU& cpu)
{
    if (!m_frame)
    {
        pf::perf::blocking(pf::perf::BlockSwap);
        const uint32_t start = pf::perf::enabled() ? pf::perf::now() : 0;
        app->unlockContext(cpu);
        m_frame = gpu().beginFrame(m_params.BackBufferWidth, m_params.BackBufferHeight);
        app->lockContext(cpu);
        if (pf::perf::enabled())
            pf::perf::displayWait(pf::perf::DisplaySlot, pf::perf::now() - start);
        m_sceneOpen = false;
        m_frameFull = false;
        m_stream = &frameStream(m_frame->index);
    }
    return m_frame;
}

// Hands the recorded frame to the device, with the gamma ramp it is shown through (8 bits a channel).
void Device::submit()
{
    m_frame->resolve = &resolveFrame;
    m_frame->commands = m_stream->data;
    m_frame->commandCount = m_stream->size;
    m_frame->gammaIdentity = m_gammaIdentity;
    m_frame->gammaExponent = m_gammaExponent;
    if (!m_gammaIdentity)
    {
        for (int c = 0; c < 3; ++c)
            for (int i = 0; i < 256; ++i)
                m_frame->gammaRamp[c][i] = uint8_t(m_gammaRamp[c][i] >> 8);
        // The ramp is applied when the back buffer is shown, so the back buffer is drawn offscreen.
        if (!gpu().backBufferTexture(m_frame))
            m_frame->gammaIdentity = true;
    }
    m_submitted = m_frame->index;
    const uint32_t start = pf::perf::enabled() ? pf::perf::now() : 0;
    gpu().submitFrame(m_frame, m_vblanks);
    if (pf::perf::enabled())
        pf::perf::displayWait(pf::perf::DisplaySubmit, pf::perf::now() - start);
    m_frame = nullptr;
    m_stream = nullptr;
}

void Device::record(uint32_t header, DWORD value)
{
    if (!m_stream)
    {
        m_stateDirty = true;
        return;
    }
    uint32_t* w = m_stream->append(2);
    w[0] = header;
    w[1] = value;
}

void Device::recordWords(uint32_t header, const void* payload, uint32_t words)
{
    if (!m_stream)
    {
        m_stateDirty = true;
        return;
    }
    uint32_t* w = m_stream->append(1 + words);
    w[0] = header;
    memcpy(w + 1, payload, words * 4);
}

bool Device::targetOf(Surface* surface, render::Target* target, uint16_t* width, uint16_t* height) const
{
    memset(target, 0, sizeof(*target));
    *width = uint16_t(m_params.BackBufferWidth);
    *height = uint16_t(m_params.BackBufferHeight);
    if (!surface || surface->role() == Surface::RoleBackBuffer)
        return true;
    if (surface->role() != Surface::RoleLevel || !surface->container() || !surface->container()->desc().target
        || !surface->container()->storage())
        return false;
    target->color = surface->container()->storage();
    target->face = uint8_t(surface->face());
    target->level = uint8_t(surface->level());
    *width = uint16_t(surface->width());
    *height = uint16_t(surface->height());
    return true;
}

render::Scene* Device::scene()
{
    render::Target target;
    uint16_t width, height;
    targetOf(m_state.target, &target, &width, &height);
    if (m_state.depth && m_state.depth->depth())
        target.depth = m_state.depth->depth();
    return scene(target, width, height);
}

render::Scene* Device::scene(const render::Target& target, uint16_t width, uint16_t height)
{
    render::Frame& f = *m_frame;
    if (m_sceneOpen && f.sceneCount)
    {
        render::Scene& last = f.scenes[f.sceneCount - 1];
        if (last.target.color == target.color && last.target.face == target.face
            && last.target.level == target.level && last.target.depth == target.depth)
            return &last;
    }
    if (f.sceneCount == f.sceneCapacity)
    {
        if (!m_frameFull)
            pf::logError("d3d9: frame %u: scene limit reached, draws dropped", f.index);
        m_frameFull = true;
        return nullptr;
    }
    render::Scene& s = f.scenes[f.sceneCount++];
    memset(&s, 0, sizeof(s));
    s.target = target;
    s.width = width;
    s.height = height;
    s.firstDraw = f.drawCount;
    if (target.color)
        const_cast<render::TextureStorage*>(target.color)->lastUse = f.index;
    if (target.depth)
        const_cast<render::DepthStorage*>(target.depth)->lastUse = f.index;
    m_sceneOpen = true;
    return &s;
}

uint16_t Device::viewportIndex(render::Frame& f, const render::Viewport& viewport)
{
    if (f.viewportCount && sameWords(&f.viewports[f.viewportCount - 1], &viewport, sizeof(viewport) / 4))
        return uint16_t(f.viewportCount - 1);
    if (f.viewportCount == f.viewportCapacity)
        return uint16_t(f.viewportCount - 1);
    f.viewports[f.viewportCount] = viewport;
    return uint16_t(f.viewportCount++);
}

// The state resolution of a draw happens on the render thread (d3d9_record.cpp); the game thread records the
// geometry, the textures as they are now (marking them read by this frame) and the active effect pass.
HRESULT Device::draw(WinApplication* app, x86::CPU& cpu, DWORD type, UINT primitives, const void* vertices,
                     const uint16_t* indices, uint32_t count, uint8_t layout, uint32_t texComponents, bool positionT,
                     render::BufferStorage* vertexStorage, render::BufferStorage* indexStorage)
{
    const uint32_t inputs = vertexCount(type, primitives);
    if (!inputs)
    {
        if (type < D3DPT_TRIANGLELIST || type > D3DPT_TRIANGLEFAN)
            D3D_LOG_ONCE("d3d9: primitive type %u not drawn", type);
        return type >= 1 && type <= 6 ? D3D_OK : D3DERR_INVALIDCALL;
    }
    if (m_effectPass && !m_effectPass->drawable)
    {
        m_effectPass->dropped();
        return D3D_OK;
    }
    if (!m_effectPass && (m_vertexShader || m_pixelShader))
    {
        // Shader objects have no program of their own: the draw is dropped.
        Shader* s = m_vertexShader ? m_vertexShader : m_pixelShader;
        if (s->logDrop())
            pf::logInfo("d3d9: draws with shader object 0x%08x (%u bytes of bytecode) are dropped: no program",
                        s->guest(), uint32_t(s->code().size()));
        return D3D_OK;
    }
    render::Frame* f = frame(app, cpu);
    if (m_frameFull)
        return D3D_OK;
    render::Scene* s = scene();
    if (!s)
        return D3D_OK;
    if (f->drawCount == f->drawCapacity)
    {
        pf::logError("d3d9: frame %u: draw limit reached, draws dropped", f->index);
        m_frameFull = true;
        return D3D_OK;
    }
    if (m_stateDirty)
    {
        recordWords(CmdFixedState, static_cast<const FixedState*>(&m_state), sizeof(FixedState) / 4);
        m_stateDirty = false;
    }
    if (m_constantsDirty)
    {
        for (int pixel = 0; pixel < 2; ++pixel)
        {
            const uint32_t registers = pixel ? PixelFloatConstants : VertexFloatConstants;
            uint32_t* w = m_stream->append(2 + 4 * registers);
            w[0] = CmdConstants | uint32_t(pixel) << 8;
            w[1] = registers;
            memcpy(w + 2, pixel ? m_psFloat : m_vsFloat, 16 * registers);
        }
        m_constantsDirty = false;
    }

    render::DrawCall& d = f->draws[f->drawCount];
    d.vertices = vertices;
    d.indices = indices;
    d.count = inputs;
    d.vertexCount = count;
    const D3DVIEWPORT9& vp = m_state.viewport;
    d.viewport = viewportIndex(*f, render::Viewport{ float(vp.X), float(vp.Y), float(vp.Width), float(vp.Height),
                                                     vp.MinZ, vp.MaxZ });

    uint32_t* w = m_stream->append(1 + sizeof(DrawRecord) / 4);
    w[0] = CmdDraw;
    DrawRecord& r = *reinterpret_cast<DrawRecord*>(w + 1);
    r.draw = f->drawCount;
    r.primitive = primitive(type);
    r.layout = layout;
    r.texComponents = uint8_t(texComponents);
    r.positionT = positionT ? 1 : 0;
    r.vertexStorage = vertexStorage;
    r.effect = m_effectPass ? m_effectPass->programs : EffectDraw{ 0, 0, NoEffect, 0 };
    // Effect programs sample the stages of their sampler registers; fixed function reads stage 0.
    const uint32_t stages = r.effect.kind != NoEffect ? render::MaxSamplers : 1;
    for (uint32_t i = 0; i < render::MaxSamplers; ++i)
        r.textures[i] = i < stages ? storageOf(m_state.textures[i]) : nullptr;
    for (const render::TextureStorage* t : r.textures)
    {
        if (t)
            const_cast<render::TextureStorage*>(t)->lastUse = f->index;
    }
    if (vertexStorage)
        vertexStorage->lastUse = f->index;
    if (indexStorage)
        indexStorage->lastUse = f->index;
    m_lastDraw = f->drawCount;
#if NFSC_RENDER_CHECK
    checkConstants();
#endif
    ++m_drawSerial;
    ++f->drawCount;
    s->drawCount = f->drawCount - s->firstDraw;
    return D3D_OK;
}

bool Device::nativeGeometry(DWORD type, UINT vertices, UINT startIndex, UINT count, NativeGeometry& out)
{
    Buffer* vb = m_state.stream;
    Buffer* ib = m_state.indices;
    VertexDeclaration* decl = m_state.declaration ? m_state.declaration
                            : m_state.fvf ? fvfDeclaration(m_state.fvf) : nullptr;
    if (!vb || !ib || !decl || !m_state.streamStride)
        return false;
    // As DrawIndexedPrimitive with base vertex and minimum index 0.
    const UINT first = m_state.streamOffset;
    vb->prepare(first, first + vertices * m_state.streamStride);
    ib->prepare(startIndex * 2, (startIndex + vertexCount(type, count)) * 2);
    out.call.vertices = static_cast<const uint8_t*>(vb->storage()->data) + m_state.streamOffset;
    out.call.indices = static_cast<const uint16_t*>(ib->storage()->data) + startIndex;
    out.call.count = vertexCount(type, count);
    out.call.vertexCount = vertices;
    out.layout = decl->layout(m_state.streamStride);
    out.primitive = primitive(type);
    out.vertexStorage = vb->storage();
    out.indexStorage = ib->storage();
    return true;
}

void Device::nativeDraw(WinApplication* app, x86::CPU& cpu, const NativeGeometry& geometry,
                        const render::TextureStorage* const* textures, const NativeDraw& draw, NativeBlocks blocks,
                        const void* context)
{
    if (!geometry.call.count)
    {
        m_nativeFrame = 0;
        return;
    }
    render::Frame* f = frame(app, cpu);
    if (m_frameFull)
        return;
    render::Scene* s = scene();
    if (!s)
        return;
    if (f->drawCount == f->drawCapacity)
    {
        pf::logError("d3d9: frame %u: draw limit reached, draws dropped", f->index);
        m_frameFull = true;
        return;
    }
    uint32_t vertexOffset = render::NoConstants, fragmentOffset = render::NoConstants;
    if (draw.reuse && m_nativeFrame == f->index)
    {
        vertexOffset = m_nativeVertex;
        fragmentOffset = m_nativeFragment;
    }
    else
    {
        float* vc = nullptr;
        float* fc = nullptr;
        if ((draw.vertexFloats && !(vc = render::allocateConstants(*f, draw.vertexFloats, &vertexOffset)))
            || (draw.fragmentFloats && !(fc = render::allocateConstants(*f, draw.fragmentFloats, &fragmentOffset))))
        {
            pf::logError("d3d9: frame %u: constant arena full, draws dropped", f->index);
            m_frameFull = true;
            return;
        }
        if (blocks)
        {
            blocks(context, vc, fc);
        }
        else
        {
            if (vc)
                memcpy(vc, draw.vertexConstants, 4 * draw.vertexFloats);
            if (fc)
                memcpy(fc, draw.fragmentConstants, 4 * draw.fragmentFloats);
        }
        m_nativeFrame = f->index;
        m_nativeVertex = vertexOffset;
        m_nativeFragment = fragmentOffset;
    }
    render::DrawCall& d = f->draws[f->drawCount];
    memset(&d.state, 0, sizeof(d.state));
    d.vertexConstants = vertexOffset;
    d.fragmentConstants = fragmentOffset;
    d.vertices = geometry.call.vertices;
    d.indices = geometry.call.indices;
    d.count = geometry.call.count;
    d.vertexCount = geometry.call.vertexCount;
    const D3DVIEWPORT9& vp = m_state.viewport;
    d.viewport = viewportIndex(*f, render::Viewport{ float(vp.X), float(vp.Y), float(vp.Width), float(vp.Height),
                                                     vp.MinZ, vp.MaxZ });
    uint32_t* w = m_stream->append(1 + sizeof(NativeDrawRecord) / 4);
    w[0] = CmdNativeDraw;
    NativeDrawRecord& r = *reinterpret_cast<NativeDrawRecord*>(w + 1);
    r.draw = f->drawCount;
    r.state = draw.state;
    r.test[0] = draw.test[0];
    r.test[1] = draw.test[1];
    r.blendOp = draw.blendOp;
    r.blendFactor = draw.blendFactor;
    r.vertexStorage = geometry.vertexStorage;
    for (uint32_t i = 0; i < render::MaxSamplers; ++i)
    {
        if (textures[i])
            const_cast<render::TextureStorage*>(textures[i])->lastUse = f->index;
    }
    geometry.vertexStorage->lastUse = f->index;
    geometry.indexStorage->lastUse = f->index;
    m_lastDraw = f->drawCount;
    ++m_drawSerial;
    ++f->drawCount;
    s->drawCount = f->drawCount - s->firstDraw;
}

void Device::recordNativeCheck(uint32_t draw, const NativeGeometry& geometry, const NativeDraw& native)
{
    if (!m_stream)
        return;
    const uint32_t words = sizeof(NativeCheckRecord) / 4;
    uint32_t* w = m_stream->append(1 + words + native.vertexFloats + native.fragmentFloats);
    w[0] = CmdNativeCheck;
    NativeCheckRecord& r = *reinterpret_cast<NativeCheckRecord*>(w + 1);
    r.native.draw = draw;
    r.native.state = native.state;
    r.native.test[0] = native.test[0];
    r.native.test[1] = native.test[1];
    r.native.blendOp = native.blendOp;
    r.native.blendFactor = native.blendFactor;
    r.native.vertexStorage = geometry.vertexStorage;
    r.vertices = geometry.call.vertices;
    r.indices = geometry.call.indices;
    r.count = geometry.call.count;
    r.vertexCount = geometry.call.vertexCount;
    r.vertexFloats = native.vertexFloats;
    r.fragmentFloats = native.fragmentFloats;
    if (native.vertexFloats)
        memcpy(w + 1 + words, native.vertexConstants, 4 * native.vertexFloats);
    if (native.fragmentFloats)
        memcpy(w + 1 + words + native.vertexFloats, native.fragmentConstants, 4 * native.fragmentFloats);
}

VertexDeclaration* Device::fvfDeclaration(DWORD fvf)
{
    for (VertexDeclaration* d : m_fvfDeclarations)
    {
        if (d->fvf() == fvf)
            return d;
    }
    VertexDeclaration* d = new VertexDeclaration(this, nullptr, 0, fvf);
    m_fvfDeclarations.push_back(d);
    return d;
}

// ---- Methods -------------------------------------------------------------------------------------------------------

HRESULT Device::TestCooperativeLevel(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    RC_USE(cpu);
    return D3D_OK;
}

UINT Device::GetAvailableTextureMem(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    RC_USE(cpu);
    return 96 * 1024 * 1024;
}

HRESULT Device::EvictManagedResources(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    RC_USE(cpu);
    return D3D_OK;
}

HRESULT Device::GetDirect3D(WinApplication* app, x86::CPU& cpu, x86::reg32* d3d)
{
    RC_USE(app);
    RC_USE(cpu);
    m_d3d->addRef();
    *d3d = m_d3d->guest();
    return D3D_OK;
}

HRESULT Device::GetDeviceCaps(WinApplication* app, x86::CPU& cpu, D3DCAPS9* caps)
{
    RC_USE(app);
    RC_USE(cpu);
    if (!caps)
        return D3DERR_INVALIDCALL;
    Direct3D::fillCaps(caps);
    return D3D_OK;
}

HRESULT Device::GetDisplayMode(WinApplication* app, x86::CPU& cpu, UINT swapChain, D3DDISPLAYMODE* mode)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(swapChain);
    if (!mode)
        return D3DERR_INVALIDCALL;
    *mode = D3DDISPLAYMODE{ m_params.BackBufferWidth, m_params.BackBufferHeight, 60, D3DFMT_X8R8G8B8 };
    return D3D_OK;
}

HRESULT Device::GetCreationParameters(WinApplication* app, x86::CPU& cpu, D3DDEVICE_CREATION_PARAMETERS* params)
{
    RC_USE(app);
    RC_USE(cpu);
    if (!params)
        return D3DERR_INVALIDCALL;
    *params = D3DDEVICE_CREATION_PARAMETERS{ 0, 1, m_window, m_flags };
    return D3D_OK;
}

HRESULT Device::SetCursorProperties(WinApplication* app, x86::CPU& cpu, UINT x, UINT y, x86::reg32 surface)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(x);
    RC_USE(y);
    RC_USE(surface);
    return D3D_OK;
}

void Device::SetCursorPosition(WinApplication* app, x86::CPU& cpu, int x, int y, DWORD flags)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(x);
    RC_USE(y);
    RC_USE(flags);
}

BOOL Device::ShowCursor(WinApplication* app, x86::CPU& cpu, BOOL show)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(show);
    return 0;
}

UINT Device::GetNumberOfSwapChains(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    RC_USE(cpu);
    return 1;
}

HRESULT Device::Reset(WinApplication* app, x86::CPU& cpu, D3DPRESENT_PARAMETERS* params)
{
    if (!params)
        return D3DERR_INVALIDCALL;
    if (m_frame)
        submit();
    RC_USE(app);
    RC_USE(cpu);
    resetBackBuffer(*params);
    for (Object*& texture : m_state.textures)
        hold(texture, static_cast<Object*>(nullptr));
    hold(m_state.stream, static_cast<Buffer*>(nullptr));
    hold(m_state.indices, static_cast<Buffer*>(nullptr));
    hold(m_state.declaration, static_cast<VertexDeclaration*>(nullptr));
    hold(m_vertexShader, static_cast<Shader*>(nullptr));
    hold(m_pixelShader, static_cast<Shader*>(nullptr));
    for (UINT& frequency : m_streamFrequency)
        frequency = 1;
    defaults();
    return D3D_OK;
}

HRESULT Device::Present(WinApplication* app, x86::CPU& cpu, const RECT* source, const RECT* dest, HWND window,
                        x86::reg32 dirtyRegion)
{
    RC_USE(window);
    RC_USE(dirtyRegion);
    if (source || dest)
        D3D_LOG_ONCE("d3d9: Present rectangles ignored");
    // A frame without draws still shows (black), so the pacing stays the same.
    frame(app, cpu);
    submit();
    m_sceneOpen = false;
    drawFrameDone();
    pf::watchdog::progress();
    return D3D_OK;
}

HRESULT Device::GetBackBuffer(WinApplication* app, x86::CPU& cpu, UINT swapChain, UINT index, DWORD type,
                              x86::reg32* surface)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(type);
    if (swapChain != 0 || index != 0 || !surface)
        return D3DERR_INVALIDCALL;
    m_backBuffer->addRef();
    *surface = m_backBuffer->guest();
    return D3D_OK;
}

HRESULT Device::GetRasterStatus(WinApplication* app, x86::CPU& cpu, UINT swapChain, DWORD* status)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(swapChain);
    if (status)
    {
        status[0] = 0;
        status[1] = 0;
    }
    return D3D_OK;
}

void Device::SetDialogBoxMode(WinApplication* app, x86::CPU& cpu, BOOL enable)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(enable);
}

// The ramp (red, green, blue: 256 words each) applies to the frames presented from now on (D3DSGR_NO_CALIBRATION).
void Device::SetGammaRamp(WinApplication* app, x86::CPU& cpu, UINT swapChain, DWORD flags, const WORD* ramp)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(flags);
    if (swapChain != 0 || !ramp || memcmp(m_gammaRamp, ramp, sizeof(m_gammaRamp)) == 0)
        return;
    memcpy(m_gammaRamp, ramp, sizeof(m_gammaRamp));
    m_gammaIdentity = true;
    for (int c = 0; c < 3 && m_gammaIdentity; ++c)
        for (int i = 0; i < 256 && m_gammaIdentity; ++i)
            m_gammaIdentity = (m_gammaRamp[c][i] >> 8) == i;
    m_gammaExponent = rampExponent(m_gammaRamp);
}

void Device::GetGammaRamp(WinApplication* app, x86::CPU& cpu, UINT swapChain, WORD* ramp)
{
    RC_USE(app);
    RC_USE(cpu);
    if (swapChain != 0 || !ramp)
        return;
    memcpy(ramp, m_gammaRamp, sizeof(m_gammaRamp));
}

namespace
{

// Direct3D format to texture format for a usage; FormatNone when not supported (CheckDeviceFormat agrees).
texture::Format textureFormat(DWORD format, DWORD usage)
{
    if (usage & D3DUSAGE_DEPTHSTENCIL)
    {
        // Depth textures: a 32-bit float depth surface for D24S8, D24X8 and D16 (no stencil plane).
        const bool depth = format == D3DFMT_D24S8 || format == D3DFMT_D24X8 || format == D3DFMT_D16;
        return depth && !(usage & D3DUSAGE_RENDERTARGET) ? texture::FormatDepth : texture::FormatNone;
    }
    switch (format)
    {
    case D3DFMT_A8R8G8B8: return texture::FormatARGB8888;
    case D3DFMT_X8R8G8B8:
        D3D_LOG_ONCE("d3d9: X8R8G8B8 textures are kept as A8R8G8B8");
        return texture::FormatARGB8888;
    case D3DFMT_DXT1:     return usage & D3DUSAGE_RENDERTARGET ? texture::FormatNone : texture::FormatBC1;
    case D3DFMT_DXT3:     return usage & D3DUSAGE_RENDERTARGET ? texture::FormatNone : texture::FormatBC2;
    case D3DFMT_DXT5:     return usage & D3DUSAGE_RENDERTARGET ? texture::FormatNone : texture::FormatBC3;
    case D3DFMT_A16B16G16R16F:
        // Render targets only, stored as half floats: the precision of the Direct3D format.
        return usage & D3DUSAGE_RENDERTARGET ? texture::FormatRGBA16F : texture::FormatNone;
    default:              return texture::FormatNone;
    }
}

// The shadow map colour and depth textures (0x00715150) are stored ShadowMapSize square instead of 1024 (a deliberate
// platform change); they report 1024 and are drawn at that size, scaled.
const UINT ShadowMapSize = 128;
const x86::reg32 ShadowColorReturn = 0x00715174, ShadowDepthReturn = 0x007151A6;

uint32_t fullChain(uint32_t width, uint32_t height)
{
    uint32_t levels = 1;
    while ((width >> levels) || (height >> levels))
        ++levels;
    return levels;
}

bool powerOfTwo(uint32_t v)
{
    return v && !(v & (v - 1));
}

// The car environment cube and its face depth surfaces (0x0070DE00) are stored envmap_size square instead of 256;
// they report 256 and are drawn at that size, scaled.
const UINT EnvironmentCubeSize = 256;
const x86::reg32 EnvironmentCubeReturn = 0x0070DE86, EnvironmentDepthReturn = 0x0070DED1;

uint16_t environmentCubeStored()
{
    static const int s_size = pf::Config::getInt("envmap_size", 128);
    return uint16_t(s_size >= 16 && s_size <= int(EnvironmentCubeSize) && powerOfTwo(uint32_t(s_size)) ? s_size : 128);
}

}

HRESULT Device::CreateTexture(WinApplication* app, x86::CPU& cpu, UINT width, UINT height, UINT levels, DWORD usage,
                              DWORD format, DWORD pool, x86::reg32* texture, x86::reg32 sharedHandle)
{
    RC_USE(sharedHandle);
    const pf::perf::TextureTimer timer(pf::perf::TextureCreate);
    const texture::Format f = textureFormat(format, usage);
    const bool target = (usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL)) != 0;
    if (!texture || !width || !height || width > 4096 || height > 4096 || f == texture::FormatNone)
    {
        pf::logError("d3d9: CreateTexture %ux%u format 0x%x usage 0x%x not supported", width, height, format, usage);
        return D3DERR_INVALIDCALL;
    }
    const uint32_t chain = fullChain(width, height);
    render::TextureDesc desc;
    desc.format = f;
    desc.levels = uint8_t(usage & D3DUSAGE_DEPTHSTENCIL ? 1 : (levels == 0 || levels > chain ? chain : levels));
    desc.cube = false;
    desc.target = target;
    desc.width = uint16_t(width);
    desc.height = uint16_t(height);
    if (target && desc.levels > 1 && (f != texture::FormatARGB8888 || !powerOfTwo(width) || !powerOfTwo(height)))
    {
        // Render targets with levels are swizzled A8R8G8B8 colour surfaces.
        pf::logError("d3d9: CreateTexture %ux%u format 0x%x render target with %u levels not supported", width, height,
                     format, levels);
        return D3DERR_INVALIDCALL;
    }
    if (!target && (!powerOfTwo(width) || !powerOfTwo(height)))
    {
        // D3DPTEXTURECAPS_NONPOW2CONDITIONAL: one uncompressed level, stored in linear rows.
        if (texture::isBlock(f) || (levels != 1 && !(levels == 0 && chain == 1)))
        {
            pf::logError("d3d9: CreateTexture %ux%u format 0x%x with %u levels: not a power of two", width, height,
                         format, levels);
            return D3DERR_INVALIDCALL;
        }
        desc.levels = 1;
        desc.linear = true;
    }
    const x86::reg32 caller = app->getMemory<x86::reg32>(cpu.esp);
    if ((caller == ShadowColorReturn || caller == ShadowDepthReturn) && width == 1024 && height == 1024)
        desc.width = desc.height = uint16_t(ShadowMapSize);
    Texture* t = new Texture(this, desc, format, usage, pool, width, height);
    if (!t->valid())
    {
        pf::logError("d3d9: out of texture memory for %ux%u format 0x%x", width, height, format);
        delete t;
        return D3DERR_OUTOFVIDEOMEMORY;
    }
    *texture = publish(app, t);
    return D3D_OK;
}

HRESULT Device::CreateCubeTexture(WinApplication* app, x86::CPU& cpu, UINT edge, UINT levels, DWORD usage,
                                  DWORD format, DWORD pool, x86::reg32* texture, x86::reg32 sharedHandle)
{
    RC_USE(sharedHandle);
    const texture::Format f = textureFormat(format, usage);
    const bool target = (usage & D3DUSAGE_RENDERTARGET) != 0;
    if (!texture || !powerOfTwo(edge) || edge > 4096 || f == texture::FormatNone || f == texture::FormatDepth
        || (target && f != texture::FormatARGB8888))
    {
        pf::logError("d3d9: CreateCubeTexture %u format 0x%x not supported", edge, format);
        return D3DERR_INVALIDCALL;
    }
    const uint32_t chain = fullChain(edge, edge);
    render::TextureDesc desc;
    desc.format = f;
    desc.levels = uint8_t(levels == 0 || levels > chain ? chain : levels);
    desc.cube = true;
    desc.target = target;
    desc.width = desc.height = uint16_t(edge);
    if (target && edge == EnvironmentCubeSize && levels == 1
        && app->getMemory<x86::reg32>(cpu.esp) == EnvironmentCubeReturn)
        desc.width = desc.height = environmentCubeStored();
    Texture* t = new Texture(this, desc, format, usage, pool, edge, edge);
    if (!t->valid())
    {
        delete t;
        return D3DERR_OUTOFVIDEOMEMORY;
    }
    *texture = publish(app, t);
    return D3D_OK;
}

HRESULT Device::CreateVolumeTexture(WinApplication* app, x86::CPU& cpu, UINT width, UINT height, UINT depth,
                                    UINT levels, DWORD usage, DWORD format, DWORD pool, x86::reg32* texture,
                                    x86::reg32 sharedHandle)
{
    RC_USE(cpu);
    RC_USE(sharedHandle);
    bool block;
    if (!texture || !width || !height || !depth || !formatBits(format, &block) || (usage & D3DUSAGE_RENDERTARGET))
    {
        pf::logError("d3d9: CreateVolumeTexture %ux%ux%u format 0x%x usage 0x%x not supported", width, height, depth,
                     format, usage);
        return D3DERR_INVALIDCALL;
    }
    uint32_t chain = 1;
    for (UINT size = std::max(width, std::max(height, depth)); size > 1; size >>= 1)
        ++chain;
    VolumeTexture* t = new VolumeTexture(this, width, height, depth, levels == 0 || levels > chain ? chain : levels,
                                         format, usage, pool);
    *texture = publish(app, t);
    return D3D_OK;
}

HRESULT Device::CreateVertexBuffer(WinApplication* app, x86::CPU& cpu, UINT length, DWORD usage, DWORD fvf,
                                   DWORD pool, x86::reg32* buffer, x86::reg32 sharedHandle)
{
    RC_USE(cpu);
    RC_USE(sharedHandle);
    if (!buffer || !length)
    {
        pf::logError("d3d9: CreateVertexBuffer of %u bytes refused", length);
        return D3DERR_INVALIDCALL;
    }
    Buffer* b = new Buffer(this, KindVertexBuffer, length, usage, pool, fvf);
    if (!b->valid())
    {
        pf::logError("d3d9: CreateVertexBuffer of %u bytes: out of video memory", length);
        delete b;
        return D3DERR_OUTOFVIDEOMEMORY;
    }
    *buffer = publish(app, b);
    return D3D_OK;
}

HRESULT Device::CreateIndexBuffer(WinApplication* app, x86::CPU& cpu, UINT length, DWORD usage, DWORD format,
                                  DWORD pool, x86::reg32* buffer, x86::reg32 sharedHandle)
{
    RC_USE(cpu);
    RC_USE(sharedHandle);
    if (!buffer || !length)
    {
        pf::logError("d3d9: CreateIndexBuffer of %u bytes refused", length);
        return D3DERR_INVALIDCALL;
    }
    if (format != D3DFMT_INDEX16)
    {
        pf::logError("d3d9: index format %u not supported", format);
        return D3DERR_INVALIDCALL;
    }
    Buffer* b = new Buffer(this, KindIndexBuffer, length, usage, pool, format);
    if (!b->valid())
    {
        pf::logError("d3d9: CreateIndexBuffer of %u bytes: out of video memory", length);
        delete b;
        return D3DERR_OUTOFVIDEOMEMORY;
    }
    *buffer = publish(app, b);
    return D3D_OK;
}

HRESULT Device::CreateRenderTarget(WinApplication* app, x86::CPU& cpu, UINT width, UINT height, DWORD format,
                                   DWORD multiSample, DWORD quality, BOOL lockable, x86::reg32* surface,
                                   x86::reg32 sharedHandle)
{
    RC_USE(multiSample);
    RC_USE(quality);
    RC_USE(lockable);
    if (!surface)
        return D3DERR_INVALIDCALL;
    // A render target texture whose only reference is its surface.
    x86::reg32 texture = 0;
    const HRESULT result = CreateTexture(app, cpu, width, height, 1, D3DUSAGE_RENDERTARGET, format, 0, &texture,
                                         sharedHandle);
    if (result != D3D_OK)
        return result;
    Texture* t = static_cast<Texture*>(lookup(texture));
    *surface = t->surface(app, 0, 0)->guest();
    t->release();
    return D3D_OK;
}

HRESULT Device::CreateDepthStencilSurface(WinApplication* app, x86::CPU& cpu, UINT width, UINT height, DWORD format,
                                          DWORD multiSample, DWORD quality, BOOL discard, x86::reg32* surface,
                                          x86::reg32 sharedHandle)
{
    RC_USE(multiSample);
    RC_USE(quality);
    RC_USE(discard);
    RC_USE(sharedHandle);
    if (!surface || !width || !height)
        return D3DERR_INVALIDCALL;
    const bool cubeDepth = width == EnvironmentCubeSize && height == EnvironmentCubeSize
                        && app->getMemory<x86::reg32>(cpu.esp) == EnvironmentDepthReturn;
    render::DepthStorage* depth = cubeDepth ? gpu().createDepth(environmentCubeStored(), environmentCubeStored())
                                            : gpu().createDepth(width, height);
    if (!depth)
        return D3DERR_OUTOFVIDEOMEMORY;
    Surface* s = new Surface(this, Surface::RoleDepth, width, height, format);
    s->setDepth(depth);
    *surface = publish(app, s);
    return D3D_OK;
}

HRESULT Device::SetRenderTarget(WinApplication* app, x86::CPU& cpu, DWORD index, Surface* surface)
{
    RC_USE(app);
    RC_USE(cpu);
    if (index != 0)
    {
        D3D_LOG_ONCE("d3d9: render target %u ignored", index);
        return index < 4 && !surface ? D3D_OK : D3DERR_INVALIDCALL;
    }
    if (!surface || surface->role() == Surface::RoleDepth
        || (surface->role() == Surface::RoleLevel && (!surface->container() || !surface->container()->desc().target)))
        return D3DERR_INVALIDCALL;
    hold(m_state.target, surface);
    // A new render target resets the viewport to the whole target.
    m_state.viewport = D3DVIEWPORT9{ 0, 0, surface->width(), surface->height(), 0.0f, 1.0f };
    logState(ViewportWords, sizeof(D3DVIEWPORT9) / 4);
    recordWords(CmdViewport, &m_state.viewport, sizeof(D3DVIEWPORT9) / 4);
    return D3D_OK;
}

HRESULT Device::GetRenderTarget(WinApplication* app, x86::CPU& cpu, DWORD index, x86::reg32* surface)
{
    RC_USE(app);
    RC_USE(cpu);
    if (!surface)
        return D3DERR_INVALIDCALL;
    if (index != 0 || !m_state.target)
    {
        *surface = 0;
        return D3DERR_NOTFOUND;
    }
    m_state.target->addRef();
    *surface = m_state.target->guest();
    return D3D_OK;
}

HRESULT Device::SetDepthStencilSurface(WinApplication* app, x86::CPU& cpu, Surface* surface)
{
    RC_USE(app);
    RC_USE(cpu);
    if (surface && surface->role() != Surface::RoleDepth)
        return D3DERR_INVALIDCALL;
    hold(m_state.depth, surface);
    return D3D_OK;
}

HRESULT Device::GetDepthStencilSurface(WinApplication* app, x86::CPU& cpu, x86::reg32* surface)
{
    RC_USE(app);
    RC_USE(cpu);
    if (!surface)
        return D3DERR_INVALIDCALL;
    if (!m_state.depth)
    {
        *surface = 0;
        return D3DERR_NOTFOUND;
    }
    m_state.depth->addRef();
    *surface = m_state.depth->guest();
    return D3D_OK;
}

HRESULT Device::BeginScene(WinApplication* app, x86::CPU& cpu)
{
    frame(app, cpu);
    return D3D_OK;
}

HRESULT Device::EndScene(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    RC_USE(cpu);
    return D3D_OK;
}

HRESULT Device::Clear(WinApplication* app, x86::CPU& cpu, DWORD count, const D3DRECT* rects, DWORD flags,
                      DWORD argb, float z, DWORD stencil)
{
    RC_USE(stencil);
    render::Frame* f = frame(app, cpu);
    if (m_frameFull)
        return D3D_OK;
    render::Scene* s = scene();
    if (!s)
        return D3D_OK;
    if (!s->target.depth)
        flags &= ~D3DCLEAR_ZBUFFER;
    const D3DVIEWPORT9& vp = m_state.viewport;
    const LONG vx0 = LONG(vp.X), vy0 = LONG(vp.Y);
    const LONG vx1 = std::min<LONG>(vx0 + LONG(vp.Width), s->width), vy1 = std::min<LONG>(vy0 + LONG(vp.Height), s->height);
    const uint32_t rectCount = rects && count ? count : 1;
    for (uint32_t i = 0; i < rectCount; ++i)
    {
        LONG x0 = vx0, y0 = vy0, x1 = vx1, y1 = vy1;
        if (rects && count)
        {
            x0 = std::max(x0, rects[i].x1);
            y0 = std::max(y0, rects[i].y1);
            x1 = std::min(x1, rects[i].x2);
            y1 = std::min(y1, rects[i].y2);
        }
        if (x0 >= x1 || y0 >= y1)
            continue;
        DWORD what = flags;
        // A whole depth clear before the scene's first draw is the scene's starting depth.
        if ((what & D3DCLEAR_ZBUFFER) && s->drawCount == 0 && x0 == 0 && y0 == 0 && x1 == s->width && y1 == s->height)
        {
            s->depthCleared = true;
            s->clearDepth = z;
            what &= ~D3DCLEAR_ZBUFFER;
        }
        if (!(what & (D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER)))
            continue;
        if (f->drawCount == f->drawCapacity)
        {
            m_frameFull = true;
            return D3D_OK;
        }
        render::DrawCall& d = f->draws[f->drawCount];
        memset(&d, 0, sizeof(d));
        render::VertexLayout layout;
        memset(&layout, 0, sizeof(layout));
        layout.stride = 12;
        layout.count = 1;
        layout.elements[0] = render::VertexElement{ render::UsagePosition, render::TypeFloat3, 0 };
        d.state.layout = render::registerLayout(layout);
        d.state.vertexProgram = render::VpClear;
        d.state.fragmentProgram = render::FpClear;
        d.state.primitive = render::PrimitiveFan;
        d.state.colorMask = (what & D3DCLEAR_TARGET) ? 0xf : 0;
        d.state.depthEnable = d.state.depthWrite = (what & D3DCLEAR_ZBUFFER) ? 1 : 0;
        d.state.depthFunc = 8;
        d.state.cullMode = 1;
        float* v = static_cast<float*>(render::allocateVertices(*f, 48));
        float* c = render::allocateConstants(*f, sizeof(render::ClearFragmentConstants) / 4, &d.fragmentConstants);
        if (!v || !c)
        {
            m_frameFull = true;
            return D3D_OK;
        }
        // Pixel edges (centres at integers) in clip space of the whole target.
        const float sx = 2.0f / s->width, sy = 2.0f / s->height;
        const float l = (float(x0) - 0.5f) * sx - 1.0f, r = (float(x1) - 0.5f) * sx - 1.0f;
        const float t = 1.0f - (float(y0) - 0.5f) * sy, b = 1.0f - (float(y1) - 0.5f) * sy;
        const float quad[12] = { l, t, z, r, t, z, r, b, z, l, b, z };
        memcpy(v, quad, sizeof(quad));
        memset(c, 0, sizeof(render::ClearFragmentConstants));
        color(argb, reinterpret_cast<render::ClearFragmentConstants*>(c)->color);
        d.vertices = v;
        d.count = 4;
        d.vertexCount = 4;
        d.vertexConstants = render::NoConstants;
        d.viewport = viewportIndex(*f, render::Viewport{ 0, 0, float(s->width), float(s->height), 0.0f, 1.0f });
        ++f->drawCount;
        s->drawCount = f->drawCount - s->firstDraw;
    }
    return D3D_OK;
}

HRESULT Device::quad(render::Frame& f, render::Scene& s, const RECT& rect, DWORD argb,
                     const render::TextureStorage* texture, const float uv[4], bool linear, uint32_t level)
{
    if (f.drawCount == f.drawCapacity)
    {
        m_frameFull = true;
        return D3D_OK;
    }
    render::DrawCall& d = f.draws[f.drawCount];
    memset(&d, 0, sizeof(d));
    d.state.primitive = render::PrimitiveFan;
    d.state.colorMask = 0xf;
    d.state.depthFunc = 8;
    d.state.cullMode = 1;
    d.count = d.vertexCount = 4;
    d.viewport = viewportIndex(f, render::Viewport{ 0, 0, float(s.width), float(s.height), 0.0f, 1.0f });
    // Pixel edges: Direct3D window coordinates have pixel centres at integers.
    const float x0 = float(rect.left) - 0.5f, x1 = float(rect.right) - 0.5f;
    const float y0 = float(rect.top) - 0.5f, y1 = float(rect.bottom) - 0.5f;
    render::VertexLayout layout;
    memset(&layout, 0, sizeof(layout));
    if (!texture)
    {
        layout.stride = 12;
        layout.count = 1;
        layout.elements[0] = render::VertexElement{ render::UsagePosition, render::TypeFloat3, 0 };
        d.state.layout = render::registerLayout(layout);
        d.state.vertexProgram = render::VpClear;
        d.state.fragmentProgram = render::FpClear;
        float* v = static_cast<float*>(render::allocateVertices(f, 48));
        float* c = render::allocateConstants(f, sizeof(render::ClearFragmentConstants) / 4, &d.fragmentConstants);
        if (!v || !c)
        {
            m_frameFull = true;
            return D3D_OK;
        }
        const float sx = 2.0f / s.width, sy = 2.0f / s.height;
        const float l = x0 * sx - 1.0f, r = x1 * sx - 1.0f, t = 1.0f - y0 * sy, b = 1.0f - y1 * sy;
        const float corners[12] = { l, t, 0.0f, r, t, 0.0f, r, b, 0.0f, l, b, 0.0f };
        memcpy(v, corners, sizeof(corners));
        memset(c, 0, sizeof(render::ClearFragmentConstants));
        color(argb, reinterpret_cast<render::ClearFragmentConstants*>(c)->color);
        d.vertices = v;
        d.vertexConstants = render::NoConstants;
    }
    else
    {
        // A copy: clip-space vertices and the screen pass-through programs (the texel as it is).
        layout.stride = 24;
        layout.count = 2;
        layout.elements[0] = render::VertexElement{ render::UsagePosition, render::TypeFloat4, 0 };
        layout.elements[1] = render::VertexElement{ render::UsageTex0, render::TypeFloat2, 16 };
        d.state.layout = render::registerLayout(layout);
        d.state.vertexProgram = render::VpScreen;
        d.state.fragmentProgram = render::FpPassthru;
        d.state.textures[0] = texture;
        d.state.samplers[0] = render::Sampler{ render::AddressClamp, render::AddressClamp,
                                               uint8_t(linear ? 3 : 0), render::MipNone, 0, uint8_t(level) };
        struct Vertex
        {
            float x, y, z, w;
            float u, v;
        };
        static_assert(sizeof(Vertex) == 24, "copy vertex");
        Vertex* v = static_cast<Vertex*>(render::allocateVertices(f, 4 * sizeof(Vertex)));
        if (!v)
        {
            m_frameFull = true;
            return D3D_OK;
        }
        const float sx = 2.0f / s.width, sy = 2.0f / s.height;
        const float l = x0 * sx - 1.0f, r = x1 * sx - 1.0f, t = 1.0f - y0 * sy, b = 1.0f - y1 * sy;
        v[0] = Vertex{ l, t, 0.0f, 1.0f, uv[0], uv[1] };
        v[1] = Vertex{ r, t, 0.0f, 1.0f, uv[2], uv[1] };
        v[2] = Vertex{ r, b, 0.0f, 1.0f, uv[2], uv[3] };
        v[3] = Vertex{ l, b, 0.0f, 1.0f, uv[0], uv[3] };
        d.vertexConstants = render::NoConstants;
        d.fragmentConstants = render::NoConstants;
        d.vertices = v;
        const_cast<render::TextureStorage*>(texture)->lastUse = f.index;
    }
    ++f.drawCount;
    s.drawCount = f.drawCount - s.firstDraw;
    return D3D_OK;
}

// Return address of the final pass's second scene copy (0x00729770), read only by the rain drops 0x00722CB0 draws
// over it, and the global that points to the number of drops.
const x86::reg32 RainCopyReturn = 0x00729852, RainDropCount = 0x00B4AFFC;

// A GPU copy: a quad over the destination rectangle sampling the source rectangle (point sampled unless the filter
// is LINEAR and the sizes differ). Sources are render target or texture levels (not cube faces) and the back buffer;
// destinations are render target levels and the back buffer. Depth surfaces are not copied.
HRESULT Device::StretchRect(WinApplication* app, x86::CPU& cpu, Surface* source, const RECT* sourceRect, Surface* dest,
                            const RECT* destRect, DWORD filter)
{
    // The rain copy is skipped when no drop will be drawn.
    if (app->getMemory<x86::reg32>(cpu.esp) == RainCopyReturn)
    {
        const x86::reg32 drops = app->getMemory<x86::reg32>(RainDropCount);
        if (drops && app->getMemory<x86::reg32>(drops) == 0)
            return D3D_OK;
    }
    render::Target target;
    uint16_t dw, dh;
    if (!source || !dest || source == dest || !targetOf(dest, &target, &dw, &dh)
        || dest->role() == Surface::RoleDepth || source->role() == Surface::RoleDepth)
        return D3DERR_INVALIDCALL;
    uint32_t sw = m_params.BackBufferWidth, sh = m_params.BackBufferHeight;
    if (source->role() == Surface::RoleLevel)
    {
        const Texture* t = source->container();
        if (!t || !t->storage() || t->desc().cube || (t->storage() == target.color && source->level() == target.level))
        {
            D3D_LOG_ONCE("d3d9: StretchRect from a cube face or its own target refused");
            return D3DERR_INVALIDCALL;
        }
        sw = source->width();
        sh = source->height();
    }
    const RECT sr = sourceRect ? *sourceRect : RECT{ 0, 0, LONG(sw), LONG(sh) };
    const RECT dr = destRect ? *destRect : RECT{ 0, 0, LONG(dw), LONG(dh) };
    if (sr.left < 0 || sr.top < 0 || sr.right > LONG(sw) || sr.bottom > LONG(sh) || sr.left >= sr.right
        || sr.top >= sr.bottom || dr.left < 0 || dr.top < 0 || dr.right > LONG(dw) || dr.bottom > LONG(dh)
        || dr.left >= dr.right || dr.top >= dr.bottom)
        return D3DERR_INVALIDCALL;
    render::Frame* f = frame(app, cpu);
    if (m_frameFull)
        return D3D_OK;
    const render::TextureStorage* texture = source->role() == Surface::RoleLevel ? source->container()->storage()
                                                                               : gpu().backBufferTexture(f);
    if (!texture)
        return D3DERR_OUTOFVIDEOMEMORY;
    render::Scene* s = scene(target, dw, dh);
    if (!s)
        return D3D_OK;
    const float uv[4] = { float(sr.left) / float(sw), float(sr.top) / float(sh), float(sr.right) / float(sw),
                          float(sr.bottom) / float(sh) };
    const bool scaled = sr.right - sr.left != dr.right - dr.left || sr.bottom - sr.top != dr.bottom - dr.top;
    const uint32_t level = source->role() == Surface::RoleLevel ? source->level() : 0;
    return quad(*f, *s, dr, 0, texture, uv, scaled && filter == 2, level);
}

// Render targets and the back buffer are filled on the GPU; A8R8G8B8 texture levels through a lock.
HRESULT Device::ColorFill(WinApplication* app, x86::CPU& cpu, Surface* surface, const RECT* rect, DWORD argb)
{
    if (!surface || surface->role() == Surface::RoleDepth)
        return D3DERR_INVALIDCALL;
    const RECT full = { 0, 0, LONG(surface->width()), LONG(surface->height()) };
    const RECT r = rect ? *rect : full;
    if (r.left < 0 || r.top < 0 || r.right > full.right || r.bottom > full.bottom || r.left >= r.right
        || r.top >= r.bottom)
        return D3DERR_INVALIDCALL;
    render::Target target;
    uint16_t w, h;
    if (targetOf(surface, &target, &w, &h))
    {
        render::Frame* f = frame(app, cpu);
        if (m_frameFull)
            return D3D_OK;
        render::Scene* s = scene(target, w, h);
        return s ? quad(*f, *s, r, argb, nullptr, nullptr, false, 0) : D3D_OK;
    }
    const Texture* t = surface->container();
    if (!t || t->desc().format != texture::FormatARGB8888)
        return D3DERR_INVALIDCALL;
    D3DLOCKED_RECT locked;
    if (surface->LockRect(app, cpu, &locked, &r, 0) != D3D_OK)
        return D3DERR_INVALIDCALL;
    for (LONG y = 0; y < r.bottom - r.top; ++y)
    {
        uint8_t* row = g_guestBase + locked.pBits + uint32_t(y) * uint32_t(locked.Pitch);
        for (LONG x = 0; x < r.right - r.left; ++x)
            memcpy(row + 4 * x, &argb, 4);
    }
    return surface->UnlockRect(app, cpu);
}

HRESULT Device::SetTransform(WinApplication* app, x86::CPU& cpu, DWORD state, const D3DMATRIX* matrix)
{
    RC_USE(app);
    RC_USE(cpu);
    D3D9_TRACE(direct::TraceTransform, state, 0, 0, 0, 0, 0, matrix ? matrix->m : nullptr, 16);
    if (!matrix)
        return D3DERR_INVALIDCALL;
    uint32_t t;
    switch (state)
    {
    case D3DTS_WORLD:       t = TransformWorld; break;
    case D3DTS_VIEW:        t = TransformView; break;
    case D3DTS_PROJECTION:  t = TransformProjection; break;
    case D3DTS_TEXTURE0:    t = TransformTexture0; break;
    default:                D3D_LOG_ONCE("d3d9: transform %u ignored", state); return D3D_OK;
    }
    if (sameWords(&m_state.transforms[t], matrix, sizeof(D3DMATRIX) / 4))
        return D3D_OK;
    m_state.transforms[t] = *matrix;
    logState(TransformWords + 16 * t, 16);
    recordWords(CmdTransform | t << 8, matrix, 16);
    return D3D_OK;
}

HRESULT Device::GetTransform(WinApplication* app, x86::CPU& cpu, DWORD state, D3DMATRIX* matrix)
{
    RC_USE(app);
    RC_USE(cpu);
    if (!matrix)
        return D3DERR_INVALIDCALL;
    switch (state)
    {
    case D3DTS_WORLD:       *matrix = m_state.transforms[TransformWorld]; break;
    case D3DTS_VIEW:        *matrix = m_state.transforms[TransformView]; break;
    case D3DTS_PROJECTION:  *matrix = m_state.transforms[TransformProjection]; break;
    case D3DTS_TEXTURE0:    *matrix = m_state.transforms[TransformTexture0]; break;
    default:                *matrix = s_identity; break;
    }
    return D3D_OK;
}

HRESULT Device::MultiplyTransform(WinApplication* app, x86::CPU& cpu, DWORD state, const D3DMATRIX* matrix)
{
    if (!matrix)
        return D3DERR_INVALIDCALL;
    D3DMATRIX current;
    GetTransform(app, cpu, state, &current);
    D3DMATRIX product;
    multiply(matrix->m, current.m, product.m);
    return SetTransform(app, cpu, state, &product);
}

HRESULT Device::SetViewport(WinApplication* app, x86::CPU& cpu, const D3DVIEWPORT9* viewport)
{
    RC_USE(app);
    RC_USE(cpu);
    if (!viewport)
        return D3DERR_INVALIDCALL;
    if (sameWords(&m_state.viewport, viewport, sizeof(D3DVIEWPORT9) / 4))
        return D3D_OK;
    m_state.viewport = *viewport;
    logState(ViewportWords, sizeof(D3DVIEWPORT9) / 4);
    recordWords(CmdViewport, viewport, sizeof(D3DVIEWPORT9) / 4);
    return D3D_OK;
}

HRESULT Device::GetViewport(WinApplication* app, x86::CPU& cpu, D3DVIEWPORT9* viewport)
{
    RC_USE(app);
    RC_USE(cpu);
    if (!viewport)
        return D3DERR_INVALIDCALL;
    *viewport = m_state.viewport;
    return D3D_OK;
}

HRESULT Device::SetMaterial(WinApplication* app, x86::CPU& cpu, const DWORD* material)
{
    RC_USE(app);
    RC_USE(cpu);
    if (!material)
        return D3DERR_INVALIDCALL;
    memcpy(m_material, material, sizeof(m_material));
    return D3D_OK;
}

HRESULT Device::GetMaterial(WinApplication* app, x86::CPU& cpu, DWORD* material)
{
    RC_USE(app);
    RC_USE(cpu);
    if (!material)
        return D3DERR_INVALIDCALL;
    memcpy(material, m_material, sizeof(m_material));
    return D3D_OK;
}

HRESULT Device::SetLight(WinApplication* app, x86::CPU& cpu, DWORD index, const DWORD* light)
{
    RC_USE(app);
    RC_USE(cpu);
    if (!light || index >= 8)
        return D3DERR_INVALIDCALL;
    memcpy(m_lights[index], light, sizeof(m_lights[index]));
    return D3D_OK;
}

HRESULT Device::GetLight(WinApplication* app, x86::CPU& cpu, DWORD index, DWORD* light)
{
    RC_USE(app);
    RC_USE(cpu);
    if (!light || index >= 8)
        return D3DERR_INVALIDCALL;
    memcpy(light, m_lights[index], sizeof(m_lights[index]));
    return D3D_OK;
}

HRESULT Device::LightEnable(WinApplication* app, x86::CPU& cpu, DWORD index, BOOL enable)
{
    RC_USE(app);
    RC_USE(cpu);
    if (index >= 8)
        return D3DERR_INVALIDCALL;
    m_lightEnabled[index] = enable;
    return D3D_OK;
}

HRESULT Device::GetLightEnable(WinApplication* app, x86::CPU& cpu, DWORD index, BOOL* enable)
{
    RC_USE(app);
    RC_USE(cpu);
    if (index >= 8 || !enable)
        return D3DERR_INVALIDCALL;
    *enable = m_lightEnabled[index] ? 128 : 0;
    return D3D_OK;
}

HRESULT Device::SetClipPlane(WinApplication* app, x86::CPU& cpu, DWORD index, const float* plane)
{
    RC_USE(app);
    RC_USE(cpu);
    if (index >= 6 || !plane)
        return D3DERR_INVALIDCALL;
    memcpy(m_clipPlanes[index], plane, sizeof(m_clipPlanes[index]));
    D3D_LOG_ONCE("d3d9: user clip planes are stored, not applied");
    return D3D_OK;
}

HRESULT Device::GetClipPlane(WinApplication* app, x86::CPU& cpu, DWORD index, float* plane)
{
    RC_USE(app);
    RC_USE(cpu);
    if (index >= 6 || !plane)
        return D3DERR_INVALIDCALL;
    memcpy(plane, m_clipPlanes[index], sizeof(m_clipPlanes[index]));
    return D3D_OK;
}

// D3DCLIPSTATUS9: ClipUnion, ClipIntersection.
HRESULT Device::SetClipStatus(WinApplication* app, x86::CPU& cpu, const DWORD* status)
{
    RC_USE(app);
    RC_USE(cpu);
    if (!status)
        return D3DERR_INVALIDCALL;
    m_clipStatus[0] = status[0];
    m_clipStatus[1] = status[1];
    return D3D_OK;
}

HRESULT Device::GetClipStatus(WinApplication* app, x86::CPU& cpu, DWORD* status)
{
    RC_USE(app);
    RC_USE(cpu);
    if (!status)
        return D3DERR_INVALIDCALL;
    status[0] = m_clipStatus[0];
    status[1] = m_clipStatus[1];
    return D3D_OK;
}

HRESULT Device::SetRenderState(WinApplication* app, x86::CPU& cpu, DWORD state, DWORD value)
{
    RC_USE(app);
    RC_USE(cpu);
    D3D9_TRACE(direct::TraceRenderState, state, value);
    if (state >= MaxRenderStates)
        return D3DERR_INVALIDCALL;
    if (m_state.renderStates[state] == value)
        return D3D_OK;
    m_state.renderStates[state] = value;
    logState(state, 1);
    record(CmdRenderState | state << 8, value);
    return D3D_OK;
}

HRESULT Device::GetRenderState(WinApplication* app, x86::CPU& cpu, DWORD state, DWORD* value)
{
    RC_USE(app);
    RC_USE(cpu);
    if (state >= MaxRenderStates || !value)
        return D3DERR_INVALIDCALL;
    *value = m_state.renderStates[state];
    return D3D_OK;
}

HRESULT Device::GetTexture(WinApplication* app, x86::CPU& cpu, DWORD stage, x86::reg32* texture)
{
    RC_USE(app);
    RC_USE(cpu);
    if (stage >= MaxSamplerStages || !texture)
        return D3DERR_INVALIDCALL;
    Object* t = m_state.textures[stage];
    if (t)
        t->addRef();
    *texture = t ? t->guest() : 0;
    return D3D_OK;
}

HRESULT Device::SetTexture(WinApplication* app, x86::CPU& cpu, DWORD stage, Object* texture)
{
    RC_USE(app);
    RC_USE(cpu);
    D3D9_TRACE(direct::TraceDeviceTexture, stage, uint32_t(uintptr_t(texture)));
    if (stage >= MaxSamplerStages)
    {
        D3D_LOG_ONCE("d3d9: texture stage %u ignored", stage);
        return D3D_OK;
    }
    if (texture && texture->kind() != KindTexture && texture->kind() != KindCubeTexture
        && texture->kind() != KindVolumeTexture)
        return D3DERR_INVALIDCALL;
    hold(m_state.textures[stage], texture);
    return D3D_OK;
}

HRESULT Device::GetTextureStageState(WinApplication* app, x86::CPU& cpu, DWORD stage, DWORD type, DWORD* value)
{
    RC_USE(app);
    RC_USE(cpu);
    if (stage >= MaxStages || type >= D3DTSS_COUNT || !value)
        return D3DERR_INVALIDCALL;
    *value = m_state.stageStates[stage][type];
    return D3D_OK;
}

HRESULT Device::SetTextureStageState(WinApplication* app, x86::CPU& cpu, DWORD stage, DWORD type, DWORD value)
{
    RC_USE(app);
    RC_USE(cpu);
    if (stage >= MaxStages || type >= D3DTSS_COUNT)
        return D3DERR_INVALIDCALL;
    if (m_state.stageStates[stage][type] == value)
        return D3D_OK;
    m_state.stageStates[stage][type] = value;
    logState(StageStateWords + stage * D3DTSS_COUNT + type, 1);
    record(CmdStageState | stage << 8 | type << 16, value);
    return D3D_OK;
}

HRESULT Device::GetSamplerState(WinApplication* app, x86::CPU& cpu, DWORD sampler, DWORD type, DWORD* value)
{
    RC_USE(app);
    RC_USE(cpu);
    if (sampler >= MaxSamplerStages || type >= MaxSamplerStates || !value)
        return D3DERR_INVALIDCALL;
    *value = m_state.samplerStates[sampler][type];
    return D3D_OK;
}

HRESULT Device::SetSamplerState(WinApplication* app, x86::CPU& cpu, DWORD sampler, DWORD type, DWORD value)
{
    RC_USE(app);
    RC_USE(cpu);
    D3D9_TRACE(direct::TraceSamplerState, sampler, type, value);
    if (sampler >= MaxSamplerStages || type >= MaxSamplerStates)
        return D3D_OK;
    if (m_state.samplerStates[sampler][type] == value)
        return D3D_OK;
    m_state.samplerStates[sampler][type] = value;
    logState(SamplerStateWords + sampler * MaxSamplerStates + type, 1);
    record(CmdSamplerState | sampler << 8 | type << 16, value);
    return D3D_OK;
}

HRESULT Device::ValidateDevice(WinApplication* app, x86::CPU& cpu, DWORD* passes)
{
    RC_USE(app);
    RC_USE(cpu);
    if (passes)
        *passes = 1;
    return D3D_OK;
}

// Palettes for P8 textures: stored (256 entries each); P8 textures are not created, the game expands them.
HRESULT Device::SetPaletteEntries(WinApplication* app, x86::CPU& cpu, UINT palette, const DWORD* entries)
{
    RC_USE(app);
    RC_USE(cpu);
    if (palette > 0xffff || !entries)
        return D3DERR_INVALIDCALL;
    if (palette >= m_palettes.size())
        m_palettes.resize(palette + 1);
    m_palettes[palette].assign(entries, entries + 256);
    return D3D_OK;
}

HRESULT Device::GetPaletteEntries(WinApplication* app, x86::CPU& cpu, UINT palette, DWORD* entries)
{
    RC_USE(app);
    RC_USE(cpu);
    if (palette >= m_palettes.size() || m_palettes[palette].empty() || !entries)
        return D3DERR_INVALIDCALL;
    memcpy(entries, m_palettes[palette].data(), 256 * sizeof(DWORD));
    return D3D_OK;
}

HRESULT Device::SetCurrentTexturePalette(WinApplication* app, x86::CPU& cpu, UINT palette)
{
    RC_USE(app);
    RC_USE(cpu);
    if (palette >= m_palettes.size() || m_palettes[palette].empty())
        return D3DERR_INVALIDCALL;
    m_currentPalette = palette;
    return D3D_OK;
}

HRESULT Device::GetCurrentTexturePalette(WinApplication* app, x86::CPU& cpu, UINT* palette)
{
    RC_USE(app);
    RC_USE(cpu);
    if (!palette)
        return D3DERR_INVALIDCALL;
    *palette = m_currentPalette;
    return D3D_OK;
}

HRESULT Device::SetScissorRect(WinApplication* app, x86::CPU& cpu, const RECT* rect)
{
    RC_USE(app);
    RC_USE(cpu);
    if (!rect)
        return D3DERR_INVALIDCALL;
    m_scissor = *rect;
    D3D_LOG_ONCE("d3d9: scissor rectangles are stored, not applied");
    return D3D_OK;
}

HRESULT Device::GetScissorRect(WinApplication* app, x86::CPU& cpu, RECT* rect)
{
    RC_USE(app);
    RC_USE(cpu);
    if (!rect)
        return D3DERR_INVALIDCALL;
    *rect = m_scissor;
    return D3D_OK;
}

HRESULT Device::SetSoftwareVertexProcessing(WinApplication* app, x86::CPU& cpu, BOOL software)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(software);
    return D3D_OK;
}

BOOL Device::GetSoftwareVertexProcessing(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    RC_USE(cpu);
    return 0;
}

// No N-patch caps (MaxNpatchTessellationLevel 0): the segment count is stored, patches are never tessellated.
HRESULT Device::SetNPatchMode(WinApplication* app, x86::CPU& cpu, float segments)
{
    RC_USE(app);
    RC_USE(cpu);
    m_nPatchSegments = segments;
    return D3D_OK;
}

HRESULT Device::DrawPrimitive(WinApplication* app, x86::CPU& cpu, DWORD type, UINT start, UINT count)
{
    const DrawTimer timer;
    Buffer* vb = m_state.stream;
    VertexDeclaration* decl = m_state.declaration ? m_state.declaration
                            : m_state.fvf ? fvfDeclaration(m_state.fvf) : nullptr;
    if (!vb || !decl || !m_state.streamStride)
        return D3DERR_INVALIDCALL;
    const uint32_t vertices = vertexCount(type, count);
    const UINT first = m_state.streamOffset + start * m_state.streamStride;
    vb->prepare(first, first + vertices * m_state.streamStride);
    const uint8_t* data = static_cast<const uint8_t*>(vb->storage()->data) + m_state.streamOffset
                        + size_t(start) * m_state.streamStride;
    return draw(app, cpu, type, count, data, nullptr, vertices, decl->layout(m_state.streamStride),
                decl->texComponents(), decl->positionT(), vb->storage(), nullptr);
}

HRESULT Device::DrawIndexedPrimitive(WinApplication* app, x86::CPU& cpu, DWORD type, LONG baseVertex, UINT minIndex,
                                     UINT vertices, UINT startIndex, UINT count)
{
    D3D9_TRACE(direct::TraceDrawIndexed, type, uint32_t(baseVertex), minIndex, vertices, startIndex, count);
    const DrawTimer timer;
    Buffer* vb = m_state.stream;
    Buffer* ib = m_state.indices;
    VertexDeclaration* decl = m_state.declaration ? m_state.declaration
                            : m_state.fvf ? fvfDeclaration(m_state.fvf) : nullptr;
    if (!vb || !ib || !decl || !m_state.streamStride)
        return D3DERR_INVALIDCALL;
    // The vertices the indices may reference (MinVertexIndex, NumVertices) and the indices drawn.
    const ptrdiff_t lowest = ptrdiff_t(m_state.streamOffset)
                           + (ptrdiff_t(baseVertex) + ptrdiff_t(minIndex)) * ptrdiff_t(m_state.streamStride);
    const UINT first = lowest > 0 ? UINT(lowest) : 0;
    vb->prepare(first, first + vertices * m_state.streamStride);
    ib->prepare(startIndex * 2, (startIndex + vertexCount(type, count)) * 2);
    const uint8_t* data = static_cast<const uint8_t*>(vb->storage()->data) + m_state.streamOffset
                        + ptrdiff_t(baseVertex) * ptrdiff_t(m_state.streamStride);
    const uint16_t* indices = static_cast<const uint16_t*>(ib->storage()->data) + startIndex;
    return draw(app, cpu, type, count, data, indices, minIndex + vertices, decl->layout(m_state.streamStride),
                decl->texComponents(), decl->positionT(), vb->storage(), ib->storage());
}

HRESULT Device::DrawPrimitiveUP(WinApplication* app, x86::CPU& cpu, DWORD type, UINT count, const void* data,
                                UINT stride)
{
    const DrawTimer timer;
    VertexDeclaration* decl = m_state.declaration ? m_state.declaration
                            : m_state.fvf ? fvfDeclaration(m_state.fvf) : nullptr;
    // A user-pointer draw unbinds stream 0.
    hold(m_state.stream, static_cast<Buffer*>(nullptr));
    m_state.streamOffset = m_state.streamStride = 0;
    if (!decl || !data || !stride)
        return D3DERR_INVALIDCALL;
    const uint32_t vertices = vertexCount(type, count);
    render::Frame* f = frame(app, cpu);
    void* copy = render::allocateVertices(*f, vertices * stride);
    if (!copy)
    {
        if (!m_frameFull)
            pf::logError("d3d9: frame %u: vertex arena full, draws dropped", f->index);
        m_frameFull = true;
        return D3D_OK;
    }
    memcpy(copy, data, size_t(vertices) * stride);
    return draw(app, cpu, type, count, copy, nullptr, vertices, decl->layout(stride), decl->texComponents(),
                decl->positionT(), nullptr, nullptr);
}

HRESULT Device::DrawIndexedPrimitiveUP(WinApplication* app, x86::CPU& cpu, DWORD type, UINT minIndex, UINT vertices,
                                       UINT count, const void* indices, DWORD indexFormat, const void* data,
                                       UINT stride)
{
    const DrawTimer timer;
    VertexDeclaration* decl = m_state.declaration ? m_state.declaration
                            : m_state.fvf ? fvfDeclaration(m_state.fvf) : nullptr;
    hold(m_state.stream, static_cast<Buffer*>(nullptr));
    hold(m_state.indices, static_cast<Buffer*>(nullptr));
    m_state.streamOffset = m_state.streamStride = 0;
    if (!decl || !data || !indices || !stride || indexFormat != D3DFMT_INDEX16)
        return D3DERR_INVALIDCALL;
    const uint32_t inputs = vertexCount(type, count);
    const uint32_t reach = minIndex + vertices;
    render::Frame* f = frame(app, cpu);
    void* vcopy = render::allocateVertices(*f, reach * stride);
    void* icopy = render::allocateVertices(*f, inputs * 2);
    if (!vcopy || !icopy)
    {
        m_frameFull = true;
        return D3D_OK;
    }
    memcpy(vcopy, data, size_t(reach) * stride);
    memcpy(icopy, indices, size_t(inputs) * 2);
    return draw(app, cpu, type, count, vcopy, static_cast<const uint16_t*>(icopy), reach, decl->layout(stride),
                decl->texComponents(), decl->positionT(), nullptr, nullptr);
}

HRESULT Device::CreateVertexDeclaration(WinApplication* app, x86::CPU& cpu, const D3DVERTEXELEMENT9* elements,
                                        x86::reg32* declaration)
{
    RC_USE(cpu);
    if (!elements || !declaration)
        return D3DERR_INVALIDCALL;
    uint32_t count = 0;
    while (count < 64 && elements[count].Stream != 0xFF)
        ++count;
    *declaration = publish(app, new VertexDeclaration(this, elements, count, 0));
    return D3D_OK;
}

HRESULT Device::SetVertexDeclaration(WinApplication* app, x86::CPU& cpu, VertexDeclaration* declaration)
{
    RC_USE(app);
    RC_USE(cpu);
    D3D9_TRACE(direct::TraceDeclaration, uint32_t(uintptr_t(declaration)));
    hold(m_state.declaration, declaration);
    m_state.fvf = 0;
    return D3D_OK;
}

HRESULT Device::GetVertexDeclaration(WinApplication* app, x86::CPU& cpu, x86::reg32* declaration)
{
    RC_USE(app);
    RC_USE(cpu);
    if (!declaration)
        return D3DERR_INVALIDCALL;
    if (m_state.declaration)
        m_state.declaration->addRef();
    *declaration = m_state.declaration ? m_state.declaration->guest() : 0;
    return D3D_OK;
}

HRESULT Device::SetFVF(WinApplication* app, x86::CPU& cpu, DWORD fvf)
{
    RC_USE(app);
    RC_USE(cpu);
    hold(m_state.declaration, static_cast<VertexDeclaration*>(nullptr));
    m_state.fvf = fvf;
    return D3D_OK;
}

HRESULT Device::GetFVF(WinApplication* app, x86::CPU& cpu, DWORD* fvf)
{
    RC_USE(app);
    RC_USE(cpu);
    if (!fvf)
        return D3DERR_INVALIDCALL;
    *fvf = m_state.fvf;
    return D3D_OK;
}

namespace
{

// Copies constant registers in or out of a bank of count registers of width values; false out of range.
template< typename T >
bool constants(T* bank, UINT registers, UINT width, UINT start, UINT count, const T* in, T* out)
{
    if (start > registers || count > registers - start || (count && !(in || out)))
        return false;
    if (in)
        memcpy(bank + size_t(start) * width, in, size_t(count) * width * sizeof(T));
    else
        memcpy(out, bank + size_t(start) * width, size_t(count) * width * sizeof(T));
    return true;
}

}

HRESULT Device::CreateVertexShader(WinApplication* app, x86::CPU& cpu, x86::reg32 function, x86::reg32* shader)
{
    RC_USE(cpu);
    if (!function || !shader)
        return D3DERR_INVALIDCALL;
    const uint8_t* code = g_guestBase + function;
    const uint32_t bytes = Shader::length(code, 1024 * 1024);
    uint32_t version;
    memcpy(&version, code, 4);
    if (!bytes || (version >> 16) != 0xfffe || (version & 0xffff) > (0xFFFE0200 & 0xffff))
        return D3DERR_INVALIDCALL;
    *shader = publish(app, new Shader(this, KindVertexShader, code, bytes));
    return D3D_OK;
}

HRESULT Device::SetVertexShader(WinApplication* app, x86::CPU& cpu, Object* shader)
{
    RC_USE(app);
    RC_USE(cpu);
    if (shader && shader->kind() != KindVertexShader)
        return D3DERR_INVALIDCALL;
    hold(m_vertexShader, static_cast<Shader*>(shader));
    return D3D_OK;
}

HRESULT Device::GetVertexShader(WinApplication* app, x86::CPU& cpu, x86::reg32* shader)
{
    RC_USE(app);
    RC_USE(cpu);
    if (!shader)
        return D3DERR_INVALIDCALL;
    if (m_vertexShader)
        m_vertexShader->addRef();
    *shader = m_vertexShader ? m_vertexShader->guest() : 0;
    return D3D_OK;
}

uint64_t Device::setConstants(bool pixel, uint32_t first, const float* values, uint32_t count)
{
    float* bank = pixel ? m_psFloat : m_vsFloat;
    const uint32_t registers = pixel ? PixelFloatConstants : VertexFloatConstants;
    if (first > registers || count > registers - first || (count && !values))
        return 0;
    const uint64_t stamp = ++m_constantStamp;
#if NFSC_RENDER_CHECK
    checkWrite(pixel, first, values, count, false);
#endif
    m_constantLog[m_constantWrites++ % WriteLogSize] = Written{ uint16_t(first | (pixel ? 0x8000u : 0u)),
                                                                uint16_t(count) };
    writeStamps((pixel ? m_psStamps : m_vsStamps) + first, stamp, count);
    // Only the registers from the first to the last changed one are written and recorded.
    uint32_t begin = 0, end = count;
    while (begin < end && sameWords(bank + 4 * (first + begin), values + 4 * begin, 4))
        ++begin;
    while (end > begin && sameWords(bank + 4 * (first + end - 1), values + 4 * (end - 1), 4))
        --end;
    if (begin == end)
        return stamp;
    copyRegisters(bank + 4 * (first + begin), values + 4 * begin, end - begin);
    if (!m_stream)
    {
        m_constantsDirty = true;
        return stamp;
    }
    uint32_t* w = m_stream->append(2 + 4 * (end - begin));
    w[0] = CmdConstants | uint32_t(pixel ? 1 : 0) << 8 | (first + begin) << 16;
    w[1] = end - begin;
    copyRegisters(w + 2, values + 4 * begin, end - begin);
    return stamp;
}

#if NFSC_RENDER_CHECK
void Device::referenceConstants(bool pixel, uint32_t first, const float* values, uint32_t count)
{
    const uint32_t registers = pixel ? PixelFloatConstants : VertexFloatConstants;
    if (first > registers || count > registers - first || !count || !values)
        return;
    checkWrite(pixel, first, values, count, true);
}

void Device::checkWrite(bool pixel, uint32_t first, const float* values, uint32_t count, bool reference)
{
    ConstantCheck& k = m_constantCheck;
    if (!count)
        return;
    const uint32_t b = pixel ? 1 : 0;
    copyRegisters((pixel ? k.pixel : k.vertex) + 4 * first, values, count);
    k.low[b] = std::min(k.low[b], first);
    k.high[b] = std::max(k.high[b], first + count);
    k.referenced = k.referenced || reference;
}

void Device::checkConstants()
{
    ConstantCheck& k = m_constantCheck;
    bool differ = false;
    for (uint32_t b = 0; b < 2; ++b)
    {
        const float* bank = b ? m_psFloat : m_vsFloat;
        float* check = b ? k.pixel : k.vertex;
        for (uint32_t r = k.low[b]; r < k.high[b]; ++r)
        {
            if (sameWords(bank + 4 * r, check + 4 * r, 4))
                continue;
            if (k.reported < 16)
            {
                ++k.reported;
                uint32_t d[4], c[4];
                memcpy(d, bank + 4 * r, sizeof(d));
                memcpy(c, check + 4 * r, sizeof(c));
                pf::logInfo("render check: draw %u: %s constant %u: device %08x %08x %08x %08x reference %08x %08x "
                            "%08x %08x", m_drawSerial, b ? "pixel" : "vertex", r, d[0], d[1], d[2], d[3], c[0], c[1],
                            c[2], c[3]);
            }
            copyRegisters(check + 4 * r, bank + 4 * r, 1);
            differ = true;
        }
        k.low[b] = b ? PixelFloatConstants : VertexFloatConstants;
        k.high[b] = 0;
    }
    if (k.referenced)
    {
        ++k.draws;
        if (differ)
            ++k.differ;
    }
    k.referenced = false;
}
#endif

HRESULT Device::SetVertexShaderConstantF(WinApplication* app, x86::CPU& cpu, UINT start, const float* data,
                                         UINT count)
{
    RC_USE(app);
    RC_USE(cpu);
    return setConstants(false, start, data, count) ? D3D_OK : D3DERR_INVALIDCALL;
}

HRESULT Device::GetVertexShaderConstantF(WinApplication* app, x86::CPU& cpu, UINT start, float* data, UINT count)
{
    RC_USE(app);
    RC_USE(cpu);
    return constants<float>(m_vsFloat, VertexFloatConstants, 4, start, count, nullptr, data) ? D3D_OK
                                                                                             : D3DERR_INVALIDCALL;
}

HRESULT Device::SetVertexShaderConstantI(WinApplication* app, x86::CPU& cpu, UINT start, const int32_t* data,
                                         UINT count)
{
    RC_USE(app);
    RC_USE(cpu);
    return constants<int32_t>(m_vsInt, IntConstants, 4, start, count, data, nullptr) ? D3D_OK : D3DERR_INVALIDCALL;
}

HRESULT Device::GetVertexShaderConstantI(WinApplication* app, x86::CPU& cpu, UINT start, int32_t* data, UINT count)
{
    RC_USE(app);
    RC_USE(cpu);
    return constants<int32_t>(m_vsInt, IntConstants, 4, start, count, nullptr, data) ? D3D_OK : D3DERR_INVALIDCALL;
}

HRESULT Device::SetVertexShaderConstantB(WinApplication* app, x86::CPU& cpu, UINT start, const BOOL* data, UINT count)
{
    RC_USE(app);
    RC_USE(cpu);
    return constants<BOOL>(m_vsBool, BoolConstants, 1, start, count, data, nullptr) ? D3D_OK : D3DERR_INVALIDCALL;
}

HRESULT Device::GetVertexShaderConstantB(WinApplication* app, x86::CPU& cpu, UINT start, BOOL* data, UINT count)
{
    RC_USE(app);
    RC_USE(cpu);
    return constants<BOOL>(m_vsBool, BoolConstants, 1, start, count, nullptr, data) ? D3D_OK : D3DERR_INVALIDCALL;
}

HRESULT Device::CreatePixelShader(WinApplication* app, x86::CPU& cpu, x86::reg32 function, x86::reg32* shader)
{
    RC_USE(cpu);
    if (!function || !shader)
        return D3DERR_INVALIDCALL;
    const uint8_t* code = g_guestBase + function;
    const uint32_t bytes = Shader::length(code, 1024 * 1024);
    uint32_t version;
    memcpy(&version, code, 4);
    if (!bytes || (version >> 16) != 0xffff || (version & 0xffff) > (0xFFFF0200 & 0xffff))
        return D3DERR_INVALIDCALL;
    *shader = publish(app, new Shader(this, KindPixelShader, code, bytes));
    return D3D_OK;
}

HRESULT Device::SetPixelShaderConstantF(WinApplication* app, x86::CPU& cpu, UINT start, const float* data, UINT count)
{
    RC_USE(app);
    RC_USE(cpu);
    return setConstants(true, start, data, count) ? D3D_OK : D3DERR_INVALIDCALL;
}

HRESULT Device::GetPixelShaderConstantF(WinApplication* app, x86::CPU& cpu, UINT start, float* data, UINT count)
{
    RC_USE(app);
    RC_USE(cpu);
    return constants<float>(m_psFloat, PixelFloatConstants, 4, start, count, nullptr, data) ? D3D_OK
                                                                                            : D3DERR_INVALIDCALL;
}

HRESULT Device::SetPixelShaderConstantI(WinApplication* app, x86::CPU& cpu, UINT start, const int32_t* data,
                                        UINT count)
{
    RC_USE(app);
    RC_USE(cpu);
    return constants<int32_t>(m_psInt, IntConstants, 4, start, count, data, nullptr) ? D3D_OK : D3DERR_INVALIDCALL;
}

HRESULT Device::GetPixelShaderConstantI(WinApplication* app, x86::CPU& cpu, UINT start, int32_t* data, UINT count)
{
    RC_USE(app);
    RC_USE(cpu);
    return constants<int32_t>(m_psInt, IntConstants, 4, start, count, nullptr, data) ? D3D_OK : D3DERR_INVALIDCALL;
}

HRESULT Device::SetPixelShaderConstantB(WinApplication* app, x86::CPU& cpu, UINT start, const BOOL* data, UINT count)
{
    RC_USE(app);
    RC_USE(cpu);
    return constants<BOOL>(m_psBool, BoolConstants, 1, start, count, data, nullptr) ? D3D_OK : D3DERR_INVALIDCALL;
}

HRESULT Device::GetPixelShaderConstantB(WinApplication* app, x86::CPU& cpu, UINT start, BOOL* data, UINT count)
{
    RC_USE(app);
    RC_USE(cpu);
    return constants<BOOL>(m_psBool, BoolConstants, 1, start, count, nullptr, data) ? D3D_OK : D3DERR_INVALIDCALL;
}

// Event queries only; with a null result pointer the call reports whether the type is supported.
HRESULT Device::CreateQuery(WinApplication* app, x86::CPU& cpu, DWORD type, x86::reg32* query)
{
    RC_USE(cpu);
    const DWORD D3DQUERYTYPE_EVENT = 8;
    if (type != D3DQUERYTYPE_EVENT)
    {
        D3D_LOG_ONCE("d3d9: query type %u not available", type);
        return D3DERR_NOTAVAILABLE;
    }
    if (query)
        *query = publish(app, new Query(this, type));
    return D3D_OK;
}

HRESULT Device::SetStreamSource(WinApplication* app, x86::CPU& cpu, UINT stream, Buffer* buffer, UINT offset,
                                UINT stride)
{
    RC_USE(app);
    RC_USE(cpu);
    D3D9_TRACE(direct::TraceStreamSource, stream, uint32_t(uintptr_t(buffer)), offset, stride);
    if (stream != 0)
    {
        D3D_LOG_ONCE("d3d9: stream %u ignored", stream);
        return D3D_OK;
    }
    if (buffer && buffer->kind() != KindVertexBuffer)
        return D3DERR_INVALIDCALL;
    hold(m_state.stream, buffer);
    m_state.streamOffset = offset;
    m_state.streamStride = stride;
    return D3D_OK;
}

HRESULT Device::GetStreamSource(WinApplication* app, x86::CPU& cpu, UINT stream, x86::reg32* buffer, UINT* offset,
                                UINT* stride)
{
    RC_USE(app);
    RC_USE(cpu);
    if (stream != 0 || !buffer || !offset || !stride)
        return D3DERR_INVALIDCALL;
    if (m_state.stream)
        m_state.stream->addRef();
    *buffer = m_state.stream ? m_state.stream->guest() : 0;
    *offset = m_state.streamOffset;
    *stride = m_state.streamStride;
    return D3D_OK;
}

HRESULT Device::SetStreamSourceFreq(WinApplication* app, x86::CPU& cpu, UINT stream, UINT divider)
{
    RC_USE(app);
    RC_USE(cpu);
    if (stream >= 16)
        return D3DERR_INVALIDCALL;
    m_streamFrequency[stream] = divider;
    if (divider != 1)
        D3D_LOG_ONCE("d3d9: stream frequency %u stored, instancing not applied", divider);
    return D3D_OK;
}

HRESULT Device::GetStreamSourceFreq(WinApplication* app, x86::CPU& cpu, UINT stream, UINT* divider)
{
    RC_USE(app);
    RC_USE(cpu);
    if (stream >= 16 || !divider)
        return D3DERR_INVALIDCALL;
    *divider = m_streamFrequency[stream];
    return D3D_OK;
}

HRESULT Device::SetIndices(WinApplication* app, x86::CPU& cpu, Buffer* buffer)
{
    RC_USE(app);
    RC_USE(cpu);
    D3D9_TRACE(direct::TraceIndices, uint32_t(uintptr_t(buffer)));
    if (buffer && buffer->kind() != KindIndexBuffer)
        return D3DERR_INVALIDCALL;
    hold(m_state.indices, buffer);
    return D3D_OK;
}

HRESULT Device::GetIndices(WinApplication* app, x86::CPU& cpu, x86::reg32* buffer)
{
    RC_USE(app);
    RC_USE(cpu);
    if (!buffer)
        return D3DERR_INVALIDCALL;
    if (m_state.indices)
        m_state.indices->addRef();
    *buffer = m_state.indices ? m_state.indices->guest() : 0;
    return D3D_OK;
}

HRESULT Device::SetPixelShader(WinApplication* app, x86::CPU& cpu, Object* shader)
{
    RC_USE(app);
    RC_USE(cpu);
    if (shader && shader->kind() != KindPixelShader)
        return D3DERR_INVALIDCALL;
    hold(m_pixelShader, static_cast<Shader*>(shader));
    return D3D_OK;
}

HRESULT Device::GetPixelShader(WinApplication* app, x86::CPU& cpu, x86::reg32* shader)
{
    RC_USE(app);
    RC_USE(cpu);
    if (!shader)
        return D3DERR_INVALIDCALL;
    if (m_pixelShader)
        m_pixelShader->addRef();
    *shader = m_pixelShader ? m_pixelShader->guest() : 0;
    return D3D_OK;
}

}}
