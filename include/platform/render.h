#ifndef PLATFORM_RENDER_H_
#define PLATFORM_RENDER_H_

// The frame data model between the Direct3D 9 layer (game thread, records) and a device that draws it (the GXM
// render thread on the Vita). Platform neutral.
//
// A Frame is a list of scenes (one render target and depth surface each) and of draw calls. A draw call has a
// DrawState (compared with memcmp: equal states need no state changes), the vertex and index data it reads, its
// shader constants (offsets into the frame's constant arena) and a viewport. Clears are draw calls of the clear
// programs. Programs are the port's own: the fixed-function variants, the clear programs and the translations of the
// game's effect shaders.
//
// Coordinates follow Direct3D: clip space z in [0, 1], window y down, pixel centres at integer coordinates (the
// devices add the half pixel). Matrices are Direct3D row-vector matrices, stored as 4 rows of float4.

#include <platform/texture.h>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace pf { namespace render {

using texture::Format;

// ---- Resources (created by the device; the layer keeps pointers) ---------------------------------------------------

// lastUse: index of the last frame that read the resource; a frame with a higher index than the device's completed
// frame may still read it, so the layer writes a busy resource only through a new copy.
struct Resource
{
    uint32_t lastUse = 0;
};

struct TextureDesc
{
    Format      format;
    uint8_t     levels;
    bool        cube;
    bool        target;     // render target: every level and face rendered to by scenes
    uint16_t    width, height;
    bool        linear = false; // streamed: one uncompressed level in linear rows, rewritten by the CPU (movie frames)
    uint16_t    depth = 0;      // a volume texture's atlas: its slices, side by side along u
    bool        yuv = false;    // streamed 4:2:0 planes (BT.601 video range) the texture unit converts to RGB
};

// A 4:2:0 frame: width x height luma bytes and (width / 2) x ((height + 1) / 2) bytes of each chroma plane.
struct Planes
{
    const uint8_t*  y;
    const uint8_t*  u;
    const uint8_t*  v;
    uint32_t        yStride, uvStride;
    uint32_t        width, height;
};

struct DepthStorage : Resource
{
    uint16_t    width, height;
    bool        kept = false;   // stored after every scene: the surface of a depth texture
};

struct TextureStorage : Resource
{
    TextureDesc     desc;
    DepthStorage*   depth = nullptr;    // FormatDepth: the depth surface scenes render to, owned by the texture
    // Smallest alpha of each level as last written (2D textures that are not targets), 0 while unknown.
    std::atomic<uint8_t> levelAlpha[16] = {};
};

struct BufferStorage : Resource
{
    uint32_t    size;
    void*       data;       // vertex or index data the GPU reads; written by the layer
    std::atomic<uint32_t> writes{0};    // in-place writes of the layer
    // Render thread (d3d9 resolver): smallest byte at alphaOffset + n * alphaStride over the buffer, read when writes
    // was alphaWrites; alphaScans counts the scans.
    uint32_t    alphaWrites = ~0u;
    uint16_t    alphaStride = 0, alphaOffset = 0;
    uint8_t     alpha = 0, alphaScans = 0;
};

// ---- Programs ------------------------------------------------------------------------------------------------------

enum VertexProgram : uint8_t
{
    VpFixed,                    // fixed function, untransformed positions
    VpFixedScreen,              // fixed function, POSITIONT
    VpClear,
    // Effect shaders (shaders/<name>.cg)
    VpFe,
    VpFeMask,
    VpFilter,
    VpScreenFilter,
    VpPip,
    VpScreen,
    VpCar,
    VpGlass,
    VpGlassLow,
    VpCarNormalMap,
    VpUcap,
    VpWorld,
    VpWorldLow,
    VpPrelit,
    VpSky,
    VpWorldNormalMap,
    VpRoad,
    VpWorldBone,
    VpInstance,
    VpWater,
    VpParticle,
    VpStreak,
    VpFlare,
    VpRainDrop,
    VpGhost,
    VpRvm,
    VpCaster,
    VpCasterDepth,
    VpDepth,
    VpDepthSkinned,
    VpMotionBlur,
    VpCarNormalMap1,
    VpGlass1,
    VpFlare1,
    VpRoad1,
    VpWorld1,
    VpWorldNormalMap1,
    VpUcap1,
    VpCount
};

