// D3DX effects of d3dx9_30: ID3DXEffect and ID3DXEffectPool over the compiled fx_2_0 effects in the executable's
// resources. Parameters, annotations, techniques and passes are parsed with their values; values are kept and read
// back with the D3DX conversions; shared parameters live in the pool. As D3DX does, BeginPass applies a pass's states
// to the device (render, stage and sampler states, textures, the shaders' constant registers and samplers) and
// CommitChanges the ones whose parameters changed since; Begin saves the states the technique sets and End restores
// them. A pass draws with the port's translation of its shaders, found by the bytecode's hash; a pass without one
// drops its draws (logged once per technique).

#include <winapi/d3dx9_30.h>
#include <winapi/d3d9_internal.h>
#include <winapi/d3d9_direct.h>
#include <winapi/d3d9_native.h>
#include <winapi/kernel32.h>
#include <winapi/support.h>
#include <lib/heap.h>
#include <platform/config.h>
#include <platform/log.h>
#include <recomp.h>
#include <algorithm>
#include <cstring>
#include <map>
#include <strings.h>
#include <memory>
#include <string>
#include <vector>

namespace win32 { namespace d3d9
{

namespace
{

const HRESULT S_FALSE_ = 1;

enum ParameterClass : DWORD
{
    ClassScalar, ClassVector, ClassMatrixRows, ClassMatrixColumns, ClassObject, ClassStruct
};

enum ParameterType : DWORD
{
    TypeVoid, TypeBool, TypeInt, TypeFloat, TypeString, TypeTexture, TypeTexture1D, TypeTexture2D, TypeTexture3D,
    TypeTextureCube, TypeSampler, TypeSampler1D, TypeSampler2D, TypeSampler3D, TypeSamplerCube, TypePixelShader,
    TypeVertexShader, TypePixelFragment, TypeVertexFragment
};

bool isTexture(DWORD type)
{
    return type >= TypeTexture && type <= TypeTextureCube;
}

bool isSampler(DWORD type)
{
    return type >= TypeSampler && type <= TypeSamplerCube;
}

// Effect state operations (the compiler's table, in order): render states, texture stage states, then the other
// classes; values are the Direct3D state numbers.
enum : uint32_t
{
    OpRenderFirst = 0, OpStageFirst = 103, OpFvf = 122, OpVertexShader = 146, OpPixelShader = 147, OpTexture = 164,
    OpSamplerFirst = 165, OpSamplerLast = 177, OpCount = 182
};

const uint8_t s_renderStates[OpStageFirst] = {
    7, 8, 9, 14, 15, 16, 19, 20, 22, 23, 24, 25, 26, 27, 28, 29, 34, 35, 36, 37, 38, 48, 52, 53, 54, 55, 56, 57, 58, 59,
    60, 128, 129, 130, 131, 132, 133, 134, 135, 198, 199, 200, 201, 202, 203, 204, 205, 136, 137, 139, 140, 141, 142,
    143, 145, 146, 147, 148, 151, 152, 154, 155, 166, 156, 157, 158, 159, 160, 161, 162, 163, 165, 167, 168, 170, 171,
    172, 173, 174, 175, 176, 178, 179, 180, 181, 182, 183, 184, 185, 53, 187, 188, 189, 190, 191, 192, 193, 194, 195,
    206, 207, 208, 171
};

const uint8_t s_stageStates[121 - OpStageFirst] = {
    1, 26, 2, 3, 4, 27, 5, 6, 28, 7, 8, 9, 10, 11, 22, 23, 24, 32
};

// A state index of 256 in a sampler's block: the register the shader binds the sampler to.
const uint32_t SamplerSelf = 256;

// 64-bit FNV-1a of a shader's bytecode up to its END token.
uint64_t bytecodeHash(const uint8_t* code, uint32_t bytes)
{
    const uint32_t length = Shader::length(code, bytes);
    uint64_t h = 0xcbf29ce484222325ull;
    for (uint32_t i = 0; i < length; ++i)
        h = (h ^ code[i]) * 0x100000001b3ull;
    return h;
}

// The port's translations of the game's shaders, by bytecode hash (effect, technique, pass in the comment).
struct Translation
{
    uint64_t    hash;
    bool        pixel;
    uint8_t     program;        // render::VertexProgram or render::FragmentProgram
    bool        motionBlur = false;     // above the device's shader model: valid only with motion_blur=1
};

const Translation s_translations[] = {
    { 0xdb5d963353ba54feull, false, render::VpFe },             // FE world
    { 0xe35d34f19be1a32eull, true, render::FpFe },
    { 0x5a814375cbcb359aull, false, render::VpFeMask },         // FE_MASK world
    { 0x6d2610a5bf44f387ull, true, render::FpFeMask },
    { 0xe5f70cc831c579a3ull, false, render::VpFilter },         // FILTER filter p0-p5
    { 0xe2354ddcebc01cddull, true, render::FpFilter1 },
    { 0x1fb0a67a152dc612ull, true, render::FpFilter2 },
    { 0x1bb33328033cd514ull, true, render::FpFilter3 },
    { 0x23da752e36e0e44aull, true, render::FpFilter4 },
    { 0x0f1b345408b851c7ull, true, render::FpFilterLum },
    { 0x8f4ecafcbaef7139ull, true, render::FpFilterPremul },
    { 0x96da60cf48d99ce8ull, false, render::VpScreenFilter },   // SCREENFILTER filter p0, p1
    { 0xd148055ebb5bd746ull, true, render::FpScreenFilter },
    { 0x0004d079b65e6b7aull, true, render::FpScreenFilterGain },
    { 0x1d3d9219d682f43cull, false, render::VpPip },            // RVMPIP pip (shader 2)
    { 0x69334a615a0763cbull, true, render::FpPip },
    { 0xb43f7ea0b58928e8ull, false, render::VpScreen },         // vs_1_1 screen passes (VISUALTREATMENT, SCREENEFFECT)
    { 0x4f57b9c22c4eca26ull, false, render::VpScreen },         // vs_2_0 screen passes (VISUALTREATMENT)
    { 0xd53a672b716e1b2dull, true, render::FpVisualTreatment }, // VISUALTREATMENT visualtreatment (shader 2)
    { 0x0f538db52956ff17ull, true, render::FpPassthru },        // VISUALTREATMENT screen_passthru
    { 0x60dac52b83d76015ull, true, render::FpUves },            // VISUALTREATMENT uvesovercliff
    { 0x3f9963568f373db7ull, true, render::FpUvesDarken },      // VISUALTREATMENT uvesovercliffdarken
    { 0xddbec47f14e3750cull, true, render::FpBlend },           // SCREENEFFECT BlendTextures
    { 0x6d31115bb011e18bull, true, render::FpCombine },         // SCREENEFFECT CombineReflectionColourAndHeadlightAlpha
    { 0xd10efa110e403c34ull, true, render::FpDown2 },           // SCREENEFFECT DownScale2x2
    { 0x6eb3f27847de966cull, true, render::FpDown2MotionBlur }, // SCREENEFFECT DownScale2x2ForMotionBlur
    { 0xe8544c69d649d951ull, true, render::FpDown4 },           // SCREENEFFECT DownScale4x4
    { 0xa4e360cfe18d23e5ull, true, render::FpDownBloom },       // SCREENEFFECT DownScaleForBloom
    { 0xca370e07205b522full, true, render::FpGauss },           // SCREENEFFECT GaussBlur5x5
    { 0x15d747e9137c954dull, true, render::FpBlur15 },          // SCREENEFFECT TwoPassBlur
    { 0xb994c09f25b1cb20ull, false, render::VpCar },            // CAR car (shader 2)
    { 0x69a69244118cb705ull, true, render::FpCar },
    { 0x31c86273aff938a7ull, false, render::VpGlass },          // GLASSREFLECTSHADER glassreflect (shader 2)
    { 0xdbb2dc4e2c9f5e7full, true, render::FpGlass },
    { 0x815a01bada91b5a6ull, false, render::VpGlassLow },       // GLASSREFLECTSHADER, WORLDNORMALMAP lowlod
    { 0x5a713cd381f14ff1ull, true, render::FpGlassLow },
    { 0x3d07db701182fbf2ull, false, render::VpCarNormalMap },   // CARNORMALMAP car_normalmap (shader 2)
    { 0x9937fcfbb18afe76ull, true, render::FpCarNormalMap },
    { 0x208c46b5e7a0a2daull, false, render::VpUcap },           // UCAP main (shader 2)
    { 0xa95639d1ff94651aull, true, render::FpUcap },
    { 0x6248dc70a0b1af50ull, false, render::VpWorld },          // WORLD world (shader 2)
    { 0x8387de4ffc9d454dull, true, render::FpWorld },
    { 0x486b497f96ff2307ull, false, render::VpWorldLow },       // WORLD lowlod
    { 0x17b1c6d244e9efd6ull, true, render::FpModulate },        // WORLD, WORLDNORMALMAP lowlod
    { 0xf0c3d55cd414b304ull, false, render::VpPrelit },         // WORLDPRELIT world
    { 0x00e473cea249a9d1ull, true, render::FpPrelit },
    { 0x333af1dff73d1e9aull, false, render::VpSky },            // SKY sky (shader 1; FE's pixel shader)
    { 0xe249411edcc536dbull, false, render::VpWorldNormalMap }, // WORLDNORMALMAP worldnormalmap (shader 2)
    { 0x673a61b02284cf4cull, true, render::FpWorldNormalMap },
    { 0x2b10de84b2c166c2ull, false, render::VpRoad },           // WORLDREFLECT dryroad, raining_on_road (shader 2)
    { 0xe7eddfefaed6786cull, true, render::FpRoad },
    { 0x6b2a287189c2c7f4ull, false, render::VpWorldBone },      // WORLDBONE world (shader 1)
    { 0xcfb4f2273869095eull, true, render::FpTexture },
    { 0xf481b23e4e8a24f6ull, false, render::VpInstance },       // INSTANCEMESH TShader_Instancing
    { 0x0b56375cb346b945ull, true, render::FpColor },
    { 0xa61d1f681501eaa2ull, false, render::VpWater },          // WATERSHADER water (shader 2)
    { 0x50e1b7e17777a848ull, true, render::FpWater },
    { 0xff464e8f7bf8b14full, false, render::VpParticle },       // PARTICLES fuzzz, no_fuzzz (shader 2)
    { 0xb2e64e0b1318e653ull, true, render::FpParticle },
    { 0xfac3d11dec0c5a62ull, false, render::VpStreak },         // PARTICLES streak_flares (shader 2)
    { 0xb95865d841e6084cull, true, render::FpStreak },
    { 0xf8c119bb3e0f4a8full, false, render::VpFlare },          // PARTICLES flares (shader 2)
    { 0xb77a48f6c7cb0a02ull, true, render::FpFlare },
    { 0xfd2e17278c0b4894ull, false, render::VpRainDrop },       // RAIN_DROP filter
    { 0x8a0eb118b3c8e666ull, true, render::FpRainDrop },
    { 0x563146d9379e8417ull, false, render::VpGhost },          // GHOSTCAR ghostcar (shader 2)
    { 0x58d3998524ceb6e6ull, true, render::FpGhost },
    { 0x17a8f9f621b0e504ull, false, render::VpRvm },            // RVMPIP rvm (shader 2)
    { 0x048b940283c0b8a4ull, true, render::FpRvm },
    { 0x479c0facc134450dull, false, render::VpCaster },         // SHADOW_MAP_MESH world
    { 0x42e13806ac6a7c0full, true, render::FpCasterGreen },
    { 0x06d94afd353fd7feull, false, render::VpCaster },         // CAR_SHADOW_MAP car
    { 0x1f87486ec5dbefafull, true, render::FpCasterBlack },
    { 0xba7d7cca2519ad79ull, false, render::VpCasterDepth },    // SHADOW_MAP_MESH_DEPTH depth_technique_noalpha
    { 0xeb767b2810a0630cull, true, render::FpCasterDepth },
    { 0xf291a699a1703775ull, true, render::FpVisualTreatmentEnhanced },  // VISUALTREATMENT visualtreatment_enchanced
    { 0x5b01b04a3857378bull, false, render::VpDepth },          // WORLDDEPTH world
    { 0x6ab7a65c39a6bc6cull, true, render::FpDepthTexture },
    { 0x740fc25f6a3ff062ull, false, render::VpDepthSkinned },   // WORLDDEPTH skinned
    { 0xc4937a1e3713a8d1ull, true, render::FpCasterDepth },
    { 0xdcb619bfe49c24ecull, false, render::VpMotionBlur, true },   // VISUALTREATMENT motionblur (shader 3)
    { 0x863fc3d105e96a17ull, true, render::FpMotionBlur, true },
    { 0x00a100dd5b73b8f4ull, false, render::VpScreen, true },       // VISUALTREATMENT composite_blur (shader 3)
    { 0x375aef8c38cb7b04ull, true, render::FpComposite, true },
    { 0xa4369c21ff958289ull, false, render::VpCar },            // CAR car (shader 1; the same registers)
    { 0xd9472f802191f2c2ull, true, render::FpCar1 },
    { 0x5064c0f8c5844154ull, false, render::VpCarNormalMap1 },  // CARNORMALMAP car_normalmap (shader 1)
    { 0x94b5b4455de5b75aull, true, render::FpCarNormalMap1 },
    { 0x5321a5fda7fc0ec0ull, false, render::VpGlass1 },         // GLASSREFLECTSHADER glassreflect (shader 1)
    { 0xf039f3df990ff43eull, true, render::FpGlass1 },
    { 0xb2185417437ccd3full, false, render::VpGhost },          // GHOSTCAR ghostcar (shader 1)
    { 0x35cd2c3affd64422ull, true, render::FpGhost },
    { 0x61d8ec268ed33f6eull, false, render::VpPip },            // RVMPIP pip (shader 1)
    { 0xfc043634e40ac4ebull, true, render::FpPip1 },
    { 0xe58093030a89dc6eull, false, render::VpRvm },            // RVMPIP rvm (shader 1)
    { 0x2e3109544d363771ull, true, render::FpRvm1 },
    { 0x7cc3bb16bc501946ull, false, render::VpScreen },         // VISUALTREATMENT visualtreatment (shader 1)
    { 0x20496dd6b5b393a6ull, true, render::FpVisualTreatment1 },
    { 0xaf74f0eb9e67f002ull, true, render::FpParticle1 },       // PARTICLES no_fuzzz (shader 1)
    { 0xcd522c832bb04251ull, false, render::VpFlare1 },         // PARTICLES flares (shader 1; FE's pixel shader)
    { 0x5ee7f886aaec84fdull, false, render::VpRoad1 },          // WORLDREFLECT dryroad, raining_on_road (shader 1)
    { 0xc4166fddeeafe159ull, true, render::FpRoad1 },
    { 0x391a7380ccd6ccb6ull, true, render::FpOpaque },          // WATERSHADER water (shader 1)
    { 0xafd878f5563f9c0bull, false, render::VpWorld1 },         // WORLD world_1_1
    { 0x0629ab428cce0ff3ull, true, render::FpRoad1 },           // (the road's pixel shader, other compiler build)
    { 0x30b165f9390f2995ull, false, render::VpWorldNormalMap1 },    // WORLDNORMALMAP worldnormalmap_1_1
    { 0x9e041a9c72460c32ull, false, render::VpUcap1 },          // UCAP main_1_1
    { 0xf68fe200a7e388f7ull, true, render::FpTextureOpaque },
};

// The fragment program of a translation: with vt_lut=nearest (unless it is "blend") the visual treatment programs
// that read the nearest colour cube slice.
uint8_t translatedFragment(uint8_t program)
{
    static const bool s_blend = strcasecmp(pf::Config::getString("vt_lut", "nearest"), "blend") == 0;
    if (!s_blend && program == render::FpVisualTreatment)
        return render::FpVisualTreatmentNearest;
    if (!s_blend && program == render::FpVisualTreatmentEnhanced)
        return render::FpVisualTreatmentEnhancedNearest;
    return program;
}

// Pixel programs replaced by another when paired with a vertex program (the same pixel shader with fog).
struct Pairing
{
    uint8_t     vertex, fragment, result;
};

const Pairing s_pairings[] = {
    { render::VpSky, render::FpFe, render::FpFeFog },
    { render::VpStreak, render::FpFe, render::FpModulate },
    { render::VpFlare1, render::FpFe, render::FpModulate },
};

const uint8_t NoProgram = 0xff;

// A constant of a shader's constant table (D3DXSHADER_CONSTANTINFO and its type).
struct TableConstant
{
    std::string name;
    uint16_t    registerSet, registerIndex, registerCount;
    uint16_t    cls, rows, columns, elements;
};

enum : uint16_t
{
    RegisterSetBool, RegisterSetInt4, RegisterSetFloat4, RegisterSetSampler
};

// The constant table of a comment payload (after its CTAB tag); false when malformed.
bool parseConstantTable(const uint8_t* table, uint32_t size, std::vector<TableConstant>& out)
{
    auto u16 = [table](uint32_t at) { uint16_t v; memcpy(&v, table + at, 2); return v; };
    auto u32 = [table](uint32_t at) { uint32_t v; memcpy(&v, table + at, 4); return v; };
    if (size < 28)
        return false;
    const uint32_t count = u32(12), info = u32(16);
    if (info > size || count > (size - info) / 20)
        return false;
    for (uint32_t i = 0; i < count; ++i)
    {
        const uint32_t at = info + 20 * i;
        const uint32_t name = u32(at), type = u32(at + 12);
        if (name >= size || type > size - 12)
            return false;
        TableConstant c;
        c.name.assign(reinterpret_cast<const char*>(table + name), strnlen(reinterpret_cast<const char*>(table + name),
                                                                              size - name));
        c.registerSet = u16(at + 4);
        c.registerIndex = u16(at + 6);
        c.registerCount = u16(at + 8);
        c.cls = u16(type);
        c.rows = u16(type + 4);
        c.columns = u16(type + 6);
        c.elements = u16(type + 8);
        out.push_back(c);
    }
    return true;
}

// The comment payload with a tag (CTAB, FXLC, ...) among the comments after the version token; null when none.
const uint8_t* commentBlock(const uint8_t* code, uint32_t bytes, uint32_t tag, uint32_t* size)
{
    uint32_t at = 4;
    while (at + 8 <= bytes)
    {
        uint32_t token, fourcc;
        memcpy(&token, code + at, 4);
        if ((token & 0xffff) != 0xfffe)
            return nullptr;
        const uint32_t length = 4 * ((token >> 16) & 0x7fff);
        if (length < 4 || at + 4 + length > bytes)
            return nullptr;
        memcpy(&fourcc, code + at + 4, 4);
        if (fourcc == tag)
        {
            *size = length - 4;
            return code + at + 8;
        }
        at += 4 + length;
    }
    return nullptr;
}

const uint32_t TagCtab = 0x42415443, TagFxlc = 0x434c5846;   // "CTAB", "FXLC"

// fused_matrices (default 1): the matrix setters also write the registers of the current pass's transposed
// constants, which the uploads then take as they are.
bool fusedMatrices()
{
    static const bool s_fused = pf::Config::getBool("fused_matrices", true);
    return s_fused;
}

// A state's value: a literal, a parameter (TEXTURE = <param>), an expression of one parameter, or a shader.
enum StateKind : uint8_t
{
    StateLiteral, StateParameter, StateExpression, StateShader
};

struct Block;

struct State
{
    uint32_t    op = 0;
    uint32_t    index = 0;          // stage; SamplerSelf in a sampler's block
    uint8_t     kind = StateLiteral;
    uint8_t     valueType = TypeInt;
    DWORD       value = 0;          // literal bits
    int         param = -1;         // the parameter referenced or read by the expression
    int         shader = -1;        // index of the shader binding
    const Block* block = nullptr;   // the parameter's values (set after parsing)
};

// How a constant's registers are filled from its parameter: not at all (not numeric), the float values in order,
// each 4x4 float matrix transposed, or value by value with conversions.
enum ConstantLayout : uint8_t
{
    LayoutNone, LayoutCopy, LayoutTranspose, LayoutValues
};

// A float constant of a shader bound to a parameter, with its last upload: the parameter block's version then and
// the device's stamp for the registers it wrote (0: none).
struct ShaderConstant
{
    int         param;
    uint16_t    registerIndex, registerCount;
    uint16_t    cls, rows, columns, elements;
    const Block* block;             // the parameter's values
    uint32_t    version = 0;
    uint64_t    stamp = 0;
    uint8_t     layout = LayoutValues;
    uint32_t    words = 0;          // LayoutCopy: the parameter's values; LayoutTranspose: its matrices
    const uint8_t* values = nullptr;    // the parameter's storage
    std::vector<float> image = {};  // LayoutTranspose: the registers the matrix setters write with the storage
    uint32_t    imageVersion = 0;   // the block version image holds (0: none)
};

// A pass's shader: bytecode, translation, the parameters its constant table binds.
struct ShaderBinding
{
    x86::reg32                  code = 0;
    uint32_t                    bytes = 0;
    bool                        pixel = false;
    uint64_t                    hash = 0;
    uint8_t                     program = NoProgram;
    bool                        motionBlur = false;     // Translation::motionBlur
    std::vector<ShaderConstant> constants;
    std::vector<std::pair<int, uint32_t>> samplers;     // sampler parameter, register
};

// What CommitChanges applies for a pass, in the order BeginPass does, when its parameter block changed: a shader
// constant, or a state or sampler state read from a parameter.
struct Commit
{
    const Block*    block;
    ShaderConstant* constant;       // null: the state
    const State*    state;
    uint32_t        stage;          // sampler register of a sampler state
    bool            pixel;          // the constant's shader
};

// Handles below every guest address; larger values are names (D3DX accepts a name for a handle).
enum : x86::reg32
{
    HandleParam = 0x1000,
    HandleTechnique = 0x2000,
    HandlePass = 0x3000,
    HandleLimit = 0x1000
};

// Values of a top-level parameter; texture slots hold guest COM pointers with a reference.
struct Block
{
    std::vector<uint8_t>    bytes;
    std::vector<uint8_t>    slotTypes;      // per dword: TypeBool/Int/Float, or the object type
    uint32_t                version = 0;    // value of the change counter at the last write
    ~Block()
    {
        for (size_t i = 0; i < slotTypes.size(); ++i)
        {
            if (!isTexture(slotTypes[i]))
                continue;
            x86::reg32 texture;
            memcpy(&texture, bytes.data() + 4 * i, 4);
            if (Object* object = lookup(texture))
                object->release();
        }
    }
};

struct Param
{
    std::string             name, semantic;
    x86::reg32              guestName = 0, guestSemantic = 0;
    DWORD                   type = 0, cls = 0, rows = 0, columns = 0, elements = 0, flags = 0;
    uint32_t                bytes = 0;
    std::vector<int>        members;        // struct members, or the elements of an array
    std::vector<int>        annotations;
    std::shared_ptr<Block>  block;
    uint32_t                offset = 0;     // in block
    bool                    floats = false; // every value slot is a float
    std::vector<State>      samplerStates;  // a sampler's block
};

struct Pass
{
    std::string             name;
    x86::reg32              guestName = 0;
    std::vector<int>        annotations;
    x86::reg32              vertexShader = 0, pixelShader = 0;     // bytecode in the resource
    uint32_t                technique = 0;
    std::vector<State>      states;
    int                     vertexBinding = -1, pixelBinding = -1;  // shader bindings of the shader states
    std::vector<Commit>     commits;
    std::vector<std::pair<int, ShaderConstant*>> fused;    // the shaders' LayoutTranspose constants and parameters
    // Drawable with both shaders translated, or with none (fixed function: kind NoEffect); else draws are dropped.
    bool                    drawable = true;
    EffectDraw              programs = { 0, 0, NoEffect, 0 };
};

struct Technique
{
    std::string             name;
    x86::reg32              guestName = 0;
    std::vector<int>        annotations;
    std::vector<int>        passes;
};

x86::reg32 guestString(const std::string& text)
{
    const x86::reg32 address = heap::allocate(x86::reg32(text.size() + 1), false);
    if (address)
        memcpy(g_guestBase + address, text.c_str(), text.size() + 1);
    return address;
}

float toFloat(const uint8_t* p, uint8_t type)
{
    if (type == TypeFloat)
    {
        float f;
        memcpy(&f, p, 4);
        return f;
    }
    x86::sreg32 i;
    memcpy(&i, p, 4);
    return type == TypeBool ? (i ? 1.0f : 0.0f) : float(i);
}

void fromFloat(uint8_t* p, uint8_t type, float value)
{
    if (type == TypeFloat)
    {
        memcpy(p, &value, 4);
        return;
    }
    const x86::sreg32 i = type == TypeBool ? (value != 0.0f ? 1 : 0) : x86::sreg32(value);
    memcpy(p, &i, 4);
}

x86::sreg32 toInt(const uint8_t* p, uint8_t type)
{
    if (type == TypeFloat)
        return x86::sreg32(toFloat(p, type));
    x86::sreg32 i;
    memcpy(&i, p, 4);
    return type == TypeBool ? (i ? 1 : 0) : i;
}

void fromInt(uint8_t* p, uint8_t type, x86::sreg32 value)
{
    if (type == TypeFloat)
    {
        const float f = float(value);
        memcpy(p, &f, 4);
        return;
    }
    const x86::sreg32 i = type == TypeBool ? (value ? 1 : 0) : value;
    memcpy(p, &i, 4);
}

// The compiled effect: reads with bounds checks; strings are a size and the bytes.
class Reader
{
public:
    Reader(const uint8_t* data, uint32_t size) : m_data(data), m_size(size) {}
    bool ok() const { return m_ok; }
    uint32_t dword(uint32_t offset)
    {
        if (offset + 4 > m_size || offset + 4 < offset)
        {
            m_ok = false;
            return 0;
        }
        uint32_t v;
        memcpy(&v, m_data + offset, 4);
        return v;
    }
    std::string string(uint32_t offset)
    {
        const uint32_t length = dword(offset);
        if (!m_ok || offset + 4 + length > m_size)
        {
            m_ok = false;
            return std::string();
        }
        std::string s(reinterpret_cast<const char*>(m_data + offset + 4), length);
        while (!s.empty() && s.back() == 0)
            s.pop_back();
        return s;
    }
    const uint8_t* at(uint32_t offset, uint32_t bytes)
    {
        if (offset + bytes > m_size || offset + bytes < offset)
        {
            m_ok = false;
            return nullptr;
        }
        return m_data + offset;
    }

private:
    const uint8_t*  m_data;
    uint32_t        m_size;
    bool            m_ok = true;
};

}

class EffectPool : public Object
{
public:
    EffectPool() : Object(KindEffectPool) {}
    std::map<std::string, std::shared_ptr<Block>> shared;
};

class Effect;

class EffectPassProxy : public EffectPass
{
public:
    Effect*     effect = nullptr;
    uint32_t    pass = 0;
    void dropped() const override;
};

class Effect : public Object
{
public:
    Effect(Device* device, EffectPool* pool, const std::string& resourceName)
        :   Object(KindEffect), m_device(device), m_pool(pool), m_resourceName(resourceName)
    {
        device->addRef();
        if (pool)
            pool->addRef();
        m_proxy.effect = this;
    }
    ~Effect() override
    {
        if (m_device->effectPass() == &m_proxy)
            m_device->setEffectPass(nullptr);
        for (const Saved& s : m_saved)
        {
            if (Object* object = s.object ? lookup(s.object) : nullptr)
                object->release();
        }
        if (m_pool)
            m_pool->release();
        m_device->release();
    }

