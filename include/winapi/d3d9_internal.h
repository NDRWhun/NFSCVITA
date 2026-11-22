#ifndef WINAPI_D3D9_INTERNAL_H_
#define WINAPI_D3D9_INTERNAL_H_

// Direct3D 9 layer: COM objects in guest memory, Direct3D types, the device state machine that records frames
// (platform/render.h) and the binding the effect layer (d3dx9_effect.cpp) gives the device.
//
// A guest COM object is 8 bytes of guest memory: its vtable and a handle. The handle indexes the host object
// table; vtable slots hold their own guest address, under which the method is registered.

#include <lib/winapp.h>
#include <lib/memmap.h>
#include <winapi/types.h>
#include <winapi/wrapper.h>
#include <platform/log.h>
#include <platform/render.h>
#include <arm_neon.h>
#include <cstring>
#include <vector>

namespace win32 { namespace d3d9
{

namespace render = pf::render;
namespace texture = pf::texture;

// Bitwise equality of count dwords at a and b.
inline bool sameWords(const void* a, const void* b, uint32_t count)
{
    const uint8_t* p = static_cast<const uint8_t*>(a);
    const uint8_t* q = static_cast<const uint8_t*>(b);
    for (uint32_t i = 0; i < count; ++i, p += 4, q += 4)
    {
        uint32_t x, y;
        memcpy(&x, p, 4);
        memcpy(&y, q, 4);
        if (x != y)
            return false;
    }
    return true;
}

// Copies count 16-byte constant registers with inline vector moves instead of a library call.
inline void copyRegisters(void* to, const void* from, uint32_t count)
{
    uint8_t* p = static_cast<uint8_t*>(to);
    const uint8_t* q = static_cast<const uint8_t*>(from);
    for (uint32_t i = 0; i < count; ++i, p += 16, q += 16)
        vst1q_u8(p, vld1q_u8(q));
}

// Writes the transpose of a 4x4 matrix of 32-bit words (its rows as columns) with inline vector moves.
inline void transposeWords(void* to, const void* from)
{
    const uint8_t* q = static_cast<const uint8_t*>(from);
    const uint32x4x2_t a = vtrnq_u32(vreinterpretq_u32_u8(vld1q_u8(q)), vreinterpretq_u32_u8(vld1q_u8(q + 16)));
    const uint32x4x2_t b = vtrnq_u32(vreinterpretq_u32_u8(vld1q_u8(q + 32)), vreinterpretq_u32_u8(vld1q_u8(q + 48)));
    uint8_t* p = static_cast<uint8_t*>(to);
    vst1q_u8(p, vreinterpretq_u8_u32(vcombine_u32(vget_low_u32(a.val[0]), vget_low_u32(b.val[0]))));
    vst1q_u8(p + 16, vreinterpretq_u8_u32(vcombine_u32(vget_low_u32(a.val[1]), vget_low_u32(b.val[1]))));
    vst1q_u8(p + 32, vreinterpretq_u8_u32(vcombine_u32(vget_high_u32(a.val[0]), vget_high_u32(b.val[0]))));
    vst1q_u8(p + 48, vreinterpretq_u8_u32(vcombine_u32(vget_high_u32(a.val[1]), vget_high_u32(b.val[1]))));
}

// Zeroes count 16-byte constant registers with inline vector stores.
inline void clearRegisters(void* to, uint32_t count)
{
    uint8_t* p = static_cast<uint8_t*>(to);
    for (uint32_t i = 0; i < count; ++i, p += 16)
        vst1q_u8(p, vdupq_n_u8(0));
}

// ---- Constants and types -------------------------------------------------------------------------------------------

enum : HRESULT
{
    D3D_OK                  = 0,
    S_FALSE                 = 1,
    D3DERR_MOREDATA         = 0x887600A3,
    D3DERR_NOTFOUND         = 0x88760866,
    D3DERR_NOTAVAILABLE     = 0x8876086A,
    D3DERR_INVALIDCALL      = 0x8876086C,
    D3DERR_OUTOFVIDEOMEMORY = 0x8876017C,
    E_NOINTERFACE           = 0x80004002,
    E_FAIL                  = 0x80004005
};

enum : DWORD
{
    D3DFMT_UNKNOWN          = 0,
    D3DFMT_A8R8G8B8         = 21,
    D3DFMT_X8R8G8B8         = 22,
    D3DFMT_D24S8            = 75,
    D3DFMT_D24X8            = 77,
    D3DFMT_D16              = 80,
    D3DFMT_INDEX16          = 101,
    D3DFMT_INDEX32          = 102,
    D3DFMT_A16B16G16R16F    = 113,
    D3DFMT_DXT1             = 0x31545844,
    D3DFMT_DXT3             = 0x33545844,
    D3DFMT_DXT5             = 0x35545844
};

enum : DWORD
{
    D3DUSAGE_RENDERTARGET   = 0x1,
    D3DUSAGE_DEPTHSTENCIL   = 0x2,
    D3DUSAGE_DYNAMIC        = 0x200,
    D3DUSAGE_AUTOGENMIPMAP  = 0x400,

    D3DLOCK_READONLY        = 0x10,
    D3DLOCK_DISCARD         = 0x2000,
    D3DLOCK_NOOVERWRITE     = 0x1000,

    D3DRTYPE_SURFACE        = 1,
    D3DRTYPE_VOLUME         = 2,
    D3DRTYPE_TEXTURE        = 3,
    D3DRTYPE_VOLUMETEXTURE  = 4,
    D3DRTYPE_CUBETEXTURE    = 5,
    D3DRTYPE_VERTEXBUFFER   = 6,
    D3DRTYPE_INDEXBUFFER    = 7
};

struct D3DMATRIX
{
    float m[16];        // rows
};

struct D3DVIEWPORT9
{
    DWORD X, Y, Width, Height;
    float MinZ, MaxZ;
};

struct D3DPRESENT_PARAMETERS
{
    UINT    BackBufferWidth;
    UINT    BackBufferHeight;
    DWORD   BackBufferFormat;
    UINT    BackBufferCount;
    DWORD   MultiSampleType;
    DWORD   MultiSampleQuality;
    DWORD   SwapEffect;
    HWND    hDeviceWindow;
    BOOL    Windowed;
    BOOL    EnableAutoDepthStencil;
    DWORD   AutoDepthStencilFormat;
    DWORD   Flags;
    UINT    FullScreen_RefreshRateInHz;
    UINT    PresentationInterval;
};

struct D3DDISPLAYMODE
{
    UINT    Width, Height, RefreshRate;
    DWORD   Format;
};

struct D3DLOCKED_RECT
{
    LONG    Pitch;
    DWORD   pBits;      // guest address
};

struct D3DSURFACE_DESC
{
    DWORD   Format, Type, Usage, Pool, MultiSampleType, MultiSampleQuality;
    UINT    Width, Height;
};

struct D3DVERTEXBUFFER_DESC
{
    DWORD   Format, Type, Usage, Pool;
    UINT    Size;
    DWORD   FVF;
};

struct D3DINDEXBUFFER_DESC
{
    DWORD   Format, Type, Usage, Pool;
    UINT    Size;
};

struct D3DVERTEXELEMENT9
{
    WORD    Stream;
    WORD    Offset;
    BYTE    Type;
    BYTE    Method;
    BYTE    Usage;
    BYTE    UsageIndex;
};

struct D3DRECT
{
    LONG x1, y1, x2, y2;
};

struct D3DADAPTER_IDENTIFIER9
{
    char    Driver[512];
    char    Description[512];
    char    DeviceName[32];
    DWORD   DriverVersionLowPart;
    DWORD   DriverVersionHighPart;
    DWORD   VendorId;
    DWORD   DeviceId;
    DWORD   SubSysId;
    DWORD   Revision;
    GUID    DeviceIdentifier;
    DWORD   WHQLLevel;
};

struct D3DDEVICE_CREATION_PARAMETERS
{
    UINT    AdapterOrdinal;
    DWORD   DeviceType;
    HWND    hFocusWindow;
    DWORD   BehaviorFlags;
};

// D3DCAPS9 as 76 DWORDs; the indices of the fields this layer sets.
enum CapsField
{
    CapsDeviceType = 0, CapsAdapterOrdinal, CapsCaps, CapsCaps2, CapsCaps3, CapsPresentationIntervals,
    CapsCursorCaps, CapsDevCaps, CapsPrimitiveMiscCaps, CapsRasterCaps, CapsZCmpCaps, CapsSrcBlendCaps,
    CapsDestBlendCaps, CapsAlphaCmpCaps, CapsShadeCaps, CapsTextureCaps, CapsTextureFilterCaps,
    CapsCubeTextureFilterCaps, CapsVolumeTextureFilterCaps, CapsTextureAddressCaps, CapsVolumeTextureAddressCaps,
    CapsLineCaps, CapsMaxTextureWidth, CapsMaxTextureHeight, CapsMaxVolumeExtent, CapsMaxTextureRepeat,
    CapsMaxTextureAspectRatio, CapsMaxAnisotropy, CapsMaxVertexW, CapsGuardBandLeft, CapsGuardBandTop,
    CapsGuardBandRight, CapsGuardBandBottom, CapsExtentsAdjust, CapsStencilCaps, CapsFVFCaps, CapsTextureOpCaps,
    CapsMaxTextureBlendStages, CapsMaxSimultaneousTextures, CapsVertexProcessingCaps, CapsMaxActiveLights,
    CapsMaxUserClipPlanes, CapsMaxVertexBlendMatrices, CapsMaxVertexBlendMatrixIndex, CapsMaxPointSize,
    CapsMaxPrimitiveCount, CapsMaxVertexIndex, CapsMaxStreams, CapsMaxStreamStride, CapsVertexShaderVersion,
    CapsMaxVertexShaderConst, CapsPixelShaderVersion, CapsPixelShader1xMaxValue, CapsDevCaps2,
    CapsMaxNpatchTessellationLevel, CapsReserved5, CapsMasterAdapterOrdinal, CapsAdapterOrdinalInGroup,
    CapsNumberOfAdaptersInGroup, CapsDeclTypes, CapsNumSimultaneousRTs, CapsStretchRectFilterCaps,
    CapsVS20Caps, CapsPS20Caps = CapsVS20Caps + 4, CapsVertexTextureFilterCaps = CapsPS20Caps + 5,
    CapsMaxVShaderInstructionsExecuted, CapsMaxPShaderInstructionsExecuted, CapsMaxVertexShader30InstructionSlots,
    CapsMaxPixelShader30InstructionSlots,
    CapsCount = 76
};

struct D3DCAPS9
{
    DWORD field[CapsCount];
};

// ---- Objects -------------------------------------------------------------------------------------------------------

enum Kind
{
    KindDirect3D,
    KindDevice,
    KindTexture,
    KindCubeTexture,
    KindSurface,
    KindVertexBuffer,
    KindIndexBuffer,
    KindVertexDeclaration,
    KindEffect,
    KindEffectPool,
    KindVolumeTexture,
    KindVolume,
    KindQuery,
    KindVertexShader,
    KindPixelShader,
    KindCount
};

class Object
{
public:
    explicit Object(Kind kind) : m_kind(kind), m_refs(1), m_guest(0) {}
    virtual ~Object();

