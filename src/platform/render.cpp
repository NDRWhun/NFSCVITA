#include <platform/render.h>
#include <platform/log.h>
#include <atomic>
#include <cstring>

namespace pf { namespace render {

namespace
{

#define FIELD(type, member, name) { name, uint16_t(offsetof(type, member) / sizeof(float)), \
                                    uint16_t(sizeof(type::member) / sizeof(float)) }
#define COUNT(fields) uint8_t(sizeof(fields) / sizeof(fields[0]))
#define FLOATS(type) uint16_t(sizeof(type) / sizeof(float))

const UniformField s_fixedVertex[] = {
    FIELD(FixedVertexConstants, wvp, "uWvp"),
    FIELD(FixedVertexConstants, screen, "uScreen"),
    FIELD(FixedVertexConstants, texMatrix, "uTexMatrix"),
    FIELD(FixedVertexConstants, texPad, "uTexPad"),
    FIELD(FixedVertexConstants, diffuseMask, "uDiffuseMask"),
    FIELD(FixedVertexConstants, factorMask, "uFactorMask"),
    FIELD(FixedVertexConstants, factor, "uFactor"),
};
const UniformField s_fixedFragment[] = {
    FIELD(FixedFragmentConstants, alphaTest, "uAlphaTest"),
    FIELD(FixedFragmentConstants, textureMask, "uTextureMask"),
    FIELD(FixedFragmentConstants, scale, "uScale"),
    FIELD(FixedFragmentConstants, addMask, "uAddMask"),
};
const UniformField s_clearFragment[] = {
    FIELD(ClearFragmentConstants, color, "uColor"),
};

// Effect programs in enum order: name, constant registers; vertex programs also pixel registers, bias register,
// flags; fragment programs also samplers, flags, volume sampler.
struct EffectVertex
{
    const char* name;
    uint8_t     registers;
    uint8_t     pixelRegisters = 0;
    uint8_t     biasRegister = NoBias;
    uint8_t     flags = 0;
};

struct EffectFragment
{
    const char* name;
    uint8_t     samplers, registers, flags, volumeSampler;
    uint8_t     volumeSampler2 = NoVolume;
};

const uint8_t Ps1 = ProgramClampConstants;
const uint8_t Ps1Proj = ProgramClampConstants | ProgramProjectStage0;

const EffectVertex s_effectVertex[VpCount - VpFe] = {
    { "fe_v", 5 }, { "femask_v", 4 }, { "filter_v", 0 }, { "screenfilter_v", 0 }, { "pip_v", 1 }, { "screen_v", 0 },
    { "car_v", 28 }, { "glass_v", 12 }, { "glasslow_v", 5 }, { "carnm_v", 28 }, { "ucap_v", 120 },
    { "world_v", 13, 7, 12, ProgramFog }, { "worldlow_v", 6 }, { "prelit_v", 6, 0, NoBias, ProgramFog },
    { "sky_v", 5, 0, NoBias, ProgramFog }, { "worldnm_v", 11, 7, NoBias, ProgramFog },
    { "road_v", 7, 4, NoBias, ProgramFog }, { "worldbone_v", 72 }, { "instance_v", 250 }, { "water_v", 8, 7 },
    { "particle_v", 42 }, { "streak_v", 9 }, { "flare_v", 7 }, { "raindrop_v", 0 }, { "ghost_v", 11 },
    { "rvm_v", 0 }, { "caster_v", 4 }, { "casterdepth_v", 4 }, { "depth_v", 4 }, { "depthskinned_v", 68 },
    { "motionblur_v", 0 }, { "carnm1_v", 24, 0, NoBias, ProgramFog }, { "glass1_v", 8, 0, NoBias, ProgramFog },
    { "flare1_v", 7 }, { "road1_v", 10, 0, NoBias, ProgramFog }, { "world1_v", 12, 0, NoBias, ProgramFog },
    { "worldnm1_v", 11, 0, NoBias, ProgramFog }, { "ucap1_v", 80 },
};

const EffectFragment s_effectFragment[FpCount - FpFe] = {
    { "fe_f", 1, 0, Ps1Proj, NoVolume },
    { "femask_f", 2, 0, Ps1, NoVolume },
    { "filter1_f", 1, 1, Ps1, NoVolume },
    { "filter2_f", 2, 2, Ps1, NoVolume },
    { "filter3_f", 3, 3, Ps1, NoVolume },
    { "filter4_f", 4, 4, Ps1, NoVolume },
    { "filterlum_f", 4, 5, Ps1, NoVolume },
    { "filterpremul_f", 4, 4, Ps1, NoVolume },
    { "screenfilter_f", 1, 1, Ps1Proj, NoVolume },
    { "screenfiltergain_f", 3, 0, Ps1Proj, NoVolume },
    { "pip_f", 3, 1, 0, 2 },
    { "vt_f", 4, 2, 0, 3 },
    { "passthru_f", 1, 0, 0, NoVolume },
    { "uves_f", 1, 1, 0, NoVolume },
    { "uvesdarken_f", 1, 1, 0, NoVolume },
    { "blend_f", 2, 1, 0, NoVolume },
    { "combine_f", 2, 0, 0, NoVolume },
    { "down2_f", 1, 4, 0, NoVolume },
    { "down2mb_f", 2, 5, 0, NoVolume },
    { "down4_f", 1, 16, 0, NoVolume },
    { "downbloom_f", 1, 16, 0, NoVolume },
    { "gauss_f", 1, 26, 0, NoVolume },
    { "blur15_f", 1, 30, 0, NoVolume },
    { "car_f", 2, 10, ProgramFog, NoVolume },
    { "glass_f", 3, 7, ProgramFog, NoVolume },
    { "glasslow_f", 1, 0, 0, NoVolume },
    { "carnm_f", 3, 8, ProgramFog, NoVolume },
    { "ucap_f", 2, 11, 0, NoVolume },
    { "world_f", 1, 7, 0, NoVolume },
    { "modulate_f", 1, 0, 0, NoVolume },
    { "prelit_f", 1, 0, 0, NoVolume },
    { "fefog_f", 1, 0, Ps1Proj, NoVolume },
    { "worldnm_f", 3, 7, 0, NoVolume },
    { "road_f", 4, 4, 0, NoVolume },
    { "texture_f", 1, 0, 0, NoVolume },
    { "color_f", 0, 0, 0, NoVolume },
    { "water_f", 3, 7, 0, NoVolume },
    { "particle_f", 2, 2, 0, NoVolume },
    { "streak_f", 1, 0, 0, NoVolume },
    { "flare_f", 2, 1, 0, NoVolume },
    { "raindrop_f", 2, 2, 0, NoVolume },
    { "ghost_f", 1, 0, 0, NoVolume },
    { "rvm_f", 3, 1, 0, 2 },
    { "castergreen_f", 0, 0, 0, NoVolume },
    { "casterblack_f", 0, 0, 0, NoVolume },
    { "casterdepth_f", 0, 0, 0, NoVolume },
    { "vtx_f", 7, 6, 0, 5, 6 },
    { "depthtexture_f", 1, 0, 0, NoVolume },
    { "motionblur_f", 1, 9, 0, NoVolume },
    { "composite_f", 2, 1, 0, NoVolume },
    { "car1_f", 2, 1, Ps1 | ProgramFog, NoVolume },
    { "carnm1_f", 2, 1, Ps1, NoVolume },
    { "glass1_f", 2, 0, 0, NoVolume },
    { "pip1_f", 2, 0, 0, NoVolume },
    { "rvm1_f", 2, 0, 0, NoVolume },
    { "vt1_f", 2, 2, Ps1, NoVolume },
    { "particle1_f", 1, 1, Ps1, NoVolume },
    { "road1_f", 3, 1, Ps1, NoVolume },
    { "opaque_f", 0, 0, 0, NoVolume },
    { "textureopaque_f", 1, 0, 0, NoVolume },
    { "down4tent_f", 1, 16, 0, NoVolume },
    { "downbloomtent_f", 1, 16, 0, NoVolume },
    { "vtnearest_f", 4, 2, 0, 3 },
    { "vtxnearest_f", 7, 6, 0, 5, 6 },
};

// The program tables, effect programs' uniform fields built from their descriptions.
struct Tables
{
    VertexProgramInfo   vertex[VpCount];
    FragmentProgramInfo fragment[FpCount];
    UniformField        vertexFields[VpCount - VpFe][3];
    UniformField        fragmentFields[FpCount - FpFe][6];