    bool parse(const uint8_t* data, uint32_t size, x86::reg32 guestData);
    const std::string& resourceName() const { return m_resourceName; }
    // The native renderer's table of pass index of a technique (built on first use); null when it has none.
    native::Pass* nativePass(x86::reg32 technique, uint32_t index);
    // The registers of a constant from its parameter's current value (the storage itself for whole registers).
    const float* constantRegisters(const ShaderConstant& constant, float* registers) const;
    static uint32_t changes()               { return s_changes; }
    // A draw of a pass without programs is dropped: logged once per technique.
    void logDrop(uint32_t pass) const;

    HRESULT GetDesc(WinApplication* app, x86::CPU& cpu, DWORD* desc);
    HRESULT GetParameterDesc(WinApplication* app, x86::CPU& cpu, x86::reg32 handle, DWORD* desc);
    HRESULT GetTechniqueDesc(WinApplication* app, x86::CPU& cpu, x86::reg32 handle, DWORD* desc);
    HRESULT GetPassDesc(WinApplication* app, x86::CPU& cpu, x86::reg32 handle, DWORD* desc);
    x86::reg32 GetParameter(WinApplication* app, x86::CPU& cpu, x86::reg32 parent, UINT index);
    x86::reg32 GetParameterByName(WinApplication* app, x86::CPU& cpu, x86::reg32 parent, x86::reg32 name);
    x86::reg32 GetParameterBySemantic(WinApplication* app, x86::CPU& cpu, x86::reg32 parent, x86::reg32 semantic);
    x86::reg32 GetParameterElement(WinApplication* app, x86::CPU& cpu, x86::reg32 parent, UINT index);
    x86::reg32 GetTechnique(WinApplication* app, x86::CPU& cpu, UINT index);
    x86::reg32 GetTechniqueByName(WinApplication* app, x86::CPU& cpu, x86::reg32 name);
    x86::reg32 GetPass(WinApplication* app, x86::CPU& cpu, x86::reg32 technique, UINT index);
    x86::reg32 GetPassByName(WinApplication* app, x86::CPU& cpu, x86::reg32 technique, x86::reg32 name);
    x86::reg32 GetAnnotation(WinApplication* app, x86::CPU& cpu, x86::reg32 object, UINT index);
    x86::reg32 GetAnnotationByName(WinApplication* app, x86::CPU& cpu, x86::reg32 object, x86::reg32 name);
    HRESULT SetValue(WinApplication* app, x86::CPU& cpu, x86::reg32 handle, x86::reg32 data, UINT bytes);
    HRESULT GetValue(WinApplication* app, x86::CPU& cpu, x86::reg32 handle, x86::reg32 data, UINT bytes);
    HRESULT SetBool(WinApplication* app, x86::CPU& cpu, x86::reg32 handle, BOOL value);
    HRESULT GetBool(WinApplication* app, x86::CPU& cpu, x86::reg32 handle, BOOL* value);
    HRESULT SetBoolArray(WinApplication* app, x86::CPU& cpu, x86::reg32 handle, const BOOL* values, UINT count);
    HRESULT GetBoolArray(WinApplication* app, x86::CPU& cpu, x86::reg32 handle, BOOL* values, UINT count);
    HRESULT SetInt(WinApplication* app, x86::CPU& cpu, x86::reg32 handle, x86::sreg32 value);
    HRESULT GetInt(WinApplication* app, x86::CPU& cpu, x86::reg32 handle, x86::sreg32* value);
    HRESULT SetIntArray(WinApplication* app, x86::CPU& cpu, x86::reg32 handle, const x86::sreg32* values, UINT count);
    HRESULT GetIntArray(WinApplication* app, x86::CPU& cpu, x86::reg32 handle, x86::sreg32* values, UINT count);
    HRESULT SetFloat(WinApplication* app, x86::CPU& cpu, x86::reg32 handle, float value);
    HRESULT GetFloat(WinApplication* app, x86::CPU& cpu, x86::reg32 handle, float* value);
    HRESULT SetFloatArray(WinApplication* app, x86::CPU& cpu, x86::reg32 handle, const float* values, UINT count);
    HRESULT GetFloatArray(WinApplication* app, x86::CPU& cpu, x86::reg32 handle, float* values, UINT count);
    HRESULT SetVector(WinApplication* app, x86::CPU& cpu, x86::reg32 handle, const float* vector);
    HRESULT GetVector(WinApplication* app, x86::CPU& cpu, x86::reg32 handle, float* vector);
    HRESULT SetVectorArray(WinApplication* app, x86::CPU& cpu, x86::reg32 handle, const float* vectors, UINT count);
    HRESULT GetVectorArray(WinApplication* app, x86::CPU& cpu, x86::reg32 handle, float* vectors, UINT count);
    HRESULT SetMatrix(WinApplication* app, x86::CPU& cpu, x86::reg32 handle, const float* matrix);
    HRESULT GetMatrix(WinApplication* app, x86::CPU& cpu, x86::reg32 handle, float* matrix);
    HRESULT SetMatrixArray(WinApplication* app, x86::CPU& cpu, x86::reg32 handle, const float* matrices, UINT count);
    HRESULT GetMatrixArray(WinApplication* app, x86::CPU& cpu, x86::reg32 handle, float* matrices, UINT count);
    HRESULT SetMatrixPointerArray(WinApplication* app, x86::CPU& cpu, x86::reg32 handle, const x86::reg32* matrices,
                                  UINT count);
    HRESULT SetMatrixTranspose(WinApplication* app, x86::CPU& cpu, x86::reg32 handle, const float* matrix);
    HRESULT GetMatrixTranspose(WinApplication* app, x86::CPU& cpu, x86::reg32 handle, float* matrix);
    HRESULT SetMatrixTransposeArray(WinApplication* app, x86::CPU& cpu, x86::reg32 handle, const float* matrices,
                                    UINT count);
    HRESULT GetMatrixTransposeArray(WinApplication* app, x86::CPU& cpu, x86::reg32 handle, float* matrices,
                                    UINT count);
    HRESULT SetMatrixTransposePointerArray(WinApplication* app, x86::CPU& cpu, x86::reg32 handle,
                                           const x86::reg32* matrices, UINT count);
    HRESULT SetString(WinApplication* app, x86::CPU& cpu, x86::reg32 handle, x86::reg32 text);
    HRESULT GetString(WinApplication* app, x86::CPU& cpu, x86::reg32 handle, x86::reg32* text);
    HRESULT SetTexture(WinApplication* app, x86::CPU& cpu, x86::reg32 handle, x86::reg32 texture);
    HRESULT GetTexture(WinApplication* app, x86::CPU& cpu, x86::reg32 handle, x86::reg32* texture);
    HRESULT SetArrayRange(WinApplication* app, x86::CPU& cpu, x86::reg32 handle, UINT start, UINT end);
    HRESULT GetPool(WinApplication* app, x86::CPU& cpu, x86::reg32* pool);
    HRESULT SetTechnique(WinApplication* app, x86::CPU& cpu, x86::reg32 technique);
    x86::reg32 GetCurrentTechnique(WinApplication* app, x86::CPU& cpu);
    HRESULT ValidateTechnique(WinApplication* app, x86::CPU& cpu, x86::reg32 technique);
    HRESULT FindNextValidTechnique(WinApplication* app, x86::CPU& cpu, x86::reg32 technique, x86::reg32* next);
    BOOL IsParameterUsed(WinApplication* app, x86::CPU& cpu, x86::reg32 param, x86::reg32 technique);
    HRESULT Begin(WinApplication* app, x86::CPU& cpu, UINT* passes, DWORD flags);
    HRESULT BeginPass(WinApplication* app, x86::CPU& cpu, UINT pass);
    HRESULT CommitChanges(WinApplication* app, x86::CPU& cpu);
    HRESULT EndPass(WinApplication* app, x86::CPU& cpu);
    HRESULT End(WinApplication* app, x86::CPU& cpu);
    HRESULT GetDevice(WinApplication* app, x86::CPU& cpu, x86::reg32* device);
    HRESULT OnLostDevice(WinApplication* app, x86::CPU& cpu);
    HRESULT OnResetDevice(WinApplication* app, x86::CPU& cpu);

private:
    int parseTypedef(Reader& r, uint32_t offset, uint32_t* end, bool element);
    static uint32_t typedefBytes(Reader& r, uint32_t offset, uint32_t* end);
    bool parseValue(Reader& r, int p, uint32_t offset);
    void assignStorage(int p, const std::shared_ptr<Block>& block, uint32_t offset);
    std::vector<int> parseAnnotations(Reader& r, uint32_t& at, uint32_t count);

