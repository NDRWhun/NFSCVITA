// Every import of NFSC.exe bound the way the generated registration binds it (win32::<dll>::<name> behind
// win32::Wrapper), so a missing or mismatched shim fails the runtime build and not only the game build. The imported
// variables of msvcr71 (_iob, _adjust_fdiv) are bound by address and are not listed.
#include <lib/imports.h>
#include <winapi/wrapper.h>
#include <winapi/advapi32.h>
#include <winapi/d3d9.h>
#include <winapi/d3dx9_30.h>
#include <winapi/dinput8.h>
#include <winapi/dsound.h>
#include <winapi/kernel32.h>
#include <winapi/msvcp71.h>
#include <winapi/msvcr71.h>
#include <winapi/netapi32.h>
#include <winapi/ole32.h>
#include <winapi/rpcrt4.h>
#include <winapi/shfolder.h>
#include <winapi/user32.h>
#include <winapi/winmm.h>
#include <winapi/ws2_32.h>

// Import table order of NFSC.exe; decorated C++ names by their names in tools/recompiler/disasm/writer.py.
#define NFSC_IMPORTS(X) \
    X(advapi32, CheckTokenMembership) X(advapi32, FreeSid) X(advapi32, RegCreateKeyA) X(advapi32, RegOpenKeyExA) \
    X(advapi32, RegCloseKey) X(advapi32, RegSetValueExA) X(advapi32, RegQueryValueExA) \
    X(advapi32, AllocateAndInitializeSid) X(advapi32, CryptReleaseContext) X(advapi32, CryptGenRandom) \
    X(advapi32, CryptAcquireContextA) X(dinput8, DirectInput8Create) X(dsound, DirectSoundCreate) \
    X(kernel32, GetLongPathNameA) X(kernel32, GetDriveTypeA) X(kernel32, GetLogicalDrives) \
    X(kernel32, GetVersionExA) X(kernel32, FindClose) X(kernel32, FindNextFileA) X(kernel32, FindFirstFileA) \
    X(kernel32, CreateDirectoryA) X(kernel32, GetModuleHandleA) X(kernel32, Sleep) X(kernel32, GlobalMemoryStatusEx) \
    X(kernel32, Process32Next) X(kernel32, Process32First) X(kernel32, CreateToolhelp32Snapshot) \
    X(kernel32, SetErrorMode) X(kernel32, GetTickCount) X(kernel32, GetCurrentProcessId) X(kernel32, ReadFile) \
    X(kernel32, SuspendThread) X(kernel32, MultiByteToWideChar) X(kernel32, FatalAppExitA) X(kernel32, CloseHandle) \
    X(kernel32, CreateFileA) X(kernel32, RemoveDirectoryA) X(kernel32, SetEndOfFile) X(kernel32, QueueUserAPC) \
    X(kernel32, RaiseException) X(kernel32, InterlockedExchange) X(kernel32, ReleaseSemaphore) \
    X(kernel32, InterlockedDecrement) X(kernel32, CreateSemaphoreA) X(kernel32, InterlockedCompareExchange) \
    X(kernel32, InterlockedExchangeAdd) X(kernel32, DebugBreak) X(kernel32, TryEnterCriticalSection) \
    X(kernel32, CreateMutexA) X(kernel32, GetPriorityClass) X(kernel32, SetPriorityClass) X(kernel32, ExitProcess) \
    X(kernel32, ResumeThread) X(kernel32, SetThreadIdealProcessor) X(kernel32, TlsSetValue) X(kernel32, TlsGetValue) \
    X(kernel32, TlsAlloc) X(kernel32, GetSystemInfo) X(kernel32, VirtualProtect) X(kernel32, GetDiskFreeSpaceExA) \
    X(kernel32, GetFileSize) X(kernel32, MoveFileA) X(kernel32, DeleteFileA) X(kernel32, SetFilePointer) \
    X(kernel32, WriteFile) X(kernel32, GetCurrentDirectoryA) X(kernel32, CreateEventA) X(kernel32, ResetEvent) \
    X(kernel32, SetEvent) X(kernel32, CreateWaitableTimerA) X(kernel32, GetSystemTime) \
    X(kernel32, SystemTimeToFileTime) X(kernel32, WaitForSingleObject) X(kernel32, SetWaitableTimer) \
    X(kernel32, GetExitCodeThread) X(kernel32, SetThreadPriority) X(kernel32, GetThreadPriority) \
    X(kernel32, SleepEx) X(kernel32, TerminateThread) X(kernel32, GetCurrentThreadId) X(kernel32, GetCurrentProcess) \
    X(kernel32, GetCurrentThread) X(kernel32, DuplicateHandle) X(kernel32, GlobalFree) X(kernel32, GlobalAlloc) \
    X(kernel32, CreateThread) X(kernel32, LeaveCriticalSection) X(kernel32, EnterCriticalSection) \
    X(kernel32, GetComputerNameA) X(kernel32, QueryPerformanceCounter) X(kernel32, QueryPerformanceFrequency) \
    X(kernel32, CreateProcessA) X(kernel32, GetLastError) X(kernel32, IsBadReadPtr) X(kernel32, IsDebuggerPresent) \
    X(kernel32, ReleaseMutex) X(kernel32, OutputDebugStringA) X(kernel32, DeleteCriticalSection) \
    X(kernel32, InitializeCriticalSection) X(kernel32, GetSystemDirectoryA) X(kernel32, LoadLibraryA) \
    X(kernel32, GetProcAddress) X(kernel32, FreeLibrary) X(kernel32, GetSystemTimeAsFileTime) \
    X(msvcp71, string_destroy) X(msvcp71, string_copy) X(msvcp71, string_from_chars) X(msvcr71, _mkdir) \
    X(msvcr71, _findfirst) X(msvcr71, _findnext) X(msvcr71, _findclose) X(msvcr71, getenv) \
    X(msvcr71, vswprintf_count) X(msvcr71, bsearch) X(msvcr71, _beginthreadex) X(msvcr71, _endthreadex) \
    X(msvcr71, _write) X(msvcr71, _open) X(msvcr71, _lseek) X(msvcr71, _read) X(msvcr71, _filelength) \
    X(msvcr71, _rmdir) X(msvcr71, remove) X(msvcr71, isdigit) X(msvcr71, __security_error_handler) \
    X(msvcr71, _localtime64) X(msvcr71, _CIacos) X(msvcr71, _CIasin) X(msvcr71, _CIpow) X(msvcr71, _purecall) \
    X(msvcr71, __CxxFrameHandler) X(msvcr71, qsort) X(msvcr71, memmove) X(msvcr71, sprintf) \
    X(msvcr71, exception_destroy) X(msvcr71, exception_construct) X(msvcr71, tolower) X(msvcr71, atoi) \
    X(msvcr71, strncpy) X(msvcr71, strstr) X(msvcr71, _CxxThrowException) X(msvcr71, exception_copy) \
    X(msvcr71, ceil) X(msvcr71, printf) X(msvcr71, malloc) X(msvcr71, free) X(msvcr71, floor) X(msvcr71, vfprintf) \
    X(msvcr71, strncmp) X(msvcr71, _CIcosh) X(msvcr71, _CIfmod) X(msvcr71, _CIsinh) X(msvcr71, _CItanh) \
    X(msvcr71, _control87) X(msvcr71, fclose) X(msvcr71, fopen) X(msvcr71, strchr) X(msvcr71, _isnan) \
    X(msvcr71, exit) X(msvcr71, vsprintf) X(msvcr71, longjmp) X(msvcr71, _setjmp3) X(msvcr71, isspace) \
    X(msvcr71, strncat) X(msvcr71, strcspn) X(msvcr71, _getdiskfree) X(msvcr71, fread) X(msvcr71, strerror) \
    X(msvcr71, _errno) X(msvcr71, fprintf) X(msvcr71, isprint) X(msvcr71, ungetc) X(msvcr71, fgetc) \
    X(msvcr71, _exit) X(msvcr71, fwrite) X(msvcr71, _aligned_malloc) X(msvcr71, _controlfp) X(msvcr71, _ftime) \
    X(msvcr71, strrchr) X(msvcr71, rand) X(msvcr71, strtok) X(msvcr71, _strnicmp) X(msvcr71, _strcmpi) \
    X(msvcr71, _vsnprintf) X(msvcr71, strlen) X(msvcr71, _snprintf) X(msvcr71, memset) X(msvcr71, memcmp) \
    X(msvcr71, memcpy) X(msvcr71, srand) X(msvcr71, localtime) X(msvcr71, fflush) X(msvcr71, _mbscpy) \
    X(msvcr71, calloc) X(msvcr71, strcmp) X(msvcr71, strtoul) X(msvcr71, _mbscat) X(msvcr71, sqrt) \
    X(msvcr71, sscanf) X(msvcr71, cos) X(msvcr71, raise) X(msvcr71, realloc) X(msvcr71, abs) X(msvcr71, fgetc) \
    X(msvcr71, toupper) X(msvcr71, fputc) X(msvcr71, clock) X(msvcr71, _c_exit) X(msvcr71, _XcptFilter) \
    X(msvcr71, _cexit) X(msvcr71, __p___initenv) X(msvcr71, _amsg_exit) X(msvcr71, __getmainargs) \
    X(msvcr71, _initterm) X(msvcr71, __setusermatherr) X(msvcr71, __p__commode) X(msvcr71, __p__fmode) \
    X(msvcr71, __set_app_type) X(msvcr71, _except_handler3) X(msvcr71, terminate) X(msvcr71, __dllonexit) \
    X(msvcr71, _onexit) X(msvcr71, type_info_destroy) X(msvcr71, _getdrive) X(msvcr71, _fullpath) \
    X(msvcr71, strcoll) X(msvcr71, _getcwd) X(msvcr71, _aligned_realloc) X(msvcr71, _aligned_free) \
    X(msvcr71, _mktime64) X(msvcr71, _gmtime64) X(msvcr71, wcslen) X(msvcr71, _time64) X(msvcr71, wcscpy) \
    X(msvcr71, time) X(msvcr71, mktime) X(msvcr71, gmtime) X(msvcr71, strtol) X(msvcr71, _close) \
    X(netapi32, Netbios) X(rpcrt4, UuidToStringA) X(rpcrt4, RpcStringFreeA) X(shfolder, SHGetFolderPathA) \
    X(user32, SetWindowPos) X(user32, SetWindowLongA) X(user32, DestroyWindow) X(user32, DefWindowProcA) \
    X(user32, BeginPaint) X(user32, EndPaint) X(user32, PostMessageA) X(user32, IsIconic) X(user32, PostQuitMessage) \
    X(user32, SetCapture) X(user32, ReleaseCapture) X(user32, PeekMessageA) X(user32, ShowCursor) \
    X(user32, ShowWindow) X(user32, TranslateMessage) X(user32, DispatchMessageA) X(user32, SendInput) \
    X(user32, GetWindowRect) X(user32, GetClientRect) X(user32, MapVirtualKeyA) X(user32, ToUnicode) \
    X(user32, MessageBoxA) X(user32, LoadCursorA) X(user32, LoadIconA) X(user32, GetCursorPos) X(user32, wvsprintfA) \
    X(user32, SetFocus) X(user32, SetForegroundWindow) X(user32, SetCursor) X(user32, UpdateWindow) \
    X(user32, CreateWindowExA) X(user32, AdjustWindowRect) X(user32, GetForegroundWindow) X(user32, GetMessageA) \
    X(user32, GetDesktopWindow) X(user32, GetKeyboardLayout) X(user32, MapVirtualKeyExA) X(user32, RegisterClassExA) \
    X(user32, GetKeyState) X(winmm, timeEndPeriod) X(winmm, timeGetTime) X(winmm, timeBeginPeriod) X(ws2_32, htonl) \
    X(ws2_32, send) X(ws2_32, socket) X(ws2_32, ioctlsocket) X(ws2_32, recv) X(ws2_32, connect) \
    X(ws2_32, setsockopt) X(ws2_32, htons) X(ws2_32, WSACreateEvent) X(ws2_32, shutdown) X(ws2_32, WSACloseEvent) \
    X(ws2_32, WSASetEvent) X(ws2_32, WSARecv) X(ws2_32, WSARecvFrom) X(ws2_32, WSAResetEvent) \
    X(ws2_32, WSAWaitForMultipleEvents) X(ws2_32, WSAGetOverlappedResult) X(ws2_32, bind) X(ws2_32, listen) \
    X(ws2_32, accept) X(ws2_32, sendto) X(ws2_32, recvfrom) X(ws2_32, getpeername) X(ws2_32, getsockname) \
    X(ws2_32, gethostname) X(ws2_32, WSAIoctl) X(ws2_32, select) X(ws2_32, closesocket) X(ws2_32, getservbyport) \
    X(ws2_32, gethostbyaddr) X(ws2_32, getservbyname) X(ws2_32, inet_ntoa) X(ws2_32, gethostbyname) \
    X(ws2_32, WSAGetLastError) X(ws2_32, inet_addr) X(ws2_32, WSAStartup) X(ws2_32, WSACleanup) X(ws2_32, htonl) \
    X(ws2_32, getsockopt) X(ws2_32, htons) X(d3d9, Direct3DCreate9) X(d3dx9_30, D3DXVec3Transform) \
    X(d3dx9_30, D3DXMatrixInverse) X(d3dx9_30, D3DXCreateVolumeTextureFromFileInMemory) \
    X(d3dx9_30, D3DXCreateCubeTextureFromFileInMemory) X(d3dx9_30, D3DXLoadSurfaceFromSurface) \
    X(d3dx9_30, D3DXSaveSurfaceToFileA) X(d3dx9_30, D3DXCreateEffectPool) X(d3dx9_30, D3DXCreateEffectFromResourceA) \
    X(d3dx9_30, D3DXVec4Transform) X(d3dx9_30, D3DXMatrixPerspectiveLH) X(d3dx9_30, D3DXMatrixTranslation) \
    X(d3dx9_30, D3DXVec3TransformCoordArray) X(d3dx9_30, D3DXVec3Normalize) X(d3dx9_30, D3DXVec3TransformNormal) \
    X(d3dx9_30, D3DXSaveSurfaceToFileInMemory) X(d3dx9_30, D3DXFillVolumeTexture) X(d3dx9_30, D3DXMatrixMultiply) \
    X(d3dx9_30, D3DXMatrixOrthoLH) X(d3dx9_30, D3DXMatrixTranspose) X(ole32, CoCreateGuid)

namespace win32
{

#define NFSC_IMPORT_ENTRY(dll, name) \
    { #dll, #name, &Wrapper<decltype(&dll::name), &dll::name>::stdcall },

static const ImportEntry s_imports[] = {
    NFSC_IMPORTS(NFSC_IMPORT_ENTRY)
};

const ImportEntry* importsBegin()
{
    return s_imports;
}

const ImportEntry* importsEnd()
{
    return s_imports + sizeof(s_imports) / sizeof(s_imports[0]);
}

}
