// Direct3D resources: textures (2D, cube, render targets), surfaces, vertex and index buffers, vertex
// declarations. Guest locks go to guest memory; unlocks write the GPU copy (a new one when frames in flight
// still read the current one).

#include <winapi/d3d9_internal.h>
#include <platform/config.h>
#include <platform/gxm.h>
#include <platform/log.h>
#include <platform/perf.h>
#include <algorithm>

namespace win32 { namespace d3d9
{

namespace
{

pf::gxm::Device& gpu()
{
    return pf::gxm::Device::get();
}

uint8_t* host(x86::reg32 address)
{
    return g_guestBase + address;
}

x86::reg32 guestOf(Object* object)
{
    return object ? object->guest() : 0;
}

// The texture whose level 0 was locked last, whole, while it stays locked, and the guest address of its pixels.
Texture* s_lastLocked = nullptr;
x86::reg32 s_lastBits = 0;

}

bool streamPlanes(x86::reg32 bits, uint32_t pitch, const render::Planes& planes)
{
    return s_lastLocked && s_lastBits == bits && s_lastLocked->streamPlanes(pitch, planes);
}

// ---- Texture -------------------------------------------------------------------------------------------------------

Texture::Texture(Device* device, const render::TextureDesc& desc, DWORD format, DWORD usage, DWORD pool, UINT width,
                 UINT height)
    :   Object(desc.cube ? KindCubeTexture : KindTexture)
    ,   m_device(device)
    ,   m_desc(desc)
    ,   m_format(format)
    ,   m_usage(usage)
    ,   m_pool(pool)
    ,   m_width(width)
    ,   m_height(height)
    ,   m_storage(gpu().createTexture(desc))
{
    device->addRef();
    const uint32_t count = (desc.cube ? 6 : 1) * desc.levels;
    m_locks.assign(count, nullptr);
    m_lockFlags.assign(count, 0);
    m_written.assign(count, false);
    m_lockCounts.assign(count, 0);
    m_shadows.assign(count, nullptr);
    m_surfaces.assign(count, nullptr);
}

Texture::~Texture()
{
    for (size_t i = 0; i < m_locks.size(); ++i)
    {
        if (m_locks[i] != m_shadows[i])
            delete m_locks[i];
        delete m_shadows[i];
    }
    for (Surface* surface : m_surfaces)
        delete surface;
    if (s_lastLocked == this)
        s_lastLocked = nullptr;
    if (m_planes && m_planes != m_storage)
        gpu().release(m_planes);
    if (m_storage)
        gpu().release(m_storage);
    m_device->release();
}

void Texture::fillDesc(UINT level, D3DSURFACE_DESC* desc) const
{
    desc->Format = m_format;
    desc->Type = D3DRTYPE_SURFACE;
    desc->Usage = m_usage;
    desc->Pool = m_pool;
    desc->MultiSampleType = 0;
    desc->MultiSampleQuality = 0;
    desc->Width = texture::levelDim(m_width, level);
    desc->Height = texture::levelDim(m_height, level);
}

Surface* Texture::surface(WinApplication* app, uint32_t face, uint32_t level)
{
    const uint32_t index = face * m_desc.levels + level;
    Surface*& s = m_surfaces[index];
    if (!s)
    {
        const bool depth = m_desc.format == texture::FormatDepth;
        s = new Surface(m_device, depth ? Surface::RoleDepth : Surface::RoleLevel, texture::levelDim(m_width, level),
                        texture::levelDim(m_height, level), m_format);
        s->setContainer(this, face, level);
        if (depth && m_storage)
            s->setDepth(m_storage->depth);
        publish(app, s);
    }
    s->addRef();
    return s;
}

HRESULT Texture::GetDevice(WinApplication* app, x86::CPU& cpu, x86::reg32* device)
{
    RC_USE(app);
    RC_USE(cpu);
    m_device->addRef();
    *device = m_device->guest();
    return D3D_OK;
}

DWORD Texture::SetPriority(WinApplication* app, x86::CPU& cpu, DWORD priority)
{
    RC_USE(app);
    RC_USE(cpu);
    const DWORD previous = m_priority;
    m_priority = priority;
    return previous;
}

DWORD Texture::GetPriority(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    RC_USE(cpu);
    return m_priority;
}

void Texture::PreLoad(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    RC_USE(cpu);
}

DWORD Texture::GetType(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    RC_USE(cpu);
    return m_desc.cube ? D3DRTYPE_CUBETEXTURE : D3DRTYPE_TEXTURE;
}

// The most detailed level managed textures keep; levels stay stored, so it only reads back.
DWORD Texture::SetLOD(WinApplication* app, x86::CPU& cpu, DWORD lod)
{
    RC_USE(app);
    RC_USE(cpu);
    const DWORD previous = m_lod;
    if (m_pool == 1)
        m_lod = lod < m_desc.levels ? lod : m_desc.levels - 1u;
    return previous;
}

DWORD Texture::GetLOD(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    RC_USE(cpu);
    return m_lod;
}

DWORD Texture::GetLevelCount(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    RC_USE(cpu);
    return m_desc.levels;
}

HRESULT Texture::SetAutoGenFilterType(WinApplication* app, x86::CPU& cpu, DWORD filter)
{
    RC_USE(app);
    RC_USE(cpu);
    if (filter < 1 || filter > 3)
        return D3DERR_INVALIDCALL;
    m_autoGenFilter = filter;
    return D3D_OK;
}

DWORD Texture::GetAutoGenFilterType(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    RC_USE(cpu);
    return m_autoGenFilter;
}

// No texture is created with D3DUSAGE_AUTOGENMIPMAP (CheckDeviceFormat refuses it), so there is nothing to generate.
void Texture::GenerateMipSubLevels(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    RC_USE(cpu);
}

HRESULT Texture::GetLevelDesc(WinApplication* app, x86::CPU& cpu, UINT level, D3DSURFACE_DESC* desc)
{
    RC_USE(app);
    RC_USE(cpu);
    if (level >= m_desc.levels || !desc)
        return D3DERR_INVALIDCALL;
    fillDesc(level, desc);
    return D3D_OK;
}

HRESULT Texture::GetSurfaceLevel(WinApplication* app, x86::CPU& cpu, UINT level, x86::reg32* surface)
{
    RC_USE(cpu);
    if (level >= m_desc.levels || m_desc.cube || !surface)
        return D3DERR_INVALIDCALL;
    *surface = guestOf(this->surface(app, 0, level));
    return D3D_OK;
}

HRESULT Texture::GetCubeMapSurface(WinApplication* app, x86::CPU& cpu, DWORD face, UINT level, x86::reg32* surface)
{
    RC_USE(cpu);
    if (level >= m_desc.levels || !m_desc.cube || face >= 6 || !surface)
        return D3DERR_INVALIDCALL;
    *surface = guestOf(this->surface(app, face, level));
    return D3D_OK;
}

HRESULT Texture::lock(uint32_t face, uint32_t level, D3DLOCKED_RECT* locked, const RECT* rect, DWORD flags)
{
    if (!m_storage || m_desc.target || level >= m_desc.levels || !locked)
        return D3DERR_INVALIDCALL;
    const uint32_t index = face * m_desc.levels + level;
    if (m_locks[index])
        return D3DERR_INVALIDCALL;
    const texture::Format format = m_desc.format;
    const uint32_t bytes = texture::levelBytes(format, m_desc.width, m_desc.height, level);
    const uint32_t pitch = texture::levelPitch(format, m_desc.width, level);
    // A level locked again keeps its linear copy: only locks write textures, so it equals the GPU copy and the
    // lock needs no read back (no unswizzle).
    MemMap* memory = m_shadows[index];
    if (!memory)
    {
        memory = new MemMap(bytes);
        if (m_written[index] && !(flags & D3DLOCK_DISCARD))
            gpu().readTexture(m_storage, face, level, host(memory->getBlockStart()), pitch);
    }
    if (m_lockCounts[index] < 2)
        ++m_lockCounts[index];
    m_locks[index] = memory;
    m_lockFlags[index] = flags;
    x86::reg32 bits = memory->getBlockStart();
    if (rect)
    {
        const uint32_t unit = texture::isBlock(format) ? 4 : 1;
        bits += uint32_t(rect->top) / unit * pitch + uint32_t(rect->left) / unit * texture::unitBytes(format);
    }
    s_lastLocked = index == 0 && !rect ? this : nullptr;
    s_lastBits = bits;
    locked->Pitch = LONG(pitch);
    locked->pBits = bits;
    return D3D_OK;
}

bool Texture::streamPlanes(uint32_t pitch, const render::Planes& planes)
{
    if (!m_locks[0] || m_locks.size() != 1 || m_lockCounts[0] < 2 || (m_lockFlags[0] & D3DLOCK_READONLY)
        || m_desc.target || texture::isBlock(m_desc.format) || texture::unitBytes(m_desc.format) != 4
        || pitch != texture::levelPitch(m_desc.format, m_desc.width, 0) || planes.width > m_desc.width
        || planes.height > m_desc.height || (m_desc.width & 15) || (m_desc.height & 1))
        return false;
    if (!m_planes)
    {
        // The planes go to the current copy when no frame reads it any more, else to a new one.
        if (m_storage->desc.yuv && !gpu().busy(*m_storage))
            m_planes = m_storage;
        else
        {
            render::TextureDesc desc = m_desc;
            desc.linear = true;
            desc.yuv = true;
            m_planes = gpu().createTexture(desc);
            if (!m_planes)
                return false;
        }
    }
    gpu().writePlanes(m_planes, planes);
    return true;
}

HRESULT Texture::unlock(uint32_t face, uint32_t level)
{
    if (level >= m_desc.levels)
        return D3DERR_INVALIDCALL;
    const uint32_t index = face * m_desc.levels + level;
    MemMap* memory = m_locks[index];
    if (!memory)
        return D3DERR_INVALIDCALL;
    if (s_lastLocked == this && index == 0)
        s_lastLocked = nullptr;
    if (m_planes && index == 0)
    {
        // A movie frame handed over as planes: shown instead of the locked pixels.
        if (m_planes != m_storage)
        {
            gpu().release(m_storage);
            m_storage = m_planes;
        }
        m_planes = nullptr;
        m_written[index] = true;
    }
    else if (!(m_lockFlags[index] & D3DLOCK_READONLY))
    {
        const bool powerOfTwo = !(m_desc.width & (m_desc.width - 1)) && !(m_desc.height & (m_desc.height - 1));
        if (m_lockCounts[index] >= 2 && !m_desc.linear && m_locks.size() == 1 && !m_desc.target
            && !texture::isBlock(m_desc.format) && powerOfTwo)
        {
            // Rewritten over and over: continue in linear storage, written with a row copy instead of a swizzle.
            render::TextureDesc desc = m_desc;
            desc.linear = true;
            if (render::TextureStorage* streamed = gpu().createTexture(desc))
            {
                gpu().release(m_storage);
                m_storage = streamed;
                m_desc = desc;
            }
        }
        else if (gpu().busy(*m_storage) || m_storage->desc.yuv)
        {
            // Frames in flight still read this copy (or it holds planes): continue in a new one. A single level is
            // rewritten whole just below, so only textures with more levels or faces copy the old contents.
            render::TextureStorage* fresh = gpu().createTexture(m_desc);
            if (fresh)
            {
                if (m_locks.size() > 1)
                    gpu().copyTexture(fresh, m_storage);
                gpu().release(m_storage);
                m_storage = fresh;
            }
        }
        // Out of memory for a new copy of planes: the last frame stays.
        if (!m_storage->desc.yuv)
            gpu().writeTexture(m_storage, face, level, host(memory->getBlockStart()),
                               texture::levelPitch(m_desc.format, m_desc.width, level));
        m_written[index] = true;
    }
    if (m_lockCounts[index] >= 2)
        m_shadows[index] = memory;
    else
        delete memory;
    m_locks[index] = nullptr;
    return D3D_OK;
}

HRESULT Texture::LockRect(WinApplication* app, x86::CPU& cpu, UINT level, D3DLOCKED_RECT* locked, const RECT* rect,
                          DWORD flags)
{
    RC_USE(app);
    RC_USE(cpu);
    const pf::perf::TextureTimer timer(pf::perf::TextureLock);
    return m_desc.cube ? D3DERR_INVALIDCALL : lock(0, level, locked, rect, flags);
}

HRESULT Texture::UnlockRect(WinApplication* app, x86::CPU& cpu, UINT level)
{
    RC_USE(app);
    RC_USE(cpu);
    const pf::perf::TextureTimer timer(pf::perf::TextureUnlock);
    return m_desc.cube ? D3DERR_INVALIDCALL : unlock(0, level);
}

HRESULT Texture::LockRectCube(WinApplication* app, x86::CPU& cpu, DWORD face, UINT level, D3DLOCKED_RECT* locked,
                              const RECT* rect, DWORD flags)
{
    RC_USE(app);
    RC_USE(cpu);
    return face < 6 ? lock(face, level, locked, rect, flags) : D3DERR_INVALIDCALL;
}

HRESULT Texture::UnlockRectCube(WinApplication* app, x86::CPU& cpu, DWORD face, UINT level)
{
    RC_USE(app);
    RC_USE(cpu);
    return face < 6 ? unlock(face, level) : D3DERR_INVALIDCALL;
}

HRESULT Texture::AddDirtyRect(WinApplication* app, x86::CPU& cpu, const RECT* rect)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(rect);
    return D3D_OK;
}

HRESULT Texture::AddDirtyRectCube(WinApplication* app, x86::CPU& cpu, DWORD face, const RECT* rect)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(face);
    RC_USE(rect);
    return D3D_OK;
}