    Tables()
    {
        vertex[VpFixed] = { "ff_v", FLOATS(FixedVertexConstants), COUNT(s_fixedVertex), s_fixedVertex, 0, 0, NoBias,
                            0, 0, 0 };
        vertex[VpFixedScreen] = { "ff_screen_v", FLOATS(FixedVertexConstants), COUNT(s_fixedVertex), s_fixedVertex,
                                  0, 0, NoBias, 0, 0, 0 };
        vertex[VpClear] = { "clearz_v", 0, 0, nullptr, 0, 0, NoBias, 0, 0, 0 };
        for (int i = 0; i < VpCount - VpFe; ++i)
        {
            const EffectVertex& e = s_effectVertex[i];
            UniformField* fields = vertexFields[i];
            uint8_t count = 0;
            uint16_t floats = 0;
            if (e.registers)
            {
                fields[count++] = { "uC", 0, uint16_t(4 * e.registers) };
                floats = uint16_t(4 * e.registers);
            }
            uint16_t pixel = 0, fog = 0;
            if (e.pixelRegisters)
            {
                pixel = floats;
                fields[count++] = { "uP", floats, uint16_t(4 * e.pixelRegisters) };
                floats = uint16_t(floats + 4 * e.pixelRegisters);
            }
            if (e.flags & ProgramFog)
            {
                fog = floats;
                fields[count++] = { "uFog", floats, 4 };
                floats = uint16_t(floats + 4);
            }
            vertex[VpFe + i] = { e.name, floats, count, fields, e.registers, e.pixelRegisters, e.biasRegister,
                                 e.flags, pixel, fog };
        }
        fragment[FpFixedColor] = fixed("ff_color_f", 0);
        fragment[FpFixedTexture] = fixed("ff_tex_f", 1);
        fragment[FpFixedProjected] = fixed("ff_proj_f", 1);
        fragment[FpClear] = FragmentProgramInfo{ "clear_f", FLOATS(ClearFragmentConstants), COUNT(s_clearFragment),
                                                 s_clearFragment, 0, 0, false, 0, 0, NoVolume, 0, 0, 0, NoVolume,
                                                 0 };
        for (int i = 0; i < FpCount - FpFe; ++i)
        {
            const EffectFragment& e = s_effectFragment[i];
            UniformField* fields = fragmentFields[i];
            uint8_t count = 0;
            uint16_t floats = 4;
            fields[count++] = { "uAlphaTest", 0, 4 };
            if (e.registers)
            {
                fields[count++] = { "uC", floats, uint16_t(4 * e.registers) };
                floats = uint16_t(floats + 4 * e.registers);
            }
            uint16_t proj = 0, volume = 0, fog = 0;
            if (e.flags & ProgramProjectStage0)
            {
                proj = floats;
                fields[count++] = { "uProj", floats, 4 };
                floats = uint16_t(floats + 4);
            }
            if (e.volumeSampler != NoVolume)
            {
                volume = floats;
                fields[count++] = { "uVol", floats, 4 };
                floats = uint16_t(floats + 4);
            }
            if (e.flags & ProgramFog)
            {
                fog = floats;
                fields[count++] = { "uFog", floats, 4 };
                floats = uint16_t(floats + 4);
            }
            uint16_t volume2 = 0;
            if (e.volumeSampler2 != NoVolume)
            {
                volume2 = floats;
                fields[count++] = { "uVol2", floats, 4 };
                floats = uint16_t(floats + 4);
            }
            fragment[FpFe + i] = FragmentProgramInfo{ e.name, floats, count, fields, e.samplers, 0, true, e.registers,
                                                      e.flags, e.volumeSampler, proj, volume, fog, e.volumeSampler2,
                                                      volume2 };
        }
    }

