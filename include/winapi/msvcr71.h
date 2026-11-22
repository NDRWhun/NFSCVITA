#ifndef msvcr71_H_
#define msvcr71_H_

// The Microsoft C runtime 7.1 (msvcr71.dll) imports of NFSC.exe. cdecl functions take a Cdecl tag
// (winapi/wrapper.h) and read variadic arguments from the guest stack; methods of the C++ classes are thiscall
// (this in ecx). Guest pointers that the result may point into are passed as guest addresses.

#include <x86.h>
#include <winapi/types.h>
#include <winapi/wrapper.h>

namespace win32 { class WinApplication; }

namespace win32 { namespace msvcr71
{

// Observer of malloc blocks (site: malloc's caller, via: the next guest code address on the stack) and of frees;
// before guest code runs.
typedef void (*HeapObserver)(x86::reg32 block, x86::reg32 size, x86::reg32 site, x86::reg32 via);
void setHeapObserver(HeapObserver allocated, void (*released)(x86::reg32 block));

// Imported variables: guest addresses of _iob (the stdio FILE table) and _adjust_fdiv.
x86::reg32 data_iob(WinApplication* app);
x86::reg32 data_adjust_fdiv(WinApplication* app);

// Start-up and exit.
void __set_app_type(WinApplication* app, x86::CPU& cpu, Cdecl, int type);
x86::reg32 __p__fmode(WinApplication* app, x86::CPU& cpu, Cdecl);
x86::reg32 __p__commode(WinApplication* app, x86::CPU& cpu, Cdecl);
x86::reg32 __p___initenv(WinApplication* app, x86::CPU& cpu, Cdecl);
void __setusermatherr(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 handler);
int __getmainargs(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 argc, x86::reg32 argv, x86::reg32 envp,
                  int expandWildcards, x86::reg32 startInfo);
void _initterm(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 begin, x86::reg32 end);
x86::reg32 _onexit(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 function);
x86::reg32 __dllonexit(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 function, x86::reg32 begin,
                       x86::reg32 end);
void exit(WinApplication* app, x86::CPU& cpu, Cdecl, int status);
void _exit(WinApplication* app, x86::CPU& cpu, Cdecl, int status);
void _cexit(WinApplication* app, x86::CPU& cpu, Cdecl);
void _c_exit(WinApplication* app, x86::CPU& cpu, Cdecl);
void _amsg_exit(WinApplication* app, x86::CPU& cpu, Cdecl, int code);
int raise(WinApplication* app, x86::CPU& cpu, Cdecl, int signal);
int _purecall(WinApplication* app, x86::CPU& cpu, Cdecl);
void __security_error_handler(WinApplication* app, x86::CPU& cpu, Cdecl, int code, x86::reg32 data);
void terminate(WinApplication* app, x86::CPU& cpu, Cdecl);

// Structured and C++ exception handling: reached only through an exception dispatch.
int _except_handler3(WinApplication* app, x86::CPU& cpu, Cdecl);
int __CxxFrameHandler(WinApplication* app, x86::CPU& cpu, Cdecl);
int _XcptFilter(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 code, x86::reg32 pointers);
void _CxxThrowException(WinApplication* app, x86::CPU& cpu, x86::reg32 object, x86::reg32 throwInfo);
void exception_construct(WinApplication* app, x86::CPU& cpu);
void exception_copy(WinApplication* app, x86::CPU& cpu, x86::reg32 other);
void exception_destroy(WinApplication* app, x86::CPU& cpu);
void type_info_destroy(WinApplication* app, x86::CPU& cpu);

// setjmp/longjmp (include/recomp.h): _setjmp3 records the registers, longjmp restores them and resumes after the
// setjmp call.
int _setjmp3(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 buffer, int count);
void longjmp(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 buffer, int value);

// Threads.
x86::reg32 _beginthreadex(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 security, UINT stackSize,
                          x86::reg32 start, x86::reg32 argument, UINT flags, x86::reg32 threadId);
void _endthreadex(WinApplication* app, x86::CPU& cpu, Cdecl, UINT code);

// Memory.
x86::reg32 malloc(WinApplication* app, x86::CPU& cpu, Cdecl, UINT size);
x86::reg32 calloc(WinApplication* app, x86::CPU& cpu, Cdecl, UINT count, UINT size);
x86::reg32 realloc(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 block, UINT size);
void free(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 block);
x86::reg32 _aligned_malloc(WinApplication* app, x86::CPU& cpu, Cdecl, UINT size, UINT alignment);
x86::reg32 _aligned_realloc(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 block, UINT size, UINT alignment);
void _aligned_free(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 block);

// Memory and strings.
x86::reg32 memmove(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 dst, x86::reg32 src, UINT count);
x86::reg32 memcpy(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 dst, x86::reg32 src, UINT count);
x86::reg32 memset(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 dst, int value, UINT count);
int memcmp(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 a, x86::reg32 b, UINT count);
UINT strlen(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 s);
int strcmp(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 a, x86::reg32 b);
int strncmp(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 a, x86::reg32 b, UINT count);
int _strcmpi(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 a, x86::reg32 b);
int _strnicmp(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 a, x86::reg32 b, UINT count);
int strcoll(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 a, x86::reg32 b);
x86::reg32 strncpy(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 dst, x86::reg32 src, UINT count);
x86::reg32 strncat(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 dst, x86::reg32 src, UINT count);
x86::reg32 _mbscpy(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 dst, x86::reg32 src);
x86::reg32 _mbscat(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 dst, x86::reg32 src);
x86::reg32 strchr(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 s, int c);
x86::reg32 strrchr(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 s, int c);
x86::reg32 strstr(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 s, x86::reg32 find);
UINT strcspn(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 s, x86::reg32 reject);
x86::reg32 strtok(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 s, x86::reg32 delimiters);
x86::reg32 strerror(WinApplication* app, x86::CPU& cpu, Cdecl, int error);
UINT wcslen(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 s);
x86::reg32 wcscpy(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 dst, x86::reg32 src);
int atoi(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 s);
x86::sreg32 strtol(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 s, x86::reg32 end, int base);
UINT strtoul(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 s, x86::reg32 end, int base);
int tolower(WinApplication* app, x86::CPU& cpu, Cdecl, int c);
int toupper(WinApplication* app, x86::CPU& cpu, Cdecl, int c);
int isdigit(WinApplication* app, x86::CPU& cpu, Cdecl, int c);
int isspace(WinApplication* app, x86::CPU& cpu, Cdecl, int c);
int isprint(WinApplication* app, x86::CPU& cpu, Cdecl, int c);
int abs(WinApplication* app, x86::CPU& cpu, Cdecl, int value);

// Formatted text.
int printf(WinApplication* app, x86::CPU& cpu, Cdecl);
int sprintf(WinApplication* app, x86::CPU& cpu, Cdecl);
int _snprintf(WinApplication* app, x86::CPU& cpu, Cdecl);
int fprintf(WinApplication* app, x86::CPU& cpu, Cdecl);
int vsprintf(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 buffer, x86::reg32 format, x86::reg32 arguments);
int _vsnprintf(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 buffer, UINT count, x86::reg32 format,
               x86::reg32 arguments);
int vfprintf(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 stream, x86::reg32 format, x86::reg32 arguments);
int vswprintf_count(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 buffer, UINT count, x86::reg32 format,
                    x86::reg32 arguments);
int sscanf(WinApplication* app, x86::CPU& cpu, Cdecl);

// Streams.
x86::reg32 fopen(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 path, x86::reg32 mode);
int fclose(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 stream);
UINT fread(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 buffer, UINT size, UINT count, x86::reg32 stream);
UINT fwrite(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 buffer, UINT size, UINT count, x86::reg32 stream);
int fflush(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 stream);
int fgetc(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 stream);
int fputc(WinApplication* app, x86::CPU& cpu, Cdecl, int c, x86::reg32 stream);
int ungetc(WinApplication* app, x86::CPU& cpu, Cdecl, int c, x86::reg32 stream);

// Low-level files and folders.
int _open(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 path, int flags, int permissions);
int _close(WinApplication* app, x86::CPU& cpu, Cdecl, int fd);
int _read(WinApplication* app, x86::CPU& cpu, Cdecl, int fd, x86::reg32 buffer, UINT count);
int _write(WinApplication* app, x86::CPU& cpu, Cdecl, int fd, x86::reg32 buffer, UINT count);
x86::sreg32 _lseek(WinApplication* app, x86::CPU& cpu, Cdecl, int fd, x86::sreg32 offset, int origin);
x86::sreg32 _filelength(WinApplication* app, x86::CPU& cpu, Cdecl, int fd);
x86::sreg32 _findfirst(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 pattern, x86::reg32 data);
int _findnext(WinApplication* app, x86::CPU& cpu, Cdecl, x86::sreg32 handle, x86::reg32 data);
int _findclose(WinApplication* app, x86::CPU& cpu, Cdecl, x86::sreg32 handle);
int _mkdir(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 path);
int _rmdir(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 path);
int remove(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 path);
x86::reg32 _getcwd(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 buffer, int size);
int _getdrive(WinApplication* app, x86::CPU& cpu, Cdecl);
x86::reg32 _fullpath(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 buffer, x86::reg32 path, UINT size);
UINT _getdiskfree(WinApplication* app, x86::CPU& cpu, Cdecl, UINT drive, x86::reg32 info);
x86::reg32 getenv(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 name);

// Time.
x86::sreg32 time(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 out);
x86::sreg64 _time64(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 out);
x86::sreg32 clock(WinApplication* app, x86::CPU& cpu, Cdecl);
void _ftime(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 out);
x86::reg32 localtime(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 time);
x86::reg32 gmtime(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 time);
x86::sreg32 mktime(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 tm);
x86::reg32 _localtime64(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 time);
x86::reg32 _gmtime64(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 time);
x86::sreg64 _mktime64(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 tm);

// Mathematics: double arguments on the stack, double results in st0; the _CI functions take st0 (and st1).
void ceil(WinApplication* app, x86::CPU& cpu, Cdecl);
void floor(WinApplication* app, x86::CPU& cpu, Cdecl);
void sqrt(WinApplication* app, x86::CPU& cpu, Cdecl);
void cos(WinApplication* app, x86::CPU& cpu, Cdecl);
void _CIacos(WinApplication* app, x86::CPU& cpu, Cdecl);
void _CIasin(WinApplication* app, x86::CPU& cpu, Cdecl);
void _CIcosh(WinApplication* app, x86::CPU& cpu, Cdecl);
void _CIsinh(WinApplication* app, x86::CPU& cpu, Cdecl);
void _CItanh(WinApplication* app, x86::CPU& cpu, Cdecl);
void _CIpow(WinApplication* app, x86::CPU& cpu, Cdecl);
void _CIfmod(WinApplication* app, x86::CPU& cpu, Cdecl);
int _isnan(WinApplication* app, x86::CPU& cpu, Cdecl);
UINT _control87(WinApplication* app, x86::CPU& cpu, Cdecl, UINT value, UINT mask);
UINT _controlfp(WinApplication* app, x86::CPU& cpu, Cdecl, UINT value, UINT mask);

// Miscellaneous.
int rand(WinApplication* app, x86::CPU& cpu, Cdecl);
void srand(WinApplication* app, x86::CPU& cpu, Cdecl, UINT seed);
void qsort(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 base, UINT count, UINT width, x86::reg32 compare);
x86::reg32 bsearch(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 key, x86::reg32 base, UINT count, UINT width,
                   x86::reg32 compare);
x86::reg32 _errno(WinApplication* app, x86::CPU& cpu, Cdecl);

// Runtime services for msvcp71 and the D3DX shims: guest heap blocks as malloc/free give them.
x86::reg32 allocate(x86::reg32 size);
void release(x86::reg32 block);

}}

#endif