    Kind kind() const               { return m_kind; }
    x86::reg32 guest() const        { return m_guest; }
    // Deletes the object (and frees its guest block) on the last reference. Texture surfaces and volumes count
    // their container's references, as in Direct3D.
    virtual ULONG addRef()          { return ++m_refs; }
    virtual ULONG release();

    HRESULT QueryInterface(WinApplication* app, x86::CPU& cpu, const GUID* iid, x86::reg32* object);
    ULONG AddRef(WinApplication* app, x86::CPU& cpu);
    ULONG Release(WinApplication* app, x86::CPU& cpu);
    // IDirect3DResource9 private data: bytes by GUID, or a COM pointer holding a reference (D3DSPD_IUNKNOWN).
    HRESULT SetPrivateData(WinApplication* app, x86::CPU& cpu, const GUID* guid, x86::reg32 data, DWORD size,
                           DWORD flags);
    HRESULT GetPrivateData(WinApplication* app, x86::CPU& cpu, const GUID* guid, x86::reg32 data, DWORD* size);
    HRESULT FreePrivateData(WinApplication* app, x86::CPU& cpu, const GUID* guid);

private:
    friend x86::reg32 publish(WinApplication* app, Object* object);

    struct PrivateData
    {
        GUID                    guid;
        std::vector<uint8_t>    bytes;
        x86::reg32              object;     // D3DSPD_IUNKNOWN: the pointer, with a reference
    };
    void freePrivate(size_t index);

    Kind                        m_kind;
    uint32_t                    m_refs;
    x86::reg32                  m_guest;
    std::vector<PrivateData>    m_private;
};

// Gives a host object its guest COM object (the vtable of its kind) and returns the guest address.
x86::reg32 publish(WinApplication* app, Object* object);

// The guest range of the COM objects and the host objects by handle, as lookup reads them.
struct ObjectTable
{
    x86::reg32  start = 0, end = 0;
    Object**    objects = nullptr;
    uint32_t    count = 0;
};
extern ObjectTable g_objectTable;
// lookup of a pointer that is 0 or not a live object: null, logged unless 0.
Object* lookupFailed(x86::reg32 guest);

// The host object of a guest COM pointer, inlined into every caller; null for 0 or an unknown pointer (logged).
__attribute__((always_inline)) inline Object* lookup(x86::reg32 guest)
{
    const ObjectTable& t = g_objectTable;
    if (guest - t.start < t.end - t.start)
    {
        uint32_t handle;
        memcpy(&handle, g_guestBase + guest + 4, 4);
        if (handle < t.count && t.objects[handle])
            return t.objects[handle];
    }
    return lookupFailed(guest);
}
// Builds the vtables and registers the methods, once.
void initInterfaces(WinApplication* app);

// A vtable slot: the method, or null for one that logs its name and guest address and traps.
struct MethodEntry
{
    const char* name;
    MethodPtr   method;
};
// Builds the vtable of a kind; slots in interface order.
void buildInterface(WinApplication* app, Kind kind, const char* interfaceName, const MethodEntry* methods,
                    uint32_t count);
// The effect interfaces (d3dx9_effect.cpp).
void initEffectInterfaces(WinApplication* app);

#define D3D_METHOD(cls, fn) { #fn, &win32::Wrapper<decltype(&cls::fn), &cls::fn>::stdcall }
#define D3D_NAMED(name, cls, fn) { name, &win32::Wrapper<decltype(&cls::fn), &cls::fn>::stdcall }
#define D3D_TRAP(fn) { #fn, nullptr }

// Logs a message once per call site.
#define D3D_LOG_ONCE(...) do { static bool logged_ = false; if (!logged_) { logged_ = true; pf::logInfo(__VA_ARGS__); } } while (0)

class Device;
class Texture;
class Surface;
class Buffer;
class VertexDeclaration;

class Direct3D : public Object
{
public:
    Direct3D() : Object(KindDirect3D) {}

    HRESULT RegisterSoftwareDevice(WinApplication* app, x86::CPU& cpu, x86::reg32 init);
    UINT GetAdapterCount(WinApplication* app, x86::CPU& cpu);
    HRESULT GetAdapterIdentifier(WinApplication* app, x86::CPU& cpu, UINT adapter, DWORD flags,
                                 D3DADAPTER_IDENTIFIER9* identifier);
    UINT GetAdapterModeCount(WinApplication* app, x86::CPU& cpu, UINT adapter, DWORD format);
    HRESULT EnumAdapterModes(WinApplication* app, x86::CPU& cpu, UINT adapter, DWORD format, UINT mode,
                             D3DDISPLAYMODE* out);
    HRESULT GetAdapterDisplayMode(WinApplication* app, x86::CPU& cpu, UINT adapter, D3DDISPLAYMODE* out);
    HRESULT CheckDeviceType(WinApplication* app, x86::CPU& cpu, UINT adapter, DWORD type, DWORD displayFormat,
                            DWORD backFormat, BOOL windowed);
    HRESULT CheckDeviceFormat(WinApplication* app, x86::CPU& cpu, UINT adapter, DWORD type, DWORD adapterFormat,
                              DWORD usage, DWORD resourceType, DWORD format);
    HRESULT CheckDeviceMultiSampleType(WinApplication* app, x86::CPU& cpu, UINT adapter, DWORD type, DWORD format,
                                       BOOL windowed, DWORD multiSample, DWORD* qualityLevels);
    HRESULT CheckDepthStencilMatch(WinApplication* app, x86::CPU& cpu, UINT adapter, DWORD type, DWORD adapterFormat,
                                   DWORD targetFormat, DWORD depthFormat);
    HRESULT CheckDeviceFormatConversion(WinApplication* app, x86::CPU& cpu, UINT adapter, DWORD type,
                                        DWORD sourceFormat, DWORD targetFormat);
    HRESULT GetDeviceCaps(WinApplication* app, x86::CPU& cpu, UINT adapter, DWORD type, D3DCAPS9* caps);
    HANDLE GetAdapterMonitor(WinApplication* app, x86::CPU& cpu, UINT adapter);
    HRESULT CreateDevice(WinApplication* app, x86::CPU& cpu, UINT adapter, DWORD type, HWND window, DWORD flags,
                         D3DPRESENT_PARAMETERS* params, x86::reg32* device);

