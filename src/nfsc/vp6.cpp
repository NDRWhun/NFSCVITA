// Native frame decode of the game's VP6 movie decoder (guest 0x87f85f and its callees). Each function names the
// guest function it replaces; the kernels follow the guest's MMX kernel set (0x883000), instruction for instruction
// where saturation or rounding could differ.

#include "vp6.h"

#include <arm_neon.h>
#include <algorithm>
#include <cstring>
#include <initializer_list>
#include <vector>

namespace nfsc
{
namespace vp6
{

namespace
{

uintptr_t s_base = 0;

inline uint8_t* P(uint32_t a)               { return reinterpret_cast<uint8_t*>(s_base + a); }
inline uint8_t L8(uint32_t a)               { return *P(a); }
inline int8_t LS8(uint32_t a)               { return int8_t(*P(a)); }
inline uint16_t L16(uint32_t a)             { uint16_t v; memcpy(&v, P(a), 2); return v; }
inline int16_t LS16(uint32_t a)             { int16_t v; memcpy(&v, P(a), 2); return v; }
inline uint32_t L32(uint32_t a)             { uint32_t v; memcpy(&v, P(a), 4); return v; }
inline int32_t LS32(uint32_t a)             { int32_t v; memcpy(&v, P(a), 4); return v; }
inline void S8(uint32_t a, uint32_t v)      { *P(a) = uint8_t(v); }
inline void S16(uint32_t a, uint32_t v)     { const uint16_t x = uint16_t(v); memcpy(P(a), &x, 2); }
inline void S32(uint32_t a, uint32_t v)     { memcpy(P(a), &v, 4); }
inline uint32_t BE32(uint32_t a)
{
    const uint8_t* p = P(a);
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

// Guest tables (.rdata, and the IDCT constants 0xbbbeb8 that 0x882efa fills at start-up).
enum : uint32_t
{
    TabBlockMode     = 0x9ff8b4,    // mode -> prediction needs motion compensation (dword)
    TabHuffToken     = 0x9ff918,    // Huffman token -> base value (dword)
    TabHuffBand      = 0x9ff948,    // coefficient index -> Huffman band (dword)
    TabDefBands1     = 0x9ffa50,    // default bands (dword-sized bytes, 64)
    TabDefBands0     = 0x9ffa90,
    TabRefFrame      = 0x9ffad0,    // mode -> reference frame: 0 none, 1 previous, 2 golden (dword)
    TabBitMask       = 0x9ffb10,    // (1 << n) - 1 (dword)
    TabQuantAc       = 0x9ffb98,    // luma AC quantiser (dword)
    TabQuantAcUv     = 0x9ffc98,
    TabQuantDc       = 0x9ffd98,    // luma DC quantiser (word)
    TabQuantDcUv     = 0x9ffe18,
    TabBlockPlane    = 0xa00098,    // block -> quantiser table (byte)
    TabNeighbours    = 0xa000a8,    // 12 (row, column) pairs
    TabTokenBase     = 0xa00108,    // DCT category: base (word), extra bits - 1 (word), bit probabilities
    TabCoefBand      = 0xa001c8,    // coefficient index -> band (dword)
    TabDcUpdate      = 0xa00718,
    TabBandUpdate    = 0xa00730,
    TabRunUpdate     = 0xa00770,
    TabRunDefault    = 0xa0078c,
    TabAcUpdate      = 0xa007a8,
    TabDcContext     = 0xa0093c,
    TabMvUpdate      = 0xa009b1,
    TabMvDefault     = 0xa009d4,
    TabModeVectors   = 0xa00a00,
    TabModeDefault   = 0xa00dc0,
    TabScanMmx       = 0xa7bda8,    // MMX coefficient order (dword)
    TabScanNatural   = 0xa7bca8,
    TabRound64       = 0xa7bfc0,
    TabBilinear      = 0xa7bfd0,    // 8 x 32 bytes: two taps, four words each
    TabSignFlip      = 0xa7c2e0,
    TabIdct          = 0xbbbeb8,
    GlobalArgSlot    = 0xbbbeb0,
};

// Instance fields (guest offsets).
enum : uint32_t
{
    OffAbove = 0x00, OffLeft = 0x04, OffDcPred = 0x08, OffCoefs = 0x10, OffMode = 0x14, OffBlockModes = 0x18,
    OffBlockMvs = 0x30, OffNearest = 0x48, OffNear = 0x4c, OffNearestIndex = 0x50, OffGoldNearest = 0x54,
    OffGoldNear = 0x58, OffGoldNearestIndex = 0x5c, OffBlockRow = 0x60, OffBlockCol = 0x64, OffBlockOffset = 0x68,
    OffBlockStride = 0x6c, OffPlane = 0x70, OffMvShift = 0x74, OffMvMask = 0x78, OffMcStride = 0x7c,
    OffFieldMb = 0x80, OffLeftCtx = 0x84, OffAboveY = 0xc4, OffAboveU = 0xc8, OffAboveV = 0xcc, OffDcY = 0xdc,
    OffDcU = 0xe4, OffDcV = 0xec, OffQuant = 0xf4, OffPartition = 0xf8, OffRaw = 0xfc, OffBoolA = 0x10c,
    OffBoolB = 0x124, OffHuff = 0x13c, OffBorder = 0x148, OffYStride = 0x154, OffUvStride = 0x158,
    OffFieldFlag = 0x15c, OffBlocksX = 0x178, OffYOrigin = 0x190, OffUOrigin = 0x194, OffVOrigin = 0x198,
    OffLastMode = 0x19c, OffMbCount = 0x1a0, OffMbRows = 0x1a4, OffMbCols = 0x1a8, OffNewFrame = 0x1b8,
    OffGolden = 0x1bc, OffPrevious = 0x1c0, OffSpare = 0x1c4, OffResidual = 0x1c8, OffPrediction = 0x1d8,
    OffScan = 0x1e4, OffBandOrder = 0x224, OffBands = 0x2a4, OffNeighbourOffsets = 0x2e4, OffFilterFlag = 0x324,
    OffGoldenRefresh = 0x328, OffFieldProb = 0x32c, OffRunProbs = 0x330, OffModeMap = 0x34c, OffMvMap = 0x354,
    OffDcTrees = 0x364, OffDcWeights = 0x484, OffAcTrees = 0x4fc, OffAcWeights = 0x193c, OffRunTrees = 0x21ac,
    OffRunWeights = 0x22fc, OffHuffman = 0x2388, OffDcLookup = 0x238c, OffAcLookup = 0x248c, OffRunLookup = 0x368c,
    OffZeroRuns = 0x378c, OffMvSignProbs = 0x37d0, OffMvZeroProbs = 0x37d2, OffMvShortProbs = 0x37d4,
    OffMvLongProbs = 0x37e2, OffModeProbs = 0x37f2, OffSameModeProbs = 0x3842, OffModeTrees = 0x386a,
    OffDcProbs = 0x39d2, OffAcProbs = 0x39e8, OffDcCtxProbs = 0x3b74, OffInterFrame = 0x3b92,
};

// ---------------------------------------------------------------------------------------------------------------
// Kernels (MMX set of 0x883000).

inline int16x4_t mulhi(int16x4_t a, int16x4_t b)       { return vshrn_n_s32(vmull_s16(a, b), 16); }
inline int16x4_t unpcklwd(int16x4_t a, int16x4_t b)    { return vzip_s16(a, b).val[0]; }
inline int16x4_t unpckhwd(int16x4_t a, int16x4_t b)    { return vzip_s16(a, b).val[1]; }
inline int16x4_t unpckldq(int16x4_t a, int16x4_t b)
{
    return vreinterpret_s16_s32(vzip_s32(vreinterpret_s32_s16(a), vreinterpret_s32_s16(b)).val[0]);
}
inline int16x4_t unpckhdq(int16x4_t a, int16x4_t b)
{
    return vreinterpret_s16_s32(vzip_s32(vreinterpret_s32_s16(a), vreinterpret_s32_s16(b)).val[1]);
}

// One 8-point pass of 0x884a1d over B[0..7], transposing the result in 4 x 4 blocks.
inline void rowPass64(int16x4_t* B, const int16x4_t* K)
{
    int16x4_t m0, m1, m2, m3, m4, m5, m6, m7;
    m2 = B[6]; m6 = K[6]; m4 = m2; m7 = B[3]; m4 = mulhi(m4, m6); m1 = K[8]; m6 = mulhi(m6, m7); m5 = m1;
    m1 = mulhi(m1, m2); m3 = B[2]; m5 = mulhi(m5, m7); m0 = K[4]; m4 = vadd_s16(m4, m2); m6 = vadd_s16(m6, m7);
    m2 = vadd_s16(m2, m1); m1 = B[7]; m7 = vadd_s16(m7, m5); m5 = m0; m0 = mulhi(m0, m3); m4 = vqadd_s16(m4, m7);
    m5 = mulhi(m5, m1); m7 = K[10]; m6 = vqsub_s16(m6, m2); m0 = vadd_s16(m0, m3); m3 = mulhi(m3, m7); m2 = B[4];
    m7 = mulhi(m7, m1); m5 = vadd_s16(m5, m1); m1 = m2; m2 = mulhi(m2, K[5]); m3 = vqsub_s16(m3, m5); m5 = B[5];
    m0 = vqadd_s16(m0, m7); m7 = m5; m0 = vqsub_s16(m0, m4); m5 = mulhi(m5, K[5]); m2 = vadd_s16(m2, m1);
    m1 = mulhi(m1, K[9]); m4 = vqadd_s16(m4, m4); m4 = vqadd_s16(m4, m0); m3 = vqsub_s16(m3, m6);
    m5 = vadd_s16(m5, m7); m6 = vqadd_s16(m6, m6); m7 = mulhi(m7, K[9]); m6 = vqadd_s16(m6, m3); B[2] = m4;
    m1 = vqsub_s16(m1, m5); m4 = K[7]; m5 = m3; m3 = mulhi(m3, m4); m7 = vqadd_s16(m7, m2); B[4] = m6; m2 = m0;
    m6 = B[0]; m0 = mulhi(m0, m4); m5 = vadd_s16(m5, m3); m3 = B[1]; m5 = vqsub_s16(m5, m1); m2 = vadd_s16(m2, m0);
    m6 = vqsub_s16(m6, m3); m0 = m6; m6 = mulhi(m6, m4); m3 = vqadd_s16(m3, m3); m1 = vqadd_s16(m1, m1);
    m3 = vqadd_s16(m3, m0); m1 = vqadd_s16(m1, m5); m4 = mulhi(m4, m3); m6 = vqadd_s16(m6, m0); m6 = vqsub_s16(m6, m2);
    m2 = vqadd_s16(m2, m2); m0 = B[2]; m2 = vqadd_s16(m2, m6); m4 = vadd_s16(m4, m3); m2 = vqsub_s16(m2, m1);
    m3 = B[4]; m4 = vqsub_s16(m4, m7); m1 = vqadd_s16(m1, m1); m7 = vqadd_s16(m7, m7); m1 = vqadd_s16(m1, m2);
    m7 = vqadd_s16(m7, m4); m4 = vqsub_s16(m4, m3); m3 = vqadd_s16(m3, m3); m6 = vqsub_s16(m6, m5);
    m5 = vqadd_s16(m5, m5); m3 = vqadd_s16(m3, m4); m5 = vqadd_s16(m5, m6); m7 = vqsub_s16(m7, m0);
    m0 = vqadd_s16(m0, m0); B[2] = m1; m0 = vqadd_s16(m0, m7); m1 = m4; m4 = unpcklwd(m4, m5); B[0] = m0;
    m1 = unpckhwd(m1, m5); m0 = m6; m6 = unpcklwd(m6, m7); m5 = m4; m4 = unpckldq(m4, m6); m5 = unpckhdq(m5, m6);
    m6 = m1; B[1] = m4; m0 = unpckhwd(m0, m7); B[3] = m5; m6 = unpckhdq(m6, m0); m4 = B[0]; m1 = unpckldq(m1, m0);
    m5 = B[2]; m0 = m4; B[7] = m6; m0 = unpcklwd(m0, m5); B[5] = m1; m4 = unpckhwd(m4, m5); m5 = m2;
    m2 = unpcklwd(m2, m3); m1 = m0; m0 = unpckldq(m0, m2); m1 = unpckhdq(m1, m2); m2 = m4; B[0] = m0;
    m5 = unpckhwd(m5, m3); B[2] = m1; m4 = unpckhdq(m4, m5); m2 = unpckldq(m2, m5); B[6] = m4; B[4] = m2;
}

// The second pass of 0x884a1d over B[0], B[2] .. B[14], with the final rounding.
inline void colPass64(int16x4_t* B, const int16x4_t* K)
{
    int16x4_t m0, m1, m2, m3, m4, m5, m6, m7;
    m2 = B[6]; m6 = K[6]; m4 = m2; m7 = B[10]; m4 = mulhi(m4, m6); m1 = K[8]; m6 = mulhi(m6, m7); m5 = m1;
    m1 = mulhi(m1, m2); m3 = B[2]; m5 = mulhi(m5, m7); m0 = K[4]; m4 = vadd_s16(m4, m2); m6 = vadd_s16(m6, m7);
    m2 = vadd_s16(m2, m1); m1 = B[14]; m7 = vadd_s16(m7, m5); m5 = m0; m0 = mulhi(m0, m3); m4 = vqadd_s16(m4, m7);
    m5 = mulhi(m5, m1); m7 = K[10]; m6 = vqsub_s16(m6, m2); m0 = vadd_s16(m0, m3); m3 = mulhi(m3, m7); m2 = B[4];
    m7 = mulhi(m7, m1); m5 = vadd_s16(m5, m1); m1 = m2; m2 = mulhi(m2, K[5]); m3 = vqsub_s16(m3, m5); m5 = B[12];
    m0 = vqadd_s16(m0, m7); m7 = m5; m0 = vqsub_s16(m0, m4); m5 = mulhi(m5, K[5]); m2 = vadd_s16(m2, m1);
    m1 = mulhi(m1, K[9]); m4 = vqadd_s16(m4, m4); m4 = vqadd_s16(m4, m0); m3 = vqsub_s16(m3, m6);
    m5 = vadd_s16(m5, m7); m6 = vqadd_s16(m6, m6); m7 = mulhi(m7, K[9]); m6 = vqadd_s16(m6, m3); B[2] = m4;
    m1 = vqsub_s16(m1, m5); m4 = K[7]; m5 = m3; m3 = mulhi(m3, m4); m7 = vqadd_s16(m7, m2); B[4] = m6; m2 = m0;
    m6 = B[0]; m0 = mulhi(m0, m4); m5 = vadd_s16(m5, m3); m3 = B[8]; m5 = vqsub_s16(m5, m1); m2 = vadd_s16(m2, m0);
    m6 = vqsub_s16(m6, m3); m0 = m6; m6 = mulhi(m6, m4); m3 = vqadd_s16(m3, m3); m1 = vqadd_s16(m1, m1);
    m3 = vqadd_s16(m3, m0); m1 = vqadd_s16(m1, m5); m4 = mulhi(m4, m3); m6 = vqadd_s16(m6, m0); m6 = vqsub_s16(m6, m2);
    m2 = vqadd_s16(m2, m2); m0 = B[2]; m2 = vqadd_s16(m2, m6); m4 = vadd_s16(m4, m3); m2 = vqsub_s16(m2, m1);
    m2 = vqadd_s16(m2, K[11]); m1 = vqadd_s16(m1, m1); m1 = vqadd_s16(m1, m2); m2 = vshr_n_s16(m2, 4);
    m4 = vqsub_s16(m4, m7); m1 = vshr_n_s16(m1, 4); m3 = B[4]; m7 = vqadd_s16(m7, m7); B[4] = m2;
    m7 = vqadd_s16(m7, m4); B[2] = m1; m4 = vqsub_s16(m4, m3); m4 = vqadd_s16(m4, K[11]); m3 = vqadd_s16(m3, m3);
    m3 = vqadd_s16(m3, m4); m4 = vshr_n_s16(m4, 4); m6 = vqsub_s16(m6, m5); m3 = vshr_n_s16(m3, 4);
    m6 = vqadd_s16(m6, K[11]); m5 = vqadd_s16(m5, m5); m5 = vqadd_s16(m5, m6); m6 = vshr_n_s16(m6, 4); B[8] = m4;
    m5 = vshr_n_s16(m5, 4); B[6] = m3; m7 = vqsub_s16(m7, m0); m7 = vqadd_s16(m7, K[11]); m0 = vqadd_s16(m0, m0);
    m0 = vqadd_s16(m0, m7); m7 = vshr_n_s16(m7, 4); B[12] = m6; m0 = vshr_n_s16(m0, 4); B[10] = m5; B[14] = m7;
    B[0] = m0;
}

// The first pass of 0x88510e (inputs B[1], B[3], B[5], B[7] are zero), transposing like rowPass64.
inline void rowPass10(int16x4_t* B, const int16x4_t* K)
{
    int16x4_t m0, m1, m2, m3, m4, m5, m6, m7;
    m2 = B[6]; m6 = K[6]; m4 = m2; m1 = K[8]; m4 = mulhi(m4, m6); m3 = B[2]; m1 = mulhi(m1, m2); m0 = K[4];
    m4 = vadd_s16(m4, m2); m6 = vdup_n_s16(0); m2 = vadd_s16(m2, m1); m5 = B[4]; m0 = mulhi(m0, m3); m1 = m5;
    m0 = vadd_s16(m0, m3); m3 = mulhi(m3, K[10]); m6 = vqsub_s16(m6, m2); m5 = mulhi(m5, K[5]); m0 = vqsub_s16(m0, m4);
    m7 = B[4]; m4 = vqadd_s16(m4, m4); m7 = vadd_s16(m7, m5); m4 = vqadd_s16(m4, m0); m1 = mulhi(m1, K[9]);
    m3 = vqsub_s16(m3, m6); B[2] = m4; m6 = vqadd_s16(m6, m6); m4 = K[7]; m6 = vqadd_s16(m6, m3); m5 = m3;
    m3 = mulhi(m3, m4); B[4] = m6; m2 = m0; m6 = B[0]; m0 = mulhi(m0, m4); m5 = vadd_s16(m5, m3);
    m2 = vadd_s16(m2, m0); m5 = vqsub_s16(m5, m1); m6 = mulhi(m6, m4); m6 = vadd_s16(m6, B[0]); m1 = vqadd_s16(m1, m1);
    m4 = m6; m1 = vqadd_s16(m1, m5); m6 = vqsub_s16(m6, m2); m2 = vqadd_s16(m2, m2); m0 = B[2]; m2 = vqadd_s16(m2, m6);
    m2 = vqsub_s16(m2, m1); m3 = B[4]; m4 = vqsub_s16(m4, m7); m1 = vqadd_s16(m1, m1); m7 = vqadd_s16(m7, m7);
    m1 = vqadd_s16(m1, m2); m7 = vqadd_s16(m7, m4); m4 = vqsub_s16(m4, m3); m3 = vqadd_s16(m3, m3);
    m6 = vqsub_s16(m6, m5); m5 = vqadd_s16(m5, m5); m3 = vqadd_s16(m3, m4); m5 = vqadd_s16(m5, m6);
    m7 = vqsub_s16(m7, m0); m0 = vqadd_s16(m0, m0); B[2] = m1; m0 = vqadd_s16(m0, m7); m1 = m4; m4 = unpcklwd(m4, m5);
    B[0] = m0; m1 = unpckhwd(m1, m5); m0 = m6; m6 = unpcklwd(m6, m7); m5 = m4; m4 = unpckldq(m4, m6);
    m5 = unpckhdq(m5, m6); m6 = m1; B[1] = m4; m0 = unpckhwd(m0, m7); B[3] = m5; m6 = unpckhdq(m6, m0); m4 = B[0];
    m1 = unpckldq(m1, m0); m5 = B[2]; m0 = m4; B[7] = m6; m0 = unpcklwd(m0, m5); B[5] = m1; m4 = unpckhwd(m4, m5);
    m5 = m2; m2 = unpcklwd(m2, m3); m1 = m0; m0 = unpckldq(m0, m2); m1 = unpckhdq(m1, m2); m2 = m4; B[0] = m0;
    m5 = unpckhwd(m5, m3); B[2] = m1; m4 = unpckhdq(m4, m5); m2 = unpckldq(m2, m5); B[6] = m4; B[4] = m2;
}

// The second pass of 0x88510e over B[0], B[2], B[4], B[6], with the final rounding.
inline void colPass10(int16x4_t* B, const int16x4_t* K)
{
    int16x4_t m0, m1, m2, m3, m4, m5, m6, m7;
    m2 = B[6]; m6 = K[6]; m4 = m2; m1 = K[8]; m4 = mulhi(m4, m6); m3 = B[2]; m1 = mulhi(m1, m2); m0 = K[4];
    m4 = vadd_s16(m4, m2); m6 = vdup_n_s16(0); m2 = vadd_s16(m2, m1); m5 = B[4]; m0 = mulhi(m0, m3); m1 = m5;
    m0 = vadd_s16(m0, m3); m3 = mulhi(m3, K[10]); m6 = vqsub_s16(m6, m2); m5 = mulhi(m5, K[5]); m0 = vqsub_s16(m0, m4);
    m7 = B[4]; m4 = vqadd_s16(m4, m4); m7 = vadd_s16(m7, m5); m4 = vqadd_s16(m4, m0); m1 = mulhi(m1, K[9]);
    m3 = vqsub_s16(m3, m6); B[2] = m4; m6 = vqadd_s16(m6, m6); m4 = K[7]; m6 = vqadd_s16(m6, m3); m5 = m3;
    m3 = mulhi(m3, m4); B[4] = m6; m2 = m0; m6 = B[0]; m0 = mulhi(m0, m4); m5 = vadd_s16(m5, m3);
    m2 = vadd_s16(m2, m0); m5 = vqsub_s16(m5, m1); m6 = mulhi(m6, m4); m6 = vadd_s16(m6, B[0]); m1 = vqadd_s16(m1, m1);
    m4 = m6; m1 = vqadd_s16(m1, m5); m6 = vqsub_s16(m6, m2); m2 = vqadd_s16(m2, m2); m0 = B[2]; m2 = vqadd_s16(m2, m6);
    m2 = vqsub_s16(m2, m1); m2 = vqadd_s16(m2, K[11]); m1 = vqadd_s16(m1, m1); m1 = vqadd_s16(m1, m2);
    m2 = vshr_n_s16(m2, 4); m4 = vqsub_s16(m4, m7); m1 = vshr_n_s16(m1, 4); m3 = B[4]; m7 = vqadd_s16(m7, m7);
    B[4] = m2; m7 = vqadd_s16(m7, m4); B[2] = m1; m4 = vqsub_s16(m4, m3); m4 = vqadd_s16(m4, K[11]);
    m3 = vqadd_s16(m3, m3); m3 = vqadd_s16(m3, m4); m4 = vshr_n_s16(m4, 4); m6 = vqsub_s16(m6, m5);
    m3 = vshr_n_s16(m3, 4); m6 = vqadd_s16(m6, K[11]); m5 = vqadd_s16(m5, m5); m5 = vqadd_s16(m5, m6);
    m6 = vshr_n_s16(m6, 4); B[8] = m4; m5 = vshr_n_s16(m5, 4); B[6] = m3; m7 = vqsub_s16(m7, m0);
    m7 = vqadd_s16(m7, K[11]); m0 = vqadd_s16(m0, m0); m0 = vqadd_s16(m0, m7); m7 = vshr_n_s16(m7, 4); B[12] = m6;
    m0 = vshr_n_s16(m0, 4); B[10] = m5; B[14] = m7; B[0] = m0;
}

// 0x8849c8: DC-only inverse transform.
void idctDc(const int16_t* c, const int16_t* q, int16_t* o)
{
    int16x8_t v = vdupq_n_s16(0);
    if (c[0] != 0)
        v = vdupq_n_s16(int16_t((int32_t(q[0]) * c[0] + 15) >> 5));
    for (int i = 0; i < 64; i += 8)
        vst1q_s16(o + i, v);
}

// 0x884a1d: dequantisation and full inverse transform.
void idct64(const int16_t* c, const int16_t* q, int16_t* o)
{
    const int16_t* k = reinterpret_cast<const int16_t*>(P(TabIdct));
    int16x4_t B[16], K[12];
    for (int i = 4; i < 12; ++i)
        K[i] = vld1_s16(k + 4 * i);
    for (int i = 0; i < 16; ++i)
        B[i] = vmul_s16(vld1_s16(c + 4 * i), vld1_s16(q + 4 * i));
    rowPass64(B, K);
    rowPass64(B + 8, K);
    colPass64(B, K);
    colPass64(B + 1, K);
    for (int i = 0; i < 16; ++i)
        vst1_s16(o + 4 * i, B[i]);
}

// 0x88510e: dequantisation and inverse transform of the top-left 4 x 4 coefficients.
void idct10(const int16_t* c, const int16_t* q, int16_t* o)
{
    const int16_t* k = reinterpret_cast<const int16_t*>(P(TabIdct));
    int16x4_t B[16], K[12];
    for (int i = 4; i < 12; ++i)
        K[i] = vld1_s16(k + 4 * i);
    for (int i = 0; i < 16; ++i)
        B[i] = vdup_n_s16(0);
    for (int i = 0; i < 8; i += 2)
        B[i] = vmul_s16(vld1_s16(c + 4 * i), vld1_s16(q + 4 * i));
    rowPass10(B, K);
    colPass10(B, K);
    colPass10(B + 1, K);
    for (int i = 0; i < 16; ++i)
        vst1_s16(o + 4 * i, B[i]);
}

// 0x886e56: intra block, residual + 128 saturated to bytes.
void putIntra(uint8_t* dst, const int16_t* r, int32_t stride)
{
    const int8x8_t flip = vld1_s8(reinterpret_cast<const int8_t*>(P(TabSignFlip)));
    for (int y = 0; y < 8; ++y)
    {
        const int8x8_t v = veor_s8(vqmovn_s16(vld1q_s16(r + 8 * y)), flip);
        vst1_s8(reinterpret_cast<int8_t*>(dst + y * stride), v);
    }
}

// 0x886e8d: reference bytes plus residual, saturated.
void addInter(uint8_t* dst, const uint8_t* ref, const int16_t* r, int32_t stride)
{
    for (int y = 0; y < 8; ++y)
    {
        const int16x8_t p = vreinterpretq_s16_u16(vmovl_u8(vld1_u8(ref + y * stride)));
        vst1_u8(dst + y * stride, vqmovun_s16(vqaddq_s16(p, vld1q_s16(r + 8 * y))));
    }
}

// 0x88770b: 8 x 8 bytes to words.
void copyBlock(const uint8_t* src, int32_t stride, int16_t* d)
{
    for (int y = 0; y < 8; ++y)
        vst1q_s16(d + 8 * y, vreinterpretq_s16_u16(vmovl_u8(vld1_u8(src + y * stride))));
}

// Two-tap filter of 8 bytes with taps t0, t1 (four equal words each), as 0x8869f4 computes it per half.
inline int16x8_t filter8(uint8x8_t a, uint8x8_t b, int16x8_t t0, int16x8_t t1, int16x8_t round)
{
    const int16x8_t x = vmulq_s16(vreinterpretq_s16_u16(vmovl_u8(a)), t0);
    const int16x8_t y = vmulq_s16(vreinterpretq_s16_u16(vmovl_u8(b)), t1);
    return vshrq_n_s16(vaddq_s16(vaddq_s16(x, y), round), 7);
}

inline int16x8_t taps(uint32_t table)
{
    return vcombine_s16(vld1_s16(reinterpret_cast<const int16_t*>(P(table))),
                        vld1_s16(reinterpret_cast<const int16_t*>(P(table))));
}

// 0x886beb with the bilinear filters (its only caller passes 0 for the four-tap flag): the prediction from a and
// b = a + offset (offset +-1, +-stride or +-(stride +- 1)) with filter phases fx, fy, as 8 x 8 bytes in tmp.
// Returns false (tmp unwritten) when a and b are the same.
bool predict(const uint8_t* a, const uint8_t* b, uint8_t* tmp, int32_t stride, uint32_t fx, uint32_t fy)
{
    int32_t diff = int32_t(b - a);
    const uint8_t* src = a;
    if (diff < 0)
    {
        src = b;
        diff = -diff;
    }
    if (diff == 0)
        return false;
    const int16x8_t round = taps(TabRound64);
    if (diff == 1 || diff == stride)
    {
        // 0x8869f4 (horizontal) and 0x886a7a (vertical).
        const uint32_t t = TabBilinear + 32 * (diff == 1 ? fx : fy);
        const int16x8_t t0 = taps(t), t1 = taps(t + 16);
        const int32_t step = diff == 1 ? 1 : stride;
        for (int y = 0; y < 8; ++y)
        {
            const uint8_t* s = src + y * stride;
            vst1_u8(tmp + 8 * y, vqmovun_s16(filter8(vld1_u8(s), vld1_u8(s + step), t0, t1, round)));
        }
    }
    else if (diff == stride - 1 || diff == stride + 1)
    {
        // 0x886d58: horizontal pass, then vertical between the clamped previous row and the unclamped current row.
        if (diff == stride - 1)
            --src;
        const uint32_t tx = TabBilinear + 32 * fx, ty = TabBilinear + 32 * fy;
        const int16x8_t h0 = taps(tx), h1 = taps(tx + 16), v0 = taps(ty), v1 = taps(ty + 16);
        const uint8_t* s = src;
        uint8x8_t prev = vqmovun_s16(filter8(vld1_u8(s), vld1_u8(s + 1), h0, h1, round));
        for (int y = 0; y < 8; ++y)
        {
            s += stride;
            const int16x8_t cur = filter8(vld1_u8(s), vld1_u8(s + 1), h0, h1, round);
            const int16x8_t up = vmulq_s16(vreinterpretq_s16_u16(vmovl_u8(prev)), v0);
            prev = vqmovun_s16(cur);
            const int16x8_t out = vshrq_n_s16(vaddq_s16(vaddq_s16(vmulq_s16(cur, v1), up), round), 7);
            vst1_u8(tmp + 8 * y, vqmovun_s16(out));
        }
    }
    return true;
}

// Reconstruction with a residual that is the same in all 64 positions (r), as addInter, addPredictionBytes (p: the
// prediction bytes, stride 8) and putIntra compute it.
void addInterConstant(uint8_t* dst, const uint8_t* ref, int16_t r, int32_t stride)
{
    const int16x8_t v = vdupq_n_s16(r);
    for (int y = 0; y < 8; ++y)
    {
        const int16x8_t p = vreinterpretq_s16_u16(vmovl_u8(vld1_u8(ref + y * stride)));
        vst1_u8(dst + y * stride, vqmovun_s16(vqaddq_s16(p, v)));
    }
}

void addPredictionConstant(const uint8_t* p, int16_t r, uint8_t* dst, int32_t stride)
{
    const int16x8_t v = vdupq_n_s16(r);
    for (int y = 0; y < 8; ++y)
    {
        const int16x8_t w = vreinterpretq_s16_u16(vmovl_u8(vld1_u8(p + 8 * y)));
        vst1_u8(dst + y * stride, vqmovun_s16(vaddq_s16(w, v)));
    }
}

void putIntraConstant(uint8_t* dst, int16_t r, int32_t stride)
{
    const int8x8_t flip = vld1_s8(reinterpret_cast<const int8_t*>(P(TabSignFlip)));
    const int8x8_t v = veor_s8(vqmovn_s16(vdupq_n_s16(r)), flip);
    for (int y = 0; y < 8; ++y)
        vst1_s8(reinterpret_cast<int8_t*>(dst + y * stride), v);
}

// 0x887782 on byte input: prediction bytes (stride 8) plus a residual (wrapping), saturated.
void addPredictionBytes(const uint8_t* p, const int16_t* r, uint8_t* dst, int32_t stride)
{
    for (int y = 0; y < 8; ++y)
    {
        const int16x8_t w = vreinterpretq_s16_u16(vmovl_u8(vld1_u8(p + 8 * y)));
        vst1_u8(dst + y * stride, vqmovun_s16(vaddq_s16(w, vld1q_s16(r + 8 * y))));
    }
}

// 8 x 8 bytes from src (stride srcStride) to dst (stride dstStride), row by row.
void copyBytes(uint8_t* dst, int32_t dstStride, const uint8_t* src, int32_t srcStride)
{
    for (int y = 0; y < 8; ++y)
        vst1_u8(dst + y * dstStride, vld1_u8(src + y * srcStride));
}

// ---------------------------------------------------------------------------------------------------------------
// Bitstreams.

// Boolean decoder (instance +0x10c and +0x124): +4 range, +8 value, +0xc bits to the next byte, +0x10 position,
// +0x14 buffer.
struct BoolDecoder
{
    uint32_t range;
    uint32_t value;
    int32_t count;
    uint32_t pos;
    uint32_t buffer;

    void load(uint32_t a)   { range = L32(a + 4); value = L32(a + 8); count = LS32(a + 0xc); pos = L32(a + 0x10);
                              buffer = L32(a + 0x14); }
    void store(uint32_t a)  { S32(a + 4, range); S32(a + 8, value); S32(a + 0xc, uint32_t(count));
                              S32(a + 0x10, pos); S32(a + 0x14, buffer); }

    // 0x87fd04 / 0x87fb1d.
    uint32_t bit(uint32_t prob)
    {
        const uint32_t split = (((range - 1) * prob) >> 8) + 1;
        const uint32_t big = split << 24;
        uint32_t result;
        if (value >= big)
        {
            result = 1;
            range -= split;
            value -= big;
        }
        else
        {
            result = 0;
            range = split;
        }
        // The guest's one-bit normalisation loop in one shift: at most one byte enters within 7 shifts.
        if (range < 0x80)
        {
            const int32_t shift = __builtin_clz(range) - 24;
            range <<= shift;
            value <<= shift;
            count -= shift;
            if (count <= 0)
            {
                value |= uint32_t(L8(buffer + pos)) << -count;
                count += 8;
                ++pos;
            }
        }
        return result;
    }

    // 0x87fb96 / 0x881e0f: probability one half.
    uint32_t half()
    {
        uint32_t split = (range + 1) >> 1;
        const uint32_t big = split << 24;
        uint32_t result = 0;
        if (value >= big)
        {
            result = 1;
            value -= big;
            split = range - split;
        }
        range = split << 1;
        value <<= 1;
        if (--count == 0)
        {
            value |= L8(buffer + pos);
            count = 8;
            ++pos;
        }
        return result;
    }

    // 0x87f24e: n bits, most significant first.
    uint32_t literal(int n)
    {
        uint32_t v = 0;
        while (--n >= 0)
            v |= half() << n;
        return v;
    }
};

// 0x882c4d.
void boolInit(uint32_t a, uint32_t data)
{
    S32(a + 0x10, 0);
    S32(a, 0);
    S32(a + 4, 0xff);
    S32(a + 0xc, 8);
    S32(a + 0x14, data);
    S32(a + 8, BE32(data));
    S32(a + 0x10, 4);
}

// Huffman reader (instance +0x13c): +0 bits left in the cache, +4 cache, +8 next word.
struct HuffReader
{
    uint32_t bits;
    uint32_t cache;
    uint32_t next;

    void load(uint32_t a)   { bits = L32(a); cache = L32(a + 4); next = L32(a + 8); }
    void store(uint32_t a)  { S32(a, bits); S32(a + 4, cache); S32(a + 8, next); }

    // 0x87fbf7: n bits (eax = reader, ecx = n).
    uint32_t read(uint32_t n)
    {
        cache &= L32(TabBitMask + 4 * bits);
        int32_t c = int32_t(n - bits);
        uint32_t high = 0;
        if (c > 0)
        {
            high = cache << (c & 31);
            cache = BE32(next);
            next += 4;
            c -= 32;
        }
        bits = uint32_t(-c);
        return (cache >> (bits & 31)) | high;
    }

    // 0x87fc80: skips n bits (eax = reader, ecx = n).
    void skip(uint32_t n)
    {
        bits -= n;
        if (int32_t(bits) < 0)
        {
            cache = BE32(next);
            next += 4;
            bits += 32;
        }
    }

    // 0x87fcba: one bit.
    uint32_t bit()
    {
        if (bits != 0)
        {
            --bits;
            return (cache >> (bits & 31)) & 1;
        }
        cache = BE32(next);
        next += 4;
        bits = 0x1f;
        return cache >> 31;
    }

    // 0x87fdf5 (edi = reader): a token through the 6-bit lookup table, then the tree.
    uint32_t token(uint32_t tree, uint32_t lookup)
    {
        uint32_t peek = ((1u << (bits & 31)) - 1) & cache;
        if (bits >= 6)
            peek >>= (bits - 6) & 31;
        else
            peek = (L8(next) | (peek << 8)) >> ((bits + 2) & 31);
        const uint32_t entry = L16(lookup + 2 * peek);
        skip(entry >> 12);
        if (entry & 1)
            return (entry >> 1) & 0x1f;
        uint32_t node = entry & 0x3e;
        do
        {
            const uint32_t at = tree + ((node >> 1) & 0x7f) * 12;
            node = bit() ? L32(at + 4) : L32(at);
        } while ((node & 1) == 0);
        return (node >> 1) & 0x7f;
    }
};

// 0x882c9b / 0x882cd9: big-endian bit reader at instance +0xfc (+0 buffer, +4 cache, +8 bits, +0xc offset).
struct RawReader
{
    uint32_t buffer;
    uint32_t cache;
    uint32_t bits;
    uint32_t offset;

    void init(uint32_t data)  { buffer = data; cache = BE32(data); bits = 32; offset = 4; }
    void load(uint32_t a)     { buffer = L32(a); cache = L32(a + 4); bits = L32(a + 8); offset = L32(a + 0xc); }
    void store(uint32_t a)    { S32(a, buffer); S32(a + 4, cache); S32(a + 8, bits); S32(a + 0xc, offset); }

    uint32_t read(uint32_t n)
    {
        uint32_t high = 0;
        uint32_t word = cache;
        if (bits < n)
        {
            n -= bits;
            high = (cache >> ((32 - bits) & 31)) << (n & 31);
            word = BE32(buffer + offset);
            offset += 4;
            bits = 32;
        }
        bits -= n;
        cache = word << (n & 31);
        return (word >> ((32 - n) & 31)) | high;
    }
};

// ---------------------------------------------------------------------------------------------------------------
// Frame decoder state: the guest instance plus the bitstream readers kept in registers while a frame decodes.

// A motion compensated prediction (0x88054a): source, filter offset (0 copies the block), stride, filter phases.
struct Prediction
{
    uint32_t src;
    int32_t offset;
    int32_t stride;
    uint8_t fx, fy;
};

enum Kind : uint8_t
{
    KindReference,      // same block of the previous or golden frame plus the residual
    KindPredicted,      // motion compensated prediction plus the residual
    KindIntra,          // residual + 128
};

// One block as the decode stage records it for reconstructBlock. Its coefficients are count entries of the row's
// list from first on: position << 16 | value, ascending positions, nonzero values only.
struct BlockJob
{
    uint32_t dst;           // destination in the new frame
    int32_t stride;         // destination and reference stride
    Prediction prediction;  // the reference block (src) or the prediction
    uint32_t first;
    uint8_t count;
    uint8_t eob;            // end-of-block class: picks the inverse transform
    uint8_t kind;
    uint8_t chroma;         // dequantisation table: 0 luma, 1 chroma
};

// Positions of the coefficient block (MMX order) that the inverse transforms of 0x88510e read and that the guest
// clears after it.
constexpr uint64_t Idct10Reads = 0x0f0f0f0full;
constexpr uint64_t Idct10Clears = 0x10f0f0fffull;

struct Frame
{
    uint32_t I;
    BoolDecoder a;      // +0x10c: header, modes and vectors
    BoolDecoder b;      // +0x124: coefficients
    HuffReader h;       // +0x13c: coefficients in Huffman mode
    bool huffman;
    bool predicted;                 // a block of the frame used a motion compensated prediction
    Prediction lastPrediction;      // the last one, whose result stays in the guest's prediction buffer
    // The guest's six coefficient blocks (instance +0x10) and its zero run counters (+0x378c) while a frame decodes;
    // live has a bit for every position that may hold a nonzero coefficient.
    alignas(16) int16_t coefs[6][64];
    uint64_t live[6];
    int32_t zeroRuns[4];
    // Per-frame instance tables the coefficient decode reads: the scan position of each order index (+0x1e4) and
    // the end-of-block class bytes from +0x263 on.
    uint8_t scan[64];
    uint8_t eobClass[257];
};

// Guest constant tables as bytes, copied once.
struct Tables
{
    bool ready;
    uint8_t coefBand[64];       // TabCoefBand
    uint8_t huffBand[64];       // TabHuffBand
    int32_t huffToken[12];      // TabHuffToken
    uint8_t refFrame[10];       // TabRefFrame
    uint8_t blockMode[10];      // TabBlockMode
};

Tables s_tables;

void loadTables()
{
    for (uint32_t i = 0; i < 64; ++i)
    {
        s_tables.coefBand[i] = uint8_t(L32(TabCoefBand + 4 * i));
        s_tables.huffBand[i] = uint8_t(L32(TabHuffBand + 4 * i));
    }
    for (uint32_t i = 0; i < 12; ++i)
        s_tables.huffToken[i] = LS32(TabHuffToken + 4 * i);
    for (uint32_t i = 0; i < 10; ++i)
    {
        s_tables.refFrame[i] = uint8_t(L32(TabRefFrame + 4 * i));
        s_tables.blockMode[i] = uint8_t(L32(TabBlockMode + 4 * i));
    }
    s_tables.ready = true;
}

inline uint32_t refFrame(uint32_t mode)     { return L32(TabRefFrame + 4 * mode); }

// 0x881e70: coefficient order by band, and the last index of each order position.
void bandOrder(uint32_t I, uint32_t bands)
{
    const uint32_t order = I + OffBandOrder;
    S8(order, 0);
    uint32_t n = 1;
    for (uint32_t band = 0; band < 16; ++band)
        for (uint32_t i = 1; i < 64; ++i)
            if (L8(bands + i) == band)
                S8(order + n++, i);
    uint8_t highest = 0;
    for (uint32_t i = 0; i < 64; ++i)
    {
        if (highest < L8(order + i))
            highest = L8(order + i);
        S8(order + 0x40 + i, highest + 1);
    }
}

// 0x88382a: a signed change of a mode probability.
int32_t modeDelta(BoolDecoder& d)
{
    if (!d.bit(0xcd))
        return 0;
    int32_t v = d.half() ? -1 : 1;
    if (!d.bit(0xab))
        return v * (1 << ((3 - d.bit(0x53)) & 31));
    if (!d.bit(199))
    {
        if (!d.bit(0x8c))
        {
            if (d.bit(0x7d))
                return v * 0x10;
            if (!d.bit(0x68))
                return v * 0x18;
            v *= 5;
        }
        else
            v *= 3;
    }
    else
        v = int32_t(d.literal(7)) * v;
    return v * 4;
}

// 0x88366b: mode tree probabilities from the mode probability vectors.
void modeTrees(uint32_t I)
{
    for (uint32_t k = 0; k < 10; ++k)
    {
        for (uint32_t c = 0; c < 3; ++c)
        {
            const uint32_t vec = I + OffModeProbs + 20 * c;
            uint32_t w[10];
            uint32_t sum = 0;
            for (uint32_t j = 0; j < 10; ++j)
            {
                w[j] = k == j ? 0 : uint32_t(L8(vec + j)) * 100;
                sum += w[j];
            }
            const uint32_t same = L8(vec + 10 + k), other = L8(vec + k);
            S8(I + OffSameModeProbs + k + 10 * c, ~uint32_t((same * 0xff) / (other + 1 + same)));
            const uint32_t t = I + OffModeTrees + 9 * k + 90 * c;
            const uint32_t a = w[2] + w[4] + w[3] + w[0];
            S8(t + 0, (a * 0xff) / (sum + 1) + 1);
            S8(t + 1, ((w[2] + w[0]) * 0xff) / (a + 1) + 1);
            const uint32_t b = w[6] + w[9] + w[8] + w[5];
            S8(t + 2, ((w[1] + w[7]) * 0xff) / (b + w[7] + 1 + w[1]) + 1);
            S8(t + 3, (w[0] * 0xff) / (w[2] + w[0] + 1) + 1);
            S8(t + 4, (w[3] * 0xff) / (w[4] + w[3] + 1) + 1);
            S8(t + 5, (w[1] * 0xff) / (w[1] + w[7] + 1) + 1);
            S8(t + 6, ((w[6] + w[5]) * 0xff) / (b + 1) + 1);
            S8(t + 7, (w[5] * 0xff) / (w[6] + w[5] + 1) + 1);
            S8(t + 8, (w[8] * 0xff) / (w[9] + w[8] + 1) + 1);
        }
    }
}

// 0x8838ec: mode probability updates of an inter frame.
void modeProbs(Frame& f)
{
    const uint32_t I = f.I;
    for (uint32_t ctx = 0; ctx < 3; ++ctx)
    {
        const uint32_t vec = I + OffModeProbs + 20 * ctx;
        if (f.a.bit(0xae))
        {
            const uint32_t src = TabModeVectors + (f.a.literal(4) + 16 * ctx) * 20;
            for (uint32_t i = 0; i < 10; ++i)
            {
                S8(vec + 10 + i, L8(src + 2 * i));
                S8(vec + i, L8(src + 2 * i + 1));
            }
        }
        if (f.a.bit(0xfe))
        {
            for (uint32_t i = 0; i < 10; ++i)
            {
                int32_t v = modeDelta(f.a) + L8(vec + 10 + i);
                S8(vec + 10 + i, v < 0 ? 0 : v > 0xff ? 0xff : v);
                v = modeDelta(f.a) + L8(vec + i);
                S8(vec + i, v < 0 ? 0 : v > 0xff ? 0xff : v);
            }
        }
    }
    modeTrees(I);
}

// A 7-bit probability: doubled, zero becomes one.
inline uint32_t prob7(BoolDecoder& d)
{
    const uint8_t v = uint8_t(d.literal(7) << 1);
    return v == 0 ? 1 : v;
}

// 0x883533: motion vector probability updates.
void vectorProbs(Frame& f)
{
    const uint32_t I = f.I;
    for (uint32_t k = 0; k < 2; ++k)
    {
        const uint32_t t = TabMvUpdate + 17 * k;
        if (f.a.bit(L8(t - 1)))
            S8(I + OffMvZeroProbs + k, prob7(f.a));
        if (f.a.bit(L8(t)))
            S8(I + OffMvSignProbs + k, prob7(f.a));
    }
    for (uint32_t k = 0; k < 2; ++k)
        for (uint32_t i = 0; i < 7; ++i)
            if (f.a.bit(L8(TabMvUpdate + 1 + 17 * k + i)))
                S8(I + OffMvShortProbs + 7 * k + i, prob7(f.a));
    for (uint32_t k = 0; k < 2; ++k)
        for (uint32_t i = 0; i < 8; ++i)
            if (f.a.bit(L8(TabMvUpdate + 8 + 17 * k + i)))
                S8(I + OffMvLongProbs + 8 * k + i, prob7(f.a));
}

// 0x8834ae: DC context probabilities.
void dcContextProbs(uint32_t I)
{
    uint32_t out = I + OffDcCtxProbs;
    for (uint32_t p = 0; p < 2; ++p)
    {
        for (uint32_t c = 0; c < 3; ++c)
        {
            for (uint32_t i = 0; i < 5; ++i)
            {
                const uint32_t t = TabDcContext + 4 * (2 * c + 6 * i);
                int32_t v = ((int32_t(L8(I + OffDcProbs + 11 * p + i)) * LS32(t - 4) + 0x80) >> 8) + LS32(t);
                if (v > 0xff)
                    v = 0xff;
                if (v < 1)
                    v = 1;
                S8(out + i, v);
            }
            out += 5;
        }
    }
}

// 0x88225b: coefficient probability updates.
void coefProbs(Frame& f, bool inter)
{
    const uint32_t I = f.I;
    uint8_t last[11];
    memset(last, 0x80, sizeof(last));
    for (uint32_t p = 0; p < 22; p += 11)
    {
        for (uint32_t i = 0; i < 11; ++i)
        {
            if (f.a.bit(L8(TabDcUpdate + i + p)))
            {
                last[i] = uint8_t(prob7(f.a));
                S8(I + OffDcProbs + p + i, last[i]);
            }
            else if (!inter)
                S8(I + OffDcProbs + p + i, last[i]);
        }
    }
    if (!inter)
        memcpy(P(I + OffRunProbs), P(TabRunDefault), 28);
    if (f.a.bit(0x80))
    {
        for (uint32_t i = 1; i < 64; ++i)
            if (f.a.bit(L8(TabBandUpdate + i)))
                S8(I + OffBands + i, f.a.literal(4));
        bandOrder(I, I + OffBands);
    }
    for (uint32_t p = 0; p < 28; p += 14)
        for (uint32_t i = 0; i < 14; ++i)
            if (f.a.bit(L8(TabRunUpdate + i + p)))
                S8(I + OffRunProbs + p + i, prob7(f.a));
    uint32_t t = TabAcUpdate;
    for (uint32_t ctx = 0; ctx < 3; ++ctx)
    {
        for (uint32_t plane = 0; plane < 2; ++plane)
        {
            for (uint32_t band = 0; band < 6; ++band)
            {
                const uint32_t dst = I + OffAcProbs + (ctx * 6 + plane * 18 + band) * 11;
                for (uint32_t i = 0; i < 11; ++i)
                {
                    if (f.a.bit(L8(t + i)))
                    {
                        last[i] = uint8_t(prob7(f.a));
                        S8(dst + i, last[i]);
                    }
                    else if (!inter)
                        S8(dst + i, last[i]);
                }
                t += 11;
            }
        }
    }
    dcContextProbs(I);
}

// 0x881ec0: leaf weights of a 12-token tree from its probabilities.
void weights12(uint32_t prob, uint32_t w)
{
    auto p = [&](int i) { return uint32_t(L8(prob + i)); };
    S32(w + 0x2c, (p(0) * p(1)) >> 8);
    S32(w + 0x00, ((0xff - p(1)) * p(0)) >> 8);
    S32(w + 0x04, (p(2) * (0xff - p(0))) >> 8);
    uint32_t a = ((0xff - p(2)) * (0xff - p(0))) >> 8;
    uint32_t b = (p(3) * a) >> 8;
    S32(w + 0x08, (p(4) * b) >> 8);
    b = ((0xff - p(4)) * b) >> 8;
    S32(w + 0x0c, (p(5) * b) >> 8);
    S32(w + 0x10, ((0xff - p(5)) * b) >> 8);
    a = ((0xff - p(3)) * a) >> 8;
    b = (p(6) * a) >> 8;
    S32(w + 0x14, (p(7) * b) >> 8);
    S32(w + 0x18, ((0xff - p(7)) * b) >> 8);
    a = ((0xff - p(6)) * a) >> 8;
    b = (p(8) * a) >> 8;
    S32(w + 0x1c, (p(9) * b) >> 8);
    S32(w + 0x20, ((0xff - p(9)) * b) >> 8);
    b = ((0xff - p(8)) * a) >> 8;
    S32(w + 0x24, (p(10) * b) >> 8);
    S32(w + 0x28, ((0xff - p(10)) * b) >> 8);
}

// 0x882000: leaf weights of the 9-token run length tree.
void weights9(uint32_t prob, uint32_t w)
{
    auto p = [&](int i) { return uint32_t(L8(prob + i)); };
    uint32_t a = (p(0) * p(1)) >> 8;
    S32(w + 0x00, (p(2) * a) >> 8);
    S32(w + 0x04, ((0xff - p(2)) * a) >> 8);
    a = ((0xff - p(1)) * p(0)) >> 8;
    S32(w + 0x08, (p(3) * a) >> 8);
    S32(w + 0x0c, ((0xff - p(3)) * a) >> 8);
    a = ((((0xff - p(0)) * p(4)) >> 8) * p(5)) >> 8;
    S32(w + 0x10, (p(6) * a) >> 8);
    S32(w + 0x14, ((0xff - p(6)) * a) >> 8);
    a = ((((0xff - p(0)) * p(4)) >> 8) * (0xff - p(5))) >> 8;
    S32(w + 0x18, (p(7) * a) >> 8);
    S32(w + 0x1c, ((0xff - p(7)) * a) >> 8);
    S32(w + 0x20, ((0xff - p(0)) * (0xff - p(4))) >> 8);
}

// 0x88321a: Huffman tree from n leaf weights (nodes of 12 bytes: two child words, a probability byte). The guest
// leaves its uninitialised stack in the upper 24 bits of the child words; they are written as zero here.
void buildTree(uint32_t tree, uint32_t w, int32_t n)
{
    struct Node
    {
        int32_t next;
        int32_t weight;
        uint32_t value;
    } nodes[64];
    for (int32_t i = 0; i < n; ++i)
    {
        nodes[i].value = (uint32_t(i * 2) & 0xfe) | 1;
        if (L32(w + 4 * i) == 0)
            S32(w + 4 * i, 1);
        nodes[i].next = -1;
        nodes[i].weight = LS32(w + 4 * i);
    }
    // 0x8831d1: inserts node c into the list at head, sorted by weight.
    auto insert = [&](int32_t c, int32_t& head) {
        int32_t e = head, prev = e;
        while (e != -1 && nodes[c].weight > nodes[e].weight)
        {
            prev = e;
            e = nodes[e].next;
        }
        if (e == head)
            head = c;
        else
            nodes[prev].next = c;
        nodes[c].next = e;
    };
    int32_t head = 0;
    for (int32_t i = 1; i < n; ++i)
        insert(i, head);
    int32_t counter = n - 1;
    int32_t m = n;
    uint32_t out = tree + uint32_t(n - 1) * 12 + 8;
    int32_t second = nodes[head].next;
    while (second != -1)
    {
        const int32_t w1 = nodes[head].weight;
        const int32_t total = nodes[second].weight + w1;
        S32(out - 0x14, nodes[head].value);
        S32(out - 0x10, nodes[second].value);
        out -= 12;
        --counter;
        nodes[m].next = -1;
        nodes[m].weight = total;
        nodes[m].value = uint32_t(counter & 0x7f) << 1;
        S8(out, uint32_t((w1 << 8) / total));
        int32_t rest = nodes[second].next;
        insert(m, rest);
        ++m;
        head = rest;
        second = nodes[head].next;
    }
}

// 0x88335c: 6-bit lookup table of a tree (bit 0 leaf, bits 1-7 token or node, bits 12-15 bits used).
void buildLookup(uint32_t tree, uint32_t lookup)
{
    for (uint32_t i = 0; i < 64; ++i)
    {
        uint32_t node = 0, used = 0;
        int32_t bit = 6;
        do
        {
            ++used;
            --bit;
            const uint32_t at = tree + ((node >> 1) & 0x7f) * 12;
            node = ((i >> bit) & 1) ? L32(at + 4) : L32(at);
        } while ((node & 1) == 0 && bit > 0);
        S16(lookup + 2 * i, ((((node >> 1) & 0x7f) | (used * 0x800)) << 1) | (node & 1));
    }
}

// 0x8820fb: Huffman trees and lookup tables from the coefficient probabilities.
void huffmanTables(uint32_t I)
{
    for (uint32_t p = 0; p < 2; ++p)
    {
        const uint32_t w = I + OffDcWeights + 0x30 * p, tree = I + OffDcTrees + 0x90 * p;
        weights12(I + OffDcProbs + 11 * p, w);
        buildTree(tree, w, 12);
        buildLookup(tree, I + OffDcLookup + 0x80 * p);
    }
    for (uint32_t p = 0; p < 2; ++p)
    {
        const uint32_t w = I + OffRunWeights + 0x38 * p, tree = I + OffRunTrees + 0xa8 * p;
        weights9(I + OffRunProbs + 14 * p, w);
        buildTree(tree, w, 9);
        buildLookup(tree, I + OffRunLookup + 0x80 * p);
    }
    uint32_t t = 0;
    for (uint32_t ctx = 0; ctx < 3; ++ctx)
        for (uint32_t plane = 0; plane < 2; ++plane)
            for (uint32_t band = 0; band < 6; ++band, ++t)
            {
                const uint32_t w = I + OffAcWeights + 0x30 * t, tree = I + OffAcTrees + 0x90 * t;
                weights12(I + OffAcProbs + (ctx * 6 + plane * 18 + band) * 11, w);
                buildTree(tree, w, 12);
                buildLookup(tree, I + OffAcLookup + 0x80 * t);
            }
}

// 0x8824a4: above contexts and DC predictors at the start of a frame.
void resetFrameContexts(uint32_t I)
{
    const uint32_t blocks = L32(I + OffBlocksX);
    for (uint32_t u = 0, a = L32(I + OffAboveY); u < blocks + 8; ++u, a += 16)
    {
        S32(a + 4, 0xffffffff);
        S16(a + 8, 4);
        S16(a + 10, 0);
        S8(a, 0);
    }
    for (uint32_t u = 0, a = L32(I + OffAboveU), b = L32(I + OffAboveV); u < (blocks >> 1) + 8; ++u, a += 16, b += 16)
    {
        S32(a + 4, 0xffffffff);
        S16(a + 8, 4);
        S8(a, 0);
        S16(a + 10, 0);
        S32(b + 4, 0xffffffff);
        S16(b + 8, 4);
        S8(b, 0);
        S16(b + 10, 0);
    }
    S16(I + OffDcY, 0);
    S16(I + OffDcU, 0x80);
    S16(I + OffDcV, 0x80);
    for (uint32_t i = 0; i < 2; ++i)
    {
        S16(I + 0xde + 2 * i, 0);
        S16(I + 0xe6 + 2 * i, 0);
        S16(I + 0xee + 2 * i, 0);
    }
}

// 0x882430: left contexts at the start of a macroblock row.
void resetRowContexts(uint32_t I)
{
    memset(P(I + OffLeftCtx), 0, 0x40);
    for (uint32_t i = 0; i < 4; ++i)
    {
        S32(I + OffLeftCtx + 16 * i + 4, 0xffffffff);
        S16(I + OffLeftCtx + 16 * i + 8, 4);
    }
}

// 0x880451: nearest and near vectors of the reference frame ref among the 12 neighbours.
void findNeighbours(uint32_t I, uint32_t row, uint32_t col, uint32_t ref, uint32_t* context)
{
    const uint32_t here = L32(I + OffMbCols) * row + col;
    const uint32_t modes = L32(I + OffModeMap), mvs = L32(I + OffMvMap);
    uint32_t nearest = 0, near = 0, count = 1, index = 0;
    for (; index < 12; ++index)
    {
        const uint32_t at = L32(I + OffNeighbourOffsets + 4 * index) + here;
        if (refFrame(uint32_t(int32_t(LS8(modes + at)))) == ref && L32(mvs + 4 * at) != 0)
        {
            nearest = L32(mvs + 4 * at);
            count = 2;
            break;
        }
    }
    for (uint32_t i = index + 1; i < 12; ++i)
    {
        const uint32_t at = L32(I + OffNeighbourOffsets + 4 * i) + here;
        if (refFrame(uint32_t(int32_t(LS8(modes + at)))) == ref)
        {
            const uint32_t mv = L32(mvs + 4 * at);
            if (mv != nearest && mv != 0)
            {
                near = mv;
                count = 0;
                break;
            }
        }
    }
    if (ref == 1)
    {
        *context = count;
        S32(I + OffNearestIndex, index);
        S32(I + OffNearest, nearest);
        S32(I + OffNear, near);
    }
    else
    {
        S32(I + OffGoldNearestIndex, index);
        S32(I + OffGoldNearest, nearest);
        S32(I + OffGoldNear, near);
    }
}

// 0x880127: macroblock mode.
uint32_t decodeMode(Frame& f, uint32_t last, uint32_t context)
{
    const uint32_t I = f.I;
    BoolDecoder& d = f.a;
    if (d.bit(L8(I + OffSameModeProbs + context * 10 + last)))
        return last;
    const uint32_t t = I + OffModeTrees + (last + context * 10) * 9;
    if (!d.bit(L8(t)))
    {
        if (!d.bit(L8(t + 1)))
            return d.bit(L8(t + 3)) << 1;
        return d.bit(L8(t + 4)) + 3;
    }
    if (!d.bit(L8(t + 2)))
        return d.bit(L8(t + 5)) ? 7 : 1;
    if (!d.bit(L8(t + 6)))
        return d.bit(L8(t + 7)) + 5;
    return d.bit(L8(t + 8)) + 8;
}

// 0x88021a: a motion vector, relative to the nearest vector when it is one of the two closest neighbours.
void decodeVector(Frame& f, int16_t* mv, uint32_t mode)
{
    const uint32_t I = f.I;
    BoolDecoder& d = f.a;
    int16_t bx = 0, by = 0;
    if (mode == 2)
    {
        if (LS32(I + OffNearestIndex) < 2)
        {
            bx = LS16(I + OffNearest);
            by = LS16(I + OffNearest + 2);
        }
    }
    else if (LS32(I + OffGoldNearestIndex) < 2)
    {
        bx = LS16(I + OffGoldNearest);
        by = LS16(I + OffGoldNearest + 2);
    }
    for (uint32_t k = 0; k < 2; ++k)
    {
        int32_t v;
        if (!d.bit(L8(I + OffMvZeroProbs + k)))
        {
            const uint32_t s = I + OffMvShortProbs + 7 * k;
            if (!d.bit(L8(s)))
            {
                if (!d.bit(L8(s + 1)))
                    v = d.bit(L8(s + 2));
                else
                    v = d.bit(L8(s + 3)) + 2;
            }
            else if (!d.bit(L8(s + 4)))
                v = d.bit(L8(s + 5)) + 4;
            else
                v = d.bit(L8(s + 6)) + 6;
        }
        else
        {
            const uint32_t l = I + OffMvLongProbs + 8 * k;
            uint32_t u = d.bit(L8(l));
            u += d.bit(L8(l + 1)) * 2;
            u += d.bit(L8(l + 2)) * 4;
            u += d.bit(L8(l + 7)) * 0x80;
            u += d.bit(L8(l + 6)) * 0x40;
            u += d.bit(L8(l + 5)) * 0x20;
            u += d.bit(L8(l + 4)) * 0x10;
            if ((u & 0xf0) == 0)
                v = int32_t(u + 8);
            else
                v = int32_t(u + d.bit(L8(l + 3)) * 8);
        }
        if (v != 0 && d.bit(L8(I + OffMvSignProbs + k)))
            v = -v;
        if (k == 0)
            mv[0] = int16_t(bx + v);
        else
            mv[1] = int16_t(v + by);
    }
}

// 0x8800e4: the mode of one luma block of a four-vector macroblock.
uint32_t blockMode(Frame& f)
{
    const uint32_t a = f.a.half();
    const uint32_t b = f.a.half();
    static const uint8_t modes[4] = { 0, 2, 3, 4 };
    return modes[b + a * 2];
}

// 0x880843: mode and motion vectors of an inter frame macroblock.
void interModes(Frame& f, uint32_t row, uint32_t col)
{
    const uint32_t I = f.I;
    uint32_t context;
    findNeighbours(I, row, col, 1, &context);
    const uint32_t mode = decodeMode(f, L32(I + OffLastMode), context);
    S32(I + OffLastMode, mode);
    const uint32_t mapIndex = L32(I + OffMbCols) * row + col;
    S8(L32(I + OffModeMap) + mapIndex, mode);
    S32(I + OffMode, mode);
    const uint32_t mvAt = L32(I + OffMvMap) + 4 * mapIndex;
    if (mode == 7)
    {
        for (uint32_t b = 0; b < 4; ++b)
            S32(I + OffBlockModes + 4 * b, blockMode(f));
        S32(I + OffBlockModes + 16, 7);
        S32(I + OffBlockModes + 20, 7);
        int32_t sx = 0, sy = 0;
        for (uint32_t b = 0; b < 4; ++b)
        {
            const uint32_t m = L32(I + OffBlockModes + 4 * b);
            const uint32_t at = I + OffBlockMvs + 4 * b;
            if (m == 0)
            {
                S16(at, 0);
                S16(at + 2, 0);
                continue;
            }
            int16_t mv[2];
            if (m == 3)
            {
                mv[0] = LS16(I + OffNearest);
                mv[1] = LS16(I + OffNearest + 2);
            }
            else if (m == 4)
            {
                mv[0] = LS16(I + OffNear);
                mv[1] = LS16(I + OffNear + 2);
            }
            else if (m == 2)
                decodeVector(f, mv, 2);
            else
                continue;
            S16(at, uint16_t(mv[0]));
            S16(at + 2, uint16_t(mv[1]));
            sx += mv[0];
            sy += mv[1];
        }
        S16(mvAt, L16(I + OffBlockMvs + 12));
        S16(mvAt + 2, L16(I + OffBlockMvs + 14));
        const uint32_t ax = uint32_t((int32_t(sx >= 0) + 1 + sx) >> 2);
        const uint32_t ay = uint32_t((int32_t(sy >= 0) + 1 + sy) >> 2);
        S16(I + OffBlockMvs + 16, ax);
        S16(I + OffBlockMvs + 18, ay);
        S16(I + OffBlockMvs + 20, ax);
        S16(I + OffBlockMvs + 22, ay);
        return;
    }
    int16_t mv[2] = { 0, 0 };
    uint32_t unused;
    if (mode == 2)
        decodeVector(f, mv, 2);
    else if (mode == 3)
    {
        mv[0] = LS16(I + OffNearest);
        mv[1] = LS16(I + OffNearest + 2);
    }
    else if (mode == 4)
    {
        mv[0] = LS16(I + OffNear);
        mv[1] = LS16(I + OffNear + 2);
    }
    else if (mode == 6)
    {
        findNeighbours(I, row, col, 2, &unused);
        decodeVector(f, mv, 6);
    }
    else if (mode == 8)
    {
        findNeighbours(I, row, col, 2, &unused);
        mv[0] = LS16(I + OffGoldNearest);
        mv[1] = LS16(I + OffGoldNearest + 2);
    }
    else if (mode == 9)
    {
        findNeighbours(I, row, col, 2, &unused);
        mv[0] = LS16(I + OffGoldNear);
        mv[1] = LS16(I + OffGoldNear + 2);
    }
    S16(mvAt, uint16_t(mv[0]));
    S16(mvAt + 2, uint16_t(mv[1]));
    for (uint32_t b = 0; b < 6; ++b)
    {
        S16(I + OffBlockMvs + 4 * b, uint16_t(mv[0]));
        S16(I + OffBlockMvs + 4 * b + 2, uint16_t(mv[1]));
        S32(I + OffBlockModes + 4 * b, mode);
    }
}

// Value of a DCT token above 4 (0x882615): its category (probabilities probs + 6 .. 10) base plus extra bits.
inline uint32_t tokenValue(BoolDecoder& d, const uint8_t* probs)
{
    uint32_t cat;
    if (!d.bit(probs[6]))
        cat = d.bit(probs[7]) + 5;
    else if (!d.bit(probs[8]))
        cat = d.bit(probs[9]) + 7;
    else
        cat = d.bit(probs[10]) + 9;
    const uint32_t t = TabTokenBase + cat * 16;
    uint32_t v = L16(t);
    for (int32_t bit = LS16(t + 2); bit >= 0; --bit)
        v += d.bit(L8(t + 4 + uint32_t(bit))) << (bit & 31);
    return v;
}

// A coefficient into the block: v with sign s, at position pos.
inline void putCoef(int16_t* c, uint64_t& live, uint32_t pos, uint32_t v, uint32_t s)
{
    c[pos] = int16_t(uint16_t((v ^ (0u - s)) + s));
    live |= uint64_t(1) << pos;
}

// 0x882615 on a local decoder: coefficients of a block; returns the end-of-block class.
inline uint32_t boolBlock(const Frame& f, BoolDecoder& d, int16_t* c, uint64_t& live, uint32_t chroma, uint8_t* above,
                          uint8_t* left)
{
    const uint32_t I = f.I;
    const uint8_t* const acProbs = P(I + OffAcProbs + chroma * 0xc6);
    const uint8_t* const runProbs = P(I + OffRunProbs);
    const uint8_t* const dcProbs = P(I + OffDcProbs + chroma * 11);
    const uint8_t* const ctx = P(I + OffDcCtxProbs + (chroma * 3 + *above + *left) * 5);
    uint32_t prev;
    if (!d.bit(ctx[0]))
    {
        *left = 0;
        prev = 0;
        *above = 0;
    }
    else
    {
        *left = 1;
        *above = 1;
        uint32_t v;
        if (!d.bit(ctx[2]))
        {
            prev = 1;
            v = 1;
        }
        else
        {
            prev = 2;
            if (!d.bit(ctx[3]))
                v = !d.bit(ctx[4]) ? 2 : d.bit(dcProbs[5]) + 3;
            else
                v = tokenValue(d, dcProbs);
        }
        putCoef(c, live, 0, v, d.half());
    }
    uint8_t i = 1;
    do
    {
        const uint8_t* const pr = acProbs + (s_tables.coefBand[i] + prev * 6) * 11;
        if ((i < 2 || prev != 0) && !d.bit(pr[0]))
        {
            if (!d.bit(pr[1]))
            {
                ++i;
                break;
            }
            prev = 0;
            const uint8_t* const r = runProbs + (i > 5 ? 14 : 0);
            uint32_t run;
            if (!d.bit(r[0]))
            {
                if (!d.bit(r[1]))
                    run = d.bit(r[2]) + 1;
                else
                    run = d.bit(r[3]) + 3;
            }
            else if (!d.bit(r[4]))
            {
                if (!d.bit(r[5]))
                    run = d.bit(r[6]) + 5;
                else
                    run = d.bit(r[7]) + 7;
            }
            else
            {
                run = d.bit(r[8]);
                run += d.bit(r[9]) * 2;
                run += d.bit(r[10]) * 4;
                run += d.bit(r[11]) * 8;
                run += d.bit(r[12]) * 0x10;
                run += 9;
                run += d.bit(r[13]) * 0x20;
            }
            i = uint8_t(i + run);
        }
        else
        {
            uint32_t v;
            if (!d.bit(pr[2]))
            {
                prev = 1;
                v = 1;
            }
            else
            {
                prev = 2;
                if (!d.bit(pr[3]))
                    v = !d.bit(pr[4]) ? 2 : d.bit(pr[5]) + 3;
                else
                    v = tokenValue(d, pr);
            }
            const uint32_t s = d.half();
            putCoef(c, live, f.scan[i], v, s);
            ++i;
        }
    } while (i < 0x40);
    return f.eobClass[uint8_t(i - 1) + 1];
}

// 0x882615: coefficients of a block from the boolean decoder; returns the end-of-block class.
uint32_t boolCoefs(Frame& f, int16_t* c, uint64_t& live, uint32_t chroma, uint8_t* above, uint8_t* left)
{
    BoolDecoder d = f.b;
    const uint32_t eob = boolBlock(f, d, c, live, chroma, above, left);
    f.b = d;
    return eob;
}

// Zero run count of the Huffman decoder (0x87fe8f).
inline uint32_t huffRun(HuffReader& h)
{
    uint32_t v = h.read(2) + 1;
    if (v == 3)
        v = h.read(2) + 3;
    else if (v == 4)
        v = h.bit() ? h.read(6) + 0xb : h.read(2) + 7;
    return v;
}

// 0x87fe8f on a local reader: coefficients of a block; returns the end-of-block class.
inline uint32_t huffBlock(Frame& f, HuffReader& h, int16_t* c, uint64_t& live, uint32_t chroma)
{
    const uint32_t I = f.I;
    int32_t& dcRuns = f.zeroRuns[chroma];
    uint32_t i = 1;
    uint32_t ctx;
    if (dcRuns >= 1)
    {
        --dcRuns;
        ctx = 0;
    }
    else
    {
        const uint32_t token = h.token(I + OffDcTrees + chroma * 0x90, I + OffDcLookup + chroma * 0x80);
        int32_t value = s_tables.huffToken[token];
        if (token == 0xb)
            return f.eobClass[i];
        if (token == 0)
        {
            dcRuns = int32_t(huffRun(h) - 1);
            ctx = 0;
        }
        else
        {
            if (token > 4)
                value += int32_t(h.read(token <= 9 ? token - 4 : 0xb));
            putCoef(c, live, 0, uint32_t(value), h.bit());
            ctx = (value > 1) + 1;
        }
    }
    int32_t& acRuns = f.zeroRuns[2 + chroma];
    if (acRuns >= 1)
    {
        --acRuns;
        return f.eobClass[i];
    }
    const uint32_t acTrees = I + OffAcTrees + chroma * 6 * 0x90, acLookup = I + OffAcLookup + chroma * 6 * 0x80;
    do
    {
        const uint32_t t = s_tables.huffBand[i] + ctx * 12;
        const uint32_t token = h.token(acTrees + t * 0x90, acLookup + t * 0x80);
        if (token == 0)
        {
            const uint32_t high = i >= 6;
            const uint32_t run = h.token(I + OffRunTrees + high * 0xa8, I + OffRunLookup + high * 0x80);
            if (run < 8)
                i += run;
            else
                i = i + h.read(6) + 8;
            ctx = 0;
            ++i;
        }
        else if (token == 0xb)
        {
            if (i == 1)
                acRuns = int32_t(huffRun(h) - 1);
            break;
        }
        else
        {
            int32_t value = s_tables.huffToken[token];
            if (token > 4)
                value += int32_t(h.read(token <= 9 ? token - 4 : 0xb));
            putCoef(c, live, f.scan[i], uint32_t(value), h.bit());
            ctx = (value > 1) + 1;
            ++i;
        }
    } while (int32_t(i) < 0x40);
    return f.eobClass[i];
}

// 0x87fe8f: coefficients of a block from the Huffman reader; returns the end-of-block class.
uint32_t huffCoefs(Frame& f, int16_t* c, uint64_t& live, uint32_t chroma)
{
    HuffReader h = f.h;
    const uint32_t eob = huffBlock(f, h, c, live, chroma);
    f.h = h;
    return eob;
}

// 0x87fd73: DC prediction from the left and above blocks of the same reference frame (ref).
inline void predictDc(int16_t* c, uint32_t ref, uint32_t store, uint32_t above, uint32_t left)
{
    uint32_t sum = 0;
    uint8_t n = 0;
    if (ref == L16(left + 8))
    {
        sum = uint32_t(int32_t(LS16(left + 10)));
        n = 1;
    }
    if (ref == L16(above + 8))
    {
        sum += uint32_t(int32_t(LS16(above + 10)));
        ++n;
    }
    if (n == 0)
        sum = uint32_t(int32_t(LS16(store + 2 * ref)));
    if (n == 2)
        sum = uint32_t(int32_t(((sum & 0xffff) >> 15) + sum) >> 1);
    c[0] = int16_t(uint16_t(uint32_t(uint16_t(c[0])) + sum));
    S16(store + 2 * ref, uint16_t(c[0]));
}

// 0x88054a: the motion compensated prediction of a block, as the source and filter of predict or copyBlock.
// Where a block of a macroblock decodes: the instance fields 0x880ab1 sets for it.
struct BlockPlace
{
    uint32_t above, left;   // contexts (+0x00, +0x04)
    uint32_t dcPred;        // DC predictors of the plane (+0x08)
    uint32_t offset;        // block offset in the frame (+0x68)
    int32_t stride;         // destination and reference stride (+0x6c)
    uint32_t chroma;        // plane != 0 (+0x70)
    uint32_t mvShift;       // (+0x74)
    uint32_t mvMask;        // (+0x78)
    int32_t mcStride;       // motion compensation stride (+0x7c)
};

void planPrediction(uint32_t I, uint32_t block, uint32_t mode, const BlockPlace& place, Prediction& p)
{
    uint32_t ref = L32(I + OffPrevious);
    const uint32_t mask = place.mvMask;
    if (s_tables.refFrame[mode] == 2)
        ref = L32(I + OffGolden);
    const int16_t mx = LS16(I + OffBlockMvs + 4 * block), my = LS16(I + OffBlockMvs + 4 * block + 2);
    const int32_t stride = place.mcStride;
    const uint32_t shift = place.mvShift & 31;
    uint32_t fx = uint32_t(int32_t(mx)) & mask;
    uint32_t fy = uint32_t(int32_t(my)) & mask;
    const int32_t ix = int32_t(((uint32_t(int32_t(mx) >> 31) & mask) + uint32_t(int32_t(mx)))) >> shift;
    const int32_t iy = int32_t(((uint32_t(int32_t(my) >> 31) & mask) + uint32_t(int32_t(my)))) >> shift;
    p.src = uint32_t(iy * stride + ix) + place.offset + ref;
    p.stride = stride;
    int32_t dir = 0;
    if (fx != 0)
        dir = mx > 0 ? 1 : -1;
    if (fy != 0)
        dir += (my > 0 ? 1 : -1) * stride;
    p.offset = dir;
    if (block < 4)
    {
        fx <<= 1;
        fy <<= 1;
    }
    p.fx = uint8_t(fx);
    p.fy = uint8_t(fy);
}

// The prediction of a block into dst (words).
inline void runPrediction(const Prediction& p, int16_t* dst)
{
    uint8_t tmp[64];
    if (p.offset == 0)
        copyBlock(P(p.src), p.stride, dst);
    else if (predict(P(p.src), P(p.src) + p.offset, tmp, p.stride, p.fx, p.fy))
        copyBlock(tmp, 8, dst);
}

// The prediction of a block as bytes (stride 8) in tmp; false when predict leaves it unwritten.
inline bool predictionBytes(const Prediction& p, uint8_t* tmp)
{
    if (p.offset == 0)
    {
        copyBytes(tmp, 8, P(p.src), p.stride);
        return true;
    }
    return predict(P(p.src), P(p.src) + p.offset, tmp, p.stride, p.fx, p.fy);
}

// The coefficient block of a recorded job in c (all 64 positions).
inline void denseCoefs(const BlockJob& j, const uint32_t* entries, int16_t* c)
{
    for (int i = 0; i < 64; i += 8)
        vst1q_s16(c + i, vdupq_n_s16(0));
    for (uint32_t k = 0; k < j.count; ++k)
        c[entries[k] >> 16] = int16_t(entries[k]);
}

// The inverse transform of a recorded block into out (words).
inline void runTransform(const BlockJob& j, const uint32_t* entries, const int16_t* q, int16_t* out)
{
    alignas(16) int16_t c[64];
    denseCoefs(j, entries, c);
    if (j.eob <= 1)
        idctDc(c, q, out);
    else if (j.eob <= 10)
        idct10(c, q, out);
    else
        idct64(c, q, out);
}

// The residual of a recorded block: Zero, Constant (all positions r) or Full (in residual).
enum Residual
{
    ResidualZero,
    ResidualConstant,
    ResidualFull,
};

inline Residual residualOf(const BlockJob& j, const uint32_t* entries, const int16_t* q, int16_t& r, int16_t* residual)
{
    if (j.eob <= 1)
    {
        // 0x8849c8 reads only the first coefficient.
        if (j.count == 0 || (entries[0] >> 16) != 0)
            return ResidualZero;
        r = int16_t((int32_t(q[0]) * int16_t(entries[0]) + 15) >> 5);
        return r == 0 ? ResidualZero : ResidualConstant;
    }
    // Both transforms map zero coefficients to a zero residual.
    const uint64_t reads = j.eob <= 10 ? Idct10Reads : ~uint64_t(0);
    bool any = false;
    for (uint32_t k = 0; k < j.count; ++k)
        any |= ((reads >> (entries[k] >> 16)) & 1) != 0;
    if (!any)
        return ResidualZero;
    runTransform(j, entries, q, residual);
    return ResidualFull;
}

// 0x880635 for a recorded block: inverse transform, prediction and reconstruction into the new frame.
// quant: the luma and chroma dequantisation tables.
void reconstructBlock(const BlockJob& j, const uint32_t* entries, const int16_t* const* quant)
{
    alignas(16) int16_t residual[64];
    alignas(8) uint8_t pred[64];
    int16_t r = 0;
    const Residual kind = residualOf(j, entries, quant[j.chroma], r, residual);
    uint8_t* dst = P(j.dst);
    switch (j.kind)
    {
    case KindReference:
        if (kind == ResidualZero)
            copyBytes(dst, j.stride, P(j.prediction.src), j.stride);
        else if (kind == ResidualConstant)
            addInterConstant(dst, P(j.prediction.src), r, j.stride);
        else
            addInter(dst, P(j.prediction.src), residual, j.stride);
        break;
    case KindPredicted:
        if (!predictionBytes(j.prediction, pred))
            break;
        if (kind == ResidualZero)
            copyBytes(dst, j.stride, pred, 8);
        else if (kind == ResidualConstant)
            addPredictionConstant(pred, r, dst, j.stride);
        else
            addPredictionBytes(pred, residual, dst, j.stride);
        break;
    default:
        if (kind == ResidualFull)
            putIntra(dst, residual, j.stride);
        else
            putIntraConstant(dst, r, j.stride);
        break;
    }
}

// Coefficient entries of a decoded block appended to out (n: the count); the positions the guest clears after the
// inverse transform of class eob become zero.
inline uint32_t recordCoefs(int16_t* c, uint64_t& live, uint32_t eob, uint32_t* out)
{
    const uint64_t clears = eob <= 1 ? 1 : eob <= 10 ? Idct10Clears : ~uint64_t(0);
    uint32_t n = 0;
    uint64_t keep = 0;
    for (uint32_t half = 0; half < 64; half += 32)
    {
        uint32_t m = uint32_t(live >> half);
        while (m != 0)
        {
            const uint32_t pos = uint32_t(__builtin_ctz(m)) + half;
            m &= m - 1;
            const int16_t v = c[pos];
            if (v == 0)
                continue;
            out[n++] = (pos << 16) | uint16_t(v);
            if ((clears >> pos) & 1)
                c[pos] = 0;
            else
                keep |= uint64_t(1) << pos;
        }
    }
    live = keep;
    return n;
}

// The ring slot of a macroblock row: its block jobs and their coefficient entries.
struct RowSlot
{
    std::vector<BlockJob> jobs;
    std::vector<uint32_t> entries;
    uint32_t used;          // entries of the row so far
};

// 0x8806e1: coefficients, DC prediction and contexts of one block, recorded with what its reconstruction needs.
void decodeBlock(Frame& f, uint32_t block, uint32_t mode, const BlockPlace& place, BlockJob& job, RowSlot& slot)
{
    const uint32_t I = f.I;
    int16_t* const c = f.coefs[block];
    uint64_t live = f.live[block];
    const uint32_t above = place.above, left = place.left;
    const uint32_t chroma = place.chroma;
    const uint32_t eob = f.huffman ? huffCoefs(f, c, live, chroma) : boolCoefs(f, c, live, chroma, P(above), P(left));
    const uint32_t ref = s_tables.refFrame[mode];
    predictDc(c, ref, place.dcPred, above, left);
    live |= 1;
    S32(left + 4, L32(I + OffBlockModes + 4 * block));
    S32(above + 4, L32(left + 4));
    S16(left + 10, uint16_t(c[0]));
    S16(above + 10, uint16_t(c[0]));
    S16(left + 8, ref);
    S16(above + 8, ref);
    job.chroma = L8(TabBlockPlane + block);
    job.eob = uint8_t(eob);
    job.first = slot.used;
    job.count = uint8_t(recordCoefs(c, live, eob, slot.entries.data() + slot.used));
    f.live[block] = live;
    slot.used += job.count;
    // 0x880635: the kind of reconstruction.
    const uint32_t offset = place.offset;
    job.dst = L32(I + OffNewFrame) + offset;
    job.stride = place.stride;
    if (mode == 0)
    {
        job.kind = KindReference;
        job.prediction.src = L32(I + OffPrevious) + offset;
    }
    else if (s_tables.blockMode[mode] != 0)
    {
        job.kind = KindPredicted;
        planPrediction(I, block, mode, place, job.prediction);
        f.lastPrediction = job.prediction;
        f.predicted = true;
    }
    else if (mode != 5)
        job.kind = KindIntra;
    else
    {
        job.kind = KindReference;
        job.prediction.src = L32(I + OffGolden) + offset;
    }
}

// 0x880ab1: one macroblock; its six blocks are recorded in jobs and slot.
void decodeMacroblock(Frame& f, uint32_t row, uint32_t col, BlockJob* jobs, RowSlot& slot)
{
    const uint32_t I = f.I;
    if (L32(I + OffFieldFlag) == 0)
        S32(I + OffFieldMb, 0);
    else
    {
        uint8_t p = L8(I + OffFieldProb);
        if (col > 3)
        {
            if (L32(I + OffFieldMb) == 0)
                p = uint8_t(p + uint8_t(int32_t(0x100 - uint32_t(p)) >> 1));
            else
                p = uint8_t(p - (p >> 1));
        }
        S32(I + OffFieldMb, f.a.bit(p));
    }
    if (L8(I + OffInterFrame) == 0)
        S32(I + OffMode, 1);
    else
        interModes(f, row, col);
    const uint32_t mode = L32(I + OffMode);
    const uint32_t ys = L32(I + OffYStride);
    const bool field = L32(I + OffFieldMb) != 0;
    const uint32_t step = field ? 1 : 8;
    const uint32_t off = row * 16 * ys + L32(I + OffYOrigin) + col * 16;
    const uint32_t aboveY = L32(I + OffAboveY) + col * 32;
    if (slot.entries.size() < slot.used + 6 * 64)
        slot.entries.resize(std::max<size_t>(slot.entries.size() * 2, slot.used + 6 * 64));
    BlockPlace p{ aboveY, I + OffLeftCtx, I + OffDcY, off, int32_t(field ? ys * 2 : ys), 0, 2, 3, int32_t(ys) };
    decodeBlock(f, 0, mode, p, jobs[0], slot);
    p.offset = off + 8;
    p.above = aboveY + 0x10;
    decodeBlock(f, 1, mode, p, jobs[1], slot);
    p.offset = ys * step + off;
    p.above = aboveY;
    p.left = I + OffLeftCtx + 0x10;
    decodeBlock(f, 2, mode, p, jobs[2], slot);
    p.offset += 8;
    p.above = aboveY + 0x10;
    decodeBlock(f, 3, mode, p, jobs[3], slot);
    const uint32_t us = L32(I + OffUvStride);
    p = BlockPlace{ L32(I + OffAboveU) + col * 16, I + OffLeftCtx + 0x20, I + OffDcU,
                    row * 8 * us + L32(I + OffUOrigin) + col * 8, int32_t(us), 1, 3, 7, int32_t(us) };
    decodeBlock(f, 4, mode, p, jobs[4], slot);
    p.offset = us * (row * 8) + L32(I + OffVOrigin) + col * 8;
    p.above = L32(I + OffAboveV) + col * 16;
    p.left = I + OffLeftCtx + 0x30;
    p.dcPred = I + OffDcV;
    decodeBlock(f, 5, mode, p, jobs[5], slot);
    // The fields as the guest leaves them after the last block.
    S32(I + OffAbove, p.above);
    S32(I + OffLeft, p.left);
    S32(I + OffDcPred, p.dcPred);
    S32(I + OffBlockRow, row * 8);
    S32(I + OffBlockCol, col * 8);
    S32(I + OffBlockOffset, p.offset);
    S32(I + OffBlockStride, us);
    S32(I + OffPlane, 2);
    S32(I + OffMvShift, 3);
    S32(I + OffMvMask, 7);
    S32(I + OffMcStride, us);
}

// 0x882a93 up to its macroblock loop: probabilities, tables and contexts of a frame.
void setupFrame(Frame& f, uint32_t argumentSlot)
{
    const uint32_t I = f.I;
    S32(GlobalArgSlot, argumentSlot);
    const bool inter = L8(I + OffInterFrame) != 0;
    if (!inter)
    {
        memcpy(P(I + OffModeProbs), P(TabModeDefault), 0x50);
        memcpy(P(I + OffMvZeroProbs), P(TabMvDefault + 0x20), 2);
        memcpy(P(I + OffMvShortProbs), P(TabMvDefault), 14);
        memcpy(P(I + OffMvSignProbs), P(TabMvDefault + 0x24), 2);
        memcpy(P(I + OffMvLongProbs), P(TabMvDefault + 0x10), 16);
        memset(P(L32(I + OffModeMap)), 1, L32(I + OffMbCount));
        memcpy(P(I + OffBands), P(L32(I + OffFieldFlag) == 1 ? TabDefBands0 : TabDefBands1), 0x40);
        bandOrder(I, I + OffBands);
    }
    else
    {
        modeProbs(f);
        vectorProbs(f);
        S32(I + OffLastMode, 0);
    }
    coefProbs(f, inter);
    const uint32_t scan = L32(L32(I + OffQuant) + 0xc);
    for (uint32_t i = 0; i < 64; ++i)
        S8(I + OffScan + i, L8(scan + 4 * L8(I + OffBandOrder + i)));
    if (f.huffman)
        huffmanTables(I);
    if (L32(I + OffFieldFlag) == 1)
        S32(I + OffFieldProb, f.a.literal(8) & 0xff);
    resetFrameContexts(I);
    memset(P(L32(I + OffCoefs)), 0, 0x300);
    memset(P(I + OffZeroRuns), 0, 16);
}

// 0x881ae3 in parts: border extension of a new frame (struct 0x880df3: plane origins, size in blocks, strides,
// border). The parts of the planes they read and write are disjoint, so any order with sides before the copies of
// the first and last rows gives the same bytes.
struct Borders
{
    uint8_t* frame;
    uint32_t origin[3];     // luma, U, V
    uint32_t bw, bh, ys, us, b;

    void load(uint32_t border, uint32_t to)
    {
        frame = P(to);
        for (uint32_t i = 0; i < 3; ++i)
            origin[i] = L32(border + 4 * i);
        bw = L32(border + 0xc);
        bh = L32(border + 0x10);
        ys = L32(border + 0x14);
        us = L32(border + 0x18);
        b = L32(border + 0x1c);
    }

    // Left and right columns of luma rows [y0, y1) and chroma rows [y0 / 2, y1 / 2), clamped to the planes.
    void sides(uint32_t y0, uint32_t y1) const
    {
        uint8_t* row = frame + origin[0] + y0 * ys;
        for (uint32_t y = y0; y < y1 && y < bh * 8; ++y, row += ys)
        {
            memset(row - b, row[0], b);
            memset(row + bw * 8, row[bw * 8 - 1], b);
        }
        const uint32_t hb = b >> 1, width = (b >> 3) * 4 + (hb & 3);
        for (uint32_t plane = 1; plane < 3; ++plane)
        {
            row = frame + origin[plane] + (y0 >> 1) * us;
            for (uint32_t y = y0 >> 1; y < (y1 >> 1) && y < bh * 4; ++y, row += us)
            {
                memset(row - hb, row[0], width);
                memset(row + bw * 4, row[bw * 4 - 1], width);
            }
        }
    }

    // The rows above (top) or below the planes from their first or last row.
    void copies(bool top) const
    {
        const uint8_t* first = frame + ys * b;
        const uint8_t* last = first + (bh * 8 - 1) * ys;
        for (uint32_t y = 0; y < b; ++y)
        {
            if (top)
                memcpy(frame + y * ys, first, ys);
            else
                memcpy(const_cast<uint8_t*>(last) + ys + y * ys, last, ys);
        }
        const uint32_t hb = b >> 1;
        for (uint32_t plane = 1; plane < 3; ++plane)
        {
            uint8_t* firstRow = frame + origin[plane] - hb;
            const uint8_t* bottom = firstRow + (bh * 4 - 1) * us;
            uint8_t* above = firstRow - hb * us;
            for (uint32_t y = 0; y < hb; ++y)
            {
                if (top)
                    memcpy(above + y * us, firstRow, us);
                else
                    memcpy(const_cast<uint8_t*>(bottom) + us + y * us, bottom, us);
            }
        }
    }

    void all() const
    {
        sides(0, bh * 8);
        copies(true);
        copies(false);
    }
};

// 0x57dba0: one row of 32-bit pixels from 4:2:0 planes, two pixels per step through the Y, U and V tables (four
// words each), added with wrapping and saturated to bytes.
void convertRow(const uint8_t* y, const uint8_t* u, const uint8_t* v, uint8_t* dst, const uint8_t* end)
{
    const int16_t* ty = reinterpret_cast<const int16_t*>(P(ConvertTables));
    const int16_t* tu = ty + 0x400;
    const int16_t* tv = ty + 0x800;
    while (dst != end)
    {
        const int16x4_t c = vadd_s16(vld1_s16(tu + 4 * *u++), vld1_s16(tv + 4 * *v++));
        const int16x4_t p0 = vadd_s16(vld1_s16(ty + 4 * y[0]), c);
        const int16x4_t p1 = vadd_s16(vld1_s16(ty + 4 * y[1]), c);
        y += 2;
        vst1_u8(dst, vqmovun_s16(vcombine_s16(p0, p1)));
        dst += 8;
    }
}

} // namespace

uint32_t convertFrame(const uint32_t args[9], bool& rowsWritten)
{
    Rows rows;
    const uint32_t result = prepareConversion(args, rows, rowsWritten);
    if (rowsWritten)
        convertRows(rows);
    return result;
}

uint32_t prepareConversion(const uint32_t args[9], Rows& rows, bool& convert)
{
    convert = false;
    const int32_t height = int32_t(args[5]);
    if (args[3] & 1)
        return 2;
    if (args[7] & 7)
        return 3;
    uint32_t dst = args[6];
    int32_t width = int32_t(args[4]);
    if (dst & 7)
    {
        if (dst & 3)
            return 4;
        dst += 4;
        --width;
    }
    width &= ~1;
    if (width < 2 || height < 1)
        return 0;
    // 0x57dee0 with order 0: swaps the first two words of the first 255 entries of each table when the tables are
    // in the other order.
    if (L32(ConvertOrderFlag) != 0)
    {
        for (uint32_t e = 0; e < 0x7f8; e += 8)
        {
            for (uint32_t t = 0; t < 3; ++t)
            {
                const uint32_t a = ConvertTables + 0x800 * t + e;
                const uint32_t w0 = L16(a);
                S16(a, L16(a + 2));
                S16(a + 2, w0);
            }
        }
        S32(ConvertOrderFlag, 0);
    }
    rows = Rows{ args[0], args[1], args[2], args[3], dst, args[7], uint32_t(width), uint32_t(height) };
    convert = true;
    return 0;
}

void convertRows(const Rows& rows)
{
    const uint32_t rowBytes = rows.width << 2;
    const uint32_t vOffset = rows.v - rows.u;
    uint32_t y = rows.y, u = rows.u, dst = rows.destination;
    for (uint32_t row = 0; row < rows.height; ++row)
    {
        convertRow(P(y), P(u), P(u + vOffset), P(dst), P(dst + rowBytes));
        y += rows.yStride;
        if (row & 1)
            u += uint32_t(int32_t(rows.yStride) >> 1);
        dst += rows.pitch;
    }
}

void setMemoryBase(uintptr_t base)
{
    s_base = base;
}

bool headerBegin(uint32_t I, uint32_t data, uint32_t& quantiser)
{
    RawReader r;
    r.init(data);
    S8(I + OffInterFrame, r.read(1));
    quantiser = r.read(6) & 0xff;
    S32(I + OffFilterFlag, r.read(1) & 0xff);
    bool resize = false;
    if (L8(I + OffInterFrame) == 0)
    {
        r.read(5);
        r.read(2);
        S32(I + OffFieldFlag, r.read(1) & 0xff);
        boolInit(I + OffBoolA, r.buffer + 4);
        S32(I + OffPartition, r.read(16));
        BoolDecoder a;
        a.load(I + OffBoolA);
        const uint32_t rows = (a.literal(8) & 0xff) << 1;
        const uint32_t cols = (a.literal(8) & 0xff) << 1;
        a.literal(8);
        a.literal(8);
        a.literal(2);
        a.store(I + OffBoolA);
        if (rows != L32(I + 0x174) || cols != L32(I + OffBlocksX))
        {
            S32(I + InstWidth, cols << 3);
            S32(I + InstHeight, rows << 3);
            resize = true;
        }
    }
    else
    {
        boolInit(I + OffBoolA, r.buffer + 3);
        S32(I + OffPartition, r.read(16));
        BoolDecoder a;
        a.load(I + OffBoolA);
        S32(I + OffGoldenRefresh, a.bit(0x80));
        a.store(I + OffBoolA);
    }
    r.store(I + OffRaw);
    return resize;
}

void headerEnd(uint32_t I, uint32_t index)
{
    BoolDecoder a;
    a.load(I + OffBoolA);
    S32(I + OffHuffman, a.bit(0x80));
    a.store(I + OffBoolA);
    // 0x880f64: dequantisation tables when the quantiser changed.
    const uint32_t q = L32(I + OffQuant);
    S32(q, index);
    S32(q + 4, L32(TabQuantAc + 4 * index));
    if (L32(TabQuantAc + 4 * index) != L32(q + 8))
    {
        S32(q + 8, L32(q + 4));
        // 0x88317f: MMX coefficient order and its inverse.
        S32(q + 0xc, TabScanMmx);
        for (uint32_t i = 0; i < 64; ++i)
            S8(q + 0x10 + L32(TabScanMmx + 4 * L32(TabScanNatural + 4 * i)), i);
        // 0x880eea.
        const uint32_t y = L32(q + 0x50), uv = L32(q + 0x54);
        for (uint32_t i = 1; i < 64; ++i)
            S16(y + 2 * L8(q + 0x10 + i), uint32_t(L16(TabQuantAc + 4 * index)) << 2);
        S16(y, uint32_t(L16(TabQuantDc + 2 * index)) << 2);
        for (uint32_t i = 1; i < 64; ++i)
            S16(uv + 2 * L8(q + 0x10 + i), uint32_t(L16(TabQuantAcUv + 4 * index)) << 2);
        S16(uv, uint32_t(L16(TabQuantDcUv + 2 * index)) << 2);
    }
}

namespace
{

// The frame being decoded: decode stage state and the ring of recorded macroblock rows.
struct Body
{
    Frame f;
    uint32_t rows;              // macroblock rows of the picture (the guest's rows 3 .. rows - 4)
    uint32_t cols;              // macroblocks per row
    RowSlot slots[RingRows];
    const int16_t* quant[2];    // luma and chroma dequantisation tables
    Borders borders;            // of the new frame
    bool rowBorders;            // reconstructRow extends the borders of its rows
};

Body s_body;

inline RowSlot& rowSlot(uint32_t row)
{
    return s_body.slots[row % RingRows];
}

}

uint32_t beginBody(uint32_t I, uint32_t data, uint32_t argumentSlot, bool& independent)
{
    if (!s_tables.ready)
        loadTables();
    Frame& f = s_body.f;
    f.I = I;
    f.predicted = false;
    const uint32_t coefData = L32(I + OffPartition) + data;
    f.huffman = L32(I + OffHuffman) != 0;
    if (f.huffman)
    {
        S32(I + OffHuff, 0);
        S32(I + OffHuff + 4, 0);
        S32(I + OffHuff + 8, coefData);
    }
    else
        boolInit(I + OffBoolB, coefData);
    f.a.load(I + OffBoolA);
    f.b.load(I + OffBoolB);
    f.h.load(I + OffHuff);
    setupFrame(f, argumentSlot);
    memset(f.coefs, 0, sizeof(f.coefs));
    memset(f.live, 0, sizeof(f.live));
    memset(f.zeroRuns, 0, sizeof(f.zeroRuns));
    memcpy(f.scan, P(I + OffScan), sizeof(f.scan));
    memcpy(f.eobClass, P(I + 0x263), sizeof(f.eobClass));
    const uint32_t q = L32(I + OffQuant);
    s_body.quant[0] = reinterpret_cast<const int16_t*>(P(L32(q + 0x50)));
    s_body.quant[1] = reinterpret_cast<const int16_t*>(P(L32(q + 0x54)));
    const uint32_t rows = L32(I + OffMbRows), cols = L32(I + OffMbCols);
    s_body.rows = rows > 6 ? rows - 6 : 0;
    s_body.cols = cols > 6 ? cols - 6 : 0;
    for (RowSlot& slot : s_body.slots)
    {
        if (slot.jobs.size() < s_body.cols * 6)
            slot.jobs.resize(s_body.cols * 6);
        slot.used = 0;
    }
    // Reconstruction reads only the previous and golden frames: any block order gives the same pixels unless the
    // new frame is one of them.
    const uint32_t frame = L32(I + OffNewFrame);
    independent = frame != L32(I + OffPrevious) && frame != L32(I + OffGolden);
    // Then each row can also extend the borders of its pixel rows, if the last one holds the planes' last rows.
    Borders& borders = s_body.borders;
    borders.load(L32(I + OffBorder), frame);
    s_body.rowBorders = independent && s_body.rows != 0 && (s_body.rows - 1) * 16 < borders.bh * 8;
    return s_body.rows;
}

void decodeRow(uint32_t row)
{
    Frame& f = s_body.f;
    resetRowContexts(f.I);
    RowSlot& slot = rowSlot(row);
    slot.used = 0;
    BlockJob* jobs = slot.jobs.data();
    for (uint32_t col = 0; col < s_body.cols; ++col)
        decodeMacroblock(f, row + 3, col + 3, jobs + 6 * col, slot);
}

void reconstructRow(uint32_t row)
{
    const RowSlot& slot = rowSlot(row);
    const BlockJob* jobs = slot.jobs.data();
    const uint32_t* entries = slot.entries.data();
    for (uint32_t i = 0; i < s_body.cols * 6; ++i)
        reconstructBlock(jobs[i], entries + jobs[i].first, s_body.quant);
    if (!s_body.rowBorders)
        return;
    const bool last = row + 1 == s_body.rows;
    s_body.borders.sides(row * 16, last ? s_body.borders.bh * 8 : row * 16 + 16);
    if (row == 0)
        s_body.borders.copies(true);
    if (last)
        s_body.borders.copies(false);
}

void endBody()
{
    Frame& f = s_body.f;
    const uint32_t I = f.I;
    // What the guest leaves in its coefficient blocks, zero run counters, and residual and prediction buffers: the
    // last block's inverse transform and the last motion compensated prediction.
    memcpy(P(L32(I + OffCoefs)), f.coefs, sizeof(f.coefs));
    memcpy(P(I + OffZeroRuns), f.zeroRuns, sizeof(f.zeroRuns));
    if (s_body.rows != 0 && s_body.cols != 0)
    {
        const RowSlot& slot = rowSlot(s_body.rows - 1);
        const BlockJob& last = slot.jobs[s_body.cols * 6 - 1];
        runTransform(last, slot.entries.data() + last.first, s_body.quant[last.chroma],
                     reinterpret_cast<int16_t*>(P(L32(I + OffResidual))));
    }
    if (f.predicted)
        runPrediction(f.lastPrediction, reinterpret_cast<int16_t*>(P(L32(I + OffPrediction))));
    f.a.store(I + OffBoolA);
    if (!f.huffman)
        f.b.store(I + OffBoolB);
    else
        f.h.store(I + OffHuff);
    // Frame rotation (0x87f85f).
    const uint32_t previous = L32(I + OffPrevious);
    S32(I + OffPrevious, L32(I + OffNewFrame));
    S32(I + OffNewFrame, L32(I + OffSpare) != 0 ? L32(I + OffSpare) : previous);
    S32(I + OffSpare, 0);
    if (!s_body.rowBorders)
    {
        s_body.borders.load(L32(I + OffBorder), L32(I + OffPrevious));
        s_body.borders.all();
    }
    if (L8(I + OffInterFrame) == 0 || L32(I + OffGoldenRefresh) != 0)
    {
        S32(I + OffSpare, L32(I + OffGolden));
        S32(I + OffGolden, L32(I + OffPrevious));
    }
}

void decodeBody(uint32_t instance, uint32_t data, uint32_t argumentSlot)
{
    bool independent;
    const uint32_t rows = beginBody(instance, data, argumentSlot, independent);
    for (uint32_t row = 0; row < rows; ++row)
    {
        decodeRow(row);
        reconstructRow(row);
    }
    endBody();
}

}
}
