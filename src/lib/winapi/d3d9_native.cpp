// Native renderer runs of effect passes (d3d9_native.h): the pass tables applied to a mirror of the device state and
// constant registers, draws built with the resolver's effect state and constant functions and recorded finished.

#include <winapi/d3d9_native.h>
#include <platform/config.h>
#include <platform/log.h>
#include <algorithm>
#include <cstring>

namespace win32 { namespace d3d9 { namespace native {

namespace
{

enum : uint8_t
{
    TypeBool = 1, TypeInt = 2, TypeFloat = 3
};

enum : DWORD
{
    D3DRS_FOGENABLE = 28, D3DRS_FOGCOLOR = 34, D3DRS_BLENDOP = 171, D3DRS_BLENDFACTOR = 193
};

// A parameter value as a float, as the effect layer converts it.
float slotFloat(const uint8_t* p, uint8_t type)
{
    if (type == TypeFloat)
    {
        float f;
        memcpy(&f, p, 4);
        return f;
    }
    int32_t i;
    memcpy(&i, p, 4);
    return type == TypeBool ? (i ? 1.0f : 0.0f) : float(i);
}

uint32_t floatBits(float f)
{
    uint32_t bits;
    memcpy(&bits, &f, 4);
    return bits;
}

bool textureKind(const Object* object)
{
    return object->kind() == KindTexture || object->kind() == KindCubeTexture || object->kind() == KindVolumeTexture;
}

enum : uint32_t
{
    StageWords = offsetof(FixedState, stageStates) / 4,
    SamplerWords = offsetof(FixedState, samplerStates) / 4
};

// Runs take only the device state words and registers written since the last run, unless native_sync=0.
bool deltaSync()
{
    static const bool s_delta = pf::Config::getBool("native_sync", true);
    return s_delta;
}

// Production runs write a draw's new constant blocks into the frame directly, unless native_blocks=0.
bool directBlocks()
{
    static const bool s_direct = pf::Config::getBool("native_blocks", true);
    return s_direct;
}

// A run's draw takes the previous draw's effect state when nothing it is made of changed, unless native_reuse=0.
bool stateReuse()
{
    static const bool s_reuse = pf::Config::getBool("native_reuse", true);
    return s_reuse;
}

// A run end's state writes, made on the device.
struct DeviceWrites
{
    Device*             device;
    WinApplication*     app;
    x86::CPU&           cpu;

    void render(DWORD state, DWORD value) { device->SetRenderState(app, cpu, state, value); }
    void stage(DWORD stage, DWORD type, DWORD value) { device->SetTextureStageState(app, cpu, stage, type, value); }
    void sampler(DWORD sampler, DWORD type, DWORD value) { device->SetSamplerState(app, cpu, sampler, type, value); }
    void texture(DWORD sampler, Object* texture) { device->SetTexture(app, cpu, sampler, texture); }
};

#if NFSC_RENDER_CHECK
// A run end's state writes, listed.
struct ListedWrites
{
    std::vector<uintptr_t> calls;

