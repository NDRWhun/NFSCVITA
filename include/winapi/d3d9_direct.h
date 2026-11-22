#ifndef WINAPI_D3D9_DIRECT_H_
#define WINAPI_D3D9_DIRECT_H_

// Effect and device calls made by native renderer code by guest COM pointer, without the guest vtables, and the
// call trace of the render check build (NFSC_RENDER_CHECK) that compares them with the translated code's calls.

#include <winapi/d3d9_internal.h>
#include <cstdint>
#include <vector>

namespace win32 { namespace d3d9 { namespace direct {

// ID3DXEffect methods on the effect behind the guest pointer effect.
HRESULT setMatrix(WinApplication* app, x86::CPU& cpu, x86::reg32 effect, x86::reg32 handle, const float* matrix);
HRESULT setMatrixArray(WinApplication* app, x86::CPU& cpu, x86::reg32 effect, x86::reg32 handle,
                       const float* matrices, UINT count);
HRESULT setVectorArray(WinApplication* app, x86::CPU& cpu, x86::reg32 effect, x86::reg32 handle,
                       const float* vectors, UINT count);
HRESULT setFloat(WinApplication* app, x86::CPU& cpu, x86::reg32 effect, x86::reg32 handle, float value);
HRESULT setTexture(WinApplication* app, x86::CPU& cpu, x86::reg32 effect, x86::reg32 handle, x86::reg32 texture);
HRESULT commitChanges(WinApplication* app, x86::CPU& cpu, x86::reg32 effect);
x86::reg32 getTechnique(WinApplication* app, x86::CPU& cpu, x86::reg32 effect, UINT index);
HRESULT setTechnique(WinApplication* app, x86::CPU& cpu, x86::reg32 effect, x86::reg32 technique);
// Begin with the pass count returned (0 on failure).
UINT begin(WinApplication* app, x86::CPU& cpu, x86::reg32 effect, DWORD flags);
HRESULT beginPass(WinApplication* app, x86::CPU& cpu, x86::reg32 effect, UINT pass);
HRESULT endPass(WinApplication* app, x86::CPU& cpu, x86::reg32 effect);
HRESULT end(WinApplication* app, x86::CPU& cpu, x86::reg32 effect);

// The device behind the guest pointer device.
inline Device* device(x86::reg32 guest)
{
    return static_cast<Device*>(lookup(guest));
}

enum TraceKind : uint8_t
{
    TraceEffectMatrix,          // effect, handle; 16 floats
    TraceEffectVectors,         // effect, handle, count; the floats the effect takes
    TraceEffectFloat,           // effect, handle; 1 float
    TraceEffectTexture,         // effect, handle, texture
    TraceEffectCommit,          // effect
    TraceRenderState,           // state, value
    TraceSamplerState,          // sampler, type, value
    TraceTransform,             // state; 16 floats
    TraceDrawIndexed,           // type, base, min, vertices, start, count
    TraceStreamSource,          // stream, buffer, offset, stride
    TraceIndices,               // buffer
    TraceDeviceTexture,         // stage, texture
    TraceEffectMatrices,        // effect, handle, count; 16 floats per matrix
    TraceEffectTechnique,       // effect, technique
    TraceEffectBegin,           // effect, flags
    TraceEffectBeginPass,       // effect, pass
    TraceEffectEndPass,         // effect
    TraceEffectEnd,             // effect
    TraceDeclaration,           // declaration
    TraceWildcard             // plans only: calls of translated code that are not compared
};

enum
{
    TraceFloats = 16
};

// One call. Objects are host object addresses; floats beyond TraceFloats are folded into hash.
struct TraceCall
{
    uint8_t     kind;
    uint8_t     floats;         // valid entries of value
    uint16_t    mask;           // compared floats (bit per entry); 0xffff: all
    uint32_t    args[6];
    float       value[TraceFloats];
    uint64_t    hash;
};

void fold(TraceCall& call, const float* values, uint32_t count);

#if NFSC_RENDER_CHECK
// While set, the effect and device methods append their calls (render check build only).
extern std::vector<TraceCall>* g_trace;
void trace(uint8_t kind, uint32_t a0, uint32_t a1 = 0, uint32_t a2 = 0, uint32_t a3 = 0, uint32_t a4 = 0,
           uint32_t a5 = 0, const float* values = nullptr, uint32_t count = 0);
#define D3D9_TRACE(...) do { if (::win32::d3d9::direct::g_trace) ::win32::d3d9::direct::trace(__VA_ARGS__); } while (0)
// Calls made inside a traced method are not traced while this lives.
struct TraceSuspend
{
    std::vector<TraceCall>* saved = g_trace;
    TraceSuspend() { g_trace = nullptr; }
    ~TraceSuspend() { g_trace = saved; }
};
#define D3D9_TRACE_SUSPEND() const ::win32::d3d9::direct::TraceSuspend traceSuspend
#else
#define D3D9_TRACE(...) do {} while (0)
#define D3D9_TRACE_SUSPEND() do {} while (0)
#endif

}}}

#endif
