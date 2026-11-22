// World textures of the track regions' stream sections at reduced resolution. Each region's table gives a section
// with texture packs a smaller size, and its reads of the stream file give the section as if it were repacked
// without the top mip levels of its textures: world_texture_drop levels for the world texture section Y0 (eleven
// texture packs that the streamer reads whole into one Main Pool block when the world loads, 0x79ebf4),
// section_texture_drop for the other sections. The other chunks keep their offsets modulo 128, so the part of a
// section that stays after activation (the perm chunks, which 0x699420 copies out) keeps its layout and size. The
// layouts are built on a thread of their own from start-up and kept in worldtex.cache in the data folder, which
// later starts read while the region's files and the levels are unchanged; a region's files wait for its layouts
// when first read.

#include "worldtex.h"

#include <lib/file.h>
#include <platform/config.h>
#include <platform/cores.h>
#include <platform/log.h>
#include <platform/sync.h>
#include <platform/time.h>
#include <recomp.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace nfsc { namespace worldtex {

namespace
{

struct RegionFiles
{
    const char* name;
    const char* table;
    const char* stream;
    const char* tableSuffix;
    const char* streamSuffix;
};

// The smaller region first: the layouts are built in this order.
const RegionFiles Files[] = {
    { "L5RB", "C:\\TRACKS\\L5RB.BUN", "C:\\TRACKS\\STREAML5RB.BUN", "tracks/l5rb.bun", "tracks/streaml5rb.bun" },
    { "L5RA", "C:\\TRACKS\\L5RA.BUN", "C:\\TRACKS\\STREAML5RA.BUN", "tracks/l5ra.bun", "tracks/streaml5ra.bun" },
};
const int RegionCount = 2;

const x86::reg32 ChunkStreamTable = 0x00034110;
const x86::reg32 ChunkDiscBundles = 0x00034113;
const x86::reg32 ChunkTexturePack = 0xB3300000;
const x86::reg32 ChunkPackHeader = 0xB3310000;
const x86::reg32 ChunkTextureInfos = 0x33310004;
const x86::reg32 ChunkTextureFormats = 0x33310005;
const x86::reg32 ChunkPackData = 0xB3320000;
const x86::reg32 ChunkPackPixels = 0x33320002;

// Section records (0x5C bytes): number +0x08 (int16), file offset +0x14, size +0x18, compressed size +0x1C; at run
// time the loaded block +0x4C and its size +0x54.
const x86::reg32 RecordSize = 0x5C;
const x86::reg32 RecNumber = 0x08;
const x86::reg32 RecFileOffset = 0x14;
const x86::reg32 RecSize = 0x18;
const x86::reg32 RecCompressedSize = 0x1C;
const x86::reg32 RecMemory = 0x4C;
const x86::reg32 RecLoadedSize = 0x54;
const x86::sreg16 SectionY0 = 2500;

// Texture info entries, each after 12 bytes: placement +0x08, image size +0x10, top level size +0x18, width and
// height +0x1C/+0x1E (int16), log2 of both +0x20/+0x21, mip levels +0x26, name length +0x4C, name +0x4D.
const x86::reg32 InfoPrefix = 12;
const x86::reg32 InfoPlacement = 0x08;
const x86::reg32 InfoImageSize = 0x10;
const x86::reg32 InfoTopSize = 0x18;
const x86::reg32 InfoWidth = 0x1C;
const x86::reg32 InfoHeight = 0x1E;
const x86::reg32 InfoShiftWidth = 0x20;
const x86::reg32 InfoShiftHeight = 0x21;
const x86::reg32 InfoMipLevels = 0x26;
const x86::reg32 InfoNameLength = 0x4C;
const x86::reg32 InfoName = 0x4D;
// Format entries: 0x18 bytes, D3DFORMAT at +0x0C.
const x86::reg32 FormatEntry = 0x18;
const x86::reg32 FormatCode = 0x0C;
const x86::reg32 FormatDxt1 = 0x31545844;
const x86::reg32 FormatDxt3 = 0x33545844;
const x86::reg32 FormatDxt5 = 0x35545844;

// The streamer object: sections +0, section count +4. TextureInfo image data +0x50. The game's mip skip of world
// textures (0x725e0d), set from the world detail setting by eDisplayFrame.
const x86::reg32 TrackStreamer = 0x00B70650;
const x86::reg32 TexImageData = 0x50;
const x86::reg32 MipSkip = 0x00AB09CC;

// A run of a rebuilt section: bytes of a rewritten pack header, or of the stream file.
struct Segment
{
    x86::reg32 offset;      // in the rebuilt section
    x86::reg32 size;
    x86::reg32 source;      // stream file offset when header < 0
    int        header;
    int        drop;        // mip levels left out of the texture whose data this is
};

struct Section
{
    x86::reg32 recordPos;   // the record's position in the table file
    x86::reg32 fileOffset;
    x86::reg32 size;
    x86::reg32 newSize;
    x86::reg32 firstSegment;
    x86::reg32 endSegment;
};

struct Region
{
    std::vector<Section>                 sections;   // by file offset
    std::vector<Segment>                 segments;
    std::vector<std::vector<x86::reg8>>  headers;
    std::atomic<bool>                    built{ false };
    pf::Semaphore*                       ready = nullptr;
};

struct Texture
{
    x86::reg32 entry;       // in the pack header
    x86::reg32 placement;
    x86::reg32 imageSize;
    x86::reg32 dropped;     // bytes of the levels left out
    int        levels;      // levels left out
};

bool         s_installed = false;
int          s_worldLevels = 0;
int          s_sectionLevels = 0;
Region       s_regions[RegionCount];
pf::Thread*  s_builder = nullptr;
x86::reg32   s_cachedTable = 0;
x86::reg32   s_cachedRecord = 0;
const Region*  s_cachedRegion = nullptr;
const Section* s_cachedSection = nullptr;
bool         s_loggedSkip = false;

x86::reg32 rd32(const x86::reg8* p)
{
    x86::reg32 v;
    memcpy(&v, p, 4);
    return v;
}

x86::reg16 rd16(const x86::reg8* p)
{
    x86::reg16 v;
    memcpy(&v, p, 2);
    return v;
}

void wr32(x86::reg8* p, x86::reg32 v)
{
    memcpy(p, &v, 4);
}

void wr16(x86::reg8* p, x86::reg16 v)
{
    memcpy(p, &v, 2);
}

x86::reg32 align128(x86::reg32 v)
{
    return (v + 127) & ~x86::reg32(127);
}

// Up to size bytes at offset; the bytes read, or -1.
x86::sreg32 readUpTo(int file, x86::reg32 offset, void* buffer, x86::reg32 size)
{
    if (lseek(file, off_t(offset), SEEK_SET) != off_t(offset))
        return -1;
    x86::reg8* p = static_cast<x86::reg8*>(buffer);
    x86::reg32 done = 0;
    while (done < size)
    {
        const ssize_t n = ::read(file, p + done, size - done);
        if (n < 0)
            return -1;
        if (n == 0)
            break;
        done += x86::reg32(n);
    }
    return x86::sreg32(done);
}

bool readAt(int file, x86::reg32 offset, void* buffer, x86::reg32 size)
{
    return readUpTo(file, offset, buffer, size) == x86::sreg32(size);
}

// Small reads of a file through a window: chunk headers often lie close together.
class Reader
{
public:
    explicit Reader(int file) : m_file(file), m_start(0), m_length(0), m_window(Window) {}

