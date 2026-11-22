#ifndef rpcrt4_H_
#define rpcrt4_H_

#include <x86.h>
#include <winapi/types.h>

namespace win32 { class WinApplication; }

// RPC runtime: UUID strings.
namespace win32 { namespace rpcrt4
{

LONG UuidToStringA(WinApplication* app, x86::CPU& cpu, const GUID* Uuid, x86::reg32* StringUuid);
LONG RpcStringFreeA(WinApplication* app, x86::CPU& cpu, x86::reg32* String);

}}

#endif