    // A parameter or annotation handle (or name) to its index, -1 if none.
    int param(x86::reg32 handle) const;
    int technique(x86::reg32 handle) const;
    int pass(x86::reg32 handle) const;
    bool techniqueValid(int t) const;
    uint8_t* slot(int p, uint32_t i) { return m_params[size_t(p)].block->bytes.data() + m_params[size_t(p)].offset + 4 * i; }
    const uint8_t* slot(int p, uint32_t i) const
    {
        return m_params[size_t(p)].block->bytes.data() + m_params[size_t(p)].offset + 4 * i;
    }
    uint8_t slotType(int p, uint32_t i) const
    {
        return m_params[size_t(p)].block->slotTypes[(m_params[size_t(p)].offset >> 2) + i];
    }
    uint32_t slots(int p) const { return m_params[size_t(p)].bytes / 4; }
    bool numeric(int p) const { return m_params[size_t(p)].cls != ClassObject; }
    HRESULT setFloats(int p, const float* values, uint32_t count);
    HRESULT getFloats(int p, float* values, uint32_t count);
    HRESULT setMatrices(x86::reg32 handle, const float* matrices, UINT count, bool transpose);
    void fuseMatrices(int p, const float* matrices, uint32_t count, bool transpose, uint32_t before);
    HRESULT getMatrices(x86::reg32 handle, float* matrices, UINT count, bool transpose);
    void setTextureSlot(int p, uint32_t i, x86::reg32 texture);
    // Marks a parameter's value as changed (for CommitChanges).
    void touch(int p) { m_params[size_t(p)].block->version = ++s_changes; }

    // States and shaders.
    bool parseState(Reader& r, uint32_t at, State& state);
    bool parseExpression(const uint8_t* code, uint32_t bytes, State& state);
    int topParam(const std::string& name) const;
    void bindShaders();
    // The DWORD a state sets: the literal, the expression's result or the referenced parameter's value.
    DWORD stateValue(const State& state);
    // Applies a state (stage: the sampler register for a sampler's block).
    void applyState(WinApplication* app, x86::CPU& cpu, const State& state, uint32_t stage);
    void applyShader(WinApplication* app, x86::CPU& cpu, ShaderBinding& shader);
    void setLayout(ShaderConstant& constant) const;
    void uploadConstant(ShaderConstant& constant, bool pixel);
    // The device states the technique's passes set, captured at Begin and restored at End.
    struct Saved
    {
        uint8_t     kind;           // 0 render, 1 stage, 2 sampler, 3 texture, 4 vertex input
        uint32_t    index, type;
        DWORD       value;
        x86::reg32  object;         // texture or vertex declaration, with a reference
    };
    void collectSaved(const State& state, uint32_t stage, DWORD flags, std::vector<Saved>& out) const;
    void captureState(WinApplication* app, x86::CPU& cpu, DWORD flags);
    void restoreState(WinApplication* app, x86::CPU& cpu);

    static uint32_t             s_changes;          // parameter change counter, all effects

    Device*                     m_device;
    EffectPool*                 m_pool;
    std::string                 m_resourceName;
    std::vector<Param>          m_params;           // parameters, members, elements and annotations
    std::vector<int>            m_top;              // top-level parameters
    std::vector<Technique>      m_techniques;
    std::vector<Pass>           m_passes;
    std::map<uint32_t, x86::reg32> m_strings;       // string object id -> guest string
    std::vector<std::pair<int, uint32_t>> m_stringSlots;   // string parameters' slots and object ids
    std::vector<ShaderBinding>  m_shaders;
    int                         m_technique = 0;
    bool                        m_begun = false;
    int                         m_pass = -1;        // the pass begun, -1 outside BeginPass/EndPass
    bool                        m_fused = fusedMatrices();  // matrix setters write the pass's transposed registers
    uint32_t                    m_applied = 0;      // change counter when the pass's states were last applied
    std::vector<Saved>          m_saved;
    std::vector<Saved>          m_savedTemplate;    // the states m_saved holds, without values
    int                         m_savedTechnique = -1;  // technique and sampler flag of m_savedTemplate
    DWORD                       m_savedFlags = 0;
    EffectPassProxy             m_proxy;
    mutable std::vector<bool>   m_dropLogged;
    std::vector<std::unique_ptr<native::Pass>> m_native;    // per pass: its table, once built
    std::vector<bool>           m_nativeBuilt;
    void buildNative(int pass, native::Pass& table);
    void nativeState(const State& state, uint32_t stage, native::Pass& table) const;
};

uint32_t Effect::s_changes = 0;

}}

namespace win32
{

D3D9_UNPACKER(d3d9::Effect)
D3D9_UNPACKER(d3d9::EffectPool)

}