    static void fillCaps(D3DCAPS9* caps);
};

// Textures, cube textures and their surfaces. A texture may be stored smaller than its Direct3D size (the reduced
// shadow map): it reports and is drawn at its Direct3D size, its scenes scaled to the storage.
class Texture : public Object
{
public:
    Texture(Device* device, const render::TextureDesc& desc, DWORD format, DWORD usage, DWORD pool, UINT width,
            UINT height);
    ~Texture() override;

    render::TextureStorage* storage() const    { return m_storage; }
    const render::TextureDesc& desc() const    { return m_desc; }
    DWORD format() const                        { return m_format; }
    DWORD usage() const                         { return m_usage; }
    Device* device() const                      { return m_device; }
    bool valid() const                          { return m_storage != nullptr; }
    void fillDesc(UINT level, D3DSURFACE_DESC* desc) const;
    // The surface of a level (and face), kept while the texture lives; one reference added for the caller.
    Surface* surface(WinApplication* app, uint32_t face, uint32_t level);

    HRESULT GetDevice(WinApplication* app, x86::CPU& cpu, x86::reg32* device);
    DWORD SetPriority(WinApplication* app, x86::CPU& cpu, DWORD priority);
    DWORD GetPriority(WinApplication* app, x86::CPU& cpu);
    void PreLoad(WinApplication* app, x86::CPU& cpu);
    DWORD GetType(WinApplication* app, x86::CPU& cpu);
    DWORD SetLOD(WinApplication* app, x86::CPU& cpu, DWORD lod);
    DWORD GetLOD(WinApplication* app, x86::CPU& cpu);
    DWORD GetLevelCount(WinApplication* app, x86::CPU& cpu);
    HRESULT SetAutoGenFilterType(WinApplication* app, x86::CPU& cpu, DWORD filter);
    DWORD GetAutoGenFilterType(WinApplication* app, x86::CPU& cpu);
    void GenerateMipSubLevels(WinApplication* app, x86::CPU& cpu);
    HRESULT GetLevelDesc(WinApplication* app, x86::CPU& cpu, UINT level, D3DSURFACE_DESC* desc);
    HRESULT GetSurfaceLevel(WinApplication* app, x86::CPU& cpu, UINT level, x86::reg32* surface);
    HRESULT LockRect(WinApplication* app, x86::CPU& cpu, UINT level, D3DLOCKED_RECT* locked, const RECT* rect,
                     DWORD flags);
    HRESULT UnlockRect(WinApplication* app, x86::CPU& cpu, UINT level);
    HRESULT AddDirtyRect(WinApplication* app, x86::CPU& cpu, const RECT* rect);
    HRESULT GetCubeMapSurface(WinApplication* app, x86::CPU& cpu, DWORD face, UINT level, x86::reg32* surface);
    HRESULT LockRectCube(WinApplication* app, x86::CPU& cpu, DWORD face, UINT level, D3DLOCKED_RECT* locked,
                         const RECT* rect, DWORD flags);
    HRESULT UnlockRectCube(WinApplication* app, x86::CPU& cpu, DWORD face, UINT level);
    HRESULT AddDirtyRectCube(WinApplication* app, x86::CPU& cpu, DWORD face, const RECT* rect);
    // A movie frame for the locked level 0 (streamPlanes in d3d9.h); false when the texture cannot show it.
    bool streamPlanes(uint32_t pitch, const render::Planes& planes);

private:
    HRESULT lock(uint32_t face, uint32_t level, D3DLOCKED_RECT* locked, const RECT* rect, DWORD flags);
    HRESULT unlock(uint32_t face, uint32_t level);

    Device*                 m_device;
    render::TextureDesc     m_desc;
    DWORD                   m_format, m_usage, m_pool;
    UINT                    m_width, m_height;      // the Direct3D size
    DWORD                   m_autoGenFilter = 2;    // D3DTEXF_LINEAR
    DWORD                   m_lod = 0;
    DWORD                   m_priority = 0;
    render::TextureStorage* m_storage;
    std::vector<MemMap*>    m_locks;        // per face * levels + level: the guest buffer of a lock
    std::vector<DWORD>      m_lockFlags;
    std::vector<bool>       m_written;      // the level has been written since creation
    std::vector<uint8_t>    m_lockCounts;   // locks so far, saturating at 2
    std::vector<MemMap*>    m_shadows;      // linear copy kept for levels locked repeatedly (movie frames)
    std::vector<Surface*>   m_surfaces;
    render::TextureStorage* m_planes = nullptr; // movie frame planes the current lock shows from its unlock on
};

struct D3DVOLUME_DESC
{
    DWORD   Format, Type, Usage, Pool;
    UINT    Width, Height, Depth;
};

struct D3DLOCKED_BOX
{
    LONG    RowPitch;
    LONG    SlicePitch;
    DWORD   pBits;      // guest address
};

struct D3DBOX
{
    UINT    Left, Top, Right, Bottom, Front, Back;
};

// Bits per pixel of a Direct3D format, or per 4x4 block for the DXT formats (block set); 0 when unknown.
uint32_t formatBits(DWORD format, bool* block);

class Volume;

// Volume textures: every level in guest memory, rows and slices packed; locks, the description and the D3DX loaders
// work on that data. The renderer has no 3D textures: what is sampled is a 2D atlas of level 0 (32-bit formats),
// its depth slices side by side, slice z covering u in [z / depth, (z + 1) / depth) and all of v. A program reads
// coordinate (u, v, w) as the two slices around w * depth - 0.5, each at u' = (z + clamp(u, 0.5 / width,
// 1 - 0.5 / width)) / depth, and blends them; the atlas uses linear filtering and clamping.
class VolumeTexture : public Object
{
public:
    VolumeTexture(Device* device, UINT width, UINT height, UINT depth, UINT levels, DWORD format, DWORD usage,
                  DWORD pool);
    ~VolumeTexture() override;

    bool valid() const                  { return !m_levels.empty(); }
    UINT levelCount() const             { return UINT(m_levels.size()); }
    DWORD format() const                { return m_format; }
    Device* device() const              { return m_device; }
    // Size, pitches and guest address of a level.
    void levelLayout(UINT level, UINT* width, UINT* height, UINT* depth, UINT* rowPitch, UINT* slicePitch) const;
    x86::reg32 levelData(UINT level) const;
    void fillDesc(UINT level, D3DVOLUME_DESC* desc) const;
    // Level 0 was written outside a lock (the D3DX fills): the atlas is rebuilt before it is next sampled.
    void contentsChanged()              { m_dirty = true; }
    // The atlas as it is now, rebuilt from level 0 when written since; null for formats it cannot hold.
    const render::TextureStorage* storage();

    HRESULT GetDevice(WinApplication* app, x86::CPU& cpu, x86::reg32* device);
    DWORD SetPriority(WinApplication* app, x86::CPU& cpu, DWORD priority);
    DWORD GetPriority(WinApplication* app, x86::CPU& cpu);
    void PreLoad(WinApplication* app, x86::CPU& cpu);
    DWORD GetType(WinApplication* app, x86::CPU& cpu);
    DWORD SetLOD(WinApplication* app, x86::CPU& cpu, DWORD lod);
    DWORD GetLOD(WinApplication* app, x86::CPU& cpu);
    DWORD GetLevelCount(WinApplication* app, x86::CPU& cpu);
    HRESULT SetAutoGenFilterType(WinApplication* app, x86::CPU& cpu, DWORD filter);
    DWORD GetAutoGenFilterType(WinApplication* app, x86::CPU& cpu);
    void GenerateMipSubLevels(WinApplication* app, x86::CPU& cpu);
    HRESULT GetLevelDesc(WinApplication* app, x86::CPU& cpu, UINT level, D3DVOLUME_DESC* desc);
    HRESULT GetVolumeLevel(WinApplication* app, x86::CPU& cpu, UINT level, x86::reg32* volume);
    HRESULT LockBox(WinApplication* app, x86::CPU& cpu, UINT level, D3DLOCKED_BOX* locked, const D3DBOX* box,
                    DWORD flags);
    HRESULT UnlockBox(WinApplication* app, x86::CPU& cpu, UINT level);
    HRESULT AddDirtyBox(WinApplication* app, x86::CPU& cpu, const D3DBOX* box);

