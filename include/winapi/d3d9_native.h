#ifndef WINAPI_D3D9_NATIVE_H_
#define WINAPI_D3D9_NATIVE_H_

// Native renderer for effect passes: per-pass tables built once from the parsed effects (programs, states, the
// registers and layout of each parameter's constant), and a run that applies them to its own mirror of the device
// state and constant registers and records finished draws (render::DrawState and constant blocks) instead of going
// through the effect and device layers. In the render check build a run shadows the translated path's draws and
// its draws are compared with theirs on the render thread.

#include <winapi/d3d9_internal.h>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace win32 { namespace d3d9 { namespace native {

enum StateTarget : uint8_t
{
    TargetRender, TargetStage, TargetSampler, TargetTexture
};

enum StateSource : uint8_t
{
    SourceLiteral,              // value
    SourceRaw,                  // the parameter's first value as stored
    SourceExpression            // the parameter's first value as a float, in the state's value type
};

// A state a pass sets: render, stage or sampler state, or a stage's texture.
struct PassState
{
    uint8_t         target;     // StateTarget
    uint8_t         source;     // StateSource
    uint8_t         valueType;  // expressions: bool, int or float
    uint8_t         slotType;   // the parameter's first value's type
    uint16_t        index;      // stage or sampler register
    uint16_t        type;       // Direct3D render, stage or sampler state number
    DWORD           value;
    const uint8_t*  slot;       // the parameter's first value
};

// A shader constant bound to a parameter, and the native run's last upload of it.
struct PassConstant
{
    bool            pixel;
    uint16_t        first, count;   // registers
    const uint32_t* version;        // the parameter block's change counter value
    const void*     effect;         // for the conversion (nativeRegisters)
    const void*     constant;
    uint32_t        written = 0;    // version uploaded
    uint64_t        stamp = 0;      // register stamp of the upload; 0: none
};

enum OpKind : uint8_t
{
    OpState, OpConstant
};

struct PassOp
{
    uint8_t         kind;           // OpKind
    uint16_t        index;          // into states or constants
    const uint32_t* version;        // commits: the parameter block's change counter value
};

struct Pass
{
    EffectDraw                  programs;
    uint32_t                    passes;         // passes of the technique
    bool                        world;          // an effect of the world family
    std::vector<PassState>      states;
    std::vector<PassConstant>   constants;
    std::vector<PassOp>         apply;          // BeginPass order
    std::vector<PassOp>         commits;        // CommitChanges order
    // The states the technique's Begin saves and its End restores.
    uint8_t                     savedRender[MaxRenderStates];
    uint8_t                     savedStage[MaxStages][D3DTSS_COUNT];
    uint8_t                     savedSampler[MaxSamplerStages][MaxSamplerStates];
    uint8_t                     savedTexture[MaxSamplerStages];
};

// The table of pass index of the technique handle of the effect behind the guest pointer effect; null when it has
// none (no programs, or a technique that saves the vertex input).
Pass* pass(x86::reg32 effect, x86::reg32 technique, uint32_t index);
// The registers of a constant from its parameter's current value (scratch: 4 * count floats, maybe not used).
const float* nativeRegisters(const PassConstant& constant, float* scratch);
// The effects' parameter change counter.
uint32_t changes();

// A run of one effect pass: the states the pass and the flush set, the constant registers, its draws.
class Run
{
public:
    // emit: production run (records draws, brings the device up to date at the end); else a render check shadow.
    explicit Run(bool emit) : m_emit(emit) {}

