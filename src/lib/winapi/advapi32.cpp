#include <winapi/advapi32.h>
#include <winapi/support.h>
#include <lib/heap.h>
#include <lib/registry.h>
#include <platform/log.h>
#include <platform/time.h>
#include <x86.h>

namespace win32 { namespace advapi32
{

static const LSTATUS ERROR_SUCCESS = 0;
static const LSTATUS ERROR_FILE_NOT_FOUND = 2;
static const LSTATUS ERROR_ACCESS_DENIED = 5;
static const LSTATUS ERROR_INVALID_HANDLE = 6;
static const DWORD REG_CREATED_NEW_KEY = 1;
static const DWORD REG_OPENED_EXISTING_KEY = 2;
static const REGSAM KEY_SET_VALUE = 0x0002;
static const REGSAM MAXIMUM_ALLOWED = 0x02000000;
static const REGSAM GENERIC_ALL = 0x10000000;
static const REGSAM GENERIC_WRITE = 0x40000000;

// The user is an administrator, so every key grants what is asked; values can be set through a handle whose access
// mask includes setting them (KEY_SET_VALUE, as in KEY_WRITE and KEY_ALL_ACCESS, or a generic right covering it).
static bool canSetValues(REGSAM samDesired)
{
    return (samDesired & (KEY_SET_VALUE | MAXIMUM_ALLOWED | GENERIC_ALL | GENERIC_WRITE)) != 0;
}

// The key of a handle: a predefined root or an opened key.
static registry::Key* keyOf(WinApplication* app, HKEY hKey)
{
    if (registry::Key* root = registry::root(hKey))
        return root;
    RegistryKey* key = dynamic_cast<RegistryKey*>(app->getResource(hKey));
    return key ? key->key() : nullptr;
}

// Whether values can be set through a handle: the predefined roots have full access.
static bool writableOf(WinApplication* app, HKEY hKey)
{
    if (registry::root(hKey))
        return true;
    RegistryKey* key = dynamic_cast<RegistryKey*>(app->getResource(hKey));
    return key && key->writable();
}

static LSTATUS open(WinApplication* app, HKEY hKey, LPCSTR lpSubKey, bool create, REGSAM samDesired,
                    PHKEY phkResult, LPDWORD lpdwDisposition)
{
    registry::Key* parent = keyOf(app, hKey);
    if (!parent || !phkResult)
        return ERROR_INVALID_HANDLE;
    registry::Key* key = registry::find(parent, lpSubKey, false);
    if (lpdwDisposition)
        *lpdwDisposition = key ? REG_OPENED_EXISTING_KEY : REG_CREATED_NEW_KEY;
    if (!key && create)
        key = registry::find(parent, lpSubKey, true);
    if (!key)
        return ERROR_FILE_NOT_FOUND;
    *phkResult = app->allocateResource(new RegistryKey(key, canSetValues(samDesired)));
    return ERROR_SUCCESS;
}

LSTATUS RegCloseKey(WinApplication* app, x86::CPU& cpu,
                    HKEY hKey)
{
    RC_USE(cpu);
    if (registry::root(hKey))
        return ERROR_SUCCESS;
    return dynamic_cast<RegistryKey*>(app->getResource(hKey)) && app->freeResource(hKey) ? ERROR_SUCCESS
                                                                                       : ERROR_INVALID_HANDLE;
}

LSTATUS RegCreateKeyExA(WinApplication* app, x86::CPU& cpu,
                        HKEY hKey, LPCSTR lpSubKey, DWORD Reserved, LPSTR lpClass, DWORD dwOptions,
                        REGSAM samDesired, LPSECURITY_ATTRIBUTES lpSecurityAttributes, PHKEY phkResult,
                        LPDWORD lpdwDisposition)
{
    RC_USE(cpu);
    RC_USE(Reserved);
    RC_USE(lpClass);
    RC_USE(dwOptions);
    RC_USE(lpSecurityAttributes);
    return open(app, hKey, lpSubKey, true, samDesired, phkResult, lpdwDisposition);
}

LSTATUS RegFlushKey(WinApplication* app, x86::CPU& cpu,
                    HKEY hKey)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(hKey);
    return ERROR_SUCCESS;
}

LSTATUS RegOpenKeyExA(WinApplication* app, x86::CPU& cpu,
                      HKEY hKey, LPCSTR lpSubKey, DWORD ulOptions, REGSAM samDesired, PHKEY phkResult)
{
    RC_USE(cpu);
    RC_USE(ulOptions);
    return open(app, hKey, lpSubKey, false, samDesired, phkResult, nullptr);
}

// The Windows 3.1 form: opens (never creates) with full access.
LSTATUS RegOpenKeyA(WinApplication* app, x86::CPU& cpu,
                    HKEY hKey, LPCSTR lpSubKey, PHKEY phkResult)
{
    RC_USE(cpu);
    return open(app, hKey, lpSubKey, false, MAXIMUM_ALLOWED, phkResult, nullptr);
}

// A null name is the key's default value.
LSTATUS RegQueryValueExA(WinApplication* app, x86::CPU& cpu,
                         HKEY hKey, LPCSTR lpValueName, LPDWORD lpReserved, LPDWORD lpType, LPBYTE lpData, LPDWORD lpcbData)
{
    RC_USE(cpu);
    RC_USE(lpReserved);
    registry::Key* key = keyOf(app, hKey);
    if (!key)
        return ERROR_INVALID_HANDLE;
    const LSTATUS result = LSTATUS(registry::query(key, lpValueName ? lpValueName : "", lpType, lpData, lpcbData));
    if (result == ERROR_FILE_NOT_FOUND)
        pf::logInfo("RegQueryValueExA: no value %s", lpValueName ? lpValueName : "(default)");
    return result;
}

// A handle opened for reading only (KEY_READ) cannot set values.
LSTATUS RegSetValueExA(WinApplication* app, x86::CPU& cpu,
                       HKEY hKey, LPCSTR lpValueName, DWORD Reserved, DWORD dwType, const BYTE* lpData,
                       DWORD cbData)
{
    RC_USE(cpu);
    RC_USE(Reserved);
    registry::Key* key = keyOf(app, hKey);
    if (!key)
        return ERROR_INVALID_HANDLE;
    if (!writableOf(app, hKey))
        return ERROR_ACCESS_DENIED;
    registry::setValue(key, lpValueName ? lpValueName : "", dwType, lpData, lpData ? cbData : 0);
    return ERROR_SUCCESS;
}

LSTATUS RegCreateKeyA(WinApplication* app, x86::CPU& cpu, HKEY hKey, LPCSTR lpSubKey, PHKEY phkResult)
{
    RC_USE(cpu);
    return open(app, hKey, lpSubKey, true, MAXIMUM_ALLOWED, phkResult, nullptr);
}

// A SID in guest memory: revision 1, the sub-authority count, the 6-byte identifier authority, the sub-authorities.
BOOL AllocateAndInitializeSid(WinApplication* app, x86::CPU& cpu, const BYTE* pIdentifierAuthority,
                              BYTE nSubAuthorityCount, DWORD nSubAuthority0, DWORD nSubAuthority1,
                              DWORD nSubAuthority2, DWORD nSubAuthority3, DWORD nSubAuthority4,
                              DWORD nSubAuthority5, DWORD nSubAuthority6, DWORD nSubAuthority7, x86::reg32* pSid)
{
    if (!pIdentifierAuthority || !pSid || nSubAuthorityCount > 8)
    {
        setLastError(app, cpu, error::InvalidParameter);
        return 0;
    }
    const x86::reg32 sid = heap::allocate(8 + 4 * x86::reg32(nSubAuthorityCount), true);
    if (!sid)
    {
        setLastError(app, cpu, 8);  // ERROR_NOT_ENOUGH_MEMORY
        return 0;
    }
    const DWORD sub[8] = { nSubAuthority0, nSubAuthority1, nSubAuthority2, nSubAuthority3, nSubAuthority4,
                           nSubAuthority5, nSubAuthority6, nSubAuthority7 };
    app->getMemory<BYTE>(sid) = 1;
    app->getMemory<BYTE>(sid + 1) = nSubAuthorityCount;
    for (x86::reg32 i = 0; i < 6; ++i)
        app->getMemory<BYTE>(sid + 2 + i) = pIdentifierAuthority[i];
    for (x86::reg32 i = 0; i < nSubAuthorityCount; ++i)
        app->getMemory<DWORD>(sid + 8 + 4 * i) = sub[i];
    *pSid = sid;
    return 1;
}

x86::reg32 FreeSid(WinApplication* app, x86::CPU& cpu, x86::reg32 pSid)
{
    RC_USE(app);
    RC_USE(cpu);
    if (pSid)
        heap::release(pSid);
    return 0;
}

// The user is an administrator, as the default account of Windows XP: every SID checked is one of the token's.
BOOL CheckTokenMembership(WinApplication* app, x86::CPU& cpu, HANDLE TokenHandle, x86::reg32 SidToCheck,
                          BOOL* IsMember)
{
    RC_USE(TokenHandle);
    if (!SidToCheck || !IsMember)
    {
        setLastError(app, cpu, error::InvalidParameter);
        return 0;
    }
    *IsMember = 1;
    return 1;
}

// Providers: one handle; the random bytes come from a generator seeded with the clock (not cryptographic).
BOOL CryptAcquireContextA(WinApplication* app, x86::CPU& cpu, x86::reg32* phProv, LPCSTR szContainer,
                          LPCSTR szProvider, DWORD dwProvType, DWORD dwFlags)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(szContainer);
    RC_USE(szProvider);
    RC_USE(dwProvType);
    RC_USE(dwFlags);
    if (!phProv)
        return 0;
    *phProv = 1;
    return 1;
}

BOOL CryptGenRandom(WinApplication* app, x86::CPU& cpu, HANDLE hProv, DWORD dwLen, BYTE* pbBuffer)
{
    RC_USE(app);
    RC_USE(cpu);
    static x86::reg64 s_state = 0;
    if (hProv != 1 || (!pbBuffer && dwLen))
        return 0;
    if (!s_state)
        s_state = pf::ticksUs() * 0x9e3779b97f4a7c15ull | 1;
    for (DWORD i = 0; i < dwLen; ++i)
    {
        s_state ^= s_state << 13;
        s_state ^= s_state >> 7;
        s_state ^= s_state << 17;
        pbBuffer[i] = BYTE(s_state >> 24);
    }
    return 1;
}

BOOL CryptReleaseContext(WinApplication* app, x86::CPU& cpu, HANDLE hProv, DWORD dwFlags)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(dwFlags);
    return hProv == 1 ? 1 : 0;
}

}}