    HRESULT lock(UINT level, D3DLOCKED_BOX* locked, const D3DBOX* box, DWORD flags);
    HRESULT unlock(UINT level);

private:
    Device*                 m_device;
    UINT                    m_width, m_height, m_depth;
    DWORD                   m_format, m_usage, m_pool;
    DWORD                   m_autoGenFilter = 2;
    DWORD                   m_lod = 0;
    DWORD                   m_priority = 0;
    std::vector<MemMap*>    m_levels;
    std::vector<bool>       m_locked;
    std::vector<DWORD>      m_lockFlags;
    std::vector<Volume*>    m_volumes;
    render::TextureStorage* m_atlas = nullptr;
    bool                    m_dirty = true;
};

// A level of a volume texture (IDirect3DVolume9); counts its container's references.
class Volume : public Object
{
public:
    Volume(VolumeTexture* container, UINT level) : Object(KindVolume), m_container(container), m_level(level) {}

    ULONG addRef() override             { return m_container->addRef(); }
    ULONG release() override            { return m_container->release(); }

    HRESULT GetDevice(WinApplication* app, x86::CPU& cpu, x86::reg32* device);
    HRESULT GetContainer(WinApplication* app, x86::CPU& cpu, const GUID* iid, x86::reg32* container);
    HRESULT GetDesc(WinApplication* app, x86::CPU& cpu, D3DVOLUME_DESC* desc);
    HRESULT LockBox(WinApplication* app, x86::CPU& cpu, D3DLOCKED_BOX* locked, const D3DBOX* box, DWORD flags);
    HRESULT UnlockBox(WinApplication* app, x86::CPU& cpu);

private:
    VolumeTexture*  m_container;
    UINT            m_level;
};

class Surface : public Object
{
public:
    enum Role
    {
        RoleLevel,          // a level (face) of a texture
        RoleBackBuffer,
        RoleDepth           // a depth surface, or the level of a depth texture (with a container)
    };

    Surface(Device* device, Role role, uint32_t width, uint32_t height, DWORD format);
    ~Surface() override;

    Role role() const                       { return m_role; }
    uint32_t width() const                  { return m_width; }
    uint32_t height() const                 { return m_height; }
    Texture* container() const              { return m_container; }
    uint32_t face() const                   { return m_face; }
    uint32_t level() const                  { return m_level; }
    render::DepthStorage* depth() const     { return m_depth; }
    void setContainer(Texture* texture, uint32_t face, uint32_t level);
    void setDepth(render::DepthStorage* depth) { m_depth = depth; }
    ULONG addRef() override;
    ULONG release() override;

    HRESULT GetDevice(WinApplication* app, x86::CPU& cpu, x86::reg32* device);
    DWORD SetPriority(WinApplication* app, x86::CPU& cpu, DWORD priority);
    DWORD GetPriority(WinApplication* app, x86::CPU& cpu);
    void PreLoad(WinApplication* app, x86::CPU& cpu);
    DWORD GetType(WinApplication* app, x86::CPU& cpu);
    HRESULT GetContainer(WinApplication* app, x86::CPU& cpu, const GUID* iid, x86::reg32* container);
    HRESULT GetDesc(WinApplication* app, x86::CPU& cpu, D3DSURFACE_DESC* desc);
    HRESULT LockRect(WinApplication* app, x86::CPU& cpu, D3DLOCKED_RECT* locked, const RECT* rect, DWORD flags);
    HRESULT UnlockRect(WinApplication* app, x86::CPU& cpu);

private:
    Device*                 m_device;
    Role                    m_role;
    uint32_t                m_width, m_height;
    DWORD                   m_format;
    Texture*                m_container = nullptr;
    uint32_t                m_face = 0, m_level = 0;
    render::DepthStorage*   m_depth = nullptr;      // owned for RoleDepth without a container
    DWORD                   m_priority = 0;
};

// Event queries (IDirect3DQuery9 of type D3DQUERYTYPE_EVENT): done once the GPU has finished every frame up to the
// one the query was issued in.
class Query : public Object
{
public:
    Query(Device* device, DWORD type);
    ~Query() override;

    HRESULT GetDevice(WinApplication* app, x86::CPU& cpu, x86::reg32* device);
    DWORD GetType(WinApplication* app, x86::CPU& cpu);
    DWORD GetDataSize(WinApplication* app, x86::CPU& cpu);
    HRESULT Issue(WinApplication* app, x86::CPU& cpu, DWORD flags);
    HRESULT GetData(WinApplication* app, x86::CPU& cpu, x86::reg32 data, DWORD size, DWORD flags);

private:
    Device*     m_device;
    DWORD       m_type;
    uint32_t    m_frame = 0;        // frame of the last Issue(END); 0: none, done
};

// Vertex and pixel shader objects: the bytecode as given; programs come from the effect layer, so draws with a shader
// object set and no effect pass are dropped (logged once per object).
class Shader : public Object
{
public:
    Shader(Device* device, Kind kind, const uint8_t* code, uint32_t bytes);
    ~Shader() override;

    const std::vector<uint8_t>& code() const    { return m_code; }
    // The draw drop is logged once.
    bool logDrop()                              { const bool first = !m_dropLogged; m_dropLogged = true; return first; }

    HRESULT GetDevice(WinApplication* app, x86::CPU& cpu, x86::reg32* device);
    HRESULT GetFunction(WinApplication* app, x86::CPU& cpu, x86::reg32 data, UINT* size);

    // Bytes of a shader token stream up to and with its END token; 0 when none within limit bytes.
    static uint32_t length(const uint8_t* code, uint32_t limit);

private:
    Device*                 m_device;
    std::vector<uint8_t>    m_code;
    bool                    m_dropLogged = false;
};

// Vertex and index buffers: the guest writes a shadow copy in guest memory; unlocks copy into the GPU buffer,
// a new one when the current is still read by frames in flight (unless NOOVERWRITE). A buffer that keeps its shadow
// leaves the written bytes pending at unlock: draws copy them up to the end of what they read.
class Buffer : public Object
{
public:
    Buffer(Device* device, Kind kind, UINT size, DWORD usage, DWORD pool, DWORD format);
    ~Buffer() override;

    render::BufferStorage* storage() const  { return m_storage; }
    bool valid() const                      { return m_storage != nullptr; }
    UINT size() const                       { return m_size; }
    DWORD format() const                    { return m_format; }

    HRESULT GetDevice(WinApplication* app, x86::CPU& cpu, x86::reg32* device);
    DWORD SetPriority(WinApplication* app, x86::CPU& cpu, DWORD priority);
    DWORD GetPriority(WinApplication* app, x86::CPU& cpu);
    void PreLoad(WinApplication* app, x86::CPU& cpu);
    DWORD GetType(WinApplication* app, x86::CPU& cpu);
    HRESULT Lock(WinApplication* app, x86::CPU& cpu, UINT offset, UINT size, x86::reg32* data, DWORD flags);
    HRESULT Unlock(WinApplication* app, x86::CPU& cpu);
    HRESULT GetVertexDesc(WinApplication* app, x86::CPU& cpu, D3DVERTEXBUFFER_DESC* desc);
    HRESULT GetIndexDesc(WinApplication* app, x86::CPU& cpu, D3DINDEXBUFFER_DESC* desc);

    // A draw reads bytes [begin, end) of the GPU copy: the pending bytes up to end are copied first.
    void prepare(UINT begin, UINT end)
    {
        if (begin < m_pendingEnd && end > m_pendingBegin)
            copyPending(end);
    }

private:
    // Copies the locked range of the guest copy to the GPU copy, or leaves it pending.
    void update();
    void copyPending(UINT end);

    Device*                 m_device;
    UINT                    m_size;
    DWORD                   m_usage, m_pool, m_format;
    MemMap*                 m_shadow = nullptr;     // guest copy while locked; kept for dynamic or relocked buffers
    render::BufferStorage*  m_storage;
    UINT                    m_lockOffset = 0, m_lockSize = 0;
    DWORD                   m_lockFlags = 0;
    int                     m_locks = 0;
    uint8_t                 m_lockCount = 0;        // first locks so far, saturating at 2
    bool                    m_written = false;
    UINT                    m_pendingBegin = 0, m_pendingEnd = 0;  // shadow bytes not yet in the GPU copy
};

class VertexDeclaration : public Object
{
public:
    VertexDeclaration(Device* device, const D3DVERTEXELEMENT9* elements, uint32_t count, DWORD fvf);

    DWORD fvf() const                           { return m_fvf; }
    // The layout of these elements at a stream stride (registered once per stride).
    uint8_t layout(uint32_t stride);
    // Components of the TEXCOORD0 input (0: none).
    uint32_t texComponents() const              { return m_texComponents; }
    bool positionT() const                      { return m_layout.positionT; }

    HRESULT GetDevice(WinApplication* app, x86::CPU& cpu, x86::reg32* device);
    HRESULT GetDeclaration(WinApplication* app, x86::CPU& cpu, D3DVERTEXELEMENT9* elements, UINT* count);