namespace win32 { namespace d3d9
{

void EffectPassProxy::dropped() const
{
    effect->logDrop(pass);
}

void Effect::logDrop(uint32_t p) const
{
    const Pass& pass = m_passes[p];
    const ShaderBinding* vs = pass.vertexBinding >= 0 ? &m_shaders[size_t(pass.vertexBinding)] : nullptr;
    const ShaderBinding* ps = pass.pixelBinding >= 0 ? &m_shaders[size_t(pass.pixelBinding)] : nullptr;
    const uint32_t t = pass.technique;
    if (t < m_dropLogged.size() && !m_dropLogged[t])
    {
        m_dropLogged[t] = true;
        pf::logInfo("d3dx9: effect %s technique %s has no program: its draws are dropped (pass %s, vertex shader "
                    "%016llx, pixel shader %016llx)", m_resourceName.c_str(), m_techniques[t].name.c_str(),
                    pass.name.c_str(), static_cast<unsigned long long>(vs ? vs->hash : 0),
                    static_cast<unsigned long long>(ps ? ps->hash : 0));
    }
}

// ---- Parsing -------------------------------------------------------------------------------------------------------

// Bytes of the values of a typedef, and the end of the typedef.
uint32_t Effect::typedefBytes(Reader& r, uint32_t offset, uint32_t* end)
{
    const DWORD cls = r.dword(offset + 4);
    const DWORD elements = r.dword(offset + 16);
    uint32_t at = offset + 20;
    uint32_t bytes = 4;
    if (cls <= ClassMatrixColumns)
    {
        bytes = 4 * r.dword(at) * r.dword(at + 4);
        at += 8;
    }
    else if (cls == ClassStruct)
    {
        const uint32_t count = r.dword(at);
        at += 4;
        bytes = 0;
        for (uint32_t i = 0; i < count && r.ok(); ++i)
            bytes += typedefBytes(r, at, &at);
    }
    *end = at;
    return bytes * (elements ? elements : 1);
}

// Type, class, name, semantic, element count, then rows and columns or the inline struct members. An array gets one
// parameter per element (without name), each with members of its own. end: past the typedef.
int Effect::parseTypedef(Reader& r, uint32_t offset, uint32_t* end, bool element)
{
    const int index = int(m_params.size());
    m_params.emplace_back();
    Param info;
    info.type = r.dword(offset);
    info.cls = r.dword(offset + 4);
    const uint32_t name = r.dword(offset + 8), semantic = r.dword(offset + 12);
    if (!element)
    {
        info.name = name ? r.string(name) : std::string();
        info.semantic = semantic ? r.string(semantic) : std::string();
        info.elements = r.dword(offset + 16);
    }
    uint32_t at = offset + 20;
    uint32_t elementBytes = 4;
    if (info.cls <= ClassMatrixColumns)
    {
        info.columns = r.dword(at);
        info.rows = r.dword(at + 4);
        at += 8;
        elementBytes = 4 * info.rows * info.columns;
    }
    else if (info.cls == ClassStruct)
    {
        const uint32_t count = r.dword(at);
        at += 4;
        elementBytes = 0;
        for (uint32_t i = 0; i < count && r.ok(); ++i)
        {
            if (info.elements)
            {
                // Each element parses its own members below; here only their size.
                elementBytes += typedefBytes(r, at, &at);
            }
            else
            {
                const int member = parseTypedef(r, at, &at, false);
                info.members.push_back(member);
                elementBytes += m_params[size_t(member)].bytes;
            }
        }
    }
    info.bytes = elementBytes * (info.elements ? info.elements : 1);
    info.guestName = info.name.empty() ? 0 : guestString(info.name);
    info.guestSemantic = info.semantic.empty() ? 0 : guestString(info.semantic);
    if (info.elements)
    {
        for (uint32_t e = 0; e < info.elements && r.ok(); ++e)
        {
            uint32_t ignored;
            info.members.push_back(parseTypedef(r, offset, &ignored, true));
        }
    }
    m_params[size_t(index)] = info;
    if (end)
        *end = at;
    return index;
}

void Effect::assignStorage(int index, const std::shared_ptr<Block>& block, uint32_t offset)
{
    Param& p = m_params[size_t(index)];
    p.block = block;
    p.offset = offset;
    if (p.members.empty())
    {
        const uint8_t type = p.cls == ClassObject ? uint8_t(p.type) : uint8_t(p.type == TypeVoid ? TypeFloat : p.type);
        for (uint32_t i = 0; i < p.bytes / 4; ++i)
            block->slotTypes[offset / 4 + i] = type;
        p.floats = type == TypeFloat;
        return;
    }
    uint32_t at = offset;
    bool floats = true;
    for (int m : std::vector<int>(p.members))
    {
        assignStorage(m, block, at);
        at += m_params[size_t(m)].bytes;
        floats = floats && m_params[size_t(m)].floats;
    }
    m_params[size_t(index)].floats = floats;
}

// Numeric values are the bytes as stored; objects are one object id per slot (strings resolve at the end).
bool Effect::parseValue(Reader& r, int index, uint32_t offset)
{
    const Param& p = m_params[size_t(index)];
    const uint8_t* source = r.at(offset, p.bytes);
    if (!source)
        return false;
    for (uint32_t i = 0; i < p.bytes / 4; ++i)
    {
        const uint8_t type = slotType(index, i);
        if (type == TypeString)
        {
            uint32_t id;
            memcpy(&id, source + 4 * i, 4);
            m_stringSlots.emplace_back(index, i);
            memcpy(slot(index, i), &id, 4);
        }
        else if (type <= TypeFloat)
        {
            memcpy(slot(index, i), source + 4 * i, 4);
        }
    }
    return true;
}

std::vector<int> Effect::parseAnnotations(Reader& r, uint32_t& at, uint32_t count)
{
    std::vector<int> result;
    for (uint32_t i = 0; i < count && r.ok(); ++i)
    {
        const uint32_t type = r.dword(at), value = r.dword(at + 4);
        at += 8;
        const int a = parseTypedef(r, type, nullptr, false);
        auto block = std::make_shared<Block>();
        block->bytes.assign(m_params[size_t(a)].bytes, 0);
        block->slotTypes.assign(m_params[size_t(a)].bytes / 4, 0);
        assignStorage(a, block, 0);
        parseValue(r, a, value);
        result.push_back(a);
    }
    return result;
}

bool Effect::parse(const uint8_t* data, uint32_t size, x86::reg32 guestData)
{
    Reader top(data, size);
    if (top.dword(0) != 0xfeff0901)
        return false;
    const uint32_t start = top.dword(4);
    Reader r(data + 8, size - 8);
    uint32_t at = start;
    const uint32_t parameterCount = r.dword(at), techniqueCount = r.dword(at + 4);
    at += 16;
    for (uint32_t i = 0; i < parameterCount && r.ok(); ++i)
    {
        const uint32_t type = r.dword(at), value = r.dword(at + 4), flags = r.dword(at + 8);
        const uint32_t annotationCount = r.dword(at + 12);
        at += 16;
        std::vector<int> annotations = parseAnnotations(r, at, annotationCount);
        const int p = parseTypedef(r, type, nullptr, false);
        m_params[size_t(p)].flags = flags;
        m_params[size_t(p)].annotations = annotations;
        m_top.push_back(p);
        const bool shared = (flags & 1) && m_pool;
        std::shared_ptr<Block> block;
        if (shared)
        {
            std::shared_ptr<Block>& entry = m_pool->shared[m_params[size_t(p)].name];
            if (entry && entry->bytes.size() == m_params[size_t(p)].bytes)
                block = entry;
        }
        const bool fresh = !block;
        if (fresh)
        {
            block = std::make_shared<Block>();
            block->bytes.assign(m_params[size_t(p)].bytes, 0);
            block->slotTypes.assign(m_params[size_t(p)].bytes / 4, 0);
            if (shared)
                m_pool->shared[m_params[size_t(p)].name] = block;
        }
        assignStorage(p, block, 0);
        if (fresh)
            parseValue(r, p, value);
        Param& param = m_params[size_t(p)];
        if (isSampler(param.type) && param.cls == ClassObject)
        {
            // The sampler's state block: a count, then the states.
            if (param.elements)
                pf::logError("d3dx9: effect %s: sampler array %s: its states are not applied", m_resourceName.c_str(),
                             param.name.c_str());
            const uint32_t count = param.elements ? 0 : r.dword(value);
            for (uint32_t s = 0; s < count && r.ok(); ++s)
            {
                State state;
                parseState(r, value + 4 + 16 * s, state);
                m_params[size_t(p)].samplerStates.push_back(state);
            }
        }
    }
    for (uint32_t t = 0; t < techniqueCount && r.ok(); ++t)
    {
        Technique technique;
        technique.name = r.string(r.dword(at));
        const uint32_t annotationCount = r.dword(at + 4), passCount = r.dword(at + 8);
        at += 12;
        technique.annotations = parseAnnotations(r, at, annotationCount);
        for (uint32_t i = 0; i < passCount && r.ok(); ++i)
        {
            Pass pass;
            pass.name = r.string(r.dword(at));
            const uint32_t passAnnotations = r.dword(at + 4), stateCount = r.dword(at + 8);
            at += 12;
            pass.annotations = parseAnnotations(r, at, passAnnotations);
            for (uint32_t s = 0; s < stateCount && r.ok(); ++s)
            {
                State state;
                parseState(r, at, state);
                pass.states.push_back(state);
                at += 16;
            }
            pass.technique = t;
            pass.guestName = guestString(pass.name);
            technique.passes.push_back(int(m_passes.size()));
            m_passes.push_back(pass);
        }
        technique.guestName = guestString(technique.name);
        m_techniques.push_back(technique);
    }
    const uint32_t stringCount = r.dword(at), resourceCount = r.dword(at + 4);
    at += 8;
    for (uint32_t i = 0; i < stringCount && r.ok(); ++i)
    {
        const uint32_t id = r.dword(at);
        m_strings[id] = guestString(r.string(at + 4));
        at += 8 + ((r.dword(at + 4) + 3) & ~3u);
    }
    // Resources: a pass's state (technique, pass, state) or a sampler parameter's state (no technique, parameter,
    // element, state) gets shader bytecode or an expression (usage 0) or a parameter name (usage 1).
    for (uint32_t i = 0; i < resourceCount && r.ok(); ++i)
    {
        const uint32_t techniqueIndex = r.dword(at), index = r.dword(at + 4), stateIndex = r.dword(at + 12);
        const uint32_t usage = r.dword(at + 16), bytes = r.dword(at + 20);
        const uint32_t blob = at + 24;
        at += 24 + ((bytes + 3) & ~3u);
        const uint8_t* data = r.at(blob, bytes);
        State* state = nullptr;
        Pass* pass = nullptr;
        if (techniqueIndex == 0xffffffff)
        {
            if (index < m_top.size() && stateIndex < m_params[size_t(m_top[index])].samplerStates.size())
                state = &m_params[size_t(m_top[index])].samplerStates[stateIndex];
        }
        else if (techniqueIndex < m_techniques.size() && index < m_techniques[techniqueIndex].passes.size())
        {
            pass = &m_passes[size_t(m_techniques[techniqueIndex].passes[index])];
            if (stateIndex < pass->states.size())
                state = &pass->states[stateIndex];
        }
        if (!state || !data)
        {
            pf::logError("d3dx9: effect %s: resource %u has no state", m_resourceName.c_str(), i);
            continue;
        }
        if (usage == 1)
        {
            state->kind = StateParameter;
            state->param = topParam(std::string(reinterpret_cast<const char*>(data), strnlen(
                reinterpret_cast<const char*>(data), bytes)));
            if (state->param < 0)
                pf::logError("d3dx9: effect %s: resource %u names an unknown parameter", m_resourceName.c_str(), i);
        }
        else if (usage == 0 && (state->op == OpVertexShader || state->op == OpPixelShader) && bytes >= 4)
        {
            ShaderBinding shader;
            shader.code = guestData + 8 + blob;
            shader.bytes = bytes;
            shader.pixel = state->op == OpPixelShader;
            state->kind = StateShader;
            state->shader = int(m_shaders.size());
            m_shaders.push_back(shader);
            if (pass && !shader.pixel && !pass->vertexShader)
                pass->vertexShader = shader.code;
            else if (pass && shader.pixel && !pass->pixelShader)
                pass->pixelShader = shader.code;
        }
        else if (usage != 0 || !parseExpression(data, bytes, *state))
        {
            pf::logError("d3dx9: effect %s: resource %u (usage %u, state operation %u) is not supported",
                         m_resourceName.c_str(), i, usage, state->op);
        }
    }
    for (const auto& s : m_stringSlots)
    {
        uint32_t id;
        memcpy(&id, slot(s.first, s.second), 4);
        const auto it = m_strings.find(id);
        const x86::reg32 text = it != m_strings.end() ? it->second : 0;
        memcpy(slot(s.first, s.second), &text, 4);
    }
    m_dropLogged.assign(m_techniques.size(), false);
    // States that read a parameter keep its block.
    auto link = [this](State& s) { s.block = s.param >= 0 ? m_params[size_t(s.param)].block.get() : nullptr; };
    for (Pass& pass : m_passes)
        for (State& s : pass.states)
            link(s);
    for (Param& p : m_params)
        for (State& s : p.samplerStates)
            link(s);
    bindShaders();
    return r.ok() && at <= size - 8;
}

// Each shader's translation and the parameters of its constant table; a translation whose registers or samplers do
// not cover the table's is refused.
void Effect::bindShaders()
{
    for (Pass& pass : m_passes)
    {
        for (const State& state : pass.states)
        {
            if (state.kind == StateShader)
                (state.op == OpPixelShader ? pass.pixelBinding : pass.vertexBinding) = state.shader;
        }
    }
    for (ShaderBinding& shader : m_shaders)
    {
        const uint8_t* code = g_guestBase + shader.code;
        shader.hash = bytecodeHash(code, shader.bytes);
        for (const Translation& t : s_translations)
        {
            if (t.hash == shader.hash && t.pixel == shader.pixel)
            {
                shader.program = t.pixel ? translatedFragment(t.program) : t.program;
                shader.motionBlur = t.motionBlur;
            }
        }
        uint32_t size = 0;
        const uint8_t* table = commentBlock(code, shader.bytes, TagCtab, &size);
        std::vector<TableConstant> constants;
        if (table && !parseConstantTable(table, size, constants))
        {
            pf::logError("d3dx9: effect %s: shader %016llx has a malformed constant table", m_resourceName.c_str(),
                         static_cast<unsigned long long>(shader.hash));
            shader.program = NoProgram;
            continue;
        }
        uint32_t registers = 0, samplers = 0;
        if (shader.program != NoProgram)
        {
            registers = shader.pixel ? render::fragmentProgram(render::FragmentProgram(shader.program)).registers
                                     : render::vertexProgram(render::VertexProgram(shader.program)).registers;
            samplers = shader.pixel ? render::fragmentProgram(render::FragmentProgram(shader.program)).samplers : 0;
        }
        for (const TableConstant& c : constants)
        {
            const int p = topParam(c.name);
            if (p < 0 || (c.registerSet != RegisterSetFloat4 && c.registerSet != RegisterSetSampler))
            {
                pf::logError("d3dx9: effect %s: shader %016llx constant %s (register set %u) is not bound",
                             m_resourceName.c_str(), static_cast<unsigned long long>(shader.hash), c.name.c_str(),
                             c.registerSet);
                continue;
            }
            const uint32_t end = uint32_t(c.registerIndex) + c.registerCount;
            if (shader.program != NoProgram && end > (c.registerSet == RegisterSetSampler ? samplers : registers))
            {
                pf::logError("d3dx9: effect %s: shader %016llx reads %s beyond its translation", m_resourceName.c_str(),
                             static_cast<unsigned long long>(shader.hash), c.name.c_str());
                shader.program = NoProgram;
            }
            if (c.registerSet == RegisterSetSampler)
            {
                for (uint32_t k = 0; k < c.registerCount; ++k)
                    shader.samplers.emplace_back(p, c.registerIndex + k);
            }
            else
            {
                shader.constants.push_back(ShaderConstant{ p, c.registerIndex, c.registerCount, c.cls, c.rows,
                                                           c.columns, c.elements, m_params[size_t(p)].block.get() });
                setLayout(shader.constants.back());
                if (shader.constants.back().layout == LayoutTranspose)
                    shader.constants.back().image.assign(4 * std::min<uint32_t>(c.registerCount, 256), 0.0f);
            }
        }
    }
    for (Pass& pass : m_passes)
    {
        const ShaderBinding* vs = pass.vertexBinding >= 0 ? &m_shaders[size_t(pass.vertexBinding)] : nullptr;
        const ShaderBinding* ps = pass.pixelBinding >= 0 ? &m_shaders[size_t(pass.pixelBinding)] : nullptr;
        if (vs && ps && vs->program != NoProgram && ps->program != NoProgram)
        {
            uint8_t fragment = ps->program;
            for (const Pairing& p : s_pairings)
            {
                if (p.vertex == vs->program && p.fragment == fragment)
                    fragment = p.result;
            }
            pass.programs = EffectDraw{ vs->program, fragment, 0, 0 };
        }
        else if (vs || ps)
        {
            pass.drawable = false;
        }
        for (const State& state : pass.states)
        {
            if (state.kind == StateShader)
            {
                ShaderBinding& shader = m_shaders[size_t(state.shader)];
                for (ShaderConstant& c : shader.constants)
                {
                    pass.commits.push_back(Commit{ c.block, &c, nullptr, 0, shader.pixel });
                    if (c.layout == LayoutTranspose && !c.image.empty())
                        pass.fused.emplace_back(c.param, &c);
                }
                for (const auto& sampler : shader.samplers)
                    for (const State& s : m_params[size_t(sampler.first)].samplerStates)
                    {
                        if (s.kind != StateLiteral && s.block)
                            pass.commits.push_back(Commit{ s.block, nullptr, &s, sampler.second, false });
                    }
            }
            else if (state.kind != StateLiteral && state.block)
            {
                pass.commits.push_back(Commit{ state.block, nullptr, &state, 0, false });
            }
        }
    }
}

int Effect::topParam(const std::string& name) const
{
    for (int p : m_top)
    {
        if (m_params[size_t(p)].name == name)
            return p;
    }
    return -1;
}

// Operation, index, value type and value offset; a numeric value is its first DWORD, an object value its id (the
// resources replace it).
bool Effect::parseState(Reader& r, uint32_t at, State& state)
{
    state.op = r.dword(at);
    state.index = r.dword(at + 4);
    const uint32_t type = r.dword(at + 8), value = r.dword(at + 12);
    state.valueType = uint8_t(r.dword(type));
    state.value = r.dword(value);
    if (state.op >= OpCount)
    {
        pf::logError("d3dx9: effect %s: state operation %u is not known", m_resourceName.c_str(), state.op);
        return false;
    }
    return r.ok();
}

// The expressions of these effects all copy one parameter: "mov" of input register c0.x (the constant table names
// the parameter) to output 0. Anything else is refused.
bool Effect::parseExpression(const uint8_t* code, uint32_t bytes, State& state)
{
    uint32_t size = 0, tableSize = 0;
    const uint8_t* fxlc = commentBlock(code, bytes, TagFxlc, &size);
    const uint8_t* table = commentBlock(code, bytes, TagCtab, &tableSize);
    std::vector<TableConstant> inputs;
    if (!fxlc || !table || size < 36 || !parseConstantTable(table, tableSize, inputs) || inputs.size() != 1)
        return false;
    uint32_t w[9];
    memcpy(w, fxlc, sizeof(w));
    // count 1; mov of 1 component; 1 input: no index, constant table 2, offset 0; output: no index, table 4, offset 0.
    const uint32_t expected[9] = { 1, 0x10000001, 1, 0, 2, 0, 0, 4, 0 };
    if (memcmp(w, expected, sizeof(w)) != 0 || inputs[0].registerSet != RegisterSetFloat4 || inputs[0].registerIndex)
        return false;
    const int p = topParam(inputs[0].name);
    if (p < 0)
        return false;
    state.kind = StateExpression;
    state.param = p;
    return true;
}

// ---- Applying passes -----------------------------------------------------------------------------------------------

DWORD Effect::stateValue(const State& state)
{
    if (state.kind == StateLiteral || state.param < 0)
        return state.value;
    DWORD raw;
    memcpy(&raw, slot(state.param, 0), 4);
    if (state.kind == StateParameter)
        return raw;
    // The expression copies the parameter as a float; the state takes it in its value type.
    const float f = toFloat(slot(state.param, 0), slotType(state.param, 0));
    if (state.valueType == TypeFloat)
    {
        memcpy(&raw, &f, 4);
        return raw;
    }
    return state.valueType == TypeBool ? (f != 0.0f ? 1 : 0) : DWORD(x86::sreg32(f));
}

void Effect::applyState(WinApplication* app, x86::CPU& cpu, const State& state, uint32_t stage)
{
    const uint32_t op = state.op;
    const DWORD index = state.index == SamplerSelf ? stage : state.index;
    if (op < OpStageFirst)
    {
        m_device->SetRenderState(app, cpu, s_renderStates[op], stateValue(state));
    }
    else if (op < OpStageFirst + sizeof(s_stageStates))
    {
        m_device->SetTextureStageState(app, cpu, index, s_stageStates[op - OpStageFirst], stateValue(state));
    }
    else if (op == OpFvf)
    {
        m_device->SetFVF(app, cpu, stateValue(state));
    }
    else if (op == OpVertexShader || op == OpPixelShader)
    {
        if (state.kind == StateShader)
            applyShader(app, cpu, m_shaders[size_t(state.shader)]);
    }
    else if (op == OpTexture)
    {
        x86::reg32 texture = 0;
        if (state.kind == StateParameter && state.param >= 0 && isTexture(slotType(state.param, 0)))
            memcpy(&texture, slot(state.param, 0), 4);
        m_device->SetTexture(app, cpu, index, texture ? lookup(texture) : nullptr);
    }
    else if (op >= OpSamplerFirst && op <= OpSamplerLast)
    {
        m_device->SetSamplerState(app, cpu, index, op - OpTexture, stateValue(state));
    }
    else
    {
        D3D_LOG_ONCE("d3dx9: effect %s: pass state operation %u is not applied", m_resourceName.c_str(), op);
    }
}

// The shader's constant registers from their parameters and its samplers' states on their registers.
void Effect::applyShader(WinApplication* app, x86::CPU& cpu, ShaderBinding& shader)
{
    for (ShaderConstant& c : shader.constants)
        uploadConstant(c, shader.pixel);
    for (const auto& sampler : shader.samplers)
    {
        for (const State& state : m_params[size_t(sampler.first)].samplerStates)
            applyState(app, cpu, state, sampler.second);
    }
}

// A parameter as constant registers: one register per row (MATRIX_ROWS) or column (MATRIX_COLUMNS) of each element,
// or per element of a scalar or vector; at most the table's register count; unused components 0. Skipped while the
// parameter is unchanged since the last upload and the registers still hold it.
void Effect::setLayout(ShaderConstant& c) const
{
    const Param& q = m_params[size_t(c.param)];
    c.values = q.block->bytes.data() + q.offset;
    if (q.cls > ClassMatrixColumns)
    {
        c.layout = LayoutNone;
        return;
    }
    // The registers hold the storage in order when the table's layout equals the parameter's: matrices of the same
    // class and shape with 4-component registers, or 4-vectors.
    const bool direct = c.cls == q.cls && c.rows == q.rows && c.columns == q.columns
                     && (q.cls == ClassMatrixColumns ? q.rows == 4
                         : q.cls == ClassMatrixRows ? q.columns == 4
                         : q.rows == 1 && q.columns == 4);
    const bool transpose = c.cls == ClassMatrixColumns && q.cls == ClassMatrixRows && c.rows == 4 && c.columns == 4
                        && q.rows == 4 && q.columns == 4;
    c.layout = LayoutValues;
    if (q.floats && direct)
    {
        c.layout = LayoutCopy;
        c.words = slots(c.param);
    }
    else if (q.floats && transpose)
    {
        c.layout = LayoutTranspose;
        c.words = std::max<uint32_t>(q.elements, 1);
    }
}

void Effect::uploadConstant(ShaderConstant& c, bool pixel)
{
    if (!c.registerCount || c.layout == LayoutNone)
        return;
    const uint32_t count = std::min<uint32_t>(c.registerCount, 256);
    if (c.stamp && c.version == c.block->version && m_device->constantsHeld(pixel, c.registerIndex, count, c.stamp))
        return;
    c.version = c.block->version;
    float registers[4 * 256];
    const float* values = c.imageVersion && c.imageVersion == c.version ? c.image.data()
                                                                       : constantRegisters(c, registers);
    c.stamp = m_device->setConstants(pixel, c.registerIndex, values, count);
#if NFSC_RENDER_CHECK
    // The reference: the registers from the parameter's storage.
    float reference[4 * 256];
    m_device->referenceConstants(pixel, c.registerIndex, constantRegisters(c, reference), count);
#endif
}

const float* Effect::constantRegisters(const ShaderConstant& c, float* registers) const
{
    const uint32_t count = std::min<uint32_t>(c.registerCount, 256);
    if (c.layout == LayoutCopy)
    {
        // Float values are their registers' bits: whole registers straight from the storage.
        const uint32_t n = std::min(4 * count, c.words);
        if (n == 4 * count)
            return reinterpret_cast<const float*>(c.values);
        clearRegisters(registers + (n & ~3u), count - n / 4);
        memcpy(registers, c.values, 4 * n);
        return registers;
    }
    if (c.layout == LayoutTranspose)
    {
        // 4x4 float matrices stored by rows, read by columns: each element's words transposed.
        const uint32_t filled = std::min(count, 4 * c.words);
        clearRegisters(registers + 4 * filled, count - filled);
        uint32_t reg = 0;
        for (; reg + 4 <= filled; reg += 4)
            transposeWords(registers + 4 * reg, c.values + 16 * reg);
        for (; reg < filled; ++reg)
        {
            const uint32_t first = (reg & ~3u) * 4 + (reg & 3);
            for (uint32_t row = 0; row < 4; ++row)
                memcpy(registers + 4 * reg + row, c.values + 4 * (first + 4 * row), 4);
        }
        return registers;
    }
    const Param& q = m_params[size_t(c.param)];
    const uint32_t elements = std::max<uint32_t>(q.elements, 1);
    const uint8_t* const values = c.values;
    const uint8_t* const types = q.block->slotTypes.data() + (q.offset >> 2);
    const bool direct = c.cls == q.cls && c.rows == q.rows && c.columns == q.columns
                     && (q.cls == ClassMatrixColumns ? q.rows == 4
                         : q.cls == ClassMatrixRows ? q.columns == 4
                         : q.rows == 1 && q.columns == 4);
    if (direct)
    {
        const uint32_t n = std::min(4 * count, slots(c.param));
        clearRegisters(registers + (n & ~3u), count - n / 4);
        for (uint32_t k = 0; k < n; ++k)
            registers[k] = toFloat(values + 4 * k, types[k]);
        return registers;
    }
    clearRegisters(registers, count);
    if (c.cls == ClassMatrixColumns && q.cls == ClassMatrixRows && c.rows == 4 && c.columns == 4 && q.rows == 4
        && q.columns == 4)
    {
        // 4x4 matrices stored by rows, read by columns: each element transposed.
        for (uint32_t reg = 0; reg < count && reg < 4 * elements; ++reg)
        {
            const uint32_t first = (reg & ~3u) * 4 + (reg & 3);
            for (uint32_t row = 0; row < 4; ++row)
                registers[4 * reg + row] = toFloat(values + 4 * (first + 4 * row), types[first + 4 * row]);
        }
        return registers;
    }
    const uint32_t elementSlots = q.rows * q.columns;
    // Element e's value at row r, column c of the parameter's storage.
    auto value = [this, &q, &c, elementSlots](uint32_t e, uint32_t row, uint32_t column) {
        if (row >= q.rows || column >= q.columns)
            return 0.0f;
        const uint32_t i = e * elementSlots + (q.cls == ClassMatrixColumns ? column * q.rows + row
                                                                            : row * q.columns + column);
        return i < slots(c.param) ? toFloat(slot(c.param, i), slotType(c.param, i)) : 0.0f;
    };
    uint32_t reg = 0;
    for (uint32_t e = 0; e < elements && reg < count; ++e)
    {
        if (c.cls == ClassMatrixColumns)
        {
            for (uint32_t column = 0; column < c.columns && reg < count; ++column, ++reg)
                for (uint32_t row = 0; row < c.rows && row < 4; ++row)
                    registers[4 * reg + row] = value(e, row, column);
        }
        else if (c.cls == ClassMatrixRows)
        {
            for (uint32_t row = 0; row < c.rows && reg < count; ++row, ++reg)
                for (uint32_t column = 0; column < c.columns && column < 4; ++column)
                    registers[4 * reg + column] = value(e, row, column);
        }
        else
        {
            for (uint32_t column = 0; column < c.columns && column < 4; ++column)
                registers[4 * reg + column] = value(e, 0, column);
            ++reg;
        }
    }
    return registers;
}

void Effect::collectSaved(const State& state, uint32_t stage, DWORD flags, std::vector<Saved>& out) const
{
    const uint32_t op = state.op;
    const DWORD index = state.index == SamplerSelf ? stage : state.index;
    if (op < OpStageFirst)
        out.push_back(Saved{ 0, s_renderStates[op], 0, 0, 0 });
    else if (op < OpStageFirst + sizeof(s_stageStates))
        out.push_back(Saved{ 1, index, s_stageStates[op - OpStageFirst], 0, 0 });
    else if (op >= OpSamplerFirst && op <= OpSamplerLast && !(flags & 4))
        out.push_back(Saved{ 2, index, op - OpTexture, 0, 0 });
    else if (op == OpTexture)
        out.push_back(Saved{ 3, index, 0, 0, 0 });
    else if (op == OpFvf)
        out.push_back(Saved{ 4, 0, 0, 0, 0 });
    else if ((op == OpVertexShader || op == OpPixelShader) && state.kind == StateShader)
    {
        for (const auto& sampler : m_shaders[size_t(state.shader)].samplers)
            for (const State& s : m_params[size_t(sampler.first)].samplerStates)
                collectSaved(s, sampler.second, flags, out);
    }
}

// Begin without D3DXFX_DONOTSAVESTATE: the device states every pass of the technique sets (sampler states not with
// D3DXFX_DONOTSAVESAMPLERSTATE).
void Effect::captureState(WinApplication* app, x86::CPU& cpu, DWORD flags)
{
    // A Begin without End: the earlier capture is dropped.
    for (const Saved& s : m_saved)
    {
        if (Object* object = s.object ? lookup(s.object) : nullptr)
            object->release();
    }
    // The states to save depend only on the technique and the sampler flag: collected once per change of either.
    if (m_savedTechnique != m_technique || m_savedFlags != (flags & 4))
    {
        std::vector<Saved>& list = m_savedTemplate;
        list.clear();
        for (int p : m_techniques[size_t(m_technique)].passes)
            for (const State& state : m_passes[size_t(p)].states)
                collectSaved(state, 0, flags, list);
        auto key = [](const Saved& s) { return uint64_t(s.kind) << 48 | uint64_t(s.index) << 16 | s.type; };
        std::sort(list.begin(), list.end(), [&key](const Saved& a, const Saved& b) { return key(a) < key(b); });
        list.erase(std::unique(list.begin(), list.end(),
                               [&key](const Saved& a, const Saved& b) { return key(a) == key(b); }), list.end());
        m_savedTechnique = m_technique;
        m_savedFlags = flags & 4;
    }
    m_saved = m_savedTemplate;
    for (Saved& s : m_saved)
    {
        switch (s.kind)
        {
        case 0: m_device->GetRenderState(app, cpu, s.index, &s.value); break;
        case 1: m_device->GetTextureStageState(app, cpu, s.index, s.type, &s.value); break;
        case 2: m_device->GetSamplerState(app, cpu, s.index, s.type, &s.value); break;
        case 3: m_device->GetTexture(app, cpu, s.index, &s.object); break;
        default:
            m_device->GetFVF(app, cpu, &s.value);
            m_device->GetVertexDeclaration(app, cpu, &s.object);
            break;
        }
    }
}

void Effect::restoreState(WinApplication* app, x86::CPU& cpu)
{
    for (const Saved& s : m_saved)
    {
        Object* object = s.object ? lookup(s.object) : nullptr;
        switch (s.kind)
        {
        case 0: m_device->SetRenderState(app, cpu, s.index, s.value); break;
        case 1: m_device->SetTextureStageState(app, cpu, s.index, s.type, s.value); break;
        case 2: m_device->SetSamplerState(app, cpu, s.index, s.type, s.value); break;
        case 3: m_device->SetTexture(app, cpu, s.index, object); break;
        default:
            if (object && object->kind() == KindVertexDeclaration)
                m_device->SetVertexDeclaration(app, cpu, static_cast<VertexDeclaration*>(object));
            else
                m_device->SetFVF(app, cpu, s.value);
            break;
        }
        if (object)
            object->release();
    }
    m_saved.clear();
}

// ---- Native renderer tables ----------------------------------------------------------------------------------------

native::Pass* Effect::nativePass(x86::reg32 handle, uint32_t index)
{
    const int t = technique(handle);
    if (t < 0 || index >= m_techniques[size_t(t)].passes.size())
        return nullptr;
    const int p = m_techniques[size_t(t)].passes[index];
    if (m_native.empty())
    {
        m_native.resize(m_passes.size());
        m_nativeBuilt.assign(m_passes.size(), false);
    }
    if (!m_nativeBuilt[size_t(p)])
    {
        m_nativeBuilt[size_t(p)] = true;
        std::unique_ptr<native::Pass> table(new native::Pass());
        buildNative(p, *table);
        if (table->programs.kind != NoEffect)
            m_native[size_t(p)] = std::move(table);
    }
    return m_native[size_t(p)].get();
}

// A state as BeginPass applies it (stage: the sampler register of a sampler's block); not added when it applies
// nothing the native renderer mirrors.
void Effect::nativeState(const State& state, uint32_t stage, native::Pass& table) const
{
    const uint32_t op = state.op;
    native::PassState s = {};
    s.index = uint16_t(state.index == SamplerSelf ? stage : state.index);
    s.valueType = state.valueType;
    s.value = state.value;
    s.source = state.kind == StateLiteral || state.param < 0 ? native::SourceLiteral
             : state.kind == StateParameter ? native::SourceRaw : native::SourceExpression;
    if (state.param >= 0)
    {
        s.slot = slot(state.param, 0);
        s.slotType = slotType(state.param, 0);
    }
    if (op < OpStageFirst)
    {
        s.target = native::TargetRender;
        s.type = s_renderStates[op];
    }
    else if (op < OpStageFirst + sizeof(s_stageStates))
    {
        s.target = native::TargetStage;
        s.type = s_stageStates[op - OpStageFirst];
    }
    else if (op == OpTexture)
    {
        s.target = native::TargetTexture;
        if (!(state.kind == StateParameter && state.param >= 0 && isTexture(s.slotType)))
        {
            s.source = native::SourceLiteral;
            s.value = 0;
        }
    }
    else if (op >= OpSamplerFirst && op <= OpSamplerLast)
    {
        s.target = native::TargetSampler;
        s.type = uint16_t(op - OpTexture);
    }
    else
    {
        return;
    }
    table.states.push_back(s);
}

// The pass's programs, its states and constants in BeginPass and CommitChanges order, and the states its technique
// saves; programs.kind stays NoEffect for a pass without programs or a technique that saves the vertex input.
void Effect::buildNative(int p, native::Pass& table)
{
    const Pass& pass = m_passes[size_t(p)];
    table.programs = EffectDraw{ 0, 0, NoEffect, 0 };
    const Technique& owner = m_techniques[pass.technique];
    table.passes = uint32_t(owner.passes.size());
    table.world = m_resourceName.compare(0, 9, "IDI_WORLD") == 0 || m_resourceName == "IDI_SKY_FX";
    memset(table.savedRender, 0, sizeof(table.savedRender));
    memset(table.savedStage, 0, sizeof(table.savedStage));
    memset(table.savedSampler, 0, sizeof(table.savedSampler));
    memset(table.savedTexture, 0, sizeof(table.savedTexture));
    std::vector<Saved> saved;
    for (int q : owner.passes)
        for (const State& state : m_passes[size_t(q)].states)
            collectSaved(state, 0, 0, saved);
    for (const Saved& s : saved)
    {
        switch (s.kind)
        {
        case 0:
            if (s.index < MaxRenderStates)
                table.savedRender[s.index] = 1;
            break;
        case 1:
            if (s.index < MaxStages && s.type < D3DTSS_COUNT)
                table.savedStage[s.index][s.type] = 1;
            break;
        case 2:
            if (s.index < MaxSamplerStages && s.type < MaxSamplerStates)
                table.savedSampler[s.index][s.type] = 1;
            break;
        case 3:
            if (s.index < MaxSamplerStages)
                table.savedTexture[s.index] = 1;
            break;
        default:
            return;
        }
    }
    if (!pass.drawable || pass.programs.kind == NoEffect)
        return;
    auto addState = [this, &table](const State& state, uint32_t stage, std::vector<native::PassOp>& ops,
                                   const uint32_t* version) {
        const size_t before = table.states.size();
        nativeState(state, stage, table);
        if (table.states.size() != before)
            ops.push_back(native::PassOp{ native::OpState, uint16_t(before), version });
    };
    for (const State& state : pass.states)
    {
        if (state.op != OpVertexShader && state.op != OpPixelShader)
        {
            addState(state, 0, table.apply, nullptr);
            continue;
        }
        if (state.kind != StateShader)
            continue;
        const ShaderBinding& shader = m_shaders[size_t(state.shader)];
        for (const ShaderConstant& c : shader.constants)
        {
            if (!c.registerCount || c.layout == LayoutNone)
                continue;
            native::PassConstant constant;
            constant.pixel = shader.pixel;
            constant.first = c.registerIndex;
            constant.count = uint16_t(std::min<uint32_t>(c.registerCount, 256));
            constant.version = &c.block->version;
            constant.effect = this;
            constant.constant = &c;
            table.apply.push_back(native::PassOp{ native::OpConstant, uint16_t(table.constants.size()), nullptr });
            table.constants.push_back(constant);
        }
        for (const auto& sampler : shader.samplers)
            for (const State& s : m_params[size_t(sampler.first)].samplerStates)
                addState(s, sampler.second, table.apply, nullptr);
    }
    for (const Commit& c : pass.commits)
    {
        if (!c.constant)
        {
            addState(*c.state, c.stage, table.commits, &c.block->version);
            continue;
        }
        for (size_t k = 0; k < table.constants.size(); ++k)
        {
            if (table.constants[k].constant == c.constant)
                table.commits.push_back(native::PassOp{ native::OpConstant, uint16_t(k), &c.block->version });
        }
    }
    table.programs = pass.programs;
}

// ---- Lookup --------------------------------------------------------------------------------------------------------

int Effect::param(x86::reg32 handle) const
{
    if (handle >= HandleParam && handle < HandleParam + HandleLimit)
    {
        const uint32_t i = handle - HandleParam;
        return i < m_params.size() ? int(i) : -1;
    }
    if (handle >= 0x10000)
    {
        const char* name = reinterpret_cast<const char*>(g_guestBase + handle);
        for (int p : m_top)
        {
            if (m_params[size_t(p)].name == name)
                return p;
        }
    }
    return -1;
}

int Effect::technique(x86::reg32 handle) const
{
    if (handle >= HandleTechnique && handle < HandleTechnique + HandleLimit)
    {
        const uint32_t i = handle - HandleTechnique;
        return i < m_techniques.size() ? int(i) : -1;
    }
    if (handle >= 0x10000)
    {
        const char* name = reinterpret_cast<const char*>(g_guestBase + handle);
        for (size_t t = 0; t < m_techniques.size(); ++t)
        {
            if (m_techniques[t].name == name)
                return int(t);
        }
    }
    return -1;
}

int Effect::pass(x86::reg32 handle) const
{
    if (handle >= HandlePass && handle < HandlePass + HandleLimit)
    {
        const uint32_t i = handle - HandlePass;
        return i < m_passes.size() ? int(i) : -1;
    }
    return -1;
}

// Every pass's shaders within the device's shader versions.
bool Effect::techniqueValid(int t) const
{
    D3DCAPS9 caps;
    Direct3D::fillCaps(&caps);
    static const bool s_motionBlur = pf::Config::getBool("motion_blur", false);
    static const bool s_roadReflection = pf::Config::getBool("road_reflection", false);
    // road_reflection=0: the reflection chain's last pass is invalid, so the game limits Road Reflection to off.
    if (!s_roadReflection && m_techniques[size_t(t)].name == "CombineReflectionColourAndHeadlightAlpha")
        return false;
    for (int p : m_techniques[size_t(t)].passes)
    {
        const Pass& pass = m_passes[size_t(p)];
        const bool above =
            (pass.vertexShader && (rc::ld32(pass.vertexShader) & 0xffff) > (caps.field[CapsVertexShaderVersion] & 0xffff))
            || (pass.pixelShader && (rc::ld32(pass.pixelShader) & 0xffff) > (caps.field[CapsPixelShaderVersion] & 0xffff));
        // Above the caps: only the motion blur translations, with the setting on.
        const bool translated = pass.vertexBinding >= 0 && pass.pixelBinding >= 0
                             && m_shaders[size_t(pass.vertexBinding)].motionBlur
                             && m_shaders[size_t(pass.pixelBinding)].motionBlur;
        if (above && !(s_motionBlur && translated))
            return false;
    }
    return true;
}

// ---- Values --------------------------------------------------------------------------------------------------------

HRESULT Effect::setFloats(int p, const float* values, uint32_t count)
{
    if (p < 0 || !numeric(p))
        return D3DERR_INVALIDCALL;
    const Param& q = m_params[size_t(p)];
    const uint32_t n = std::min(count, slots(p));
    uint8_t* const storage = q.block->bytes.data() + q.offset;
    const uint8_t* const types = q.block->slotTypes.data() + (q.offset >> 2);
    for (uint32_t i = 0; i < n; ++i)
        fromFloat(storage + 4 * i, types[i], values[i]);
    touch(p);
    return D3D_OK;
}

HRESULT Effect::getFloats(int p, float* values, uint32_t count)
{
    if (p < 0 || !numeric(p))
        return D3DERR_INVALIDCALL;
    const uint32_t n = std::min(count, slots(p));
    for (uint32_t i = 0; i < n; ++i)
        values[i] = toFloat(slot(p, i), slotType(p, i));
    return D3D_OK;
}

void Effect::setTextureSlot(int p, uint32_t i, x86::reg32 texture)
{
    x86::reg32 old;
    memcpy(&old, slot(p, i), 4);
    if (old == texture)
        return;
    if (Object* object = lookup(texture))
        object->addRef();
    if (Object* object = lookup(old))
        object->release();
    memcpy(slot(p, i), &texture, 4);
    touch(p);
}

// One matrix per element, its upper-left rows by columns (transposed for the transpose setters); MATRIX_COLUMNS
// parameters store the columns one after the other.
HRESULT Effect::setMatrices(x86::reg32 handle, const float* matrices, UINT count, bool transpose)
{
    const int p = param(handle);
    if (p < 0 || !numeric(p))
        return D3DERR_INVALIDCALL;
    const Param& info = m_params[size_t(p)];
    const int* targets = info.elements ? info.members.data() : &p;
    const size_t targetCount = info.elements ? info.members.size() : 1;
    bool rows4x4 = true;
    for (UINT k = 0; k < count && k < targetCount; ++k)
    {
        const Param& q = m_params[size_t(targets[k])];
        uint8_t* const values = q.block->bytes.data() + q.offset;
        const uint8_t* const types = q.block->slotTypes.data() + (q.offset >> 2);
        const uint32_t rows = q.rows, columns = q.columns;
        const bool byColumns = q.cls == ClassMatrixColumns;
        const float* m = matrices + 16 * k;
        if (q.floats && rows == 4 && columns == 4 && (byColumns || q.cls == ClassMatrixRows))
        {
            // A 4x4 float matrix: its 16 words as given, or transposed.
            if (byColumns == transpose)
                memcpy(values, m, 64);
            else
                transposeWords(values, m);
            rows4x4 = rows4x4 && !byColumns;
            continue;
        }
        rows4x4 = false;
        for (uint32_t row = 0; row < rows; ++row)
            for (uint32_t column = 0; column < columns; ++column)
            {
                const uint32_t i = byColumns ? column * rows + row : row * columns + column;
                fromFloat(values + 4 * i, types[i], transpose ? m[column * 4 + row] : m[row * 4 + column]);
            }
    }
    const uint32_t before = m_params[size_t(p)].block->version;
    touch(p);
    if (rows4x4 && m_pass >= 0 && m_fused)
        fuseMatrices(p, matrices, uint32_t(std::min<size_t>(count, targetCount)), transpose, before);
    return D3D_OK;
}

// The registers of the current pass's LayoutTranspose constants of parameter p after setMatrices stored count 4x4
// float row matrices (block version before: the one ahead of the write): element k's registers are its stored rows
// transposed, so the matrix as given transposed, or as given for the transpose setters. A constant whose image
// did not hold version before and that reads elements not written is filled from the storage.
void Effect::fuseMatrices(int p, const float* matrices, uint32_t count, bool transpose, uint32_t before)
{
    const uint32_t elements = std::max<uint32_t>(m_params[size_t(p)].elements, 1);
    for (const auto& f : m_passes[size_t(m_pass)].fused)
    {
        if (f.first != p)
            continue;
        ShaderConstant& c = *f.second;
        const uint32_t registers = uint32_t(c.image.size() / 4);
        float* const image = c.image.data();
        if (count < elements && 4 * count < registers && (!c.imageVersion || c.imageVersion != before))
        {
            constantRegisters(c, image);
            c.imageVersion = c.block->version;
            continue;
        }
        for (uint32_t k = 0; k < count && 4 * k < registers; ++k)
        {
            const float* m = matrices + 16 * k;
            const uint32_t n = std::min<uint32_t>(4, registers - 4 * k);
            if (n == 4)
            {
                if (transpose)
                    copyRegisters(image + 16 * k, m, 4);
                else
                    transposeWords(image + 16 * k, m);
                continue;
            }
            float t[16];
            if (transpose)
                copyRegisters(t, m, 4);
            else
                transposeWords(t, m);
            copyRegisters(image + 16 * k, t, n);
        }
        c.imageVersion = c.block->version;
    }
}

HRESULT Effect::getMatrices(x86::reg32 handle, float* matrices, UINT count, bool transpose)
{
    const int p = param(handle);
    if (p < 0 || !numeric(p))
        return D3DERR_INVALIDCALL;
    const Param& info = m_params[size_t(p)];
    const int* targets = info.elements ? info.members.data() : &p;
    const size_t targetCount = info.elements ? info.members.size() : 1;
    for (UINT k = 0; k < count && k < targetCount; ++k)
    {
        const Param& q = m_params[size_t(targets[k])];
        const uint8_t* const values = q.block->bytes.data() + q.offset;
        const uint8_t* const types = q.block->slotTypes.data() + (q.offset >> 2);
        const uint32_t rows = q.rows, columns = q.columns;
        const bool byColumns = q.cls == ClassMatrixColumns;
        float* m = matrices + 16 * k;
        for (int i = 0; i < 16; ++i)
            m[i] = 0.0f;
        for (uint32_t row = 0; row < rows; ++row)
            for (uint32_t column = 0; column < columns; ++column)
            {
                const uint32_t i = byColumns ? column * rows + row : row * columns + column;
                const float v = toFloat(values + 4 * i, types[i]);
                if (transpose)
                    m[column * 4 + row] = v;
                else
                    m[row * 4 + column] = v;
            }
    }
    return D3D_OK;
}

// ---- ID3DXBaseEffect -----------------------------------------------------------------------------------------------

// D3DXEFFECT_DESC: Creator, Parameters, Techniques, Functions.
HRESULT Effect::GetDesc(WinApplication* app, x86::CPU& cpu, DWORD* desc)
{
    RC_USE(app);
    RC_USE(cpu);
    if (!desc)
        return D3DERR_INVALIDCALL;
    desc[0] = 0;
    desc[1] = DWORD(m_top.size());
    desc[2] = DWORD(m_techniques.size());
    desc[3] = 0;
    return D3D_OK;
}

// D3DXPARAMETER_DESC: Name, Semantic, Class, Type, Rows, Columns, Elements, Annotations, StructMembers, Flags,
// Bytes.
HRESULT Effect::GetParameterDesc(WinApplication* app, x86::CPU& cpu, x86::reg32 handle, DWORD* desc)
{
    RC_USE(app);
    RC_USE(cpu);
    const int p = param(handle);
    if (p < 0 || !desc)
        return D3DERR_INVALIDCALL;
    const Param& q = m_params[size_t(p)];
    desc[0] = q.guestName;
    desc[1] = q.guestSemantic;
    desc[2] = q.cls;
    desc[3] = q.type;
    desc[4] = q.rows;
    desc[5] = q.columns;
    desc[6] = q.elements;
    desc[7] = DWORD(q.annotations.size());
    desc[8] = q.cls == ClassStruct && !q.elements ? DWORD(q.members.size()) : 0;
    desc[9] = q.flags;
    desc[10] = q.bytes;
    return D3D_OK;
}

// D3DXTECHNIQUE_DESC: Name, Passes, Annotations.
HRESULT Effect::GetTechniqueDesc(WinApplication* app, x86::CPU& cpu, x86::reg32 handle, DWORD* desc)
{
    RC_USE(app);
    RC_USE(cpu);
    const int t = technique(handle);
    if (t < 0 || !desc)
        return D3DERR_INVALIDCALL;
    desc[0] = m_techniques[size_t(t)].guestName;
    desc[1] = DWORD(m_techniques[size_t(t)].passes.size());
    desc[2] = DWORD(m_techniques[size_t(t)].annotations.size());
    return D3D_OK;
}

// D3DXPASS_DESC: Name, Annotations, the vertex and pixel shader bytecode.
HRESULT Effect::GetPassDesc(WinApplication* app, x86::CPU& cpu, x86::reg32 handle, DWORD* desc)
{
    RC_USE(app);
    RC_USE(cpu);
    const int p = pass(handle);
    if (p < 0 || !desc)
        return D3DERR_INVALIDCALL;
    desc[0] = m_passes[size_t(p)].guestName;
    desc[1] = DWORD(m_passes[size_t(p)].annotations.size());
    desc[2] = m_passes[size_t(p)].vertexShader;
    desc[3] = m_passes[size_t(p)].pixelShader;
    return D3D_OK;
}

x86::reg32 Effect::GetParameter(WinApplication* app, x86::CPU& cpu, x86::reg32 parent, UINT index)
{
    RC_USE(app);
    RC_USE(cpu);
    if (!parent)
        return index < m_top.size() ? HandleParam + x86::reg32(m_top[index]) : 0;
    const int p = param(parent);
    if (p < 0 || m_params[size_t(p)].elements || index >= m_params[size_t(p)].members.size())
        return 0;
    return HandleParam + x86::reg32(m_params[size_t(p)].members[index]);
}

x86::reg32 Effect::GetParameterByName(WinApplication* app, x86::CPU& cpu, x86::reg32 parent, x86::reg32 name)
{
    RC_USE(app);
    RC_USE(cpu);
    if (!name)
        return 0;
    const char* text = reinterpret_cast<const char*>(g_guestBase + name);
    const std::vector<int>* list = &m_top;
    if (parent)
    {
        const int p = param(parent);
        if (p < 0)
            return 0;
        list = &m_params[size_t(p)].members;
    }
    for (int p : *list)
    {
        if (m_params[size_t(p)].name == text)
            return HandleParam + x86::reg32(p);
    }
    return 0;
}

x86::reg32 Effect::GetParameterBySemantic(WinApplication* app, x86::CPU& cpu, x86::reg32 parent, x86::reg32 semantic)
{
    RC_USE(app);
    RC_USE(cpu);
    if (!semantic)
        return 0;
    const char* text = reinterpret_cast<const char*>(g_guestBase + semantic);
    const std::vector<int>* list = &m_top;
    if (parent)
    {
        const int p = param(parent);
        if (p < 0)
            return 0;
        list = &m_params[size_t(p)].members;
    }
    for (int p : *list)
    {
        if (strcasecmp(m_params[size_t(p)].semantic.c_str(), text) == 0)
            return HandleParam + x86::reg32(p);
    }
    return 0;
}

x86::reg32 Effect::GetParameterElement(WinApplication* app, x86::CPU& cpu, x86::reg32 parent, UINT index)
{
    RC_USE(app);
    RC_USE(cpu);
    const int p = param(parent);
    if (p < 0 || !m_params[size_t(p)].elements || index >= m_params[size_t(p)].members.size())
        return 0;
    return HandleParam + x86::reg32(m_params[size_t(p)].members[index]);
}

x86::reg32 Effect::GetTechnique(WinApplication* app, x86::CPU& cpu, UINT index)
{
    RC_USE(app);
    RC_USE(cpu);
    return index < m_techniques.size() ? HandleTechnique + index : 0;
}

x86::reg32 Effect::GetTechniqueByName(WinApplication* app, x86::CPU& cpu, x86::reg32 name)
{
    RC_USE(app);
    RC_USE(cpu);
    if (!name)
        return 0;
    const char* text = reinterpret_cast<const char*>(g_guestBase + name);
    for (size_t t = 0; t < m_techniques.size(); ++t)
    {
        if (m_techniques[t].name == text)
            return HandleTechnique + x86::reg32(t);
    }
    return 0;
}

x86::reg32 Effect::GetPass(WinApplication* app, x86::CPU& cpu, x86::reg32 handle, UINT index)
{
    RC_USE(app);
    RC_USE(cpu);
    const int t = technique(handle);
    if (t < 0 || index >= m_techniques[size_t(t)].passes.size())
        return 0;
    return HandlePass + x86::reg32(m_techniques[size_t(t)].passes[index]);
}

x86::reg32 Effect::GetPassByName(WinApplication* app, x86::CPU& cpu, x86::reg32 handle, x86::reg32 name)
{
    RC_USE(app);
    RC_USE(cpu);
    const int t = technique(handle);
    if (t < 0 || !name)
        return 0;
    const char* text = reinterpret_cast<const char*>(g_guestBase + name);
    for (int p : m_techniques[size_t(t)].passes)
    {
        if (m_passes[size_t(p)].name == text)
            return HandlePass + x86::reg32(p);
    }
    return 0;
}

x86::reg32 Effect::GetAnnotation(WinApplication* app, x86::CPU& cpu, x86::reg32 object, UINT index)
{
    RC_USE(app);
    RC_USE(cpu);
    const std::vector<int>* list = nullptr;
    if (const int t = technique(object); t >= 0)
        list = &m_techniques[size_t(t)].annotations;
    else if (const int p = pass(object); p >= 0)
        list = &m_passes[size_t(p)].annotations;
    else if (const int q = param(object); q >= 0)
        list = &m_params[size_t(q)].annotations;
    if (!list || index >= list->size())
        return 0;
    return HandleParam + x86::reg32((*list)[index]);
}

x86::reg32 Effect::GetAnnotationByName(WinApplication* app, x86::CPU& cpu, x86::reg32 object, x86::reg32 name)
{
    if (!name)
        return 0;
    const char* text = reinterpret_cast<const char*>(g_guestBase + name);
    for (UINT i = 0;; ++i)
    {
        const x86::reg32 a = GetAnnotation(app, cpu, object, i);
        if (!a)
            return 0;
        if (m_params[a - HandleParam].name == text)
            return a;
    }
}

// Raw bytes; texture slots take references.
HRESULT Effect::SetValue(WinApplication* app, x86::CPU& cpu, x86::reg32 handle, x86::reg32 data, UINT bytes)
{
    RC_USE(app);
    RC_USE(cpu);
    const int p = param(handle);
    if (p < 0 || !data)
        return D3DERR_INVALIDCALL;
    const uint32_t n = std::min<uint32_t>(bytes, m_params[size_t(p)].bytes) / 4;
    for (uint32_t i = 0; i < n; ++i)
    {
        if (isTexture(slotType(p, i)))
            setTextureSlot(p, i, rc::ld32(data + 4 * i));
        else
            memcpy(slot(p, i), g_guestBase + data + 4 * i, 4);
    }
    touch(p);
    return D3D_OK;
}

HRESULT Effect::GetValue(WinApplication* app, x86::CPU& cpu, x86::reg32 handle, x86::reg32 data, UINT bytes)
{
    RC_USE(app);
    RC_USE(cpu);
    const int p = param(handle);
    if (p < 0 || !data)
        return D3DERR_INVALIDCALL;
    const uint32_t n = std::min<uint32_t>(bytes, m_params[size_t(p)].bytes);
    memcpy(g_guestBase + data, slot(p, 0), n);
    for (uint32_t i = 0; i < n / 4; ++i)
    {
        if (isTexture(slotType(p, i)))
        {
            if (Object* object = lookup(rc::ld32(data + 4 * i)))
                object->addRef();
        }
    }
    return D3D_OK;
}

HRESULT Effect::SetBool(WinApplication* app, x86::CPU& cpu, x86::reg32 handle, BOOL value)
{
    return SetBoolArray(app, cpu, handle, &value, 1);
}

HRESULT Effect::GetBool(WinApplication* app, x86::CPU& cpu, x86::reg32 handle, BOOL* value)
{
    return GetBoolArray(app, cpu, handle, value, 1);
}

HRESULT Effect::SetBoolArray(WinApplication* app, x86::CPU& cpu, x86::reg32 handle, const BOOL* values, UINT count)
{
    RC_USE(app);
    RC_USE(cpu);
    const int p = param(handle);
    if (p < 0 || !numeric(p) || !values)
        return D3DERR_INVALIDCALL;
    for (uint32_t i = 0; i < std::min(count, slots(p)); ++i)
        fromInt(slot(p, i), slotType(p, i), values[i] ? 1 : 0);
    touch(p);
    return D3D_OK;
}

HRESULT Effect::GetBoolArray(WinApplication* app, x86::CPU& cpu, x86::reg32 handle, BOOL* values, UINT count)
{
    RC_USE(app);
    RC_USE(cpu);
    const int p = param(handle);
    if (p < 0 || !numeric(p) || !values)
        return D3DERR_INVALIDCALL;
    for (uint32_t i = 0; i < std::min(count, slots(p)); ++i)
        values[i] = toInt(slot(p, i), slotType(p, i)) != 0 ? 1 : 0;
    return D3D_OK;
}

HRESULT Effect::SetInt(WinApplication* app, x86::CPU& cpu, x86::reg32 handle, x86::sreg32 value)
{
    return SetIntArray(app, cpu, handle, &value, 1);
}

HRESULT Effect::GetInt(WinApplication* app, x86::CPU& cpu, x86::reg32 handle, x86::sreg32* value)
{
    return GetIntArray(app, cpu, handle, value, 1);
}

HRESULT Effect::SetIntArray(WinApplication* app, x86::CPU& cpu, x86::reg32 handle, const x86::sreg32* values,
                            UINT count)
{
    RC_USE(app);
    RC_USE(cpu);
    const int p = param(handle);
    if (p < 0 || !numeric(p) || !values)
        return D3DERR_INVALIDCALL;
    for (uint32_t i = 0; i < std::min(count, slots(p)); ++i)
        fromInt(slot(p, i), slotType(p, i), values[i]);
    touch(p);
    return D3D_OK;
}

HRESULT Effect::GetIntArray(WinApplication* app, x86::CPU& cpu, x86::reg32 handle, x86::sreg32* values, UINT count)
{
    RC_USE(app);
    RC_USE(cpu);
    const int p = param(handle);
    if (p < 0 || !numeric(p) || !values)
        return D3DERR_INVALIDCALL;
    for (uint32_t i = 0; i < std::min(count, slots(p)); ++i)
        values[i] = toInt(slot(p, i), slotType(p, i));
    return D3D_OK;
}

HRESULT Effect::SetFloat(WinApplication* app, x86::CPU& cpu, x86::reg32 handle, float value)
{
    RC_USE(app);
    RC_USE(cpu);
    D3D9_TRACE(direct::TraceEffectFloat, uint32_t(uintptr_t(this)), handle, 0, 0, 0, 0, &value, 1);
    return setFloats(param(handle), &value, 1);
}

HRESULT Effect::GetFloat(WinApplication* app, x86::CPU& cpu, x86::reg32 handle, float* value)
{
    RC_USE(app);
    RC_USE(cpu);
    return value ? getFloats(param(handle), value, 1) : D3DERR_INVALIDCALL;
}

HRESULT Effect::SetFloatArray(WinApplication* app, x86::CPU& cpu, x86::reg32 handle, const float* values, UINT count)
{
    RC_USE(app);
    RC_USE(cpu);
    return values ? setFloats(param(handle), values, count) : D3DERR_INVALIDCALL;
}

HRESULT Effect::GetFloatArray(WinApplication* app, x86::CPU& cpu, x86::reg32 handle, float* values, UINT count)
{
    RC_USE(app);
    RC_USE(cpu);
    return values ? getFloats(param(handle), values, count) : D3DERR_INVALIDCALL;
}

// A vector fills the parameter's columns (one element of an array).
HRESULT Effect::SetVector(WinApplication* app, x86::CPU& cpu, x86::reg32 handle, const float* vector)
{
    return SetVectorArray(app, cpu, handle, vector, 1);
}

HRESULT Effect::GetVector(WinApplication* app, x86::CPU& cpu, x86::reg32 handle, float* vector)
{
    return GetVectorArray(app, cpu, handle, vector, 1);
}

HRESULT Effect::SetVectorArray(WinApplication* app, x86::CPU& cpu, x86::reg32 handle, const float* vectors,
                               UINT count)
{
    RC_USE(app);
    RC_USE(cpu);
    D3D9_TRACE(direct::TraceEffectVectors, uint32_t(uintptr_t(this)), handle, count, 0, 0, 0, vectors,
               vectors ? 4 * count : 0);
    const int p = param(handle);
    if (p < 0 || !numeric(p) || !vectors)
        return D3DERR_INVALIDCALL;
    const Param& q = m_params[size_t(p)];
    const uint32_t columns = std::max<uint32_t>(q.columns, 1);
    const uint32_t n = slots(p);
    uint8_t* const values = q.block->bytes.data() + q.offset;
    const uint8_t* const types = q.block->slotTypes.data() + (q.offset >> 2);
    if (q.floats && columns == 4)
        memcpy(values, vectors, 4 * std::min<size_t>(size_t(count) * 4, n));
    else
    {
        for (UINT k = 0; k < count; ++k)
            for (uint32_t c = 0; c < columns && c < 4 && k * columns + c < n; ++c)
                fromFloat(values + 4 * (k * columns + c), types[k * columns + c], vectors[4 * k + c]);
    }
    touch(p);
    return D3D_OK;
}

HRESULT Effect::GetVectorArray(WinApplication* app, x86::CPU& cpu, x86::reg32 handle, float* vectors, UINT count)
{
    RC_USE(app);
    RC_USE(cpu);
    const int p = param(handle);
    if (p < 0 || !numeric(p) || !vectors)
        return D3DERR_INVALIDCALL;
    const uint32_t columns = std::max<uint32_t>(m_params[size_t(p)].columns, 1);
    for (UINT k = 0; k < count; ++k)
        for (uint32_t c = 0; c < 4; ++c)
            vectors[4 * k + c] = c < columns && k * columns + c < slots(p)
                               ? toFloat(slot(p, k * columns + c), slotType(p, k * columns + c)) : 0.0f;
    return D3D_OK;
}

HRESULT Effect::SetMatrix(WinApplication* app, x86::CPU& cpu, x86::reg32 handle, const float* matrix)
{
    RC_USE(app);
    RC_USE(cpu);
    D3D9_TRACE(direct::TraceEffectMatrix, uint32_t(uintptr_t(this)), handle, 0, 0, 0, 0, matrix, matrix ? 16 : 0);
    return matrix ? setMatrices(handle, matrix, 1, false) : D3DERR_INVALIDCALL;
}

HRESULT Effect::GetMatrix(WinApplication* app, x86::CPU& cpu, x86::reg32 handle, float* matrix)
{
    RC_USE(app);
    RC_USE(cpu);
    return matrix ? getMatrices(handle, matrix, 1, false) : D3DERR_INVALIDCALL;
}

HRESULT Effect::SetMatrixArray(WinApplication* app, x86::CPU& cpu, x86::reg32 handle, const float* matrices,
                               UINT count)
{
    RC_USE(app);
    RC_USE(cpu);
    D3D9_TRACE(direct::TraceEffectMatrices, uint32_t(uintptr_t(this)), handle, count, 0, 0, 0, matrices,
               matrices ? 16 * count : 0);
    return matrices ? setMatrices(handle, matrices, count, false) : D3DERR_INVALIDCALL;
}

HRESULT Effect::GetMatrixArray(WinApplication* app, x86::CPU& cpu, x86::reg32 handle, float* matrices, UINT count)
{
    RC_USE(app);
    RC_USE(cpu);
    return matrices ? getMatrices(handle, matrices, count, false) : D3DERR_INVALIDCALL;
}

HRESULT Effect::SetMatrixPointerArray(WinApplication* app, x86::CPU& cpu, x86::reg32 handle,
                                      const x86::reg32* matrices, UINT count)
{
    RC_USE(app);
    RC_USE(cpu);
    if (!matrices)
        return D3DERR_INVALIDCALL;
    std::vector<float> all(size_t(count) * 16);
    for (UINT k = 0; k < count; ++k)
        memcpy(all.data() + 16 * k, g_guestBase + matrices[k], 64);
    return setMatrices(handle, all.data(), count, false);
}

HRESULT Effect::SetMatrixTranspose(WinApplication* app, x86::CPU& cpu, x86::reg32 handle, const float* matrix)
{
    RC_USE(app);
    RC_USE(cpu);
    return matrix ? setMatrices(handle, matrix, 1, true) : D3DERR_INVALIDCALL;
}

HRESULT Effect::GetMatrixTranspose(WinApplication* app, x86::CPU& cpu, x86::reg32 handle, float* matrix)
{
    RC_USE(app);
    RC_USE(cpu);
    return matrix ? getMatrices(handle, matrix, 1, true) : D3DERR_INVALIDCALL;
}

HRESULT Effect::SetMatrixTransposeArray(WinApplication* app, x86::CPU& cpu, x86::reg32 handle, const float* matrices,
                                        UINT count)
{
    RC_USE(app);
    RC_USE(cpu);
    return matrices ? setMatrices(handle, matrices, count, true) : D3DERR_INVALIDCALL;
}

HRESULT Effect::GetMatrixTransposeArray(WinApplication* app, x86::CPU& cpu, x86::reg32 handle, float* matrices,
                                        UINT count)
{
    RC_USE(app);
    RC_USE(cpu);
    return matrices ? getMatrices(handle, matrices, count, true) : D3DERR_INVALIDCALL;
}

HRESULT Effect::SetMatrixTransposePointerArray(WinApplication* app, x86::CPU& cpu, x86::reg32 handle,
                                               const x86::reg32* matrices, UINT count)
{
    RC_USE(app);
    RC_USE(cpu);
    if (!matrices)
        return D3DERR_INVALIDCALL;
    std::vector<float> all(size_t(count) * 16);
    for (UINT k = 0; k < count; ++k)
        memcpy(all.data() + 16 * k, g_guestBase + matrices[k], 64);
    return setMatrices(handle, all.data(), count, true);
}

HRESULT Effect::SetString(WinApplication* app, x86::CPU& cpu, x86::reg32 handle, x86::reg32 text)
{
    RC_USE(app);
    RC_USE(cpu);
    const int p = param(handle);
    if (p < 0 || slotType(p, 0) != TypeString || !text)
        return D3DERR_INVALIDCALL;
    const x86::reg32 copy = guestString(reinterpret_cast<const char*>(g_guestBase + text));
    memcpy(slot(p, 0), &copy, 4);
    touch(p);
    return D3D_OK;
}

HRESULT Effect::GetString(WinApplication* app, x86::CPU& cpu, x86::reg32 handle, x86::reg32* text)
{
    RC_USE(app);
    RC_USE(cpu);
    const int p = param(handle);
    if (p < 0 || slotType(p, 0) != TypeString || !text)
        return D3DERR_INVALIDCALL;
    memcpy(text, slot(p, 0), 4);
    return D3D_OK;
}

HRESULT Effect::SetTexture(WinApplication* app, x86::CPU& cpu, x86::reg32 handle, x86::reg32 texture)
{
    RC_USE(app);
    RC_USE(cpu);
    D3D9_TRACE(direct::TraceEffectTexture, uint32_t(uintptr_t(this)), handle, texture);
    const int p = param(handle);
    if (p < 0 || !isTexture(slotType(p, 0)))
        return D3DERR_INVALIDCALL;
    setTextureSlot(p, 0, texture);
    return D3D_OK;
}

HRESULT Effect::GetTexture(WinApplication* app, x86::CPU& cpu, x86::reg32 handle, x86::reg32* texture)
{
    RC_USE(app);
    RC_USE(cpu);
    const int p = param(handle);
    if (p < 0 || !isTexture(slotType(p, 0)) || !texture)
        return D3DERR_INVALIDCALL;
    memcpy(texture, slot(p, 0), 4);
    if (Object* object = lookup(*texture))
        object->addRef();
    return D3D_OK;
}

// A hint for the upload of array parameters: values are unchanged.
HRESULT Effect::SetArrayRange(WinApplication* app, x86::CPU& cpu, x86::reg32 handle, UINT start, UINT end)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(start);
    RC_USE(end);
    return param(handle) >= 0 ? D3D_OK : D3DERR_INVALIDCALL;
}

// ---- ID3DXEffect ---------------------------------------------------------------------------------------------------

HRESULT Effect::GetPool(WinApplication* app, x86::CPU& cpu, x86::reg32* pool)
{
    RC_USE(app);
    RC_USE(cpu);
    if (!pool)
        return D3DERR_INVALIDCALL;
    if (m_pool)
        m_pool->addRef();
    *pool = m_pool ? m_pool->guest() : 0;
    return D3D_OK;
}

HRESULT Effect::SetTechnique(WinApplication* app, x86::CPU& cpu, x86::reg32 handle)
{
    RC_USE(app);
    RC_USE(cpu);
    D3D9_TRACE(direct::TraceEffectTechnique, uint32_t(uintptr_t(this)), handle);
    const int t = technique(handle);
    if (t < 0)
        return D3DERR_INVALIDCALL;
    m_technique = t;
    return D3D_OK;
}

x86::reg32 Effect::GetCurrentTechnique(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    RC_USE(cpu);
    return m_techniques.empty() ? 0 : HandleTechnique + x86::reg32(m_technique);
}

HRESULT Effect::ValidateTechnique(WinApplication* app, x86::CPU& cpu, x86::reg32 handle)
{
    RC_USE(app);
    RC_USE(cpu);
    const int t = technique(handle);
    if (t < 0)
        return D3DERR_INVALIDCALL;
    return techniqueValid(t) ? D3D_OK : E_FAIL;
}

HRESULT Effect::FindNextValidTechnique(WinApplication* app, x86::CPU& cpu, x86::reg32 handle, x86::reg32* next)
{
    RC_USE(app);
    RC_USE(cpu);
    if (!next)
        return D3DERR_INVALIDCALL;
    const int start = handle ? technique(handle) + 1 : 0;
    for (int t = start; t >= 0 && size_t(t) < m_techniques.size(); ++t)
    {
        if (techniqueValid(t))
        {
            *next = HandleTechnique + x86::reg32(t);
            return D3D_OK;
        }
    }
    *next = 0;
    return S_FALSE_;
}

// Parameters are taken as used by every technique.
BOOL Effect::IsParameterUsed(WinApplication* app, x86::CPU& cpu, x86::reg32 handle, x86::reg32 handleTechnique)
{
    RC_USE(app);
    RC_USE(cpu);
    return param(handle) >= 0 && technique(handleTechnique) >= 0 ? 1 : 0;
}

// D3DXFX_DONOTSAVESTATE (1): nothing is saved; D3DXFX_DONOTSAVESAMPLERSTATE (4): sampler states are not.
HRESULT Effect::Begin(WinApplication* app, x86::CPU& cpu, UINT* passes, DWORD flags)
{
    D3D9_TRACE(direct::TraceEffectBegin, uint32_t(uintptr_t(this)), flags);
    D3D9_TRACE_SUSPEND();
    if (m_techniques.empty())
        return D3DERR_INVALIDCALL;
    if (!(flags & 1))
        captureState(app, cpu, flags);
    if (passes)
        *passes = UINT(m_techniques[size_t(m_technique)].passes.size());
    m_begun = true;
    return D3D_OK;
}

// Every state of the pass, in order (a shader's constants and samplers with its state).
HRESULT Effect::BeginPass(WinApplication* app, x86::CPU& cpu, UINT index)
{
    D3D9_TRACE(direct::TraceEffectBeginPass, uint32_t(uintptr_t(this)), index);
    D3D9_TRACE_SUSPEND();
    if (!m_begun || index >= m_techniques[size_t(m_technique)].passes.size())
        return D3DERR_INVALIDCALL;
    m_pass = m_techniques[size_t(m_technique)].passes[index];
    for (const State& state : m_passes[size_t(m_pass)].states)
        applyState(app, cpu, state, 0);
    m_applied = s_changes;
    m_proxy.pass = uint32_t(m_pass);
    m_proxy.drawable = m_passes[size_t(m_pass)].drawable;
    m_proxy.programs = m_passes[size_t(m_pass)].programs;
    m_device->setEffectPass(&m_proxy);
    return D3D_OK;
}

// The states, constants and samplers of parameters set since the pass's states were applied.
HRESULT Effect::CommitChanges(WinApplication* app, x86::CPU& cpu)
{
    D3D9_TRACE(direct::TraceEffectCommit, uint32_t(uintptr_t(this)));
    D3D9_TRACE_SUSPEND();
    // No parameter of any effect set since: nothing to apply.
    if (m_pass < 0 || m_applied == s_changes)
        return D3D_OK;
    const uint32_t since = m_applied;
    for (const Commit& c : m_passes[size_t(m_pass)].commits)
    {
        if (c.block->version <= since)
            continue;
        if (c.constant)
            uploadConstant(*c.constant, c.pixel);
        else
            applyState(app, cpu, *c.state, c.stage);
    }
    m_applied = s_changes;
    return D3D_OK;
}

HRESULT Effect::EndPass(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    RC_USE(cpu);
    D3D9_TRACE(direct::TraceEffectEndPass, uint32_t(uintptr_t(this)));
    D3D9_TRACE_SUSPEND();
    if (m_device->effectPass() == &m_proxy)
        m_device->setEffectPass(nullptr);
    m_pass = -1;
    return D3D_OK;
}

HRESULT Effect::End(WinApplication* app, x86::CPU& cpu)
{
    D3D9_TRACE(direct::TraceEffectEnd, uint32_t(uintptr_t(this)));
    D3D9_TRACE_SUSPEND();
    if (m_device->effectPass() == &m_proxy)
        m_device->setEffectPass(nullptr);
    m_pass = -1;
    restoreState(app, cpu);
    m_begun = false;
    return D3D_OK;
}

HRESULT Effect::GetDevice(WinApplication* app, x86::CPU& cpu, x86::reg32* device)
{
    RC_USE(app);
    RC_USE(cpu);
    if (!device)
        return D3DERR_INVALIDCALL;
    m_device->addRef();
    *device = m_device->guest();
    return D3D_OK;
}

HRESULT Effect::OnLostDevice(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    RC_USE(cpu);
    return D3D_OK;
}

HRESULT Effect::OnResetDevice(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    RC_USE(cpu);
    return D3D_OK;
}

// ---- Direct calls --------------------------------------------------------------------------------------------------

namespace direct
{

HRESULT setMatrix(WinApplication* app, x86::CPU& cpu, x86::reg32 effect, x86::reg32 handle, const float* matrix)
{
    Effect* e = static_cast<Effect*>(lookup(effect));
    return e ? e->SetMatrix(app, cpu, handle, matrix) : D3DERR_INVALIDCALL;
}

HRESULT setMatrixArray(WinApplication* app, x86::CPU& cpu, x86::reg32 effect, x86::reg32 handle,
                       const float* matrices, UINT count)
{
    Effect* e = static_cast<Effect*>(lookup(effect));
    return e ? e->SetMatrixArray(app, cpu, handle, matrices, count) : D3DERR_INVALIDCALL;
}

HRESULT setVectorArray(WinApplication* app, x86::CPU& cpu, x86::reg32 effect, x86::reg32 handle,
                       const float* vectors, UINT count)
{
    Effect* e = static_cast<Effect*>(lookup(effect));
    return e ? e->SetVectorArray(app, cpu, handle, vectors, count) : D3DERR_INVALIDCALL;
}

HRESULT setFloat(WinApplication* app, x86::CPU& cpu, x86::reg32 effect, x86::reg32 handle, float value)
{
    Effect* e = static_cast<Effect*>(lookup(effect));
    return e ? e->SetFloat(app, cpu, handle, value) : D3DERR_INVALIDCALL;
}

HRESULT setTexture(WinApplication* app, x86::CPU& cpu, x86::reg32 effect, x86::reg32 handle, x86::reg32 texture)
{
    Effect* e = static_cast<Effect*>(lookup(effect));
    return e ? e->SetTexture(app, cpu, handle, texture) : D3DERR_INVALIDCALL;
}

HRESULT commitChanges(WinApplication* app, x86::CPU& cpu, x86::reg32 effect)
{
    Effect* e = static_cast<Effect*>(lookup(effect));
    return e ? e->CommitChanges(app, cpu) : D3DERR_INVALIDCALL;
}

x86::reg32 getTechnique(WinApplication* app, x86::CPU& cpu, x86::reg32 effect, UINT index)
{
    Effect* e = static_cast<Effect*>(lookup(effect));
    return e ? e->GetTechnique(app, cpu, index) : 0;
}

HRESULT setTechnique(WinApplication* app, x86::CPU& cpu, x86::reg32 effect, x86::reg32 technique)
{
    Effect* e = static_cast<Effect*>(lookup(effect));
    return e ? e->SetTechnique(app, cpu, technique) : D3DERR_INVALIDCALL;
}

UINT begin(WinApplication* app, x86::CPU& cpu, x86::reg32 effect, DWORD flags)
{
    Effect* e = static_cast<Effect*>(lookup(effect));
    UINT passes = 0;
    if (e)
        e->Begin(app, cpu, &passes, flags);
    return passes;
}

HRESULT beginPass(WinApplication* app, x86::CPU& cpu, x86::reg32 effect, UINT pass)
{
    Effect* e = static_cast<Effect*>(lookup(effect));
    return e ? e->BeginPass(app, cpu, pass) : D3DERR_INVALIDCALL;
}

HRESULT endPass(WinApplication* app, x86::CPU& cpu, x86::reg32 effect)
{
    Effect* e = static_cast<Effect*>(lookup(effect));
    return e ? e->EndPass(app, cpu) : D3DERR_INVALIDCALL;
}

HRESULT end(WinApplication* app, x86::CPU& cpu, x86::reg32 effect)
{
    Effect* e = static_cast<Effect*>(lookup(effect));
    return e ? e->End(app, cpu) : D3DERR_INVALIDCALL;
}

}

namespace native
{

Pass* pass(x86::reg32 effect, x86::reg32 technique, uint32_t index)
{
    Object* object = lookup(effect);
    if (!object || object->kind() != KindEffect)
        return nullptr;
    return static_cast<Effect*>(object)->nativePass(technique, index);
}

const float* nativeRegisters(const PassConstant& constant, float* scratch)
{
    return static_cast<const Effect*>(constant.effect)->constantRegisters(
        *static_cast<const ShaderConstant*>(constant.constant), scratch);
}

uint32_t changes()
{
    return Effect::changes();
}

}

namespace direct
{

void fold(TraceCall& call, const float* values, uint32_t count)
{
    call.floats = uint8_t(count < TraceFloats ? count : TraceFloats);
    call.mask = 0xffff;
    call.hash = 0;
    for (uint32_t i = 0; i < call.floats; ++i)
        call.value[i] = values[i];
    uint64_t h = 0xcbf29ce484222325ull;
    for (uint32_t i = TraceFloats; i < count; ++i)
    {
        uint32_t bits;
        memcpy(&bits, values + i, 4);
        h = (h ^ bits) * 0x100000001b3ull;
    }
    call.hash = count > TraceFloats ? h : 0;
}

#if NFSC_RENDER_CHECK
std::vector<TraceCall>* g_trace = nullptr;

void trace(uint8_t kind, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5,
           const float* values, uint32_t count)
{
    TraceCall call = {};
    call.kind = kind;
    call.args[0] = a0;
    call.args[1] = a1;
    call.args[2] = a2;
    call.args[3] = a3;
    call.args[4] = a4;
    call.args[5] = a5;
    fold(call, values, values ? count : 0);
    g_trace->push_back(call);
}
#endif

}

// ---- Interfaces ----------------------------------------------------------------------------------------------------

namespace
{

#define D3DX_UNKNOWN D3D_NAMED("QueryInterface", Object, QueryInterface), D3D_NAMED("AddRef", Object, AddRef), \
                     D3D_NAMED("Release", Object, Release)

// ID3DXEffect of d3dx9_30: ID3DXBaseEffect (slots 3-56), then the effect methods.
const MethodEntry s_effect[] = {
    D3DX_UNKNOWN,
    D3D_METHOD(Effect, GetDesc),
    D3D_METHOD(Effect, GetParameterDesc),
    D3D_METHOD(Effect, GetTechniqueDesc),
    D3D_METHOD(Effect, GetPassDesc),
    D3D_TRAP(GetFunctionDesc),
    D3D_METHOD(Effect, GetParameter),
    D3D_METHOD(Effect, GetParameterByName),
    D3D_METHOD(Effect, GetParameterBySemantic),
    D3D_METHOD(Effect, GetParameterElement),
    D3D_METHOD(Effect, GetTechnique),
    D3D_METHOD(Effect, GetTechniqueByName),
    D3D_METHOD(Effect, GetPass),
    D3D_METHOD(Effect, GetPassByName),
    D3D_TRAP(GetFunction),
    D3D_TRAP(GetFunctionByName),
    D3D_METHOD(Effect, GetAnnotation),
    D3D_METHOD(Effect, GetAnnotationByName),
    D3D_METHOD(Effect, SetValue),
    D3D_METHOD(Effect, GetValue),
    D3D_METHOD(Effect, SetBool),
    D3D_METHOD(Effect, GetBool),
    D3D_METHOD(Effect, SetBoolArray),
    D3D_METHOD(Effect, GetBoolArray),
    D3D_METHOD(Effect, SetInt),
    D3D_METHOD(Effect, GetInt),
    D3D_METHOD(Effect, SetIntArray),
    D3D_METHOD(Effect, GetIntArray),
    D3D_METHOD(Effect, SetFloat),
    D3D_METHOD(Effect, GetFloat),
    D3D_METHOD(Effect, SetFloatArray),
    D3D_METHOD(Effect, GetFloatArray),
    D3D_METHOD(Effect, SetVector),
    D3D_METHOD(Effect, GetVector),
    D3D_METHOD(Effect, SetVectorArray),
    D3D_METHOD(Effect, GetVectorArray),
    D3D_METHOD(Effect, SetMatrix),
    D3D_METHOD(Effect, GetMatrix),
    D3D_METHOD(Effect, SetMatrixArray),
    D3D_METHOD(Effect, GetMatrixArray),
    D3D_METHOD(Effect, SetMatrixPointerArray),
    D3D_TRAP(GetMatrixPointerArray),
    D3D_METHOD(Effect, SetMatrixTranspose),
    D3D_METHOD(Effect, GetMatrixTranspose),
    D3D_METHOD(Effect, SetMatrixTransposeArray),
    D3D_METHOD(Effect, GetMatrixTransposeArray),
    D3D_METHOD(Effect, SetMatrixTransposePointerArray),
    D3D_TRAP(GetMatrixTransposePointerArray),
    D3D_METHOD(Effect, SetString),
    D3D_METHOD(Effect, GetString),
    D3D_METHOD(Effect, SetTexture),
    D3D_METHOD(Effect, GetTexture),
    D3D_TRAP(GetPixelShader),
    D3D_TRAP(GetVertexShader),
    D3D_METHOD(Effect, SetArrayRange),
    D3D_METHOD(Effect, GetPool),
    D3D_METHOD(Effect, SetTechnique),
    D3D_METHOD(Effect, GetCurrentTechnique),
    D3D_METHOD(Effect, ValidateTechnique),
    D3D_METHOD(Effect, FindNextValidTechnique),
    D3D_METHOD(Effect, IsParameterUsed),
    D3D_METHOD(Effect, Begin),
    D3D_METHOD(Effect, BeginPass),
    D3D_METHOD(Effect, CommitChanges),
    D3D_METHOD(Effect, EndPass),
    D3D_METHOD(Effect, End),
    D3D_METHOD(Effect, GetDevice),
    D3D_METHOD(Effect, OnLostDevice),
    D3D_METHOD(Effect, OnResetDevice),
    D3D_TRAP(SetStateManager),
    D3D_TRAP(GetStateManager),
    D3D_TRAP(BeginParameterBlock),
    D3D_TRAP(EndParameterBlock),
    D3D_TRAP(ApplyParameterBlock),
    D3D_TRAP(CloneEffect),
};
static_assert(sizeof(s_effect) / sizeof(s_effect[0]) == 77, "ID3DXEffect of d3dx9_30 has 77 slots");

const MethodEntry s_pool[] = {
    D3DX_UNKNOWN,
};

}

void initEffectInterfaces(WinApplication* app)
{
    buildInterface(app, KindEffect, "ID3DXEffect", s_effect, sizeof(s_effect) / sizeof(s_effect[0]));
    buildInterface(app, KindEffectPool, "ID3DXEffectPool", s_pool, sizeof(s_pool) / sizeof(s_pool[0]));
}

}}

