// Native draw list flush: eDataRender::FlushList (0x727230) with the per-record effect setters SetTransform
// (0x71e660), the light context (0x71dfa0), the mesh texture and state setters (0x7234a0, 0x71dce0, 0x71de10,
// 0x71deb0) and the draw (0x71ecb0) as native code calling the effect and the device directly, with the light
// material setter (0x71e030), the bone (0x714cc0) and PCA (0x71e540, 0x70d650) setters, stencil-masked draws and
// the effect switches (0x723600, the classes' Start methods except the road reflection and water ones,
// EndPass/End); native_flush=0, native_material=0, native_parts=0 and native_switch=0 turn them off. The vertex
// buffer setter, other texture animations and those two Start methods still run translated code. With
// native_uniforms the effect runs that allow it draw through the native renderer's pass tables (d3d9_native.h). In
// the render check build (NFSC_RENDER_CHECK) the translated code draws, each native step is compared with its calls
// and the native renderer shadows the runs, its draws compared on the render thread.

#include <lib/guest.h>
#include <native.h>
#include <platform/config.h>
#include <platform/log.h>
#include <recomp.h>
#include <winapi/d3d9_direct.h>
#include <winapi/d3d9_native.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace nfsc
{

void sub_727230_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_723600(win32::WinApplication* app, x86::CPU& cpu);
void sub_71e660(win32::WinApplication* app, x86::CPU& cpu);
void sub_71dfa0(win32::WinApplication* app, x86::CPU& cpu);
void sub_71e030(win32::WinApplication* app, x86::CPU& cpu);
void sub_71e540(win32::WinApplication* app, x86::CPU& cpu);
void sub_7234a0(win32::WinApplication* app, x86::CPU& cpu);
void sub_714cc0(win32::WinApplication* app, x86::CPU& cpu);
void sub_70ec20(win32::WinApplication* app, x86::CPU& cpu);
void sub_71ecb0(win32::WinApplication* app, x86::CPU& cpu);
void sub_71dce0(win32::WinApplication* app, x86::CPU& cpu);
void sub_71deb0(win32::WinApplication* app, x86::CPU& cpu);

namespace
{

namespace d3d9 = win32::d3d9;
namespace direct = win32::d3d9::direct;
namespace nr = win32::d3d9::native;
typedef void (*GuestFunction)(win32::WinApplication*, x86::CPU&);

// Guest data of the flush.
enum : x86::reg32
{
    Records             = 0xab2780,     // 0x68-byte records
    RecordCount         = 0xab0bf0,
    RecordVertices      = 0xab0bec,
    SortKeys            = 0xb1db90,
    CurrentEffect       = 0xab0ba4,
    DevicePointer       = 0xab0abc,
    CurrentView         = 0xab08f4,
    ViewTable           = 0xb47a90,     // 0x1e0 bytes per view
    Indices             = 0xab0bb8,     // index buffer set last
    StreamBuffer        = 0xab0bb4,     // vertex buffer set last
    VertexBuffersMade   = 0xab0bf8,
    StripPool           = 0xab09ac,
    LastMatrix          = 0xb1f2e8,     // SetTransform's cache: matrix, effect, view frame
    LastEffect          = 0xb1f2e4,
    LastFrame           = 0xa651a4,
    LightVector         = 0xb77f34,     // pointer; +0x160 the world light vector
    LastFlags           = 0xab0b88,     // mesh texture setter's cache
    LastTextureEffect   = 0xab0b8c,
    LastState           = 0xb1db70,     // texture state and the five textures
    LastTextures        = 0xb1db78,
    CullReflect         = 0xa650e4,
    CullView            = 0xa6523c,
    IdentityMatrix      = 0xb1def0,
    CountEffects        = 0xb1df64,     // statistics counters
    CountTransforms     = 0xb1df54,
    CountIdentity       = 0xb1df60,
    CountStreams        = 0xb1df48,
    CountIndices        = 0xb1df4c,
    CountStrips         = 0xb1df44,
    SetTransformMethod  = 0x71e660,
    LightMaterialMethod = 0x71e030
};

int s_enabled = -1;

bool enabled()
{
    if (s_enabled < 0)
        s_enabled = pf::Config::getBool("native_flush", true) ? 1 : 0;
    return s_enabled != 0;
}

// The light material setter natively too, unless native_material=0 (always compared in the check build).
bool materialEnabled()
{
#if NFSC_RENDER_CHECK
    return true;
#else
    static int s_material = -1;
    if (s_material < 0)
        s_material = pf::Config::getBool("native_material", true) ? 1 : 0;
    return s_material != 0;
#endif
}

// Bones, PCA weights and stencil-masked draws natively too, unless native_parts=0 (always compared in the check
// build).
bool partsEnabled()
{
#if NFSC_RENDER_CHECK
    return true;
#else
    static int s_parts = -1;
    if (s_parts < 0)
        s_parts = pf::Config::getBool("native_parts", true) ? 1 : 0;
    return s_parts != 0;
#endif
}

void add32(x86::reg32 address, x86::reg32 value)
{
    rc::st32(address, rc::ld32(address) + value);
}

const float* floats(x86::reg32 address)
{
    return reinterpret_cast<const float*>(rc::host(address));
}

// ---- Math as the guest computes it --------------------------------------------------------------------------------

// D3DXMatrixMultiply: out = a * b, single precision in its order.
void multiply(const float* a, const float* b, float* out)
{
    float r[16];
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            r[i * 4 + j] = a[i * 4 + 0] * b[j] + a[i * 4 + 1] * b[4 + j] + a[i * 4 + 2] * b[8 + j]
                         + a[i * 4 + 3] * b[12 + j];
    memcpy(out, r, sizeof(r));
}

// D3DXVec4Transform.
void transform4(const float* v, const float* m, float* out)
{
    float r[4];
    for (int j = 0; j < 4; ++j)
        r[j] = v[0] * m[j] + v[1] * m[4 + j] + v[2] * m[8 + j] + v[3] * m[12 + j];
    memcpy(out, r, sizeof(r));
}

// 0x46f760: v / |v| in double, (1, 0, 0) for a zero length.
void normalize(const float* v, float* out)
{
    const double x = v[0], y = v[1], z = v[2];
    const double length = std::sqrt(x * x + y * y + z * z);
    if (length == 0.0)
    {
        out[0] = 1.0f;
        out[1] = out[2] = 0.0f;
        return;
    }
    const double inverse = 1.0 / length;
    out[0] = float(inverse * x);
    out[1] = float(y * inverse);
    out[2] = float(z * inverse);
}

// 0x46fa90: a x b in double.
void cross(const float* a, const float* b, float* out)
{
    const double x = double(b[2]) * a[1] - double(b[1]) * a[2];
    const double y = double(b[0]) * a[2] - double(b[2]) * a[0];
    const double z = double(b[1]) * a[0] - double(a[1]) * b[0];
    out[0] = float(x);
    out[1] = float(y);
    out[2] = float(z);
}

// 0x70e5a0: inverse of a rigid transform, the negated translation times the transposed rotation.
void rigidInverse(const float* m, float* out)
{
    const float t[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, -m[12], -m[13], -m[14], 1 };
    const float r[16] = { m[0], m[4], m[8], 0, m[1], m[5], m[9], 0, m[2], m[6], m[10], 0, 0, 0, 0, 1 };
    multiply(t, r, out);
}

// ---- Sinks: what a native step does ----------------------------------------------------------------------------

// Performs the calls; during a native run the pass, state and draw calls go to the run.
struct Exec
{
    win32::WinApplication*  app;
    x86::CPU&               cpu;
    d3d9::Device*           device;
    nr::Run*                run = nullptr;

    x86::reg32 load(x86::reg32 address) { return rc::ld32(address); }
    void store(x86::reg32 address, x86::reg32 value) { rc::st32(address, value); }
    void matrix(x86::reg32 effect, x86::reg32 handle, const float* m, uint16_t = 0xffff)
    {
        direct::setMatrix(app, cpu, effect, handle, m);
    }
    void matrices(x86::reg32 effect, x86::reg32 handle, const float* m, uint32_t count)
    {
        direct::setMatrixArray(app, cpu, effect, handle, m, count);
    }
    void vectors(x86::reg32 effect, x86::reg32 handle, const float* v, uint32_t count, uint16_t = 0xffff)
    {
        direct::setVectorArray(app, cpu, effect, handle, v, count);
    }
    void technique(x86::reg32 effect, x86::reg32 handle) { direct::setTechnique(app, cpu, effect, handle); }
    uint32_t begin(x86::reg32 effect) { return run ? run->table()->passes : direct::begin(app, cpu, effect, 0); }
    void setPasses(x86::reg32 e, uint32_t passes) { rc::st32(e + 0x14, passes); }
    void beginPass(x86::reg32 effect, uint32_t pass)
    {
        if (run)
            run->beginPass();
        else
            direct::beginPass(app, cpu, effect, pass);
    }
    void endPass(x86::reg32 effect)
    {
        if (!run)
            direct::endPass(app, cpu, effect);
    }
    void end(x86::reg32 effect)
    {
        if (run)
            run->end(app, cpu);
        else
            direct::end(app, cpu, effect);
    }
    void declaration(x86::reg32 declaration)
    {
        device->SetVertexDeclaration(app, cpu, static_cast<d3d9::VertexDeclaration*>(d3d9::lookup(declaration)));
    }
    // A guest function by address on a copy of the caller's state (thiscall, no arguments).
    void guestAt(x86::reg32 address, x86::reg32 self)
    {
        x86::CPU copy = cpu;
        copy.ecx = self;
        native::callGuest(app, copy, address);
        if (copy.terminate)
            cpu.terminate = true;
    }
    void scalar(x86::reg32 effect, x86::reg32 handle, float value) { direct::setFloat(app, cpu, effect, handle, value); }
    void texture(x86::reg32 effect, x86::reg32 handle, x86::reg32 texture)
    {
        direct::setTexture(app, cpu, effect, handle, texture);
    }
    void commit(x86::reg32 effect)
    {
        if (run)
            run->commit();
        else
            direct::commitChanges(app, cpu, effect);
    }
    void renderState(uint32_t state, uint32_t value)
    {
        if (run)
            run->renderState(state, value);
        else
            device->SetRenderState(app, cpu, state, value);
    }
    void samplerState(uint32_t sampler, uint32_t type, uint32_t value)
    {
        if (run)
            run->samplerState(sampler, type, value);
        else
            device->SetSamplerState(app, cpu, sampler, type, value);
    }
    void transform(uint32_t state, const float* m)
    {
        device->SetTransform(app, cpu, state, reinterpret_cast<const d3d9::D3DMATRIX*>(m));
    }
    void drawIndexed(uint32_t type, uint32_t vertices, uint32_t start, uint32_t count)
    {
        if (run)
            run->drawIndexed(app, cpu, type, vertices, start, count);
        else
            device->DrawIndexedPrimitive(app, cpu, type, 0, 0, vertices, start, count);
    }
    // A translated function on a copy of the caller's state: this in ecx, arguments on the guest stack.
    void guest(GuestFunction function, x86::reg32 self, std::initializer_list<x86::reg32> args)
    {
        x86::CPU copy = cpu;
        copy.esp -= 4 * x86::reg32(args.size() + 1);
        x86::reg32 at = copy.esp + 4;
        for (x86::reg32 a : args)
        {
            rc::st32(at, a);
            at += 4;
        }
        copy.ecx = self;
        function(app, copy);
        if (copy.terminate)
            cpu.terminate = true;
    }
};

#if NFSC_RENDER_CHECK
// Records the calls (objects as host addresses, as the effect and device trace them) and the stores.
struct Plan
{
    std::vector<direct::TraceCall>                      calls;
    std::vector<std::pair<x86::reg32, x86::reg32>>      stores;

    x86::reg32 load(x86::reg32 address)
    {
        for (size_t i = stores.size(); i-- > 0;)
        {
            if (stores[i].first == address)
                return stores[i].second;
        }
        return rc::ld32(address);
    }
    void store(x86::reg32 address, x86::reg32 value) { stores.emplace_back(address, value); }
    void add(uint8_t kind, std::initializer_list<uint32_t> args, const float* values = nullptr, uint32_t count = 0,
             uint16_t mask = 0xffff)
    {
        direct::TraceCall call = {};
        call.kind = kind;
        uint32_t i = 0;
        for (uint32_t a : args)
            call.args[i++] = a;
        direct::fold(call, values, values ? count : 0);
        call.mask = mask;
        calls.push_back(call);
    }
    static uint32_t object(x86::reg32 guest) { return uint32_t(uintptr_t(d3d9::lookup(guest))); }
    void matrix(x86::reg32 effect, x86::reg32 handle, const float* m, uint16_t mask = 0xffff)
    {
        add(direct::TraceEffectMatrix, {object(effect), handle}, m, 16, mask);
    }
    void matrices(x86::reg32 effect, x86::reg32 handle, const float* m, uint32_t count)
    {
        add(direct::TraceEffectMatrices, {object(effect), handle, count}, m, 16 * count);
    }
    void vectors(x86::reg32 effect, x86::reg32 handle, const float* v, uint32_t count, uint16_t mask = 0xffff)
    {
        add(direct::TraceEffectVectors, {object(effect), handle, count}, v, 4 * count, mask);
    }
    void technique(x86::reg32 effect, x86::reg32 handle) { add(direct::TraceEffectTechnique, {object(effect), handle}); }
    uint32_t begin(x86::reg32 effect)
    {
        add(direct::TraceEffectBegin, {object(effect), 0});
        return 0;
    }
    void setPasses(x86::reg32, uint32_t) {}
    void beginPass(x86::reg32 effect, uint32_t pass) { add(direct::TraceEffectBeginPass, {object(effect), pass}); }
    void endPass(x86::reg32 effect) { add(direct::TraceEffectEndPass, {object(effect)}); }
    void end(x86::reg32 effect) { add(direct::TraceEffectEnd, {object(effect)}); }
    void declaration(x86::reg32 declaration) { add(direct::TraceDeclaration, {object(declaration)}); }
    void guestAt(x86::reg32, x86::reg32) { add(direct::TraceWildcard, {}); }
    void scalar(x86::reg32 effect, x86::reg32 handle, float value)
    {
        add(direct::TraceEffectFloat, {object(effect), handle}, &value, 1);
    }
    void texture(x86::reg32 effect, x86::reg32 handle, x86::reg32 texture)
    {
        add(direct::TraceEffectTexture, {object(effect), handle, texture});
    }
    void commit(x86::reg32 effect) { add(direct::TraceEffectCommit, {object(effect)}); }
    void renderState(uint32_t state, uint32_t value) { add(direct::TraceRenderState, {state, value}); }
    void samplerState(uint32_t sampler, uint32_t type, uint32_t value)
    {
        add(direct::TraceSamplerState, {sampler, type, value});
    }
    void transform(uint32_t state, const float* m) { add(direct::TraceTransform, {state}, m, 16); }
    void drawIndexed(uint32_t type, uint32_t vertices, uint32_t start, uint32_t count)
    {
        add(direct::TraceDrawIndexed, {type, 0, 0, vertices, start, count});
    }
    void guest(GuestFunction, x86::reg32, std::initializer_list<x86::reg32>) { add(direct::TraceWildcard, {}); }
};
#endif

// ---- Per-record steps ----------------------------------------------------------------------------------------------

// The fields of an effect object the per-record steps read: the D3DX effect and parameter handles.
struct EffectFields
{
    x86::reg32  e = 0;
    x86::reg32  fx, worldTranspose, worldView, world, worldViewProj, eye, light, headlight;
    x86::reg32  ambient;
    x86::reg32  transformMethod, materialMethod;   // vtable +8 and +4
    bool        fixed;
    bool        headlightOn;                        // the headlight handle set and [[e + 0x1754] + 0x14] == 1
};

// The headlight switch of the effect's settings is read with the fields, unless flush_headlight=0 (then at every
// transform).
bool headlightWithFields()
{
    static int s_headlight = -1;
    if (s_headlight < 0)
        s_headlight = pf::Config::getBool("flush_headlight", true) ? 1 : 0;
    return s_headlight != 0;
}

bool headlightSet(x86::reg32 e, x86::reg32 headlight)
{
    return headlight && rc::ld32(rc::ld32(e + 0x1754) + 0x14) == 1;
}

void readFields(EffectFields& f, x86::reg32 e)
{
    f.e = e;
    f.transformMethod = rc::ld32(rc::ld32(e) + 8);
    f.materialMethod = rc::ld32(rc::ld32(e) + 4);
    f.fx = rc::ld32(e + 0x44);
    f.fixed = rc::ld8(e + 0x40) == 0;
    f.worldTranspose = rc::ld32(e + 0x1740);
    f.worldView = rc::ld32(e + 0x4f8);
    f.world = rc::ld32(e + 0x4d0);
    f.worldViewProj = rc::ld32(e + 0x1588);
    f.eye = rc::ld32(e + 0xd40);
    f.light = rc::ld32(e + 0xd68);
    f.headlight = rc::ld32(e + 0xc78);
    f.headlightOn = headlightWithFields() && headlightSet(e, f.headlight);
    f.ambient = rc::ld32(e + 0x778);
}

// The fields are read once per effect run, unless flush_fields=0 (then once per record).
bool fieldsPerRun()
{
    static int s_fields = -1;
    if (s_fields < 0)
        s_fields = pf::Config::getBool("flush_fields", true) ? 1 : 0;
    return s_fields != 0;
}

// The guest data of the records ahead is requested while the current one draws, unless flush_prefetch=0.
bool prefetchAhead()
{
    static int s_prefetch = -1;
    if (s_prefetch < 0)
        s_prefetch = pf::Config::getBool("flush_prefetch", true) ? 1 : 0;
    return s_prefetch != 0;
}

void prefetch(x86::reg32 address)
{
    __builtin_prefetch(rc::host(address));
}

// The record at sort position i.
x86::reg32 recordAt(x86::reg32 keys, x86::reg32 i)
{
    return Records + rc::ld32(keys + 8 * i) * 0x68;
}

// Requests, while record i draws: record i + 3; the matrix, mesh, solid and first texture of record i + 2 (prefetched
// one record before); the solid's mesh info and the vertex buffer object of record i + 1.
void prefetchRecords(x86::reg32 keys, x86::reg32 i, x86::reg32 count)
{
    if (i + 3 < count)
    {
        const x86::reg32 r = recordAt(keys, i + 3);
        prefetch(r);
        prefetch(r + 0x67);
    }
    if (i + 2 < count)
    {
        const x86::reg32 r = recordAt(keys, i + 2);
        const x86::reg32 matrix = rc::ld32(r + 0x38);
        prefetch(matrix);
        prefetch(matrix + 0x3f);
        const x86::reg32 mesh = rc::ld32(r + 0x1c);
        prefetch(mesh + 0x40);
        prefetch(mesh + 0x7c);
        prefetch(rc::ld32(r + 0x24));
        prefetch(rc::ld32(r + 0x40) + 0x34);
    }
    if (i + 1 < count)
    {
        const x86::reg32 r = recordAt(keys, i + 1);
        if (rc::ld32(r + 0x20) == 0)
        {
            prefetch(rc::ld32(rc::ld32(r + 0x24)) + 0x28);
            prefetch(rc::ld32(rc::ld32(r + 0x1c) + 0x74) + 4);
        }
    }
}

// SetTransform (0x71e660, thiscall on the effect object e): view and projection when the view's frame or the effect
// changed, then the world-derived parameters of the effect.
template< typename S >
void setTransform(S& s, const EffectFields& f, x86::reg32 matrix, x86::reg32 view, bool flagged)
{
    const x86::reg32 e = f.e;
    const x86::reg32 current = s.load(CurrentEffect);
    const x86::reg32 frame = s.load(view);
    if (matrix == s.load(LastMatrix) && current == s.load(LastEffect) && frame == s.load(LastFrame))
        return;
    const x86::reg32 viewMatrix = view + 0x100;
    const x86::reg32 projection = view + 0xc0;
    const x86::reg32 combined = flagged ? view + 0x190 : view + 0x140;
    const bool fixed = f.fixed;
    if (frame != s.load(LastFrame))
    {
        if (fixed)
        {
            s.transform(2, floats(viewMatrix));
            s.transform(3, floats(projection));
        }
        s.store(LastFrame, frame);
    }
    else if (current != s.load(LastEffect) && fixed)
    {
        s.transform(2, floats(viewMatrix));
        s.transform(3, floats(projection));
    }
    s.store(LastEffect, current);
    s.store(LastMatrix, matrix);
    const x86::reg32 effect = f.fx;
    float m[16];
    memcpy(m, floats(matrix), sizeof(m));
    if (const x86::reg32 handle = f.worldTranspose)
    {
        // The transpose of the rows normalised and their cross product; the guest's 4th row and column are
        // uninitialised stack, 0 here and not compared.
        float n0[3], n1[3], c[3];
        normalize(m, n0);
        normalize(m + 4, n1);
        cross(n0, n1, c);
        float t[16] = {};
        uint16_t mask = 0;
        for (int j = 0; j < 3; ++j)
        {
            t[j * 4 + 0] = n0[j];
            t[j * 4 + 1] = n1[j];
            t[j * 4 + 2] = c[j];
            mask |= uint16_t(7u << (j * 4));
        }
        s.matrix(effect, handle, t, mask);
    }
    if (const x86::reg32 handle = f.worldView)
    {
        float wv[16];
        multiply(m, floats(viewMatrix), wv);
        s.matrix(effect, handle, wv);
    }
    if (const x86::reg32 handle = f.world)
        s.matrix(effect, handle, m);
    if (fixed)
    {
        s.transform(0x100, m);
    }
    else
    {
        float wvp[16];
        multiply(m, floats(combined), wvp);
        s.matrix(effect, f.worldViewProj, wvp);
    }
    const x86::reg32 eye = f.eye;
    const x86::reg32 light = f.light;
    if (eye || light)
    {
        float inverse[16];
        rigidInverse(m, inverse);
        if (eye)
        {
            const float position[4] = { floats(view + 0x40)[0], floats(view + 0x40)[1], floats(view + 0x40)[2], 1.0f };
            float local[4];
            transform4(position, inverse, local);
            s.vectors(effect, eye, local, 1);
        }
        if (light)
        {
            float local[4];
            transform4(floats(rc::ld32(LightVector) + 0x160), inverse, local);
            float unit[3];
            normalize(local, unit);
            const float vector[4] = { unit[0], unit[1], unit[2], local[3] };
            s.vectors(effect, light, vector, 1);
        }
    }
    const x86::reg32 headlight = f.headlight;
    if (headlightWithFields() ? f.headlightOn : headlightSet(e, headlight))
    {
        static const float bias[16] = { 0.5f, 0, 0, 0, 0, 0.5f, 0, 0, 0, 0, 0.5f, 0, 0.5f, 0.5f, 0.5f, 1 };
        float wvp[16], out[16];
        multiply(m, floats(ViewTable + 9 * 0x1e0 + 0x140), wvp);
        multiply(wvp, bias, out);
        s.matrix(effect, headlight, out);
    }
}

// Light context (0x71dfa0): its harmonic coefficients and colour and direction matrices, once per context.
template< typename S >
void setLightContext(S& s, x86::reg32 e, x86::reg32 light)
{
    if (!light || rc::ld32(light) != 0 || s.load(e + 0x1784) == light)
        return;
    s.store(e + 0x1784, light);
    const x86::reg32 effect = rc::ld32(e + 0x44);
    if (const x86::reg32 handle = rc::ld32(e + 0xc28))
        s.vectors(effect, handle, floats(light + 0x130), 10);
    if (const x86::reg32 handle = rc::ld32(e + 0xd90))
        s.matrix(effect, handle, floats(light + 0x10));
    if (const x86::reg32 handle = rc::ld32(e + 0xdb8))
        s.matrix(effect, handle, floats(light + 0x50));
}

// Light material (0x71e030): its colours, specular and environment terms and powers as vectors, once per
// material; products and differences in double, as the guest computes them.
template< typename S >
void setLightMaterial(S& s, const EffectFields& fields, x86::reg32 p)
{
    const x86::reg32 e = fields.e;
    if (!p || !fields.ambient || s.load(e + 0x1780) == p)
        return;
    s.store(e + 0x1780, p);
    const auto f = [p](x86::reg32 offset) { return rc::ldf32(p + offset); };
    const auto d = [p](x86::reg32 offset) { return double(rc::ldf32(p + offset)); };
    const double c = double(rc::ldf32(0xa6b980));
    const float ambient[4] = { float(d(0x68) * d(0x64)), float(d(0x6c) * d(0x64)), float(d(0x70) * d(0x64)), f(0x84) };
    const float range[4] = { float(d(0x78) * d(0x74) - double(ambient[0])),
                             float(d(0x7c) * d(0x74) - double(ambient[1])),
                             float(d(0x80) * d(0x74) - double(ambient[2])), float(d(0x88) - double(ambient[3])) };
    const float specular[4] = { float(c * d(0x9c) * d(0x98)), float(c * d(0xa0) * d(0x98)),
                                float(c * d(0xa4) * d(0x98)), 0.0f };
    const float specularRange[4] = { float(c * d(0xac) * d(0xa8) - double(specular[0])),
                                     float(c * d(0xb0) * d(0xa8) - double(specular[1])),
                                     float(c * d(0xb4) * d(0xa8) - double(specular[2])), 0.0f };
    const float envmap[4] = { float(d(0xc8) * d(0xc4) * 0.5), float(d(0xcc) * d(0xc4) * 0.5),
                              float(d(0xd0) * d(0xc4) * 0.5), 0.0f };
    const float envmapRange[4] = { float(d(0xd8) * d(0xd4) * 0.5 - double(envmap[0])),
                                   float(d(0xdc) * d(0xd4) * 0.5 - double(envmap[1])),
                                   float(d(0xe0) * d(0xd4) * 0.5 - double(envmap[2])), 0.0f };
    const float misc[4] = { f(0x54), f(0x8c), f(0xb8), 0.0f };
    const double lowest = 0.001953125;
    const double power = d(0x58) > lowest ? d(0x58) : lowest;
    const double specularPower = d(0xbc) > lowest ? d(0xbc) : lowest;
    const float powers[4] = { f(0x58), f(0xbc), float(1.0 / power), float(1.0 / specularPower) };
    const float clamps[4] = { f(0x5c), f(0x90), f(0xe4), float(d(0xe8) - d(0xe4)) };
    const float scales[4] = { f(0x60), f(0x94), f(0xc0), 0.0f };
    const x86::reg32 effect = rc::ld32(e + 0x44);
    s.vectors(effect, rc::ld32(e + 0x778), ambient, 1);
    s.vectors(effect, rc::ld32(e + 0x7a0), range, 1);
    s.vectors(effect, rc::ld32(e + 0x1308), specular, 1);
    s.vectors(effect, rc::ld32(e + 0x1358), specularRange, 1);
    s.vectors(effect, rc::ld32(e + 0x8b8), envmap, 1);
    s.vectors(effect, rc::ld32(e + 0x890), envmapRange, 1);
    if (const x86::reg32 handle = rc::ld32(e + 0x1330))
        s.scalar(effect, handle, f(0x8c));
    if (const x86::reg32 handle = rc::ld32(e + 0x390))
        s.vectors(effect, handle, misc, 1);
    if (const x86::reg32 handle = rc::ld32(e + 0x3b8))
        s.vectors(effect, handle, powers, 1);
    if (const x86::reg32 handle = rc::ld32(e + 0x3e0))
        s.vectors(effect, handle, clamps, 1);
    if (const x86::reg32 handle = rc::ld32(e + 0x408))
        s.vectors(effect, handle, scales, 1);
}

// Base texture and its states (0x71dce0) from the state word at state.
template< typename S >
void setTextureState(S& s, x86::reg32 e, x86::reg32 texture, x86::reg32 state)
{
    s.texture(rc::ld32(e + 0x44), rc::ld32(e + 0x660), texture);
    const x86::reg32 w = rc::ld32(state);
    s.samplerState(0, 1, w >> 12 & 3);
    s.samplerState(0, 2, w >> 14 & 3);
    s.renderState(0xf, w >> 2 & 1);
    s.renderState(0x18, (w >> 24 & 0xf) << 4);
    s.renderState(0x19, 5);
    s.renderState(0x1b, w >> 3 & 1);
    s.renderState(0x13, w >> 4 & 0xf);
    s.renderState(0x14, w >> 8 & 0xf);
    s.renderState(0xe, w & 1);
    s.renderState(0xa8, ((w & 0x400000) | 0x380000) >> 19);
}

// Texture coordinate animation (0x71deb0): none or a fixed scroll here, the other kinds in translated code.
template< typename S >
void setTextureAnimation(S& s, x86::reg32 e, x86::reg32 info)
{
    const x86::reg32 handle = rc::ld32(e + 0x430);
    if (!handle)
        return;
    const x86::reg32 effect = rc::ld32(e + 0x44);
    const uint8_t kind = rc::ld8(info + 0x36);
    if (kind == 0)
    {
        static const float zero[4] = {};
        if (s.load(e + 0x1778) != 1)
            s.vectors(effect, handle, zero, 1);
        s.store(e + 0x1778, 1);
        return;
    }
    if (kind == 3)
    {
        const double step = -0.0009765625;
        const float offset[4] = { float(double(int16_t(rc::ld16(info + 0x42))) * step),
                                  float(double(int16_t(rc::ld16(info + 0x44))) * step), 0.0f, 0.0f };
        s.vectors(effect, handle, offset, 1);
        s.store(e + 0x1778, 0);
        return;
    }
    s.guest(sub_71deb0, e, {info});
}

// Mesh textures and states (0x7234a0) when the record's texture state, textures, flags or effect changed.
template< typename S >
void setMeshTextures(S& s, x86::reg32 e, x86::reg32 record, x86::reg32 flags)
{
    const x86::reg32 lastFlags = s.load(LastFlags);
    const x86::reg32 lastEffect = s.load(LastTextureEffect);
    x86::reg32 w[7];
    for (int k = 0; k < 7; ++k)
        w[k] = rc::ld32(record + 4 * k);
    x86::reg32 changed = (lastFlags - flags) | (lastEffect - e);
    for (int k = 0; k < 7; ++k)
    {
        if (k == 1)
            continue;
        changed |= s.load(LastState + 4 * k) - w[k];
        s.store(LastState + 4 * k, w[k]);
    }
    s.store(LastFlags, flags);
    s.store(LastTextureEffect, e);
    if (!changed)
        return;
    x86::reg32 cull = 1;
    if ((w[0] & 2) && rc::ld8(CullReflect))
        cull = ((flags >> 18 & 1) ^ (rc::ld32(CullView) == 2 ? 1u : 0u)) ? 2 : 3;
    s.renderState(0x16, cull);
    setTextureState(s, e, w[2], record);
    if (w[0] & 0x40000)
    {
        const x86::reg32 effect = rc::ld32(e + 0x44);
        static const x86::reg32 slots[4] = { 0xf70, 0xca0, 0x12e0, 0xf98 };
        for (int k = 0; k < 4; ++k)
        {
            const x86::reg32 handle = rc::ld32(e + slots[k]);
            if (w[3 + k] && handle)
                s.texture(effect, handle, w[3 + k]);
        }
    }
    setTextureAnimation(s, e, w[1]);
    const x86::reg32 layered = w[0] >> 17 & 1;
    if (s.load(e + 0x177c) != layered)
    {
        s.store(e + 0x177c, layered);
        if (const x86::reg32 handle = rc::ld32(e + 0xb60))
            s.scalar(rc::ld32(e + 0x44), handle, layered ? 0.0f : 1.0f);
    }
}

// Alpha test states (0x7140f0): enable, and with it the reference and function.
template< typename S >
void setAlphaTest(S& s, uint32_t enable, uint32_t reference, uint32_t function)
{
    s.renderState(0xf, enable);
    if (enable)
    {
        s.renderState(0x18, reference);
        s.renderState(0x19, function);
    }
}

// The indexed draw (0x71ecb0). Masked meshes (state bit 21) with alpha mode 3: an opaque pass where the alpha test
// passes, then a blended pass without depth writes where it fails; other modes: one blended pass without the test.
template< typename S >
void draw(S& s, x86::reg32 e, x86::reg32 vertices, x86::reg32 start, x86::reg32 count, x86::reg32 record)
{
    const x86::reg32 w = rc::ld32(record);
    if (w & 0x200000)
    {
        if (!partsEnabled())
        {
            s.guest(sub_71ecb0, e, {vertices, start, count, record});
            return;
        }
        const x86::reg32 effect = rc::ld32(e + 0x44);
        if (rc::ld32(0xa65370) == 3)
        {
            const x86::reg32 reference = (w >> 24 & 0xf) << 4;
            s.renderState(0x1b, 0);
            setAlphaTest(s, 1, reference, 7);
            s.renderState(0xe, 1);
            s.commit(effect);
            s.drawIndexed(4, vertices, start, count);
            s.renderState(0x1b, 1);
            s.renderState(0x13, 5);
            s.renderState(0x14, 6);
            setAlphaTest(s, 1, reference, 2);
            s.renderState(0xe, 0);
            s.commit(effect);
            s.drawIndexed(4, vertices, start, count);
            return;
        }
        setAlphaTest(s, 0, 0, 5);
        s.renderState(0x1b, 1);
        s.renderState(0x13, 5);
        s.renderState(0x14, 6);
        s.renderState(0xe, 0);
    }
    s.commit(rc::ld32(e + 0x44));
    s.drawIndexed(4, vertices, start, count);
}

// Bone matrices (0x714cc0): the 16 palette matrices the mesh's bone bytes name, on the current effect.
template< typename S >
void setBones(S& s, x86::reg32 palette, x86::reg32 mesh)
{
    float bones[16 * 16];
    for (x86::reg32 k = 0; k < 16; ++k)
        memcpy(bones + 16 * k, floats(palette + 64 * x86::reg32(rc::ld8(mesh + 0x1e + k))), 64);
    const x86::reg32 e = rc::ld32(CurrentEffect);
    s.matrices(rc::ld32(e + 0x44), rc::ld32(e + 0x1d8), bones, 16);
}

// Interpolated PCA weights of one channel (0x70d650) into the animation's output buffer; returns its address.
template< typename S >
x86::reg32 pcaChannel(S& s, x86::reg32 animation, uint32_t channel, x86::reg32 frame0, x86::reg32 frame1, float t)
{
    const float rest = float(1.0 - double(t));
    const x86::reg32 entry = animation + 12 * channel;
    const x86::reg32 first = rc::ld16(entry + 0xc);
    const x86::reg32 out = rc::ld32(animation + 0x84) + 4 * x86::reg32(rc::ld16(entry + 0xa));
    const x86::reg32 bias = rc::ld32(animation + 0x7c) + 4 * first;
    const x86::reg32 scale = rc::ld32(animation + 0x80) + 4 * first;
    const x86::reg32 stride = rc::ld16(animation + 2);
    const x86::reg32 data = rc::ld32(animation + 0x88);
    const x86::reg32 p0 = data + 2 * (stride * frame0 + first);
    const x86::reg32 p1 = data + 2 * (stride * frame1 + first);
    const uint32_t count = rc::ld8(entry + 9);
    for (uint32_t i = 0; i < count; ++i)
    {
        const double a = double(rc::ld16(p0 + 2 * i)) * double(rest);
        const double b = double(rc::ld16(p1 + 2 * i)) * double(t);
        const double v = (a + b) * double(rc::ldf32(scale + 4 * i)) * 0x1.0001p-16
                       + double(rc::ldf32(bias + 4 * i));
        const float f = float(v);
        x86::reg32 bits;
        memcpy(&bits, &f, 4);
        s.store(out + 4 * i, bits);
    }
    return out;
}

// PCA blend weights (0x71e540): interpolated between two frames of an animation, or one frame of the static data.
template< typename S >
void setPca(S& s, x86::reg32 e, x86::reg32 pca)
{
    const x86::reg32 effect = rc::ld32(e + 0x44);
    const auto handle = [e](x86::reg32 slot) { return rc::ld32(e + 0x70 + 0x28 * slot); };
    const x86::reg32 animation = rc::ld32(pca + 0xc);
    if (animation)
    {
        s.vectors(effect, handle(rc::ld32(animation + 0x74)), floats(rc::ld32(animation + 0x78)), 1);
        const uint32_t channels = rc::ld16(animation + 6);
        for (uint32_t i = 0; i < channels; ++i)
        {
            const x86::reg32 entry = animation + 0x10 + 12 * i;
            const x86::reg32 out = pcaChannel(s, animation, rc::ld8(entry - 8), rc::ld32(pca), rc::ld32(pca + 4),
                                              rc::ldf32(pca + 8));
            const uint32_t vectors = rc::ld8(entry - 7) >> 2;
            // What the effect reads: the buffer with this step's stores.
            std::vector<float> values(4 * vectors);
            for (uint32_t k = 0; k < 4 * vectors; ++k)
            {
                const x86::reg32 bits = s.load(out + 4 * k);
                memcpy(&values[k], &bits, 4);
            }
            s.vectors(effect, handle(rc::ld32(entry)), values.data(), vectors);
        }
        return;
    }
    const x86::reg32 data = rc::ld32(pca + 0x10);
    const uint32_t channels = rc::ld16(data + 0x36);
    for (uint32_t i = 0; i < channels; ++i)
    {
        const x86::reg32 entry = data + 0x40 + 12 * i;
        const x86::reg32 offset = rc::ld16(data + 12 * (x86::reg32(rc::ld8(entry - 8)) + 5))
                                + x86::reg32(rc::ld16(data + 0x32)) * rc::ld32(pca);
        s.vectors(effect, handle(rc::ld32(entry)), floats(rc::ld32(data + 0xa4) + 4 * offset),
                  rc::ld8(entry - 7) >> 2);
    }
}

// ---- Effect switches ---------------------------------------------------------------------------------------------

// Effect switches natively too, unless native_switch=0 (always compared in the check build).
bool switchEnabled()
{
#if NFSC_RENDER_CHECK
    return true;
#else
    static int s_switch = -1;
    if (s_switch < 0)
        s_switch = pf::Config::getBool("native_switch", true) ? 1 : 0;
    return s_switch != 0;
#endif
}

// Handle of parameter slot k of the effect object e.
x86::reg32 slotHandle(x86::reg32 e, x86::reg32 k)
{
    return rc::ld32(e + 0x70 + 0x28 * k);
}

float wordFloat(x86::reg32 bits)
{
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

// The 4-float vector at a guest address as this step sees it, its own stores included.
template< typename S >
void loadVector(S& s, x86::reg32 address, float out[4])
{
    for (int k = 0; k < 4; ++k)
        out[k] = wordFloat(s.load(address + 4 * k));
}

// The scene vector of slot 0x8b the world-type Start methods set (0x748a70's body).
template< typename S >
void setSceneVector(S& s, x86::reg32 e)
{
    s.store(0xa6c208, rc::ld32(0xa650e0));
    s.store(0xa6c210, 0x3ecccccd);
    float v[4];
    loadVector(s, rc::ld32(0xa99bbc) == 6 ? 0xa6c204 : 0xa6c214, v);
    s.vectors(rc::ld32(e + 0x44), slotHandle(e, 0x8b), v, 1);
}

// The environment textures and reflection of the car Start methods (0x73e180, 0x73e200).
template< typename S >
void setCarEnvironment(S& s, x86::reg32 e)
{
    const x86::reg32 fx = rc::ld32(e + 0x44);
    const x86::reg32 mode = rc::ld32(0xa99bbc);
    if (mode == 6)
    {
        s.texture(fx, slotHandle(e, 0x38), rc::ld32(0xab0920));
        s.scalar(fx, slotHandle(e, 0x37), rc::ldf32(0xa6c228));
    }
    else if (mode == 3)
    {
        s.texture(fx, slotHandle(e, 0x38), rc::ld32(0xab0954));
        s.scalar(fx, slotHandle(e, 0x37), rc::ldf32(0xb43030));
    }
}

// An effect class's Start method natively; false for the ones still translated.
template< typename S >
bool startMethod(S& s, x86::reg32 e, x86::reg32 method)
{
    const x86::reg32 fx = rc::ld32(e + 0x44);
    switch (method)
    {
    case 0x749790:      // world, world min, rain drop, particles
        if (slotHandle(e, 0x78))
            s.scalar(fx, slotHandle(e, 0x78), rc::ldf32(0xa6c224));
        if (slotHandle(e, 0x8b))
            setSceneVector(s, e);
        return true;
    case 0x748a70:      // world normal map
        setSceneVector(s, e);
        return true;
    case 0x73e150:      // world bone
        s.texture(fx, slotHandle(e, 0x38), rc::ld32(0xab0920));
        return true;
    case 0x73e180:      // car
        if (const x86::reg32 t = rc::ld32(0xab09e8))
            s.texture(fx, slotHandle(e, 0x84), t);
        setCarEnvironment(s, e);
        return true;
    case 0x73e200:      // car normal map
        setCarEnvironment(s, e);
        if (const x86::reg32 t = rc::ld32(0xab09e8))
            s.texture(fx, slotHandle(e, 0x84), t);
        return true;
    case 0x7497f0:      // sky: cloud scroll once per frame, sun position (its z and w are uninitialised stack)
    {
        s.texture(fx, slotHandle(e, 0x59), rc::ld32(e + 0x1788));
        const x86::reg32 frame = rc::ld32(0xa62f04);
        if (s.load(0xb4cfb4) != frame)
        {
            s.store(0xb4cfb4, frame);
            const float scroll = float(double(rc::ldf32(rc::ld32(0xb77f34))) * 0.5 * double(rc::ldf32(0xa996f8))
                                       * double(rc::ldf32(0x9ea518)) + double(wordFloat(s.load(0xb4cfb0))));
            x86::reg32 bits;
            memcpy(&bits, &scroll, 4);
            s.store(0xb4cfb0, bits);
        }
        s.scalar(fx, slotHandle(e, 0x2b), wordFloat(s.load(0xb4cfb0)));
        const x86::reg32 sun = rc::ld32(0xb4295c);
        const float position[4] = { rc::ldf32(sun + 0x20), rc::ldf32(sun + 0x24), 0.0f, 0.0f };
        s.vectors(fx, slotHandle(e, 0x48), position, 1, 0x3);
        setSceneVector(s, e);
        return true;
    }
    case 0x748ae0:      // glass
    {
        const x86::reg32 index = rc::ld32(0xb42ea8);
        s.texture(fx, slotHandle(e, 0x38), index < rc::ld32(0xab09e4) ? rc::ld32(0xab09d0 + 4 * index)
                                                                      : rc::ld32(0xab09d0));
        s.texture(fx, slotHandle(e, 0x88), rc::ld32(rc::ld32(rc::ld32(0xb4303c)) + 0x10));
        setSceneVector(s, e);
        return true;
    }
    default:
        return rc::ld8(method) == 0xc3;     // a bare ret
    }
}

// The start of an effect run (0x723600 with no technique name): technique, Start method, declaration, Begin and
// the first pass.
template< typename S >
void startEffect(S& s, win32::WinApplication* app, x86::CPU& cpu, x86::reg32 e, uint8_t low)
{
    const x86::reg32 fx = rc::ld32(e + 0x44);
    const x86::reg32 lowLod = rc::ld32(e + 0x1774);
    const x86::reg32 index = (rc::ld32(0xab0b98) == 1 || low) && lowLod != 0xffffffffu
                           ? lowLod : rc::ld32(rc::ld32(e + 0x1754) + 0x10);
    s.technique(fx, direct::getTechnique(app, cpu, fx, index));
    s.store(CurrentEffect, e);
    const x86::reg32 method = rc::ld32(rc::ld32(e) + 0x10);
    if (!startMethod(s, e, method))
        s.guestAt(method, e);
    s.declaration(rc::ld32(e + 0x48));
    s.setPasses(e, s.begin(fx));
    s.beginPass(fx, 0);
}

// The end of an effect run: EndPass, the class's End method (a bare ret for every class), End.
template< typename S >
void finishEffect(S& s, x86::reg32 e)
{
    const x86::reg32 fx = rc::ld32(e + 0x44);
    s.endPass(fx);
    const x86::reg32 method = rc::ld32(rc::ld32(e) + 0x14);
    if (rc::ld8(method) != 0xc3)
        s.guestAt(method, e);
    s.end(fx);
    s.store(CurrentEffect, 0);
}

// ---- Native runs -------------------------------------------------------------------------------------------------

#if !NFSC_RENDER_CHECK
// Effect runs drawn by the native renderer (finished draws, no effect or device layer): native_uniforms=1 the world
// effects, 2 every effect with a pass table, 0 (default) none. The render check build compares every run that could.
int uniformsMode()
{
    static int s_mode = -1;
    if (s_mode < 0)
        s_mode = pf::Config::getInt("native_uniforms", 0);
    return s_mode;
}

bool uniformsFor(const nr::Pass* table)
{
    return uniformsMode() >= 2 || (uniformsMode() == 1 && table->world);
}
#endif

// A Start method startMethod performs natively.
bool startNative(x86::reg32 method)
{
    switch (method)
    {
    case 0x749790:
    case 0x748a70:
    case 0x73e150:
    case 0x73e180:
    case 0x73e200:
    case 0x7497f0:
    case 0x748ae0:
        return true;
    default:
        return rc::ld8(method) == 0xc3;
    }
}

// The pass table of the effect run starting at sort position from when every step of the run is native code
// calling only effect setters and the device state the run mirrors; null otherwise.
nr::Pass* nativeTable(win32::WinApplication* app, x86::CPU& cpu, x86::reg32 e, uint8_t low, x86::reg32 from)
{
    if (!switchEnabled())
        return nullptr;
    const x86::reg32 vtable = rc::ld32(e);
    if (!startNative(rc::ld32(vtable + 0x10)) || rc::ld8(rc::ld32(vtable + 0x14)) != 0xc3
        || rc::ld32(vtable + 8) != SetTransformMethod)
        return nullptr;
    const bool material = materialEnabled() && rc::ld32(vtable + 4) == LightMaterialMethod;
    const x86::reg32 keys = rc::ld32(SortKeys);
    for (x86::reg32 i = from; x86::sreg32(i) < x86::sreg32(rc::ld32(RecordCount)); ++i)
    {
        const x86::reg32 record = Records + rc::ld32(keys + 8 * i) * 0x68;
        if (rc::ld32(record + 0x2c) != e || rc::ld8(record + 0x64) != low)
            break;
        if (rc::ld32(record + 0x20) != 0 || (rc::ld32(record + 0x34) && !material))
            return nullptr;
        if (!partsEnabled() && ((rc::ld32(record) & 0x200000) || rc::ld32(record + 0x3c) || rc::ld32(record + 0x60)))
            return nullptr;
        if (rc::ld32(e + 0x430))
        {
            const uint8_t kind = rc::ld8(rc::ld32(record + 4) + 0x36);
            if (kind != 0 && kind != 3)
                return nullptr;
        }
    }
    const x86::reg32 fx = rc::ld32(e + 0x44);
    const x86::reg32 lowLod = rc::ld32(e + 0x1774);
    const x86::reg32 index = (rc::ld32(0xab0b98) == 1 || low) && lowLod != 0xffffffffu
                           ? lowLod : rc::ld32(rc::ld32(e + 0x1754) + 0x10);
    return nr::pass(fx, direct::getTechnique(app, cpu, fx, index), 0);
}

#if NFSC_RENDER_CHECK
nr::Run s_shadow(false);
nr::Run* s_feed = nullptr;     // the shadow run the steps' calls go to
#else
nr::Run s_run(true);
#endif

// ---- The flush ---------------------------------------------------------------------------------------------------

#if NFSC_RENDER_CHECK
struct CheckStats
{
    uint64_t steps = 0, equal = 0, differ = 0, stores = 0, storesDiffer = 0, wildcards = 0;
    uint64_t runs = 0, runEnds = 0, runEndsDiffer = 0, unmatched = 0, abandoned = 0;
    uint32_t flushes = 0, reported = 0;
};
CheckStats s_check;

// The shadow run gets a checked step's calls: states, commits and draws, its draws matched with the translated
// draws recorded since serial. Translated parts it cannot follow end the shadow.
template< typename P >
void feed(Exec& exec, x86::reg32 record, const char* step, const P& plan, uint32_t serial)
{
    if (!s_feed)
        return;
    for (const direct::TraceCall& c : plan.calls)
    {
        switch (c.kind)
        {
        case direct::TraceRenderState:
            s_feed->renderState(c.args[0], c.args[1]);
            break;
        case direct::TraceSamplerState:
            s_feed->samplerState(c.args[0], c.args[1], c.args[2]);
            break;
        case direct::TraceEffectCommit:
            s_feed->commit();
            break;
        case direct::TraceDrawIndexed:
            s_feed->drawIndexed(exec.app, exec.cpu, c.args[0], c.args[3], c.args[4], c.args[5]);
            break;
        case direct::TraceWildcard:
            ++s_check.abandoned;
            s_feed = nullptr;
            return;
        default:
            break;
        }
    }
    if (!s_feed->drawsFor(serial))
    {
        ++s_check.unmatched;
        if (s_check.reported < 32)
        {
            ++s_check.reported;
            pf::logInfo("render check: flush %u record 0x%08x %s: native draws do not match the translated draws",
                        s_check.flushes, record, step);
        }
    }
}

// After the translated End of a shadowed run: the device against what a native run leaves.
void endShadow()
{
    if (!s_feed)
        return;
    ++s_check.runEnds;
    const uint32_t differ = s_feed->checkEnd(s_check.reported < 32);
    if (differ)
    {
        ++s_check.runEndsDiffer;
        if (s_check.reported < 32)
        {
            ++s_check.reported;
            pf::logInfo("render check: flush %u: effect end: %u states or registers differ", s_check.flushes, differ);
        }
    }
    s_feed = nullptr;
}

const char* const s_kindNames[] = { "effect matrix", "effect vectors", "effect float", "effect texture",
                                    "effect commit", "render state", "sampler state", "transform", "draw indexed",
                                    "stream source", "indices", "device texture", "effect matrices",
                                    "effect technique", "effect begin", "effect begin pass", "effect end pass",
                                    "effect end", "declaration", "translated code" };

bool sameCall(const direct::TraceCall& a, const direct::TraceCall& b)
{
    if (a.kind != b.kind || memcmp(a.args, b.args, sizeof(a.args)) != 0 || a.floats != b.floats || a.hash != b.hash)
        return false;
    for (uint32_t i = 0; i < a.floats; ++i)
    {
        if (((a.mask & b.mask) >> i & 1) && memcmp(&a.value[i], &b.value[i], 4) != 0)
            return false;
    }
    return true;
}

void describe(const char* who, const direct::TraceCall* c)
{
    if (!c)
    {
        pf::logInfo("render check:   %s: none", who);
        return;
    }
    pf::logInfo("render check:   %s: %s %08x %08x %08x %08x %08x %08x floats %u hash %016llx", who,
                c->kind < sizeof(s_kindNames) / sizeof(s_kindNames[0]) ? s_kindNames[c->kind] : "?", c->args[0],
                c->args[1], c->args[2], c->args[3], c->args[4], c->args[5], c->floats,
                static_cast<unsigned long long>(c->hash));
    for (uint32_t i = 0; i < c->floats; i += 4)
        pf::logInfo("render check:     %08x %08x %08x %08x", reinterpret_cast<const uint32_t*>(c->value)[i],
                    i + 1 < c->floats ? reinterpret_cast<const uint32_t*>(c->value)[i + 1] : 0,
                    i + 2 < c->floats ? reinterpret_cast<const uint32_t*>(c->value)[i + 2] : 0,
                    i + 3 < c->floats ? reinterpret_cast<const uint32_t*>(c->value)[i + 3] : 0);
}

// The plan matches the trace, a wildcard standing for any run of calls; reports the first mismatch.
void compare(const char* step, x86::reg32 record, const Plan& plan, const std::vector<direct::TraceCall>& trace)
{
    ++s_check.steps;
    size_t wildcard = plan.calls.size();
    for (size_t i = 0; i < plan.calls.size(); ++i)
    {
        if (plan.calls[i].kind == direct::TraceWildcard)
            wildcard = i;
    }
    const size_t suffix = wildcard < plan.calls.size() ? plan.calls.size() - wildcard - 1 : 0;
    size_t bad = SIZE_MAX;
    if (wildcard == plan.calls.size() && plan.calls.size() != trace.size())
        bad = std::min(plan.calls.size(), trace.size());
    else if (wildcard < plan.calls.size() && trace.size() < wildcard + suffix)
        bad = std::min(wildcard, trace.size());
    for (size_t i = 0; bad == SIZE_MAX && i < std::min(wildcard, trace.size()); ++i)
    {
        if (!sameCall(plan.calls[i], trace[i]))
            bad = i;
    }
    for (size_t k = 0; bad == SIZE_MAX && k < suffix; ++k)
    {
        if (!sameCall(plan.calls[plan.calls.size() - 1 - k], trace[trace.size() - 1 - k]))
            bad = trace.size() - 1 - k;
    }
    for (const auto& st : plan.stores)
    {
        ++s_check.stores;
        if (rc::ld32(st.first) != st.second)
        {
            ++s_check.storesDiffer;
            if (s_check.reported < 32)
            {
                ++s_check.reported;
                pf::logInfo("render check: flush %u record 0x%08x %s: store 0x%08x native 0x%08x translated 0x%08x",
                            s_check.flushes, record, step, st.first, st.second, rc::ld32(st.first));
            }
        }
    }
    if (wildcard < plan.calls.size())
        ++s_check.wildcards;
    if (bad == SIZE_MAX)
    {
        ++s_check.equal;
        return;
    }
    ++s_check.differ;
    if (s_check.reported >= 32)
        return;
    ++s_check.reported;
    pf::logInfo("render check: flush %u record 0x%08x %s: call %u differs (native %u calls, translated %u)",
                s_check.flushes, record, step, unsigned(bad), unsigned(plan.calls.size()), unsigned(trace.size()));
    if (wildcard == plan.calls.size() && plan.calls.size() != trace.size())
    {
        // Different counts: both sequences.
        for (size_t i = 0; i < plan.calls.size() && i < 8; ++i)
            describe("native", &plan.calls[i]);
        for (size_t i = 0; i < trace.size() && i < 8; ++i)
            describe("translated", &trace[i]);
        return;
    }
    const size_t pi = bad < wildcard ? bad : plan.calls.size() - (trace.size() - bad);
    describe("native", pi < plan.calls.size() ? &plan.calls[pi] : nullptr);
    describe("translated", bad < trace.size() ? &trace[bad] : nullptr);
}

// Check build: the native step planned, then the translated function run with its calls traced and compared.
template< typename N >
void step(Exec& exec, const char* name, x86::reg32 record, N native, GuestFunction function, x86::reg32 self,
          std::initializer_list<x86::reg32> args)
{
    Plan plan;
    native(plan);
    std::vector<direct::TraceCall> trace;
    const uint32_t serial = exec.device->drawSerial();
    direct::g_trace = &trace;
    exec.guest(function, self, args);
    direct::g_trace = nullptr;
    compare(name, record, plan, trace);
    feed(exec, record, name, plan, serial);
}

// The same with the translated part given as a callable.
template< typename N, typename G >
void stepCall(Exec& exec, const char* name, x86::reg32 record, N native, G translated)
{
    Plan plan;
    native(plan);
    std::vector<direct::TraceCall> trace;
    const uint32_t serial = exec.device->drawSerial();
    direct::g_trace = &trace;
    translated();
    direct::g_trace = nullptr;
    compare(name, record, plan, trace);
    feed(exec, record, name, plan, serial);
}
#else
template< typename N >
void step(Exec& exec, const char*, x86::reg32, N native, GuestFunction, x86::reg32, std::initializer_list<x86::reg32>)
{
    native(exec);
}

template< typename N, typename G >
void stepCall(Exec& exec, const char*, x86::reg32, N native, G)
{
    native(exec);
}
#endif

// Ends the effect run of e: EndPass, the effect's own End, End, as the guest calls them.
void endEffect(win32::WinApplication* app, x86::CPU& cpu, x86::reg32 e)
{
    const x86::reg32 fx = rc::ld32(e + 0x44);
    for (int k = 0; k < 3; ++k)
    {
        x86::CPU copy = cpu;
        x86::reg32 method;
        if (k == 1)
        {
            copy.ecx = e;
            method = rc::ld32(rc::ld32(e) + 0x14);
        }
        else
        {
            copy.esp -= 4;
            rc::st32(copy.esp, fx);
            method = rc::ld32(rc::ld32(fx) + (k ? 0x10c : 0x108));
        }
        native::callGuest(app, copy, method);
        if (copy.terminate)
            cpu.terminate = true;
    }
    rc::st32(CurrentEffect, 0);
}

// Ends the effect run of e natively with native_switch=1, else as the guest calls it.
void closeEffect(Exec& exec, x86::reg32 record, x86::reg32 e)
{
    if (switchEnabled())
        stepCall(exec, "effect end", record, [&](auto& s) { finishEffect(s, e); },
                 [&] { endEffect(exec.app, exec.cpu, e); });
    else
        endEffect(exec.app, exec.cpu, e);
}

// Ends the effect run of e, and its native run or shadow.
void finishRun(Exec& exec, x86::reg32 record, x86::reg32 e)
{
    closeEffect(exec, record, e);
#if NFSC_RENDER_CHECK
    endShadow();
#endif
    exec.run = nullptr;
}

// A virtual method of an effect object: translated code by address, the known one directly.
void virtualCall(Exec& exec, x86::reg32 e, uint32_t slot, x86::reg32 expected, GuestFunction known,
                 std::initializer_list<x86::reg32> args)
{
    const x86::reg32 method = rc::ld32(rc::ld32(e) + slot);
    if (known && method == expected)
    {
        exec.guest(known, e, args);
        return;
    }
    x86::CPU copy = exec.cpu;
    copy.esp -= 4 * x86::reg32(args.size());
    x86::reg32 at = copy.esp;
    for (x86::reg32 a : args)
    {
        rc::st32(at, a);
        at += 4;
    }
    copy.ecx = e;
    native::callGuest(exec.app, copy, method);
    if (copy.terminate)
        exec.cpu.terminate = true;
}

void flush(win32::WinApplication* app, x86::CPU& cpu)
{
    const x86::reg32 total = rc::ld32(RecordCount);
    if (!total)
        return;
#if NFSC_RENDER_CHECK
    if (++s_check.flushes % 2000 == 0)
        pf::logInfo("render check: %u flushes, %llu steps: %llu equal, %llu differ, %llu with translated parts; "
                    "%llu stores, %llu differ", s_check.flushes, static_cast<unsigned long long>(s_check.steps),
                    static_cast<unsigned long long>(s_check.equal), static_cast<unsigned long long>(s_check.differ),
                    static_cast<unsigned long long>(s_check.wildcards),
                    static_cast<unsigned long long>(s_check.stores),
                    static_cast<unsigned long long>(s_check.storesDiffer));
    if (s_check.flushes % 2000 == 0)
        pf::logInfo("render check: %llu native runs shadowed, %llu ends checked, %llu differ, %llu steps with "
                    "unmatched draws, %llu runs left to translated code; %llu run starts, %llu differ from the device",
                    static_cast<unsigned long long>(s_check.runs),
                    static_cast<unsigned long long>(s_check.runEnds),
                    static_cast<unsigned long long>(s_check.runEndsDiffer),
                    static_cast<unsigned long long>(s_check.unmatched),
                    static_cast<unsigned long long>(s_check.abandoned),
                    static_cast<unsigned long long>(s_shadow.syncs()),
                    static_cast<unsigned long long>(s_shadow.syncsDiffer()));
    const d3d9::Device* checked = direct::device(rc::ld32(DevicePointer));
    if (checked && s_check.flushes % 2000 == 0)
        pf::logInfo("render check: effect constants: %llu draws compared, %llu differ",
                    static_cast<unsigned long long>(checked->constantCheckDraws()),
                    static_cast<unsigned long long>(checked->constantCheckDiffer()));
#endif
    Exec exec{app, cpu, direct::device(rc::ld32(DevicePointer))};
    x86::reg32 current = 0;
    uint8_t lowLod = 0xff;
    EffectFields fields;
    const x86::reg32 keys = rc::ld32(SortKeys);
    for (x86::reg32 i = 0; x86::sreg32(i) < x86::sreg32(rc::ld32(RecordCount)); ++i)
    {
        const x86::reg32 index = rc::ld32(keys + 8 * i);
        const x86::reg32 record = Records + index * 0x68;
        if (prefetchAhead())
            prefetchRecords(keys, i, rc::ld32(RecordCount));
        const x86::reg32 recordEffect = rc::ld32(record + 0x2c);
        const uint8_t low = rc::ld8(record + 0x64);
        if (current != recordEffect || lowLod != low)
        {
            if (current)
                finishRun(exec, record, current);
#if NFSC_RENDER_CHECK
            nr::Pass* table = nativeTable(app, cpu, recordEffect, low, i);
            if (table)
            {
                ++s_check.runs;
                s_shadow.begin(exec.device, table);
            }
#else
            nr::Pass* table = uniformsMode() ? nativeTable(app, cpu, recordEffect, low, i) : nullptr;
            if (table && uniformsFor(table))
            {
                s_run.begin(exec.device, table);
                exec.run = &s_run;
            }
#endif
            if (switchEnabled())
                step(exec, "effect start", record,
                     [&](auto& s) { startEffect(s, app, cpu, recordEffect, low); }, sub_723600, recordEffect,
                     {0, 0, low});
            else
                exec.guest(sub_723600, recordEffect, {0, 0, low});
#if NFSC_RENDER_CHECK
            if (table)
            {
                s_shadow.beginPass();
                s_feed = &s_shadow;
            }
#endif
            add32(CountEffects, 1);
            fields.e = 0;
            current = recordEffect;
            lowLod = low;
        }
        const x86::reg32 e = rc::ld32(CurrentEffect);
        if (fields.e != e || !fieldsPerRun())
            readFields(fields, e);
        const x86::reg32 view = ViewTable + rc::ld32(rc::ld32(CurrentView) + 4) * 0x1e0;
        const bool flagged = rc::ld8(rc::ld32(record + 0x40) + 0x34) != 0;
        const x86::reg32 matrix = rc::ld32(record + 0x38);
        if (fields.transformMethod == SetTransformMethod)
            step(exec, "transform", record, [&](auto& s) { setTransform(s, fields, matrix, view, flagged); },
                 sub_71e660, e, {matrix, view, x86::reg32(flagged)});
        else
            virtualCall(exec, e, 8, 0, nullptr, {matrix, view, x86::reg32(flagged)});
        add32(CountTransforms, 1);
        if (matrix == IdentityMatrix)
            add32(CountIdentity, 1);
        const x86::reg32 kind = rc::ld32(record + 0x20);
        const x86::reg32 info = kind ? 0 : rc::ld32(rc::ld32(record + 0x24));
        if (const x86::reg32 light = rc::ld32(record + 0x30))
        {
            const x86::reg32 le = rc::ld32(CurrentEffect);
            step(exec, "light context", record, [&](auto& s) { setLightContext(s, le, light); }, sub_71dfa0, le,
                 {light, matrix});
        }
        if (const x86::reg32 material = rc::ld32(record + 0x34))
        {
            const x86::reg32 me = rc::ld32(CurrentEffect);
            const x86::reg32 flags = rc::ld32(record + 0x28);
            if (me != fields.e)
                readFields(fields, me);
            if (materialEnabled() && fields.materialMethod == LightMaterialMethod)
                step(exec, "light material", record, [&](auto& s) { setLightMaterial(s, fields, material); },
                     sub_71e030, me, {material, flags});
            else
                virtualCall(exec, me, 4, LightMaterialMethod, sub_71e030, {material, flags});
        }
        if (const x86::reg32 pca = rc::ld32(record + 0x60))
        {
            const x86::reg32 pe = rc::ld32(CurrentEffect);
            if (partsEnabled())
                step(exec, "pca", record, [&](auto& s) { setPca(s, pe, pca); }, sub_71e540, pe, {pca});
            else
                exec.guest(sub_71e540, pe, {pca});
        }
        const x86::reg32 mesh = rc::ld32(record + 0x1c);
        if (kind == 0)
        {
            const x86::reg32 te = rc::ld32(CurrentEffect);
            const x86::reg32 flags = rc::ld32(record + 0x28);
            step(exec, "mesh textures", record, [&](auto& s) { setMeshTextures(s, te, record, flags); }, sub_7234a0,
                 te, {record, flags});
            if (const x86::reg32 blend = rc::ld32(record + 0x3c))
            {
                if (partsEnabled())
                    step(exec, "bones", record, [&](auto& s) { setBones(s, blend, mesh); }, sub_714cc0, 0,
                         {blend, mesh});
                else
                    exec.guest(sub_714cc0, 0, {blend, mesh});
            }
            if (!rc::ld32(mesh + 0x74) && rc::ld32(mesh + 0x40))
            {
                if (x86::sreg32(rc::ld32(VertexBuffersMade)) >= 3 || !rc::ld32(info + 0x28))
                    continue;
                exec.guest(sub_70ec20, info, {});
                add32(VertexBuffersMade, 1);
            }
            if (const x86::reg32 buffer = rc::ld32(mesh + 0x74))
            {
                exec.device->SetStreamSource(app, cpu, 0, static_cast<d3d9::Buffer*>(d3d9::lookup(buffer)), 0,
                                             rc::ld32(recordEffect + 8));
                rc::st32(StreamBuffer, rc::ld32(mesh + 0x74));
                add32(CountStreams, 1);
            }
            const x86::reg32 indices = rc::ld32(info + 0x28);
            if (indices != rc::ld32(Indices))
            {
                exec.device->SetIndices(app, cpu, static_cast<d3d9::Buffer*>(d3d9::lookup(indices)));
                rc::st32(Indices, rc::ld32(info + 0x28));
                add32(CountIndices, 1);
            }
            if (rc::ld32(info + 0x2c) && rc::ld32(mesh + 0x40) < 0xffff && rc::ld32(mesh + 0x60)
                && rc::ld32(info + 0x28))
            {
                const x86::reg32 vertices = rc::ld32(mesh + 0x78), start = rc::ld32(mesh + 0x64);
                const x86::reg32 count = rc::ld32(mesh + 0x60);
                step(exec, "draw", record,
                     [&](auto& s) { draw(s, recordEffect, vertices, start, count, record); }, sub_71ecb0,
                     recordEffect, {vertices, start, count, record});
            }
        }
        else
        {
            // A strip packet: the first texture's states, then each pass draws the strip from the packet.
            const x86::reg32 texture = rc::ld32(record + 0x40);
            const x86::reg32 plat = rc::ld32(texture);
            step(exec, "strip texture", record,
                 [&](auto& s) { setTextureState(s, recordEffect, rc::ld32(plat + 0x10), plat + 8); }, sub_71dce0,
                 recordEffect, {rc::ld32(plat + 0x10), plat + 8});
            step(exec, "strip animation", record,
                 [&](auto& s) { setTextureAnimation(s, recordEffect, texture); }, sub_71deb0, recordEffect,
                 {texture});
            exec.renderState(0x16, 1);
            exec.renderState(0xe, 0);
            rc::st32(StreamBuffer, 0);
            const x86::reg32 passes = rc::ld32(recordEffect + 0x14);
            exec.commit(rc::ld32(recordEffect + 0x44));
            for (x86::sreg32 p = x86::sreg32(passes); p > 0; --p)
                exec.device->DrawPrimitiveUP(app, cpu, 5, rc::ld32(mesh) - 2, rc::host(mesh + 4), 0x18);
            exec.renderState(0xe, 1);
            add32(CountStrips, 1);
            const x86::reg32 pool = rc::ld32(StripPool);
            rc::st32(mesh, rc::ld32(pool + 0x10));
            rc::st32(pool + 0x18, rc::ld32(pool + 0x18) - 1);
            rc::st32(pool + 0x10, mesh);
        }
    }
    if (current)
        finishRun(exec, 0, current);
    rc::st32(Indices, 0);
    rc::st32(RecordVertices, 0);
    rc::st32(RecordCount, 0);
}

}

namespace native
{

// eDataRender::FlushList: draws the records of the draw list in sort-key order and empties it.
void sub_727230(win32::WinApplication* app, x86::CPU& cpu)
{
    if (!enabled())
    {
        sub_727230_guest(app, cpu);
        return;
    }
    flush(app, cpu);
    cpu.esp += 4;
}

}
}