    // Layout of an FVF code; stride from the FVF.
    static render::VertexLayout fvfLayout(DWORD fvf, uint32_t* texComponents);

private:
    Device*                         m_device;
    std::vector<D3DVERTEXELEMENT9>  m_elements;
    DWORD                           m_fvf;
    render::VertexLayout            m_layout;   // stride 0
    uint32_t                        m_texComponents = 0;
    uint32_t                        m_stride = 0;
    uint8_t                         m_layoutId = 0;
};

// ---- Recording: the game thread records state changes and draws; the render thread resolves them -----------------

// Commands of a frame's stream: a header word (Command in bits 0-7, arguments above) and payload words.
enum Command : uint8_t
{
    CmdRenderState,         // header: state << 8; value
    CmdStageState,          // header: stage << 8 | type << 16; value
    CmdSamplerState,        // header: sampler << 8 | type << 16; value
    CmdTransform,           // header: transform << 8 (Transform); 16 floats
    CmdViewport,            // D3DVIEWPORT9
    CmdFixedState,          // FixedState, whole
    CmdConstants,           // header: pixel << 8 | first register << 16; register count; 4 floats a register
    CmdDraw,                // DrawRecord
    CmdNativeDraw,          // NativeDrawRecord
    CmdNativeCheck          // render check build: NativeCheckRecord, then its vertex and fragment constant floats
};

enum Transform : uint8_t
{
    TransformWorld, TransformView, TransformProjection, TransformTexture0, TransformCount
};

// Growing word buffer, one per frame in flight.
struct Stream
{
    uint32_t*   data = nullptr;
    uint32_t    size = 0, capacity = 0;

    uint32_t* append(uint32_t words)
    {
        if (size + words > capacity)
            grow(words);
        uint32_t* p = data + size;
        size += words;
        return p;
    }
    void grow(uint32_t words);
};

// The stream of a frame index; game thread. Waits for nothing: the frame slots keep it free.
Stream& frameStream(uint32_t frameIndex);
// Render thread: completes the draws of a frame from its stream (Frame::resolve).
void resolveFrame(render::Frame& frame);

// Device state the draws read, mirrored on the render thread.
struct FixedState
{
    DWORD           renderStates[256];
    DWORD           stageStates[8][33];
    DWORD           samplerStates[16][14];
    D3DMATRIX       transforms[TransformCount];
    D3DVIEWPORT9    viewport;
};

enum : uint8_t
{
    NoEffect = 0xff
};

// The programs of a draw's effect pass; kind NoEffect: fixed function (no pass, or a pass without shaders).
struct EffectDraw
{
    uint8_t     vertexProgram, fragmentProgram;     // render::VertexProgram, render::FragmentProgram
    uint8_t     kind;
    uint8_t     pad;
};

struct DrawRecord
{
    uint32_t                        draw;       // index in Frame::draws; the game thread filled its geometry
    uint8_t                         primitive, layout, texComponents, positionT;
    EffectDraw                      effect;
    const render::TextureStorage*   textures[render::MaxSamplers];  // the device's stages 0..
    const render::BufferStorage*    vertexStorage;  // the vertex buffer the geometry is in; null: frame arena
};

// A draw the native renderer built on the game thread (geometry, viewport and constants already in the frame): its
// state before the alpha test elision and the blend factor resolution, which the render thread applies.
struct NativeDrawRecord
{
    uint32_t                        draw;           // index in Frame::draws
    render::DrawState               state;
    float                           test[2];        // alpha test constants
    DWORD                           blendOp, blendFactor;
    const render::BufferStorage*    vertexStorage;
};

// Render check build: the native renderer's draw for the draw the translated path recorded at index draw, its
// constants after the record.
struct NativeCheckRecord
{
    NativeDrawRecord                native;
    const void*                     vertices;
    const uint16_t*                 indices;
    uint32_t                        count, vertexCount;
    uint32_t                        vertexFloats, fragmentFloats;
};

// What an effect draw is made of: the device state, the stage textures, the constant registers and the pass's
// programs (the resolver's mirrors, or the native renderer's).
struct EffectSource
{
    const FixedState*               state;
    const render::TextureStorage*   textures[render::MaxSamplers];
    const float*                    vertexRegisters;
    const float*                    pixelRegisters;
    EffectDraw                      programs;
    uint8_t                         layout, primitive;
};

// The state of an effect draw before the alpha test elision and the blend factor resolution; test gets the alpha
// test constants, volume and volume2 the volume atlases sampled.
void effectState(const EffectSource& source, render::DrawState& state, float test[2],
                 const render::TextureStorage** volume, const render::TextureStorage** volume2);
// Its constant blocks, the programs' constantFloats each (vc or fc null when the program has none).
void effectConstants(const EffectSource& source, const render::DrawState& state, const float test[2],
                     const render::TextureStorage* volume, const render::TextureStorage* volume2, float* vc,
                     float* fc);
// The sampled storage of a texture, cube or volume texture object (null for others).
const render::TextureStorage* storageOf(Object* object);

// The geometry of an indexed draw of the native renderer, from the device's stream source and indices.
struct NativeGeometry
{
    render::DrawCall        call;           // vertices, indices, count, vertexCount
    uint8_t                 layout, primitive;
    render::BufferStorage*  vertexStorage;
    render::BufferStorage*  indexStorage;
};

// A draw of the native renderer: its state (alpha test elision and blend factor resolution left to the render
// thread) and constant blocks.
struct NativeDraw
{
    render::DrawState       state;
    float                   test[2];
    DWORD                   blendOp, blendFactor;
    const float*            vertexConstants;
    const float*            fragmentConstants;
    uint32_t                vertexFloats, fragmentFloats;
    bool                    reuse;              // the blocks of the previous native draw (nativeConstantsKept)
};

// ---- Effects (d3dx9_effect.cpp) ------------------------------------------------------------------------------------

// The active pass of an effect. Its states, textures and shader constants are on the device (as D3DX sets them),
// so a draw needs only the pass's programs.
class EffectPass
{
public:
    virtual ~EffectPass() {}
    // A draw of a pass that is not drawable is dropped; the pass logs why.
    virtual void dropped() const = 0;

    bool        drawable = true;        // false: the pass has no program
    EffectDraw  programs = { 0, 0, NoEffect, 0 };
};

// ---- Fixed-function resolution (d3d9_state.cpp, pure) ----------------------------------------------------------------

// Texture stage state indices and values used here.
enum : DWORD
{
    D3DTSS_COLOROP = 1, D3DTSS_COLORARG1 = 2, D3DTSS_COLORARG2 = 3, D3DTSS_ALPHAOP = 4, D3DTSS_ALPHAARG1 = 5,
    D3DTSS_ALPHAARG2 = 6, D3DTSS_TEXCOORDINDEX = 11, D3DTSS_TEXTURETRANSFORMFLAGS = 24, D3DTSS_COUNT = 33,

    D3DTOP_DISABLE = 1, D3DTOP_SELECTARG1 = 2, D3DTOP_SELECTARG2 = 3, D3DTOP_MODULATE = 4, D3DTOP_MODULATE2X = 5,
    D3DTOP_MODULATE4X = 6, D3DTOP_ADD = 7,

    D3DTA_DIFFUSE = 0, D3DTA_CURRENT = 1, D3DTA_TEXTURE = 2, D3DTA_TFACTOR = 3, D3DTA_SELECTMASK = 0xf,

    D3DTTFF_DISABLE = 0, D3DTTFF_COUNT2 = 2, D3DTTFF_COUNT3 = 3, D3DTTFF_COUNT4 = 4, D3DTTFF_PROJECTED = 0x100
};

struct FixedStage
{
    DWORD colorOp, colorArg1, colorArg2;
    DWORD alphaOp, alphaArg1, alphaArg2;
    DWORD transformFlags;
    bool  textured;         // a texture is bound to stage 0
};

struct FixedResult
{
    uint8_t fragmentProgram;    // FpFixedColor, FpFixedTexture or FpFixedProjected
    float   textureMask[4];
    float   scale[4];
    float   addMask[4];
    float   diffuseMask[4];
    float   factorMask[4];
};

// Stage 0 colour and alpha operations as program and masks; unsupported operations are logged and approximated.
FixedResult resolveFixed(const FixedStage& stage);
// Texture coordinate padding for an input of n components: missing components become (.., 1, 0).
void texturePad(uint32_t components, float pad[4]);
// Alpha test constants (sign, threshold) for D3DCMP func and ALPHAREF; false: the test always passes.
bool alphaTest(DWORD func, DWORD ref, float out[2]);

struct BlendFactorDraw
{
    DWORD   src, dst;           // D3DBLEND; BLENDFACTOR / INVBLENDFACTOR replaced
    uint8_t output;             // render::FactorOutput
    float   factor[4];          // uBlendFactor (rgba)
};
// BLENDFACTOR (14) and INVBLENDFACTOR (15) blends as fixed factors and a program factor output. variants: the
// program has factor variants and no alpha test; alphaWrite: the colour mask writes alpha. What is not exact is
// logged and approximated (14 as ONE, 15 as ZERO).
BlendFactorDraw resolveBlendFactor(DWORD src, DWORD dst, DWORD op, DWORD factor, bool variants, bool alphaWrite);
// Row-vector product out = a * b.
void multiply(const float* a, const float* b, float* out);

// ---- Device --------------------------------------------------------------------------------------------------------

enum : DWORD
{
    MaxRenderStates = 256,
    MaxStages = 8,
    MaxSamplerStates = 14,
    MaxSamplerStages = 16
};

class Device : public Object
{
public:
    Device(WinApplication* app, Direct3D* d3d, const D3DPRESENT_PARAMETERS& params, HWND window, DWORD flags);
    ~Device() override;