// ---- Entry points --------------------------------------------------------------------------------------------------

namespace win32 { namespace d3dx9_30
{

HRESULT D3DXCreateEffectPool(WinApplication* app, x86::CPU& cpu, x86::reg32* pool)
{
    RC_USE(cpu);
    d3d9::initInterfaces(app);
    if (!pool)
        return d3d9::D3DERR_INVALIDCALL;
    *pool = d3d9::publish(app, new d3d9::EffectPool());
    return d3d9::D3D_OK;
}

// The resource is an RT_RCDATA entry of the executable (module 0) by name or id.
HRESULT D3DXCreateEffectFromResourceA(WinApplication* app, x86::CPU& cpu, x86::reg32 device, HMODULE module,
                                      x86::reg32 resource, x86::reg32 defines, x86::reg32 include, DWORD flags,
                                      x86::reg32 pool, x86::reg32* effect, x86::reg32* errors)
{
    RC_USE(defines);
    RC_USE(include);
    RC_USE(flags);
    d3d9::initInterfaces(app);
    if (errors)
        *errors = 0;
    d3d9::Object* deviceObject = d3d9::lookup(device);
    d3d9::Object* poolObject = pool ? d3d9::lookup(pool) : nullptr;
    if (!deviceObject || deviceObject->kind() != d3d9::KindDevice || !effect ||
        (pool && (!poolObject || poolObject->kind() != d3d9::KindEffectPool)))
        return d3d9::D3DERR_INVALIDCALL;
    if (module != 0 && module != 0x400000)
        unsupported(app, cpu, "D3DXCreateEffectFromResourceA", "module 0x%08x", module);
    const x86::reg32 info = kernel32::FindResourceA(app, cpu, 0, resource, 10);
    const std::string name = resource >= 0x10000 ? std::string(reinterpret_cast<const char*>(g_guestBase + resource))
                                                 : std::to_string(resource);
    if (!info)
    {
        pf::logError("d3dx9: effect resource %s not found", name.c_str());
        return 0x88760B59;  // D3DXERR_INVALIDDATA
    }
    const x86::reg32 data = kernel32::LoadResource(app, cpu, 0, info);
    const DWORD size = kernel32::SizeofResource(app, cpu, 0, info);
    d3d9::Effect* e = new d3d9::Effect(static_cast<d3d9::Device*>(deviceObject),
                                       static_cast<d3d9::EffectPool*>(poolObject), name);
    if (!e->parse(g_guestBase + data, size, data))
    {
        pf::logError("d3dx9: effect %s (%u bytes) is not a valid compiled effect", name.c_str(), size);
        delete e;
        return 0x88760B59;  // D3DXERR_INVALIDDATA
    }
    *effect = d3d9::publish(app, e);
    pf::logInfo("d3dx9: effect %s created", name.c_str());
    return d3d9::D3D_OK;
}

}}