    bool read(x86::reg32 offset, void* buffer, x86::reg32 size)
    {
        if (size > Window)
            return readAt(m_file, offset, buffer, size);
        if (offset < m_start || offset - m_start > m_length || m_length - (offset - m_start) < size)
        {
            m_start = offset & ~(Window - 1);
            if (offset + size - m_start > Window)
                m_start = offset;
            const x86::sreg32 n = readUpTo(m_file, m_start, m_window.data(), Window);
            m_length = n < 0 ? 0 : x86::reg32(n);
            if (offset - m_start > m_length || m_length - (offset - m_start) < size)
                return false;
        }
        memcpy(buffer, &m_window[offset - m_start], size);
        return true;
    }

private:
    static const x86::reg32 Window = 4096;
    int                     m_file;
    x86::reg32              m_start;
    x86::reg32              m_length;
    std::vector<x86::reg8>  m_window;
};

// A chunk header at offset: false when it cannot be read or runs past end.
bool chunkAt(Reader& reader, x86::reg32 base, x86::reg32 offset, x86::reg32 end, x86::reg32* id, x86::reg32* length)
{
    x86::reg8 head[8];
    if (end - offset < 8 || !reader.read(base + offset, head, 8))
        return false;
    *id = rd32(head);
    *length = rd32(head + 4);
    return *length <= end - offset - 8;
}

// Bytes of one DXT mip level.
x86::reg32 levelSize(int width, int height, x86::reg32 block)
{
    return x86::reg32(std::max(1, (width + 3) / 4) * std::max(1, (height + 3) / 4)) * block;
}

// Length of the sub-chunk at offset of a header buffer, or 0 when it does not end by end.
x86::reg32 subChunkEnd(const std::vector<x86::reg8>& header, x86::reg32 offset, x86::reg32 end)
{
    if (end < offset || end - offset < 8)
        return 0;
    const x86::reg32 length = rd32(&header[offset + 4]);
    return length <= end - offset - 8 ? offset + 8 + length : 0;
}

// One texture pack at [start, end) of the section at base, placed at out (same value modulo 128 as start): its
// header with the texture infos rewritten, then its textures at 128-byte placements without the top levels of DXT
// textures that have more levels (not "ADS_" textures, which the game never reduces). Gives the end of its content
// and the offsets of its data and pixel chunk headers in the pack.
const char* buildPack(Region& region, Reader& reader, x86::reg32 base, x86::reg32 start, x86::reg32 end,
                      x86::reg32 out, int levels, x86::reg32* contentEnd, x86::reg32* dataRel, x86::reg32* pixelsRel)
{
    x86::reg32 id, length;
    x86::reg32 dataPos = 0, pixelsPos = 0, lastId = 0;
    for (x86::reg32 child = start + 8; child < end; child += 8 + length)
    {
        if (!chunkAt(reader, base, child, end, &id, &length))
            return "pack child";
        lastId = id;
        dataPos = child;
    }
    if (lastId != ChunkPackData)
        return "pack data not last";
    lastId = 0;
    for (x86::reg32 child = dataPos + 8; child < end; child += 8 + length)
    {
        if (!chunkAt(reader, base, child, end, &id, &length))
            return "pack data child";
        lastId = id;
        pixelsPos = child;
    }
    if (lastId != ChunkPackPixels)
        return "pack pixels not last";
    // The loader aligns the pixel base to 128 bytes inside the payload.
    const x86::reg32 pixels = align128(pixelsPos + 8);
    if (pixels > end)
        return "pack pixels";
    const x86::reg32 headerSize = pixels - start;
    std::vector<x86::reg8> header(headerSize);
    if (!reader.read(base + start, header.data(), headerSize))
        return "pack header not readable";

    // Texture infos and formats inside the pack header chunk.
    x86::reg32 infos = 0, infosEnd = 0, formats = 0, formatsEnd = 0;
    const x86::reg32 headersEnd = dataPos - start;
    for (x86::reg32 child = 8, childEnd; child < headersEnd; child = childEnd)
    {
        childEnd = subChunkEnd(header, child, headersEnd);
        if (!childEnd)
            return "pack header chunk";
        if (rd32(&header[child]) != ChunkPackHeader)
            continue;
        for (x86::reg32 sub = child + 8, subEnd; sub < childEnd; sub = subEnd)
        {
            subEnd = subChunkEnd(header, sub, childEnd);
            if (!subEnd)
                return "pack header sub-chunk";
            if (rd32(&header[sub]) == ChunkTextureInfos)
            {
                infos = sub + 8;
                infosEnd = subEnd;
            }
            else if (rd32(&header[sub]) == ChunkTextureFormats)
            {
                formats = sub + 8;
                formatsEnd = subEnd;
            }
        }
    }
    if (!infos || !formats)
        return "pack without texture infos";

    std::vector<Texture> textures;
    x86::reg32 index = 0;
    for (x86::reg32 at = infos; at + InfoPrefix + InfoName + 1 <= infosEnd; ++index)
    {
        x86::reg8* entry = &header[at + InfoPrefix];
        const x86::reg32 nameLength = entry[InfoNameLength];
        if (at + InfoPrefix + InfoName + nameLength > infosEnd)
            return "texture info";
        const x86::reg32 format = formats + index * FormatEntry;
        if (format + FormatEntry > formatsEnd)
            return "format table";
        const x86::reg32 code = rd32(&header[format + FormatCode]);
        const x86::reg32 block = code == FormatDxt1 ? 8 : (code == FormatDxt3 || code == FormatDxt5) ? 16 : 0;
        const int width = x86::sreg16(rd16(entry + InfoWidth));
        const int height = x86::sreg16(rd16(entry + InfoHeight));
        const int mips = x86::sreg8(entry[InfoMipLevels]);
        const int shiftWidth = entry[InfoShiftWidth];
        const int shiftHeight = entry[InfoShiftHeight];
        Texture t = { at + InfoPrefix, rd32(entry + InfoPlacement), rd32(entry + InfoImageSize), 0, 0 };
        bool reducible = block && mips > 1 && mips <= 16 && shiftWidth < 16 && shiftHeight < 16 &&
                         width == 1 << shiftWidth && height == 1 << shiftHeight &&
                         !(nameLength >= 4 && memcmp(entry + InfoName, "ADS_", 4) == 0);
        if (reducible)
        {
            x86::reg32 sizes[16];
            x86::reg32 total = 0;
            for (int l = 0; l < mips; ++l)
            {
                sizes[l] = levelSize(std::max(1, width >> l), std::max(1, height >> l), block);
                total += sizes[l];
            }
            reducible = total == t.imageSize && sizes[0] == rd32(entry + InfoTopSize);
            const int drop = std::min(levels, mips - 1);
            if (reducible)
            {
                for (int l = 0; l < drop; ++l)
                    t.dropped += sizes[l];
                t.levels = drop;
                wr32(entry + InfoImageSize, t.imageSize - t.dropped);
                wr32(entry + InfoTopSize, sizes[drop]);
                wr16(entry + InfoWidth, x86::reg16(std::max(1, width >> drop)));
                wr16(entry + InfoHeight, x86::reg16(std::max(1, height >> drop)));
                entry[InfoShiftWidth] = x86::reg8(std::max(0, shiftWidth - drop));
                entry[InfoShiftHeight] = x86::reg8(std::max(0, shiftHeight - drop));
                entry[InfoMipLevels] = x86::reg8(mips - drop);
            }
        }
        textures.push_back(t);
        at = ((start + at + InfoPrefix + InfoName + nameLength + 3) & ~x86::reg32(3)) - start;
    }
    if (index * FormatEntry != formatsEnd - formats)
        return "format count";

    std::sort(textures.begin(), textures.end(),
              [](const Texture& a, const Texture& b) { return a.placement < b.placement; });
    const int headerIndex = int(region.headers.size());
    region.segments.push_back(Segment{ out, headerSize, 0, headerIndex, 0 });
    x86::reg32 placed = 0, previousEnd = 0;
    for (const Texture& t : textures)
    {
        if (t.placement < previousEnd || t.imageSize > end - pixels || t.placement > end - pixels - t.imageSize)
            return "texture placement";
        previousEnd = t.placement + t.imageSize;
        placed = align128(placed);
        wr32(&header[t.entry + InfoPlacement], placed);
        const x86::reg32 size = t.imageSize - t.dropped;
        if (size)
            region.segments.push_back(Segment{ out + headerSize + placed, size,
                                               base + pixels + t.placement + t.dropped, -1, t.levels });
        placed += size;
    }
    region.headers.push_back(std::move(header));
    *contentEnd = out + headerSize + placed;
    *dataRel = dataPos - start;
    *pixelsRel = pixelsPos - start;
    return nullptr;
}

// Sets the sizes of the pack placed at out, which now ends at next.
void closePack(Region& region, x86::reg32 out, x86::reg32 next, x86::reg32 dataRel, x86::reg32 pixelsRel)
{
    std::vector<x86::reg8>& header = region.headers.back();
    wr32(&header[4], next - out - 8);
    wr32(&header[dataRel + 4], next - (out + dataRel) - 8);
    wr32(&header[pixelsRel + 4], next - (out + pixelsRel) - 8);
}

// The section at base (size bytes) rebuilt without the top levels of its textures: its segments and headers are
// added to the region and its new size given; a section without texture packs, or one that gets no smaller, adds
// nothing and keeps its size.
const char* buildSection(Region& region, Reader& reader, x86::reg32 base, x86::reg32 size, int levels,
                         x86::reg32* newSize)
{
    struct Chunk
    {
        x86::reg32 start, end, id;
    };
    std::vector<Chunk> chunks;
    bool packs = false;
    x86::reg32 id, length;
    for (x86::reg32 pos = 0; pos < size; pos += 8 + length)
    {
        if (!chunkAt(reader, base, pos, size, &id, &length))
            return "section chunk";
        chunks.push_back(Chunk{ pos, pos + 8 + length, id });
        packs = packs || id == ChunkTexturePack;
    }
    *newSize = size;
    if (!packs)
        return nullptr;
    const size_t firstSegment = region.segments.size();
    const size_t firstHeader = region.headers.size();
    x86::reg32 out = 0;
    const char* error = nullptr;
    for (size_t i = 0; i < chunks.size() && !error; ++i)
    {
        const Chunk& c = chunks[i];
        out += (c.start - out) & 127;
        if (c.id != ChunkTexturePack)
        {
            Segment* last = region.segments.size() > firstSegment ? &region.segments.back() : nullptr;
            if (last && last->header < 0 && !last->drop && last->offset + last->size == out &&
                last->source + last->size == base + c.start)
                last->size += c.end - c.start;
            else
                region.segments.push_back(Segment{ out, c.end - c.start, base + c.start, -1, 0 });
            out += c.end - c.start;
            continue;
        }
        x86::reg32 contentEnd = 0, dataRel = 0, pixelsRel = 0;
        error = buildPack(region, reader, base, c.start, c.end, out, levels, &contentEnd, &dataRel, &pixelsRel);
        if (error)
            break;
        const x86::reg32 nextStart = i + 1 < chunks.size() ? chunks[i + 1].start : size;
        const x86::reg32 next = contentEnd + ((nextStart - contentEnd) & 127);
        closePack(region, out, next, dataRel, pixelsRel);
        out = next;
    }
    if (error || out >= size)
    {
        region.segments.resize(firstSegment);
        region.headers.resize(firstHeader);
        return error;
    }
    *newSize = out;
    return nullptr;
}

// The region's sections that get smaller; errors leave the region as it is.
const char* buildRegion(int index, x86::reg32* sizeBefore, x86::reg32* sizeAfter, int* skipped)
{
    Region& region = s_regions[index];
    const std::string tablePath = win32::File::hostPath(Files[index].table);
    const std::string streamPath = win32::File::hostPath(Files[index].stream);
    const int tableFile = open(tablePath.c_str(), O_RDONLY);
    if (tableFile < 0)
        return "table file not found";
    const off_t tableSize = lseek(tableFile, 0, SEEK_END);
    std::vector<x86::reg8> table(tableSize > 0 ? size_t(tableSize) : 0);
    const bool tableRead = !table.empty() && readAt(tableFile, 0, table.data(), x86::reg32(table.size()));
    close(tableFile);
    if (!tableRead)
        return "table file not readable";
    x86::reg32 records = 0, recordsEnd = 0;
    for (x86::reg32 pos = 0; table.size() - pos >= 8;)
    {
        const x86::reg32 id = rd32(&table[pos]);
        const x86::reg32 length = rd32(&table[pos + 4]);
        if (length > table.size() - pos - 8)
            break;
        if (id == ChunkStreamTable)
        {
            records = pos + 8;
            recordsEnd = pos + 8 + length;
        }
        else if (id == ChunkDiscBundles && length)
        {
            return "disc bundles";
        }
        pos += 8 + length;
    }
    if (!records)
        return "no section table";
    const int stream = open(streamPath.c_str(), O_RDONLY);
    if (stream < 0)
        return "stream file not found";
    const off_t streamSize = lseek(stream, 0, SEEK_END);
    Reader reader(stream);
    const char* firstError = nullptr;
    for (x86::reg32 r = records; r + RecordSize <= recordsEnd; r += RecordSize)
    {
        const x86::reg8* record = &table[r];
        const int levels = x86::sreg16(rd16(record + RecNumber)) == SectionY0 ? s_worldLevels : s_sectionLevels;
        const x86::reg32 offset = rd32(record + RecFileOffset);
        const x86::reg32 size = rd32(record + RecSize);
        if (!levels || !size || rd32(record + RecCompressedSize) != size ||
            x86::reg64(offset) + size > x86::reg64(streamSize))
            continue;
        const x86::reg32 firstSegment = x86::reg32(region.segments.size());
        x86::reg32 newSize = size;
        const char* error = buildSection(region, reader, offset, size, levels, &newSize);
        if (error)
        {
            if (!firstError)
            {
                firstError = error;
                pf::logError("world textures: region %s section %.8s: %s; it loads at full size", Files[index].name,
                             reinterpret_cast<const char*>(record), error);
            }
            ++*skipped;
            continue;
        }
        if (newSize == size)
            continue;
        region.sections.push_back(Section{ r, offset, size, newSize, firstSegment,
                                           x86::reg32(region.segments.size()) });
        *sizeBefore += size;
        *sizeAfter += newSize;
    }
    close(stream);
    std::sort(region.sections.begin(), region.sections.end(),
              [](const Section& a, const Section& b) { return a.fileOffset < b.fileOffset; });
    for (size_t i = 1; i < region.sections.size(); ++i)
    {
        if (region.sections[i].fileOffset < region.sections[i - 1].fileOffset + region.sections[i - 1].size)
            return "overlapping sections";
    }
    return nullptr;
}

const char* const CacheFile = "C:\\worldtex.cache";

// What a region's layouts are built from: its files' sizes and modification times and the levels left out.
struct CacheKey
{
    x86::reg64  tableSize;
    x86::reg64  tableTime;
    x86::reg64  streamSize;
    x86::reg64  streamTime;
    x86::sreg32 worldLevels;
    x86::sreg32 sectionLevels;
};

bool cacheKey(int index, CacheKey* key)
{
    struct stat table, stream;
    if (stat(win32::File::hostPath(Files[index].table).c_str(), &table) != 0 ||
        stat(win32::File::hostPath(Files[index].stream).c_str(), &stream) != 0)
        return false;
    memset(key, 0, sizeof(*key));
    key->tableSize = x86::reg64(table.st_size);
    key->tableTime = x86::reg64(table.st_mtime);
    key->streamSize = x86::reg64(stream.st_size);
    key->streamTime = x86::reg64(stream.st_mtime);
    key->worldLevels = s_worldLevels;
    key->sectionLevels = s_sectionLevels;
    return true;
}

// The cache file in the data folder: the magic, then entries, newest first, each of one region under one key (the
// region's index, the key, the size of the rest, then the counts of sections, segments and headers, the sections,
// the segments and the headers, each after its size), and the magic again at the end. It keeps the entries of the
// last MaxCacheEntries builds, so other settings or game files keep theirs.
const char CacheMagic[8] = { 'N', 'F', 'S', 'C', 'W', 'T', 'X', '2' };
const size_t MaxCacheEntries = 8;

struct CacheEntry
{
    x86::reg32             region;
    CacheKey               key;
    std::vector<x86::reg8> body;
};

class CacheReader
{
public:
    explicit CacheReader(const std::vector<x86::reg8>& data) : m_data(data), m_at(0) {}
    bool take(void* out, size_t size)
    {
        if (m_data.size() - m_at < size)
            return false;
        memcpy(out, &m_data[m_at], size);
        m_at += size;
        return true;
    }
    bool skip(size_t size)
    {
        if (m_data.size() - m_at < size)
            return false;
        m_at += size;
        return true;
    }
    bool atEnd() const { return m_at == m_data.size(); }

private:
    const std::vector<x86::reg8>& m_data;
    size_t                        m_at;
};

// A cache entry's body into region; false when it is not consistent.
bool parseBody(const std::vector<x86::reg8>& body, Region& region)
{
    CacheReader reader(body);
    x86::reg32 counts[3];
    if (!reader.take(counts, sizeof(counts)) || counts[0] > body.size() || counts[1] > body.size() ||
        counts[2] > body.size())
        return false;
    region.sections.resize(counts[0]);
    region.segments.resize(counts[1]);
    region.headers.resize(counts[2]);
    if (!reader.take(region.sections.data(), region.sections.size() * sizeof(Section)) ||
        !reader.take(region.segments.data(), region.segments.size() * sizeof(Segment)))
        return false;
    for (std::vector<x86::reg8>& header : region.headers)
    {
        x86::reg32 size;
        if (!reader.take(&size, 4) || size > body.size())
            return false;
        header.resize(size);
        if (!reader.take(header.data(), size))
            return false;
    }
    for (const Section& s : region.sections)
    {
        if (s.firstSegment > s.endSegment || s.endSegment > counts[1])
            return false;
    }
    for (const Segment& s : region.segments)
    {
        if (s.header >= int(counts[2]) || (s.header >= 0 && region.headers[s.header].size() < s.size))
            return false;
    }
    return reader.atEnd();
}

// Reads the cache file's entries into entries and fills the regions whose key matches from them; true in loaded for
// each region it filled.
void loadCache(const CacheKey* keys, const bool* haveKey, bool* loaded, std::vector<CacheEntry>* entries)
{
    FILE* file = fopen(win32::File::hostPath(CacheFile).c_str(), "rb");
    if (!file)
        return;
    std::vector<x86::reg8> data;
    if (fseek(file, 0, SEEK_END) == 0)
    {
        const long size = ftell(file);
        data.resize(size > 0 ? size_t(size) : 0);
        fseek(file, 0, SEEK_SET);
        if (fread(data.data(), 1, data.size(), file) != data.size())
            data.clear();
    }
    fclose(file);
    char magic[8];
    if (data.size() < 16 || memcmp(&data[data.size() - 8], CacheMagic, 8) != 0)
        return;
    data.resize(data.size() - 8);
    CacheReader reader(data);
    if (!reader.take(magic, 8) || memcmp(magic, CacheMagic, 8) != 0)
        return;
    std::vector<CacheEntry> read;
    while (!reader.atEnd())
    {
        CacheEntry entry;
        x86::reg32 size;
        if (!reader.take(&entry.region, 4) || !reader.take(&entry.key, sizeof(entry.key)) || !reader.take(&size, 4) ||
            size > data.size() || entry.region >= x86::reg32(RegionCount))
            return;
        entry.body.resize(size);
        if (!reader.take(entry.body.data(), size))
            return;
        read.push_back(std::move(entry));
    }
    for (const CacheEntry& entry : read)
    {
        const int i = int(entry.region);
        if (loaded[i] || !haveKey[i] || memcmp(&entry.key, &keys[i], sizeof(entry.key)) != 0)
            continue;
        Region staged;
        if (!parseBody(entry.body, staged))
            continue;
        s_regions[i].sections.swap(staged.sections);
        s_regions[i].segments.swap(staged.segments);
        s_regions[i].headers.swap(staged.headers);
        loaded[i] = true;
    }
    entries->swap(read);
}

// A region's layouts as a cache entry body.
std::vector<x86::reg8> entryBody(const Region& region)
{
    std::vector<x86::reg8> body;
    const auto add = [&body](const void* data, size_t size) {
        const x86::reg8* p = static_cast<const x86::reg8*>(data);
        body.insert(body.end(), p, p + size);
    };
    const x86::reg32 counts[3] = { x86::reg32(region.sections.size()), x86::reg32(region.segments.size()),
                                   x86::reg32(region.headers.size()) };
    add(counts, sizeof(counts));
    add(region.sections.data(), region.sections.size() * sizeof(Section));
    add(region.segments.data(), region.segments.size() * sizeof(Segment));
    for (const std::vector<x86::reg8>& header : region.headers)
    {
        const x86::reg32 size = x86::reg32(header.size());
        add(&size, 4);
        add(header.data(), header.size());
    }
    return body;
}

// Writes the cacheable regions' entries first, then the earlier entries of other regions or keys, up to
// MaxCacheEntries.
void saveCache(const CacheKey* keys, const bool* cacheable, const std::vector<CacheEntry>& earlier)
{
    std::vector<CacheEntry> entries;
    for (int i = 0; i < RegionCount; ++i)
    {
        if (cacheable[i])
            entries.push_back(CacheEntry{ x86::reg32(i), keys[i], entryBody(s_regions[i]) });
    }
    for (const CacheEntry& entry : earlier)
    {
        if (entries.size() >= MaxCacheEntries)
            break;
        const int i = int(entry.region);
        if (!cacheable[i] || memcmp(&entry.key, &keys[i], sizeof(entry.key)) != 0)
            entries.push_back(entry);
    }
    const std::string path = win32::File::hostPath(CacheFile);
    FILE* file = fopen(path.c_str(), "wb");
    if (!file)
    {
        pf::logError("world textures: %s not writable", path.c_str());
        return;
    }
    bool ok = fwrite(CacheMagic, 8, 1, file) == 1;
    for (const CacheEntry& entry : entries)
    {
        const x86::reg32 size = x86::reg32(entry.body.size());
        ok = ok && fwrite(&entry.region, 4, 1, file) == 1 && fwrite(&entry.key, sizeof(entry.key), 1, file) == 1 &&
             fwrite(&size, 4, 1, file) == 1 && fwrite(entry.body.data(), 1, size, file) == size;
    }
    ok = ok && fwrite(CacheMagic, 8, 1, file) == 1;
    ok = fclose(file) == 0 && ok;
    if (!ok)
    {
        pf::logError("world textures: writing %s failed", path.c_str());
        remove(path.c_str());
        return;
    }
    pf::logInfo("world textures: layouts written to %s (%u entries)", path.c_str(), unsigned(entries.size()));
}

int build(void*)
{
    CacheKey keys[RegionCount];
    bool haveKey[RegionCount], loaded[RegionCount] = {}, cacheable[RegionCount] = {};
    for (int i = 0; i < RegionCount; ++i)
        haveKey[i] = cacheKey(i, &keys[i]);
    const uint32_t loadStart = pf::ticksMs();
    std::vector<CacheEntry> entries;
    loadCache(keys, haveKey, loaded, &entries);
    const uint32_t loadMs = pf::ticksMs() - loadStart;
    bool built = false;
    for (int i = 0; i < RegionCount; ++i)
    {
        Region& region = s_regions[i];
        if (loaded[i])
        {
            x86::reg64 before = 0, after = 0;
            for (const Section& s : region.sections)
            {
                before += s.size;
                after += s.newSize;
            }
            pf::logInfo("world textures: region %s: %u sections %u -> %u KB, read from %s in %u ms", Files[i].name,
                        unsigned(region.sections.size()), unsigned(before / 1024), unsigned(after / 1024), CacheFile,
                        unsigned(loadMs));
            cacheable[i] = true;
            region.built.store(true, std::memory_order_release);
            region.ready->post();
            continue;
        }
        const uint32_t start = pf::ticksMs();
        x86::reg32 before = 0, after = 0;
        int skipped = 0;
        const char* error = buildRegion(i, &before, &after, &skipped);
        if (error)
        {
            pf::logError("world textures: region %s: %s; its sections load at full size", Files[i].name, error);
            region.sections.clear();
            region.segments.clear();
            region.headers.clear();
        }
        else
        {
            pf::logInfo("world textures: region %s: %u sections %u -> %u KB (%d left at full size), built in %u ms",
                        Files[i].name, unsigned(region.sections.size()), unsigned(before / 1024),
                        unsigned(after / 1024), skipped, unsigned(pf::ticksMs() - start));
            cacheable[i] = haveKey[i];
            built = built || haveKey[i];
        }
        region.built.store(true, std::memory_order_release);
        region.ready->post();
    }
    if (built)
        saveCache(keys, cacheable, entries);
    return 0;
}

void waitBuilt(Region& region)
{
    if (region.built.load(std::memory_order_acquire))
        return;
    region.ready->wait();
    region.ready->post();
}

// Reads of a table file: the size and compressed size of every rebuilt section become its new size.
template<int R>
void tableFilter(x86::reg32 offset, x86::reg8* data, x86::reg32 length)
{
    Region& region = s_regions[R];
    waitBuilt(region);
    for (const Section& s : region.sections)
    {
        x86::reg8 field[8];
        wr32(field, s.newSize);
        wr32(field + 4, s.newSize);
        for (x86::reg32 i = 0; i < 8; ++i)
        {
            const x86::reg32 at = s.recordPos + RecSize + i;
            if (at - offset < length)
                data[at - offset] = field[i];
        }
    }
}

// Bytes [from, from + length) of a rebuilt section into out. Stream file runs less than ReadGap apart are read
// with one read through a buffer (each read costs about as much as that many bytes), at most ReadSpan at a time.
const x86::reg32 ReadGap = 64 * 1024;
const x86::reg32 ReadSpan = 1024 * 1024;

bool assemble(const Region& region, const Section& section, int file, x86::reg32 from, x86::reg8* out,
              x86::reg32 length)
{
    memset(out, 0, length);
    const x86::reg32 to = from + length;
    const auto end = region.segments.begin() + section.endSegment;
    auto it = std::upper_bound(region.segments.begin() + section.firstSegment, end, from,
                               [](x86::reg32 value, const Segment& s) { return value < s.offset + s.size; });
    std::vector<x86::reg8> buffer;
    while (it != end && it->offset < to)
    {
        if (it->header >= 0)
        {
            const x86::reg32 a = std::max(from, it->offset);
            const x86::reg32 b = std::min(to, it->offset + it->size);
            memcpy(out + (a - from), region.headers[it->header].data() + (a - it->offset), b - a);
            ++it;
            continue;
        }
        // The stream file runs from it on that one read covers; header segments between them are copied after.
        const x86::reg32 first = it->source + (std::max(from, it->offset) - it->offset);
        x86::reg32 last = it->source + (std::min(to, it->offset + it->size) - it->offset);
        auto next = it + 1;
        auto stop = next;
        for (; next != end && next->offset < to; ++next)
        {
            if (next->header >= 0)
                continue;
            const x86::reg32 runEnd = next->source + (std::min(to, next->offset + next->size) - next->offset);
            if (next->source < last || next->source - last > ReadGap || runEnd - first > ReadSpan)
                break;
            last = runEnd;
            stop = next + 1;
        }
        if (stop == it + 1)
        {
            if (!readAt(file, first, out + (std::max(from, it->offset) - from), last - first))
                return false;
            ++it;
            continue;
        }
        buffer.resize(last - first);
        if (!readAt(file, first, buffer.data(), last - first))
            return false;
        for (; it != stop; ++it)
        {
            const x86::reg32 a = std::max(from, it->offset);
            const x86::reg32 b = std::min(to, it->offset + it->size);
            if (it->header >= 0)
                memcpy(out + (a - from), region.headers[it->header].data() + (a - it->offset), b - a);
            else
                memcpy(out + (a - from), &buffer[it->source + (a - it->offset) - first], b - a);
        }
    }
    return true;
}

// Reads of a stream file: rebuilt sections from their segments, the rest as it is.
template<int R>
x86::reg32 streamSource(int file, x86::reg32 offset, x86::reg8* data, x86::reg32 length)
{
    Region& region = s_regions[R];
    waitBuilt(region);
    const x86::reg64 end = x86::reg64(offset) + length;
    auto it = std::upper_bound(region.sections.begin(), region.sections.end(), offset,
                               [](x86::reg32 value, const Section& s) { return value < s.fileOffset + s.newSize; });
    x86::reg32 done = 0;
    while (done < length)
    {
        const x86::reg32 pos = offset + done;
        const x86::reg64 plainEnd = it == region.sections.end() ? end
                                    : std::min<x86::reg64>(end, std::max(pos, it->fileOffset));
        if (pos < plainEnd)
        {
            const x86::reg32 want = x86::reg32(plainEnd - pos);
            const x86::sreg32 n = readUpTo(file, pos, data + done, want);
            if (n < 0)
            {
                pf::logError("world textures: stream file read at %u (%u bytes) failed", unsigned(pos), unsigned(want));
                return x86::reg32(-1);
            }
            done += x86::reg32(n);
            if (x86::reg32(n) < want)
                break;
            continue;
        }
        const x86::reg32 to = x86::reg32(std::min<x86::reg64>(end, x86::reg64(it->fileOffset) + it->newSize));
        if (!assemble(region, *it, file, pos - it->fileOffset, data + done, to - pos))
        {
            pf::logError("world textures: stream file read for section at %u failed", unsigned(it->fileOffset));
            return x86::reg32(-1);
        }
        done += to - pos;
        ++it;
    }
    lseek(file, off_t(offset) + off_t(done), SEEK_SET);
    return done;
}

template<int R>
void installRegion()
{
    s_regions[R].ready = new pf::Semaphore("worldtex", 0);
    win32::File::setReadFilter(Files[R].tableSuffix, &tableFilter<R>);
    win32::File::setReadSource(Files[R].streamSuffix, &streamSource<R>);
}

bool contains(x86::reg32 record, x86::reg32 data)
{
    const x86::reg32 block = rc::ld32(record + RecMemory);
    return block && data - block < rc::ld32(record + RecLoadedSize);
}

// The rebuilt section of a loaded record (file offset and new size).
const Section* findSection(x86::reg32 record, const Region** region)
{
    const x86::reg32 offset = rc::ld32(record + RecFileOffset);
    const x86::reg32 size = rc::ld32(record + RecSize);
    for (const Region& r : s_regions)
    {
        if (!r.built.load(std::memory_order_acquire))
            continue;
        auto it = std::lower_bound(r.sections.begin(), r.sections.end(), offset,
                                   [](const Section& s, x86::reg32 value) { return s.fileOffset < value; });
        if (it != r.sections.end() && it->fileOffset == offset && it->newSize == size)
        {
            *region = &r;
            return &*it;
        }
    }
    return nullptr;
}

// Mip levels left out of the texture whose image data is at data: 0 unless it lies in a loaded rebuilt section.
int droppedLevels(x86::reg32 data)
{
    const x86::reg32 table = rc::ld32(TrackStreamer);
    if (table != s_cachedTable || !s_cachedRecord || !contains(s_cachedRecord, data))
    {
        s_cachedTable = table;
        s_cachedRecord = 0;
        s_cachedSection = nullptr;
        const x86::reg32 count = rc::ld32(TrackStreamer + 4);
        for (x86::reg32 i = 0; table && i < count && i < 0x10000; ++i)
        {
            const x86::reg32 record = table + i * RecordSize;
            if (contains(record, data))
            {
                s_cachedRecord = record;
                s_cachedSection = findSection(record, &s_cachedRegion);
                break;
            }
        }
    }
    if (!s_cachedSection)
        return 0;
    const x86::reg32 at = data - rc::ld32(s_cachedRecord + RecMemory);
    const auto first = s_cachedRegion->segments.begin() + s_cachedSection->firstSegment;
    const auto last = s_cachedRegion->segments.begin() + s_cachedSection->endSegment;
    auto it = std::lower_bound(first, last, at, [](const Segment& s, x86::reg32 value) { return s.offset < value; });
    return it != last && it->offset == at ? it->drop : 0;
}

}