    void setEffectPass(const EffectPass* pass)  { m_effectPass = pass; }
    const EffectPass* effectPass() const        { return m_effectPass; }
    // The stream of the frame being recorded; null between frames.
    Stream* stream() const                      { return m_stream; }

    HRESULT TestCooperativeLevel(WinApplication* app, x86::CPU& cpu);
    UINT GetAvailableTextureMem(WinApplication* app, x86::CPU& cpu);
    HRESULT EvictManagedResources(WinApplication* app, x86::CPU& cpu);
    HRESULT GetDirect3D(WinApplication* app, x86::CPU& cpu, x86::reg32* d3d);
    HRESULT GetDeviceCaps(WinApplication* app, x86::CPU& cpu, D3DCAPS9* caps);
    HRESULT GetDisplayMode(WinApplication* app, x86::CPU& cpu, UINT swapChain, D3DDISPLAYMODE* mode);
    HRESULT GetCreationParameters(WinApplication* app, x86::CPU& cpu, D3DDEVICE_CREATION_PARAMETERS* params);
    HRESULT SetCursorProperties(WinApplication* app, x86::CPU& cpu, UINT x, UINT y, x86::reg32 surface);
    void SetCursorPosition(WinApplication* app, x86::CPU& cpu, int x, int y, DWORD flags);
    BOOL ShowCursor(WinApplication* app, x86::CPU& cpu, BOOL show);
    UINT GetNumberOfSwapChains(WinApplication* app, x86::CPU& cpu);
    HRESULT Reset(WinApplication* app, x86::CPU& cpu, D3DPRESENT_PARAMETERS* params);
    HRESULT Present(WinApplication* app, x86::CPU& cpu, const RECT* source, const RECT* dest, HWND window,
                    x86::reg32 dirtyRegion);
    HRESULT GetBackBuffer(WinApplication* app, x86::CPU& cpu, UINT swapChain, UINT index, DWORD type,
                          x86::reg32* surface);
    HRESULT GetRasterStatus(WinApplication* app, x86::CPU& cpu, UINT swapChain, DWORD* status);
    void SetDialogBoxMode(WinApplication* app, x86::CPU& cpu, BOOL enable);
    void SetGammaRamp(WinApplication* app, x86::CPU& cpu, UINT swapChain, DWORD flags, const WORD* ramp);
    void GetGammaRamp(WinApplication* app, x86::CPU& cpu, UINT swapChain, WORD* ramp);
    HRESULT CreateTexture(WinApplication* app, x86::CPU& cpu, UINT width, UINT height, UINT levels, DWORD usage,
                          DWORD format, DWORD pool, x86::reg32* texture, x86::reg32 sharedHandle);
    HRESULT CreateCubeTexture(WinApplication* app, x86::CPU& cpu, UINT edge, UINT levels, DWORD usage, DWORD format,
                              DWORD pool, x86::reg32* texture, x86::reg32 sharedHandle);
    HRESULT CreateVolumeTexture(WinApplication* app, x86::CPU& cpu, UINT width, UINT height, UINT depth, UINT levels,
                                DWORD usage, DWORD format, DWORD pool, x86::reg32* texture, x86::reg32 sharedHandle);
    HRESULT CreateVertexBuffer(WinApplication* app, x86::CPU& cpu, UINT length, DWORD usage, DWORD fvf, DWORD pool,
                               x86::reg32* buffer, x86::reg32 sharedHandle);
    HRESULT CreateIndexBuffer(WinApplication* app, x86::CPU& cpu, UINT length, DWORD usage, DWORD format, DWORD pool,
                              x86::reg32* buffer, x86::reg32 sharedHandle);
    HRESULT CreateRenderTarget(WinApplication* app, x86::CPU& cpu, UINT width, UINT height, DWORD format,
                               DWORD multiSample, DWORD quality, BOOL lockable, x86::reg32* surface,
                               x86::reg32 sharedHandle);
    HRESULT CreateDepthStencilSurface(WinApplication* app, x86::CPU& cpu, UINT width, UINT height, DWORD format,
                                      DWORD multiSample, DWORD quality, BOOL discard, x86::reg32* surface,
                                      x86::reg32 sharedHandle);
    HRESULT StretchRect(WinApplication* app, x86::CPU& cpu, Surface* source, const RECT* sourceRect, Surface* dest,
                        const RECT* destRect, DWORD filter);
    HRESULT ColorFill(WinApplication* app, x86::CPU& cpu, Surface* surface, const RECT* rect, DWORD color);
    HRESULT SetRenderTarget(WinApplication* app, x86::CPU& cpu, DWORD index, Surface* surface);
    HRESULT GetRenderTarget(WinApplication* app, x86::CPU& cpu, DWORD index, x86::reg32* surface);
    HRESULT SetDepthStencilSurface(WinApplication* app, x86::CPU& cpu, Surface* surface);
    HRESULT GetDepthStencilSurface(WinApplication* app, x86::CPU& cpu, x86::reg32* surface);
    HRESULT BeginScene(WinApplication* app, x86::CPU& cpu);
    HRESULT EndScene(WinApplication* app, x86::CPU& cpu);
    HRESULT Clear(WinApplication* app, x86::CPU& cpu, DWORD count, const D3DRECT* rects, DWORD flags, DWORD color,
                  float z, DWORD stencil);
    HRESULT SetTransform(WinApplication* app, x86::CPU& cpu, DWORD state, const D3DMATRIX* matrix);
    HRESULT GetTransform(WinApplication* app, x86::CPU& cpu, DWORD state, D3DMATRIX* matrix);
    HRESULT MultiplyTransform(WinApplication* app, x86::CPU& cpu, DWORD state, const D3DMATRIX* matrix);
    HRESULT SetViewport(WinApplication* app, x86::CPU& cpu, const D3DVIEWPORT9* viewport);
    HRESULT GetViewport(WinApplication* app, x86::CPU& cpu, D3DVIEWPORT9* viewport);
    HRESULT SetMaterial(WinApplication* app, x86::CPU& cpu, const DWORD* material);
    HRESULT GetMaterial(WinApplication* app, x86::CPU& cpu, DWORD* material);
    HRESULT SetLight(WinApplication* app, x86::CPU& cpu, DWORD index, const DWORD* light);
    HRESULT GetLight(WinApplication* app, x86::CPU& cpu, DWORD index, DWORD* light);
    HRESULT LightEnable(WinApplication* app, x86::CPU& cpu, DWORD index, BOOL enable);
    HRESULT GetLightEnable(WinApplication* app, x86::CPU& cpu, DWORD index, BOOL* enable);
    HRESULT SetClipPlane(WinApplication* app, x86::CPU& cpu, DWORD index, const float* plane);
    HRESULT GetClipPlane(WinApplication* app, x86::CPU& cpu, DWORD index, float* plane);
    HRESULT SetRenderState(WinApplication* app, x86::CPU& cpu, DWORD state, DWORD value);
    HRESULT GetRenderState(WinApplication* app, x86::CPU& cpu, DWORD state, DWORD* value);
    HRESULT SetClipStatus(WinApplication* app, x86::CPU& cpu, const DWORD* status);
    HRESULT GetClipStatus(WinApplication* app, x86::CPU& cpu, DWORD* status);
    HRESULT GetTexture(WinApplication* app, x86::CPU& cpu, DWORD stage, x86::reg32* texture);
    HRESULT SetTexture(WinApplication* app, x86::CPU& cpu, DWORD stage, Object* texture);
    HRESULT GetTextureStageState(WinApplication* app, x86::CPU& cpu, DWORD stage, DWORD type, DWORD* value);
    HRESULT SetTextureStageState(WinApplication* app, x86::CPU& cpu, DWORD stage, DWORD type, DWORD value);
    HRESULT GetSamplerState(WinApplication* app, x86::CPU& cpu, DWORD sampler, DWORD type, DWORD* value);
    HRESULT SetSamplerState(WinApplication* app, x86::CPU& cpu, DWORD sampler, DWORD type, DWORD value);
    HRESULT ValidateDevice(WinApplication* app, x86::CPU& cpu, DWORD* passes);
    HRESULT SetPaletteEntries(WinApplication* app, x86::CPU& cpu, UINT palette, const DWORD* entries);
    HRESULT GetPaletteEntries(WinApplication* app, x86::CPU& cpu, UINT palette, DWORD* entries);
    HRESULT SetCurrentTexturePalette(WinApplication* app, x86::CPU& cpu, UINT palette);
    HRESULT GetCurrentTexturePalette(WinApplication* app, x86::CPU& cpu, UINT* palette);
    HRESULT SetScissorRect(WinApplication* app, x86::CPU& cpu, const RECT* rect);
    HRESULT GetScissorRect(WinApplication* app, x86::CPU& cpu, RECT* rect);
    HRESULT SetSoftwareVertexProcessing(WinApplication* app, x86::CPU& cpu, BOOL software);
    BOOL GetSoftwareVertexProcessing(WinApplication* app, x86::CPU& cpu);
    HRESULT SetNPatchMode(WinApplication* app, x86::CPU& cpu, float segments);
    HRESULT DrawPrimitive(WinApplication* app, x86::CPU& cpu, DWORD type, UINT start, UINT count);
    HRESULT DrawIndexedPrimitive(WinApplication* app, x86::CPU& cpu, DWORD type, LONG baseVertex, UINT minIndex,
                                 UINT vertices, UINT startIndex, UINT count);
    HRESULT DrawPrimitiveUP(WinApplication* app, x86::CPU& cpu, DWORD type, UINT count, const void* data,
                            UINT stride);
    HRESULT DrawIndexedPrimitiveUP(WinApplication* app, x86::CPU& cpu, DWORD type, UINT minIndex, UINT vertices,
                                   UINT count, const void* indices, DWORD indexFormat, const void* data,
                                   UINT stride);
    HRESULT CreateVertexDeclaration(WinApplication* app, x86::CPU& cpu, const D3DVERTEXELEMENT9* elements,
                                    x86::reg32* declaration);
    HRESULT SetVertexDeclaration(WinApplication* app, x86::CPU& cpu, VertexDeclaration* declaration);
    HRESULT GetVertexDeclaration(WinApplication* app, x86::CPU& cpu, x86::reg32* declaration);
    HRESULT SetFVF(WinApplication* app, x86::CPU& cpu, DWORD fvf);
    HRESULT GetFVF(WinApplication* app, x86::CPU& cpu, DWORD* fvf);
    HRESULT CreateVertexShader(WinApplication* app, x86::CPU& cpu, x86::reg32 function, x86::reg32* shader);
    HRESULT SetVertexShader(WinApplication* app, x86::CPU& cpu, Object* shader);
    HRESULT GetVertexShader(WinApplication* app, x86::CPU& cpu, x86::reg32* shader);
    HRESULT SetVertexShaderConstantF(WinApplication* app, x86::CPU& cpu, UINT start, const float* data, UINT count);
    HRESULT GetVertexShaderConstantF(WinApplication* app, x86::CPU& cpu, UINT start, float* data, UINT count);
    HRESULT SetVertexShaderConstantI(WinApplication* app, x86::CPU& cpu, UINT start, const int32_t* data,
                                     UINT count);
    HRESULT GetVertexShaderConstantI(WinApplication* app, x86::CPU& cpu, UINT start, int32_t* data, UINT count);
    HRESULT SetVertexShaderConstantB(WinApplication* app, x86::CPU& cpu, UINT start, const BOOL* data, UINT count);
    HRESULT GetVertexShaderConstantB(WinApplication* app, x86::CPU& cpu, UINT start, BOOL* data, UINT count);
    HRESULT SetStreamSource(WinApplication* app, x86::CPU& cpu, UINT stream, Buffer* buffer, UINT offset,
                            UINT stride);
    HRESULT GetStreamSource(WinApplication* app, x86::CPU& cpu, UINT stream, x86::reg32* buffer, UINT* offset,
                            UINT* stride);
    HRESULT SetStreamSourceFreq(WinApplication* app, x86::CPU& cpu, UINT stream, UINT divider);
    HRESULT GetStreamSourceFreq(WinApplication* app, x86::CPU& cpu, UINT stream, UINT* divider);
    HRESULT SetIndices(WinApplication* app, x86::CPU& cpu, Buffer* buffer);
    HRESULT GetIndices(WinApplication* app, x86::CPU& cpu, x86::reg32* buffer);
    HRESULT CreatePixelShader(WinApplication* app, x86::CPU& cpu, x86::reg32 function, x86::reg32* shader);
    HRESULT SetPixelShader(WinApplication* app, x86::CPU& cpu, Object* shader);
    HRESULT GetPixelShader(WinApplication* app, x86::CPU& cpu, x86::reg32* shader);
    HRESULT SetPixelShaderConstantF(WinApplication* app, x86::CPU& cpu, UINT start, const float* data, UINT count);
    HRESULT GetPixelShaderConstantF(WinApplication* app, x86::CPU& cpu, UINT start, float* data, UINT count);
    HRESULT SetPixelShaderConstantI(WinApplication* app, x86::CPU& cpu, UINT start, const int32_t* data,
                                    UINT count);
    HRESULT GetPixelShaderConstantI(WinApplication* app, x86::CPU& cpu, UINT start, int32_t* data, UINT count);
    HRESULT SetPixelShaderConstantB(WinApplication* app, x86::CPU& cpu, UINT start, const BOOL* data, UINT count);
    HRESULT GetPixelShaderConstantB(WinApplication* app, x86::CPU& cpu, UINT start, BOOL* data, UINT count);
    HRESULT CreateQuery(WinApplication* app, x86::CPU& cpu, DWORD type, x86::reg32* query);