// ---- Surface -------------------------------------------------------------------------------------------------------

Surface::Surface(Device* device, Role role, uint32_t width, uint32_t height, DWORD format)
    :   Object(KindSurface)
    ,   m_device(device)
    ,   m_role(role)
    ,   m_width(width)
    ,   m_height(height)
    ,   m_format(format)
{
}

Surface::~Surface()
{
    if (m_role == RoleDepth && m_depth && !m_container)
        gpu().release(m_depth);
}

void Surface::setContainer(Texture* texture, uint32_t face, uint32_t level)
{
    m_container = texture;
    m_face = face;
    m_level = level;
}

ULONG Surface::addRef()
{
    return m_container ? m_container->addRef() : Object::addRef();
}

ULONG Surface::release()
{
    return m_container ? m_container->release() : Object::release();
}

HRESULT Surface::GetDevice(WinApplication* app, x86::CPU& cpu, x86::reg32* device)
{
    RC_USE(app);
    RC_USE(cpu);
    m_device->addRef();
    *device = m_device->guest();
    return D3D_OK;
}

DWORD Surface::SetPriority(WinApplication* app, x86::CPU& cpu, DWORD priority)
{
    RC_USE(app);
    RC_USE(cpu);
    const DWORD previous = m_priority;
    m_priority = priority;
    return previous;
}