void install()
{
    s_worldLevels = std::min(std::max(pf::Config::getInt("world_texture_drop", 2), 0), 3);
    s_sectionLevels = std::min(std::max(pf::Config::getInt("section_texture_drop", 2), 0), 3);
    pf::logInfo("world textures: top %d mip level(s) left out of section Y0, %d of the other track sections",
                s_worldLevels, s_sectionLevels);
    if (!s_worldLevels && !s_sectionLevels)
        return;
    installRegion<0>();
    installRegion<1>();
    s_installed = true;
    s_builder = new pf::Thread("worldtex", &build, nullptr, pf::Thread::PriorityLow, 256 * 1024,
                               pf::cores::mask(pf::cores::Io));
    s_builder->start();
}

CreationSkip::CreationSkip(x86::reg32 info)
    :   m_saved(0)
    ,   m_active(false)
{
    if (!s_installed)
        return;
    const x86::sreg32 skip = x86::sreg32(rc::ld32(MipSkip));
    const x86::reg32 data = rc::ld32(info + TexImageData);
    if (skip <= 0 || !data)
        return;
    const int levels = droppedLevels(data);
    if (!levels)
        return;
    m_saved = x86::reg32(skip);
    m_active = true;
    const x86::sreg32 lowered = std::max(skip - levels, 0);
    rc::st32(MipSkip, x86::reg32(lowered));
    if (!s_loggedSkip)
    {
        s_loggedSkip = true;
        pf::logInfo("world textures: mip skip %d lowered to %d for a texture without its top %d level(s)", int(skip),
                    int(lowered), levels);
    }
}

CreationSkip::~CreationSkip()
{
    if (m_active)
        rc::st32(MipSkip, m_saved);
}

}}
