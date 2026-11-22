#ifndef WINAPI_D3D9_H_
#define WINAPI_D3D9_H_

// d3d9.dll: Direct3D 9 over the frame model of platform/render.h (src/lib/winapi/d3d9*.cpp).

#include <x86.h>
#include <platform/render.h>
#include <winapi/types.h>

namespace win32
{

class WinApplication;

}

namespace win32 { namespace d3d9
{

// The IDirect3D9 object, or 0 for an SDK version other than 31 (D3D_SDK_VERSION of the 9.0 runtime).
x86::reg32 Direct3DCreate9(WinApplication* app, x86::CPU& cpu, UINT SDKVersion);

// A movie frame for the texture level locked last, whose pixels start at guest address bits with rows of pitch bytes:
// from its unlock on, the level shows the planes, converted by the GPU, instead of the locked pixels. False, with
// nothing changed, unless that level is still locked, rewritten every frame, 32-bit and at least the frame's size.
bool streamPlanes(x86::reg32 bits, uint32_t pitch, const pf::render::Planes& planes);

}}

#endif