    void add(uintptr_t kind, uintptr_t a, uintptr_t b, uintptr_t value)
    {
        calls.insert(calls.end(), { kind, a, b, value });
    }
    void render(DWORD state, DWORD value) { add(0, state, 0, value); }
    void stage(DWORD stage, DWORD type, DWORD value) { add(1, stage, type, value); }
    void sampler(DWORD sampler, DWORD type, DWORD value) { add(2, sampler, type, value); }
    void texture(DWORD sampler, Object* texture) { add(3, sampler, 0, uintptr_t(texture)); }
};
#endif

}

DWORD Run::value(const PassState& s) const
{
    if (s.source == SourceLiteral)
        return s.value;
    DWORD raw;
    memcpy(&raw, s.slot, 4);
    if (s.source == SourceRaw)
        return raw;
    const float f = slotFloat(s.slot, s.slotType);
    if (s.valueType == TypeFloat)
        return floatBits(f);
    return s.valueType == TypeBool ? (f != 0.0f ? 1 : 0) : DWORD(int32_t(f));
}

void Run::applyState(const PassState& s)
{
    const DWORD v = value(s);
    switch (s.target)
    {
    case TargetRender:
        if (s.type < MaxRenderStates)
        {
            m_state.renderStates[s.type] = v;
            touch(s.type);
        }
        break;
    case TargetStage:
        if (s.index < MaxStages && s.type < D3DTSS_COUNT)
        {
            m_state.stageStates[s.index][s.type] = v;
            touch(StageWords + s.index * D3DTSS_COUNT + s.type);
        }
        break;
    case TargetSampler:
        if (s.index < MaxSamplerStages && s.type < MaxSamplerStates)
        {
            m_state.samplerStates[s.index][s.type] = v;
            touch(SamplerWords + s.index * MaxSamplerStates + s.type);
        }
        break;
    default:
    {
        if (s.index >= MaxSamplerStages)
            break;
        Object* object = v ? lookup(v) : nullptr;
        if (!object || textureKind(object))
            m_state.textures[s.index] = object;
        break;
    }
    }
}

bool Run::held(const PassConstant& c) const
{
    const uint64_t* stamps = (c.pixel ? m_pixelStamps : m_vertexStamps) + c.first;
    for (uint32_t i = 0; i < c.count; ++i)
    {
        if (stamps[i] != c.stamp)
            return false;
    }
    return true;
}

// As the effect layer's upload: skipped while the parameter is unchanged and the registers still hold it.
void Run::upload(PassConstant& c)
{
    if (c.stamp && c.written == *c.version && held(c))
        return;
    c.written = *c.version;
    const uint32_t registers = c.pixel ? Device::PixelFloatConstants : Device::VertexFloatConstants;
    if (c.first > registers || c.count > registers - c.first)
    {
        c.stamp = 0;
        return;
    }
    float scratch[4 * 256];
    const float* values = nativeRegisters(c, scratch);
    float* bank = c.pixel ? m_pixel : m_vertex;
    uint64_t* stamps = (c.pixel ? m_pixelStamps : m_vertexStamps) + c.first;
    const uint64_t stamp = ++m_stamp;
    for (uint32_t i = 0; i < c.count; ++i)
        stamps[i] = stamp;
    copyRegisters(bank + 4 * c.first, values, c.count);
    uint32_t* dirty = m_dirty[c.pixel ? 1 : 0];
    if (dirty[0] == dirty[1])
    {
        dirty[0] = c.first;
        dirty[1] = uint32_t(c.first + c.count);
    }
    else
    {
        dirty[0] = std::min<uint32_t>(dirty[0], c.first);
        dirty[1] = std::max<uint32_t>(dirty[1], c.first + c.count);
    }
    ++m_generation;
    c.stamp = stamp;
}

// The banks from the device's registers when the device wrote any since they were last in step: with delta only
// the registers it wrote, which then hold no upload of the run.
void Run::syncRegisters(bool delta)
{
    const uint32_t writes = m_device->constantWrites();
    if (m_synced && writes == m_constantSeen)
        return;
    if (delta && m_synced && writes - m_constantSeen <= Device::WriteLogSize)
    {
        for (uint32_t n = m_constantSeen; n != writes; ++n)
        {
            const Device::Written& w = m_device->constantWritten(n);
            const uint32_t first = w.first & 0x7fff;
            if (w.first & 0x8000)
            {
                copyRegisters(m_pixel + 4 * first, m_device->pixelConstantsF() + 4 * first, w.count);
                memset(m_pixelStamps + first, 0, sizeof(m_pixelStamps[0]) * w.count);
            }
            else
            {
                copyRegisters(m_vertex + 4 * first, m_device->vertexConstantsF() + 4 * first, w.count);
                memset(m_vertexStamps + first, 0, sizeof(m_vertexStamps[0]) * w.count);
            }
        }
    }
    else
    {
        memcpy(m_vertex, m_device->vertexConstantsF(), sizeof(m_vertex));
        memcpy(m_pixel, m_device->pixelConstantsF(), sizeof(m_pixel));
        memset(m_vertexStamps, 0, sizeof(m_vertexStamps));
        memset(m_pixelStamps, 0, sizeof(m_pixelStamps));
    }
    memset(m_dirty, 0, sizeof(m_dirty));
    m_synced = true;
    m_constantSeen = writes;
    ++m_generation;
}

// The mirror's fixed state from the device's: with delta only the words the device logged since and the words the
// last run wrote, else all of it.
void Run::syncState(bool delta)
{
    const uint32_t writes = m_device->stateWrites();
    DWORD* to = reinterpret_cast<DWORD*>(static_cast<FixedState*>(&m_state));
    const DWORD* from = reinterpret_cast<const DWORD*>(&m_device->fixedState());
    if (delta && writes - m_stateSeen <= Device::WriteLogSize)
    {
        for (uint32_t n = m_stateSeen; n != writes; ++n)
        {
            const Device::Written& w = m_device->stateWritten(n);
            memcpy(to + w.first, from + w.first, 4 * w.count);
        }
        for (uint32_t i = 0; i < sizeof(m_touched) / 4; ++i)
        {
            for (uint32_t bits = m_touched[i]; bits; bits &= bits - 1)
            {
                const uint32_t word = 32 * i + uint32_t(__builtin_ctz(bits));
                to[word] = from[word];
            }
        }
    }
    else
    {
        memcpy(to, from, sizeof(FixedState));
    }
    memset(m_touched, 0, sizeof(m_touched));
    m_stateSeen = writes;
}

void Run::begin(Device* device, Pass* pass)
{
    const bool delta = deltaSync() && device == m_seen;
    if (device != m_device)
        m_synced = false;
    m_device = device;
    m_pass = pass;
    syncState(delta);
    ++m_stateChanges;
    m_seen = device;
    for (uint32_t i = 0; i < MaxSamplerStages; ++i)
        m_state.textures[i] = device->stageTexture(i);
    if (!m_emit)
        m_before = m_state;
    syncRegisters(delta);
#if NFSC_RENDER_CHECK
    ++m_syncs;
    if (memcmp(static_cast<const FixedState*>(&m_state), &device->fixedState(), sizeof(FixedState)) != 0
        || memcmp(m_vertex, device->vertexConstantsF(), sizeof(m_vertex)) != 0
        || memcmp(m_pixel, device->pixelConstantsF(), sizeof(m_pixel)) != 0)
    {
        if (m_syncsDiffer++ < 8)
            pf::logInfo("render check: native run start %llu: state or registers differ from the device's",
                        static_cast<unsigned long long>(m_syncs));
    }
#endif
    m_kept.clear();
    m_keptFloats.clear();
}

void Run::beginPass()
{
    for (const PassOp& op : m_pass->apply)
    {
        if (op.kind == OpConstant)
            upload(m_pass->constants[op.index]);
        else
            applyState(m_pass->states[op.index]);
    }
    m_applied = changes();
}

void Run::renderState(DWORD state, DWORD value)
{
    if (state < MaxRenderStates)
    {
        m_state.renderStates[state] = value;
        touch(state);
    }
}

void Run::samplerState(DWORD sampler, DWORD type, DWORD value)
{
    if (sampler < MaxSamplerStages && type < MaxSamplerStates)
    {
        m_state.samplerStates[sampler][type] = value;
        touch(SamplerWords + sampler * MaxSamplerStates + type);
    }
}

void Run::commit()
{
    const uint32_t now = changes();
    if (m_applied == now)
        return;
    const uint32_t since = m_applied;
    for (const PassOp& op : m_pass->commits)
    {
        if (*op.version <= since)
            continue;
        if (op.kind == OpConstant)
            upload(m_pass->constants[op.index]);
        else
            applyState(m_pass->states[op.index]);
    }
    m_applied = now;
}

// The draw's state and constant blocks from the mirror and the banks; the blocks are rebuilt only when what they
// are made of changed.
bool Run::build(DWORD type, UINT vertices, UINT startIndex, UINT count, NativeGeometry& geometry, NativeDraw& draw,
                BlockInputs& blocks)
{
    if (!m_device->nativeGeometry(type, vertices, startIndex, count, geometry))
        return false;
    EffectSource source;
    source.state = &m_state;
    for (uint32_t i = 0; i < render::MaxSamplers; ++i)
        m_textures[i] = source.textures[i] = storageOf(m_state.textures[i]);
    source.vertexRegisters = m_vertex;
    source.pixelRegisters = m_pixel;
    source.programs = m_pass->programs;
    source.layout = geometry.layout;
    source.primitive = geometry.primitive;
    const render::TextureStorage* volume;
    const render::TextureStorage* volume2;
    // What effectState reads: the mirror (through its write count), the textures, programs, geometry and bias.
    StateKey inputs;
    memset(&inputs, 0, sizeof(inputs));
    inputs.changes = m_stateChanges;
    inputs.programs = source.programs;
    inputs.layout = source.layout;
    inputs.primitive = source.primitive;
    const uint8_t bias = render::vertexProgram(render::VertexProgram(source.programs.vertexProgram)).biasRegister;
    inputs.bias = bias != render::NoBias ? floatBits(m_vertex[4 * bias]) : 0;
    memcpy(inputs.textures, source.textures, sizeof(inputs.textures));
    if (stateReuse() && m_stateValid && memcmp(&inputs, &m_stateKey, sizeof(inputs)) == 0)
    {
        draw.state = m_lastState;
        draw.test[0] = m_lastTest[0];
        draw.test[1] = m_lastTest[1];
        volume = m_lastVolume;
        volume2 = m_lastVolume2;
    }
    else
    {
        effectState(source, draw.state, draw.test, &volume, &volume2);
        memcpy(&m_stateKey, &inputs, sizeof(inputs));
        m_stateValid = true;
        m_lastState = draw.state;
        m_lastTest[0] = draw.test[0];
        m_lastTest[1] = draw.test[1];
        m_lastVolume = volume;
        m_lastVolume2 = volume2;
    }
    const DWORD* rs = m_state.renderStates;
    draw.blendOp = rs[D3DRS_BLENDOP];
    draw.blendFactor = rs[D3DRS_BLENDFACTOR];
    draw.vertexFloats = render::vertexProgram(render::VertexProgram(draw.state.vertexProgram)).constantFloats;
    draw.fragmentFloats = render::fragmentProgram(render::FragmentProgram(draw.state.fragmentProgram)).constantFloats;
    const uint64_t v1 = uintptr_t(volume), v2 = uintptr_t(volume2);
    const uint32_t key[12] = { m_generation,
                               uint32_t(draw.state.vertexProgram) | uint32_t(draw.state.fragmentProgram) << 8,
                               rs[D3DRS_FOGENABLE], rs[D3DRS_FOGCOLOR],
                               m_state.stageStates[0][D3DTSS_TEXTURETRANSFORMFLAGS], floatBits(draw.test[0]),
                               floatBits(draw.test[1]), uint32_t(v1), uint32_t(v1 >> 32), uint32_t(v2),
                               uint32_t(v2 >> 32), 0 };
    const bool same = m_blockValid && memcmp(key, m_blockKey, sizeof(key)) == 0;
    draw.reuse = same && m_emit && m_device->nativeConstantsKept();
    blocks.source = source;
    blocks.state = &draw.state;
    blocks.test = draw.test;
    blocks.volume = volume;
    blocks.volume2 = volume2;
    if (!same)
    {
        // Production runs write new blocks straight into the frame (Device::nativeDraw).
        if (!m_emit || !directBlocks())
            writeBlocks(&blocks, draw.vertexFloats ? m_vertexBlock : nullptr,
                        draw.fragmentFloats ? m_fragmentBlock : nullptr);
        memcpy(m_blockKey, key, sizeof(key));
        m_blockValid = true;
    }
    draw.vertexConstants = m_vertexBlock;
    draw.fragmentConstants = m_fragmentBlock;
    return true;
}

void Run::writeBlocks(const void* context, float* vertex, float* fragment)
{
    const BlockInputs& b = *static_cast<const BlockInputs*>(context);
    effectConstants(b.source, *b.state, b.test, b.volume, b.volume2, vertex, fragment);
}

bool Run::drawIndexed(WinApplication* app, x86::CPU& cpu, DWORD type, UINT vertices, UINT startIndex, UINT count)
{
    NativeGeometry geometry;
    NativeDraw draw;
    BlockInputs blocks;
    if (!build(type, vertices, startIndex, count, geometry, draw, blocks))
        return false;
    if (m_emit)
    {
        if (directBlocks())
            m_device->nativeDraw(app, cpu, geometry, m_textures, draw, writeBlocks, &blocks);
        else
            m_device->nativeDraw(app, cpu, geometry, m_textures, draw);
        return true;
    }
    Kept kept;
    kept.geometry = geometry;
    kept.draw = draw;
    kept.vertex = uint32_t(m_keptFloats.size());
    m_keptFloats.insert(m_keptFloats.end(), draw.vertexConstants, draw.vertexConstants + draw.vertexFloats);
    kept.fragment = uint32_t(m_keptFloats.size());
    m_keptFloats.insert(m_keptFloats.end(), draw.fragmentConstants, draw.fragmentConstants + draw.fragmentFloats);
    m_kept.push_back(kept);
    return true;
}

// The state words the device logged since the run started join the touched ones; false when the log lost some.
bool Run::touchLogged()
{
    const uint32_t writes = m_device->stateWrites();
    if (writes - m_stateSeen > Device::WriteLogSize)
        return false;
    for (uint32_t n = m_stateSeen; n != writes; ++n)
    {
        const Device::Written& w = m_device->stateWritten(n);
        for (uint32_t word = w.first; word < uint32_t(w.first + w.count) && word < StateWords; ++word)
            touch(word);
    }
    return true;
}

void Run::end(WinApplication* app, x86::CPU& cpu)
{
    DeviceWrites writes{ m_device, app, cpu };
    // Every word that is not touched still equals the device's.
    if (deltaSync() && touchLogged())
    {
        endTouched(writes);
    }
    else
    {
        endAll(writes);
        m_seen = nullptr;
    }
    for (int pixel = 0; pixel < 2; ++pixel)
    {
        uint32_t* dirty = m_dirty[pixel];
        if (dirty[0] != dirty[1])
            m_device->setConstants(pixel != 0, dirty[0], (pixel ? m_pixel : m_vertex) + 4 * dirty[0],
                                   dirty[1] - dirty[0]);
        dirty[0] = dirty[1] = 0;
    }
    m_constantSeen = m_device->constantWrites();
}

// End's state writes for the touched words, in the order endAll makes them.
template< typename S >
void Run::endTouched(S& writes)
{
    const FixedState& fs = m_device->fixedState();
    // Calls f for the touched words in [from, to), in order.
    const auto each = [this](uint32_t from, uint32_t to, auto f) {
        for (uint32_t word = from; word < to;)
        {
            const uint32_t bits = m_touched[word >> 5] >> (word & 31);
            if (!bits)
            {
                word = (word | 31) + 1;
                continue;
            }
            word += uint32_t(__builtin_ctz(bits));
            if (word >= to)
                break;
            f(word);
            ++word;
        }
    };
    each(0, StageWords, [&](uint32_t s) {
        if (!m_pass->savedRender[s] && m_state.renderStates[s] != fs.renderStates[s])
            writes.render(s, m_state.renderStates[s]);
    });
    each(StageWords, SamplerWords, [&](uint32_t word) {
        const uint32_t stage = (word - StageWords) / D3DTSS_COUNT, t = (word - StageWords) % D3DTSS_COUNT;
        if (!m_pass->savedStage[stage][t] && m_state.stageStates[stage][t] != fs.stageStates[stage][t])
            writes.stage(stage, t, m_state.stageStates[stage][t]);
    });
    for (DWORD sampler = 0; sampler < MaxSamplerStages; ++sampler)
    {
        const uint32_t first = SamplerWords + sampler * MaxSamplerStates;
        each(first, first + MaxSamplerStates, [&](uint32_t word) {
            const uint32_t t = word - first;
            if (!m_pass->savedSampler[sampler][t] && m_state.samplerStates[sampler][t] != fs.samplerStates[sampler][t])
                writes.sampler(sampler, t, m_state.samplerStates[sampler][t]);
        });
        if (!m_pass->savedTexture[sampler] && m_state.textures[sampler] != m_device->stageTexture(sampler))
            writes.texture(sampler, m_state.textures[sampler]);
    }
}

// End's state writes: every state the technique does not save that differs from the device's.
template< typename S >
void Run::endAll(S& writes)
{
    const FixedState& fs = m_device->fixedState();
    for (DWORD s = 0; s < MaxRenderStates; ++s)
    {
        if (!m_pass->savedRender[s] && m_state.renderStates[s] != fs.renderStates[s])
            writes.render(s, m_state.renderStates[s]);
    }
    for (DWORD stage = 0; stage < MaxStages; ++stage)
        for (DWORD t = 0; t < D3DTSS_COUNT; ++t)
        {
            if (!m_pass->savedStage[stage][t] && m_state.stageStates[stage][t] != fs.stageStates[stage][t])
                writes.stage(stage, t, m_state.stageStates[stage][t]);
        }
    for (DWORD sampler = 0; sampler < MaxSamplerStages; ++sampler)
    {
        for (DWORD t = 0; t < MaxSamplerStates; ++t)
        {
            if (!m_pass->savedSampler[sampler][t] && m_state.samplerStates[sampler][t] != fs.samplerStates[sampler][t])
                writes.sampler(sampler, t, m_state.samplerStates[sampler][t]);
        }
        if (!m_pass->savedTexture[sampler] && m_state.textures[sampler] != m_device->stageTexture(sampler))
            writes.texture(sampler, m_state.textures[sampler]);
    }
}

#if NFSC_RENDER_CHECK
bool Run::drawsFor(uint32_t serial)
{
    const uint32_t made = m_device->drawSerial() - serial;
    const bool matched = made == m_kept.size();
    if (matched)
    {
        const uint32_t last = m_device->lastDrawIndex();
        for (uint32_t k = 0; k < made; ++k)
        {
            NativeDraw draw = m_kept[k].draw;
            draw.vertexConstants = m_keptFloats.data() + m_kept[k].vertex;
            draw.fragmentConstants = m_keptFloats.data() + m_kept[k].fragment;
            m_device->recordNativeCheck(last - (made - 1 - k), m_kept[k].geometry, draw);
        }
    }
    m_kept.clear();
    m_keptFloats.clear();
    return matched;
}

uint32_t Run::checkEnd(bool report)
{
    const FixedState& fs = m_device->fixedState();
    uint32_t differ = 0;
    auto mismatch = [&differ, report](const char* what, uint32_t index, uint32_t type, uintptr_t expected,
                                      uintptr_t actual) {
        if (report && differ < 4)
            pf::logInfo("render check: effect end: %s %u %u native 0x%08x translated 0x%08x", what, index, type,
                        uint32_t(expected), uint32_t(actual));
        ++differ;
    };
    // A production end's writes from the touched words against the writes from every word, on this device state.
    if (touchLogged())
    {
        ListedWrites all, touched;
        endAll(all);
        endTouched(touched);
        if (all.calls != touched.calls)
            mismatch("end writes", uint32_t(all.calls.size() / 4), uint32_t(touched.calls.size() / 4), 0, 0);
    }
    for (uint32_t s = 0; s < MaxRenderStates; ++s)
    {
        const DWORD expected = m_pass->savedRender[s] ? m_before.renderStates[s] : m_state.renderStates[s];
        if (expected != fs.renderStates[s])
            mismatch("render state", s, 0, expected, fs.renderStates[s]);
    }
    for (uint32_t stage = 0; stage < MaxStages; ++stage)
        for (uint32_t t = 0; t < D3DTSS_COUNT; ++t)
        {
            const DWORD expected = m_pass->savedStage[stage][t] ? m_before.stageStates[stage][t]
                                                                 : m_state.stageStates[stage][t];
            if (expected != fs.stageStates[stage][t])
                mismatch("stage state", stage, t, expected, fs.stageStates[stage][t]);
        }
    for (uint32_t sampler = 0; sampler < MaxSamplerStages; ++sampler)
    {
        for (uint32_t t = 0; t < MaxSamplerStates; ++t)
        {
            const DWORD expected = m_pass->savedSampler[sampler][t] ? m_before.samplerStates[sampler][t]
                                                                     : m_state.samplerStates[sampler][t];
            if (expected != fs.samplerStates[sampler][t])
                mismatch("sampler state", sampler, t, expected, fs.samplerStates[sampler][t]);
        }
        const Object* expected = m_pass->savedTexture[sampler] ? m_before.textures[sampler] : m_state.textures[sampler];
        if (expected != m_device->stageTexture(sampler))
            mismatch("texture", sampler, 0, uintptr_t(expected), uintptr_t(m_device->stageTexture(sampler)));
    }
    const float* vertex = m_device->vertexConstantsF();
    const float* pixel = m_device->pixelConstantsF();
    for (uint32_t r = 0; r < Device::VertexFloatConstants; ++r)
    {
        if (!sameWords(m_vertex + 4 * r, vertex + 4 * r, 4))
            mismatch("vertex register", r, 0, floatBits(m_vertex[4 * r]), floatBits(vertex[4 * r]));
    }
    for (uint32_t r = 0; r < Device::PixelFloatConstants; ++r)
    {
        if (!sameWords(m_pixel + 4 * r, pixel + 4 * r, 4))
            mismatch("pixel register", r, 0, floatBits(m_pixel[4 * r]), floatBits(pixel[4 * r]));
    }
    return differ;
}
#endif

}}}