DWORD Surface::GetPriority(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    RC_USE(cpu);
    return m_priority;
}

void Surface::PreLoad(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    RC_USE(cpu);
}

DWORD Surface::GetType(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    RC_USE(cpu);
    return D3DRTYPE_SURFACE;
}

HRESULT Surface::GetContainer(WinApplication* app, x86::CPU& cpu, const GUID* iid, x86::reg32* container)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(iid);
    if (!container)
        return D3DERR_INVALIDCALL;
    Object* owner = m_container ? static_cast<Object*>(m_container) : static_cast<Object*>(m_device);
    owner->addRef();
    *container = owner->guest();
    return D3D_OK;
}

HRESULT Surface::GetDesc(WinApplication* app, x86::CPU& cpu, D3DSURFACE_DESC* desc)
{
    RC_USE(app);
    RC_USE(cpu);
    if (!desc)
        return D3DERR_INVALIDCALL;
    if (m_container)
    {
        m_container->fillDesc(m_level, desc);
        return D3D_OK;
    }
    desc->Format = m_format;
    desc->Type = D3DRTYPE_SURFACE;
    desc->Usage = m_role == RoleDepth ? D3DUSAGE_DEPTHSTENCIL : D3DUSAGE_RENDERTARGET;
    desc->Pool = 0;
    desc->MultiSampleType = 0;
    desc->MultiSampleQuality = 0;
    desc->Width = m_width;
    desc->Height = m_height;
    return D3D_OK;
}

HRESULT Surface::LockRect(WinApplication* app, x86::CPU& cpu, D3DLOCKED_RECT* locked, const RECT* rect, DWORD flags)
{
    if (m_container && !m_container->desc().target)
    {
        return m_container->desc().cube
             ? m_container->LockRectCube(app, cpu, m_face, m_level, locked, rect, flags)
             : m_container->LockRect(app, cpu, m_level, locked, rect, flags);
    }
    pf::logError("d3d9: render target and depth surfaces cannot be locked");
    return D3DERR_INVALIDCALL;
}

HRESULT Surface::UnlockRect(WinApplication* app, x86::CPU& cpu)
{
    if (m_container && !m_container->desc().target)
    {
        return m_container->desc().cube ? m_container->UnlockRectCube(app, cpu, m_face, m_level)
                                         : m_container->UnlockRect(app, cpu, m_level);
    }
    return D3DERR_INVALIDCALL;
}

// ---- Volume texture ------------------------------------------------------------------------------------------------