enum FragmentProgram : uint8_t
{
    FpFixedColor,               // fixed function, no texture
    FpFixedTexture,
    FpFixedProjected,           // texture coordinates divided by z (COUNT3 | PROJECTED)
    FpClear,
    // Effect shaders (shaders/<name>.cg)
    FpFe,
    FpFeMask,
    FpFilter1,
    FpFilter2,
    FpFilter3,
    FpFilter4,
    FpFilterLum,
    FpFilterPremul,
    FpScreenFilter,
    FpScreenFilterGain,
    FpPip,
    FpVisualTreatment,
    FpPassthru,
    FpUves,
    FpUvesDarken,
    FpBlend,
    FpCombine,
    FpDown2,
    FpDown2MotionBlur,
    FpDown4,
    FpDownBloom,
    FpGauss,
    FpBlur15,
    FpCar,
    FpGlass,
    FpGlassLow,
    FpCarNormalMap,
    FpUcap,
    FpWorld,
    FpModulate,
    FpPrelit,
    FpFeFog,
    FpWorldNormalMap,
    FpRoad,
    FpTexture,
    FpColor,
    FpWater,
    FpParticle,
    FpStreak,
    FpFlare,
    FpRainDrop,
    FpGhost,
    FpRvm,
    FpCasterGreen,
    FpCasterBlack,
    FpCasterDepth,
    FpVisualTreatmentEnhanced,
    FpDepthTexture,
    FpMotionBlur,
    FpComposite,
    FpCar1,
    FpCarNormalMap1,
    FpGlass1,
    FpPip1,
    FpRvm1,
    FpVisualTreatment1,
    FpParticle1,
    FpRoad1,
    FpOpaque,
    FpTextureOpaque,
    FpDown4Tent,                // FpDown4 and FpDownBloom of a source a whole multiple of the target's size
    FpDownBloomTent,
    FpVisualTreatmentNearest,   // FpVisualTreatment and FpVisualTreatmentEnhanced reading the nearest colour cube slice
    FpVisualTreatmentEnhancedNearest,
    FpCount
};

// Effect fragment programs: flags, and the place of their constants after uAlphaTest (FragmentProgramInfo).
enum : uint8_t
{
    ProgramClampConstants = 1,  // ps_1_x: the constant registers read clamped to [-1, 1]
    ProgramProjectStage0 = 2,   // ps_1_x reading tex t0 with all four coordinates: uProj
    ProgramFog = 4              // fixed-function fog from uFog: in a fragment program after the vertex program's
                                // factor; in a vertex program, COLOR1 = (fog colour * (1 - factor), factor)
};

// Constant blocks: arrays of floats; the field tables below give the uniform names.
struct FixedVertexConstants
{
    float wvp[16];              // VpFixed: world * view * projection
    float screen[4];            // VpFixedScreen: x scale, y scale, x offset, y offset (window -> clip)
    float texMatrix[16];        // stage 0 texture transform (identity without one)
    float texPad[4];            // added to the texture coordinate input: missing components become (.., 1, 0)
    float diffuseMask[4];       // 1: the vertex colour is a factor of the stage result, per component
    float factorMask[4];        // 1: the texture factor is a factor
    float factor[4];            // TEXTUREFACTOR as rgba
};

// Every fragment block starts with the alpha test: discard when alphaTest.x * alpha < alphaTest.y (programs
// built with ATEST).
struct FixedFragmentConstants
{
    float alphaTest[4];
    float textureMask[4];       // 1: the texel is a factor, per component
    float scale[4];             // 1, 2 or 4 (MODULATE, 2X, 4X)
    float addMask[4];           // 1: the texel is added (ADD)
};

struct ClearFragmentConstants
{
    float alphaTest[4];
    float color[4];
};

struct UniformField
{
    const char* name;
    uint16_t    offset;         // floats
    uint16_t    count;
};

// Vertex inputs, by Direct3D usage.
enum Usage : uint8_t
{
    UsagePosition,
    UsageNormal,
    UsageColor,
    UsageTex0,
    UsageTex1,
    UsageTex2,
    UsageTex3,
    UsageWeight,
    UsageIndices,
    UsageTangent,
    UsageBinormal,
    UsageTex4,
    UsageTex5,
    UsageTex6,
    UsageTex7,
    UsageTex8,
    UsageColor1,
    UsageCount
};

