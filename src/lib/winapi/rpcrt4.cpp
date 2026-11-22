#include <winapi/rpcrt4.h>
#include <lib/heap.h>
#include <lib/winapp.h>
#include <cstdio>

namespace win32 { namespace rpcrt4
{

static const LONG RPC_S_OK = 0;
static const LONG RPC_S_OUT_OF_MEMORY = 14;
static const LONG RPC_S_INVALID_ARG = 87;

// "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx" in lower case, in a block RpcStringFreeA releases.
LONG UuidToStringA(WinApplication* app, x86::CPU& cpu, const GUID* Uuid, x86::reg32* StringUuid)
{
    RC_USE(cpu);
    if (!StringUuid)
        return RPC_S_INVALID_ARG;
    static const x86::reg8 zero[16] = {};
    const x86::reg8* b = Uuid ? reinterpret_cast<const x86::reg8*>(Uuid) : zero;
    const x86::reg32 text = heap::allocate(37, false);
    if (!text)
        return RPC_S_OUT_OF_MEMORY;
    snprintf(&app->getMemory<char>(text), 37, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             b[3], b[2], b[1], b[0], b[5], b[4], b[7], b[6], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
    *StringUuid = text;
    return RPC_S_OK;
}

LONG RpcStringFreeA(WinApplication* app, x86::CPU& cpu, x86::reg32* String)
{
    RC_USE(app);
    RC_USE(cpu);
    if (!String)
        return RPC_S_INVALID_ARG;
    if (*String)
        heap::release(*String);
    *String = 0;
    return RPC_S_OK;
}

}}
