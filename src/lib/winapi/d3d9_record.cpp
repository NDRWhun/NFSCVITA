// Recorded frames: the game thread appends state changes, constant registers and draws to the frame's stream
// (d3d9_device.cpp); the render thread replays them into a mirror of the device state and resolves each draw
// (programs, fixed function, alpha test, blend factor, constants) before the frame is drawn.

#include <winapi/d3d9_internal.h>
#include <platform/config.h>
#include <platform/log.h>
#include <platform/perf.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace win32 { namespace d3d9
{

static_assert(sizeof(DrawRecord) % 4 == 0, "DrawRecord is whole words");
static_assert(sizeof(FixedState) % 4 == 0, "FixedState is whole words");
static_assert(sizeof(NativeDrawRecord) % 4 == 0, "NativeDrawRecord is whole words");
static_assert(sizeof(NativeCheckRecord) % 4 == 0, "NativeCheckRecord is whole words");

namespace
{

enum : DWORD
{
    D3DRS_ZENABLE = 7, D3DRS_ZWRITEENABLE = 14, D3DRS_ALPHATESTENABLE = 15, D3DRS_SRCBLEND = 19,
    D3DRS_DESTBLEND = 20, D3DRS_CULLMODE = 22, D3DRS_ZFUNC = 23, D3DRS_ALPHAREF = 24, D3DRS_ALPHAFUNC = 25,
    D3DRS_ALPHABLENDENABLE = 27, D3DRS_FOGENABLE = 28, D3DRS_FOGCOLOR = 34, D3DRS_TEXTUREFACTOR = 60,
    D3DRS_LIGHTING = 137,
    D3DRS_COLORWRITEENABLE = 168, D3DRS_BLENDOP = 171, D3DRS_BLENDFACTOR = 193,

    D3DSAMP_ADDRESSU = 1, D3DSAMP_ADDRESSV = 2, D3DSAMP_MAGFILTER = 5, D3DSAMP_MINFILTER = 6, D3DSAMP_MIPFILTER = 7
};

const float s_identity[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };

// Frame slots keep at most 3 frames in flight: beginFrame(n) returns once frame n - 3 is finished, so it has
// been resolved and the stream of n - 4 is free.
const uint32_t StreamCount = 4;
Stream s_streams[StreamCount];
std::atomic<uint32_t> s_resolved{0};

uint8_t address(DWORD mode)
{
    switch (mode)
    {
    case 2:  return render::AddressMirror;
    case 3:
    case 4:  return render::AddressClamp;
    case 5:  return render::AddressMirror;
    default: return render::AddressWrap;
    }
}

render::Sampler sampler(const DWORD* states)
{
    render::Sampler s = {};
    s.addressU = address(states[D3DSAMP_ADDRESSU]);
    s.addressV = address(states[D3DSAMP_ADDRESSV]);
    s.filter = uint8_t((states[D3DSAMP_MINFILTER] >= 2 ? 1 : 0) | (states[D3DSAMP_MAGFILTER] >= 2 ? 2 : 0));
    s.mip = uint8_t(states[D3DSAMP_MIPFILTER] >= 2 ? render::MipLinear
                  : states[D3DSAMP_MIPFILTER] == 1 ? render::MipPoint : render::MipNone);
    return s;
}

void color(DWORD argb, float out[4])
{
    out[0] = float((argb >> 16) & 255) / 255.0f;
    out[1] = float((argb >> 8) & 255) / 255.0f;
    out[2] = float(argb & 255) / 255.0f;
    out[3] = float(argb >> 24) / 255.0f;
}

// What the constants of a draw depend on; equal keys reuse the previous constants.
struct ConstantsKey
{
    uint32_t                        source;         // 0 fixed function, 1 effect programs
    uint32_t                        version;
    DWORD                           alphaFunc, alphaRef;
    DWORD                           stage0Flags;    // effect programs: stage 0 texture transform flags
    DWORD                           fogEnable, fogColor;    // effect programs with fog
    float                           blendFactor[4];
    const render::TextureStorage*   volume, *volume2;   // effect programs: the volume atlases sampled
    uint8_t                         vertexProgram, fragmentProgram, texComponents, alphaTest;
};

static_assert(sizeof(ConstantsKey) % 4 == 0, "ConstantsKey is whole words");

// uProj of an effect program for stage 0 texture transform flags: ps_1_x divides by the last coordinate the count
// names (w without a count).
void projection(DWORD flags, float out[4])
{
    out[0] = out[1] = out[3] = 0.0f;
    out[2] = 1.0f;
    if (!(flags & D3DTTFF_PROJECTED))
        return;
    const DWORD count = flags & 0xff;
    if (count == D3DTTFF_COUNT3)
    {
        out[0] = 1.0f;
        out[2] = 0.0f;
    }
    else if (count == 0 || count == D3DTTFF_COUNT4)
    {
        out[1] = 1.0f;
        out[2] = 0.0f;
    }
    else
    {
        D3D_LOG_ONCE("d3d9: projected texture coordinates with count %u not divided", count);
    }
}

// uVol of an effect program for a volume atlas: depth, 1 / depth, the half texel clamp of a slice's u.
void volumeConstants(const render::TextureStorage* atlas, float out[4])
{
    const float depth = atlas && atlas->desc.depth ? float(atlas->desc.depth) : 1.0f;
    const float width = atlas && atlas->desc.depth ? float(atlas->desc.width) / depth : 1.0f;
    out[0] = depth;
    out[1] = 1.0f / depth;
    out[2] = 0.5f / width;
    out[3] = 1.0f - 0.5f / width;
}

// What the alpha test elision found for an alpha-tested draw.
enum AlphaResult : uint8_t
{
    AlphaElided,            // discards nothing: drawn without the test
    AlphaProgram,           // a program pair whose alpha is not bounded here
    AlphaFunction,          // LESS, LESSEQUAL or NEVER
    AlphaTexture,           // no texture, or its smallest alpha is 0 or unknown
    AlphaVertex,            // vertex alpha 0 or unknown
    AlphaConstants,         // car diffuse alpha without a lower bound
    AlphaThreshold,         // the smallest alpha can fail the test
    AlphaResultCount
};

const char* const s_alphaNames[AlphaResultCount] = {
    "elided", "program", "function", "texture", "vertex", "constants", "threshold"
};

const uint32_t FrameVertexScan = 4096;      // most vertices of a frame-arena draw scanned for their alpha
const uint8_t BufferScans = 4;              // scans of one vertex buffer before its alpha counts as unknown

// Smallest vertex alpha a draw reads (the alpha byte of its colour element), 0 when unknown. Vertex buffers: the
// smallest over the whole buffer at the element's position, kept until the buffer is written in place again.
uint8_t vertexAlpha(uint8_t layout, const render::BufferStorage* storage, const render::DrawCall& draw)
{
    const render::VertexLayout& l = render::layout(layout);
    const render::VertexElement* color = nullptr;
    for (uint32_t i = 0; i < l.count; ++i)
    {
        if (l.elements[i].usage == render::UsageColor
            && (l.elements[i].type == render::TypeColor || l.elements[i].type == render::TypeUByte4N))
            color = &l.elements[i];
    }
    if (!color || !l.stride || !draw.vertices)
        return 0;
    const uint32_t stride = l.stride;
    render::BufferStorage* b = const_cast<render::BufferStorage*>(storage);
    if (!b)
    {
        if (draw.vertexCount > FrameVertexScan)
            return 0;
        const uint8_t* p = static_cast<const uint8_t*>(draw.vertices) + color->offset + 3;
        uint8_t lowest = 255;
        for (uint32_t i = 0; i < draw.vertexCount && lowest; ++i)
            lowest = p[size_t(i) * stride] < lowest ? p[size_t(i) * stride] : lowest;
        return lowest;
    }
    const ptrdiff_t start = static_cast<const uint8_t*>(draw.vertices) - static_cast<const uint8_t*>(b->data);
    if (start < 0 || uint32_t(start) >= b->size)
        return 0;
    const uint32_t offset = uint32_t(start + color->offset + 3) % stride;
    const uint32_t writes = b->writes.load(std::memory_order_acquire);
    if (b->alphaWrites == writes && b->alphaStride == stride && b->alphaOffset == offset)
        return b->alpha;
    if (b->alphaScans >= BufferScans)
        return 0;
    ++b->alphaScans;
    const uint8_t* data = static_cast<const uint8_t*>(b->data);
    uint8_t lowest = 255;
    for (uint32_t i = offset; i < b->size && lowest; i += stride)
        lowest = data[i] < lowest ? data[i] : lowest;
    b->alphaWrites = writes;
    b->alphaStride = uint16_t(stride);
    b->alphaOffset = uint16_t(offset);
    b->alpha = lowest;
    return lowest;
}

// Car vertex programs: vColor.a = saturate(min(litZ(s, s, powers.x), clamp.x) * clamp.z * range.w + min.w), litZ
// >= 0. Its lower bound from the registers min, range and clamp; false when it has none.
bool carDiffuseAlpha(const float* registers, uint32_t min, uint32_t range, uint32_t clamp, float* lowest)
{
    const float limit = registers[4 * clamp], scale = registers[4 * clamp + 2];
    const float base = registers[4 * min + 3], slope = registers[4 * range + 3];
    if (!std::isfinite(limit) || !std::isfinite(scale) || !std::isfinite(base) || !std::isfinite(slope))
        return false;
    // The power term lies between min(0, limit) and limit.
    const float a = std::min(0.0f, limit) * scale * slope, b = limit * scale * slope;
    const float low = base + std::min(a, b);
    *lowest = low < 0.0f ? 0.0f : low > 1.0f ? 1.0f : low;
    return true;
}

// Render thread only.
class Resolver
{
public:
    void resolve(render::Frame& frame);

private:
    bool fillState(render::Frame& frame, const DrawRecord& record, render::DrawCall& draw);
    AlphaResult alphaTestResult(const render::BufferStorage* vertexStorage, const render::DrawCall& draw,
                                const float test[2], const float* vertexRegisters) const;
    // The alpha test elision and the blend factor resolution of an effect or fixed-function draw's state.
    BlendFactorDraw finish(render::DrawState& state, const float test[2], DWORD blendOp, DWORD blendFactor,
                           const render::BufferStorage* vertexStorage, const render::DrawCall& draw,
                           const float* vertexRegisters, bool count);
    void nativeDraw(render::Frame& frame, const NativeDrawRecord& record);
#if NFSC_RENDER_CHECK
    void nativeCheck(render::Frame& frame, const NativeCheckRecord& record, const float* constants);
    struct CheckCounts
    {
        uint64_t draws = 0, state = 0, geometry = 0, vertex = 0, fragment = 0, missing = 0;
    };
    CheckCounts                     m_check;
    uint32_t                        m_checkReported = 0, m_checkFrames = 0;
#endif

    FixedState                      m_state = {};
    float                           m_vertexRegisters[Device::VertexFloatConstants * 4] = {};
    float                           m_pixelRegisters[Device::PixelFloatConstants * 4] = {};
    uint32_t                        m_fixedVersion = 1, m_constantVersion = 1;
    const render::TextureStorage*   m_lastTexture = nullptr;    // stage 0 of the last fixed-function draw
    bool                            m_keyValid = false;
    ConstantsKey                    m_key = {};
    uint32_t                        m_vertexConstants = render::NoConstants;
    uint32_t                        m_fragmentConstants = render::NoConstants;
    uint32_t                        m_alphaFrames = 0;
    uint32_t                        m_alphaCounts[AlphaResultCount] = {};   // perf_log: alpha-tested draws
};

Resolver s_resolver;

// Whether the alpha test of a draw (discard when test[0] * alpha < test[1]) discards no fragment. The fragment
// programs' alpha: 1, the texel's, or the texel's times the vertex program's vColor.a (doubled and clamped for
// prelit); vColor.a is the vertex colour's alpha or the car programs' diffuse alpha.
AlphaResult Resolver::alphaTestResult(const render::BufferStorage* vertexStorage, const render::DrawCall& draw,
                                      const float test[2], const float* vertexRegisters) const
{
    const render::DrawState& state = draw.state;
    int sampler = 0;
    float scale = 1.0f;
    bool vertex = true;
    switch (state.fragmentProgram)
    {
    case render::FpUcap:
    case render::FpOpaque:
    case render::FpTextureOpaque:
    case render::FpRoad:
        // Alpha 1; a step of margin for the half-precision compares.
        return test[0] - test[1] >= 1.0f / 255.0f ? AlphaElided : AlphaThreshold;
    case render::FpTexture:
    case render::FpPassthru:
    case render::FpWorldNormalMap:
        vertex = false;
        break;
    case render::FpWorld:
    case render::FpModulate:
    case render::FpCar:
    case render::FpCarNormalMap:
        break;
    case render::FpCar1:
    case render::FpCarNormalMap1:
        sampler = 1;
        break;
    case render::FpPrelit:
        scale = 2.0f;
        break;
    default:
        return AlphaProgram;
    }
    float factor = 1.0f;
    AlphaResult unknown = AlphaVertex;
    if (vertex)
    {
        switch (state.vertexProgram)
        {
        case render::VpWorld:
        case render::VpWorldLow:
        case render::VpWorld1:
        case render::VpPrelit:
        case render::VpGlass:
        case render::VpGlass1:
        case render::VpGlassLow:
            factor = -1.0f;
            break;
        case render::VpCar:
        case render::VpCarNormalMap:
            if (!carDiffuseAlpha(vertexRegisters, 21, 22, 26, &factor))
                return AlphaConstants;
            unknown = AlphaConstants;
            break;
        case render::VpCarNormalMap1:
            if (!carDiffuseAlpha(vertexRegisters, 17, 18, 22, &factor))
                return AlphaConstants;
            unknown = AlphaConstants;
            break;
        default:
            return AlphaProgram;
        }
    }
    if (test[0] <= 0.0f)
        return AlphaFunction;
    if (!state.textures[sampler])
        return AlphaTexture;
    const uint8_t texel = render::minAlpha(*state.textures[sampler]);
    if (!texel)
        return AlphaTexture;
    if (factor < 0.0f)
    {
        const uint8_t a = vertexAlpha(state.layout, vertexStorage, draw);
        if (!a)
            return AlphaVertex;
        factor = float(a) / 255.0f;
    }
    else
    {
        // Rounded down to a multiple of 1/255.
        factor = float(int(factor * 255.0f)) / 255.0f;
        if (factor <= 0.0f)
            return unknown;
    }
    // Two steps below the lowest product for filtering and interpolation rounding.
    const float lowest = float(texel) / 255.0f * factor * scale - 2.0f / 255.0f;
    return test[0] * std::min(lowest, 1.0f) >= test[1] ? AlphaElided : AlphaThreshold;
}

// Whether a screen pass reads sampler 0 bilinearly from one level a whole multiple of the viewport's size.
bool wholeMultiple(const render::DrawState& state, const D3DVIEWPORT9& viewport)
{
    const render::TextureStorage* t = state.textures[0];
    const render::Sampler& s = state.samplers[0];
    if (!t || t->desc.cube || state.vertexProgram != render::VpScreen || s.filter != 3 || !viewport.Width
        || !viewport.Height || (t->desc.levels > 1 && s.mip != render::MipNone))
        return false;
    return texture::levelDim(t->desc.width, s.level) % viewport.Width == 0
        && texture::levelDim(t->desc.height, s.level) % viewport.Height == 0;
}

// The state fields every draw takes from the render states.
void commonState(const DWORD* rs, uint8_t layout, uint8_t primitive, render::DrawState& state)
{
    memset(&state, 0, sizeof(state));
    state.layout = layout;
    state.primitive = primitive;
    state.colorMask = uint8_t(rs[D3DRS_COLORWRITEENABLE] & 0xf);
    state.depthEnable = rs[D3DRS_ZENABLE] ? 1 : 0;
    state.depthWrite = rs[D3DRS_ZWRITEENABLE] ? 1 : 0;
    state.depthFunc = uint8_t(rs[D3DRS_ZFUNC]);
    state.cullMode = uint8_t(rs[D3DRS_CULLMODE]);
    if (rs[D3DRS_BLENDOP] != 1)
        D3D_LOG_ONCE("d3d9: blend operation %u approximated by ADD", rs[D3DRS_BLENDOP]);
}

// The blend and alpha test states before the elision and the blend factor resolution.
void blendState(const DWORD* rs, render::DrawState& state, float test[2])
{
    state.blendEnable = rs[D3DRS_ALPHABLENDENABLE] ? 1 : 0;
    state.srcBlend = uint8_t(rs[D3DRS_SRCBLEND]);
    state.dstBlend = uint8_t(rs[D3DRS_DESTBLEND]);
    test[0] = test[1] = 0.0f;
    state.alphaTest = rs[D3DRS_ALPHATESTENABLE] && alphaTest(rs[D3DRS_ALPHAFUNC], rs[D3DRS_ALPHAREF], test) ? 1 : 0;
}

// Whether an effect draw's programs read fog states.
bool effectFog(const render::DrawState& state)
{
    return render::fragmentProgram(render::FragmentProgram(state.fragmentProgram)).fogOffset
        || (render::vertexProgram(render::VertexProgram(state.vertexProgram)).flags & render::ProgramFog);
}

}

void effectState(const EffectSource& source, render::DrawState& state, float test[2],
                 const render::TextureStorage** volume, const render::TextureStorage** volume2)
{
    const FixedState& fs = *source.state;
    const DWORD* rs = fs.renderStates;
    commonState(rs, source.layout, source.primitive, state);
    *volume = *volume2 = nullptr;
    // Effect programs: the render states, stage textures, sampler states and constant registers the pass and the game
    // left on the device.
    const EffectDraw& effect = source.programs;
    state.vertexProgram = effect.vertexProgram;
    state.fragmentProgram = effect.fragmentProgram;
    const render::FragmentProgramInfo& info = render::fragmentProgram(render::FragmentProgram(effect.fragmentProgram));
    for (uint32_t i = 0; i < info.samplers && i < render::MaxSamplers; ++i)
    {
        state.textures[i] = source.textures[i];
        state.samplers[i] = sampler(fs.samplerStates[i]);
        if (i == info.volumeSampler || i == info.volumeSampler2)
        {
            // The atlas is read in two clamped, filtered fetches per lookup.
            *(i == info.volumeSampler ? volume : volume2) = source.textures[i];
            state.samplers[i].addressU = state.samplers[i].addressV = render::AddressClamp;
            state.samplers[i].filter = 3;
            state.samplers[i].mip = render::MipNone;
        }
    }
    // A 4x4 mean whose taps fall on texel edges: the nine-tap programs.
    if ((state.fragmentProgram == render::FpDown4 || state.fragmentProgram == render::FpDownBloom)
        && wholeMultiple(state, fs.viewport))
        state.fragmentProgram = state.fragmentProgram == render::FpDown4 ? render::FpDown4Tent
                                                                         : render::FpDownBloomTent;
    const render::VertexProgramInfo& vinfo = render::vertexProgram(render::VertexProgram(effect.vertexProgram));
    if (vinfo.biasRegister != render::NoBias && info.samplers)
    {
        // A constant texldb bias: the sampler's mip bias, in eighths.
        const float bias = source.vertexRegisters[4 * vinfo.biasRegister] * 8.0f;
        state.samplers[0].lodBias = int8_t(std::min(32.0f, std::max(-31.0f, bias + (bias < 0 ? -0.5f : 0.5f))));
    }
    if (rs[D3DRS_FOGENABLE] && !info.fogOffset && !(vinfo.flags & render::ProgramFog))
        D3D_LOG_ONCE("d3d9: fog with effect program %s is not emulated", info.name);
    blendState(rs, state, test);
}

void effectConstants(const EffectSource& source, const render::DrawState& state, const float test[2],
                     const render::TextureStorage* volume, const render::TextureStorage* volume2, float* vc,
                     float* fc)
{
    const FixedState& fs = *source.state;
    const DWORD* rs = fs.renderStates;
    const render::VertexProgramInfo& vinfo = render::vertexProgram(render::VertexProgram(state.vertexProgram));
    const render::FragmentProgramInfo& fragment =
        render::fragmentProgram(render::FragmentProgram(state.fragmentProgram));
    const bool fog = effectFog(state);
    const DWORD fogEnable = fog && rs[D3DRS_FOGENABLE] ? 1 : 0;
    const DWORD fogColor = fog ? rs[D3DRS_FOGCOLOR] : 0;
    if (vc)
    {
        memcpy(vc, source.vertexRegisters, 16 * vinfo.registers);
        if (vinfo.pixelRegisters)
            memcpy(vc + vinfo.pixelOffset, source.pixelRegisters, 16 * vinfo.pixelRegisters);
        if (vinfo.flags & render::ProgramFog)
        {
            color(fogColor, vc + vinfo.fogOffset);
            vc[vinfo.fogOffset + 3] = float(fogEnable);
        }
    }
    if (fc)
    {
        memset(fc, 0, fragment.constantFloats * sizeof(float));
        float* c = fc + 4;
        memcpy(c, source.pixelRegisters, 16 * fragment.registers);
        if (fragment.flags & render::ProgramClampConstants)
        {
            for (uint32_t i = 0; i < 4u * fragment.registers; ++i)
                c[i] = std::min(1.0f, std::max(-1.0f, c[i]));
        }
        if (fragment.projOffset)
            projection(fs.stageStates[0][D3DTSS_TEXTURETRANSFORMFLAGS], fc + fragment.projOffset);
        if (fragment.volumeOffset)
            volumeConstants(volume, fc + fragment.volumeOffset);
        if (fragment.volumeOffset2)
            volumeConstants(volume2, fc + fragment.volumeOffset2);
        if (fragment.fogOffset)
        {
            // Vertex fog after the program: the colour blends toward FOGCOLOR by 1 - the fog factor.
            color(fogColor, fc + fragment.fogOffset);
            fc[fragment.fogOffset + 3] = float(fogEnable);
        }
        if (fragment.constantFloats >= 2)
        {
            fc[0] = test[0];
            fc[1] = test[1];
        }
    }
}

namespace
{

BlendFactorDraw Resolver::finish(render::DrawState& state, const float test[2], DWORD blendOp, DWORD blendFactor,
                                 const render::BufferStorage* vertexStorage, const render::DrawCall& draw,
                                 const float* vertexRegisters, bool count)
{
    // A test that discards nothing: the program without it (the draw becomes opaque to the GPU).
    static const bool s_elide = pf::Config::getBool("alpha_test_elide", true);
    if (state.alphaTest && (s_elide || pf::perf::enabled()))
    {
        const AlphaResult result = alphaTestResult(vertexStorage, draw, test, vertexRegisters);
        if (count)
            ++m_alphaCounts[result];
        if (s_elide && result == AlphaElided)
            state.alphaTest = 0;
    }
    const render::FragmentProgramInfo& fragment =
        render::fragmentProgram(render::FragmentProgram(state.fragmentProgram));
    BlendFactorDraw factor = {};
    if (state.blendEnable)
    {
        factor = resolveBlendFactor(state.srcBlend, state.dstBlend, blendOp, blendFactor,
                                    fragment.blendFactor && !state.alphaTest, (state.colorMask & 8) != 0);
        state.srcBlend = uint8_t(factor.src);
        state.dstBlend = uint8_t(factor.dst);
        state.factorOutput = factor.output;
    }
    return factor;
}

bool Resolver::fillState(render::Frame& f, const DrawRecord& record, render::DrawCall& draw)
{
    render::DrawState& state = draw.state;
    const DWORD* rs = m_state.renderStates;
    const bool positionT = record.positionT != 0;
    const DWORD alphaRef = rs[D3DRS_ALPHAREF];
    uint32_t source = 0;
    uint32_t version = m_fixedVersion;
    FixedResult fixed = {};
    const EffectDraw& effect = record.effect;
    const render::TextureStorage* volume = nullptr;
    const render::TextureStorage* volume2 = nullptr;
    EffectSource effectSource;
    float test[2] = { 0.0f, 0.0f };

    if (effect.kind != NoEffect)
    {
        effectSource.state = &m_state;
        memcpy(effectSource.textures, record.textures, sizeof(effectSource.textures));
        effectSource.vertexRegisters = m_vertexRegisters;
        effectSource.pixelRegisters = m_pixelRegisters;
        effectSource.programs = effect;
        effectSource.layout = record.layout;
        effectSource.primitive = record.primitive;
        effectState(effectSource, state, test, &volume, &volume2);
        source = 1;
        version = m_constantVersion;
    }
    else
    {
        commonState(rs, record.layout, record.primitive, state);
        if (rs[D3DRS_LIGHTING] && !positionT)
            D3D_LOG_ONCE("d3d9: fixed-function lighting is not emulated (vertex colours used)");
        if (rs[D3DRS_FOGENABLE])
            D3D_LOG_ONCE("d3d9: fixed-function fog is not emulated");
        const DWORD* tss = m_state.stageStates[0];
        const render::TextureStorage* texture = record.textures[0];
        if (texture != m_lastTexture)
            version = ++m_fixedVersion;
        m_lastTexture = texture;
        if (texture && texture->desc.cube)
        {
            D3D_LOG_ONCE("d3d9: cube texture on fixed-function stage 0 ignored");
            texture = nullptr;
        }
        if (m_state.stageStates[1][D3DTSS_COLOROP] != D3DTOP_DISABLE)
            D3D_LOG_ONCE("d3d9: fixed-function stages after 0 are not emulated");
        FixedStage stage = { tss[D3DTSS_COLOROP], tss[D3DTSS_COLORARG1], tss[D3DTSS_COLORARG2], tss[D3DTSS_ALPHAOP],
                             tss[D3DTSS_ALPHAARG1], tss[D3DTSS_ALPHAARG2], tss[D3DTSS_TEXTURETRANSFORMFLAGS],
                             texture != nullptr };
        fixed = resolveFixed(stage);
        state.vertexProgram = positionT ? render::VpFixedScreen : render::VpFixed;
        state.fragmentProgram = fixed.fragmentProgram;
        if (fixed.fragmentProgram != render::FpFixedColor)
        {
            state.textures[0] = texture;
            state.samplers[0] = sampler(m_state.samplerStates[0]);
        }
        blendState(rs, state, test);
    }
    const BlendFactorDraw factor = finish(state, test, rs[D3DRS_BLENDOP], rs[D3DRS_BLENDFACTOR], record.vertexStorage,
                                          draw, m_vertexRegisters, true);
    const render::FragmentProgramInfo& fragment =
        render::fragmentProgram(render::FragmentProgram(state.fragmentProgram));

    ConstantsKey key;
    memset(&key, 0, sizeof(key));
    key.source = source;
    key.version = version;
    key.alphaFunc = rs[D3DRS_ALPHAFUNC];
    key.alphaRef = alphaRef;
    key.stage0Flags = source ? m_state.stageStates[0][D3DTSS_TEXTURETRANSFORMFLAGS] : 0;
    const render::VertexProgramInfo& vinfo = render::vertexProgram(render::VertexProgram(state.vertexProgram));
    const bool fog = source && (fragment.fogOffset || (vinfo.flags & render::ProgramFog));
    key.fogEnable = fog && rs[D3DRS_FOGENABLE] ? 1 : 0;
    key.fogColor = fog ? rs[D3DRS_FOGCOLOR] : 0;
    key.volume = volume;
    key.volume2 = volume2;
    memcpy(key.blendFactor, factor.factor, sizeof(key.blendFactor));
    key.vertexProgram = state.vertexProgram;
    key.fragmentProgram = state.fragmentProgram;
    key.texComponents = uint8_t(record.texComponents | (positionT ? 0x80 : 0));
    key.alphaTest = state.alphaTest;
    if (m_keyValid && sameWords(&key, &m_key, sizeof(key) / 4))
    {
        draw.vertexConstants = m_vertexConstants;
        draw.fragmentConstants = m_fragmentConstants;
        return true;
    }

    const uint32_t vertexFloats = vinfo.constantFloats;
    const uint32_t fragmentFloats = fragment.constantFloats;
    float* vc = nullptr;
    float* fc = nullptr;
    draw.vertexConstants = draw.fragmentConstants = render::NoConstants;
    if (vertexFloats && !(vc = render::allocateConstants(f, vertexFloats, &draw.vertexConstants)))
        return false;
    if (fragmentFloats && !(fc = render::allocateConstants(f, fragmentFloats, &draw.fragmentConstants)))
        return false;

    if (effect.kind != NoEffect)
    {
        effectConstants(effectSource, state, test, volume, volume2, vc, fc);
    }
    else
    {
        render::FixedVertexConstants& v = *reinterpret_cast<render::FixedVertexConstants*>(vc);
        float wv[16];
        multiply(m_state.transforms[TransformWorld].m, m_state.transforms[TransformView].m, wv);
        multiply(wv, m_state.transforms[TransformProjection].m, v.wvp);
        // Window coordinates of the viewport to clip space.
        const D3DVIEWPORT9& vp = m_state.viewport;
        const float w = float(vp.Width ? vp.Width : 1), h = float(vp.Height ? vp.Height : 1);
        v.screen[0] = 2.0f / w;
        v.screen[1] = -2.0f / h;
        v.screen[2] = -1.0f - 2.0f * float(vp.X) / w;
        v.screen[3] = 1.0f + 2.0f * float(vp.Y) / h;
        const DWORD flags = m_state.stageStates[0][D3DTSS_TEXTURETRANSFORMFLAGS];
        memcpy(v.texMatrix, (flags & 0xff) ? m_state.transforms[TransformTexture0].m : s_identity, sizeof(v.texMatrix));
        if ((flags & D3DTTFF_PROJECTED) && (flags & 0xff) == D3DTTFF_COUNT4)
        {
            // Divided by the fourth coordinate: move it to the third, which the program divides by.
            for (int row = 0; row < 4; ++row)
                v.texMatrix[row * 4 + 2] = v.texMatrix[row * 4 + 3];
        }
        texturePad(record.texComponents, v.texPad);
        memcpy(v.diffuseMask, fixed.diffuseMask, sizeof(v.diffuseMask));
        memcpy(v.factorMask, fixed.factorMask, sizeof(v.factorMask));
        color(rs[D3DRS_TEXTUREFACTOR], v.factor);
        render::FixedFragmentConstants& p = *reinterpret_cast<render::FixedFragmentConstants*>(fc);
        memset(&p, 0, sizeof(p));
        memcpy(p.textureMask, fixed.textureMask, sizeof(p.textureMask));
        memcpy(p.scale, fixed.scale, sizeof(p.scale));
        memcpy(p.addMask, fixed.addMask, sizeof(p.addMask));
    }
    if (fc && fragmentFloats >= 2)
    {
        fc[0] = test[0];
        fc[1] = test[1];
    }
    if (fc && state.factorOutput)
        memcpy(fc + fragment.blendFactor, factor.factor, sizeof(factor.factor));
    m_key = key;
    m_keyValid = true;
    m_vertexConstants = draw.vertexConstants;
    m_fragmentConstants = draw.fragmentConstants;
    return true;
}

// A native draw: its state with the alpha test elision and the blend factor resolution.
void Resolver::nativeDraw(render::Frame& f, const NativeDrawRecord& r)
{
    render::DrawCall& d = f.draws[r.draw];
    d.state = r.state;
    const float* vc = d.vertexConstants != render::NoConstants ? f.constants + d.vertexConstants : m_vertexRegisters;
    const BlendFactorDraw factor = finish(d.state, r.test, r.blendOp, r.blendFactor, r.vertexStorage, d, vc, true);
    if (d.state.factorOutput && d.fragmentConstants != render::NoConstants)
        memcpy(f.constants + d.fragmentConstants
                   + render::fragmentProgram(render::FragmentProgram(d.state.fragmentProgram)).blendFactor,
               factor.factor, sizeof(factor.factor));
}

#if NFSC_RENDER_CHECK
struct StateField
{
    const char* name;
    uint16_t    offset, size;
};

#define STATE_FIELD(member) { #member, uint16_t(offsetof(render::DrawState, member)), \
                              uint16_t(sizeof(render::DrawState::member)) }

const StateField s_stateFields[] = {
    STATE_FIELD(textures), STATE_FIELD(samplers), STATE_FIELD(vertexProgram), STATE_FIELD(fragmentProgram),
    STATE_FIELD(layout), STATE_FIELD(primitive), STATE_FIELD(blendEnable), STATE_FIELD(srcBlend),
    STATE_FIELD(dstBlend), STATE_FIELD(colorMask), STATE_FIELD(alphaTest), STATE_FIELD(depthEnable),
    STATE_FIELD(depthWrite), STATE_FIELD(depthFunc), STATE_FIELD(cullMode), STATE_FIELD(factorOutput),
};

// Index of the first float that differs in bits, or count.
uint32_t firstDifference(const float* a, const float* b, uint32_t count)
{
    for (uint32_t i = 0; i < count; ++i)
    {
        if (memcmp(a + i, b + i, 4) != 0)
            return i;
    }
    return count;
}

// The native draw against the translated path's draw at the same index: state after the render thread's
// completion, geometry and constant blocks.
void Resolver::nativeCheck(render::Frame& f, const NativeCheckRecord& r, const float* constants)
{
    ++m_check.draws;
    const bool report = m_checkReported < 32;
    if (r.native.draw >= f.drawCount)
    {
        ++m_check.missing;
        if (report)
        {
            ++m_checkReported;
            pf::logInfo("render check: frame %u native draw for index %u: no translated draw", f.index,
                        r.native.draw);
        }
        return;
    }
    const render::DrawCall& d = f.draws[r.native.draw];
    render::DrawCall probe = d;
    probe.state = r.native.state;
    probe.vertices = r.vertices;
    probe.indices = r.indices;
    probe.count = r.count;
    probe.vertexCount = r.vertexCount;
    const float* vc = constants;
    const float* fc = constants + r.vertexFloats;
    finish(probe.state, r.native.test, r.native.blendOp, r.native.blendFactor, r.native.vertexStorage, probe,
           r.vertexFloats ? vc : m_vertexRegisters, false);
    bool differs = false;
    if (memcmp(&probe.state, &d.state, sizeof(d.state)) != 0)
    {
        ++m_check.state;
        differs = true;
        for (const StateField& field : s_stateFields)
        {
            const uint8_t* a = reinterpret_cast<const uint8_t*>(&probe.state) + field.offset;
            const uint8_t* b = reinterpret_cast<const uint8_t*>(&d.state) + field.offset;
            for (uint32_t i = 0; report && i < field.size; ++i)
            {
                if (a[i] != b[i])
                {
                    pf::logInfo("render check: frame %u draw %u: state %s byte %u: native 0x%02x translated 0x%02x",
                                f.index, r.native.draw, field.name, i, a[i], b[i]);
                    break;
                }
            }
        }
    }
    if (probe.vertices != d.vertices || probe.indices != d.indices || probe.count != d.count
        || probe.vertexCount != d.vertexCount)
    {
        ++m_check.geometry;
        differs = true;
        if (report)
            pf::logInfo("render check: frame %u draw %u: geometry native %p %p %u %u translated %p %p %u %u", f.index,
                        r.native.draw, probe.vertices, static_cast<const void*>(probe.indices), probe.count,
                        probe.vertexCount, d.vertices, static_cast<const void*>(d.indices), d.count, d.vertexCount);
    }
    const uint32_t vertexFloats = render::vertexProgram(render::VertexProgram(d.state.vertexProgram)).constantFloats;
    const uint32_t fragmentFloats =
        render::fragmentProgram(render::FragmentProgram(d.state.fragmentProgram)).constantFloats;
    const float* tv = d.vertexConstants != render::NoConstants ? f.constants + d.vertexConstants : nullptr;
    const float* tf = d.fragmentConstants != render::NoConstants ? f.constants + d.fragmentConstants : nullptr;
    const uint32_t vi = r.vertexFloats != vertexFloats || (vertexFloats && !tv) ? 0
                      : firstDifference(vc, tv, vertexFloats);
    if (vi < vertexFloats || r.vertexFloats != vertexFloats)
    {
        ++m_check.vertex;
        differs = true;
        if (report)
            pf::logInfo("render check: frame %u draw %u: vertex constants (%u floats, translated %u) float %u: "
                        "native 0x%08x translated 0x%08x", f.index, r.native.draw, r.vertexFloats, vertexFloats, vi,
                        vi < r.vertexFloats ? reinterpret_cast<const uint32_t*>(vc)[vi] : 0,
                        tv && vi < vertexFloats ? reinterpret_cast<const uint32_t*>(tv)[vi] : 0);
    }
    const uint32_t fi = r.fragmentFloats != fragmentFloats || (fragmentFloats && !tf) ? 0
                      : firstDifference(fc, tf, fragmentFloats);
    if (fi < fragmentFloats || r.fragmentFloats != fragmentFloats)
    {
        ++m_check.fragment;
        differs = true;
        if (report)
            pf::logInfo("render check: frame %u draw %u: fragment constants (%u floats, translated %u) float %u: "
                        "native 0x%08x translated 0x%08x", f.index, r.native.draw, r.fragmentFloats, fragmentFloats,
                        fi, fi < r.fragmentFloats ? reinterpret_cast<const uint32_t*>(fc)[fi] : 0,
                        tf && fi < fragmentFloats ? reinterpret_cast<const uint32_t*>(tf)[fi] : 0);
    }
    if (differs && report)
        ++m_checkReported;
}
#endif

void Resolver::resolve(render::Frame& f)
{
    m_keyValid = false;
    bool full = false;
    const uint32_t* w = f.commands;
    const uint32_t* end = w + f.commandCount;
    while (w < end)
    {
        const uint32_t header = *w++;
        const uint32_t a = (header >> 8) & 0xff, b = header >> 16;
        switch (Command(header & 0xff))
        {
        case CmdRenderState:
            m_state.renderStates[header >> 8 & 0xff] = *w++;
            ++m_fixedVersion;
            break;
        case CmdStageState:
            m_state.stageStates[a & 7][b < 33 ? b : 0] = *w++;
            ++m_fixedVersion;
            break;
        case CmdSamplerState:
            m_state.samplerStates[a & 15][b < 14 ? b : 0] = *w++;
            ++m_fixedVersion;
            break;
        case CmdTransform:
            memcpy(&m_state.transforms[a < TransformCount ? a : 0], w, sizeof(D3DMATRIX));
            w += 16;
            ++m_fixedVersion;
            break;
        case CmdViewport:
            memcpy(&m_state.viewport, w, sizeof(D3DVIEWPORT9));
            w += sizeof(D3DVIEWPORT9) / 4;
            ++m_fixedVersion;
            break;
        case CmdFixedState:
            memcpy(&m_state, w, sizeof(FixedState));
            w += sizeof(FixedState) / 4;
            ++m_fixedVersion;
            break;
        case CmdConstants:
        {
            const uint32_t count = w[0];
            float* bank = (a & 1) ? m_pixelRegisters : m_vertexRegisters;
            const uint32_t registers = (a & 1) ? Device::PixelFloatConstants : Device::VertexFloatConstants;
            if (b <= registers && count <= registers - b)
                memcpy(bank + 4 * b, w + 1, 16 * count);
            w += 1 + 4 * count;
            ++m_constantVersion;
            break;
        }
        case CmdDraw:
        {
            DrawRecord r;
            memcpy(&r, w, sizeof(r));
            w += sizeof(r) / 4;
            render::DrawCall& d = f.draws[r.draw];
            if (!full && !fillState(f, r, d))
            {
                pf::logError("d3d9: frame %u: constant arena full, draws dropped", f.index);
                full = true;
            }
            if (full)
            {
                memset(&d.state, 0, sizeof(d.state));
                d.count = 0;
            }
            break;
        }
        case CmdNativeDraw:
        {
            NativeDrawRecord r;
            memcpy(&r, w, sizeof(r));
            w += sizeof(r) / 4;
            nativeDraw(f, r);
            break;
        }
        case CmdNativeCheck:
        {
            NativeCheckRecord r;
            memcpy(&r, w, sizeof(r));
            w += sizeof(r) / 4;
#if NFSC_RENDER_CHECK
            nativeCheck(f, r, reinterpret_cast<const float*>(w));
#endif
            w += r.vertexFloats + r.fragmentFloats;
            break;
        }
        default:
            pf::fatal("d3d9: frame %u: bad command 0x%08x", f.index, header);
        }
    }
#if NFSC_RENDER_CHECK
    if (++m_checkFrames % 600 == 0)
        pf::logInfo("render check: native draws: %llu compared, state %llu, geometry %llu, vertex constants %llu, "
                    "fragment constants %llu differ, %llu without a translated draw",
                    static_cast<unsigned long long>(m_check.draws), static_cast<unsigned long long>(m_check.state),
                    static_cast<unsigned long long>(m_check.geometry), static_cast<unsigned long long>(m_check.vertex),
                    static_cast<unsigned long long>(m_check.fragment),
                    static_cast<unsigned long long>(m_check.missing));
#endif
    if (pf::perf::enabled() && ++m_alphaFrames == 300)
    {
        char line[256];
        int length = snprintf(line, sizeof(line), "perf: alpha-tested draws per frame:");
        for (uint32_t i = 0; i < AlphaResultCount && length > 0 && length < int(sizeof(line)); ++i)
            length += snprintf(line + length, sizeof(line) - length, " %s %.1f", s_alphaNames[i],
                               double(m_alphaCounts[i]) / m_alphaFrames);
        pf::logInfo("%s", line);
        m_alphaFrames = 0;
        memset(m_alphaCounts, 0, sizeof(m_alphaCounts));
    }
    s_resolved.store(f.index, std::memory_order_release);
}

}

void Stream::grow(uint32_t words)
{
    uint32_t capacity = this->capacity ? this->capacity : 64 * 1024;
    while (capacity < size + words)
        capacity *= 2;
    uint32_t* grown = static_cast<uint32_t*>(realloc(data, capacity * 4));
    if (!grown)
        pf::fatal("d3d9: out of memory for a %u KB frame recording", capacity / 256);
    data = grown;
    this->capacity = capacity;
}

Stream& frameStream(uint32_t frameIndex)
{
    if (frameIndex > StreamCount && s_resolved.load(std::memory_order_acquire) < frameIndex - StreamCount)
        pf::fatal("d3d9: frame %u begun before frame %u was resolved", frameIndex, frameIndex - StreamCount);
    Stream& s = s_streams[frameIndex % StreamCount];
    s.size = 0;
    return s;
}

void resolveFrame(render::Frame& frame)
{
    s_resolver.resolve(frame);
}

}}