uint32_t formatBits(DWORD format, bool* block)
{
    *block = false;
    switch (format)
    {
    case D3DFMT_DXT1:
        *block = true;
        return 64;
    case 0x32545844:    // DXT2
    case D3DFMT_DXT3:
    case 0x34545844:    // DXT4
    case D3DFMT_DXT5:
        *block = true;
        return 128;
    case 20:            // R8G8B8
        return 24;
    case D3DFMT_A8R8G8B8: case D3DFMT_X8R8G8B8: case 31: case 32: case 33: case 34: case 35: case 62: case 63:
    case 64: case 67: case 112: case 114:
        // A2B10G10R10 A8B8G8R8 X8B8G8R8 G16R16 A2R10G10B10 X8L8V8U8 Q8W8V8U8 V16U16 A2W10V10U10 G16R16F R32F
        return 32;
    case 23: case 24: case 25: case 26: case 29: case 30: case 40: case 51: case 60: case 61: case 81: case 111:
        // R5G6B5 X1R5G5B5 A1R5G5B5 A4R4G4B4 A8R3G3B2 X4R4G4B4 A8P8 A8L8 V8U8 L6V5U5 L16 R16F
        return 16;
    case 27: case 28: case 41: case 50: case 52:    // R3G3B2 A8 P8 L8 A4L4
        return 8;
    case 36: case 110: case 113: case 115: // A16B16G16R16 Q16W16V16U16 A16B16G16R16F G32R32F
        return 64;
    case 116:           // A32B32G32R32F
        return 128;
    default:
        return 0;
    }
}

VolumeTexture::VolumeTexture(Device* device, UINT width, UINT height, UINT depth, UINT levels, DWORD format,
                             DWORD usage, DWORD pool)
    :   Object(KindVolumeTexture)
    ,   m_device(device)
    ,   m_width(width), m_height(height), m_depth(depth)
    ,   m_format(format), m_usage(usage), m_pool(pool)
{
    device->addRef();
    for (UINT level = 0; level < levels; ++level)
    {
        UINT w, h, d, row, slice;
        m_levels.push_back(nullptr);
        levelLayout(level, &w, &h, &d, &row, &slice);
        const UINT bytes = slice * d;
        m_levels.back() = new MemMap(bytes ? bytes : 1);
    }
    m_locked.assign(m_levels.size(), false);
    m_lockFlags.assign(m_levels.size(), 0);
    m_volumes.assign(m_levels.size(), nullptr);
}

VolumeTexture::~VolumeTexture()
{
    for (MemMap* m : m_levels)
        delete m;
    for (Volume* v : m_volumes)
        delete v;
    if (m_atlas)
        gpu().release(m_atlas);
    m_device->release();
}

const render::TextureStorage* VolumeTexture::storage()
{
    if (!m_dirty)
        return m_atlas;
    m_dirty = false;
    bool block;
    if (formatBits(m_format, &block) != 32 || block || m_width * m_depth > 4096 || m_height > 4096)
    {
        D3D_LOG_ONCE("d3d9: volume texture %ux%ux%u format 0x%x has no atlas: sampled as black", m_width, m_height,
                     m_depth, m_format);
        return m_atlas;
    }
    if (m_atlas && gpu().busy(*m_atlas))
    {
        gpu().release(m_atlas);
        m_atlas = nullptr;
    }
    if (!m_atlas)
    {
        render::TextureDesc desc = { texture::FormatARGB8888, 1, false, false, uint16_t(m_width * m_depth),
                                     uint16_t(m_height) };
        desc.linear = true;
        desc.depth = uint16_t(m_depth);
        m_atlas = gpu().createTexture(desc);
        if (!m_atlas)
        {
            pf::logError("d3d9: out of texture memory for a %ux%ux%u volume atlas", m_width, m_height, m_depth);
            return nullptr;
        }
    }
    // Slices side by side: atlas row y holds row y of every slice.
    UINT w, h, d, row, slice;
    levelLayout(0, &w, &h, &d, &row, &slice);
    std::vector<uint8_t> rows(size_t(w) * d * 4 * h);
    const uint8_t* src = host(m_levels[0]->getBlockStart());
    for (UINT y = 0; y < h; ++y)
        for (UINT z = 0; z < d; ++z)
        {
            uint8_t* out = rows.data() + (size_t(y) * w * d + size_t(z) * w) * 4;
            memcpy(out, src + size_t(z) * slice + size_t(y) * row, size_t(w) * 4);
            if (m_format == D3DFMT_X8R8G8B8)
                for (UINT x = 0; x < w; ++x)
                    out[x * 4 + 3] = 0xff;
        }
    gpu().writeTexture(m_atlas, 0, 0, rows.data(), w * d * 4);
    return m_atlas;
}

void VolumeTexture::fillDesc(UINT level, D3DVOLUME_DESC* desc) const
{
    UINT row, slice;
    levelLayout(level, &desc->Width, &desc->Height, &desc->Depth, &row, &slice);
    desc->Format = m_format;
    desc->Type = D3DRTYPE_VOLUME;
    desc->Usage = m_usage;
    desc->Pool = m_pool;
}

void VolumeTexture::levelLayout(UINT level, UINT* width, UINT* height, UINT* depth, UINT* rowPitch,
                                UINT* slicePitch) const
{
    *width = texture::levelDim(m_width, level);
    *height = texture::levelDim(m_height, level);
    *depth = texture::levelDim(m_depth, level);
    bool block;
    const uint32_t bits = formatBits(m_format, &block);
    if (block)
    {
        *rowPitch = (*width + 3) / 4 * bits / 8;
        *slicePitch = *rowPitch * ((*height + 3) / 4);
    }
    else
    {
        *rowPitch = (*width * bits + 7) / 8;
        *slicePitch = *rowPitch * *height;
    }
}

x86::reg32 VolumeTexture::levelData(UINT level) const
{
    return level < m_levels.size() ? m_levels[level]->getBlockStart() : 0;
}

HRESULT VolumeTexture::GetDevice(WinApplication* app, x86::CPU& cpu, x86::reg32* device)
{
    RC_USE(app);
    RC_USE(cpu);
    m_device->addRef();
    *device = m_device->guest();
    return D3D_OK;
}

DWORD VolumeTexture::SetPriority(WinApplication* app, x86::CPU& cpu, DWORD priority)
{
    RC_USE(app);
    RC_USE(cpu);
    const DWORD previous = m_priority;
    m_priority = priority;
    return previous;
}