    // The device state before the effect's Begin.
    void begin(Device* device, Pass* pass);
    // The pass's states and constants (BeginPass).
    void beginPass();
    void renderState(DWORD state, DWORD value);
    void samplerState(DWORD sampler, DWORD type, DWORD value);
    // The states and constants of parameters changed since (CommitChanges).
    void commit();
    // An indexed draw from the device's stream source and indices: production runs record it; shadows keep it for
    // drawsFor. False when the geometry cannot draw.
    bool drawIndexed(WinApplication* app, x86::CPU& cpu, DWORD type, UINT vertices, UINT startIndex, UINT count);
    // Production: the device as the effect's End leaves it (the states the technique does not save as the run
    // left them) and the constant registers written.
    void end(WinApplication* app, x86::CPU& cpu);
    Pass* table() const                     { return m_pass; }

#if NFSC_RENDER_CHECK
    // Shadows: the kept draws recorded for the translated draws since serial (logged when the counts differ);
    // returns false then.
    bool drawsFor(uint32_t serial);
    // Shadows after the translated End: the device's states and registers against what a production run leaves;
    // returns the number of differences (the first logged while report).
    uint32_t checkEnd(bool report);
    // Starts whose state or registers differed from the device's after begin, and starts so far.
    uint64_t syncsDiffer() const            { return m_syncsDiffer; }
    uint64_t syncs() const                  { return m_syncs; }
#endif

private:
    struct Mirror : FixedState
    {
        Object*     textures[MaxSamplerStages];
    };
    // What a draw's constant blocks are made of (effectConstants' arguments; state and test in the draw).
    struct BlockInputs
    {
        EffectSource                    source;
        const render::DrawState*        state;
        const float*                    test;
        const render::TextureStorage*   volume;
        const render::TextureStorage*   volume2;
    };
    // The inputs of a draw's effect state besides the mirror's words.
    struct StateKey
    {
        uint32_t                        changes;    // m_stateChanges
        EffectDraw                      programs;
        uint8_t                         layout, primitive;
        uint32_t                        bias;       // the bias register's x, when the vertex program has one
        const render::TextureStorage*   textures[render::MaxSamplers];
    };
    struct Kept
    {
        NativeGeometry  geometry;
        NativeDraw      draw;
        uint32_t        vertex, fragment;       // offsets into m_keptFloats
    };

    // Fixed state words up to the end of the sampler states, the ones a run writes.
    enum : uint32_t { StateWords = (offsetof(FixedState, samplerStates) + sizeof(FixedState::samplerStates)) / 4 };

    DWORD value(const PassState& state) const;
    void applyState(const PassState& state);
    void touch(uint32_t word)
    {
        m_touched[word >> 5] |= 1u << (word & 31);
        ++m_stateChanges;
    }
    void syncState(bool delta);
    bool touchLogged();
    template< typename S > void endTouched(S& writes);
    template< typename S > void endAll(S& writes);
    void upload(PassConstant& constant);
    bool held(const PassConstant& constant) const;
    void syncRegisters(bool delta);
    bool build(DWORD type, UINT vertices, UINT startIndex, UINT count, NativeGeometry& geometry, NativeDraw& draw,
               BlockInputs& blocks);
    static void writeBlocks(const void* context, float* vertex, float* fragment);

    bool        m_emit;
    Device*     m_device = nullptr;
    Pass*       m_pass = nullptr;
    Mirror      m_state;
    Mirror      m_before;               // the device state at begin
    uint32_t    m_applied = 0;          // parameter change counter at the last apply
    float       m_vertex[Device::VertexFloatConstants * 4];
    float       m_pixel[Device::PixelFloatConstants * 4];
    uint64_t    m_vertexStamps[Device::VertexFloatConstants];
    uint64_t    m_pixelStamps[Device::PixelFloatConstants];
    uint64_t    m_stamp = 0;
    uint32_t    m_dirty[2][2] = {};     // per bank: written registers [first, end)
    bool        m_synced = false;       // the banks hold the device's registers as of m_constantSeen
    // The device whose logs were read up to m_stateSeen and m_constantSeen (null: copy everything next time), and
    // the state words written since the mirror was last the device's, as bits.
    const Device*   m_seen = nullptr;
    uint32_t    m_stateSeen = 0, m_constantSeen = 0;
    uint32_t    m_touched[(StateWords + 31) / 32] = {};
#if NFSC_RENDER_CHECK
    uint64_t    m_syncs = 0, m_syncsDiffer = 0;
#endif
    uint32_t    m_generation = 1;       // changes of the banks
    // The constant blocks of the last draw and what they were made of.
    float       m_vertexBlock[Device::VertexFloatConstants * 4 + 64];
    float       m_fragmentBlock[Device::PixelFloatConstants * 4 + 64];
    uint32_t    m_blockKey[12] = {};
    bool        m_blockValid = false;
    const render::TextureStorage*   m_textures[render::MaxSamplers] = {};  // the last draw's stages
    // Writes to the mirror's words so far, and the last effect state built with what it was made of.
    uint32_t                        m_stateChanges = 0;
    bool                            m_stateValid = false;
    StateKey                        m_stateKey;
    render::DrawState               m_lastState;
    float                           m_lastTest[2];
    const render::TextureStorage*   m_lastVolume = nullptr;
    const render::TextureStorage*   m_lastVolume2 = nullptr;
    std::vector<Kept>   m_kept;
    std::vector<float>  m_keptFloats;
};

}}}

#endif
