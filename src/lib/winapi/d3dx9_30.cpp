// d3dx9_30.dll: matrix and vector math in single precision, textures from DDS data in memory, filtered surface loads
// with DXT compression, and volume fills. Effects are in d3dx9_effect.cpp.

#include <winapi/d3dx9_30.h>
#include <winapi/d3d9_internal.h>
#include <winapi/support.h>
#include <lib/guest.h>
#include <lib/heap.h>
#include <platform/log.h>
#include <recomp.h>
#include <algorithm>
#include <arm_neon.h>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace win32 { namespace d3dx9_30
{

namespace
{

using d3d9::Device;
using d3d9::Surface;
using d3d9::Texture;
using d3d9::VolumeTexture;

const HRESULT D3D_OK = 0;
const HRESULT D3DERR_INVALIDCALL = 0x8876086C;
const HRESULT D3DERR_NOTAVAILABLE = 0x8876086A;
const HRESULT D3DXERR_INVALIDDATA = 0x88760B59;

void load(x86::reg32 address, float* out, uint32_t count)
{
    memcpy(out, g_guestBase + address, count * 4);
}

void store(x86::reg32 address, const float* values, uint32_t count)
{
    memcpy(g_guestBase + address, values, count * 4);
}

// Four floats from and to guest memory (any alignment) through the NEON unit, which also runs the VFP code that
// reads and writes the copies.
void load4(x86::reg32 address, float* out)
{
    vst1q_f32(out, vld1q_f32(reinterpret_cast<const float*>(g_guestBase + address)));
}

void store4(x86::reg32 address, const float* values)
{
    vst1q_f32(reinterpret_cast<float*>(g_guestBase + address), vld1q_f32(values));
}

void loadMatrix(x86::reg32 address, float* out)
{
    for (uint32_t i = 0; i < 4; ++i)
        load4(address + 16 * i, out + 4 * i);
}

void storeMatrix(x86::reg32 address, const float* values)
{
    for (uint32_t i = 0; i < 4; ++i)
        store4(address + 16 * i, values + 4 * i);
}

// Floats at a 4-byte aligned guest address, which VFP loads and stores reach directly.
const float* alignedFloats(x86::reg32 address)
{
    return reinterpret_cast<const float*>(g_guestBase + address);
}

float* alignedFloatsOut(x86::reg32 address)
{
    return reinterpret_cast<float*>(g_guestBase + address);
}

// Whether 16-float matrices at a and b share no byte (equal addresses do).
bool disjointMatrices(x86::reg32 a, x86::reg32 b)
{
    return a + 64 <= b || b + 64 <= a;
}

// Compiles a function without GCC's SLP vectoriser, so its scalar results are stored by the VFP directly.
#if defined(__GNUC__) && !defined(__clang__)
#define D3DX_VFP_STORES __attribute__((optimize("no-tree-slp-vectorize")))
#else
#define D3DX_VFP_STORES
#endif

// ---- DDS -----------------------------------------------------------------------------------------------------------

enum : DWORD
{
    D3DFMT_A8R8G8B8 = 21, D3DFMT_X8R8G8B8 = 22, D3DFMT_R5G6B5 = 23, D3DFMT_X1R5G5B5 = 24, D3DFMT_A1R5G5B5 = 25,
    D3DFMT_A4R4G4B4 = 26, D3DFMT_A8 = 28, D3DFMT_A8B8G8R8 = 32, D3DFMT_X8B8G8R8 = 33, D3DFMT_L8 = 50,
    D3DFMT_A8L8 = 51, D3DFMT_R8G8B8 = 20, D3DFMT_L16 = 81,
    D3DFMT_DXT1 = 0x31545844, D3DFMT_DXT3 = 0x33545844, D3DFMT_DXT5 = 0x35545844
};

struct DdsImage
{
    DWORD               format;
    uint32_t            width, height, depth, levels, faces;
    const x86::reg8*    data;       // levels of each face (cube) or of the volume, packed
    uint32_t            size;
};

uint32_t dword(const x86::reg8* p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

// The Direct3D format of a DDS pixel format; 0 when unknown.
DWORD ddsFormat(const x86::reg8* pf)
{
    const uint32_t flags = dword(pf + 4), fourCC = dword(pf + 8), bits = dword(pf + 12);
    const uint32_t r = dword(pf + 16), g = dword(pf + 20), b = dword(pf + 24), a = dword(pf + 28);
    const bool alpha = (flags & 0x1) != 0;
    if (flags & 0x4)
        return fourCC;
    if (flags & 0x40)
    {
        if (bits == 32 && r == 0xff0000 && g == 0xff00 && b == 0xff)
            return alpha && a == 0xff000000u ? D3DFMT_A8R8G8B8 : D3DFMT_X8R8G8B8;
        if (bits == 32 && r == 0xff && g == 0xff00 && b == 0xff0000)
            return alpha && a == 0xff000000u ? D3DFMT_A8B8G8R8 : D3DFMT_X8B8G8R8;
        if (bits == 24 && r == 0xff0000 && g == 0xff00 && b == 0xff)
            return D3DFMT_R8G8B8;
        if (bits == 16 && r == 0xf800 && g == 0x7e0 && b == 0x1f)
            return D3DFMT_R5G6B5;
        if (bits == 16 && r == 0x7c00 && g == 0x3e0 && b == 0x1f)
            return alpha && a == 0x8000 ? D3DFMT_A1R5G5B5 : D3DFMT_X1R5G5B5;
        if (bits == 16 && r == 0xf00 && g == 0xf0 && b == 0xf && alpha && a == 0xf000)
            return D3DFMT_A4R4G4B4;
        return 0;
    }
    if (flags & 0x20000)
    {
        if (bits == 8)
            return D3DFMT_L8;
        if (bits == 16)
            return alpha ? D3DFMT_A8L8 : D3DFMT_L16;
        return 0;
    }
    if ((flags & 0x2) && bits == 8)
        return D3DFMT_A8;
    return 0;
}

// Bytes of one level of a w x h image.
uint32_t levelBytes(DWORD format, uint32_t w, uint32_t h)
{
    bool block;
    const uint32_t bits = d3d9::formatBits(format, &block);
    if (block)
        return ((w + 3) / 4) * ((h + 3) / 4) * bits / 8;
    return (w * bits + 7) / 8 * h;
}

uint32_t levelPitch(DWORD format, uint32_t w)
{
    bool block;
    const uint32_t bits = d3d9::formatBits(format, &block);
    return block ? (w + 3) / 4 * bits / 8 : (w * bits + 7) / 8;
}

uint32_t dim(uint32_t size, uint32_t level)
{
    const uint32_t d = size >> level;
    return d ? d : 1;
}

bool parseDds(const x86::reg8* data, uint32_t size, DdsImage* image)
{
    if (size < 128 || dword(data) != 0x20534444 || dword(data + 4) != 124)
        return false;
    const uint32_t flags = dword(data + 8);
    image->height = dword(data + 12);
    image->width = dword(data + 16);
    image->depth = (flags & 0x800000) ? std::max<uint32_t>(dword(data + 24), 1) : 1;
    image->levels = (flags & 0x20000) ? std::max<uint32_t>(dword(data + 28), 1) : 1;
    image->format = ddsFormat(data + 76);
    const uint32_t caps2 = dword(data + 112);
    image->faces = 1;
    if (caps2 & 0x200)
    {
        image->faces = 0;
        for (uint32_t f = 0; f < 6; ++f)
            image->faces += (caps2 >> (10 + f)) & 1;
    }
    image->data = data + 128;
    image->size = size - 128;
    bool block;
    if (!image->width || !image->height || !d3d9::formatBits(image->format, &block))
        return false;
    uint64_t needed = 0;
    for (uint32_t level = 0; level < image->levels; ++level)
        needed += uint64_t(levelBytes(image->format, dim(image->width, level), dim(image->height, level))) *
                  dim(image->depth, level);
    return needed * image->faces <= image->size;
}

// ---- Pixel conversion ----------------------------------------------------------------------------------------------

// 565 to 888 by bit replication.
void rgb565(uint32_t c, uint32_t out[3])
{
    const uint32_t r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
    out[0] = (r << 3) | (r >> 2);
    out[1] = (g << 2) | (g >> 4);
    out[2] = (b << 3) | (b >> 2);
}

// One DXT block to 16 BGRA pixels.
void decodeBlock(DWORD format, const x86::reg8* block, uint8_t out[16][4])
{
    const x86::reg8* color = format == D3DFMT_DXT1 ? block : block + 8;
    const uint32_t c0 = uint32_t(color[0]) | uint32_t(color[1]) << 8;
    const uint32_t c1 = uint32_t(color[2]) | uint32_t(color[3]) << 8;
    uint32_t p[4][4];
    rgb565(c0, p[0]);
    rgb565(c1, p[1]);
    p[0][3] = p[1][3] = 255;
    const bool fourColor = format != D3DFMT_DXT1 || c0 > c1;
    for (int i = 0; i < 3; ++i)
    {
        if (fourColor)
        {
            p[2][i] = (2 * p[0][i] + p[1][i]) / 3;
            p[3][i] = (p[0][i] + 2 * p[1][i]) / 3;
        }
        else
        {
            p[2][i] = (p[0][i] + p[1][i]) / 2;
            p[3][i] = 0;
        }
    }
    p[2][3] = 255;
    p[3][3] = fourColor ? 255 : 0;
    const uint32_t bits = dword(color + 4);
    uint8_t alphas[8];
    if (format == D3DFMT_DXT5)
    {
        alphas[0] = block[0];
        alphas[1] = block[1];
        for (int i = 1; i < 7; ++i)
        {
            if (alphas[0] > alphas[1])
                alphas[i + 1] = uint8_t(((7 - i) * alphas[0] + i * alphas[1]) / 7);
            else if (i < 5)
                alphas[i + 1] = uint8_t(((5 - i) * alphas[0] + i * alphas[1]) / 5);
            else
                alphas[i + 1] = i == 5 ? 0 : 255;
        }
    }
    const uint64_t alphaBits = format == D3DFMT_DXT5 ? (uint64_t(dword(block + 2)) | uint64_t(block[6]) << 32 |
                                                        uint64_t(block[7]) << 40) : 0;
    for (int i = 0; i < 16; ++i)
    {
        const uint32_t* c = p[(bits >> (2 * i)) & 3];
        out[i][0] = uint8_t(c[2]);
        out[i][1] = uint8_t(c[1]);
        out[i][2] = uint8_t(c[0]);
        out[i][3] = uint8_t(c[3]);
        if (format == D3DFMT_DXT3)
        {
            const uint32_t a = (block[i / 2] >> (4 * (i & 1))) & 15;
            out[i][3] = uint8_t(a * 17);
        }
        else if (format == D3DFMT_DXT5)
        {
            out[i][3] = alphas[(alphaBits >> (3 * i)) & 7];
        }
    }
}

// One level to BGRA bytes (A8R8G8B8 layout, rows packed); false for a format without a conversion.
bool toArgb(DWORD format, const x86::reg8* src, uint32_t pitch, uint32_t w, uint32_t h, uint8_t* out)
{
    if (format == D3DFMT_DXT1 || format == D3DFMT_DXT3 || format == D3DFMT_DXT5)
    {
        const uint32_t blockBytes = format == D3DFMT_DXT1 ? 8 : 16;
        for (uint32_t by = 0; by < (h + 3) / 4; ++by)
        {
            for (uint32_t bx = 0; bx < (w + 3) / 4; ++bx)
            {
                uint8_t px[16][4];
                decodeBlock(format, src + by * pitch + bx * blockBytes, px);
                for (uint32_t i = 0; i < 16; ++i)
                {
                    const uint32_t x = bx * 4 + i % 4, y = by * 4 + i / 4;
                    if (x < w && y < h)
                        memcpy(out + (y * w + x) * 4, px[i], 4);
                }
            }
        }
        return true;
    }
    for (uint32_t y = 0; y < h; ++y)
    {
        const x86::reg8* row = src + y * pitch;
        for (uint32_t x = 0; x < w; ++x)
        {
            uint8_t* o = out + (y * w + x) * 4;
            uint32_t c;
            switch (format)
            {
            case D3DFMT_A8R8G8B8:
                memcpy(o, row + x * 4, 4);
                break;
            case D3DFMT_X8R8G8B8:
                memcpy(o, row + x * 4, 3);
                o[3] = 255;
                break;
            case D3DFMT_A8B8G8R8:
            case D3DFMT_X8B8G8R8:
                o[0] = row[x * 4 + 2];
                o[1] = row[x * 4 + 1];
                o[2] = row[x * 4 + 0];
                o[3] = format == D3DFMT_A8B8G8R8 ? row[x * 4 + 3] : 255;
                break;
            case D3DFMT_R8G8B8:
                memcpy(o, row + x * 3, 3);
                o[3] = 255;
                break;
            case D3DFMT_R5G6B5:
            {
                uint32_t rgb[3];
                rgb565(uint32_t(row[x * 2]) | uint32_t(row[x * 2 + 1]) << 8, rgb);
                o[0] = uint8_t(rgb[2]);
                o[1] = uint8_t(rgb[1]);
                o[2] = uint8_t(rgb[0]);
                o[3] = 255;
                break;
            }
            case D3DFMT_A1R5G5B5:
            case D3DFMT_X1R5G5B5:
                c = uint32_t(row[x * 2]) | uint32_t(row[x * 2 + 1]) << 8;
                o[0] = uint8_t(((c & 31) << 3) | ((c & 31) >> 2));
                o[1] = uint8_t((((c >> 5) & 31) << 3) | (((c >> 5) & 31) >> 2));
                o[2] = uint8_t((((c >> 10) & 31) << 3) | (((c >> 10) & 31) >> 2));
                o[3] = format == D3DFMT_A1R5G5B5 ? ((c & 0x8000) ? 255 : 0) : 255;
                break;
            case D3DFMT_A4R4G4B4:
                c = uint32_t(row[x * 2]) | uint32_t(row[x * 2 + 1]) << 8;
                o[0] = uint8_t((c & 15) * 17);
                o[1] = uint8_t(((c >> 4) & 15) * 17);
                o[2] = uint8_t(((c >> 8) & 15) * 17);
                o[3] = uint8_t(((c >> 12) & 15) * 17);
                break;
            case D3DFMT_L8:
                o[0] = o[1] = o[2] = row[x];
                o[3] = 255;
                break;
            case D3DFMT_A8L8:
                o[0] = o[1] = o[2] = row[x * 2];
                o[3] = row[x * 2 + 1];
                break;
            case D3DFMT_A8:
                o[0] = o[1] = o[2] = 0;
                o[3] = row[x];
                break;
            default:
                return false;
            }
        }
    }
    return true;
}

// Halves a BGRA image with a 2x2 box filter (rounded average of the covered texels).
std::vector<uint8_t> boxHalve(const std::vector<uint8_t>& in, uint32_t w, uint32_t h)
{
    const uint32_t nw = std::max<uint32_t>(w / 2, 1), nh = std::max<uint32_t>(h / 2, 1);
    std::vector<uint8_t> out(size_t(nw) * nh * 4);
    for (uint32_t y = 0; y < nh; ++y)
    {
        for (uint32_t x = 0; x < nw; ++x)
        {
            const uint32_t x0 = std::min(2 * x, w - 1), x1 = std::min(2 * x + 1, w - 1);
            const uint32_t y0 = std::min(2 * y, h - 1), y1 = std::min(2 * y + 1, h - 1);
            for (uint32_t c = 0; c < 4; ++c)
            {
                const uint32_t sum = in[(y0 * w + x0) * 4 + c] + in[(y0 * w + x1) * 4 + c] +
                                     in[(y1 * w + x0) * 4 + c] + in[(y1 * w + x1) * 4 + c];
                out[(y * nw + x) * 4 + c] = uint8_t((sum + 2) / 4);
            }
        }
    }
    return out;
}

bool powerOfTwo(uint32_t v)
{
    return v && !(v & (v - 1));
}

uint32_t fullChain(uint32_t w, uint32_t h, uint32_t d = 1)
{
    uint32_t levels = 1;
    for (uint32_t size = std::max(w, std::max(h, d)); size > 1; size >>= 1)
        ++levels;
    return levels;
}

// Formats the renderer stores as they are; others are converted to A8R8G8B8.
bool deviceFormat(DWORD format)
{
    return format == D3DFMT_A8R8G8B8 || format == D3DFMT_X8R8G8B8 || format == D3DFMT_DXT1 || format == D3DFMT_DXT3;
}

Device* deviceOf(x86::reg32 guest)
{
    d3d9::Object* object = d3d9::lookup(guest);
    return object && object->kind() == d3d9::KindDevice ? static_cast<Device*>(object) : nullptr;
}

// Calls a stdcall guest function on a copy of the caller's state.
void callStdcall(WinApplication* app, x86::CPU& cpu, x86::reg32 function, std::initializer_list<x86::reg32> args)
{
    x86::CPU copy = cpu;
    copy.esp -= 4 * x86::reg32(args.size());
    x86::reg32 slot = copy.esp;
    for (x86::reg32 a : args)
    {
        rc::st32(slot, a);
        slot += 4;
    }
    native::callGuest(app, copy, function);
    if (copy.terminate)
        cpu.terminate = true;
}

uint8_t unorm8(float v)
{
    if (!(v > 0.0f))
        return 0;
    if (v >= 1.0f)
        return 255;
    return uint8_t(v * 255.0f + 0.5f);
}

uint32_t unormBits(float v, uint32_t bits)
{
    const float max = float((1u << bits) - 1);
    if (!(v > 0.0f))
        return 0;
    if (v >= 1.0f)
        return uint32_t(max);
    return uint32_t(v * max + 0.5f);
}

// ---- DXT encoding --------------------------------------------------------------------------------------------------

// 565 channel expansion as the decoder does it.
int expand5(int q) { return (q << 3) | (q >> 2); }
int expand6(int q) { return (q << 2) | (q >> 4); }

struct Quant565
{
    uint8_t q5[256], q6[256];               // nearest 5- and 6-bit code of an 8-bit value
    uint8_t third5[256][2], third6[256][2]; // codes (a, b) whose (2a + b) / 3 decodes nearest to the value
    uint8_t half5[256][2], half6[256][2];   // codes (a, b) whose (a + b) / 2 decodes nearest to the value
};

void buildPairs(int bits, uint8_t third[256][2], uint8_t half[256][2])
{
    const int codes = 1 << bits;
    for (int v = 0; v < 256; ++v)
    {
        int bestThird = 1 << 30, bestHalf = 1 << 30;
        for (int a = 0; a < codes; ++a)
            for (int b = 0; b < codes; ++b)
            {
                const int ea = bits == 5 ? expand5(a) : expand6(a), eb = bits == 5 ? expand5(b) : expand6(b);
                const int spread = std::abs(ea - eb);
                const int t = std::abs((2 * ea + eb) / 3 - v) * 1024 + spread;
                const int h = std::abs((ea + eb) / 2 - v) * 1024 + spread;
                if (t < bestThird)
                {
                    bestThird = t;
                    third[v][0] = uint8_t(a);
                    third[v][1] = uint8_t(b);
                }
                if (h < bestHalf)
                {
                    bestHalf = h;
                    half[v][0] = uint8_t(a);
                    half[v][1] = uint8_t(b);
                }
            }
    }
}

const Quant565& quant565()
{
    static const Quant565 tables = []
    {
        Quant565 t;
        for (int v = 0; v < 256; ++v)
        {
            int best5 = 0, best6 = 0;
            for (int q = 0; q < 64; ++q)
            {
                if (q < 32 && std::abs(expand5(q) - v) < std::abs(expand5(best5) - v))
                    best5 = q;
                if (std::abs(expand6(q) - v) < std::abs(expand6(best6) - v))
                    best6 = q;
            }
            t.q5[v] = uint8_t(best5);
            t.q6[v] = uint8_t(best6);
        }
        buildPairs(5, t.third5, t.half5);
        buildPairs(6, t.third6, t.half6);
        return t;
    }();
    return tables;
}

int clampByte(float v)
{
    return v <= 0.0f ? 0 : v >= 255.0f ? 255 : int(v + 0.5f);
}

// Nearest 565 colour of an RGB colour (0..255 per channel).
uint16_t pack565(float r, float g, float b)
{
    const Quant565& q = quant565();
    return uint16_t(q.q5[clampByte(r)] << 11 | q.q6[clampByte(g)] << 5 | q.q5[clampByte(b)]);
}

enum ColorMode
{
    ModeFour,       // DXT3 / DXT5: four colours whatever the endpoint order
    ModeDxt1Four,   // DXT1 with c0 > c1
    ModeDxt1Three   // DXT1 with c0 <= c1: c0, c1, midpoint, transparent black
};

struct ColorBlock
{
    uint16_t c0, c1;
    uint32_t indices;
    uint32_t error;
};

// RGB pixels of a block, which of them lie inside the surface and which are transparent (DXT1).
struct BlockPixels
{
    int      rgb[16][3];
    uint32_t used;
    uint32_t transparent;
};

// The block for endpoints a and b in a mode (ordered as the mode needs) with the nearest palette entry per pixel:
// opaque pixels of a three-colour block take the first three, transparent ones 3, pixels outside the surface 0.
ColorBlock evaluateColors(const BlockPixels& px, uint16_t a, uint16_t b, ColorMode mode)
{
    bool three = false;
    if (mode == ModeDxt1Four)
    {
        if (a < b)
            std::swap(a, b);
        three = a == b;
    }
    else if (mode == ModeDxt1Three)
    {
        if (a > b)
            std::swap(a, b);
        three = true;
    }
    uint32_t e0[3], e1[3];
    rgb565(a, e0);
    rgb565(b, e1);
    int pal[4][3];
    for (int c = 0; c < 3; ++c)
    {
        pal[0][c] = int(e0[c]);
        pal[1][c] = int(e1[c]);
        pal[2][c] = three ? int(e0[c] + e1[c]) / 2 : int(2 * e0[c] + e1[c]) / 3;
        pal[3][c] = three ? 0 : int(e0[c] + 2 * e1[c]) / 3;
    }
    ColorBlock block = { a, b, 0, 0 };
    for (int i = 0; i < 16; ++i)
    {
        if (!(px.used >> i & 1))
            continue;
        if (px.transparent >> i & 1)
        {
            block.indices |= 3u << (2 * i);
            continue;
        }
        uint32_t best = ~0u;
        int bestIndex = 0;
        for (int k = 0; k < (three ? 3 : 4); ++k)
        {
            const int dr = px.rgb[i][0] - pal[k][0], dg = px.rgb[i][1] - pal[k][1], db = px.rgb[i][2] - pal[k][2];
            const uint32_t d = uint32_t(dr * dr + dg * dg + db * db);
            if (d < best)
            {
                best = d;
                bestIndex = k;
            }
        }
        block.indices |= uint32_t(bestIndex) << (2 * i);
        block.error += best;
    }
    return block;
}

// Least-squares endpoints for the indices of a block; false when the system is singular.
bool refineColors(const BlockPixels& px, const ColorBlock& block, ColorMode mode, uint16_t* a, uint16_t* b)
{
    const bool three = mode == ModeDxt1Three || (mode == ModeDxt1Four && block.c0 == block.c1);
    float aa = 0, ab = 0, bb = 0, ap[3] = {}, bp[3] = {};
    for (int i = 0; i < 16; ++i)
    {
        if (!(px.used >> i & 1) || (px.transparent >> i & 1))
            continue;
        const uint32_t k = block.indices >> (2 * i) & 3;
        float wa;
        if (k == 0)
            wa = 1.0f;
        else if (k == 1)
            wa = 0.0f;
        else if (three)
            wa = 0.5f;
        else
            wa = k == 2 ? 2.0f / 3.0f : 1.0f / 3.0f;
        const float wb = 1.0f - wa;
        aa += wa * wa;
        ab += wa * wb;
        bb += wb * wb;
        for (int c = 0; c < 3; ++c)
        {
            ap[c] += wa * float(px.rgb[i][c]);
            bp[c] += wb * float(px.rgb[i][c]);
        }
    }
    const float det = aa * bb - ab * ab;
    if (std::fabs(det) < 1e-6f)
        return false;
    float e0[3], e1[3];
    for (int c = 0; c < 3; ++c)
    {
        e0[c] = (ap[c] * bb - bp[c] * ab) / det;
        e1[c] = (bp[c] * aa - ap[c] * ab) / det;
    }
    *a = pack565(e0[0], e0[1], e0[2]);
    *b = pack565(e1[0], e1[1], e1[2]);
    return true;
}

// Endpoints and indices of a block: principal-axis extremes (or the exact pair for one colour) refined by least
// squares, in every mode the format allows, the one with the least squared error.
ColorBlock fitColors(const BlockPixels& px, DWORD format)
{
    const uint32_t opaque = px.used & ~px.transparent;
    if (!opaque)
    {
        ColorBlock block = { 0, 0, 0, 0 };
        for (int i = 0; i < 16; ++i)
            if (px.transparent >> i & 1)
                block.indices |= 3u << (2 * i);
        return block;
    }
    std::vector<std::pair<uint16_t, uint16_t>> candidates;
    float mean[3] = {};
    int count = 0, first = -1;
    bool single = true;
    for (int i = 0; i < 16; ++i)
    {
        if (!(opaque >> i & 1))
            continue;
        if (first < 0)
            first = i;
        else if (px.rgb[i][0] != px.rgb[first][0] || px.rgb[i][1] != px.rgb[first][1] ||
                 px.rgb[i][2] != px.rgb[first][2])
            single = false;
        for (int c = 0; c < 3; ++c)
            mean[c] += float(px.rgb[i][c]);
        ++count;
    }
    if (single)
    {
        const Quant565& q = quant565();
        const int* p = px.rgb[first];
        candidates.push_back({ uint16_t(q.third5[p[0]][0] << 11 | q.third6[p[1]][0] << 5 | q.third5[p[2]][0]),
                               uint16_t(q.third5[p[0]][1] << 11 | q.third6[p[1]][1] << 5 | q.third5[p[2]][1]) });
        candidates.push_back({ uint16_t(q.half5[p[0]][0] << 11 | q.half6[p[1]][0] << 5 | q.half5[p[2]][0]),
                               uint16_t(q.half5[p[0]][1] << 11 | q.half6[p[1]][1] << 5 | q.half5[p[2]][1]) });
        const uint16_t exact = pack565(float(p[0]), float(p[1]), float(p[2]));
        candidates.push_back({ exact, exact });
    }
    else
    {
        for (float& m : mean)
            m /= float(count);
        float cov[6] = {};
        for (int i = 0; i < 16; ++i)
        {
            if (!(opaque >> i & 1))
                continue;
            const float d[3] = { float(px.rgb[i][0]) - mean[0], float(px.rgb[i][1]) - mean[1],
                                 float(px.rgb[i][2]) - mean[2] };
            cov[0] += d[0] * d[0];
            cov[1] += d[0] * d[1];
            cov[2] += d[0] * d[2];
            cov[3] += d[1] * d[1];
            cov[4] += d[1] * d[2];
            cov[5] += d[2] * d[2];
        }
        float axis[3] = { cov[0] + cov[1] + cov[2], cov[1] + cov[3] + cov[4], cov[2] + cov[4] + cov[5] };
        for (int iteration = 0; iteration < 8; ++iteration)
        {
            const float scale = std::max(std::fabs(axis[0]), std::max(std::fabs(axis[1]), std::fabs(axis[2])));
            if (scale < 1e-6f)
            {
                axis[0] = axis[1] = axis[2] = 1.0f;
                break;
            }
            const float v[3] = { axis[0] / scale, axis[1] / scale, axis[2] / scale };
            axis[0] = cov[0] * v[0] + cov[1] * v[1] + cov[2] * v[2];
            axis[1] = cov[1] * v[0] + cov[3] * v[1] + cov[4] * v[2];
            axis[2] = cov[2] * v[0] + cov[4] * v[1] + cov[5] * v[2];
        }
        float lo = 0, hi = 0;
        int loIndex = first, hiIndex = first;
        bool any = false;
        for (int i = 0; i < 16; ++i)
        {
            if (!(opaque >> i & 1))
                continue;
            const float t = (float(px.rgb[i][0]) - mean[0]) * axis[0] + (float(px.rgb[i][1]) - mean[1]) * axis[1] +
                            (float(px.rgb[i][2]) - mean[2]) * axis[2];
            if (!any || t < lo)
            {
                lo = t;
                loIndex = i;
            }
            if (!any || t > hi)
            {
                hi = t;
                hiIndex = i;
            }
            any = true;
        }
        const int* l = px.rgb[loIndex];
        const int* h = px.rgb[hiIndex];
        candidates.push_back({ pack565(float(h[0]), float(h[1]), float(h[2])),
                               pack565(float(l[0]), float(l[1]), float(l[2])) });
    }
    ColorMode modes[2];
    int modeCount = 0;
    if (format != D3DFMT_DXT1)
        modes[modeCount++] = ModeFour;
    else
    {
        if (!px.transparent)
            modes[modeCount++] = ModeDxt1Four;
        modes[modeCount++] = ModeDxt1Three;
    }
    ColorBlock best = { 0, 0, 0, ~0u };
    for (int m = 0; m < modeCount; ++m)
    {
        for (const auto& candidate : candidates)
        {
            ColorBlock block = evaluateColors(px, candidate.first, candidate.second, modes[m]);
            for (int iteration = 0; iteration < 3 && block.error; ++iteration)
            {
                uint16_t a, b;
                if (!refineColors(px, block, modes[m], &a, &b))
                    break;
                const ColorBlock refined = evaluateColors(px, a, b, modes[m]);
                if (refined.error >= block.error)
                    break;
                block = refined;
            }
            if (block.error < best.error)
                best = block;
        }
    }
    return best;
}

// DXT5 alpha palette: eight values for a0 > a1, else six with 0 and 255 (the decoder's arithmetic).
void alphaPalette(int a0, int a1, int pal[8])
{
    pal[0] = a0;
    pal[1] = a1;
    for (int i = 1; i < 7; ++i)
    {
        if (a0 > a1)
            pal[i + 1] = ((7 - i) * a0 + i * a1) / 7;
        else if (i < 5)
            pal[i + 1] = ((5 - i) * a0 + i * a1) / 5;
        else
            pal[i + 1] = i == 5 ? 0 : 255;
    }
}

struct AlphaBlock
{
    int      a0, a1;
    uint64_t indices;
    uint32_t error;
};

AlphaBlock evaluateAlpha(const uint8_t alpha[16], uint32_t used, int a0, int a1)
{
    int pal[8];
    alphaPalette(a0, a1, pal);
    AlphaBlock block = { a0, a1, 0, 0 };
    for (int i = 0; i < 16; ++i)
    {
        if (!(used >> i & 1))
            continue;
        uint32_t best = ~0u;
        int bestIndex = 0;
        for (int k = 0; k < 8; ++k)
        {
            const int d = int(alpha[i]) - pal[k];
            if (uint32_t(d * d) < best)
            {
                best = uint32_t(d * d);
                bestIndex = k;
            }
        }
        block.indices |= uint64_t(bestIndex) << (3 * i);
        block.error += best;
    }
    return block;
}

// Least-squares endpoints for the interpolated indices of a block; false when there is nothing to solve.
bool refineAlpha(const uint8_t alpha[16], uint32_t used, const AlphaBlock& block, int* a0, int* a1)
{
    const bool eight = block.a0 > block.a1;
    float aa = 0, ab = 0, bb = 0, ap = 0, bp = 0;
    for (int i = 0; i < 16; ++i)
    {
        if (!(used >> i & 1))
            continue;
        const int k = int(block.indices >> (3 * i) & 7);
        if (!eight && k >= 6)
            continue;
        const float wa = k == 0 ? 1.0f : k == 1 ? 0.0f : eight ? float(8 - k) / 7.0f : float(6 - k) / 5.0f;
        const float wb = 1.0f - wa;
        aa += wa * wa;
        ab += wa * wb;
        bb += wb * wb;
        ap += wa * float(alpha[i]);
        bp += wb * float(alpha[i]);
    }
    const float det = aa * bb - ab * ab;
    if (std::fabs(det) < 1e-6f)
        return false;
    *a0 = clampByte((ap * bb - bp * ab) / det);
    *a1 = clampByte((bp * aa - ap * ab) / det);
    if (eight ? *a0 <= *a1 : *a0 > *a1)
        std::swap(*a0, *a1);
    return true;
}

// DXT5 alpha block: the eight-value fit of the extremes or the six-value fit of the values other than 0 and 255,
// each refined by least squares, the one with the least squared error.
void encodeAlpha5(const uint8_t alpha[16], uint32_t used, uint8_t* out)
{
    int lo = 255, hi = 0, lo6 = 255, hi6 = 0;
    for (int i = 0; i < 16; ++i)
    {
        if (!(used >> i & 1))
            continue;
        lo = std::min<int>(lo, alpha[i]);
        hi = std::max<int>(hi, alpha[i]);
        if (alpha[i] != 0 && alpha[i] != 255)
        {
            lo6 = std::min<int>(lo6, alpha[i]);
            hi6 = std::max<int>(hi6, alpha[i]);
        }
    }
    if (lo > hi)
        lo = hi = 255;
    if (lo6 > hi6)
        lo6 = hi6 = lo;
    AlphaBlock best = evaluateAlpha(alpha, used, lo6, hi6);
    if (hi > lo)
    {
        const AlphaBlock eight = evaluateAlpha(alpha, used, hi, lo);
        if (eight.error < best.error)
            best = eight;
    }
    for (int iteration = 0; iteration < 3 && best.error; ++iteration)
    {
        int a0, a1;
        if (!refineAlpha(alpha, used, best, &a0, &a1))
            break;
        const AlphaBlock refined = evaluateAlpha(alpha, used, a0, a1);
        if (refined.error >= best.error)
            break;
        best = refined;
    }
    out[0] = uint8_t(best.a0);
    out[1] = uint8_t(best.a1);
    for (int i = 0; i < 6; ++i)
        out[2 + i] = uint8_t(best.indices >> (8 * i));
}

// One DXT block from 16 BGRA pixels; pixels outside used (beyond the surface) are free. DXT1 alpha below 128 is
// transparent.
void encodeBlock(DWORD format, const uint8_t px[16][4], uint32_t used, uint8_t* out)
{
    BlockPixels pixels;
    pixels.used = used;
    pixels.transparent = 0;
    uint8_t alpha[16];
    for (int i = 0; i < 16; ++i)
    {
        pixels.rgb[i][0] = px[i][2];
        pixels.rgb[i][1] = px[i][1];
        pixels.rgb[i][2] = px[i][0];
        alpha[i] = px[i][3];
        if (format == D3DFMT_DXT1 && (used >> i & 1) && px[i][3] < 128)
            pixels.transparent |= 1u << i;
    }
    uint8_t* color = out;
    if (format == D3DFMT_DXT3)
    {
        for (int i = 0; i < 8; ++i)
            out[i] = uint8_t((alpha[2 * i] * 15 + 127) / 255 | ((alpha[2 * i + 1] * 15 + 127) / 255) << 4);
        color = out + 8;
    }
    else if (format == D3DFMT_DXT5)
    {
        encodeAlpha5(alpha, used, out);
        color = out + 8;
    }
    const ColorBlock block = fitColors(pixels, format);
    color[0] = uint8_t(block.c0);
    color[1] = uint8_t(block.c0 >> 8);
    color[2] = uint8_t(block.c1);
    color[3] = uint8_t(block.c1 >> 8);
    memcpy(color + 4, &block.indices, 4);
}

// ---- Surface loading -----------------------------------------------------------------------------------------------

enum : DWORD
{
    D3DX_FILTER_NONE = 1, D3DX_FILTER_POINT = 2, D3DX_FILTER_LINEAR = 3, D3DX_FILTER_TRIANGLE = 4,
    D3DX_FILTER_BOX = 5, D3DX_FILTER_MIRROR_U = 0x10000, D3DX_FILTER_MIRROR_V = 0x20000,
    D3DX_FILTER_MIRROR_W = 0x40000, D3DX_FILTER_DITHER = 0x80000, D3DX_FILTER_DITHER_DIFFUSION = 0x100000,
    D3DX_FILTER_SRGB_IN = 0x200000, D3DX_FILTER_SRGB_OUT = 0x400000, D3DX_DEFAULT = 0xffffffff
};

// Formats toArgb reads.
bool decodable(DWORD format)
{
    switch (format)
    {
    case D3DFMT_A8R8G8B8: case D3DFMT_X8R8G8B8: case D3DFMT_A8B8G8R8: case D3DFMT_X8B8G8R8: case D3DFMT_R8G8B8:
    case D3DFMT_R5G6B5: case D3DFMT_X1R5G5B5: case D3DFMT_A1R5G5B5: case D3DFMT_A4R4G4B4: case D3DFMT_L8:
    case D3DFMT_A8L8: case D3DFMT_A8: case D3DFMT_DXT1: case D3DFMT_DXT3: case D3DFMT_DXT5:
        return true;
    default:
        return false;
    }
}

bool blockFormat(DWORD format)
{
    return format == D3DFMT_DXT1 || format == D3DFMT_DXT3 || format == D3DFMT_DXT5;
}

// Formats the loader writes: every format a lockable texture of the device can have.
bool encodable(DWORD format)
{
    return format == D3DFMT_A8R8G8B8 || format == D3DFMT_X8R8G8B8 || blockFormat(format);
}

// Pixels of a rectangle of a level as BGRA bytes, rows packed.
void readRect(DWORD format, const uint8_t* bits, uint32_t pitch, const RECT& r, std::vector<uint8_t>& out)
{
    const uint32_t w = uint32_t(r.right - r.left), h = uint32_t(r.bottom - r.top);
    out.resize(size_t(w) * h * 4);
    if (!blockFormat(format))
    {
        bool block;
        const uint32_t bytes = d3d9::formatBits(format, &block) / 8;
        toArgb(format, bits + uint32_t(r.top) * pitch + uint32_t(r.left) * bytes, pitch, w, h, out.data());
        return;
    }
    // Blocks from the one holding the top-left pixel, then the rectangle cut out of them.
    const uint32_t x0 = uint32_t(r.left) & ~3u, y0 = uint32_t(r.top) & ~3u;
    const uint32_t bw = uint32_t(r.right) - x0, bh = uint32_t(r.bottom) - y0;
    std::vector<uint8_t> blocks(size_t(bw) * bh * 4);
    toArgb(format, bits + y0 / 4 * pitch + x0 / 4 * (format == D3DFMT_DXT1 ? 8 : 16), pitch, bw, bh, blocks.data());
    for (uint32_t y = 0; y < h; ++y)
        memcpy(out.data() + size_t(y) * w * 4, blocks.data() + (size_t(y + uint32_t(r.top) - y0) * bw +
                                                               (uint32_t(r.left) - x0)) * 4, size_t(w) * 4);
}

// Writes BGRA pixels (rows packed) into a rectangle of a level of width x height; the pixels of DXT blocks the
// rectangle covers in part keep their decoded values.
void writeRect(DWORD format, uint8_t* bits, uint32_t pitch, uint32_t width, uint32_t height, const RECT& r,
               const uint8_t* px)
{
    const uint32_t w = uint32_t(r.right - r.left);
    if (!blockFormat(format))
    {
        for (uint32_t y = 0; y < uint32_t(r.bottom - r.top); ++y)
        {
            uint8_t* row = bits + (uint32_t(r.top) + y) * pitch + uint32_t(r.left) * 4;
            memcpy(row, px + size_t(y) * w * 4, size_t(w) * 4);
            if (format == D3DFMT_X8R8G8B8)
                for (uint32_t x = 0; x < w; ++x)
                    row[x * 4 + 3] = 255;
        }
        return;
    }
    const uint32_t blockBytes = format == D3DFMT_DXT1 ? 8 : 16;
    for (uint32_t by = uint32_t(r.top) / 4; by < (uint32_t(r.bottom) + 3) / 4; ++by)
    {
        for (uint32_t bx = uint32_t(r.left) / 4; bx < (uint32_t(r.right) + 3) / 4; ++bx)
        {
            uint8_t* block = bits + by * pitch + bx * blockBytes;
            uint8_t pixels[16][4];
            uint32_t used = 0;
            bool partial = false;
            for (uint32_t i = 0; i < 16; ++i)
            {
                const uint32_t x = bx * 4 + i % 4, y = by * 4 + i / 4;
                if (x >= width || y >= height)
                    continue;
                used |= 1u << i;
                if (x < uint32_t(r.left) || x >= uint32_t(r.right) || y < uint32_t(r.top) || y >= uint32_t(r.bottom))
                    partial = true;
            }
            if (partial)
                decodeBlock(format, block, pixels);
            for (uint32_t i = 0; i < 16; ++i)
            {
                const uint32_t x = bx * 4 + i % 4, y = by * 4 + i / 4;
                if (x >= uint32_t(r.left) && x < uint32_t(r.right) && y >= uint32_t(r.top) && y < uint32_t(r.bottom))
                    memcpy(pixels[i], px + (size_t(y - uint32_t(r.top)) * w + (x - uint32_t(r.left))) * 4, 4);
                else if (!partial)
                    memset(pixels[i], 0, 4);
            }
            encodeBlock(format, pixels, used, block);
        }
    }
}

// Source taps of every destination coordinate along one axis.
struct AxisTaps
{
    std::vector<uint32_t> start;    // destination coordinate -> first tap; one more entry at the end
    std::vector<uint32_t> index;    // source coordinate
    std::vector<float>    weight;
};

// A source coordinate off the edge, wrapped or mirrored back.
uint32_t address(int32_t i, uint32_t size, bool mirror)
{
    const int32_t n = int32_t(size);
    if (!mirror)
        return uint32_t(((i % n) + n) % n);
    const int32_t m = ((i % (2 * n)) + 2 * n) % (2 * n);
    return uint32_t(m < n ? m : 2 * n - 1 - m);
}

// NONE: source coordinate d (none past the source: transparent black). POINT: the nearest texel to the centre.
// LINEAR: the two texels around the centre. TRIANGLE: a tent over max(1, scale) texels each side, so every source
// texel contributes equally. BOX: the texels the destination texel covers, weighted by coverage. Weights sum to 1.
AxisTaps axisTaps(DWORD type, uint32_t src, uint32_t dst, bool mirror)
{
    AxisTaps taps;
    const float scale = float(src) / float(dst);
    auto add = [&](int32_t i, float w)
    {
        if (w > 0.0f)
        {
            taps.index.push_back(address(i, src, mirror));
            taps.weight.push_back(w);
        }
    };
    for (uint32_t d = 0; d < dst; ++d)
    {
        const uint32_t first = uint32_t(taps.index.size());
        taps.start.push_back(first);
        const float centre = (float(d) + 0.5f) * scale - 0.5f;
        switch (type)
        {
        case D3DX_FILTER_NONE:
            if (d < src)
                add(int32_t(d), 1.0f);
            break;
        case D3DX_FILTER_POINT:
            add(std::min(int32_t((float(d) + 0.5f) * scale), int32_t(src) - 1), 1.0f);
            break;
        case D3DX_FILTER_LINEAR:
        {
            const int32_t i = int32_t(std::floor(centre));
            const float f = centre - float(i);
            add(i, 1.0f - f);
            add(i + 1, f);
            break;
        }
        case D3DX_FILTER_TRIANGLE:
        {
            const float radius = std::max(1.0f, scale);
            for (int32_t i = int32_t(std::ceil(centre - radius)); float(i) <= centre + radius; ++i)
                add(i, 1.0f - std::fabs(float(i) - centre) / radius);
            break;
        }
        default:
        {
            const float lo = float(d) * scale, hi = float(d + 1) * scale;
            for (int32_t i = int32_t(std::floor(lo)); float(i) < hi && i < int32_t(src); ++i)
                add(i, std::min(hi, float(i + 1)) - std::max(lo, float(i)));
            break;
        }
        }
        float sum = 0.0f;
        for (uint32_t t = first; t < taps.weight.size(); ++t)
            sum += taps.weight[t];
        for (uint32_t t = first; t < taps.weight.size(); ++t)
            taps.weight[t] /= sum;
    }
    taps.start.push_back(uint32_t(taps.index.size()));
    return taps;
}

float srgbToLinear(float v)
{
    return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
}

float linearToSrgb(float v)
{
    return v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
}

// Resamples BGRA pixels sw x sh to dw x dh with the filter (D3DX_FILTER_* type and flags); quantization to bytes
// rounds, adds a 4x4 ordered dither (DITHER) or diffuses the error (DITHER_DIFFUSION) when dither is set.
std::vector<uint8_t> resample(const std::vector<uint8_t>& src, uint32_t sw, uint32_t sh, uint32_t dw, uint32_t dh,
                              DWORD filter, bool dither)
{
    static const uint8_t bayer[4][4] = { { 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 } };
    const DWORD type = filter & 0xff;
    const AxisTaps xt = axisTaps(type, sw, dw, (filter & D3DX_FILTER_MIRROR_U) != 0);
    const AxisTaps yt = axisTaps(type, sh, dh, (filter & D3DX_FILTER_MIRROR_V) != 0);
    float lut[2][256];
    for (int v = 0; v < 256; ++v)
    {
        lut[0][v] = float(v) / 255.0f;
        lut[1][v] = (filter & D3DX_FILTER_SRGB_IN) ? srgbToLinear(lut[0][v]) : lut[0][v];
    }
    const bool srgbOut = (filter & D3DX_FILTER_SRGB_OUT) != 0;
    const bool diffuse = dither && (filter & D3DX_FILTER_DITHER_DIFFUSION);
    const bool ordered = dither && !diffuse && (filter & D3DX_FILTER_DITHER);
    std::vector<uint8_t> out(size_t(dw) * dh * 4);
    std::vector<float> row(size_t(dw) * 4), error(diffuse ? size_t(dw + 2) * 4 * 2 : 0);
    for (uint32_t y = 0; y < dh; ++y)
    {
        std::fill(row.begin(), row.end(), 0.0f);
        for (uint32_t ty = yt.start[y]; ty < yt.start[y + 1]; ++ty)
        {
            const uint8_t* line = src.data() + size_t(yt.index[ty]) * sw * 4;
            const float wy = yt.weight[ty];
            for (uint32_t x = 0; x < dw; ++x)
            {
                float* acc = &row[size_t(x) * 4];
                for (uint32_t tx = xt.start[x]; tx < xt.start[x + 1]; ++tx)
                {
                    const uint8_t* p = line + size_t(xt.index[tx]) * 4;
                    const float w = wy * xt.weight[tx];
                    acc[0] += w * lut[1][p[0]];
                    acc[1] += w * lut[1][p[1]];
                    acc[2] += w * lut[1][p[2]];
                    acc[3] += w * lut[0][p[3]];
                }
            }
        }
        float* errorRow = diffuse ? &error[size_t(y & 1) * (dw + 2) * 4] : nullptr;
        float* errorNext = diffuse ? &error[size_t((y + 1) & 1) * (dw + 2) * 4] : nullptr;
        if (diffuse)
            std::fill(errorNext, errorNext + size_t(dw + 2) * 4, 0.0f);
        for (uint32_t x = 0; x < dw; ++x)
        {
            for (uint32_t c = 0; c < 4; ++c)
            {
                float v = row[size_t(x) * 4 + c];
                if (srgbOut && c < 3)
                    v = linearToSrgb(std::max(0.0f, v));
                v *= 255.0f;
                int q;
                if (diffuse)
                {
                    v += errorRow[(x + 1) * 4 + c];
                    q = std::min(255, std::max(0, int(std::floor(v + 0.5f))));
                    const float e = v - float(q);
                    errorRow[(x + 2) * 4 + c] += e * (7.0f / 16.0f);
                    errorNext[x * 4 + c] += e * (3.0f / 16.0f);
                    errorNext[(x + 1) * 4 + c] += e * (5.0f / 16.0f);
                    errorNext[(x + 2) * 4 + c] += e * (1.0f / 16.0f);
                }
                else
                {
                    const float bias = ordered ? (float(bayer[y & 3][x & 3]) + 0.5f) / 16.0f : 0.5f;
                    q = std::min(255, std::max(0, int(std::floor(v + bias))));
                }
                out[(size_t(y) * dw + x) * 4 + c] = uint8_t(q);
            }
        }
    }
    return out;
}

// A texture level (or cube face) the CPU can lock: not a render target, depth surface or back buffer.
bool lockable(Surface* s)
{
    return s->role() == Surface::RoleLevel && s->container() && !s->container()->desc().target;
}

bool validRect(const RECT& r, const d3d9::D3DSURFACE_DESC& desc)
{
    return r.left >= 0 && r.top >= 0 && r.left < r.right && r.top < r.bottom && uint32_t(r.right) <= desc.Width &&
           uint32_t(r.bottom) <= desc.Height;
}

// The rectangle starts on a block and ends on a block or at the level's edge.
bool blockAligned(const RECT& r, const d3d9::D3DSURFACE_DESC& desc)
{
    return !(r.left & 3) && !(r.top & 3) && (!(r.right & 3) || uint32_t(r.right) == desc.Width) &&
           (!(r.bottom & 3) || uint32_t(r.bottom) == desc.Height);
}

}

// ---- Math ----------------------------------------------------------------------------------------------------------

D3DX_VFP_STORES
x86::reg32 D3DXMatrixMultiply(WinApplication* app, x86::CPU& cpu, x86::reg32 out, x86::reg32 a, x86::reg32 b)
{
    RC_USE(app);
    RC_USE(cpu);
    if (!((out | a | b) & 3) && (out == a || disjointMatrices(out, a)) && (out == b || disjointMatrices(out, b)))
    {
        // Aligned: b whole, then each row of a just before the same row of out is written; the same operations.
        const float* pa = alignedFloats(a);
        const float* pb = alignedFloats(b);
        float* po = alignedFloatsOut(out);
        const float b0 = pb[0], b1 = pb[1], b2 = pb[2], b3 = pb[3], b4 = pb[4], b5 = pb[5], b6 = pb[6],
                    b7 = pb[7], b8 = pb[8], b9 = pb[9], b10 = pb[10], b11 = pb[11], b12 = pb[12], b13 = pb[13],
                    b14 = pb[14], b15 = pb[15];
        for (int i = 0; i < 16; i += 4)
        {
            const float a0 = pa[i], a1 = pa[i + 1], a2 = pa[i + 2], a3 = pa[i + 3];
            const float r0 = a0 * b0 + a1 * b4 + a2 * b8 + a3 * b12;
            const float r1 = a0 * b1 + a1 * b5 + a2 * b9 + a3 * b13;
            const float r2 = a0 * b2 + a1 * b6 + a2 * b10 + a3 * b14;
            const float r3 = a0 * b3 + a1 * b7 + a2 * b11 + a3 * b15;
            po[i] = r0;
            po[i + 1] = r1;
            po[i + 2] = r2;
            po[i + 3] = r3;
        }
        return out;
    }
    alignas(16) float ma[16], mb[16], r[16];
    loadMatrix(a, ma);
    loadMatrix(b, mb);
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            r[i * 4 + j] = ma[i * 4 + 0] * mb[0 * 4 + j] + ma[i * 4 + 1] * mb[1 * 4 + j] +
                           ma[i * 4 + 2] * mb[2 * 4 + j] + ma[i * 4 + 3] * mb[3 * 4 + j];
    storeMatrix(out, r);
    return out;
}

x86::reg32 D3DXMatrixTranspose(WinApplication* app, x86::CPU& cpu, x86::reg32 out, x86::reg32 m)
{
    RC_USE(app);
    RC_USE(cpu);
    // The de-interleaving load gives the columns, stored as rows.
    const float32x4x4_t columns = vld4q_f32(reinterpret_cast<const float*>(g_guestBase + m));
    float* const r = reinterpret_cast<float*>(g_guestBase + out);
    for (int i = 0; i < 4; ++i)
        vst1q_f32(r + 4 * i, columns.val[i]);
    return out;
}

// The adjugate divided by the determinant, from the 2x2 minors of the upper and lower row pairs; null (and out
// unchanged) for a zero determinant.
x86::reg32 D3DXMatrixInverse(WinApplication* app, x86::CPU& cpu, x86::reg32 out, x86::reg32 determinant,
                             x86::reg32 m)
{
    RC_USE(app);
    RC_USE(cpu);
    alignas(16) float a[16];
    loadMatrix(m, a);
    const float s0 = a[0] * a[5] - a[4] * a[1];
    const float s1 = a[0] * a[6] - a[4] * a[2];
    const float s2 = a[0] * a[7] - a[4] * a[3];
    const float s3 = a[1] * a[6] - a[5] * a[2];
    const float s4 = a[1] * a[7] - a[5] * a[3];
    const float s5 = a[2] * a[7] - a[6] * a[3];
    const float c5 = a[10] * a[15] - a[14] * a[11];
    const float c4 = a[9] * a[15] - a[13] * a[11];
    const float c3 = a[9] * a[14] - a[13] * a[10];
    const float c2 = a[8] * a[15] - a[12] * a[11];
    const float c1 = a[8] * a[14] - a[12] * a[10];
    const float c0 = a[8] * a[13] - a[12] * a[9];
    const float det = s0 * c5 - s1 * c4 + s2 * c3 + s3 * c2 - s4 * c1 + s5 * c0;
    if (det == 0.0f)
        return 0;
    if (determinant)
        store(determinant, &det, 1);
    const float k = 1.0f / det;
    alignas(16) float r[16];
    r[0] = (a[5] * c5 - a[6] * c4 + a[7] * c3) * k;
    r[1] = (-a[1] * c5 + a[2] * c4 - a[3] * c3) * k;
    r[2] = (a[13] * s5 - a[14] * s4 + a[15] * s3) * k;
    r[3] = (-a[9] * s5 + a[10] * s4 - a[11] * s3) * k;
    r[4] = (-a[4] * c5 + a[6] * c2 - a[7] * c1) * k;
    r[5] = (a[0] * c5 - a[2] * c2 + a[3] * c1) * k;
    r[6] = (-a[12] * s5 + a[14] * s2 - a[15] * s1) * k;
    r[7] = (a[8] * s5 - a[10] * s2 + a[11] * s1) * k;
    r[8] = (a[4] * c4 - a[5] * c2 + a[7] * c0) * k;
    r[9] = (-a[0] * c4 + a[1] * c2 - a[3] * c0) * k;
    r[10] = (a[12] * s4 - a[13] * s2 + a[15] * s0) * k;
    r[11] = (-a[8] * s4 + a[9] * s2 - a[11] * s0) * k;
    r[12] = (-a[4] * c3 + a[5] * c1 - a[6] * c0) * k;
    r[13] = (a[0] * c3 - a[1] * c1 + a[2] * c0) * k;
    r[14] = (-a[12] * s3 + a[13] * s1 - a[14] * s0) * k;
    r[15] = (a[8] * s3 - a[9] * s1 + a[10] * s0) * k;
    storeMatrix(out, r);
    return out;
}

x86::reg32 D3DXMatrixTranslation(WinApplication* app, x86::CPU& cpu, x86::reg32 out, float x, float y, float z)
{
    RC_USE(app);
    RC_USE(cpu);
    const float r[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, x, y, z, 1 };
    store(out, r, 16);
    return out;
}

x86::reg32 D3DXMatrixPerspectiveLH(WinApplication* app, x86::CPU& cpu, x86::reg32 out, float w, float h, float zn,
                                   float zf)
{
    RC_USE(app);
    RC_USE(cpu);
    const float r[16] = { 2.0f * zn / w, 0, 0, 0,
                          0, 2.0f * zn / h, 0, 0,
                          0, 0, zf / (zf - zn), 1,
                          0, 0, zn * zf / (zn - zf), 0 };
    store(out, r, 16);
    return out;
}

x86::reg32 D3DXMatrixOrthoLH(WinApplication* app, x86::CPU& cpu, x86::reg32 out, float w, float h, float zn,
                             float zf)
{
    RC_USE(app);
    RC_USE(cpu);
    const float r[16] = { 2.0f / w, 0, 0, 0,
                          0, 2.0f / h, 0, 0,
                          0, 0, 1.0f / (zf - zn), 0,
                          0, 0, zn / (zn - zf), 1 };
    store(out, r, 16);
    return out;
}

x86::reg32 D3DXVec3Normalize(WinApplication* app, x86::CPU& cpu, x86::reg32 out, x86::reg32 v)
{
    RC_USE(app);
    RC_USE(cpu);
    float in[3];
    load(v, in, 3);
    const float length = std::sqrt(in[0] * in[0] + in[1] * in[1] + in[2] * in[2]);
    float r[3] = { 0, 0, 0 };
    if (length != 0.0f)
    {
        r[0] = in[0] / length;
        r[1] = in[1] / length;
        r[2] = in[2] / length;
    }
    store(out, r, 3);
    return out;
}

D3DX_VFP_STORES
x86::reg32 D3DXVec3Transform(WinApplication* app, x86::CPU& cpu, x86::reg32 out, x86::reg32 v, x86::reg32 m)
{
    RC_USE(app);
    RC_USE(cpu);
    if (!((out | v | m) & 3))
    {
        // Aligned: every input read before out is written; the same operations.
        const float* pv = alignedFloats(v);
        const float* pm = alignedFloats(m);
        const float x = pv[0], y = pv[1], z = pv[2];
        const float r0 = x * pm[0] + y * pm[4] + z * pm[8] + pm[12];
        const float r1 = x * pm[1] + y * pm[5] + z * pm[9] + pm[13];
        const float r2 = x * pm[2] + y * pm[6] + z * pm[10] + pm[14];
        const float r3 = x * pm[3] + y * pm[7] + z * pm[11] + pm[15];
        float* po = alignedFloatsOut(out);
        po[0] = r0;
        po[1] = r1;
        po[2] = r2;
        po[3] = r3;
        return out;
    }
    float in[3];
    alignas(16) float mm[16], r[4];
    load(v, in, 3);
    loadMatrix(m, mm);
    for (int j = 0; j < 4; ++j)
        r[j] = in[0] * mm[j] + in[1] * mm[4 + j] + in[2] * mm[8 + j] + mm[12 + j];
    store4(out, r);
    return out;
}

x86::reg32 D3DXVec3TransformNormal(WinApplication* app, x86::CPU& cpu, x86::reg32 out, x86::reg32 v, x86::reg32 m)
{
    RC_USE(app);
    RC_USE(cpu);
    float in[3], r[3];
    alignas(16) float mm[16];
    load(v, in, 3);
    loadMatrix(m, mm);
    for (int j = 0; j < 3; ++j)
        r[j] = in[0] * mm[j] + in[1] * mm[4 + j] + in[2] * mm[8 + j];
    store(out, r, 3);
    return out;
}

x86::reg32 D3DXVec3TransformCoordArray(WinApplication* app, x86::CPU& cpu, x86::reg32 out, UINT outStride,
                                       x86::reg32 v, UINT vStride, x86::reg32 m, UINT n)
{
    RC_USE(app);
    RC_USE(cpu);
    alignas(16) float mm[16];
    loadMatrix(m, mm);
    for (UINT i = 0; i < n; ++i)
    {
        float in[3], r[3];
        load(v + i * vStride, in, 3);
        const float w = in[0] * mm[3] + in[1] * mm[7] + in[2] * mm[11] + mm[15];
        for (int j = 0; j < 3; ++j)
            r[j] = (in[0] * mm[j] + in[1] * mm[4 + j] + in[2] * mm[8 + j] + mm[12 + j]) / w;
        store(out + i * outStride, r, 3);
    }
    return out;
}

D3DX_VFP_STORES
x86::reg32 D3DXVec4Transform(WinApplication* app, x86::CPU& cpu, x86::reg32 out, x86::reg32 v, x86::reg32 m)
{
    RC_USE(app);
    RC_USE(cpu);
    if (!((out | v | m) & 3))
    {
        // Aligned: every input read before out is written; the same operations.
        const float* pv = alignedFloats(v);
        const float* pm = alignedFloats(m);
        const float x = pv[0], y = pv[1], z = pv[2], w = pv[3];
        const float r0 = x * pm[0] + y * pm[4] + z * pm[8] + w * pm[12];
        const float r1 = x * pm[1] + y * pm[5] + z * pm[9] + w * pm[13];
        const float r2 = x * pm[2] + y * pm[6] + z * pm[10] + w * pm[14];
        const float r3 = x * pm[3] + y * pm[7] + z * pm[11] + w * pm[15];
        float* po = alignedFloatsOut(out);
        po[0] = r0;
        po[1] = r1;
        po[2] = r2;
        po[3] = r3;
        return out;
    }
    alignas(16) float in[4], mm[16], r[4];
    load4(v, in);
    loadMatrix(m, mm);
    for (int j = 0; j < 4; ++j)
        r[j] = in[0] * mm[j] + in[1] * mm[4 + j] + in[2] * mm[8 + j] + in[3] * mm[12 + j];
    store4(out, r);
    return out;
}

// ---- Textures ------------------------------------------------------------------------------------------------------

// A cube texture of the file's size with a complete mip chain, in the file's format when the renderer stores it and
// the file has every level, else in A8R8G8B8 with the missing levels made by a box filter.
HRESULT D3DXCreateCubeTextureFromFileInMemory(WinApplication* app, x86::CPU& cpu, x86::reg32 device, x86::reg32 data,
                                              UINT size, x86::reg32* texture)
{
    Device* d = deviceOf(device);
    DdsImage image;
    if (!d || !texture || !data)
        return D3DERR_INVALIDCALL;
    if (!parseDds(g_guestBase + data, size, &image) || image.faces != 6 || image.width != image.height)
    {
        pf::logError("d3dx9: cube texture data (%u bytes) is not a complete DDS cube map", size);
        return D3DXERR_INVALIDDATA;
    }
    if (!powerOfTwo(image.width))
        unsupported(app, cpu, "D3DXCreateCubeTextureFromFileInMemory", "%u x %u cube map needs resizing",
                    image.width, image.height);
    const uint32_t levels = fullChain(image.width, image.height);
    const bool keep = deviceFormat(image.format) && image.levels >= levels;
    const DWORD format = keep ? image.format : D3DFMT_A8R8G8B8;
    x86::reg32 guest = 0;
    const HRESULT created = d->CreateCubeTexture(app, cpu, image.width, levels, 0, format, 1, &guest, 0);
    if (created != D3D_OK)
        return created;
    Texture* t = static_cast<Texture*>(d3d9::lookup(guest));
    const x86::reg8* face = image.data;
    for (uint32_t f = 0; f < 6; ++f)
    {
        std::vector<uint8_t> argb;
        const x86::reg8* level = face;
        for (uint32_t l = 0; l < levels; ++l)
        {
            const uint32_t w = dim(image.width, l), h = dim(image.height, l);
            d3d9::D3DLOCKED_RECT locked;
            if (t->LockRectCube(app, cpu, f, l, &locked, nullptr, 0) != D3D_OK)
            {
                t->release();
                return D3DERR_INVALIDCALL;
            }
            uint8_t* dst = g_guestBase + locked.pBits;
            if (l < image.levels)
            {
                if (keep)
                {
                    const uint32_t pitch = levelPitch(format, w);
                    const uint32_t rows = levelBytes(format, w, h) / pitch;
                    for (uint32_t r = 0; r < rows; ++r)
                        memcpy(dst + r * uint32_t(locked.Pitch), level + r * pitch, pitch);
                }
                else
                {
                    argb.assign(size_t(w) * h * 4, 0);
                    if (!toArgb(image.format, level, levelPitch(image.format, w), w, h, argb.data()))
                    {
                        t->UnlockRectCube(app, cpu, f, l);
                        t->release();
                        unsupported(app, cpu, "D3DXCreateCubeTextureFromFileInMemory",
                                    "no conversion of format 0x%x", image.format);
                    }
                }
                level += levelBytes(image.format, w, h);
            }
            else
            {
                argb = boxHalve(argb, dim(image.width, l - 1), dim(image.height, l - 1));
            }
            if (!keep)
            {
                for (uint32_t r = 0; r < h; ++r)
                    memcpy(dst + r * uint32_t(locked.Pitch), argb.data() + r * w * 4, w * 4);
            }
            t->UnlockRectCube(app, cpu, f, l);
        }
        for (uint32_t l = 0; l < image.levels; ++l)
            face += levelBytes(image.format, dim(image.width, l), dim(image.height, l));
    }
    *texture = guest;
    return D3D_OK;
}

// A volume texture of the file's size and format with a complete mip chain; missing levels of 32-bit formats are
// made with a 2x2x2 box filter.
HRESULT D3DXCreateVolumeTextureFromFileInMemory(WinApplication* app, x86::CPU& cpu, x86::reg32 device,
                                                x86::reg32 data, UINT size, x86::reg32* texture)
{
    Device* d = deviceOf(device);
    DdsImage image;
    if (!d || !texture || !data)
        return D3DERR_INVALIDCALL;
    if (!parseDds(g_guestBase + data, size, &image) || image.faces != 1)
    {
        pf::logError("d3dx9: volume texture data (%u bytes) is not a DDS volume", size);
        return D3DXERR_INVALIDDATA;
    }
    const uint32_t levels = fullChain(image.width, image.height, image.depth);
    bool block;
    const uint32_t bits = d3d9::formatBits(image.format, &block);
    if (image.levels < levels && (block || bits != 32))
        unsupported(app, cpu, "D3DXCreateVolumeTextureFromFileInMemory",
                    "%u of %u levels in format 0x%x: the rest needs a filter for that format", image.levels, levels,
                    image.format);
    x86::reg32 guest = 0;
    const HRESULT created = d->CreateVolumeTexture(app, cpu, image.width, image.height, image.depth, levels, 0,
                                                   image.format, 1, &guest, 0);
    if (created != D3D_OK)
        return created;
    VolumeTexture* t = static_cast<VolumeTexture*>(d3d9::lookup(guest));
    const x86::reg8* level = image.data;
    for (uint32_t l = 0; l < levels; ++l)
    {
        UINT w, h, depth, row, slice;
        t->levelLayout(l, &w, &h, &depth, &row, &slice);
        uint8_t* dst = g_guestBase + t->levelData(l);
        if (l < image.levels)
        {
            memcpy(dst, level, size_t(slice) * depth);
            level += size_t(slice) * depth;
            continue;
        }
        UINT pw, ph, pd, prow, pslice;
        t->levelLayout(l - 1, &pw, &ph, &pd, &prow, &pslice);
        const uint8_t* src = g_guestBase + t->levelData(l - 1);
        for (UINT z = 0; z < depth; ++z)
            for (UINT y = 0; y < h; ++y)
                for (UINT x = 0; x < w; ++x)
                    for (UINT c = 0; c < 4; ++c)
                    {
                        uint32_t sum = 0;
                        for (UINT k = 0; k < 8; ++k)
                        {
                            const UINT sx = std::min(2 * x + (k & 1), pw - 1);
                            const UINT sy = std::min(2 * y + ((k >> 1) & 1), ph - 1);
                            const UINT sz = std::min(2 * z + (k >> 2), pd - 1);
                            sum += src[sz * pslice + sy * prow + sx * 4 + c];
                        }
                        dst[z * slice + y * row + x * 4 + c] = uint8_t((sum + 4) / 8);
                    }
    }
    t->contentsChanged();
    *texture = guest;
    return D3D_OK;
}

// The function is called for the centre of every texel of every level with (out, coordinate, texel size, data);
// its rgba result is stored in the texture's format.
HRESULT D3DXFillVolumeTexture(WinApplication* app, x86::CPU& cpu, x86::reg32 texture, x86::reg32 function,
                              x86::reg32 data)
{
    d3d9::Object* object = d3d9::lookup(texture);
    if (!object || object->kind() != d3d9::KindVolumeTexture || !function)
        return D3DERR_INVALIDCALL;
    VolumeTexture* t = static_cast<VolumeTexture*>(object);
    const DWORD format = t->format();
    switch (format)
    {
    case D3DFMT_A8R8G8B8: case D3DFMT_X8R8G8B8: case D3DFMT_L8: case D3DFMT_A8L8: case D3DFMT_A8:
    case D3DFMT_R5G6B5: case D3DFMT_A4R4G4B4: case D3DFMT_A1R5G5B5:
        break;
    default:
        unsupported(app, cpu, "D3DXFillVolumeTexture", "format 0x%x", format);
    }
    const x86::reg32 scratch = heap::allocate(48, true);
    if (!scratch)
        return D3DERR_NOTAVAILABLE;
    const x86::reg32 out = scratch, coordinate = scratch + 16, texel = scratch + 32;
    for (UINT l = 0; l < t->levelCount() && !cpu.terminate; ++l)
    {
        UINT w, h, d, row, slice;
        t->levelLayout(l, &w, &h, &d, &row, &slice);
        const float size[3] = { 1.0f / float(w), 1.0f / float(h), 1.0f / float(d) };
        store(texel, size, 3);
        for (UINT z = 0; z < d; ++z)
            for (UINT y = 0; y < h; ++y)
                for (UINT x = 0; x < w && !cpu.terminate; ++x)
                {
                    const float c[3] = { (float(x) + 0.5f) / float(w), (float(y) + 0.5f) / float(h),
                                         (float(z) + 0.5f) / float(d) };
                    store(coordinate, c, 3);
                    callStdcall(app, cpu, function, { out, coordinate, texel, data });
                    float v[4];
                    load(out, v, 4);
                    uint8_t* p = g_guestBase + t->levelData(l) + z * slice + y * row;
                    uint32_t packed;
                    switch (format)
                    {
                    case D3DFMT_A8R8G8B8:
                    case D3DFMT_X8R8G8B8:
                        p[x * 4 + 0] = unorm8(v[2]);
                        p[x * 4 + 1] = unorm8(v[1]);
                        p[x * 4 + 2] = unorm8(v[0]);
                        p[x * 4 + 3] = format == D3DFMT_A8R8G8B8 ? unorm8(v[3]) : 255;
                        break;
                    case D3DFMT_L8:
                        p[x] = unorm8(v[0]);
                        break;
                    case D3DFMT_A8L8:
                        p[x * 2] = unorm8(v[0]);
                        p[x * 2 + 1] = unorm8(v[3]);
                        break;
                    case D3DFMT_A8:
                        p[x] = unorm8(v[3]);
                        break;
                    case D3DFMT_R5G6B5:
                        packed = unormBits(v[0], 5) << 11 | unormBits(v[1], 6) << 5 | unormBits(v[2], 5);
                        p[x * 2] = uint8_t(packed);
                        p[x * 2 + 1] = uint8_t(packed >> 8);
                        break;
                    case D3DFMT_A4R4G4B4:
                        packed = unormBits(v[3], 4) << 12 | unormBits(v[0], 4) << 8 | unormBits(v[1], 4) << 4 |
                                 unormBits(v[2], 4);
                        p[x * 2] = uint8_t(packed);
                        p[x * 2 + 1] = uint8_t(packed >> 8);
                        break;
                    default:
                        packed = unormBits(v[3], 1) << 15 | unormBits(v[0], 5) << 10 | unormBits(v[1], 5) << 5 |
                                 unormBits(v[2], 5);
                        p[x * 2] = uint8_t(packed);
                        p[x * 2 + 1] = uint8_t(packed >> 8);
                        break;
                    }
                }
    }
    heap::release(scratch);
    t->contentsChanged();
    return D3D_OK;
}

// Loads a rectangle of one texture level (or cube face) into a rectangle of another (null: the whole surface): the
// source is read through a read-only lock, colour-keyed to transparent black, resampled with the filter (D3DX_DEFAULT:
// TRIANGLE | DITHER; MIRROR_U/V, DITHER, DITHER_DIFFUSION, SRGB_IN/OUT) and written in the destination format, DXT
// blocks compressed. The same size and format with no colour key or sRGB change is copied as it is. Palettes belong
// to palettized formats, which no texture of the device has. Render targets, depth and back-buffer surfaces trap.
HRESULT D3DXLoadSurfaceFromSurface(WinApplication* app, x86::CPU& cpu, x86::reg32 dst, x86::reg32 dstPalette,
                                   const RECT* dstRect, x86::reg32 src, x86::reg32 srcPalette, const RECT* srcRect,
                                   DWORD filter, DWORD colorKey)
{
    RC_USE(dstPalette);
    RC_USE(srcPalette);
    d3d9::Object* dstObject = d3d9::lookup(dst);
    d3d9::Object* srcObject = d3d9::lookup(src);
    if (!dstObject || !srcObject || dstObject->kind() != d3d9::KindSurface || srcObject->kind() != d3d9::KindSurface)
        return D3DERR_INVALIDCALL;
    Surface* d = static_cast<Surface*>(dstObject);
    Surface* s = static_cast<Surface*>(srcObject);
    d3d9::D3DSURFACE_DESC dd, sd;
    d->GetDesc(app, cpu, &dd);
    s->GetDesc(app, cpu, &sd);
    if (!lockable(d) || !lockable(s))
        unsupported(app, cpu, "D3DXLoadSurfaceFromSurface", "render target, depth or back-buffer surface (formats "
                    "0x%x to 0x%x)", sd.Format, dd.Format);
    if (!decodable(sd.Format) || !encodable(dd.Format))
        unsupported(app, cpu, "D3DXLoadSurfaceFromSurface", "format 0x%x to 0x%x", sd.Format, dd.Format);
    if (filter == D3DX_DEFAULT)
        filter = D3DX_FILTER_TRIANGLE | D3DX_FILTER_DITHER;
    const DWORD flags = D3DX_FILTER_MIRROR_U | D3DX_FILTER_MIRROR_V | D3DX_FILTER_MIRROR_W | D3DX_FILTER_DITHER |
                        D3DX_FILTER_DITHER_DIFFUSION | D3DX_FILTER_SRGB_IN | D3DX_FILTER_SRGB_OUT;
    if ((filter & 0xff) < D3DX_FILTER_NONE || (filter & 0xff) > D3DX_FILTER_BOX || (filter & ~(flags | 0xff)))
        return D3DERR_INVALIDCALL;
    const RECT sr = srcRect ? *srcRect : RECT{ 0, 0, LONG(sd.Width), LONG(sd.Height) };
    const RECT dr = dstRect ? *dstRect : RECT{ 0, 0, LONG(dd.Width), LONG(dd.Height) };
    if (!validRect(sr, sd) || !validRect(dr, dd))
        return D3DERR_INVALIDCALL;
    const uint32_t sw = uint32_t(sr.right - sr.left), sh = uint32_t(sr.bottom - sr.top);
    const uint32_t dw = uint32_t(dr.right - dr.left), dh = uint32_t(dr.bottom - dr.top);
    const bool srgbChange = ((filter & D3DX_FILTER_SRGB_IN) != 0) != ((filter & D3DX_FILTER_SRGB_OUT) != 0);
    const bool block = blockFormat(sd.Format);
    const bool raw = sd.Format == dd.Format && sw == dw && sh == dh && !colorKey && !srgbChange &&
                     (!block || (blockAligned(sr, sd) && blockAligned(dr, dd)));
    const bool whole = !dr.left && !dr.top && uint32_t(dr.right) == dd.Width && uint32_t(dr.bottom) == dd.Height;
    // A raw copy moves rows of pixels, or of blocks.
    bool isBlock;
    const uint32_t unit = block ? 4 : 1, unitBytes = d3d9::formatBits(sd.Format, &isBlock) / 8;
    const uint32_t rowBytes = (sw + unit - 1) / unit * unitBytes, rows = (sh + unit - 1) / unit;

    // The source pixels (or bytes) are taken first: source and destination may be the same surface.
    d3d9::D3DLOCKED_RECT sl, dl;
    if (s->LockRect(app, cpu, &sl, nullptr, d3d9::D3DLOCK_READONLY) != D3D_OK)
        return D3DERR_INVALIDCALL;
    std::vector<uint8_t> pixels;
    if (raw)
    {
        pixels.resize(size_t(rowBytes) * rows);
        for (uint32_t y = 0; y < rows; ++y)
            memcpy(pixels.data() + size_t(y) * rowBytes, g_guestBase + sl.pBits + (uint32_t(sr.top) / unit + y) *
                   uint32_t(sl.Pitch) + uint32_t(sr.left) / unit * unitBytes, rowBytes);
    }
    else
    {
        readRect(sd.Format, g_guestBase + sl.pBits, uint32_t(sl.Pitch), sr, pixels);
    }
    s->UnlockRect(app, cpu);
    if (!raw)
    {
        if (colorKey)
        {
            for (size_t i = 0; i < pixels.size(); i += 4)
            {
                uint32_t argb;
                memcpy(&argb, &pixels[i], 4);
                if (argb == colorKey)
                    memset(&pixels[i], 0, 4);
            }
        }
        pixels = resample(pixels, sw, sh, dw, dh, filter, !blockFormat(dd.Format));
    }

    // A rectangle covering the whole destination needs none of its old contents.
    if (d->LockRect(app, cpu, &dl, nullptr, whole ? DWORD(d3d9::D3DLOCK_DISCARD) : 0u) != D3D_OK)
        return D3DERR_INVALIDCALL;
    uint8_t* dp = g_guestBase + dl.pBits;
    if (raw)
    {
        for (uint32_t y = 0; y < rows; ++y)
            memcpy(dp + (uint32_t(dr.top) / unit + y) * uint32_t(dl.Pitch) + uint32_t(dr.left) / unit * unitBytes,
                   pixels.data() + size_t(y) * rowBytes, rowBytes);
    }
    else
    {
        writeRect(dd.Format, dp, uint32_t(dl.Pitch), dd.Width, dd.Height, dr, pixels.data());
    }
    d->UnlockRect(app, cpu);
    return D3D_OK;
}

// Image files of a surface need a back-buffer read-back the device does not have: traps.
HRESULT D3DXSaveSurfaceToFileA(WinApplication* app, x86::CPU& cpu, LPCSTR file, DWORD format, x86::reg32 surface,
                               x86::reg32 palette, const RECT* rect)
{
    RC_USE(palette);
    RC_USE(rect);
    unsupported(app, cpu, "D3DXSaveSurfaceToFileA", "%s, format %u, surface 0x%08x", file ? file : "", format,
                surface);
}

// Image files of a render target need a GPU read-back the device does not have: traps.
HRESULT D3DXSaveSurfaceToFileInMemory(WinApplication* app, x86::CPU& cpu, x86::reg32* buffer, DWORD format,
                                      x86::reg32 surface, x86::reg32 palette, const RECT* rect)
{
    RC_USE(buffer);
    RC_USE(palette);
    RC_USE(rect);
    unsupported(app, cpu, "D3DXSaveSurfaceToFileInMemory", "format %u, surface 0x%08x", format, surface);
}

}}