DWORD VolumeTexture::GetPriority(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    RC_USE(cpu);
    return m_priority;
}

void VolumeTexture::PreLoad(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    RC_USE(cpu);
}

DWORD VolumeTexture::GetType(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    RC_USE(cpu);
    return D3DRTYPE_VOLUMETEXTURE;
}

// The most detailed level managed textures keep; levels stay stored, so it only reads back.
DWORD VolumeTexture::SetLOD(WinApplication* app, x86::CPU& cpu, DWORD lod)
{
    RC_USE(app);
    RC_USE(cpu);
    const DWORD previous = m_lod;
    if (m_pool == 1)
        m_lod = lod < m_levels.size() ? lod : DWORD(m_levels.size() - 1);
    return previous;
}

DWORD VolumeTexture::GetLOD(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    RC_USE(cpu);
    return m_lod;
}

DWORD VolumeTexture::GetLevelCount(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    RC_USE(cpu);
    return DWORD(m_levels.size());
}

HRESULT VolumeTexture::SetAutoGenFilterType(WinApplication* app, x86::CPU& cpu, DWORD filter)
{
    RC_USE(app);
    RC_USE(cpu);
    if (filter < 1 || filter > 3)
        return D3DERR_INVALIDCALL;
    m_autoGenFilter = filter;
    return D3D_OK;
}

DWORD VolumeTexture::GetAutoGenFilterType(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    RC_USE(cpu);
    return m_autoGenFilter;
}

// No texture is created with D3DUSAGE_AUTOGENMIPMAP (CheckDeviceFormat refuses it), so there is nothing to generate.
void VolumeTexture::GenerateMipSubLevels(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    RC_USE(cpu);
}

HRESULT VolumeTexture::GetLevelDesc(WinApplication* app, x86::CPU& cpu, UINT level, D3DVOLUME_DESC* desc)
{
    RC_USE(app);
    RC_USE(cpu);
    if (!desc || level >= m_levels.size())
        return D3DERR_INVALIDCALL;
    fillDesc(level, desc);
    return D3D_OK;
}

HRESULT VolumeTexture::GetVolumeLevel(WinApplication* app, x86::CPU& cpu, UINT level, x86::reg32* volume)
{
    RC_USE(cpu);
    if (!volume || level >= m_levels.size())
        return D3DERR_INVALIDCALL;
    if (!m_volumes[level])
    {
        m_volumes[level] = new Volume(this, level);
        publish(app, m_volumes[level]);
    }
    addRef();
    *volume = m_volumes[level]->guest();
    return D3D_OK;
}

HRESULT VolumeTexture::LockBox(WinApplication* app, x86::CPU& cpu, UINT level, D3DLOCKED_BOX* locked,
                               const D3DBOX* box, DWORD flags)
{
    RC_USE(app);
    RC_USE(cpu);
    return lock(level, locked, box, flags);
}

HRESULT VolumeTexture::lock(UINT level, D3DLOCKED_BOX* locked, const D3DBOX* box, DWORD flags)
{
    if (!locked || level >= m_levels.size() || m_locked[level])
        return D3DERR_INVALIDCALL;
    UINT w, h, d, row, slice;
    levelLayout(level, &w, &h, &d, &row, &slice);
    x86::reg32 bits = m_levels[level]->getBlockStart();
    if (box)
    {
        bool block;
        const uint32_t unitBits = formatBits(m_format, &block);
        const UINT unit = block ? 4 : 1;
        bits += box->Front * slice + box->Top / unit * row + box->Left / unit * unitBits / 8;
    }
    m_locked[level] = true;
    m_lockFlags[level] = flags;
    locked->RowPitch = LONG(row);
    locked->SlicePitch = LONG(slice);
    locked->pBits = bits;
    return D3D_OK;
}

HRESULT VolumeTexture::UnlockBox(WinApplication* app, x86::CPU& cpu, UINT level)
{
    RC_USE(app);
    RC_USE(cpu);
    return unlock(level);
}

HRESULT VolumeTexture::unlock(UINT level)
{
    if (level >= m_levels.size() || !m_locked[level])
        return D3DERR_INVALIDCALL;
    m_locked[level] = false;
    if (level == 0 && !(m_lockFlags[level] & D3DLOCK_READONLY))
        m_dirty = true;
    return D3D_OK;
}

// ---- Volume --------------------------------------------------------------------------------------------------------

HRESULT Volume::GetDevice(WinApplication* app, x86::CPU& cpu, x86::reg32* device)
{
    return m_container->GetDevice(app, cpu, device);
}

HRESULT Volume::GetContainer(WinApplication* app, x86::CPU& cpu, const GUID* iid, x86::reg32* container)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(iid);
    if (!container)
        return D3DERR_INVALIDCALL;
    m_container->addRef();
    *container = m_container->guest();
    return D3D_OK;
}

HRESULT Volume::GetDesc(WinApplication* app, x86::CPU& cpu, D3DVOLUME_DESC* desc)
{
    RC_USE(app);
    RC_USE(cpu);
    if (!desc)
        return D3DERR_INVALIDCALL;
    m_container->fillDesc(m_level, desc);
    return D3D_OK;
}

HRESULT Volume::LockBox(WinApplication* app, x86::CPU& cpu, D3DLOCKED_BOX* locked, const D3DBOX* box, DWORD flags)
{
    RC_USE(app);
    RC_USE(cpu);
    return m_container->lock(m_level, locked, box, flags);
}

HRESULT Volume::UnlockBox(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    RC_USE(cpu);
    return m_container->unlock(m_level);
}

// ---- Query ---------------------------------------------------------------------------------------------------------

namespace
{

const DWORD D3DISSUE_END = 1, D3DGETDATA_FLUSH = 1;

}

Query::Query(Device* device, DWORD type)
    :   Object(KindQuery)
    ,   m_device(device)
    ,   m_type(type)
{
    device->addRef();
}

Query::~Query()
{
    m_device->release();
}

