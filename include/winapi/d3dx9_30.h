#ifndef d3dx9_30_H_
#define d3dx9_30_H_

// d3dx9_30.dll imports of NFSC.exe (stdcall). The math functions compute in single precision in the order of the
// D3DX reference formulas; effects are parsed from the compiled fx_2_0 resources of the executable; textures load
// from DDS data in memory. Direct3D objects are guest COM pointers (winapi/d3d9_internal.h).

#include <x86.h>
#include <winapi/types.h>

namespace win32 { class WinApplication; }

namespace win32 { namespace d3dx9_30
{

x86::reg32 D3DXMatrixMultiply(WinApplication* app, x86::CPU& cpu, x86::reg32 out, x86::reg32 a, x86::reg32 b);
x86::reg32 D3DXMatrixTranspose(WinApplication* app, x86::CPU& cpu, x86::reg32 out, x86::reg32 m);
x86::reg32 D3DXMatrixInverse(WinApplication* app, x86::CPU& cpu, x86::reg32 out, x86::reg32 determinant,
                             x86::reg32 m);
x86::reg32 D3DXMatrixTranslation(WinApplication* app, x86::CPU& cpu, x86::reg32 out, float x, float y, float z);
x86::reg32 D3DXMatrixPerspectiveLH(WinApplication* app, x86::CPU& cpu, x86::reg32 out, float w, float h, float zn,
                                   float zf);
x86::reg32 D3DXMatrixOrthoLH(WinApplication* app, x86::CPU& cpu, x86::reg32 out, float w, float h, float zn,
                             float zf);
x86::reg32 D3DXVec3Normalize(WinApplication* app, x86::CPU& cpu, x86::reg32 out, x86::reg32 v);
x86::reg32 D3DXVec3Transform(WinApplication* app, x86::CPU& cpu, x86::reg32 out, x86::reg32 v, x86::reg32 m);
x86::reg32 D3DXVec3TransformNormal(WinApplication* app, x86::CPU& cpu, x86::reg32 out, x86::reg32 v, x86::reg32 m);
x86::reg32 D3DXVec3TransformCoordArray(WinApplication* app, x86::CPU& cpu, x86::reg32 out, UINT outStride,
                                       x86::reg32 v, UINT vStride, x86::reg32 m, UINT n);
x86::reg32 D3DXVec4Transform(WinApplication* app, x86::CPU& cpu, x86::reg32 out, x86::reg32 v, x86::reg32 m);

HRESULT D3DXCreateEffectPool(WinApplication* app, x86::CPU& cpu, x86::reg32* pool);
HRESULT D3DXCreateEffectFromResourceA(WinApplication* app, x86::CPU& cpu, x86::reg32 device, HMODULE module,
                                      x86::reg32 resource, x86::reg32 defines, x86::reg32 include, DWORD flags,
                                      x86::reg32 pool, x86::reg32* effect, x86::reg32* errors);

HRESULT D3DXCreateCubeTextureFromFileInMemory(WinApplication* app, x86::CPU& cpu, x86::reg32 device, x86::reg32 data,
                                              UINT size, x86::reg32* texture);
HRESULT D3DXCreateVolumeTextureFromFileInMemory(WinApplication* app, x86::CPU& cpu, x86::reg32 device,
                                                x86::reg32 data, UINT size, x86::reg32* texture);
HRESULT D3DXFillVolumeTexture(WinApplication* app, x86::CPU& cpu, x86::reg32 texture, x86::reg32 function,
                              x86::reg32 data);
HRESULT D3DXLoadSurfaceFromSurface(WinApplication* app, x86::CPU& cpu, x86::reg32 dst, x86::reg32 dstPalette,
                                   const RECT* dstRect, x86::reg32 src, x86::reg32 srcPalette, const RECT* srcRect,
                                   DWORD filter, DWORD colorKey);
HRESULT D3DXSaveSurfaceToFileA(WinApplication* app, x86::CPU& cpu, LPCSTR file, DWORD format, x86::reg32 surface,
                               x86::reg32 palette, const RECT* rect);
HRESULT D3DXSaveSurfaceToFileInMemory(WinApplication* app, x86::CPU& cpu, x86::reg32* buffer, DWORD format,
                                      x86::reg32 surface, x86::reg32 palette, const RECT* rect);

}}

#endif
