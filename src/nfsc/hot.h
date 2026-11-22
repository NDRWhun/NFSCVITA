#ifndef NFSC_HOT_H_
#define NFSC_HOT_H_

// Cores of the native hot-function ports (hot.cpp) shared with other native code.

#include <x86.h>

namespace nfsc
{

// Box a..b transformed by the 4x4 guest matrix m (rows of floats, translation in row 3), as 0x70f0b0 computes it.
// lastA and lastB: the operands of its final compare.
void transformBox(x86::reg32 m, float a[3], float b[3], double& lastA, double& lastB);

// What 0x71b630 leaves besides its result: the operands of its last x87 compare and the plane it stopped at (1..6,
// 7 when the box is not outside any).
struct ViewBoxDetail
{
    bool    transformed;
    double  lastA, lastB;           // transformBox's final compare (transformed only)
    double  compare;                // compared with 0.0 last
    int     plane;
};

// 0x71b630 on view (its this): the box at guest addresses a..b, transformed by the matrix m first unless m is 0,
// against the six planes at [view] + 0x140. 0 outside a plane, 1 crossing one, 2 inside all.
int viewBoxTest(x86::reg32 view, x86::reg32 a, x86::reg32 b, x86::reg32 m, ViewBoxDetail* detail);

}

#endif