HRESULT Query::GetDevice(WinApplication* app, x86::CPU& cpu, x86::reg32* device)
{
    RC_USE(app);
    RC_USE(cpu);
    if (!device)
        return D3DERR_INVALIDCALL;
    m_device->addRef();
    *device = m_device->guest();
    return D3D_OK;
}

DWORD Query::GetType(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    RC_USE(cpu);
    return m_type;
}

DWORD Query::GetDataSize(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    RC_USE(cpu);
    return 4;   // BOOL
}

// An event marks the commands recorded so far: the frame being recorded, or the last one submitted.
HRESULT Query::Issue(WinApplication* app, x86::CPU& cpu, DWORD flags)
{
    RC_USE(app);
    RC_USE(cpu);
    if (flags != D3DISSUE_END)
        return D3DERR_INVALIDCALL;
    m_frame = m_device->issueFrame();
    return D3D_OK;
}

// Not done yet with FLUSH: waits for the GPU (the result is the same as polling until done). An event in the frame
// still being recorded counts as flushed once every submitted frame is done; without FLUSH it answers S_FALSE.
// event_query_lag=n: an event counts as done once the GPU finished the frame n before its own (the frame slots still
// bound the frames in flight).
HRESULT Query::GetData(WinApplication* app, x86::CPU& cpu, x86::reg32 data, DWORD size, DWORD flags)
{
    if (size && !data)
        return D3DERR_INVALIDCALL;
    static const uint32_t s_eventLag = uint32_t(std::max(0, std::min(2, pf::Config::getInt("event_query_lag", 0))));
    const DWORD EventQuery = 8;     // D3DQUERYTYPE_EVENT
    const uint32_t lag = m_type == EventQuery ? s_eventLag : 0;
    const uint32_t frame = m_frame > lag ? m_frame - lag : 0;
    bool done = !frame || gpu().completedFrame() >= frame;
    if (!done && (flags & D3DGETDATA_FLUSH))
    {
        const uint32_t submitted = m_device->submittedFrame();
        pf::perf::blocking(pf::perf::BlockGpu);
        const uint32_t start = pf::perf::enabled() ? pf::perf::now() : 0;
        app->unlockContext(cpu);
        gpu().waitFrame(frame < submitted ? frame : submitted);
        app->lockContext(cpu);
        if (pf::perf::enabled())
            pf::perf::displayWait(pf::perf::DisplayGpu, pf::perf::now() - start);
        done = true;
    }
    if (!done)
        return S_FALSE;
    if (data && size)
    {
        const BOOL value = 1;
        memcpy(g_guestBase + data, &value, size < 4 ? size : 4);
    }
    return D3D_OK;
}

// ---- Shader --------------------------------------------------------------------------------------------------------

Shader::Shader(Device* device, Kind kind, const uint8_t* code, uint32_t bytes)
    :   Object(kind)
    ,   m_device(device)
    ,   m_code(code, code + bytes)
{
    device->addRef();
}

Shader::~Shader()
{
    m_device->release();
}

uint32_t Shader::length(const uint8_t* code, uint32_t limit)
{
    // Version token, then instructions (length in bits 24-27 from shader model 2), comments (0xFFFE, length in bits
    // 16-30) and the END token 0x0000FFFF.
    if (limit < 8)
        return 0;
    uint32_t version;
    memcpy(&version, code, 4);
    const uint32_t major = (version >> 8) & 0xff;
    uint32_t at = 4;
    while (at + 4 <= limit)
    {
        uint32_t token;
        memcpy(&token, code + at, 4);
        at += 4;
        if (token == 0x0000FFFF)
            return at;
        const uint32_t opcode = token & 0xffff;
        if (opcode == 0xFFFE)
        {
            at += 4 * ((token >> 16) & 0x7fff);
            continue;
        }
        if (major >= 2)
        {
            at += 4 * ((token >> 24) & 0xf);
            continue;
        }
        // Shader model 1: parameters until the next token with bit 31 clear.
        while (at + 4 <= limit)
        {
            uint32_t parameter;
            memcpy(&parameter, code + at, 4);
            if (!(parameter & 0x80000000u))
                break;
            at += 4;
        }
    }
    return 0;
}

HRESULT Shader::GetDevice(WinApplication* app, x86::CPU& cpu, x86::reg32* device)
{
    RC_USE(app);
    RC_USE(cpu);
    if (!device)
        return D3DERR_INVALIDCALL;
    m_device->addRef();
    *device = m_device->guest();
    return D3D_OK;
}

HRESULT Shader::GetFunction(WinApplication* app, x86::CPU& cpu, x86::reg32 data, UINT* size)
{
    RC_USE(app);
    RC_USE(cpu);
    if (!size)
        return D3DERR_INVALIDCALL;
    if (!data)
    {
        *size = UINT(m_code.size());
        return D3D_OK;
    }
    if (*size < m_code.size())
        return D3DERR_MOREDATA;
    memcpy(g_guestBase + data, m_code.data(), m_code.size());
    *size = UINT(m_code.size());
    return D3D_OK;
}

HRESULT VolumeTexture::AddDirtyBox(WinApplication* app, x86::CPU& cpu, const D3DBOX* box)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(box);
    return D3D_OK;
}

// ---- Buffer --------------------------------------------------------------------------------------------------------

Buffer::Buffer(Device* device, Kind kind, UINT size, DWORD usage, DWORD pool, DWORD format)
    :   Object(kind)
    ,   m_device(device)
    ,   m_size(size)
    ,   m_usage(usage)
    ,   m_pool(pool)
    ,   m_format(format)
    ,   m_storage(gpu().createBuffer(size))
{
    device->addRef();
}

Buffer::~Buffer()
{
    delete m_shadow;
    if (m_storage)
        gpu().release(m_storage);
    m_device->release();
}

HRESULT Buffer::GetDevice(WinApplication* app, x86::CPU& cpu, x86::reg32* device)
{
    RC_USE(app);
    RC_USE(cpu);
    m_device->addRef();
    *device = m_device->guest();
    return D3D_OK;
}