    static FragmentProgramInfo fixed(const char* name, uint8_t samplers)
    {
        return FragmentProgramInfo{ name, FLOATS(FixedFragmentConstants), COUNT(s_fixedFragment), s_fixedFragment,
                                    samplers, 0, false, 0, 0, NoVolume, 0, 0, 0, NoVolume, 0 };
    }
};

const Tables& tables()
{
    static const Tables s_tables;
    return s_tables;
}

const char* const s_inputNames[UsageCount] = {
    "aPosition", "aNormal", "aColor", "aTex0", "aTex1", "aTex2", "aTex3", "aWeight", "aIndex", "aTangent", "aBinormal",
    "aTex4", "aTex5", "aTex6", "aTex7", "aTex8", "aColor1"
};

enum
{
    MaxLayouts = 64
};

VertexLayout            s_layouts[MaxLayouts];
std::atomic<uint32_t>   s_layoutCount{0};

}

const VertexProgramInfo& vertexProgram(VertexProgram program)
{
    return tables().vertex[program < VpCount ? program : 0];
}

const FragmentProgramInfo& fragmentProgram(FragmentProgram program)
{
    return tables().fragment[program < FpCount ? program : 0];
}

const char* factorSuffix(FactorOutput output)
{
    static const char* const s_suffixes[FactorCount] = { "", "_bfcolor", "_bfalpha", "_bfscale" };
    return output < FactorCount ? s_suffixes[output] : "";
}

const char* inputName(Usage usage)
{
    return usage < UsageCount ? s_inputNames[usage] : "";
}

uint8_t registerLayout(const VertexLayout& layout)
{
    VertexLayout key;
    memset(&key, 0, sizeof(key));
    key.stride = layout.stride;
    key.count = layout.count < MaxVertexElements ? layout.count : uint8_t(MaxVertexElements);
    key.positionT = layout.positionT;
    memcpy(key.elements, layout.elements, key.count * sizeof(VertexElement));
    const uint32_t count = s_layoutCount.load(std::memory_order_acquire);
    for (uint32_t i = 0; i < count; ++i)
    {
        if (memcmp(&s_layouts[i], &key, sizeof(key)) == 0)
            return uint8_t(i);
    }
    if (count == MaxLayouts)
        fatal("too many vertex layouts");
    s_layouts[count] = key;
    s_layoutCount.store(count + 1, std::memory_order_release);
    return uint8_t(count);
}

const VertexLayout& layout(uint8_t id)
{
    return s_layouts[id < MaxLayouts ? id : 0];
}

uint8_t minAlpha(const TextureStorage& texture)
{
    const TextureDesc& d = texture.desc;
    if (d.cube || d.target || d.yuv || !d.levels || d.levels > 16)
        return 0;
    uint8_t lowest = 255;
    for (uint32_t level = 0; level < d.levels; ++level)
    {
        const uint8_t a = texture.levelAlpha[level].load(std::memory_order_relaxed);
        lowest = a < lowest ? a : lowest;
    }
    return lowest;
}

float* allocateConstants(Frame& frame, uint32_t count, uint32_t* offset)
{
    if (frame.constantCount + count > frame.constantCapacity)
        return nullptr;
    *offset = frame.constantCount;
    frame.constantCount += count;
    return frame.constants + *offset;
}

void* allocateVertices(Frame& frame, uint32_t bytes)
{
    const uint32_t start = (frame.vertexBytes + 15) & ~15u;
    if (start + bytes > frame.vertexCapacity)
        return nullptr;
    frame.vertexBytes = start + bytes;
    return frame.vertices + start;
}

}}
