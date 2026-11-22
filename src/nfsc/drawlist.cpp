// Native model submission into the renderer's draw list: eViewPlatInterface::Render (0x729320) with the record
// writes of eDataRender::Init (0x727930) and the replacement texture swaps of its model. Setting native_submit=0
// turns it off; the translated body runs then, and for the shadow pass and texture lookups it cannot make.

#include "hot.h"

#include <native.h>
#include <platform/config.h>
#include <recomp.h>

namespace nfsc
{

void sub_729320_guest(win32::WinApplication* app, x86::CPU& cpu);

namespace
{

// Guest data of the draw list and the submission.
enum : x86::reg32
{
    ShadowCull          = 0xab0a34,     // nonzero while the shadow pass culls models against its own volume
    EffectTable         = 0xb1f660,     // effect objects by index
    SwapEffectFrom      = 0xb42fc8,     // effect replaced with flag SwapFlag
    SwapEffectTo        = 0xb43028,
    SwapTextures        = 0xab08c0,     // the 5 textures used with the replacement effect
    HiddenTexture       = 0xb1d810,     // meshes whose first texture has this +0xc value are not drawn
    DefaultMaterial     = 0xa6b910,
    Records             = 0xab2780,     // 0x68-byte records
    RecordCount         = 0xab0bf0,
    RecordVertices      = 0xab0bec,
    ListStarted         = 0xa65240,     // -1 before the list's first record
    ListRecords         = 0xab0be8,
    SortKeys            = 0xb1db90,     // pointer to (record, key) pairs
    Views               = 0xb4af10,     // 0x80 bytes each
    CurrentView         = 0xab08f4,     // pointer; +4: the view index
    MaxRecords          = 0x1000,
    RecordSize          = 0x68,
    MeshSize            = 0x90
};

enum : x86::reg32
{
    SkipCull            = 4,
    SwapFlag            = 0x2000000
};

int s_enabled = -1;

bool enabled()
{
    if (s_enabled < 0)
        s_enabled = pf::Config::getBool("native_submit", true) ? 1 : 0;
    return s_enabled != 0;
}

x86::reg32 arg(const x86::CPU& cpu, int index)
{
    return rc::ld32(cpu.esp + 4 + 4 * index);
}

// A replacement texture swap: the replacement entry and the solid's texture slot.
struct Swap
{
    x86::reg32 entry, slot;
};

enum
{
    MaxSwaps = 128
};

// The model's replacement table needs a texture lookup first (an entry not resolved yet, resolve set).
bool needsLookup(x86::reg32 model, bool resolve)
{
    const x86::reg32 table = rc::ld32(model + 0x10);
    const int count = int16_t(rc::ld16(model + 0x14));
    if (!table || !count || !resolve)
        return false;
    for (int k = 0; k < count; ++k)
    {
        if (rc::ld32(table + 12 * k + 8) == 0xffffffffu)
            return true;
    }
    return false;
}

// eModel::ApplyReplacementTextureTable (0x55db30) without lookups: each texture slot of the solid whose hash has a
// replacement entry swaps texture pointers with the first such entry.
uint32_t applyReplacements(x86::reg32 model, Swap* swaps)
{
    const x86::reg32 solid = rc::ld32(model + 0xc);
    const int textures = int8_t(rc::ld8(solid + 0x19));
    const x86::reg32 table = rc::ld32(model + 0x10);
    const int count = int16_t(rc::ld16(model + 0x14));
    if (!table || !count)
        return 0;
    uint32_t n = 0;
    const x86::reg32 slots = rc::ld32(solid + 0x2c);
    for (int j = 0; j < textures; ++j)
    {
        const x86::reg32 slot = slots + 8 * x86::reg32(j) + 4;
        const x86::reg32 hash = rc::ld32(slot - 4);
        for (int k = 0; k < count; ++k)
        {
            const x86::reg32 entry = table + 12 * x86::reg32(k);
            if (rc::ld32(entry + 4) == 0 || rc::ld32(entry) != hash)
                continue;
            const x86::reg32 replacement = rc::ld32(entry + 8);
            const x86::reg32 own = rc::ld32(slot);
            if (n < MaxSwaps)
                swaps[n++] = {entry, slot};
            rc::st32(slot, replacement);
            rc::st32(entry + 8, own);
            break;
        }
    }
    return n;
}

// eModel::RestoreReplacementTextureTable (0x558920): the swaps again, in the same order.
void restoreReplacements(const Swap* swaps, uint32_t n)
{
    for (uint32_t i = 0; i < n; ++i)
    {
        const x86::reg32 replacement = rc::ld32(swaps[i].entry + 8);
        const x86::reg32 own = rc::ld32(swaps[i].slot);
        rc::st32(swaps[i].slot, replacement);
        rc::st32(swaps[i].entry + 8, own);
    }
}

// Depth sort key of a mesh (0x714820 with its first argument 0): the distance from the current view's camera
// plane to the transformed box centre, times 100, truncated.
x86::reg32 depthKey(x86::reg32 mesh, x86::reg32 matrix)
{
    float a[3], b[3];
    for (int k = 0; k < 3; ++k)
    {
        a[k] = rc::ldf32(mesh + 4 * k);
        b[k] = rc::ldf32(mesh + 0xc + 4 * k);
    }
    double lastA, lastB;
    transformBox(matrix, a, b, lastA, lastB);
    float c[3];
    for (int k = 0; k < 3; ++k)
        c[k] = float((double(b[k]) + double(a[k])) * 0.5);
    const x86::reg32 view = Views + (rc::ld32(rc::ld32(CurrentView) + 4) << 7);
    const x86::reg32 m = rc::ld32(view) + 0x80;
    // D3DXVec3Transform's z: single precision, in its order.
    const float z = c[0] * rc::ldf32(m + 8) + c[1] * rc::ldf32(m + 24) + c[2] * rc::ldf32(m + 40) + rc::ldf32(m + 56);
    const double distance = (double(rc::ldf32(rc::ld32(view + 0x40) + 0xd8)) - double(z)) * 100.0;
    return x86::reg32(x86::truncInteger64(distance));
}

// eDataRender::Init (0x727930): appends a record and its sort key to the draw list unless it is full.
void addRecord(x86::reg32 mesh, x86::reg32 solid, x86::reg32 flags, x86::reg32 effect, const x86::reg32 textures[5],
               x86::reg32 matrix, x86::reg32 light, x86::reg32 material, x86::reg32 blend, x86::reg32 pca)
{
    const x86::reg32 index = rc::ld32(RecordCount);
    if (x86::sreg32(index) >= x86::sreg32(MaxRecords))
        return;
    const x86::reg32 r = Records + index * RecordSize;
    const x86::reg32 first = textures[0];
    rc::st32(r + 4, first);
    rc::st32(r + 8, rc::ld32(rc::ld32(first) + 0x10));
    for (int k = 1; k < 5; ++k)
        rc::st32(r + 8 + 4 * k, rc::ld32(rc::ld32(textures[k]) + 0x10));
    const x86::reg32 id = rc::ld32(effect + 4);
    const x86::reg32 keep = ((~(id - 1) | ~id) << 16) | 0xfffeffffu;
    const x86::reg32 state = (rc::ld32(rc::ld32(first) + 8) & keep) | 0x40000;
    rc::st32(r, state);
    rc::st32(r + 0x1c, mesh);
    rc::st32(r + 0x20, 0);
    rc::st32(r + 0x24, solid);
    rc::st32(r + 0x28, flags);
    rc::st32(r + 0x2c, effect);
    rc::st32(r + 0x30, light);
    rc::st32(r + 0x34, material);
    rc::st32(r + 0x38, matrix);
    rc::st32(r + 0x3c, blend);
    for (int k = 0; k < 5; ++k)
        rc::st32(r + 0x40 + 4 * k, textures[k]);
    rc::st32(r + 0x58, 0);
    rc::st32(r + 0x5c, 0xbf800000u);
    rc::st32(r + 0x60, pca);
    const bool lowLod = rc::ld32(effect + 0x1774) != 0 && (flags & 0xc00) != 0;
    rc::st8(r + 0x64, lowLod ? 1 : 0);
    x86::reg32 key;
    if (rc::ld8(first + 0x38))
        key = depthKey(mesh, matrix) | 0x80000000u;
    else if (const int8_t order = int8_t(rc::ld8(first + 0x35)))
        key = x86::reg32(x86::sreg32(order)) | 0x80000000u;
    else
        key = ((rc::ld32(mesh + 0x3c) ^ flags) & 0xffffff) | ((state >> 23 & 1) << 24) | ((id & 0x1f) << 26)
            | (lowLod ? 0x2000000u : 0);
    rc::st32(r + 0x54, key);
    const x86::reg32 keys = rc::ld32(SortKeys);
    rc::st32(keys + 8 * index, index);
    rc::st32(keys + 8 * index + 4, key);
    if (rc::ld32(ListStarted) == 0xffffffffu)
    {
        rc::st32(ListStarted, 0);
        rc::st32(ListRecords, 1);
    }
    else
    {
        rc::st32(ListRecords, rc::ld32(ListRecords) + 1);
    }
    rc::st32(RecordVertices, rc::ld32(RecordVertices) + rc::ld32(mesh + 0x7c));
    rc::st32(RecordCount, index + 1);
}

}

namespace native
{

// eViewPlatInterface::Render (thiscall: view; model, matrix, light context, flags, blend matrices, PCA data):
// a record per mesh of the model's solid that is in view and not hidden.
void sub_729320(win32::WinApplication* app, x86::CPU& cpu)
{
    if (!enabled())
    {
        sub_729320_guest(app, cpu);
        return;
    }
    const x86::reg32 view = cpu.ecx;
    const x86::reg32 model = arg(cpu, 0);
    const x86::reg32 matrix = arg(cpu, 1);
    x86::reg32 light = arg(cpu, 2);
    const x86::reg32 flags = arg(cpu, 3);
    const x86::reg32 blend = arg(cpu, 4);
    const x86::reg32 pca = arg(cpu, 5);
    const x86::reg32 solid = rc::ld32(model + 0xc);
    const x86::reg32 info = solid ? rc::ld32(solid) : 0;
    if (!solid || !rc::ld32(info + 0x28))
    {
        cpu.esp += 4 + 0x18;
        return;
    }
    const bool resolve = x86::sreg32(flags) >= 0;
    if (rc::ld32(ShadowCull) || needsLookup(model, resolve))
    {
        sub_729320_guest(app, cpu);
        return;
    }
    Swap swaps[MaxSwaps];
    const uint32_t swapCount = applyReplacements(model, swaps);
    const x86::reg32 meshes = rc::ld32(info + 0x10);
    for (x86::reg32 i = 0; x86::sreg32(i) < x86::sreg32(rc::ld32(info + 0x10)) && x86::sreg32(meshes) > 0; ++i)
    {
        const x86::reg32 mesh = rc::ld32(info + 0x14) + i * MeshSize;
        x86::reg32 effect = rc::ld32(mesh + 0x34);
        if (const x86::reg32 overrides = rc::ld32(info + 0x1c))
        {
            const x86::reg32 id = rc::ld32(effect + 4);
            if (x86::sreg32(id) < 9)
            {
                const x86::reg32 index = rc::ld32(overrides + 4 * id);
                if (index != 0xffffffffu)
                    effect = rc::ld32(EffectTable + 4 * (index & 0xffff));
            }
        }
        if (!(flags & SkipCull) && !(rc::ld8(mesh + 0x38) & 1)
            && viewBoxTest(view, mesh, mesh + 0xc, matrix, nullptr) == 0)
            continue;
        const x86::reg32 slots = rc::ld32(solid + 0x2c);
        x86::reg32 textures[5];
        for (int k = 0; k < 5; ++k)
            textures[k] = rc::ld32(slots + 8 * x86::reg32(rc::ld8(mesh + 0x18 + k)) + 4);
        if (rc::ld32(textures[0] + 0xc) == rc::ld32(HiddenTexture))
            continue;
        if ((flags & SwapFlag) && effect == rc::ld32(SwapEffectFrom))
        {
            for (int k = 0; k < 5; ++k)
                textures[k] = rc::ld32(SwapTextures + 4 * k);
            effect = rc::ld32(SwapEffectTo);
        }
        const uint8_t slot = rc::ld8(mesh + 0x1d);
        const x86::reg32 material = slot == 0xff ? rc::ld32(DefaultMaterial)
                                                 : rc::ld32(rc::ld32(solid + 0x3c) + 8 * x86::reg32(slot) + 4);
        if (rc::ld32(effect + 4) == 4 && !light)
            light = rc::ld32(view + 0x4c);
        addRecord(mesh, solid, flags, effect, textures, matrix, light, material, blend, pca);
    }
    rc::st32(info + 0x1c, 0);
    restoreReplacements(swaps, swapCount);
    rc::st32(cpu.esp + 12, light);
    cpu.esp += 4 + 0x18;
}

}
}