DWORD Buffer::SetPriority(WinApplication* app, x86::CPU& cpu, DWORD priority)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(priority);
    return 0;
}

DWORD Buffer::GetPriority(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    RC_USE(cpu);
    return 0;
}

void Buffer::PreLoad(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    RC_USE(cpu);
}

DWORD Buffer::GetType(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    RC_USE(cpu);
    return kind() == KindVertexBuffer ? D3DRTYPE_VERTEXBUFFER : D3DRTYPE_INDEXBUFFER;
}

HRESULT Buffer::Lock(WinApplication* app, x86::CPU& cpu, UINT offset, UINT size, x86::reg32* data, DWORD flags)
{
    RC_USE(app);
    RC_USE(cpu);
    if (!m_storage || !data || offset > m_size)
        return D3DERR_INVALIDCALL;
    if (!size || offset + size > m_size)
        size = m_size - offset;
    if (m_locks++ == 0)
    {
        m_lockOffset = offset;
        m_lockSize = size;
        m_lockFlags = flags;
        if (!m_shadow)
        {
            // The guest copy starts as the GPU copy, which always holds every byte written so far.
            m_shadow = new MemMap(m_size ? m_size : 1);
            if (m_written && !(flags & D3DLOCK_DISCARD))
                memcpy(host(m_shadow->getBlockStart()), m_storage->data, m_size);
            if (m_lockCount < 2)
                ++m_lockCount;
        }
    }
    else
    {
        // Nested locks: one range over all of them.
        const UINT end = std::max(m_lockOffset + m_lockSize, offset + size);
        m_lockOffset = std::min(m_lockOffset, offset);
        m_lockSize = end - m_lockOffset;
        m_lockFlags &= flags;
    }
    *data = m_shadow->getBlockStart() + offset;
    return D3D_OK;
}

HRESULT Buffer::Unlock(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    RC_USE(cpu);
    if (m_locks == 0)
        return D3DERR_INVALIDCALL;
    if (--m_locks)
        return D3D_OK;
    if (!(m_lockFlags & D3DLOCK_READONLY))
        update();
    // Buffers written once (the world and car meshes) keep only the GPU copy.
    if (m_lockCount < 2 && !(m_usage & D3DUSAGE_DYNAMIC))
    {
        delete m_shadow;
        m_shadow = nullptr;
    }
    return D3D_OK;
}

void Buffer::update()
{
    const uint8_t* shadow = host(m_shadow->getBlockStart());
    m_written = true;
    // Unlock keeps the shadow of these buffers, so their bytes can wait for the draws that read them.
    const bool kept = m_lockCount >= 2 || (m_usage & D3DUSAGE_DYNAMIC);
    if (!(m_lockFlags & D3DLOCK_NOOVERWRITE) && gpu().busy(*m_storage))
    {
        // Frames in flight read the current copy: a new one gets every byte the guest has written, now or (a kept
        // shadow) as draws read them.
        render::BufferStorage* fresh = gpu().createBuffer(m_size);
        if (fresh)
        {
            if (kept)
            {
                m_pendingBegin = 0;
                m_pendingEnd = m_size;
            }
            else
                memcpy(fresh->data, shadow, m_size);
            gpu().release(m_storage);
            m_storage = fresh;
            return;
        }
    }
    if (kept)
    {
        if (!m_lockSize)
            return;
        const UINT end = m_lockOffset + m_lockSize;
        const bool none = m_pendingBegin == m_pendingEnd;
        m_pendingBegin = none ? m_lockOffset : std::min(m_pendingBegin, m_lockOffset);
        m_pendingEnd = none ? end : std::max(m_pendingEnd, end);
        return;
    }
    memcpy(static_cast<uint8_t*>(m_storage->data) + m_lockOffset, shadow + m_lockOffset, m_lockSize);
    m_storage->writes.fetch_add(1, std::memory_order_release);
}

void Buffer::copyPending(UINT end)
{
    const UINT stop = std::min(end, m_pendingEnd);
    memcpy(static_cast<uint8_t*>(m_storage->data) + m_pendingBegin, host(m_shadow->getBlockStart()) + m_pendingBegin,
           stop - m_pendingBegin);
    m_storage->writes.fetch_add(1, std::memory_order_release);
    m_pendingBegin = stop;
    if (m_pendingBegin == m_pendingEnd)
        m_pendingBegin = m_pendingEnd = 0;
}

HRESULT Buffer::GetVertexDesc(WinApplication* app, x86::CPU& cpu, D3DVERTEXBUFFER_DESC* desc)
{
    RC_USE(app);
    RC_USE(cpu);
    if (!desc)
        return D3DERR_INVALIDCALL;
    *desc = D3DVERTEXBUFFER_DESC{ 100 /* D3DFMT_VERTEXDATA */, D3DRTYPE_VERTEXBUFFER, m_usage, m_pool, m_size,
                                  m_format };
    return D3D_OK;
}

HRESULT Buffer::GetIndexDesc(WinApplication* app, x86::CPU& cpu, D3DINDEXBUFFER_DESC* desc)
{
    RC_USE(app);
    RC_USE(cpu);
    if (!desc)
        return D3DERR_INVALIDCALL;
    *desc = D3DINDEXBUFFER_DESC{ m_format, D3DRTYPE_INDEXBUFFER, m_usage, m_pool, m_size };
    return D3D_OK;
}

// ---- Vertex declaration --------------------------------------------------------------------------------------------

namespace
{

enum : BYTE
{
    D3DDECLUSAGE_POSITION = 0, D3DDECLUSAGE_BLENDWEIGHT = 1, D3DDECLUSAGE_BLENDINDICES = 2, D3DDECLUSAGE_NORMAL = 3,
    D3DDECLUSAGE_TEXCOORD = 5, D3DDECLUSAGE_TANGENT = 6, D3DDECLUSAGE_BINORMAL = 7, D3DDECLUSAGE_POSITIONT = 9,
    D3DDECLUSAGE_COLOR = 10,
    D3DDECLTYPE_UNUSED = 17
};

void add(render::VertexLayout& layout, uint8_t usage, uint8_t type, uint16_t offset)
{
    if (layout.count < render::MaxVertexElements)
        layout.elements[layout.count++] = render::VertexElement{ usage, type, offset };
}

}