    // Shader objects and constants as set, for the effect layer (null: none).
    Shader* vertexShader() const                { return m_vertexShader; }
    Shader* pixelShader() const                 { return m_pixelShader; }
    const float* vertexConstantsF() const       { return m_vsFloat; }
    const float* pixelConstantsF() const        { return m_psFloat; }
    const int32_t* vertexConstantsI() const     { return m_vsInt; }
    const int32_t* pixelConstantsI() const      { return m_psInt; }
    const BOOL* vertexConstantsB() const        { return m_vsBool; }
    const BOOL* pixelConstantsB() const         { return m_psBool; }
    // Writes float constant registers (vertex or pixel); changed registers are recorded for the render thread.
    // Returns the write's stamp, which the registers keep until written again; 0 out of range.
    uint64_t setConstants(bool pixel, uint32_t first, const float* values, uint32_t count);
    // Registers [first, first + count) still hold what the setConstants call with this stamp wrote.
    bool constantsHeld(bool pixel, uint32_t first, uint32_t count, uint64_t stamp) const
    {
        const uint64_t* stamps = (pixel ? m_psStamps : m_vsStamps) + first;
        for (uint32_t i = 0; i < count; ++i)
        {
            if (stamps[i] != stamp)
                return false;
        }
        return true;
    }
#if NFSC_RENDER_CHECK
    // Render check build: the registers the effect layer's reference path computes for the setConstants call just
    // made; each draw compares the banks' registers written since the previous draw with these.
    void referenceConstants(bool pixel, uint32_t first, const float* values, uint32_t count);
    // Draws after reference writes whose registers were compared, and those with a register that differed.
    uint64_t constantCheckDraws() const         { return m_constantCheck.draws; }
    uint64_t constantCheckDiffer() const        { return m_constantCheck.differ; }
#endif