// An effect vertex program's block: uC (registers c0..), uP (pixel registers c0.., for work moved from the pixel
// shader), uFog (fog colour rgb, 1 when fog is enabled).
struct VertexProgramInfo
{
    const char*         name;           // shader binary shader_<name>
    uint16_t            constantFloats;
    uint8_t             fieldCount;
    const UniformField* fields;
    uint8_t             registers;      // effect programs: constant registers c0.. as uC
    uint8_t             pixelRegisters;
    uint8_t             biasRegister;   // NoBias, or the register whose x is the mip bias of sampler 0
    uint8_t             flags;          // ProgramFog: the block has uFog
    uint16_t            pixelOffset, fogOffset;     // float offsets of uP and uFog
};

// An effect fragment program's block: uAlphaTest, uC (registers c0..), uProj (the stage 0 projection: z and w
// selectors, 1 when not projected), uVol (the volume atlas of sampler volumeSampler: depth, 1 / depth, 0.5 / slice
// width, 1 - 0.5 / slice width), uFog (fog colour rgb, 1 when fog is enabled), uVol2 (the atlas of volumeSampler2).
struct FragmentProgramInfo
{
    const char*         name;           // shader_<name>; the alpha-tested variant is shader_<name>_atest
    uint16_t            constantFloats;
    uint8_t             fieldCount;
    const UniformField* fields;
    uint8_t             samplers;       // uTex0..uTex<n-1>: Direct3D sampler registers s0..s<n-1> for effect programs
    uint16_t            blendFactor;    // float offset of uBlendFactor; 0: no factor variants
    bool                effect;
    uint8_t             registers;
    uint8_t             flags;          // Program* flags
    uint8_t             volumeSampler;  // NoVolume, or the sampler that reads a volume atlas
    uint16_t            projOffset, volumeOffset, fogOffset;    // float offsets of uProj, uVol and uFog
    uint8_t             volumeSampler2; // NoVolume, or a second sampler that reads a volume atlas
    uint16_t            volumeOffset2;  // float offset of uVol2
};

enum : uint8_t
{
    NoVolume = 0xff,
    NoBias = 0xff
};

// Blend factor variants of a program (shader_<name>_bfcolor, _bfalpha, _bfscale; no alpha test). Direct3D's
// constant blend colour has no GXM factor: the layer turns BLENDFACTOR and INVBLENDFACTOR terms into fixed factors
// and one of these outputs, with uBlendFactor holding the factor (or 1 - factor).
enum FactorOutput : uint8_t
{
    FactorNone,
    FactorColor,                // output = uBlendFactor (source factor ZERO, destination SRCCOLOR)
    FactorAlpha,                // alpha = uBlendFactor.a (SRCALPHA, INVSRCALPHA)
    FactorScale,                // output = saturate(colour) * uBlendFactor (source factor ONE)
    FactorCount
};
const char* factorSuffix(FactorOutput output);

const VertexProgramInfo& vertexProgram(VertexProgram program);
const FragmentProgramInfo& fragmentProgram(FragmentProgram program);
// Name of a vertex input in the programs: aPosition, aNormal, aColor, aTex0..8, aWeight, aIndex, aTangent,
// aBinormal, aColor1.
const char* inputName(Usage usage);

// ---- Vertex layouts ------------------------------------------------------------------------------------------------

enum ElementType : uint8_t
{
    TypeFloat1,
    TypeFloat2,
    TypeFloat3,
    TypeFloat4,
    TypeColor,                  // D3DCOLOR: bytes B, G, R, A (normalised)
    TypeUByte4,
    TypeShort2,
    TypeShort4,
    TypeUByte4N,
    TypeCount
};

struct VertexElement
{
    uint8_t     usage;          // Usage
    uint8_t     type;           // ElementType
    uint16_t    offset;
};

enum
{
    MaxVertexElements = 12
};

struct VertexLayout
{
    uint16_t        stride;
    uint8_t         count;
    bool            positionT;  // POSITIONT: window coordinates and 1/w
    VertexElement   elements[MaxVertexElements];
};

// Registers a layout (equal layouts share an id) and returns its id; game thread. Ids are small and stay valid.
uint8_t registerLayout(const VertexLayout& layout);
const VertexLayout& layout(uint8_t id);

// ---- Draw state ----------------------------------------------------------------------------------------------------

enum
{
    MaxSamplers = 8
};

enum Address : uint8_t
{
    AddressWrap,
    AddressClamp,
    AddressMirror
};

enum Mip : uint8_t
{
    MipNone,
    MipPoint,
    MipLinear
};

