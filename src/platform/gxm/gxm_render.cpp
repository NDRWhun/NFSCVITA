// Direct3D frames on SceGxm: GPU heaps, resources, frame slots and scene drawing (gxm_render.h).

#include "gxm_render.h"
#include <platform/config.h>
#include <platform/cores.h>
#include <platform/log.h>
#include <platform/perf.h>
#include <platform/texture.h>
#include <psp2/kernel/error.h>
#include <algorithm>
#include <cstdio>
#include <cstring>

#define NFSC_FRAGMENT(X, name) X(name) X(name##_atest)
#define NFSC_SHADERS(X) \
    X(ff_v) X(ff_screen_v) X(clearz_v) \
    X(ff_color_f) X(ff_color_f_atest) X(ff_tex_f) X(ff_tex_f_atest) X(ff_proj_f) X(ff_proj_f_atest) X(clear_f) \
    X(fe_v) X(femask_v) X(filter_v) X(screenfilter_v) X(pip_v) X(screen_v) X(car_v) X(glass_v) X(glasslow_v) \
    X(carnm_v) X(ucap_v) X(world_v) X(worldlow_v) X(prelit_v) X(sky_v) X(worldnm_v) X(road_v) X(worldbone_v) \
    X(instance_v) X(water_v) X(particle_v) X(streak_v) X(flare_v) X(raindrop_v) X(ghost_v) X(rvm_v) \
    X(caster_v) X(casterdepth_v) X(depth_v) X(depthskinned_v) X(motionblur_v) X(carnm1_v) X(glass1_v) X(flare1_v) X(road1_v) \
    X(world1_v) X(worldnm1_v) X(ucap1_v) \
    X(car_v_branch) X(glass_v_branch) X(glasslow_v_branch) X(carnm_v_branch) X(world_v_branch) X(worldlow_v_branch) \
    X(prelit_v_branch) X(worldnm_v_branch) X(road_v_branch) X(carnm1_v_branch) X(glass1_v_branch) X(road1_v_branch) \
    X(world1_v_branch) X(worldnm1_v_branch) \
    NFSC_FRAGMENT(X, fe_f) NFSC_FRAGMENT(X, femask_f) NFSC_FRAGMENT(X, filter1_f) NFSC_FRAGMENT(X, filter2_f) \
    NFSC_FRAGMENT(X, filter3_f) NFSC_FRAGMENT(X, filter4_f) NFSC_FRAGMENT(X, filterlum_f) \
    NFSC_FRAGMENT(X, filterpremul_f) NFSC_FRAGMENT(X, screenfilter_f) NFSC_FRAGMENT(X, screenfiltergain_f) \
    NFSC_FRAGMENT(X, pip_f) NFSC_FRAGMENT(X, vt_f) NFSC_FRAGMENT(X, passthru_f) NFSC_FRAGMENT(X, uves_f) \
    NFSC_FRAGMENT(X, uvesdarken_f) NFSC_FRAGMENT(X, blend_f) NFSC_FRAGMENT(X, combine_f) NFSC_FRAGMENT(X, down2_f) \
    NFSC_FRAGMENT(X, down2mb_f) NFSC_FRAGMENT(X, down4_f) NFSC_FRAGMENT(X, downbloom_f) NFSC_FRAGMENT(X, gauss_f) \
    NFSC_FRAGMENT(X, blur15_f) NFSC_FRAGMENT(X, car_f) NFSC_FRAGMENT(X, glass_f) NFSC_FRAGMENT(X, glasslow_f) \
    NFSC_FRAGMENT(X, carnm_f) NFSC_FRAGMENT(X, ucap_f) NFSC_FRAGMENT(X, world_f) NFSC_FRAGMENT(X, modulate_f) \
    NFSC_FRAGMENT(X, prelit_f) NFSC_FRAGMENT(X, fefog_f) NFSC_FRAGMENT(X, worldnm_f) NFSC_FRAGMENT(X, road_f) \
    NFSC_FRAGMENT(X, texture_f) NFSC_FRAGMENT(X, color_f) NFSC_FRAGMENT(X, water_f) NFSC_FRAGMENT(X, particle_f) \
    NFSC_FRAGMENT(X, streak_f) NFSC_FRAGMENT(X, flare_f) NFSC_FRAGMENT(X, raindrop_f) NFSC_FRAGMENT(X, ghost_f) \
    NFSC_FRAGMENT(X, rvm_f) NFSC_FRAGMENT(X, castergreen_f) NFSC_FRAGMENT(X, casterblack_f) \
    NFSC_FRAGMENT(X, casterdepth_f) NFSC_FRAGMENT(X, vtx_f) NFSC_FRAGMENT(X, depthtexture_f) \
    NFSC_FRAGMENT(X, motionblur_f) NFSC_FRAGMENT(X, composite_f) NFSC_FRAGMENT(X, car1_f) \
    NFSC_FRAGMENT(X, carnm1_f) NFSC_FRAGMENT(X, glass1_f) NFSC_FRAGMENT(X, pip1_f) NFSC_FRAGMENT(X, rvm1_f) \
    NFSC_FRAGMENT(X, vt1_f) NFSC_FRAGMENT(X, particle1_f) NFSC_FRAGMENT(X, road1_f) NFSC_FRAGMENT(X, opaque_f) \
    NFSC_FRAGMENT(X, textureopaque_f) NFSC_FRAGMENT(X, down4tent_f) NFSC_FRAGMENT(X, downbloomtent_f) \
    NFSC_FRAGMENT(X, vtnearest_f) NFSC_FRAGMENT(X, vtxnearest_f)

extern "C" {
#define NFSC_DECLARE_SHADER(name) extern const unsigned char shader_##name[];
NFSC_SHADERS(NFSC_DECLARE_SHADER)
}

