#ifndef NFSC_VP6_H_
#define NFSC_VP6_H_

// Native frame decode of the game's VP6 movie decoder (guest 0x87f970 and its callees, MMX kernel set), bit-exact
// to the guest. All state stays in guest memory: the decoder instance (0x3b94 bytes) and the buffers it points to.
// Pure native code: no guest calls, no runtime services, so it can run outside the execution lock.

#include <cstdint>

namespace nfsc
{
namespace vp6
{

// Host address of guest address 0.
void setMemoryBase(uintptr_t base);

// Guest offsets of the decoder instance read by the glue.
enum : uint32_t
{
    InstanceSize      = 0x3b94,
    InstWidth         = 0x14c,
    InstHeight        = 0x150,
    InstCpuMhz        = 0x164,
    InstFrameSize     = 0x17c,  // luma plane bytes with borders
    InstChromaSize    = 0x180,  // one chroma plane
    InstFrames        = 0x1b8,  // new, golden, previous, spare frame pointers
    InstDecodeTime    = 0x379c, // last decode time, then its running average
};

// Frame header, part 1 (0x882c9b, then 0x882d57 up to the size check). Returns true when the frame changes the size:
// the caller then runs the guest's 0x881996 (frees and allocates the buffers) before headerEnd.
bool headerBegin(uint32_t instance, uint32_t data, uint32_t& quantiser);
// Frame header, the rest of 0x882d57.
void headerEnd(uint32_t instance, uint32_t quantiser);
// Everything after the header (0x87f85f): partitions, macroblocks, frame rotation and border extension.
// argumentSlot is the guest stack address 0x87f85f passes to 0x882a93 (stored in the global 0xbbbeb0).
void decodeBody(uint32_t instance, uint32_t data, uint32_t argumentSlot);

// decodeBody in stages, for reconstruction on other threads. beginBody returns the macroblock rows; independent is
// false when the rows must be reconstructed in order. decodeRow(r) runs in order on one thread and records row r in
// a ring of RingRows rows: row r - RingRows must be reconstructed before. reconstructRow(r) may run on any thread,
// in any order (if independent), once decodeRow(r) returned; it runs no guest code and writes only the new frame
// (its pixels and, if independent, the borders of its rows). endBody follows the last reconstruction.
constexpr uint32_t RingRows = 16;
uint32_t beginBody(uint32_t instance, uint32_t data, uint32_t argumentSlot, bool& independent);
void decodeRow(uint32_t row);
void reconstructRow(uint32_t row);
void endBody();

// The game's YUV to RGB conversion 0x57dff0 for format 1 (32-bit pixels, the movie texture). args are its nine
// cdecl arguments: Y, U, V, Y stride, width, height, destination, pitch, format. Returns the guest's result;
// rowsWritten tells whether the guest leaves its row count (the height) in the last argument slot.
uint32_t convertFrame(const uint32_t args[9], bool& rowsWritten);
// convertFrame in two parts: prepareConversion checks the arguments, sets up the tables and returns the guest's
// result; when rows is set, convertRows(rows) writes the pixels (and the guest leaves the height in the last slot).
struct Rows
{
    uint32_t y, u, v, yStride;      // planes, as the guest reads them
    uint32_t destination, pitch;    // the first pixel written (the destination, or 4 bytes on when not 8-aligned)
    uint32_t width, height;         // pixels per row (even), rows
};
uint32_t prepareConversion(const uint32_t args[9], Rows& rows, bool& convert);
void convertRows(const Rows& rows);
// Guest memory 0x57dff0 can change besides the destination: its three tables and their channel order flag.
enum : uint32_t
{
    ConvertTables     = 0xa5ec80,
    ConvertTablesSize = 0x1800,
    ConvertOrderFlag  = 0xa97bb0,
};

}
}

#endif /* !NFSC_VP6_H_ */
