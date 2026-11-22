#include <winapi/ole32.h>
#include <platform/time.h>
#include <x86.h>
#include <cstdio>
#include <cstring>

namespace win32 { namespace ole32
{

// A random (version 4) GUID from a generator seeded with the clock.
HRESULT CoCreateGuid(WinApplication* app, x86::CPU& cpu, GUID *pguid)
{
    RC_USE(app);
    RC_USE(cpu);
    if (!pguid)
        return 0x80070057;  // E_INVALIDARG
    static x86::reg64 s_state = 0;
    if (!s_state)
        s_state = pf::ticksUs() * 0x9e3779b97f4a7c15ull | 1;
    x86::reg8 b[16];
    for (x86::reg8& byte : b)
    {
        s_state ^= s_state << 13;
        s_state ^= s_state >> 7;
        s_state ^= s_state << 17;
        byte = x86::reg8(s_state >> 24);
    }
    b[7] = x86::reg8((b[7] & 0x0f) | 0x40);
    b[8] = x86::reg8((b[8] & 0x3f) | 0x80);
    memcpy(pguid, b, 16);
    return 0;
}

// "{xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx}" in UTF-16; returns the length with the terminator, 0 if it does not fit.
int StringFromGUID2(WinApplication* app, x86::CPU& cpu, const GUID* rguid, LPWSTR lpsz, int cchMax)
{
    RC_USE(app);
    RC_USE(cpu);
    if (!rguid || !lpsz)
        return 0;
    const x86::reg8* b = reinterpret_cast<const x86::reg8*>(rguid);
    char text[40];
    snprintf(text, sizeof(text), "{%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
             b[3], b[2], b[1], b[0], b[5], b[4], b[7], b[6], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
    const int length = 39;
    if (cchMax < length)
        return 0;
    for (int i = 0; i < length; ++i)
        lpsz[i] = WCHAR(x86::reg8(text[i]));
    return length;
}

}}