    // Native renderer (d3d9_native.cpp): the state draws read, the stage textures, the constant write counter.
    const FixedState& fixedState() const        { return m_state; }
    Object* stageTexture(uint32_t stage) const  { return m_state.textures[stage]; }
    uint64_t constantStamp() const              { return m_constantStamp; }
    // Logs of the fixed state words and the constant registers written: entry n of a log is at n % WriteLogSize,
    // and a reader that saw count c lost entries when the count moved more than WriteLogSize past it.
    struct Written
    {
        uint16_t    first, count;   // state words or registers; registers of the pixel bank have first | 0x8000
    };
    enum : uint32_t { WriteLogSize = 512 };
    uint32_t stateWrites() const                { return m_stateWrites; }
    uint32_t constantWrites() const             { return m_constantWrites; }
    const Written& stateWritten(uint32_t n) const       { return m_stateLog[n % WriteLogSize]; }
    const Written& constantWritten(uint32_t n) const    { return m_constantLog[n % WriteLogSize]; }
    // The geometry of an indexed draw from the stream source and indices; false when they cannot draw.
    bool nativeGeometry(DWORD type, UINT vertices, UINT startIndex, UINT count, NativeGeometry& out);
    // Writes a native draw's constant blocks (its vertexFloats and fragmentFloats floats; null when none).
    typedef void (*NativeBlocks)(const void* context, float* vertex, float* fragment);
    // Records a native draw; textures: stages 0.. as the draw reads them (marked read by the frame). New constant
    // blocks are written into the frame by blocks when given, else copied from the draw's.
    void nativeDraw(WinApplication* app, x86::CPU& cpu, const NativeGeometry& geometry,
                    const render::TextureStorage* const* textures, const NativeDraw& draw,
                    NativeBlocks blocks = nullptr, const void* context = nullptr);
    // Whether the previous native draw's constant blocks are in the frame being recorded.
    bool nativeConstantsKept() const            { return m_frame && m_nativeFrame == m_frame->index; }
    // Draws recorded so far and the frame index of the last.
    uint32_t drawSerial() const                 { return m_drawSerial; }
    uint32_t lastDrawIndex() const              { return m_lastDraw; }
    // Render check build: the native renderer's draw for the recorded draw at index draw.
    void recordNativeCheck(uint32_t draw, const NativeGeometry& geometry, const NativeDraw& native);

    // Event queries: the frame an Issue(END) belongs to, and whether that frame has been submitted (game thread).
    uint32_t issueFrame() const                 { return m_frame ? m_frame->index : m_submitted; }
    uint32_t submittedFrame() const             { return m_submitted; }

    enum : UINT
    {
        VertexFloatConstants = 256,
        PixelFloatConstants = 224,
        IntConstants = 16,
        BoolConstants = 16
    };

private:
    struct State : FixedState
    {
        Object*             textures[MaxSamplerStages];
        VertexDeclaration*  declaration;
        DWORD               fvf;
        Buffer*             stream;
        UINT                streamOffset, streamStride;
        Buffer*             indices;
        Surface*            target;
        Surface*            depth;
    };

    void defaults();
    void resetBackBuffer(const D3DPRESENT_PARAMETERS& params);
    template< typename T > void hold(T*& slot, T* object);
    render::Frame* frame(WinApplication* app, x86::CPU& cpu);
    // The scene of the current render target and depth surface, or of a given target.
    render::Scene* scene();
    render::Scene* scene(const render::Target& target, uint16_t width, uint16_t height);
    // The render target a surface is drawn into (colour only) and its size; false when it cannot be one.
    bool targetOf(Surface* surface, render::Target* target, uint16_t* width, uint16_t* height) const;
    // Appends a quad over the window rectangle rect of the scene: a clear of colour argb, or (texture set) a copy of
    // the source rectangle uv (u0, v0, u1, v1) of the texture's level.
    HRESULT quad(render::Frame& frame, render::Scene& scene, const RECT& rect, DWORD argb,
                 const render::TextureStorage* texture, const float uv[4], bool linear, uint32_t level);
    uint16_t viewportIndex(render::Frame& frame, const render::Viewport& viewport);
    // Records a state change for the render thread (header and one value word).
    void record(uint32_t header, DWORD value);
    void recordWords(uint32_t header, const void* payload, uint32_t words);
    // Appends fixed state words [first, first + count) to the state log; logAllState marks the whole state written.
    void logState(uint32_t first, uint32_t count)
    {
        m_stateLog[m_stateWrites % WriteLogSize] = Written{ uint16_t(first), uint16_t(count) };
        ++m_stateWrites;
    }
    void logAllState()                          { m_stateWrites += WriteLogSize + 1; }
    void submit();
    HRESULT draw(WinApplication* app, x86::CPU& cpu, DWORD type, UINT primitives, const void* vertices,
                 const uint16_t* indices, uint32_t vertexCount, uint8_t layout, uint32_t texComponents,
                 bool positionT, render::BufferStorage* vertexStorage, render::BufferStorage* indexStorage);
    VertexDeclaration* fvfDeclaration(DWORD fvf);

    WinApplication*         m_app;
    Direct3D*               m_d3d;
    D3DPRESENT_PARAMETERS   m_params;
    HWND                    m_window;
    DWORD                   m_flags;
    State                   m_state;
    Surface*                m_backBuffer = nullptr;
    Surface*                m_autoDepth = nullptr;
    const EffectPass*       m_effectPass = nullptr;
    std::vector<VertexDeclaration*> m_fvfDeclarations;
    DWORD                   m_lights[8][26] = {};
    BOOL                    m_lightEnabled[8] = {};
    DWORD                   m_material[17] = {};
    float                   m_clipPlanes[6][4] = {};
    DWORD                   m_clipStatus[2] = {};
    RECT                    m_scissor = {};
    float                   m_nPatchSegments = 0.0f;
    UINT                    m_streamFrequency[16] = {};
    std::vector<std::vector<DWORD>> m_palettes;     // PALETTEENTRY rgba as DWORDs, 256 per palette
    UINT                    m_currentPalette = 0xffff;
    WORD                    m_gammaRamp[3][256];
    bool                    m_gammaIdentity = true;
    float                   m_gammaExponent = 1.0f;     // the ramp is colour^m_gammaExponent; 0 when it is not
    Shader*                 m_vertexShader = nullptr;
    Shader*                 m_pixelShader = nullptr;
    float                   m_vsFloat[VertexFloatConstants * 4] = {};
    float                   m_psFloat[PixelFloatConstants * 4] = {};
    uint64_t                m_constantStamp = 0;    // setConstants calls so far
    uint64_t                m_vsStamps[VertexFloatConstants] = {};  // per register: the stamp of its last write
    uint64_t                m_psStamps[PixelFloatConstants] = {};
    Written                 m_stateLog[WriteLogSize] = {};
    Written                 m_constantLog[WriteLogSize] = {};
    uint32_t                m_stateWrites = 0, m_constantWrites = 0;
    int32_t                 m_vsInt[IntConstants * 4] = {};
    int32_t                 m_psInt[IntConstants * 4] = {};
    BOOL                    m_vsBool[BoolConstants] = {};
    BOOL                    m_psBool[BoolConstants] = {};
    uint32_t                m_submitted = 0;        // index of the last frame submitted

    // Recording
    render::Frame*          m_frame = nullptr;
    bool                    m_sceneOpen = false;    // m_frame's last scene is the current target
    uint32_t                m_vblanks;
    bool                    m_frameFull = false;
    Stream*                 m_stream = nullptr;
    bool                    m_stateDirty = true;    // changed while no frame recorded: sent whole before a draw
    bool                    m_constantsDirty = true;    // float constants changed while no frame recorded
    uint32_t                m_drawSerial = 0, m_lastDraw = 0;
    uint32_t                m_nativeFrame = 0;          // frame of the native constant blocks below
    uint32_t                m_nativeVertex = render::NoConstants, m_nativeFragment = render::NoConstants;
#if NFSC_RENDER_CHECK
    // The float banks as the reference writes leave them, and the registers written since the last draw.
    struct ConstantCheck
    {
        float       vertex[VertexFloatConstants * 4] = {};
        float       pixel[PixelFloatConstants * 4] = {};
        uint32_t    low[2] = { VertexFloatConstants, PixelFloatConstants };
        uint32_t    high[2] = { 0, 0 };
        bool        referenced = false;
        uint64_t    draws = 0, differ = 0;
        uint32_t    reported = 0;
    };
    ConstantCheck           m_constantCheck;
    // Copies registers into the check banks (reference: they came from the effect layer's reference path).
    void checkWrite(bool pixel, uint32_t first, const float* values, uint32_t count, bool reference);
    // Compares the registers written since the last draw with the check banks.
    void checkConstants();
#endif
};

}}

namespace win32
{

// Guest COM pointers as host objects in method arguments (null for 0).
#define D3D9_UNPACKER(type)                                                     \
template<> struct Unpacker<type*>                                               \
{                                                                               \
    static inline type* unpack(WinApplication* app, x86::reg32 address)        \
    {                                                                           \
        return static_cast<type*>(d3d9::lookup(app->getMemory<x86::reg32>(address))); \
    }                                                                           \
};

D3D9_UNPACKER(d3d9::Object)
D3D9_UNPACKER(d3d9::Direct3D)
D3D9_UNPACKER(d3d9::Device)
D3D9_UNPACKER(d3d9::Texture)
D3D9_UNPACKER(d3d9::Surface)
D3D9_UNPACKER(d3d9::Buffer)
D3D9_UNPACKER(d3d9::VertexDeclaration)
D3D9_UNPACKER(d3d9::VolumeTexture)
D3D9_UNPACKER(d3d9::Volume)
D3D9_UNPACKER(d3d9::Query)
D3D9_UNPACKER(d3d9::Shader)

}

#endif