struct Sampler
{
    uint8_t addressU;           // Address
    uint8_t addressV;
    uint8_t filter;             // bit 0: min linear, bit 1: mag linear
    uint8_t mip;                // Mip
    int8_t  lodBias;            // mip level bias in eighths
    uint8_t level;              // 0: every level; else the only level read (a copy from a lower level)
};

enum Primitive : uint8_t
{
    PrimitiveTriangles,
    PrimitiveStrip,
    PrimitiveFan
};

// Direct3D values: blend factors D3DBLEND (1..13; the layer resolves 14 and 15), compare D3DCMP (1..8), cull
// D3DCULL (1 none, 2 cw, 3 ccw), colour mask D3DCOLORWRITEENABLE bits (1 r, 2 g, 4 b, 8 a).
struct DrawState
{
    const TextureStorage*   textures[MaxSamplers];
    Sampler                 samplers[MaxSamplers];
    uint8_t                 vertexProgram;
    uint8_t                 fragmentProgram;
    uint8_t                 layout;
    uint8_t                 primitive;
    uint8_t                 blendEnable;
    uint8_t                 srcBlend;
    uint8_t                 dstBlend;
    uint8_t                 colorMask;
    uint8_t                 alphaTest;      // the program variant with the alpha test
    uint8_t                 depthEnable;
    uint8_t                 depthWrite;
    uint8_t                 depthFunc;
    uint8_t                 cullMode;
    uint8_t                 factorOutput;   // FactorOutput; FactorNone with the alpha test
    uint8_t                 pad[2];
};

struct Viewport
{
    float x, y, width, height;
    float minZ, maxZ;
};

enum : uint32_t
{
    NoConstants = 0xffffffffu
};

struct DrawCall
{
    DrawState       state;
    uint32_t        vertexConstants;    // float offset into Frame::constants, or NoConstants
    uint32_t        fragmentConstants;
    const void*     vertices;           // vertex 0 of the draw
    const uint16_t* indices;            // null: vertices in order
    uint32_t        count;              // indices or vertices
    uint32_t        vertexCount;        // vertices the indices reach (min index 0)
    uint16_t        viewport;           // index into Frame::viewports
};

// A render target: a level of a target texture (or of a cube face) or the back buffer (null), and a depth surface or
// none. A target level smaller than its scene's size (a reduced shadow map) is drawn with the viewports scaled to it.
struct Target
{
    const TextureStorage*   color;
    uint8_t                 face;
    uint8_t                 level;
    const DepthStorage*     depth;      // null: no depth surface
};

struct Scene
{
    Target      target;
    uint16_t    width, height;
    uint32_t    firstDraw, drawCount;
    bool        depthCleared;           // the scene starts with a clear of the whole depth surface
    float       clearDepth;
};

// Arenas and arrays of a frame slot; the device sets the pointers and capacities, the layer the counts.
struct Frame
{
    uint32_t    index;                  // 1, 2, ... in submission order
    uint16_t    width, height;          // back buffer

    Scene*      scenes;
    uint32_t    sceneCount, sceneCapacity;
    DrawCall*   draws;
    uint32_t    drawCount, drawCapacity;
    Viewport*   viewports;
    uint32_t    viewportCount, viewportCapacity;
    float*      constants;
    uint32_t    constantCount, constantCapacity;   // floats
    uint8_t*    vertices;                           // GPU-visible arena for user-pointer draws
    uint32_t    vertexBytes, vertexCapacity;

    // The layer's recording of the frame: resolve (render thread, before the frame is drawn) completes the
    // draws from it. Null: the draws are complete.
    void        (*resolve)(Frame& frame);
    const uint32_t* commands;
    uint32_t    commandCount;                       // words

    // The gamma ramp the frame is shown through: 8-bit red, green and blue tables; unused when gammaIdentity.
    bool        gammaIdentity;
    uint8_t     gammaRamp[3][256];
    float       gammaExponent;                      // the ramp is colour^gammaExponent; 0 when it is not
};

// Smallest alpha a sampler reads from a texture (all levels): 0 when unknown.
uint8_t minAlpha(const TextureStorage& texture);

// Arena helpers: null when the arena is full.
float* allocateConstants(Frame& frame, uint32_t count, uint32_t* offset);
void* allocateVertices(Frame& frame, uint32_t bytes);

inline bool operator==(const DrawState& a, const DrawState& b)
{
    return __builtin_memcmp(&a, &b, sizeof(DrawState)) == 0;
}

}}

#endif