VertexDeclaration::VertexDeclaration(Device* device, const D3DVERTEXELEMENT9* elements, uint32_t count, DWORD fvf)
    :   Object(KindVertexDeclaration)
    ,   m_device(device)
    ,   m_elements(elements, elements + count)
    ,   m_fvf(fvf)
{
    memset(&m_layout, 0, sizeof(m_layout));
    if (fvf)
    {
        m_layout = fvfLayout(fvf, &m_texComponents);
        return;
    }
    for (const D3DVERTEXELEMENT9& e : m_elements)
    {
        if (e.Stream != 0)
        {
            D3D_LOG_ONCE("d3d9: vertex elements of stream %u ignored", e.Stream);
            continue;
        }
        if (e.Type > render::TypeUByte4N)
        {
            pf::logError("d3d9: vertex element type %u not supported", e.Type);
            continue;
        }
        uint8_t usage;
        switch (e.Usage)
        {
        case D3DDECLUSAGE_POSITIONT:
            m_layout.positionT = true;
            usage = render::UsagePosition;
            break;
        case D3DDECLUSAGE_POSITION:     usage = render::UsagePosition; break;
        case D3DDECLUSAGE_NORMAL:       usage = render::UsageNormal; break;
        case D3DDECLUSAGE_BLENDWEIGHT:  usage = render::UsageWeight; break;
        case D3DDECLUSAGE_BLENDINDICES: usage = render::UsageIndices; break;
        case D3DDECLUSAGE_TANGENT:      usage = render::UsageTangent; break;
        case D3DDECLUSAGE_BINORMAL:     usage = render::UsageBinormal; break;
        case D3DDECLUSAGE_COLOR:
            if (e.UsageIndex > 1)
            {
                D3D_LOG_ONCE("d3d9: vertex colour %u ignored", e.UsageIndex);
                continue;
            }
            usage = e.UsageIndex ? render::UsageColor1 : render::UsageColor;
            break;
        case D3DDECLUSAGE_TEXCOORD:
            if (e.UsageIndex > 8)
            {
                D3D_LOG_ONCE("d3d9: texture coordinate %u ignored", e.UsageIndex);
                continue;
            }
            usage = uint8_t(e.UsageIndex < 4 ? render::UsageTex0 + e.UsageIndex
                                             : render::UsageTex4 + (e.UsageIndex - 4));
            if (e.UsageIndex == 0)
                m_texComponents = e.Type <= render::TypeFloat4 ? e.Type + 1u : 4u;
            break;
        default:
            D3D_LOG_ONCE("d3d9: vertex element usage %u ignored", e.Usage);
            continue;
        }
        add(m_layout, usage, e.Type, e.Offset);
    }
}

render::VertexLayout VertexDeclaration::fvfLayout(DWORD fvf, uint32_t* texComponents)
{
    render::VertexLayout layout;
    memset(&layout, 0, sizeof(layout));
    uint16_t offset = 0;
    switch (fvf & 0xE)
    {
    case 0x2:   // XYZ
        add(layout, render::UsagePosition, render::TypeFloat3, offset);
        offset += 12;
        break;
    case 0x4:   // XYZRHW
        add(layout, render::UsagePosition, render::TypeFloat4, offset);
        layout.positionT = true;
        offset += 16;
        break;
    default:
        if (fvf & 0xE)
            pf::logError("d3d9: FVF position 0x%x not supported", fvf & 0xE);
        break;
    }
    if (fvf & 0x10)     // NORMAL
    {
        add(layout, render::UsageNormal, render::TypeFloat3, offset);
        offset += 12;
    }
    if (fvf & 0x20)     // PSIZE
        offset += 4;
    if (fvf & 0x40)     // DIFFUSE
    {
        add(layout, render::UsageColor, render::TypeColor, offset);
        offset += 4;
    }
    if (fvf & 0x80)     // SPECULAR
        offset += 4;
    const uint32_t texCount = (fvf >> 8) & 0xF;
    *texComponents = 0;
    for (uint32_t i = 0; i < texCount; ++i)
    {
        static const uint8_t components[4] = { 2, 3, 4, 1 };
        const uint32_t n = components[(fvf >> (16 + 2 * i)) & 3];
        if (i < 4)
            add(layout, uint8_t(render::UsageTex0 + i), uint8_t(render::TypeFloat1 + n - 1), offset);
        if (i == 0)
            *texComponents = n;
        offset += uint16_t(4 * n);
    }
    layout.stride = offset;
    return layout;
}

uint8_t VertexDeclaration::layout(uint32_t stride)
{
    if (stride != m_stride || !m_stride)
    {
        render::VertexLayout layout = m_layout;
        layout.stride = uint16_t(stride);
        m_layoutId = render::registerLayout(layout);
        m_stride = stride;
    }
    return m_layoutId;
}

HRESULT VertexDeclaration::GetDevice(WinApplication* app, x86::CPU& cpu, x86::reg32* device)
{
    RC_USE(app);
    RC_USE(cpu);
    m_device->addRef();
    *device = m_device->guest();
    return D3D_OK;
}

HRESULT VertexDeclaration::GetDeclaration(WinApplication* app, x86::CPU& cpu, D3DVERTEXELEMENT9* elements,
                                          UINT* count)
{
    RC_USE(app);
    RC_USE(cpu);
    if (!count)
        return D3DERR_INVALIDCALL;
    const UINT n = UINT(m_elements.size()) + 1;
    if (elements)
    {
        memcpy(elements, m_elements.data(), m_elements.size() * sizeof(D3DVERTEXELEMENT9));
        elements[m_elements.size()] = D3DVERTEXELEMENT9{ 0xFF, 0, D3DDECLTYPE_UNUSED, 0, 0, 0 };
    }
    *count = n;
    return D3D_OK;
}

}}