namespace pf { namespace gxm {

namespace
{

struct ShaderBinary
{
    const char*             name;
    const unsigned char*    data;
};

#define NFSC_SHADER_ENTRY(name) { #name, shader_##name },
const ShaderBinary s_binaries[] = { NFSC_SHADERS(NFSC_SHADER_ENTRY) };

const unsigned char* binary(const char* name)
{
    for (const ShaderBinary& b : s_binaries)
    {
        if (strcmp(b.name, name) == 0)
            return b.data;
    }
    return nullptr;
}

uint32_t alignUp(uint32_t value, uint32_t alignment)
{
    return (value + alignment - 1) & ~(alignment - 1);
}

const uint32_t SlotVertexBytes = 4 * 1024 * 1024;
const uint32_t SlotDraws = 16384;
const uint32_t SlotConstants = 512 * 1024;      // floats
const uint32_t SlotScenes = 128;
const uint32_t SlotViewports = 512;
const uint32_t NotificationBase = 8;            // notification words 0..7 belong to the present path
const unsigned int NoUnit = ~0u;                // a sampler the program does not read
const uint32_t LodBiasZero = 31;                // texture LOD bias field for a bias of 0

SceGxmTextureFormat textureFormat(texture::Format format)
{
    switch (format)
    {
    case texture::FormatBC1:     return SCE_GXM_TEXTURE_FORMAT_UBC1_ABGR;
    case texture::FormatBC2:     return SCE_GXM_TEXTURE_FORMAT_UBC2_ABGR;
    case texture::FormatBC3:     return SCE_GXM_TEXTURE_FORMAT_UBC3_ABGR;
    case texture::FormatRGBA16F: return SCE_GXM_TEXTURE_FORMAT_F16F16F16F16_ABGR;
    case texture::FormatDepth:   return SCE_GXM_TEXTURE_FORMAT_F32M_RRRR;
    default:                     return SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ARGB;
    }
}

// The smallest alpha of a written level of a 2D texture (render::TextureStorage::levelAlpha).
void noteAlpha(render::TextureStorage& t, uint32_t level, const void* data, uint32_t pitch)
{
    const render::TextureDesc& d = t.desc;
    if (d.cube || d.target || d.yuv || level >= 16)
        return;
    t.levelAlpha[level].store(texture::minAlpha(d.format, d.width, d.height, level, data, pitch),
                              std::memory_order_relaxed);
}

void copyAlpha(render::TextureStorage& to, const render::TextureStorage& from)
{
    for (uint32_t level = 0; level < 16; ++level)
        to.levelAlpha[level].store(from.levelAlpha[level].load(std::memory_order_relaxed), std::memory_order_relaxed);
}

SceGxmBlendFactor blendFactor(uint8_t factor)
{
    switch (factor)
    {
    case 1:  return SCE_GXM_BLEND_FACTOR_ZERO;
    case 3:  return SCE_GXM_BLEND_FACTOR_SRC_COLOR;
    case 4:  return SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 5:  return SCE_GXM_BLEND_FACTOR_SRC_ALPHA;
    case 6:  return SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 7:  return SCE_GXM_BLEND_FACTOR_DST_ALPHA;
    case 8:  return SCE_GXM_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 9:  return SCE_GXM_BLEND_FACTOR_DST_COLOR;
    case 10: return SCE_GXM_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 11: return SCE_GXM_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    default: return SCE_GXM_BLEND_FACTOR_ONE;       // ONE (the layer resolves BLENDFACTOR, INVBLENDFACTOR)
    }
}

// The alpha channel reads the alpha of a colour factor, as in Direct3D.
SceGxmBlendFactor alphaFactor(SceGxmBlendFactor factor)
{
    switch (factor)
    {
    case SCE_GXM_BLEND_FACTOR_SRC_COLOR:           return SCE_GXM_BLEND_FACTOR_SRC_ALPHA;
    case SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_COLOR: return SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case SCE_GXM_BLEND_FACTOR_DST_COLOR:           return SCE_GXM_BLEND_FACTOR_DST_ALPHA;
    case SCE_GXM_BLEND_FACTOR_ONE_MINUS_DST_COLOR: return SCE_GXM_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case SCE_GXM_BLEND_FACTOR_SRC_ALPHA_SATURATE:  return SCE_GXM_BLEND_FACTOR_ONE;
    default:                                       return factor;
    }
}

// Program variant: 0 plain, 1 alpha test, 2.. blend factor outputs (FrameRenderer::Variants).
uint32_t variant(const render::DrawState& state)
{
    return state.factorOutput ? 1u + state.factorOutput : state.alphaTest ? 1u : 0u;
}

SceGxmAttributeFormat attributeFormat(uint8_t type, uint8_t* components)
{
    switch (type)
    {
    case render::TypeFloat1:  *components = 1; return SCE_GXM_ATTRIBUTE_FORMAT_F32;
    case render::TypeFloat2:  *components = 2; return SCE_GXM_ATTRIBUTE_FORMAT_F32;
    case render::TypeFloat3:  *components = 3; return SCE_GXM_ATTRIBUTE_FORMAT_F32;
    case render::TypeFloat4:  *components = 4; return SCE_GXM_ATTRIBUTE_FORMAT_F32;
    case render::TypeColor:
    case render::TypeUByte4N: *components = 4; return SCE_GXM_ATTRIBUTE_FORMAT_U8N;
    case render::TypeUByte4:  *components = 4; return SCE_GXM_ATTRIBUTE_FORMAT_U8;
    case render::TypeShort2:  *components = 2; return SCE_GXM_ATTRIBUTE_FORMAT_S16;
    case render::TypeShort4:  *components = 4; return SCE_GXM_ATTRIBUTE_FORMAT_S16;
    default:                  *components = 4; return SCE_GXM_ATTRIBUTE_FORMAT_F32;
    }
}

SceGxmPrimitiveType primitiveType(uint8_t primitive)
{
    return primitive == render::PrimitiveStrip ? SCE_GXM_PRIMITIVE_TRIANGLE_STRIP
         : primitive == render::PrimitiveFan ? SCE_GXM_PRIMITIVE_TRIANGLE_FAN : SCE_GXM_PRIMITIVE_TRIANGLES;
}

bool powerOfTwo(uint32_t v)
{
    return v && !(v & (v - 1));
}

// Default uniform buffer bytes of a program: its own size, and at least what the fields write.
uint32_t uniformBytes(const SceGxmProgram* program, const render::UniformField* fields,
                      const std::vector<const SceGxmProgramParameter*>& parameters)
{
    uint32_t bytes = sceGxmProgramGetDefaultUniformBufferSize(program);
    for (size_t i = 0; i < parameters.size(); ++i)
    {
        if (parameters[i])
            bytes = std::max<uint32_t>(bytes, (sceGxmProgramParameterGetResourceIndex(parameters[i]) + fields[i].count) * 4);
    }
    return alignUp(bytes, 16);
}

}

// ---- HostArena -----------------------------------------------------------------------------------------------------

HostArena::~HostArena()
{
    for (uint8_t* b : m_blocks)
        delete[] b;
}

void* HostArena::allocate(uint32_t size)
{
    size = alignUp(size, 16);
    if (m_block < m_blocks.size() && m_used + size > BlockSize)
    {
        ++m_block;
        m_used = 0;
    }
    if (m_block == m_blocks.size())
        m_blocks.push_back(new uint8_t[BlockSize]);
    void* p = m_blocks[m_block] + m_used;
    m_used += size;
    return p;
}

// ---- FramePool -----------------------------------------------------------------------------------------------------

FramePool::~FramePool()
{
    for (Chunk& c : m_chunks)
    {
        sceGxmUnmapMemory(c.base);
        sceKernelFreeMemBlock(c.uid);
    }
}

void FramePool::begin(uint32_t frameIndex, Finished finished, void* context)
{
    m_frame = frameIndex;
    m_finished = finished;
    m_context = context;
    if (m_current >= 0)
        m_chunks[m_current].lastUse = frameIndex;
}

int FramePool::freeChunk(uint32_t minimum)
{
    for (size_t i = 0; i < m_chunks.size(); ++i)
    {
        Chunk& c = m_chunks[i];
        if (int(i) != m_current && c.size >= minimum && (c.lastUse == 0 || m_finished(m_context, c.lastUse)))
        {
            c.used = 0;
            return int(i);
        }
    }
    uint32_t size = alignUp(std::max(minimum, m_chunkSize), 4096);
    SceUID uid = sceKernelAllocMemBlock(m_name, SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_NC_RW, size, nullptr);
    if (uint32_t(uid) == uint32_t(SCE_KERNEL_ERROR_NO_FREE_PHYSICAL_PAGE))
    {
        size = alignUp(size, 1024 * 1024);
        uid = sceKernelAllocMemBlock(m_name, SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW, size, nullptr);
    }
    if (uid < 0)
        fatal("gxm: %s: out of memory for %u KB: 0x%08x", m_name, size / 1024, uid);
    void* base = nullptr;
    sceKernelGetMemBlockBase(uid, &base);
    if (sceGxmMapMemory(base, size, SCE_GXM_MEMORY_ATTRIB_READ) < 0)
        fatal("gxm: %s: sceGxmMapMemory failed", m_name);
    m_chunks.push_back(Chunk{ uid, static_cast<uint8_t*>(base), size, 0, 0 });
    logInfo("gxm: %s pool +%u KB", m_name, size / 1024);
    return int(m_chunks.size() - 1);
}

void* FramePool::allocate(uint32_t size, uint32_t alignment)
{
    if (m_current >= 0)
    {
        Chunk& c = m_chunks[m_current];
        const uint32_t start = alignUp(c.used, alignment);
        if (start + size <= c.size)
        {
            c.used = start + size;
            c.lastUse = m_frame;
            return c.base + start;
        }
    }
    m_current = freeChunk(size + alignment);
    Chunk& c = m_chunks[m_current];
    c.used = size;
    c.lastUse = m_frame;
    return c.base;
}

void* FramePool::take(uint32_t minimum, uint32_t* size)
{
    m_current = freeChunk(minimum);
    Chunk& c = m_chunks[m_current];
    c.used = c.size;
    c.lastUse = m_frame;
    *size = c.size;
    return c.base;
}

// ---- GpuHeap -------------------------------------------------------------------------------------------------------

GpuHeap::GpuHeap(const char* name, SceKernelMemBlockType type, uint32_t chunkSize, uint32_t attributes)
    :   m_name(name), m_type(type), m_chunkSize(chunkSize), m_attributes(attributes)
{
}

GpuHeap::~GpuHeap()
{
    for (Chunk* c : m_chunks)
    {
        sceGxmUnmapMemory(c->base);
        sceKernelFreeMemBlock(c->uid);
        delete c;
    }
}

bool GpuHeap::addChunk(uint32_t minimum)
{
    // A full chunk of the heap's own memory first, then a full chunk of CDRAM, then just enough of either.
    const SceKernelMemBlockType cdram = SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW;
    const struct { SceKernelMemBlockType type; uint32_t want; } tries[] = {
        { m_type, std::max(minimum, m_chunkSize) }, { cdram, std::max(minimum, m_chunkSize) },
        { m_type, minimum }, { cdram, minimum },
    };
    SceUID uid = -1;
    uint32_t size = 0;
    SceKernelMemBlockType type = m_type;
    for (const auto& t : tries)
    {
        if (uid >= 0)
            break;
        type = t.type;
        size = alignUp(t.want, type == cdram ? 256 * 1024 : 4096);
        uid = sceKernelAllocMemBlock(m_name, type, size, nullptr);
    }
    if (uid < 0)
    {
        logError("gxm: %s heap cannot grow by %u KB (0x%08x)", m_name, alignUp(minimum, 4096) / 1024, unsigned(uid));
        return false;
    }
    void* base = nullptr;
    sceKernelGetMemBlockBase(uid, &base);
    if (sceGxmMapMemory(base, size, SceGxmMemoryAttribFlags(m_attributes)) < 0)
    {
        sceKernelFreeMemBlock(uid);
        return false;
    }
    Chunk* c = new Chunk{ uid, static_cast<uint8_t*>(base), size, {} };
    c->holes[0] = size;
    m_chunks.push_back(c);
    logInfo("gxm: %s heap +%u KB%s", m_name, size / 1024, type != m_type ? " in CDRAM" : "");
    return true;
}

void* GpuHeap::allocate(uint32_t size, uint32_t alignment)
{
    size = alignUp(size ? size : 1, 16);
    for (int attempt = 0; attempt < 2; ++attempt)
    {
        for (Chunk* c : m_chunks)
        {
            for (auto it = c->holes.begin(); it != c->holes.end(); ++it)
            {
                const uint32_t start = alignUp(it->first, alignment);
                if (start + size > it->first + it->second)
                    continue;
                const uint32_t holeStart = it->first, holeEnd = it->first + it->second;
                c->holes.erase(it);
                if (start > holeStart)
                    c->holes[holeStart] = start - holeStart;
                if (start + size < holeEnd)
                    c->holes[start + size] = holeEnd - (start + size);
                void* p = c->base + start;
                m_used[p] = std::make_pair(c, size);
                return p;
            }
        }
        if (attempt == 0 && !addChunk(size + alignment))
            return nullptr;
    }
    return nullptr;
}

void GpuHeap::free(void* pointer)
{
    auto it = m_used.find(pointer);
    if (it == m_used.end())
        return;
    Chunk* c = it->second.first;
    uint32_t start = uint32_t(static_cast<uint8_t*>(pointer) - c->base);
    uint32_t size = it->second.second;
    m_used.erase(it);
    auto next = c->holes.lower_bound(start);
    if (next != c->holes.end() && next->first == start + size)
    {
        size += next->second;
        next = c->holes.erase(next);
    }
    if (next != c->holes.begin())
    {
        auto previous = std::prev(next);
        if (previous->first + previous->second == start)
        {
            start = previous->first;
            size += previous->second;
            c->holes.erase(previous);
        }
    }
    c->holes[start] = size;
}

// ---- Initialisation ------------------------------------------------------------------------------------------------

bool FrameRenderer::init(SceGxmContext* context, SceGxmShaderPatcher* patcher, volatile unsigned int* notifications)
{
    m_context = context;
    m_patcher = patcher;
    m_main.context = context;
    m_precomputed = Config::getBool("gxm_precomputed", true);
    logInfo("gxm: %s draws", m_precomputed ? "precomputed" : "immediate");
    m_trilinear = Config::getBool("trilinear", true);
    m_drawSort = std::min(2, std::max(0, Config::getInt("draw_sort", 1)));
    logInfo("gxm: alpha-tested draws %s (draw_sort=%d)", m_drawSort == 2 ? "after opaque draws, opaque draws first"
            : m_drawSort ? "after opaque draws" : "in game order", m_drawSort);
    m_tiledTargets = Config::getBool("tiled_targets", true);
    m_textureHeap = new GpuHeap("gxm_textures", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, 16 * 1024 * 1024,
                                SCE_GXM_MEMORY_ATTRIB_RW);
    m_bufferHeap = new GpuHeap("gxm_buffers", SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_NC_RW, 8 * 1024 * 1024,
                               SCE_GXM_MEMORY_ATTRIB_READ);

    // Programs.
    auto registerProgram = [this](const char* name, SceGxmShaderPatcherId* id) -> const SceGxmProgram* {
        const unsigned char* data = binary(name);
        if (!data)
            fatal("shader %s is not linked", name);
        const SceGxmProgram* program = reinterpret_cast<const SceGxmProgram*>(data);
        if (sceGxmProgramCheck(program) < 0 || sceGxmShaderPatcherRegisterProgram(m_patcher, program, id) < 0)
            fatal("shader %s cannot be registered", name);
        return program;
    };
    // Setting vertex_branchless=0: the vertex programs with the lit power behind a branch, where there is one.
    const bool branchless = Config::getBool("vertex_branchless", true);
    for (int v = 0; v < render::VpCount; ++v)
    {
        const render::VertexProgramInfo& info = render::vertexProgram(render::VertexProgram(v));
        VertexProgram& p = m_vertexPrograms[v];
        char name[64];
        snprintf(name, sizeof(name), "%s_branch", info.name);
        p.program = registerProgram(!branchless && binary(name) ? name : info.name, &p.id);
        for (uint8_t i = 0; i < info.fieldCount; ++i)
            p.fields.push_back(sceGxmProgramFindParameterByName(p.program, info.fields[i].name));
        p.uniformBytes = uniformBytes(p.program, info.fields, p.fields);
    }
    for (int f = 0; f < render::FpCount; ++f)
    {
        const render::FragmentProgramInfo& info = render::fragmentProgram(render::FragmentProgram(f));
        for (int v = 0; v < Variants; ++v)
        {
            // Without the variant: the plain program (the layer never selects it).
            if ((v == 1 && f == render::FpClear) || (v >= 2 && !info.blendFactor))
            {
                m_fragmentPrograms[f][v] = m_fragmentPrograms[f][0];
                m_fragmentPrograms[f][v].owned = false;
                continue;
            }
            char name[64];
            snprintf(name, sizeof(name), "%s%s", info.name,
                     v == 1 ? "_atest" : render::factorSuffix(render::FactorOutput(v >= 2 ? v - 1 : 0)));
            FragmentProgram& p = m_fragmentPrograms[f][v];
            p.owned = true;
            p.program = registerProgram(name, &p.id);
            for (uint8_t i = 0; i < info.fieldCount; ++i)
                p.fields.push_back(sceGxmProgramFindParameterByName(p.program, info.fields[i].name));
            p.uniformBytes = uniformBytes(p.program, info.fields, p.fields);
            for (uint8_t s = 0; s < info.samplers; ++s)
            {
                char sampler[8] = "uTex0";
                sampler[4] = char('0' + s);
                const SceGxmProgramParameter* parameter = sceGxmProgramFindParameterByName(p.program, sampler);
                p.units[s] = parameter ? sceGxmProgramParameterGetResourceIndex(parameter) : NoUnit;
            }
        }
    }

    // 0..65535 for draws without indices.
    m_sequenceUid = sceKernelAllocMemBlock("gxm_sequence", SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_NC_RW, 128 * 1024, nullptr);
    if (m_sequenceUid < 0)
        return false;
    void* sequence = nullptr;
    sceKernelGetMemBlockBase(m_sequenceUid, &sequence);
    sceGxmMapMemory(sequence, 128 * 1024, SCE_GXM_MEMORY_ATTRIB_READ);
    m_sequence = static_cast<uint16_t*>(sequence);
    for (uint32_t i = 0; i < 65536; ++i)
        m_sequence[i] = uint16_t(i);

    // Frame slots: host arrays, a GPU-read vertex arena each.
    m_arenaUid = sceKernelAllocMemBlock("gxm_frames", SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_NC_RW,
                                        Slots * SlotVertexBytes, nullptr);
    if (m_arenaUid < 0)
        return false;
    void* arena = nullptr;
    sceKernelGetMemBlockBase(m_arenaUid, &arena);
    sceGxmMapMemory(arena, Slots * SlotVertexBytes, SCE_GXM_MEMORY_ATTRIB_READ);
    for (uint32_t i = 0; i < Slots; ++i)
    {
        Slot& s = m_slots[i];
        s.scenes.resize(SlotScenes);
        s.draws.resize(SlotDraws);
        s.viewports.resize(SlotViewports);
        s.constants.resize(SlotConstants);
        s.vertices = static_cast<uint8_t*>(arena) + i * SlotVertexBytes;
        s.done.address = notifications + NotificationBase + i;
        *s.done.address = 0;
        s.done.value = 0;
    }

    render::TextureDesc black = { texture::FormatARGB8888, 1, false, false, 8, 8 };
    m_black = static_cast<Texture*>(createTexture(black));
    if (!m_black)
        return false;
    memset(m_black->data, 0, m_black->bytes);
    for (uint32_t i = 0; i < 64; ++i)
        static_cast<uint8_t*>(m_black->data)[i * 4 + 3] = 0xff;
    render::TextureDesc ramp = { texture::FormatARGB8888, 1, false, false, 256, 1 };
    ramp.linear = true;
    for (Slot& s : m_slots)
    {
        s.ramp = static_cast<Texture*>(createTexture(ramp));
        if (!s.ramp)
            return false;
    }
    initLists();
    return true;
}

void FrameRenderer::shutdown()
{
    shutdownLists();
    for (auto& v : m_vertexCache)
        sceGxmShaderPatcherReleaseVertexProgram(m_patcher, v.second.program);
    for (auto& f : m_fragmentCache)
        sceGxmShaderPatcherReleaseFragmentProgram(m_patcher, f.second.program);
    for (auto& t : m_targets)
        sceGxmDestroyRenderTarget(t.second);
    for (SceUID block : m_targetBlocks)
        sceKernelFreeMemBlock(block);
    m_targetBlocks.clear();
    for (VertexProgram& p : m_vertexPrograms)
        if (p.id)
            sceGxmShaderPatcherUnregisterProgram(m_patcher, p.id);
    for (int f = 0; f < render::FpCount; ++f)
        for (int v = 0; v < Variants; ++v)
            if (m_fragmentPrograms[f][v].owned && m_fragmentPrograms[f][v].id)
                sceGxmShaderPatcherUnregisterProgram(m_patcher, m_fragmentPrograms[f][v].id);
    delete m_textureHeap;
    delete m_bufferHeap;
    if (m_arenaUid >= 0)
        sceKernelFreeMemBlock(m_arenaUid);
    if (m_sequenceUid >= 0)
        sceKernelFreeMemBlock(m_sequenceUid);
}

// ---- Frames (game thread) ------------------------------------------------------------------------------------------

uint32_t FrameRenderer::completedFrame()
{
    for (Slot& s : m_slots)
    {
        if (s.inFlight && *s.done.address == s.done.value)
        {
            s.inFlight = false;
            m_completed = std::max(m_completed, s.frame.index);
        }
    }
    return m_completed;
}

render::Frame* FrameRenderer::beginFrame(uint32_t width, uint32_t height)
{
    const uint32_t index = m_lastIndex + 1;
    Slot& s = m_slots[index % Slots];
    if (s.inFlight)
    {
        sceGxmNotificationWait(&s.done);
        s.inFlight = false;
        m_completed = std::max(m_completed, s.frame.index);
    }
    completedFrame();
    freePending();

    // Back buffers of another size than the display are drawn offscreen and scaled when shown.
    s.forceOffscreen = false;
    if (width != 960 || height != 544)
        prepareOffscreen(s, width, height);

    m_lastIndex = index;
    render::Frame& f = s.frame;
    f.index = index;
    f.width = uint16_t(width);
    f.height = uint16_t(height);
    f.scenes = s.scenes.data();
    f.sceneCount = 0;
    f.sceneCapacity = uint32_t(s.scenes.size());
    f.draws = s.draws.data();
    f.drawCount = 0;
    f.drawCapacity = uint32_t(s.draws.size());
    f.viewports = s.viewports.data();
    f.viewportCount = 0;
    f.viewportCapacity = uint32_t(s.viewports.size());
    f.constants = s.constants.data();
    f.constantCount = 0;
    f.constantCapacity = uint32_t(s.constants.size());
    f.vertices = static_cast<uint8_t*>(s.vertices);
    f.vertexBytes = 0;
    f.vertexCapacity = SlotVertexBytes;
    f.resolve = nullptr;
    f.commands = nullptr;
    f.commandCount = 0;
    f.gammaIdentity = true;
    f.gammaExponent = 0.0f;
    return &f;
}

void FrameRenderer::prepareOffscreen(Slot& s, uint32_t width, uint32_t height)
{
    if (s.offscreen && s.offscreen->desc.width == width && s.offscreen->desc.height == height)
        return;
    if (s.offscreen)
        release(s.offscreen);
    render::TextureDesc desc = { texture::FormatARGB8888, 1, false, true, uint16_t(width), uint16_t(height) };
    s.offscreen = static_cast<Texture*>(createTexture(desc));
}

render::TextureStorage* FrameRenderer::backBufferTexture(render::Frame* frame)
{
    Slot& s = m_slots[frame->index % Slots];
    prepareOffscreen(s, frame->width, frame->height);
    s.forceOffscreen = s.offscreen != nullptr;
    return s.offscreen;
}

void FrameRenderer::waitFrame(uint32_t index)
{
    if (completedFrame() >= index)
        return;
    Slot& s = m_slots[index % Slots];
    if (s.inFlight && s.frame.index == index)
    {
        sceGxmNotificationWait(&s.done);
        s.inFlight = false;
        m_completed = std::max(m_completed, index);
    }
}

void FrameRenderer::submitted(render::Frame* frame, uint32_t vblanks)
{
    Slot& s = m_slots[frame->index % Slots];
    s.vblanks = vblanks;
    s.done.value = frame->index;
    s.inFlight = true;
    if (s.offscreen)
        s.offscreen->lastUse = frame->index;
}

void FrameRenderer::freePending()
{
    size_t kept = 0;
    for (size_t i = 0; i < m_pending.size(); ++i)
    {
        if (m_pending[i].after <= m_completed)
            destroy(m_pending[i].kind, m_pending[i].object);
        else
            m_pending[kept++] = m_pending[i];
    }
    m_pending.resize(kept);
}

void FrameRenderer::destroy(int kind, void* object)
{
    if (kind == 0)
    {
        Texture* t = static_cast<Texture*>(object);
        m_textureHeap->free(t->data);
        delete static_cast<Depth*>(t->depth);
        delete t;
    }
    else if (kind == 1)
    {
        Depth* d = static_cast<Depth*>(object);
        m_textureHeap->free(d->data);
        m_textureHeap->free(d->stencil);
        delete d;
    }
    else
    {
        Buffer* b = static_cast<Buffer*>(object);
        m_bufferHeap->free(b->data);
        delete b;
    }
}

void FrameRenderer::release(render::TextureStorage* texture)
{
    if (texture)
        m_pending.push_back(Pending{ 0, texture, m_lastIndex + 1 });
}

void FrameRenderer::release(render::DepthStorage* depth)
{
    if (depth)
        m_pending.push_back(Pending{ 1, depth, m_lastIndex + 1 });
}

void FrameRenderer::release(render::BufferStorage* buffer)
{
    if (buffer)
        m_pending.push_back(Pending{ 2, buffer, m_lastIndex + 1 });
}

// ---- Resources (game thread) ---------------------------------------------------------------------------------------

render::TextureStorage* FrameRenderer::createTexture(const render::TextureDesc& desc)
{
    Texture* t = new Texture();
    t->desc = desc;
    const SceGxmTextureFormat format = textureFormat(desc.format);
    if (desc.format == texture::FormatDepth)
    {
        // A tiled 32-bit float depth surface, read by the texture with the same tiled layout.
        if (desc.cube || !desc.target || desc.levels != 1)
        {
            delete t;
            return nullptr;
        }
        const uint32_t w = alignUp(desc.width, SCE_GXM_TILE_SIZEX), h = alignUp(desc.height, SCE_GXM_TILE_SIZEY);
        t->stride = 0;
        t->bytes = w * h * 4;
        t->faceStride = t->bytes;
        t->data = m_textureHeap->allocate(t->bytes, SCE_GXM_DEPTHSTENCIL_SURFACE_ALIGNMENT);
        if (!t->data)
        {
            delete t;
            return nullptr;
        }
        // A new depth texture starts at the far plane.
        std::fill_n(static_cast<float*>(t->data), t->bytes / 4, 1.0f);
        Depth* d = new Depth();
        d->width = desc.width;
        d->height = desc.height;
        d->kept = true;
        d->data = t->data;
        d->stencil = nullptr;
        sceGxmDepthStencilSurfaceInit(&d->surface, SCE_GXM_DEPTH_STENCIL_FORMAT_DF32M,
                                      SCE_GXM_DEPTH_STENCIL_SURFACE_TILED, w, d->data, nullptr);
        sceGxmTextureInitTiled(&t->texture, t->data, format, desc.width, desc.height, 1);
        t->depth = d;
        return t;
    }
    if (desc.target && !desc.cube && desc.levels == 1)
    {
        // Rendered to as a colour surface. Tiled (setting tiled_targets): 32 x 32 pixel tiles, which the texture
        // unit reads with 2D locality; else linear rows of 8-pixel multiples. The CPU never reads or writes the
        // pixels of a target.
        const bool half = desc.format == texture::FormatRGBA16F;
        const SceGxmColorFormat colorFormat = half ? SCE_GXM_COLOR_FORMAT_F16F16F16F16_ABGR
                                                   : SCE_GXM_COLOR_FORMAT_A8R8G8B8;
        const SceGxmOutputRegisterSize registerSize = half ? SCE_GXM_OUTPUT_REGISTER_SIZE_64BIT
                                                           : SCE_GXM_OUTPUT_REGISTER_SIZE_32BIT;
        const bool tiled = m_tiledTargets;
        const uint32_t strideInPixels = alignUp(desc.width, tiled ? SCE_GXM_TILE_SIZEX : 8);
        const uint32_t rows = tiled ? alignUp(desc.height, SCE_GXM_TILE_SIZEY) : desc.height;
        t->bytes = strideInPixels * texture::unitBytes(desc.format) * rows;
        t->faceStride = t->bytes;
        t->data = m_textureHeap->allocate(t->bytes, 256);
        if (!t->data)
        {
            delete t;
            return nullptr;
        }
        // A new render target starts as zero (transparent black).
        memset(t->data, 0, t->bytes);
        t->surfaces.resize(1);
        if (tiled && sceGxmTextureInitTiled(&t->texture, t->data, format, desc.width, desc.height, 1) >= 0
            && sceGxmColorSurfaceInit(&t->surfaces[0], colorFormat, SCE_GXM_COLOR_SURFACE_TILED,
                                      SCE_GXM_COLOR_SURFACE_SCALE_NONE, registerSize, desc.width, desc.height,
                                      strideInPixels, t->data) >= 0)
        {
            t->stride = 0;
            return t;
        }
        if (tiled)
            logError("gxm: a tiled %ux%u target cannot be made, linear instead", desc.width, desc.height);
        t->stride = strideInPixels * texture::unitBytes(desc.format);
        sceGxmTextureInitLinear(&t->texture, t->data, format, desc.width, desc.height, 1);
        sceGxmColorSurfaceInit(&t->surfaces[0], colorFormat, SCE_GXM_COLOR_SURFACE_LINEAR,
                               SCE_GXM_COLOR_SURFACE_SCALE_NONE, registerSize, desc.width, desc.height, strideInPixels,
                               t->data);
        return t;
    }
    if (desc.target && (desc.format != texture::FormatARGB8888 || !powerOfTwo(desc.width) || !powerOfTwo(desc.height)))
    {
        delete t;
        return nullptr;
    }
    if (desc.yuv)
    {
        // Luma rows of the stride, then the U and V planes with half the stride and half the rows.
        if (desc.cube || desc.levels != 1 || (desc.width & 15) || (desc.height & 1))
        {
            delete t;
            return nullptr;
        }
        t->stride = desc.width;
        t->bytes = desc.width * desc.height * 3 / 2;
        t->faceStride = t->bytes;
        t->data = m_textureHeap->allocate(t->bytes, 256);
        if (!t->data)
        {
            delete t;
            return nullptr;
        }
        sceGxmTextureInitLinear(&t->texture, t->data, SCE_GXM_TEXTURE_FORMAT_YUV420P3_CSC0, desc.width, desc.height, 1);
        return t;
    }
    if (desc.linear && !desc.cube && desc.levels == 1 && !texture::isBlock(desc.format))
    {
        // Streamed: linear rows (8-pixel multiples) the CPU rewrites with a row copy instead of a swizzle.
        t->stride = alignUp(desc.width, 8) * texture::unitBytes(desc.format);
        t->bytes = t->stride * desc.height;
        t->faceStride = t->bytes;
        t->data = m_textureHeap->allocate(t->bytes, 256);
        if (!t->data)
        {
            delete t;
            return nullptr;
        }
        sceGxmTextureInitLinear(&t->texture, t->data, format, desc.width, desc.height, 1);
        return t;
    }
    t->stride = 0;
    t->faceStride = texture::faceStride(desc.format, desc.width, desc.height, desc.levels);
    t->bytes = texture::totalBytes(desc.format, desc.width, desc.height, desc.levels, desc.cube);
    t->data = m_textureHeap->allocate(t->bytes, 256);
    if (!t->data)
    {
        delete t;
        return nullptr;
    }
    // Cube textures and targets start as zero (black): faces are filled one at a time, or rendered to.
    if (desc.cube || desc.target)
        memset(t->data, 0, t->bytes);
    if (desc.cube)
        sceGxmTextureInitCube(&t->texture, t->data, format, desc.width, desc.height, desc.levels);
    else
        sceGxmTextureInitSwizzled(&t->texture, t->data, format, desc.width, desc.height, desc.levels);
    if (desc.target)
    {
        // Cube targets and 2D targets with levels: each level of each face a swizzled colour surface.
        const uint32_t faces = desc.cube ? 6 : 1;
        t->surfaces.resize(faces * desc.levels);
        for (uint32_t face = 0; face < faces; ++face)
            for (uint32_t level = 0; level < desc.levels; ++level)
            {
                const uint32_t w = texture::levelDim(desc.width, level), h = texture::levelDim(desc.height, level);
                uint8_t* data = static_cast<uint8_t*>(t->data) + face * t->faceStride
                              + texture::levelOffset(desc.format, desc.width, desc.height, level);
                sceGxmColorSurfaceInit(&t->surfaces[face * desc.levels + level], SCE_GXM_COLOR_FORMAT_A8R8G8B8,
                                       SCE_GXM_COLOR_SURFACE_SWIZZLED, SCE_GXM_COLOR_SURFACE_SCALE_NONE,
                                       SCE_GXM_OUTPUT_REGISTER_SIZE_32BIT, w, h, w, data);
            }
    }
    return t;
}

void FrameRenderer::writeTexture(render::TextureStorage* storage, uint32_t face, uint32_t level, const void* data,
                                 uint32_t pitch)
{
    Texture* t = static_cast<Texture*>(storage);
    const render::TextureDesc& d = t->desc;
    if (level >= d.levels || face >= (d.cube ? 6u : 1u))
        return;
    if (d.yuv)
        fatal("gxm: pixels written to a yuv texture");
    noteAlpha(*t, level, data, pitch);
    uint8_t* base = static_cast<uint8_t*>(t->data) + face * t->faceStride;
    if (t->stride)
    {
        const uint32_t rowBytes = d.width * texture::unitBytes(d.format);
        if (pitch == t->stride)
            memcpy(base, data, size_t(rowBytes) + size_t(t->stride) * (d.height - 1));
        else
            for (uint32_t y = 0; y < d.height; ++y)
                memcpy(base + y * t->stride, static_cast<const uint8_t*>(data) + y * pitch, rowBytes);
        return;
    }
    texture::swizzleLevel(d.format, d.width, d.height, level, data, pitch,
                          base + texture::levelOffset(d.format, d.width, d.height, level));
}

void FrameRenderer::writePlanes(render::TextureStorage* storage, const render::Planes& p)
{
    Texture* t = static_cast<Texture*>(storage);
    const render::TextureDesc& d = t->desc;
    const uint32_t width = std::min<uint32_t>(p.width, d.width), height = std::min<uint32_t>(p.height, d.height);
    uint8_t* y = static_cast<uint8_t*>(t->data);
    for (uint32_t row = 0; row < d.height; ++row, y += d.width)
    {
        const uint32_t n = row < height ? width : 0;
        if (n)
            memcpy(y, p.y + row * p.yStride, n);
        memset(y + n, 16, d.width - n);
    }
    const uint32_t chromaWidth = width / 2, chromaRows = (height + 1) / 2, stride = d.width / 2;
    const uint8_t* const sources[2] = { p.u, p.v };
    for (const uint8_t* source : sources)
    {
        for (uint32_t row = 0; row < d.height / 2; ++row, y += stride)
        {
            const uint32_t n = row < chromaRows ? chromaWidth : 0;
            if (n)
                memcpy(y, source + row * p.uvStride, n);
            memset(y + n, 128, stride - n);
        }
    }
}

void FrameRenderer::readTexture(const render::TextureStorage* storage, uint32_t face, uint32_t level, void* data,
                                uint32_t pitch)
{
    const Texture* t = static_cast<const Texture*>(storage);
    const render::TextureDesc& d = t->desc;
    if (level >= d.levels || face >= (d.cube ? 6u : 1u))
        return;
    if (d.yuv)
    {
        // BT.601 video range to B, G, R, A bytes.
        const uint8_t* y = static_cast<const uint8_t*>(t->data);
        const uint8_t* u = y + d.width * d.height;
        const uint8_t* v = u + d.width * d.height / 4;
        auto clamp = [](int c) { return uint8_t(c < 0 ? 0 : c > 255 ? 255 : c); };
        for (uint32_t row = 0; row < d.height; ++row)
        {
            uint8_t* out = static_cast<uint8_t*>(data) + row * pitch;
            for (uint32_t x = 0; x < d.width; ++x, out += 4)
            {
                const int l = 298 * (y[row * d.width + x] - 16);
                const int cb = u[(row / 2) * (d.width / 2) + x / 2] - 128;
                const int cr = v[(row / 2) * (d.width / 2) + x / 2] - 128;
                out[0] = clamp((l + 516 * cb + 128) >> 8);
                out[1] = clamp((l - 100 * cb - 208 * cr + 128) >> 8);
                out[2] = clamp((l + 409 * cr + 128) >> 8);
                out[3] = 255;
            }
        }
        return;
    }
    const uint8_t* base = static_cast<const uint8_t*>(t->data) + face * t->faceStride;
    if (t->stride)
    {
        const uint32_t rowBytes = d.width * texture::unitBytes(d.format);
        for (uint32_t y = 0; y < d.height; ++y)
            memcpy(static_cast<uint8_t*>(data) + y * pitch, base + y * t->stride, rowBytes);
        return;
    }
    texture::unswizzleLevel(d.format, d.width, d.height, level,
                            base + texture::levelOffset(d.format, d.width, d.height, level), data, pitch);
}

void FrameRenderer::copyTexture(render::TextureStorage* to, const render::TextureStorage* from)
{
    Texture* dst = static_cast<Texture*>(to);
    const Texture* src = static_cast<const Texture*>(from);
    if (dst->bytes == src->bytes && dst->desc.yuv == src->desc.yuv)
    {
        memcpy(dst->data, src->data, dst->bytes);
        copyAlpha(*dst, *src);
    }
}

render::DepthStorage* FrameRenderer::createDepth(uint32_t width, uint32_t height)
{
    const uint32_t w = alignUp(width, SCE_GXM_TILE_SIZEX), h = alignUp(height, SCE_GXM_TILE_SIZEY);
    Depth* d = new Depth();
    d->width = uint16_t(width);
    d->height = uint16_t(height);
    // No stencil plane: the layer never enables the stencil test, so it would only be loaded and stored.
    d->data = m_textureHeap->allocate(w * h * 4, SCE_GXM_DEPTHSTENCIL_SURFACE_ALIGNMENT);
    d->stencil = nullptr;
    if (!d->data)
    {
        delete d;
        return nullptr;
    }
    sceGxmDepthStencilSurfaceInit(&d->surface, SCE_GXM_DEPTH_STENCIL_FORMAT_DF32M, SCE_GXM_DEPTH_STENCIL_SURFACE_TILED,
                                  w, d->data, nullptr);
    return d;
}

render::BufferStorage* FrameRenderer::createBuffer(uint32_t size)
{
    Buffer* b = new Buffer();
    b->size = size;
    b->data = m_bufferHeap->allocate(size, 16);
    if (!b->data)
    {
        delete b;
        return nullptr;
    }
    return b;
}

// ---- Drawing (render thread) ---------------------------------------------------------------------------------------

uint32_t FrameRenderer::vblanks(const render::Frame& frame) const
{
    return m_slots[frame.index % Slots].vblanks;
}

SceGxmNotification* FrameRenderer::doneNotification(const render::Frame& frame)
{
    return &m_slots[frame.index % Slots].done;
}

const SceGxmTexture* FrameRenderer::offscreen(const render::Frame& frame) const
{
    const Slot& s = m_slots[frame.index % Slots];
    return s.direct || !s.offscreen ? nullptr : &s.offscreen->texture;
}

const SceGxmTexture* FrameRenderer::gammaRamp(const render::Frame& frame)
{
    Slot& s = m_slots[frame.index % Slots];
    if (frame.gammaIdentity || !s.ramp)
        return nullptr;
    // The slot's previous frame is finished, so its table is free; texels are bytes B, G, R, A.
    uint8_t* texel = static_cast<uint8_t*>(s.ramp->data);
    for (uint32_t i = 0; i < 256; ++i, texel += 4)
    {
        texel[0] = frame.gammaRamp[2][i];
        texel[1] = frame.gammaRamp[1][i];
        texel[2] = frame.gammaRamp[0][i];
        texel[3] = 255;
    }
    return &s.ramp->texture;
}

bool FrameRenderer::presentScene(const render::Frame& frame) const
{
    if (offscreen(frame))
        return true;
    for (uint32_t i = 0; i < frame.sceneCount; ++i)
    {
        if (!frame.scenes[i].target.color)
            return false;
    }
    return true;
}

SceGxmRenderTarget* FrameRenderer::renderTarget(uint32_t width, uint32_t height)
{
    const uint32_t key = width << 16 | height;
    auto it = m_targets.find(key);
    if (it != m_targets.end())
        return it->second;
    SceGxmRenderTargetParams params;
    memset(&params, 0, sizeof(params));
    params.width = uint16_t(width);
    params.height = uint16_t(height);
    params.scenesPerFrame = 8;
    params.multisampleMode = SCE_GXM_MULTISAMPLE_NONE;
    params.driverMemBlock = -1;
    SceGxmRenderTarget* target = nullptr;
    int result = sceGxmCreateRenderTarget(&params, &target);
    if (uint32_t(result) == uint32_t(SCE_GXM_ERROR_OUT_OF_MEMORY))
    {
        uint32_t bytes = 0;
        result = sceGxmGetRenderTargetMemSize(&params, &bytes);
        if (result < 0)
            fatal("sceGxmGetRenderTargetMemSize(%ux%u) failed: 0x%08x", width, height, result);
        const SceUID block = sceKernelAllocMemBlock("gxm_target", SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW,
                                                   alignUp(bytes, 1024 * 1024), nullptr);
        if (block < 0)
            fatal("render target memory (%ux%u, %u bytes) failed: 0x%08x", width, height, bytes, block);
        params.driverMemBlock = block;
        result = sceGxmCreateRenderTarget(&params, &target);
        if (result < 0)
            sceKernelFreeMemBlock(block);
        else
            m_targetBlocks.push_back(block);
    }
    if (result < 0)
        fatal("sceGxmCreateRenderTarget(%ux%u) failed: 0x%08x", width, height, result);
    m_targets[key] = target;
    return target;
}

const FrameRenderer::VertexEntry* FrameRenderer::vertexProgram(uint8_t program, uint8_t layoutId)
{
    const uint32_t key = uint32_t(program) << 8 | layoutId;
    auto it = m_vertexCache.find(key);
    if (it != m_vertexCache.end())
        return &it->second;
    const VertexProgram& p = m_vertexPrograms[program < render::VpCount ? program : 0];
    const render::VertexLayout& layout = render::layout(layoutId);
    SceGxmVertexAttribute attributes[render::UsageCount];
    unsigned int count = 0;
    for (uint32_t u = 0; u < render::UsageCount; ++u)
    {
        const SceGxmProgramParameter* parameter = sceGxmProgramFindParameterByName(p.program,
                                                                                    render::inputName(render::Usage(u)));
        if (!parameter)
            continue;
        const render::VertexElement* element = nullptr;
        for (uint32_t i = 0; i < layout.count; ++i)
        {
            if (layout.elements[i].usage == u)
                element = &layout.elements[i];
        }
        SceGxmVertexAttribute& a = attributes[count++];
        a.streamIndex = 0;
        a.regIndex = uint16_t(sceGxmProgramParameterGetResourceIndex(parameter));
        if (element)
        {
            a.offset = element->offset;
            a.format = uint8_t(attributeFormat(element->type, &a.componentCount));
        }
        else
        {
            // The layout lacks the input: read the first element (unused by the program's result).
            logError("gxm: vertex program %s: layout %u has no %s", render::vertexProgram(render::VertexProgram(program)).name,
                     layoutId, render::inputName(render::Usage(u)));
            a.offset = layout.count ? layout.elements[0].offset : 0;
            a.format = SCE_GXM_ATTRIBUTE_FORMAT_F32;
            a.componentCount = 1;
        }
    }
    SceGxmVertexStream stream = { layout.stride, SCE_GXM_INDEX_SOURCE_INDEX_16BIT };
    SceGxmVertexProgram* result = nullptr;
    const int error = sceGxmShaderPatcherCreateVertexProgram(m_patcher, p.id, attributes, count, &stream, 1, &result);
    if (error < 0)
        fatal("gxm: vertex program %s for layout %u: 0x%08x", render::vertexProgram(render::VertexProgram(program)).name,
              layoutId, error);
    VertexEntry& entry = m_vertexCache[key];
    entry.program = result;
    entry.source = uint8_t(program < render::VpCount ? program : 0);
    entry.drawSize = sceGxmGetPrecomputedDrawSize(result);
    entry.stateSize = sceGxmGetPrecomputedVertexStateSize(result);
    entry.uniformBytes = p.uniformBytes;
    return &entry;
}

const FrameRenderer::FragmentEntry* FrameRenderer::fragmentProgram(const render::DrawState& state, bool half)
{
    if (half && state.blendEnable)
    {
        static bool logged = false;
        if (!logged)
            logError("gxm: blending into a half float target is drawn without blending");
        logged = true;
    }
    const bool blend = state.blendEnable != 0 && !half;
    const uint8_t mask = state.colorMask;
    const uint32_t v = variant(state);
    const uint64_t key = uint64_t(state.vertexProgram) | uint64_t(state.fragmentProgram) << 8
                       | uint64_t(v) << 16 | uint64_t(blend) << 19 | uint64_t(mask) << 20
                       | (blend ? uint64_t(state.srcBlend) << 24 | uint64_t(state.dstBlend) << 32 : 0)
                       | uint64_t(half) << 40;
    auto it = m_fragmentCache.find(key);
    if (it != m_fragmentCache.end())
        return &it->second;
    const FragmentProgram& p = m_fragmentPrograms[state.fragmentProgram < render::FpCount ? state.fragmentProgram : 0][v];
    SceGxmBlendInfo info;
    memset(&info, 0, sizeof(info));
    info.colorMask = uint8_t(((mask & 1) ? SCE_GXM_COLOR_MASK_R : 0) | ((mask & 2) ? SCE_GXM_COLOR_MASK_G : 0)
                           | ((mask & 4) ? SCE_GXM_COLOR_MASK_B : 0) | ((mask & 8) ? SCE_GXM_COLOR_MASK_A : 0));
    info.colorFunc = SCE_GXM_BLEND_FUNC_ADD;
    info.alphaFunc = SCE_GXM_BLEND_FUNC_ADD;
    uint8_t src = blend ? state.srcBlend : 2, dst = blend ? state.dstBlend : 1;
    if (src == 12 || src == 13)
    {
        // BOTHSRCALPHA, BOTHINVSRCALPHA.
        dst = src == 12 ? 6 : 5;
        src = src == 12 ? 5 : 6;
    }
    const SceGxmBlendFactor colorSrc = blendFactor(src), colorDst = blendFactor(dst);
    info.colorSrc = colorSrc;
    info.colorDst = colorDst;
    info.alphaSrc = alphaFactor(colorSrc);
    info.alphaDst = alphaFactor(colorDst);
    const bool plain = !blend && mask == 0xf;
    const SceGxmProgram* vertex = m_vertexPrograms[state.vertexProgram < render::VpCount ? state.vertexProgram : 0].program;
    SceGxmFragmentProgram* result = nullptr;
    const int error = sceGxmShaderPatcherCreateFragmentProgram(m_patcher, p.id,
                                                               half ? SCE_GXM_OUTPUT_REGISTER_FORMAT_HALF4
                                                                    : SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4,
                                                               SCE_GXM_MULTISAMPLE_NONE, plain ? nullptr : &info,
                                                               vertex, &result);
    if (error < 0)
        fatal("gxm: fragment program %u (vertex %u): 0x%08x", state.fragmentProgram, state.vertexProgram, error);
    FragmentEntry& entry = m_fragmentCache[key];
    entry.program = result;
    entry.stateSize = sceGxmGetPrecomputedFragmentStateSize(result);
    entry.uniformBytes = p.uniformBytes;
    return &entry;
}

bool FrameRenderer::finished(void* self, uint32_t frameIndex)
{
    const Slot& s = static_cast<FrameRenderer*>(self)->m_slots[frameIndex % Slots];
    return *s.done.address >= frameIndex;
}

void FrameRenderer::preparePrograms(const render::Frame& frame)
{
    m_programs.resize(frame.drawCount);
    m_halfTarget.assign(frame.drawCount, 0);
    for (uint32_t i = 0; i < frame.sceneCount; ++i)
    {
        const render::Scene& scene = frame.scenes[i];
        const bool half = scene.target.color && scene.target.color->desc.format == texture::FormatRGBA16F;
        for (uint32_t n = 0; n < scene.drawCount && half; ++n)
            m_halfTarget[scene.firstDraw + n] = 1;
    }
    DrawPrograms last = { nullptr, nullptr };
    const render::DrawState* previous = nullptr;
    bool previousHalf = false;
    for (uint32_t i = 0; i < frame.drawCount; ++i)
    {
        const render::DrawState& s = frame.draws[i].state;
        if (!frame.draws[i].count)
        {
            m_programs[i] = DrawPrograms{ nullptr, nullptr };
            continue;
        }
        const bool half = m_halfTarget[i] != 0;
        // Consecutive draws mostly share their programs.
        if (!previous || s.vertexProgram != previous->vertexProgram || s.layout != previous->layout)
            last.vertex = vertexProgram(s.vertexProgram, s.layout);
        if (!previous || s.vertexProgram != previous->vertexProgram || s.fragmentProgram != previous->fragmentProgram
            || s.alphaTest != previous->alphaTest || s.factorOutput != previous->factorOutput
            || s.blendEnable != previous->blendEnable || s.srcBlend != previous->srcBlend
            || s.dstBlend != previous->dstBlend || s.colorMask != previous->colorMask || half != previousHalf)
            last.fragment = fragmentProgram(s, half);
        m_programs[i] = last;
        previous = &s;
        previousHalf = half;
    }
}

void FrameRenderer::upload(void* buffer, const render::UniformField* fields, uint8_t count,
                           const std::vector<const SceGxmProgramParameter*>& parameters, const float* constants)
{
    for (uint8_t i = 0; i < count && i < parameters.size(); ++i)
    {
        if (parameters[i])
            sceGxmSetUniformDataF(buffer, parameters[i], 0, fields[i].count, constants + fields[i].offset);
    }
}

// Texture of sampler i of a draw with the sampler's modes; black without a texture.
SceGxmTexture FrameRenderer::samplerTexture(const render::DrawState& s, uint32_t i) const
{
    const Texture* t = s.textures[i] ? static_cast<const Texture*>(s.textures[i]) : m_black;
    SceGxmTexture texture = t->texture;
    const render::Sampler& sm = s.samplers[i];
    if (sm.level && sm.level < t->desc.levels && !t->desc.cube)
    {
        // One lower level of a swizzled texture, read as a texture of its own.
        const render::TextureDesc& d = t->desc;
        sceGxmTextureInitSwizzled(&texture, static_cast<uint8_t*>(t->data)
                                  + texture::levelOffset(d.format, d.width, d.height, sm.level), textureFormat(d.format),
                                  texture::levelDim(d.width, sm.level), texture::levelDim(d.height, sm.level), 1);
    }
    const bool pow2 = powerOfTwo(t->desc.width) && powerOfTwo(t->desc.height);
    auto mode = [pow2](uint8_t a) {
        return !pow2 || a == render::AddressClamp ? SCE_GXM_TEXTURE_ADDR_CLAMP
             : a == render::AddressMirror ? SCE_GXM_TEXTURE_ADDR_MIRROR : SCE_GXM_TEXTURE_ADDR_REPEAT;
    };
    if (!t->desc.cube)
    {
        sceGxmTextureSetUAddrMode(&texture, mode(sm.addressU));
        sceGxmTextureSetVAddrMode(&texture, mode(sm.addressV));
    }
    // Depth is read unfiltered: the program compares each sample.
    const uint8_t filter = t->desc.format == texture::FormatDepth ? 0 : sm.filter;
    sceGxmTextureSetMinFilter(&texture, (filter & 1) ? SCE_GXM_TEXTURE_FILTER_LINEAR : SCE_GXM_TEXTURE_FILTER_POINT);
    sceGxmTextureSetMagFilter(&texture, (filter & 2) ? SCE_GXM_TEXTURE_FILTER_LINEAR : SCE_GXM_TEXTURE_FILTER_POINT);
    if (t->desc.levels > 1)
    {
        if (sm.mip == render::MipNone)
            sceGxmTextureSetMipmapCount(&texture, 1);
        sceGxmTextureSetMipFilter(&texture, sm.mip == render::MipLinear && m_trilinear
                                            ? SCE_GXM_TEXTURE_MIP_FILTER_ENABLED : SCE_GXM_TEXTURE_MIP_FILTER_DISABLED);
        sceGxmTextureSetLodBias(&texture, uint32_t(std::min(63, std::max(0, int(LodBiasZero) + sm.lodBias))));
    }
    return texture;
}

// Viewport, depth test and culling of a draw, where they changed.
void FrameRenderer::setFixedState(ContextState& c, SceGxmContext* context, const render::Frame& frame,
                                  const render::DrawCall& d, bool first)
{
    const render::DrawState& s = d.state;
    if (first || d.viewport != c.viewport)
    {
        // Direct3D window coordinates have pixel centres at integers: half a pixel to GXM's.
        // A back buffer drawn to the display directly: its pixels scaled to the display's.
        const render::Viewport& v = frame.viewports[d.viewport];
        const float sx = c.scaleX, sy = c.scaleY;
        sceGxmSetViewport(context, sx * (v.x + 0.5f * v.width + 0.5f), sx * 0.5f * v.width,
                          sy * (v.y + 0.5f * v.height + 0.5f), sy * -0.5f * v.height, v.minZ, v.maxZ - v.minZ);
        c.viewport = d.viewport;
    }
    if (first || s.depthEnable != c.last.depthEnable || s.depthWrite != c.last.depthWrite
        || s.depthFunc != c.last.depthFunc)
    {
        // Without a depth surface the depth test is off, as in Direct3D.
        const bool test = c.depth && s.depthEnable;
        const uint32_t func = test && s.depthFunc >= 1 && s.depthFunc <= 8 ? s.depthFunc - 1u : 7u;
        sceGxmSetFrontDepthFunc(context, SceGxmDepthFunc(func << 22));
        sceGxmSetFrontDepthWriteEnable(context, test && s.depthWrite ? SCE_GXM_DEPTH_WRITE_ENABLED
                                                                     : SCE_GXM_DEPTH_WRITE_DISABLED);
    }
    if (first || s.cullMode != c.last.cullMode)
        sceGxmSetCullMode(context, s.cullMode == 2 ? SCE_GXM_CULL_CW : s.cullMode == 3 ? SCE_GXM_CULL_CCW
                                                                                        : SCE_GXM_CULL_NONE);
}

void FrameRenderer::draw(Recorder& r, const render::Frame& frame, uint32_t index)
{
    const render::DrawCall& d = frame.draws[index];
    if (!d.count)
        return;     // dropped by the layer
    if (!d.indices && d.count > 65536)
    {
        logError("gxm: draw of %u vertices without indices skipped", d.count);
        return;
    }
    if (m_precomputed.load(std::memory_order_relaxed))
    {
        drawPrecomputed(r, frame, index);
        return;
    }
    ContextState& c = r.state;
    SceGxmContext* context = r.context;
    const DrawPrograms& pr = m_programs[index];
    const render::DrawState& s = d.state;
    const bool first = !c.valid;
    const bool programs = first || pr.vertex != c.vertex || pr.fragment != c.fragment;
    if (programs)
    {
        sceGxmSetVertexProgram(context, pr.vertex->program);
        sceGxmSetFragmentProgram(context, pr.fragment->program);
    }
    const render::VertexProgramInfo& vinfo = render::vertexProgram(render::VertexProgram(s.vertexProgram));
    const render::FragmentProgramInfo& finfo = render::fragmentProgram(render::FragmentProgram(s.fragmentProgram));
    if ((programs || d.vertexConstants != c.vertexConstants) && vinfo.fieldCount && d.vertexConstants != render::NoConstants)
    {
        void* buffer = nullptr;
        sceGxmReserveVertexDefaultUniformBuffer(context, &buffer);
        upload(buffer, vinfo.fields, vinfo.fieldCount, m_vertexPrograms[s.vertexProgram].fields,
               frame.constants + d.vertexConstants);
    }
    if ((programs || d.fragmentConstants != c.fragmentConstants) && finfo.fieldCount
        && d.fragmentConstants != render::NoConstants)
    {
        void* buffer = nullptr;
        sceGxmReserveFragmentDefaultUniformBuffer(context, &buffer);
        upload(buffer, finfo.fields, finfo.fieldCount, m_fragmentPrograms[s.fragmentProgram][variant(s)].fields,
               frame.constants + d.fragmentConstants);
    }
    c.vertexConstants = d.vertexConstants;
    c.fragmentConstants = d.fragmentConstants;

    const FragmentProgram& fp = m_fragmentPrograms[s.fragmentProgram][variant(s)];
    for (uint8_t i = 0; i < finfo.samplers; ++i)
    {
        if (fp.units[i] == NoUnit || (!programs && s.textures[i] == c.last.textures[i]
            && memcmp(&s.samplers[i], &c.last.samplers[i], sizeof(render::Sampler)) == 0))
            continue;
        const SceGxmTexture texture = samplerTexture(s, i);
        sceGxmSetFragmentTexture(context, fp.units[i], &texture);
    }
    setFixedState(c, context, frame, d, first);
    c.last = s;
    c.valid = true;
    c.vertex = pr.vertex;
    c.fragment = pr.fragment;

    sceGxmSetVertexStream(context, 0, d.vertices);
    sceGxmDraw(context, primitiveType(s.primitive), SCE_GXM_INDEX_FORMAT_U16, d.indices ? d.indices : m_sequence, d.count);
}

// The same draw with precomputed objects: a vertex state when the vertex constants change, a fragment state when
// its constants, textures or samplers change, and a precomputed draw. Their memory and uniform buffers come from the
// recorder's pool and live until the frame is finished.
void FrameRenderer::drawPrecomputed(Recorder& r, const render::Frame& frame, uint32_t index)
{
    const render::DrawCall& d = frame.draws[index];
    ContextState& c = r.state;
    SceGxmContext* context = r.context;
    FramePool& pool = r.data;
    const DrawPrograms& pr = m_programs[index];
    const render::DrawState& s = d.state;
    const bool first = !c.valid;
    const bool programs = first || pr.vertex != c.vertex || pr.fragment != c.fragment;
    if (programs)
    {
        sceGxmSetVertexProgram(context, pr.vertex->program);
        sceGxmSetFragmentProgram(context, pr.fragment->program);
    }
    const render::VertexProgramInfo& vinfo = render::vertexProgram(render::VertexProgram(s.vertexProgram));
    const render::FragmentProgramInfo& finfo = render::fragmentProgram(render::FragmentProgram(s.fragmentProgram));
    int error = 0;

    if (programs || d.vertexConstants != c.vertexConstants)
    {
        auto* state = static_cast<SceGxmPrecomputedVertexState*>(r.words.allocate(sizeof(SceGxmPrecomputedVertexState)));
        void* memory = pr.vertex->stateSize ? pool.allocate(pr.vertex->stateSize, 16) : nullptr;
        error = sceGxmPrecomputedVertexStateInit(state, pr.vertex->program, memory);
        if (!error && pr.vertex->uniformBytes)
        {
            void* buffer = pool.allocate(pr.vertex->uniformBytes, 16);
            if (vinfo.fieldCount && d.vertexConstants != render::NoConstants)
                upload(buffer, vinfo.fields, vinfo.fieldCount, m_vertexPrograms[s.vertexProgram].fields,
                       frame.constants + d.vertexConstants);
            else
                memset(buffer, 0, pr.vertex->uniformBytes);
            sceGxmPrecomputedVertexStateSetDefaultUniformBuffer(state, buffer);
        }
        if (!error)
            sceGxmSetPrecomputedVertexState(context, state);
    }

    bool textures = programs || d.fragmentConstants != c.fragmentConstants;
    for (uint8_t i = 0; i < finfo.samplers && !textures; ++i)
        textures = s.textures[i] != c.last.textures[i]
                || memcmp(&s.samplers[i], &c.last.samplers[i], sizeof(render::Sampler)) != 0;
    if (!error && textures)
    {
        const FragmentProgram& fp = m_fragmentPrograms[s.fragmentProgram][variant(s)];
        auto* state = static_cast<SceGxmPrecomputedFragmentState*>(r.words.allocate(sizeof(SceGxmPrecomputedFragmentState)));
        void* memory = pr.fragment->stateSize ? pool.allocate(pr.fragment->stateSize, 16) : nullptr;
        error = sceGxmPrecomputedFragmentStateInit(state, pr.fragment->program, memory);
        for (uint8_t i = 0; i < finfo.samplers && !error; ++i)
        {
            if (fp.units[i] == NoUnit)
                continue;
            const SceGxmTexture texture = samplerTexture(s, i);
            error = sceGxmPrecomputedFragmentStateSetTexture(state, fp.units[i], &texture);
        }
        if (!error && pr.fragment->uniformBytes)
        {
            void* buffer = pool.allocate(pr.fragment->uniformBytes, 16);
            if (finfo.fieldCount && d.fragmentConstants != render::NoConstants)
                upload(buffer, finfo.fields, finfo.fieldCount, fp.fields, frame.constants + d.fragmentConstants);
            else
                memset(buffer, 0, pr.fragment->uniformBytes);
            sceGxmPrecomputedFragmentStateSetDefaultUniformBuffer(state, buffer);
        }
        if (!error)
            sceGxmSetPrecomputedFragmentState(context, state);
    }

    SceGxmPrecomputedDraw* draw = nullptr;
    if (!error)
    {
        draw = static_cast<SceGxmPrecomputedDraw*>(r.words.allocate(sizeof(SceGxmPrecomputedDraw)));
        void* memory = pr.vertex->drawSize ? pool.allocate(pr.vertex->drawSize, 16) : nullptr;
        error = sceGxmPrecomputedDrawInit(draw, pr.vertex->program, memory);
        const void* streams[1] = { d.vertices };
        if (!error)
            error = sceGxmPrecomputedDrawSetAllVertexStreams(draw, streams);
    }
    if (error)
    {
        // Not expected; the immediate path takes over from here.
        logError("gxm: precomputed objects failed (0x%08x): immediate draws from now on", error);
        m_precomputed.store(false, std::memory_order_relaxed);
        c.valid = false;
        this->draw(r, frame, index);
        return;
    }
    c.vertexConstants = d.vertexConstants;
    c.fragmentConstants = d.fragmentConstants;
    setFixedState(c, context, frame, d, first);
    c.last = s;
    c.valid = true;
    c.vertex = pr.vertex;
    c.fragment = pr.fragment;

    sceGxmPrecomputedDrawSetParams(draw, primitiveType(s.primitive), SCE_GXM_INDEX_FORMAT_U16,
                                   d.indices ? d.indices : m_sequence, d.count);
    sceGxmDrawPrecomputed(context, draw);
}

void FrameRenderer::planFrame(const render::Frame& frame, bool stretchDirect)
{
    m_plan.resize(frame.sceneCount);
    for (uint32_t i = 0; i < frame.sceneCount; ++i)
    {
        const render::Scene& scene = frame.scenes[i];
        bool used = scene.target.depth && scene.depthCleared;
        for (uint32_t n = 0; n < scene.drawCount && scene.target.depth && !used; ++n)
            used = frame.draws[scene.firstDraw + n].state.depthEnable != 0;
        m_plan[i].depthUsed = used;
        m_plan[i].skipped = false;
        m_plan[i].carriedTo = i;
    }

    // A scene of colour clears alone (the game clears the back buffer right after Present) moves to the start of
    // the next scene on its target when nothing in between samples that target and its depth clear is dead.
    for (uint32_t i = 0; i < frame.sceneCount; ++i)
    {
        const render::Scene& scene = frame.scenes[i];
        bool clears = scene.drawCount != 0;
        for (uint32_t n = 0; n < scene.drawCount && clears; ++n)
        {
            const render::DrawState& s = frame.draws[scene.firstDraw + n].state;
            clears = s.vertexProgram == render::VpClear && !s.depthEnable;
        }
        if (!clears)
            continue;
        uint32_t next = i + 1;
        while (next < frame.sceneCount && (frame.scenes[next].target.color != scene.target.color
                                           || frame.scenes[next].target.face != scene.target.face
                                           || frame.scenes[next].target.level != scene.target.level))
            ++next;
        if (next == frame.sceneCount)
            continue;
        bool sampled = false;
        for (uint32_t k = i + 1; k < next && scene.target.color && !sampled; ++k)
        {
            const render::Scene& between = frame.scenes[k];
            for (uint32_t n = 0; n < between.drawCount && !sampled; ++n)
                for (const render::TextureStorage* t : frame.draws[between.firstDraw + n].state.textures)
                    sampled = sampled || t == scene.target.color;
        }
        if (sampled)
            continue;
        if (scene.target.depth && scene.depthCleared)
        {
            if (scene.target.depth->kept)
                continue;
            bool dead = true;
            for (uint32_t k = i + 1; k < frame.sceneCount; ++k)
            {
                if (frame.scenes[k].target.depth == scene.target.depth && m_plan[k].depthUsed)
                {
                    dead = frame.scenes[k].depthCleared;
                    break;
                }
            }
            if (!dead)
                continue;
        }
        m_plan[i].skipped = true;
        m_plan[i].carriedTo = next;
        m_plan[i].depthUsed = false;
    }
    // A carried scene that is itself carried passes its clears on.
    for (uint32_t i = frame.sceneCount; i-- > 0;)
    {
        if (m_plan[i].skipped && m_plan[m_plan[i].carriedTo].skipped)
            m_plan[i].carriedTo = m_plan[m_plan[i].carriedTo].carriedTo;
    }

    // A stretched back buffer whose scenes need no depth is drawn to the display at its resolution: no offscreen
    // store and no scaling pass.
    Slot& slot = m_slots[frame.index % Slots];
    slot.direct = frame.width == 960 && frame.height == 544 && !slot.forceOffscreen;
    if (!slot.direct && stretchDirect && !slot.forceOffscreen)
    {
        bool any = false, depth = false;
        for (uint32_t i = 0; i < frame.sceneCount; ++i)
        {
            if (frame.scenes[i].target.color || m_plan[i].skipped)
                continue;
            any = true;
            depth = depth || m_plan[i].depthUsed;
        }
        slot.direct = any && !depth;
    }
}

namespace
{

enum SortClass
{
    SortFixed,          // stays in place; no draw moves across it
    SortOpaque,
    SortDiscard
};

SortClass sortClass(const render::DrawCall& d, const render::Target& target)
{
    if (!d.count)
        return SortOpaque;
    const render::DrawState& s = d.state;
    // D3DCMP_LESS 2, D3DCMP_LESSEQUAL 4.
    if (s.vertexProgram == render::VpClear || s.blendEnable || s.colorMask != 0xf || !s.depthEnable || !s.depthWrite
        || (s.depthFunc != 2 && s.depthFunc != 4))
        return SortFixed;
    for (const render::TextureStorage* t : s.textures)
    {
        if (t && (t == target.color || (t->depth && t->depth == target.depth)))
            return SortFixed;
    }
    return s.alphaTest ? SortDiscard : SortOpaque;
}

// Razor's pass of a draw: 2 alpha-tested, 1 blended, 0 opaque.
uint32_t hsrPass(const render::DrawState& s)
{
    return s.alphaTest ? 2 : s.blendEnable ? 1 : 0;
}

uint32_t flushTransitions(const render::Frame& frame, const uint32_t* order, uint32_t count)
{
    uint32_t transitions = 0, previous = 0;
    for (uint32_t n = 0; n < count; ++n)
    {
        const uint32_t pass = hsrPass(frame.draws[order[n]].state);
        transitions += previous == 2 && pass < 2;
        previous = pass;
    }
    return transitions;
}

uint64_t geometryBit(const void* vertices)
{
    return uint64_t(1) << ((uintptr_t(vertices) >> 4) & 63);
}

}

// Opaque draws first, then the rest, each in order, between barriers (other depth writes, draws not tested LESS,
// EQUAL or LESSEQUAL, draws sampling the target); an opaque draw of geometry drawn earlier keeps its place.
void FrameRenderer::opaqueFirst(const render::Frame& frame, const render::Target& target, uint32_t* order,
                                uint32_t count)
{
    m_opaque.clear();
    m_held.clear();
    uint64_t held = 0;
    uint32_t out = 0;
    auto place = [&]() {
        for (uint32_t i : m_opaque)
            order[out++] = i;
        for (uint32_t i : m_held)
            order[out++] = i;
        m_opaque.clear();
        m_held.clear();
        held = 0;
    };
    for (uint32_t n = 0; n < count; ++n)
    {
        const uint32_t index = order[n];
        const render::DrawCall& d = frame.draws[index];
        const SortClass c = sortClass(d, target);
        const render::DrawState& s = d.state;
        const bool tested = s.depthEnable && s.depthFunc >= 2 && s.depthFunc <= 4;
        bool barrier = d.count && c == SortFixed && (!tested || s.depthWrite);
        for (const render::TextureStorage* t : s.textures)
            barrier = barrier || (t && (t == target.color || (t->depth && t->depth == target.depth)));
        if (barrier)
        {
            place();
            order[out++] = index;
            continue;
        }
        bool again = false;
        if (c == SortOpaque && (held & geometryBit(d.vertices)))
        {
            for (uint32_t h : m_held)
            {
                const render::DrawCall& e = frame.draws[h];
                again = again || (e.vertices == d.vertices && e.indices == d.indices && e.count == d.count);
            }
        }
        if (c == SortOpaque && !again)
        {
            m_opaque.push_back(index);
            continue;
        }
        m_held.push_back(index);
        held |= geometryBit(d.vertices);
    }
    place();
}

void FrameRenderer::sortScene(const render::Frame& frame, uint32_t scene)
{
    const ScenePlan& p = m_plan[scene];
    if (p.skipped || p.orderCount < 2)
        return;
    uint32_t* order = m_order.data() + p.firstOrder;
    const bool timed = perf::enabled();
    const uint32_t before = timed ? flushTransitions(frame, order, p.orderCount) : 0;
    if (m_drawSort && p.depthUsed)
    {
        const render::Target& target = frame.scenes[scene].target;
        if (m_drawSort == 2)
            opaqueFirst(frame, target, order, p.orderCount);
        m_held.clear();
        uint64_t held = 0;
        uint32_t out = 0;
        for (uint32_t n = 0; n < p.orderCount; ++n)
        {
            const uint32_t index = order[n];
            const render::DrawCall& d = frame.draws[index];
            const SortClass c = sortClass(d, target);
            if (c == SortDiscard)
            {
                m_held.push_back(index);
                held |= geometryBit(d.vertices);
                continue;
            }
            bool release = c == SortFixed;
            // The same geometry drawn again stays after its alpha-tested draw.
            if (!release && (held & geometryBit(d.vertices)))
            {
                for (uint32_t h : m_held)
                {
                    const render::DrawCall& e = frame.draws[h];
                    if (e.vertices == d.vertices && e.indices == d.indices && e.count == d.count)
                    {
                        release = true;
                        break;
                    }
                }
            }
            if (release)
            {
                for (uint32_t h : m_held)
                    order[out++] = h;
                m_held.clear();
                held = 0;
            }
            order[out++] = index;
        }
        for (uint32_t h : m_held)
            order[out++] = h;
        m_held.clear();
    }
    if (!timed)
        return;
    const uint32_t after = flushTransitions(frame, order, p.orderCount);
    SortStats& s = m_sortStats;
    s.allBefore += before;
    s.allAfter += after;
    if (p.orderCount > s.largest)
    {
        uint32_t discards = 0;
        for (uint32_t n = 0; n < p.orderCount; ++n)
            discards += frame.draws[order[n]].state.alphaTest ? 1 : 0;
        s.largest = p.orderCount;
        s.largestDiscards = discards;
        s.largestBefore = before;
        s.largestAfter = after;
    }
}

void FrameRenderer::drawFrame(const render::Frame& frame, const SceGxmColorSurface* display, SceGxmSyncObject* displaySync,
                              bool stretchDirect)
{
    Slot& slot = m_slots[frame.index % Slots];
    planFrame(frame, stretchDirect);
    preparePrograms(frame);
    m_main.data.begin(frame.index, &finished, this);
    m_main.words.reset();
    const bool lastWritesDone = !presentScene(frame);

    // Where each scene draws, and its draws in order: the colour clears carried to it first.
    m_order.clear();
    for (uint32_t i = 0; i < frame.sceneCount; ++i)
    {
        ScenePlan& p = m_plan[i];
        const render::Scene& scene = frame.scenes[i];
        p.color = display;
        p.sync = nullptr;
        p.width = scene.width;
        p.height = scene.height;
        p.scaleX = p.scaleY = 1.0f;
        if (scene.target.color)
        {
            const Texture* t = static_cast<const Texture*>(scene.target.color);
            const uint32_t faces = uint32_t(t->surfaces.size()) / t->desc.levels;
            const uint32_t face = scene.target.face < faces ? scene.target.face : 0;
            const uint32_t level = scene.target.level < t->desc.levels ? scene.target.level : 0;
            p.color = &t->surfaces[face * t->desc.levels + level];
            p.width = texture::levelDim(t->desc.width, level);
            p.height = texture::levelDim(t->desc.height, level);
            p.scaleX = float(p.width) / float(scene.width);
            p.scaleY = float(p.height) / float(scene.height);
        }
        else if (slot.direct)
        {
            p.sync = displaySync;
            p.width = 960;
            p.height = 544;
            p.scaleX = 960.0f / float(scene.width);
            p.scaleY = 544.0f / float(scene.height);
        }
        else if (slot.offscreen)
        {
            p.color = &slot.offscreen->surfaces[0];
        }
        else
        {
            p.sync = displaySync;
        }
        p.firstOrder = uint32_t(m_order.size());
        p.firstJob = p.jobCount = 0;
        if (!p.skipped)
        {
            for (uint32_t c = 0; c < i; ++c)
            {
                if (m_plan[c].skipped && m_plan[c].carriedTo == i)
                    for (uint32_t n = 0; n < frame.scenes[c].drawCount; ++n)
                        m_order.push_back(frame.scenes[c].firstDraw + n);
            }
            for (uint32_t n = 0; n < scene.drawCount; ++n)
                m_order.push_back(scene.firstDraw + n);
        }
        p.orderCount = uint32_t(m_order.size()) - p.firstOrder;
        sortScene(frame, i);
    }
    if (perf::enabled())
    {
        SortStats& s = m_sortStats;
        s.draws += s.largest;
        s.discards += s.largestDiscards;
        s.before += s.largestBefore;
        s.after += s.largestAfter;
        s.largest = s.largestDiscards = s.largestBefore = s.largestAfter = 0;
        if (++s.frames == 300)
        {
            const double n = s.frames;
            logInfo("perf: draw sort %s, per frame: largest scene %.0f draws, %.1f alpha-tested, HSR flush "
                    "transitions %.1f -> %.1f; all scenes %.1f -> %.1f", m_drawSort ? "on" : "off", s.draws / n,
                    s.discards / n, s.before / n, s.after / n, s.allBefore / n, s.allAfter / n);
            s = SortStats();
        }
    }
    const bool lists = recordLists(frame);

    for (uint32_t i = 0; i < frame.sceneCount; ++i)
    {
        const ScenePlan& p = m_plan[i];
        if (p.skipped)
            continue;
        const render::Scene& scene = frame.scenes[i];
        // Scenes whose draws never test depth (filter passes, the HUD) neither load nor store it.
        SceGxmDepthStencilSurface depth;
        const Depth* d = m_plan[i].depthUsed ? static_cast<const Depth*>(scene.target.depth) : nullptr;
        if (d)
        {
            depth = d->surface;
            if (scene.depthCleared)
                sceGxmDepthStencilSurfaceSetBackgroundDepth(&depth, scene.clearDepth);
            sceGxmDepthStencilSurfaceSetForceLoadMode(&depth, scene.depthCleared ? SCE_GXM_DEPTH_STENCIL_FORCE_LOAD_DISABLED
                                                                                 : SCE_GXM_DEPTH_STENCIL_FORCE_LOAD_ENABLED);
            // Stored when sampled later (a depth texture) or when the next scene of the frame using this depth
            // continues with it.
            bool store = d->kept;
            for (uint32_t j = i + 1; j < frame.sceneCount && !store; ++j)
            {
                if (frame.scenes[j].target.depth == scene.target.depth && m_plan[j].depthUsed)
                {
                    store = !frame.scenes[j].depthCleared;
                    break;
                }
            }
            sceGxmDepthStencilSurfaceSetForceStoreMode(&depth, store ? SCE_GXM_DEPTH_STENCIL_FORCE_STORE_ENABLED
                                                                     : SCE_GXM_DEPTH_STENCIL_FORCE_STORE_DISABLED);
        }
        sceGxmBeginScene(m_context, 0, renderTarget(p.width, p.height), nullptr, nullptr, p.sync, p.color,
                         d ? &depth : nullptr);
        bool recorded = lists && p.jobCount;
        for (uint32_t j = 0; j < p.jobCount && recorded; ++j)
            recorded = m_jobs[p.firstJob + j].ok;
        if (recorded)
        {
            for (uint32_t j = 0; j < p.jobCount; ++j)
            {
                const int error = sceGxmExecuteCommandList(m_context, &m_jobs[p.firstJob + j].list);
                if (error < 0)
                    logError("gxm: sceGxmExecuteCommandList failed: 0x%08x", error);
            }
        }
        else
        {
            sceGxmSetViewportEnable(m_context, SCE_GXM_VIEWPORT_ENABLED);
            sceGxmSetRegionClip(m_context, SCE_GXM_REGION_CLIP_NONE, 0, 0, p.width - 1, p.height - 1);
            sceGxmSetTwoSidedEnable(m_context, SCE_GXM_TWO_SIDED_DISABLED);
            m_main.state.reset(p.scaleX, p.scaleY, p.depthUsed);
            for (uint32_t n = 0; n < p.orderCount; ++n)
                draw(m_main, frame, m_order[p.firstOrder + n]);
        }
        const bool last = i + 1 == frame.sceneCount;
        sceGxmEndScene(m_context, nullptr, last && lastWritesDone ? &slot.done : nullptr);
    }
    // The present pass draws with the context's own state again.
    sceGxmSetPrecomputedVertexState(m_context, nullptr);
    sceGxmSetPrecomputedFragmentState(m_context, nullptr);
}

// ---- Command lists -------------------------------------------------------------------------------------------------

namespace
{

const uint32_t ListMinimumDraws = 96;   // smaller frames are drawn on the immediate context
const uint32_t JobMinimumDraws = 32;    // smallest range a list records
const uint32_t WorkerHostMem = 16 * 1024;
// Above guest threads, like the job workers: a helper preempted inside a range would hold up the render thread.
const int ListWorkerPriority = Thread::PriorityHigh + 16;

}

// Memory callbacks of the deferred contexts: a whole chunk of the worker's pool, kept until every frame that used it
// is finished. The chunk a context writes into is its pool's current one.
void* FrameRenderer::listMemory(FramePool& pool, SceSize requested, SceSize* size)
{
    uint32_t granted = 0;
    void* memory = pool.take(requested, &granted);
    *size = granted;
    return memory;
}

void* FrameRenderer::vdmMemory(void* worker, SceSize requested, SceSize* size)
{
    return listMemory(static_cast<Worker*>(worker)->vdm, requested, size);
}

void* FrameRenderer::vertexMemory(void* worker, SceSize requested, SceSize* size)
{
    return listMemory(static_cast<Worker*>(worker)->vertex, requested, size);
}

void* FrameRenderer::fragmentMemory(void* worker, SceSize requested, SceSize* size)
{
    return listMemory(static_cast<Worker*>(worker)->fragment, requested, size);
}

bool FrameRenderer::createWorker(Worker& w)
{
    w.owner = this;
    w.hostMem = malloc(WorkerHostMem);
    SceGxmDeferredContextParams params;
    memset(&params, 0, sizeof(params));
    params.hostMem = w.hostMem;
    params.hostMemSize = WorkerHostMem;
    params.vdmCallback = &vdmMemory;
    params.vertexCallback = &vertexMemory;
    params.fragmentCallback = &fragmentMemory;
    params.callbackData = &w;
    uint32_t size = 0;
    params.vdmRingBufferMem = w.vdm.take(0, &size);
    params.vdmRingBufferMemSize = size;
    params.vertexRingBufferMem = w.vertex.take(0, &size);
    params.vertexRingBufferMemSize = size;
    params.fragmentRingBufferMem = w.fragment.take(0, &size);
    params.fragmentRingBufferMemSize = size;
    const int error = sceGxmCreateDeferredContext(&params, &w.recorder.context);
    if (error < 0)
    {
        logError("gxm: sceGxmCreateDeferredContext failed: 0x%08x", error);
        w.recorder.context = nullptr;
        return false;
    }
    return true;
}

void FrameRenderer::initLists()
{
    if (!Config::getBool("gxm_lists", true))
    {
        logInfo("gxm: command lists off (gxm_lists=0)");
        return;
    }
    // The render thread records too and takes whatever the helpers have not claimed.
    std::vector<int> cores;
    if (cores::hasCore3())
        cores.push_back(3);
    if (Config::getBool("gxm_core2", true))
        cores.push_back(2);
    for (size_t i = 0; i <= cores.size(); ++i)
    {
        Worker* w = new Worker();
        if (!createWorker(*w))
        {
            delete w;
            break;
        }
        m_workers.push_back(w);
    }
    if (m_workers.empty())
    {
        logError("gxm: command lists off: no deferred context");
        return;
    }
    m_jobLock = new Mutex("gxm_jobs", false);
    m_jobsDone = new CondVar("gxm_jobs_done", *m_jobLock);
    m_wake = new Semaphore("gxm_wake", 0);
    static const char* const names[] = { "gxm_list_core3", "gxm_list_core2" };
    for (size_t i = 1; i < m_workers.size(); ++i)
    {
        Worker* w = m_workers[i];
        const int core = cores[i - 1];
        w->thread = new Thread(core == 3 ? names[0] : names[1], &workerEntry, w, ListWorkerPriority,
                               64 * 1024, Thread::Cpu0 << core);
        w->thread->start();
    }
    char where[32] = "render core";
    for (size_t i = 1; i < m_workers.size(); ++i)
        snprintf(where + strlen(where), sizeof(where) - strlen(where), ", core %d", cores[i - 1]);
    logInfo("gxm: command lists recorded by %u thread(s): %s", uint32_t(m_workers.size()), where);
}

void FrameRenderer::shutdownLists()
{
    if (m_jobLock)
    {
        {
            LockGuard guard(*m_jobLock);
            m_stop = true;
        }
        m_wake->post(int(m_workers.size()));
    }
    for (Worker* w : m_workers)
    {
        if (w->thread)
        {
            w->thread->join();
            delete w->thread;
        }
        if (w->recorder.context)
            sceGxmDestroyDeferredContext(w->recorder.context);
        free(w->hostMem);
        delete w;
    }
    m_workers.clear();
    delete m_jobsDone;
    delete m_jobLock;
    delete m_wake;
    m_jobsDone = nullptr;
    m_jobLock = nullptr;
    m_wake = nullptr;
}

int FrameRenderer::workerEntry(void* worker)
{
    Worker& w = *static_cast<Worker*>(worker);
    FrameRenderer& r = *w.owner;
    while (true)
    {
        r.m_wake->wait();
        {
            LockGuard guard(*r.m_jobLock);
            if (r.m_stop)
                break;
        }
        r.runJobs(w);
    }
    return 0;
}

// Claims jobs until none is left. A worker woken late finds the current frame's jobs, or none.
void FrameRenderer::runJobs(Worker& w)
{
    while (true)
    {
        const render::Frame* frame;
        uint32_t index;
        {
            LockGuard guard(*m_jobLock);
            if (m_stop || m_nextJob >= m_jobs.size())
                return;
            index = m_nextJob++;
            frame = m_jobFrame;
        }
        if (w.frame != frame->index)
        {
            w.frame = frame->index;
            w.recorder.data.begin(frame->index, &finished, this);
            w.vdm.begin(frame->index, &finished, this);
            w.vertex.begin(frame->index, &finished, this);
            w.fragment.begin(frame->index, &finished, this);
            w.recorder.words.reset();
        }
        const bool ok = recordJob(w, *frame, m_jobs[index]);
        LockGuard guard(*m_jobLock);
        m_jobs[index].ok = ok;
        if (++m_doneJobs == m_jobs.size())
            m_jobsDone->signal();
    }
}

bool FrameRenderer::recordJob(Worker& w, const render::Frame& frame, const Job& job)
{
    SceGxmContext* context = w.recorder.context;
    const ScenePlan& p = m_plan[job.scene];
    int error = sceGxmBeginCommandList(context);
    if (error < 0)
    {
        logError("gxm: sceGxmBeginCommandList failed: 0x%08x", error);
        return false;
    }
    // A list starts from a reset GPU state.
    sceGxmSetDefaultRegionClipAndViewport(context, p.width - 1, p.height - 1);
    sceGxmSetViewportEnable(context, SCE_GXM_VIEWPORT_ENABLED);
    sceGxmSetRegionClip(context, SCE_GXM_REGION_CLIP_NONE, 0, 0, p.width - 1, p.height - 1);
    sceGxmSetTwoSidedEnable(context, SCE_GXM_TWO_SIDED_DISABLED);
    w.recorder.state.reset(p.scaleX, p.scaleY, p.depthUsed);
    for (uint32_t n = job.begin; n < job.end; ++n)
        draw(w.recorder, frame, m_order[n]);
    SceGxmCommandList list;
    error = sceGxmEndCommandList(context, &list);
    if (error < 0)
    {
        logError("gxm: sceGxmEndCommandList failed: 0x%08x", error);
        return false;
    }
    const_cast<Job&>(job).list = list;
    return true;
}

// Splits the frame's scenes into jobs and records them on the workers and this thread; false: draw immediately.
bool FrameRenderer::recordLists(const render::Frame& frame)
{
    if (m_workers.empty() || m_order.size() < ListMinimumDraws)
        return false;
    const uint32_t threads = uint32_t(m_workers.size());
    {
        LockGuard guard(*m_jobLock);
        m_jobs.clear();
        for (uint32_t i = 0; i < frame.sceneCount; ++i)
        {
            ScenePlan& p = m_plan[i];
            p.firstJob = uint32_t(m_jobs.size());
            if (p.skipped || !p.orderCount)
                continue;
            // About two ranges per thread, so that a late helper leaves less to wait for.
            const uint32_t size = std::max(JobMinimumDraws, (p.orderCount + 2 * threads - 1) / (2 * threads));
            for (uint32_t begin = 0; begin < p.orderCount; begin += size)
            {
                const uint32_t end = std::min(p.orderCount, begin + size);
                m_jobs.push_back(Job{ i, p.firstOrder + begin, p.firstOrder + end, false, {} });
            }
            p.jobCount = uint32_t(m_jobs.size()) - p.firstJob;
        }
        m_jobFrame = &frame;
        m_nextJob = 0;
        m_doneJobs = 0;
    }
    if (threads > 1)
        m_wake->post(int(threads - 1));
    runJobs(*m_workers[0]);
    LockGuard guard(*m_jobLock);
    while (m_doneJobs < m_jobs.size())
        m_jobsDone->wait();
    return true;
}

}}
