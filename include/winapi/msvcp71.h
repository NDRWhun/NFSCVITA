#ifndef msvcp71_H_
#define msvcp71_H_

// The C++ library 7.1 (msvcp71.dll) imports of NFSC.exe: std::basic_string<char> construction and destruction,
// thiscall (this in ecx) on the library's 28-byte layout: allocator, a 16-byte buffer or pointer, size, capacity.

#include <x86.h>
#include <winapi/types.h>

namespace win32 { class WinApplication; }

namespace win32 { namespace msvcp71
{

void string_from_chars(WinApplication* app, x86::CPU& cpu, x86::reg32 chars);
void string_copy(WinApplication* app, x86::CPU& cpu, x86::reg32 other);
void string_destroy(WinApplication* app, x86::CPU& cpu);

}}

#endif
