// msvcr71.dll: start-up and exit, exceptions, setjmp/longjmp, threads, memory, strings, mathematics and the rest of
// the C runtime NFSC.exe imports, with the Microsoft runtime's results. Streams, files and time are in
// msvcr71_io.cpp, printf and scanf in msvcr71_format.cpp.

#include "msvcr71_internal.h"
#include <winapi/kernel32.h>
#include <winapi/support.h>
#include <lib/guest.h>
#include <lib/heap.h>
#include <lib/teb.h>
#include <nfsc_layout.h>
#include <platform/log.h>
#include <platform/sync.h>
#include <recomp.h>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <map>
#include <vector>

namespace win32 { namespace msvcr71
{

namespace
{

// Runtime variables and constants in guest memory.
enum : x86::reg32
{
    IobEntries = 20,
    FileSize = 32,
    OffsetIob = 0,
    OffsetAdjustFdiv = IobEntries * FileSize,
    OffsetFmode = OffsetAdjustFdiv + 4,
    OffsetCommode = OffsetFmode + 4,
    OffsetInitenv = OffsetCommode + 4,
    OffsetExceptionVtable = OffsetInitenv + 4,     // two slots
    OffsetUnknownException = OffsetExceptionVtable + 8,
    OffsetEnd = OffsetUnknownException + 32
};

x86::reg32 s_statics = 0;
pf::LwMutex s_mutex("msvcr71");
std::map<x86::reg32, x86::reg32> s_threadData;     // guest thread id -> ThreadData block
std::vector<x86::reg32> s_onexit;
x86::reg32 s_argv = 0, s_envp = 0;
int s_appType = 0;

x86::reg32 threadId(WinApplication* app, const x86::CPU& cpu)
{
    return app->getMemory<x86::reg32>(cpu.efs + teb::ThreadId);
}

void exceptionDeletingDestructor(WinApplication* app, x86::CPU& cpu);
void exceptionWhat(WinApplication* app, x86::CPU& cpu);

void initStatics(WinApplication* app)
{
    if (s_statics)
        return;
    s_statics = heap::allocate(OffsetEnd, true);
    for (x86::reg32 i = 0; i < 3; ++i)
    {
        const x86::reg32 file = s_statics + OffsetIob + i * FileSize;
        rc::st32(file + 12, i == 0 ? 0x1 : 0x2);       // _flag: _IOREAD, _IOWRT
        rc::st32(file + 16, i);                         // _file
    }
    const x86::reg32 vtable = s_statics + OffsetExceptionVtable;
    rc::st32(vtable, vtable);
    rc::st32(vtable + 4, vtable + 4);
    app->registerMethod(vtable, {"<msvcr71>exception::`scalar deleting destructor'", &exceptionDeletingDestructor});
    app->registerMethod(vtable + 4, {"<msvcr71>exception::what", &exceptionWhat});
    std::strcpy(guestChars(s_statics + OffsetUnknownException), "Unknown exception");
}

// Calls a cdecl guest function on a copy of the caller's state; returns eax. The caller's terminate request is
// passed on.
x86::reg32 callCdecl(WinApplication* app, x86::CPU& cpu, x86::reg32 function, std::initializer_list<x86::reg32> args)
{
    x86::CPU copy = cpu;
    copy.esp -= 4 * x86::reg32(args.size());
    x86::reg32 slot = copy.esp;
    for (x86::reg32 a : args)
    {
        rc::st32(slot, a);
        slot += 4;
    }
    native::callGuest(app, copy, function);
    if (copy.terminate)
        cpu.terminate = true;
    return copy.eax;
}

// Ends the process without running the onexit table (the shim returns; generated code stops at cpu.terminate).
void stopProcess(WinApplication* app, x86::CPU& cpu, int status)
{
    pf::logInfo("msvcr71: process exit %d", status);
    kernel32::ExitProcess(app, cpu, UINT(status));
}

void runOnexit(WinApplication* app, x86::CPU& cpu)
{
    for (;;)
    {
        x86::reg32 function;
        {
            pf::LwLockGuard guard(s_mutex);
            if (s_onexit.empty())
                return;
            function = s_onexit.back();
            s_onexit.pop_back();
        }
        callCdecl(app, cpu, function, {});
        if (cpu.terminate)
            return;
    }
}

// The Microsoft runtime's ctype for the "C" locale (characters 0-255).
bool isSpaceC(int c)    { return c == ' ' || (c >= 0x09 && c <= 0x0d); }
bool isDigitC(int c)    { return c >= '0' && c <= '9'; }
int lowerC(int c)       { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
int upperC(int c)       { return c >= 'a' && c <= 'z' ? c - 32 : c; }

// strtol / strtoul as the runtime parses: white space, sign, 0x / 0 prefixes for base 0 and 16, overflow to the
// limits with ERANGE (strtoul negates a negative result).
x86::reg32 parseInteger(WinApplication* app, const x86::CPU& cpu, x86::reg32 s, x86::reg32 end, int base,
                        bool isUnsigned)
{
    const char* const start = guestChars(s);
    const char* p = start;
    while (isSpaceC(x86::reg8(*p)))
        ++p;
    bool negative = false;
    if (*p == '-' || *p == '+')
        negative = *p++ == '-';
    if (base < 0 || base == 1 || base > 36)
    {
        if (end)
            rc::st32(end, s);
        return 0;
    }
    if (base == 0)
        base = *p != '0' ? 10 : (p[1] == 'x' || p[1] == 'X') ? 16 : 8;
    if (base == 16 && p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
        p += 2;
    x86::reg64 value = 0;
    bool overflow = false, any = false;
    for (;; ++p)
    {
        const int c = x86::reg8(*p);
        const int digit = isDigitC(c) ? c - '0' : (c >= 'a' && c <= 'z') ? c - 'a' + 10
                        : (c >= 'A' && c <= 'Z') ? c - 'A' + 10 : 99;
        if (digit >= base)
            break;
        any = true;
        value = value * x86::reg64(base) + x86::reg64(digit);
        if (value > 0xffffffffull)
        {
            overflow = true;
            value = 0xffffffffull;
        }
    }
    if (end)
        rc::st32(end, any ? guestAddress(p) : s);
    if (!any)
        return 0;
    if (isUnsigned)
    {
        if (overflow)
        {
            setErrno(app, cpu, ErrRange);
            return 0xffffffffu;
        }
        return negative ? x86::reg32(0u - x86::reg32(value)) : x86::reg32(value);
    }
    if (overflow || (!negative && value > 0x7fffffffull) || (negative && value > 0x80000000ull))
    {
        setErrno(app, cpu, ErrRange);
        return negative ? 0x80000000u : 0x7fffffffu;
    }
    return negative ? x86::reg32(0u - x86::reg32(value)) : x86::reg32(value);
}

// The runtime's sys_errlist.
const char* const s_errors[] = {
    "No error", "Operation not permitted", "No such file or directory", "No such process",
    "Interrupted function call", "Input/output error", "No such device or address", "Arg list too long",
    "Exec format error", "Bad file descriptor", "No child processes", "Resource temporarily unavailable",
    "Not enough space", "Permission denied", "Bad address", "Unknown error", "Resource device", "File exists",
    "Improper link", "No such device", "Not a directory", "Is a directory", "Invalid argument",
    "Too many open files in system", "Too many open files", "Inappropriate I/O control operation",
    "Unknown error", "File too large", "No space left on device", "Invalid seek", "Read-only file system",
    "Too many links", "Broken pipe", "Domain error", "Result too large", "Unknown error", "Resource deadlock avoided",
    "Unknown error", "Filename too long", "No locks available", "Function not implemented", "Directory not empty",
    "Illegal byte sequence", "Unknown error"
};
const x86::reg32 ErrorCount = sizeof(s_errors) / sizeof(s_errors[0]);
x86::reg32 s_errorStrings[ErrorCount] = {};

// x87 control word <-> the runtime's abstract control word (_MCW_EM, _MCW_IC, _MCW_RC, _MCW_PC).
x86::reg32 abstractControl(x86::reg16 cw)
{
    x86::reg32 a = 0;
    if (cw & 0x01) a |= 0x10;       // _EM_INVALID
    if (cw & 0x02) a |= 0x80000;    // _EM_DENORMAL
    if (cw & 0x04) a |= 0x08;       // _EM_ZERODIVIDE
    if (cw & 0x08) a |= 0x04;       // _EM_OVERFLOW
    if (cw & 0x10) a |= 0x02;       // _EM_UNDERFLOW
    if (cw & 0x20) a |= 0x01;       // _EM_INEXACT
    static const x86::reg32 rounding[4] = { 0x000, 0x100, 0x200, 0x300 };
    a |= rounding[(cw >> 10) & 3];
    static const x86::reg32 precision[4] = { 0x20000, 0x20000, 0x10000, 0x00000 };
    a |= precision[(cw >> 8) & 3];
    if (cw & 0x1000) a |= 0x40000;  // _IC_AFFINE
    return a;
}

x86::reg16 hardwareControl(x86::reg32 a, x86::reg16 old)
{
    x86::reg16 cw = x86::reg16(old & 0xe0c0);
    if (a & 0x10) cw |= 0x01;
    if (a & 0x80000) cw |= 0x02;
    if (a & 0x08) cw |= 0x04;
    if (a & 0x04) cw |= 0x08;
    if (a & 0x02) cw |= 0x10;
    if (a & 0x01) cw |= 0x20;
    cw |= x86::reg16(((a >> 8) & 3) << 10);
    const x86::reg32 pc = a & 0x30000;
    cw |= pc == 0x20000 ? 0x000 : pc == 0x10000 ? 0x200 : 0x300;
    if (a & 0x40000) cw |= 0x1000;
    return cw;
}

UINT control(x86::CPU& cpu, UINT value, UINT mask)
{
    const x86::reg32 current = abstractControl(cpu.fpu.control.word);
    const x86::reg32 next = (current & ~mask) | (value & mask);
    cpu.fpu.control.word = hardwareControl(next, cpu.fpu.control.word);
    return abstractControl(cpu.fpu.control.word);
}

// The x87 indefinite the runtime returns for domain errors.
double indefinite()
{
    const x86::reg64 bits = 0xfff8000000000000ull;
    double value;
    std::memcpy(&value, &bits, 8);
    return value;
}

double argumentDouble(const x86::CPU& cpu, int index)
{
    return rc::ldf64(cpu.esp + 4 + 8 * index);
}

void pushResult(x86::CPU& cpu, double value)
{
    cpu.fpu.push(x86::Float(value));
}

// Result of a one-argument function with the runtime's domain handling: NaN arguments pass through, a domain
// error gives the indefinite and EDOM.
double domainChecked(WinApplication* app, const x86::CPU& cpu, double x, double result, bool domainError)
{
    if (std::isnan(x))
        return x;
    if (domainError)
    {
        setErrno(app, cpu, ErrDom);
        return indefinite();
    }
    if (std::isinf(result) && !std::isinf(x))
        setErrno(app, cpu, ErrRange);
    return result;
}

// One step of the runtime's qsort for elements at guest addresses.
void swapElements(x86::reg32 a, x86::reg32 b, UINT width)
{
    if (a == b)
        return;
    x86::reg8* pa = g_guestBase + a;
    x86::reg8* pb = g_guestBase + b;
    for (UINT i = 0; i < width; ++i)
    {
        const x86::reg8 t = pa[i];
        pa[i] = pb[i];
        pb[i] = t;
    }
}

int compareElements(WinApplication* app, x86::CPU& cpu, x86::reg32 compare, x86::reg32 a, x86::reg32 b)
{
    return x86::sreg32(callCdecl(app, cpu, compare, { a, b }));
}

void exceptionDestroyAt(WinApplication* app, x86::CPU& cpu, x86::reg32 self)
{
    RC_USE(app);
    RC_USE(cpu);
    rc::st32(self, s_statics + OffsetExceptionVtable);
    if (rc::ld32(self + 8))
        release(rc::ld32(self + 4));
}

void exceptionDeletingDestructor(WinApplication* app, x86::CPU& cpu)
{
    const x86::reg32 self = cpu.ecx;
    const x86::reg32 flags = rc::ld32(cpu.esp + 4);
    exceptionDestroyAt(app, cpu, self);
    if (flags & 1)
        release(self);
    cpu.eax = self;
    cpu.esp += 8;
}

void exceptionWhat(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    const x86::reg32 what = rc::ld32(cpu.ecx + 4);
    cpu.eax = what ? what : s_statics + OffsetUnknownException;
    cpu.esp += 4;
}

}

// ---- Shared -------------------------------------------------------------------------------------------------------

x86::reg32 threadData(WinApplication* app, const x86::CPU& cpu)
{
    const x86::reg32 id = threadId(app, cpu);
    pf::LwLockGuard guard(s_mutex);
    auto it = s_threadData.find(id);
    if (it != s_threadData.end())
        return it->second;
    const x86::reg32 block = heap::allocate(sizeof(ThreadData), true);
    reinterpret_cast<ThreadData*>(g_guestBase + block)->randSeed = 1;
    s_threadData[id] = block;
    return block;
}

void setErrno(WinApplication* app, const x86::CPU& cpu, int value)
{
    threadDataOf(app, cpu).errnoValue = x86::reg32(value);
}

int crtErrno(int hostErrno)
{
    switch (hostErrno)
    {
    case ENOENT:    return ErrNoEnt;
    case EBADF:     return ErrBadF;
    case ENOMEM:    return ErrNoMem;
    case EACCES:    return ErrAccess;
    case EEXIST:    return ErrExist;
    case EMFILE:    return ErrMFile;
    case ENOSPC:    return ErrNoSpc;
    case ENOTEMPTY: return ErrNotEmpty;
    default:        return ErrInval;
    }
}

x86::reg32 iobAddress()
{
    return s_statics + OffsetIob;
}

int fileMode()
{
    return int(rc::ld32(s_statics + OffsetFmode));
}

x86::reg32 allocate(x86::reg32 size)
{
    return heap::allocate(size ? size : 1, false);
}

void release(x86::reg32 block)
{
    if (block)
        heap::release(block);
}

// ---- Variables and start-up ----------------------------------------------------------------------------------------

x86::reg32 data_iob(WinApplication* app)
{
    initStatics(app);
    return s_statics + OffsetIob;
}

x86::reg32 data_adjust_fdiv(WinApplication* app)
{
    initStatics(app);
    return s_statics + OffsetAdjustFdiv;
}

void __set_app_type(WinApplication* app, x86::CPU& cpu, Cdecl, int type)
{
    RC_USE(app);
    RC_USE(cpu);
    s_appType = type;
}

x86::reg32 __p__fmode(WinApplication* app, x86::CPU& cpu, Cdecl)
{
    RC_USE(cpu);
    initStatics(app);
    return s_statics + OffsetFmode;
}

x86::reg32 __p__commode(WinApplication* app, x86::CPU& cpu, Cdecl)
{
    RC_USE(cpu);
    initStatics(app);
    return s_statics + OffsetCommode;
}

x86::reg32 __p___initenv(WinApplication* app, x86::CPU& cpu, Cdecl)
{
    RC_USE(cpu);
    initStatics(app);
    return s_statics + OffsetInitenv;
}

// The math error handler is kept; the functions here report domain and range errors through errno only, as with a
// handler that returns 0.
void __setusermatherr(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 handler)
{
    RC_USE(app);
    RC_USE(cpu);
    pf::logDebug("msvcr71: math error handler 0x%08x", handler);
}

// argv from the command line with the runtime's quoting rules, envp from the environment block.
int __getmainargs(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 argc, x86::reg32 argv, x86::reg32 envp,
                  int expandWildcards, x86::reg32 startInfo)
{
    RC_USE(cpu);
    RC_USE(expandWildcards);
    RC_USE(startInfo);
    initStatics(app);
    if (!s_argv)
    {
        std::vector<std::string> words;
        const char* p = app->getAppNameRaw();
        while (*p)
        {
            while (*p == ' ' || *p == '\t')
                ++p;
            if (!*p)
                break;
            std::string word;
            bool quoted = false;
            for (; *p && (quoted || (*p != ' ' && *p != '\t')); ++p)
            {
                if (*p == '\\')
                {
                    size_t slashes = 0;
                    while (*p == '\\')
                    {
                        ++slashes;
                        ++p;
                    }
                    if (*p == '"')
                    {
                        word.append(slashes / 2, '\\');
                        if (slashes % 2)
                            word += '"';
                        else
                            quoted = !quoted;
                    }
                    else
                    {
                        word.append(slashes, '\\');
                        --p;
                    }
                }
                else if (*p == '"')
                {
                    quoted = !quoted;
                }
                else
                {
                    word += *p;
                }
            }
            words.push_back(word);
        }
        s_argv = heap::allocate(x86::reg32(4 * (words.size() + 1)), true);
        for (size_t i = 0; i < words.size(); ++i)
        {
            const x86::reg32 text = heap::allocate(x86::reg32(words[i].size() + 1), true);
            std::memcpy(guestChars(text), words[i].c_str(), words[i].size() + 1);
            rc::st32(s_argv + x86::reg32(4 * i), text);
        }
        rc::st32(argc, x86::reg32(words.size()));
        std::vector<x86::reg32> variables;
        for (x86::reg32 e = app->getEnv(); *guestChars(e); e += x86::reg32(std::strlen(guestChars(e)) + 1))
            variables.push_back(e);
        s_envp = heap::allocate(x86::reg32(4 * (variables.size() + 1)), true);
        for (size_t i = 0; i < variables.size(); ++i)
            rc::st32(s_envp + x86::reg32(4 * i), variables[i]);
        rc::st32(s_statics + OffsetInitenv, s_envp);
        pf::logInfo("msvcr71: %u command line arguments", unsigned(words.size()));
    }
    x86::reg32 count = 0;
    while (rc::ld32(s_argv + 4 * count))
        ++count;
    rc::st32(argc, count);
    rc::st32(argv, s_argv);
    rc::st32(envp, s_envp);
    return 0;
}

void _initterm(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 begin, x86::reg32 end)
{
    for (x86::reg32 p = begin; p < end && !cpu.terminate; p += 4)
    {
        const x86::reg32 function = rc::ld32(p);
        if (function)
            callCdecl(app, cpu, function, {});
    }
}

x86::reg32 _onexit(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 function)
{
    RC_USE(app);
    RC_USE(cpu);
    pf::LwLockGuard guard(s_mutex);
    s_onexit.push_back(function);
    return function;
}

// The table [*begin, *end) of a DLL's terminators: grown in guest memory with the runtime's allocator.
x86::reg32 __dllonexit(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 function, x86::reg32 begin,
                       x86::reg32 end)
{
    RC_USE(app);
    RC_USE(cpu);
    pf::LwLockGuard guard(s_mutex);
    const x86::reg32 first = rc::ld32(begin);
    const x86::reg32 last = rc::ld32(end);
    const x86::reg32 used = first ? last - first : 0;
    const x86::reg32 table = heap::allocate(used + 4, false);
    if (!table)
        return 0;
    if (used)
        std::memcpy(g_guestBase + table, g_guestBase + first, used);
    if (first)
        heap::release(first);
    rc::st32(table + used, function);
    rc::st32(begin, table);
    rc::st32(end, table + used + 4);
    return function;
}

void exit(WinApplication* app, x86::CPU& cpu, Cdecl, int status)
{
    runOnexit(app, cpu);
    stopProcess(app, cpu, status);
}

void _exit(WinApplication* app, x86::CPU& cpu, Cdecl, int status)
{
    stopProcess(app, cpu, status);
}

void _cexit(WinApplication* app, x86::CPU& cpu, Cdecl)
{
    runOnexit(app, cpu);
}

void _c_exit(WinApplication* app, x86::CPU& cpu, Cdecl)
{
    RC_USE(app);
    RC_USE(cpu);
}

void _amsg_exit(WinApplication* app, x86::CPU& cpu, Cdecl, int code)
{
    pf::logError("msvcr71: runtime error R60%02d, called from 0x%08x", code, callerAddress(app, cpu));
    stopProcess(app, cpu, 255);
}

// No signal handlers are installed (signal is not imported): the default action ends the process with 3.
int raise(WinApplication* app, x86::CPU& cpu, Cdecl, int signal)
{
    static const int known[] = { 2, 4, 8, 11, 15, 21, 22 };     // INT ILL FPE SEGV TERM BREAK ABRT
    for (int s : known)
    {
        if (s == signal)
        {
            pf::logError("msvcr71: raise(%d), called from 0x%08x", signal, callerAddress(app, cpu));
            stopProcess(app, cpu, 3);
            return 0;
        }
    }
    setErrno(app, cpu, ErrInval);
    return -1;
}

int _purecall(WinApplication* app, x86::CPU& cpu, Cdecl)
{
    pf::logError("msvcr71: runtime error R6025 (pure virtual function call), called from 0x%08x",
                 callerAddress(app, cpu));
    stopProcess(app, cpu, 255);
    return 0;
}

void __security_error_handler(WinApplication* app, x86::CPU& cpu, Cdecl, int code, x86::reg32 data)
{
    RC_USE(data);
    pf::logError("msvcr71: security check failure %d, called from 0x%08x", code, callerAddress(app, cpu));
    stopProcess(app, cpu, 3);
}

void terminate(WinApplication* app, x86::CPU& cpu, Cdecl)
{
    pf::logError("msvcr71: terminate, called from 0x%08x", callerAddress(app, cpu));
    stopProcess(app, cpu, 3);
}

// ---- Exceptions ----------------------------------------------------------------------------------------------------

int _except_handler3(WinApplication* app, x86::CPU& cpu, Cdecl)
{
    unsupported(app, cpu, "_except_handler3", "no exception dispatch");
}

int __CxxFrameHandler(WinApplication* app, x86::CPU& cpu, Cdecl)
{
    unsupported(app, cpu, "__CxxFrameHandler", "no exception dispatch, FuncInfo 0x%08x", cpu.eax);
}

int _XcptFilter(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 code, x86::reg32 pointers)
{
    unsupported(app, cpu, "_XcptFilter", "exception 0x%08x, pointers 0x%08x", code, pointers);
}

void _CxxThrowException(WinApplication* app, x86::CPU& cpu, x86::reg32 object, x86::reg32 throwInfo)
{
    unsupported(app, cpu, "_CxxThrowException", "C++ exceptions are not dispatched, object 0x%08x, info 0x%08x",
                object, throwInfo);
}

// exception::exception(): vtable, no message.
void exception_construct(WinApplication* app, x86::CPU& cpu)
{
    initStatics(app);
    const x86::reg32 self = cpu.ecx;
    rc::st32(self, s_statics + OffsetExceptionVtable);
    rc::st32(self + 4, 0);
    rc::st32(self + 8, 0);
    cpu.eax = self;
}

// exception::exception(const exception&): an owned message is copied.
void exception_copy(WinApplication* app, x86::CPU& cpu, x86::reg32 other)
{
    initStatics(app);
    const x86::reg32 self = cpu.ecx;
    rc::st32(self, s_statics + OffsetExceptionVtable);
    const x86::reg32 owned = rc::ld32(other + 8);
    const x86::reg32 what = rc::ld32(other + 4);
    rc::st32(self + 8, owned);
    if (owned && what)
    {
        const x86::reg32 length = x86::reg32(std::strlen(guestChars(what))) + 1;
        const x86::reg32 copy = allocate(length);
        if (copy)
            std::memcpy(guestChars(copy), guestChars(what), length);
        rc::st32(self + 4, copy);
    }
    else
    {
        rc::st32(self + 4, what);
    }
    cpu.eax = self;
}

void exception_destroy(WinApplication* app, x86::CPU& cpu)
{
    initStatics(app);
    exceptionDestroyAt(app, cpu, cpu.ecx);
}

// type_info::~type_info: frees the undecorated name the runtime may have cached.
void type_info_destroy(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    const x86::reg32 name = rc::ld32(cpu.ecx + 4);
    if (name)
        release(name);
}

// ---- setjmp / longjmp ---------------------------------------------------------------------------------------------

// _JUMP_BUFFER: Ebp, Ebx, Edi, Esi, Esp, Eip, Registration, TryLevel, Cookie, UnwindFunc, UnwindData[6].
int _setjmp3(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 buffer, int count)
{
    const x86::reg32 registration = app->getMemory<x86::reg32>(cpu.efs + teb::ExceptionList);
    rc::st32(buffer + 0, cpu.ebp);
    rc::st32(buffer + 4, cpu.ebx);
    rc::st32(buffer + 8, cpu.edi);
    rc::st32(buffer + 12, cpu.esi);
    rc::st32(buffer + 16, cpu.esp);
    rc::st32(buffer + 20, rc::ld32(cpu.esp));
    rc::st32(buffer + 24, registration);
    rc::st32(buffer + 28, registration == 0xffffffffu ? 0xffffffffu : rc::ld32(registration + 12));
    rc::st32(buffer + 32, 0x56433230);      // "VC20"
    rc::st32(buffer + 36, count > 0 ? rc::ld32(cpu.esp + 12) : 0);
    return 0;
}

// Restores the registers _setjmp3 saved and resumes after its call with eax = value (1 for 0). Exception frames
// registered since would need their unwind handlers run: that traps.
void longjmp(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 buffer, int value)
{
    const x86::reg32 registration = rc::ld32(buffer + 24);
    if (app->getMemory<x86::reg32>(cpu.efs + teb::ExceptionList) != registration)
        unsupported(app, cpu, "longjmp", "exception frames registered since setjmp would need unwinding (0x%08x)",
                    buffer);
    cpu.ebp = rc::ld32(buffer + 0);
    cpu.ebx = rc::ld32(buffer + 4);
    cpu.edi = rc::ld32(buffer + 8);
    cpu.esi = rc::ld32(buffer + 12);
    cpu.esp = rc::ld32(buffer + 16) + 4;
    cpu.eax = value ? x86::reg32(value) : 1;
    rc::longJump(app, cpu, buffer);
}

// ---- Threads -------------------------------------------------------------------------------------------------------

x86::reg32 _beginthreadex(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 security, UINT stackSize,
                          x86::reg32 start, x86::reg32 argument, UINT flags, x86::reg32 threadIdOut)
{
    RC_USE(security);
    if (!start)
    {
        setErrno(app, cpu, ErrInval);
        return 0;
    }
    DWORD id = 0;
    const HANDLE handle = kernel32::CreateThread(app, cpu, nullptr, stackSize, start, argument, flags & 0x4, &id);
    if (threadIdOut)
        rc::st32(threadIdOut, id);
    return handle;
}

void _endthreadex(WinApplication* app, x86::CPU& cpu, Cdecl, UINT code)
{
    kernel32::ExitThread(app, cpu, code);
}

// ---- Memory --------------------------------------------------------------------------------------------------------

static HeapObserver s_heapAllocated = nullptr;
static void (*s_heapReleased)(x86::reg32 block) = nullptr;

void setHeapObserver(HeapObserver allocated, void (*released)(x86::reg32 block))
{
    s_heapAllocated = allocated;
    s_heapReleased = released;
}

// The observer learns a malloc block with its caller and the next guest code address on the stack.
static void observeAllocation(WinApplication* app, x86::CPU& cpu, x86::reg32 block, x86::reg32 size)
{
    const nfsc::layout::ImageSection& text = nfsc::layout::sections[0];
    x86::reg32 via = 0;
    for (x86::reg32 slot = cpu.esp + 4; slot < cpu.esp + 4 + 32 * 4 && !via; slot += 4)
    {
        const x86::reg32 value = app->getMemory<x86::reg32>(slot);
        if (value - text.address < text.virtualSize)
            via = value;
    }
    s_heapAllocated(block, size, app->getMemory<x86::reg32>(cpu.esp), via);
}

x86::reg32 malloc(WinApplication* app, x86::CPU& cpu, Cdecl, UINT size)
{
    if (size >= 4 * 1024 * 1024)
    {
        // The caller, then the next code addresses on the guest stack.
        const nfsc::layout::ImageSection& text = nfsc::layout::sections[0];
        x86::reg32 chain[4] = {};
        int found = 0;
        for (x86::reg32 slot = cpu.esp + 4; slot < cpu.esp + 4 + 64 * 4 && found < 4; slot += 4)
        {
            const x86::reg32 value = app->getMemory<x86::reg32>(slot);
            if (value - text.address < text.virtualSize)
                chain[found++] = value;
        }
        pf::logInfo("malloc: %u bytes from 0x%08x (stack 0x%08x 0x%08x 0x%08x 0x%08x)", size,
                    callerAddress(app, cpu), chain[0], chain[1], chain[2], chain[3]);
    }
    const x86::reg32 block = size <= 0xffffffe0u ? allocate(size) : 0;
    if (!block)
        setErrno(app, cpu, ErrNoMem);
    else if (s_heapAllocated)
        observeAllocation(app, cpu, block, size);
    return block;
}

x86::reg32 calloc(WinApplication* app, x86::CPU& cpu, Cdecl, UINT count, UINT size)
{
    const x86::reg64 total = x86::reg64(count) * size;
    const x86::reg32 block = total <= 0xffffffe0u ? heap::allocate(total ? x86::reg32(total) : 1, true) : 0;
    if (!block)
        setErrno(app, cpu, ErrNoMem);
    return block;
}

x86::reg32 realloc(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 block, UINT size)
{
    if (!block)
        return malloc(app, cpu, Cdecl{}, size);
    if (!size)
    {
        release(block);
        return 0;
    }
    const x86::reg32 old = heap::size(block);
    if (old == 0xffffffffu)
        unsupported(app, cpu, "realloc", "0x%08x is no heap block", block);
    const x86::reg32 next = size <= 0xffffffe0u ? allocate(size) : 0;
    if (!next)
    {
        setErrno(app, cpu, ErrNoMem);
        return 0;
    }
    std::memcpy(g_guestBase + next, g_guestBase + block, old < size ? old : size);
    release(block);
    return next;
}

void free(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 block)
{
    if (block && s_heapReleased)
        s_heapReleased(block);
    if (block && !heap::release(block))
        unsupported(app, cpu, "free", "0x%08x is no heap block", block);
}

// The runtime's layout: the block from malloc, the aligned address above it with the block's address in the dword
// below.
x86::reg32 _aligned_malloc(WinApplication* app, x86::CPU& cpu, Cdecl, UINT size, UINT alignment)
{
    if (alignment & (alignment - 1))
    {
        setErrno(app, cpu, ErrInval);
        return 0;
    }
    if (alignment < 4)
        alignment = 4;
    const x86::reg64 total = x86::reg64(size) + alignment - 1 + 4;
    const x86::reg32 block = total <= 0xffffffe0u ? allocate(x86::reg32(total)) : 0;
    if (!block)
    {
        setErrno(app, cpu, ErrNoMem);
        return 0;
    }
    const x86::reg32 aligned = (block + 4 + alignment - 1) & ~(alignment - 1);
    rc::st32(aligned - 4, block);
    return aligned;
}

x86::reg32 _aligned_realloc(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 block, UINT size, UINT alignment)
{
    if (!block)
        return _aligned_malloc(app, cpu, Cdecl{}, size, alignment);
    if (!size)
    {
        _aligned_free(app, cpu, Cdecl{}, block);
        return 0;
    }
    const x86::reg32 base = rc::ld32(block - 4);
    const x86::reg32 baseSize = heap::size(base);
    if (baseSize == 0xffffffffu)
        unsupported(app, cpu, "_aligned_realloc", "0x%08x is no aligned block", block);
    const x86::reg32 old = baseSize - (block - base);
    const x86::reg32 next = _aligned_malloc(app, cpu, Cdecl{}, size, alignment);
    if (!next)
        return 0;
    std::memcpy(g_guestBase + next, g_guestBase + block, old < size ? old : size);
    release(base);
    return next;
}

void _aligned_free(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 block)
{
    if (block)
        free(app, cpu, Cdecl{}, rc::ld32(block - 4));
}

// ---- Memory and strings --------------------------------------------------------------------------------------------

x86::reg32 memmove(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 dst, x86::reg32 src, UINT count)
{
    RC_USE(app);
    RC_USE(cpu);
    if (count)
        std::memmove(g_guestBase + dst, g_guestBase + src, count);
    return dst;
}

// memcpy of msvcr71 is memmove.
x86::reg32 memcpy(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 dst, x86::reg32 src, UINT count)
{
    RC_USE(app);
    RC_USE(cpu);
    if (count)
        std::memmove(g_guestBase + dst, g_guestBase + src, count);
    return dst;
}

x86::reg32 memset(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 dst, int value, UINT count)
{
    RC_USE(app);
    RC_USE(cpu);
    if (count)
        std::memset(g_guestBase + dst, value, count);
    return dst;
}

int memcmp(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 a, x86::reg32 b, UINT count)
{
    RC_USE(app);
    RC_USE(cpu);
    const int r = count ? std::memcmp(g_guestBase + a, g_guestBase + b, count) : 0;
    return r < 0 ? -1 : r > 0 ? 1 : 0;
}

UINT strlen(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 s)
{
    RC_USE(app);
    RC_USE(cpu);
    return UINT(std::strlen(guestChars(s)));
}

int strcmp(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 a, x86::reg32 b)
{
    RC_USE(app);
    RC_USE(cpu);
    const int r = std::strcmp(guestChars(a), guestChars(b));
    return r < 0 ? -1 : r > 0 ? 1 : 0;
}

int strncmp(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 a, x86::reg32 b, UINT count)
{
    RC_USE(app);
    RC_USE(cpu);
    const unsigned char* pa = reinterpret_cast<const unsigned char*>(guestChars(a));
    const unsigned char* pb = reinterpret_cast<const unsigned char*>(guestChars(b));
    for (UINT i = 0; i < count; ++i)
    {
        if (pa[i] != pb[i])
            return int(pa[i]) - int(pb[i]);
        if (!pa[i])
            return 0;
    }
    return 0;
}

int _strcmpi(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 a, x86::reg32 b)
{
    RC_USE(app);
    RC_USE(cpu);
    const unsigned char* pa = reinterpret_cast<const unsigned char*>(guestChars(a));
    const unsigned char* pb = reinterpret_cast<const unsigned char*>(guestChars(b));
    for (;; ++pa, ++pb)
    {
        const int ca = lowerC(*pa), cb = lowerC(*pb);
        if (ca != cb || !ca)
            return ca - cb;
    }
}

int _strnicmp(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 a, x86::reg32 b, UINT count)
{
    RC_USE(app);
    RC_USE(cpu);
    const unsigned char* pa = reinterpret_cast<const unsigned char*>(guestChars(a));
    const unsigned char* pb = reinterpret_cast<const unsigned char*>(guestChars(b));
    for (UINT i = 0; i < count; ++i)
    {
        const int ca = lowerC(pa[i]), cb = lowerC(pb[i]);
        if (ca != cb || !ca)
            return ca - cb;
    }
    return 0;
}

// The "C" locale collates by byte value.
int strcoll(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 a, x86::reg32 b)
{
    return strcmp(app, cpu, Cdecl{}, a, b);
}

x86::reg32 strncpy(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 dst, x86::reg32 src, UINT count)
{
    RC_USE(app);
    RC_USE(cpu);
    char* d = guestChars(dst);
    const char* s = guestChars(src);
    UINT i = 0;
    for (; i < count && s[i]; ++i)
        d[i] = s[i];
    for (; i < count; ++i)
        d[i] = 0;
    return dst;
}

x86::reg32 strncat(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 dst, x86::reg32 src, UINT count)
{
    RC_USE(app);
    RC_USE(cpu);
    char* d = guestChars(dst) + std::strlen(guestChars(dst));
    const char* s = guestChars(src);
    UINT i = 0;
    for (; i < count && s[i]; ++i)
        d[i] = s[i];
    d[i] = 0;
    return dst;
}

// The single-byte code page: _mbscpy and _mbscat copy as strcpy and strcat.
x86::reg32 _mbscpy(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 dst, x86::reg32 src)
{
    RC_USE(app);
    RC_USE(cpu);
    std::memmove(guestChars(dst), guestChars(src), std::strlen(guestChars(src)) + 1);
    return dst;
}

x86::reg32 _mbscat(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 dst, x86::reg32 src)
{
    RC_USE(app);
    RC_USE(cpu);
    char* end = guestChars(dst) + std::strlen(guestChars(dst));
    std::memmove(end, guestChars(src), std::strlen(guestChars(src)) + 1);
    return dst;
}

x86::reg32 strchr(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 s, int c)
{
    RC_USE(app);
    RC_USE(cpu);
    const char* p = std::strchr(guestChars(s), char(c));
    return p ? guestAddress(p) : 0;
}

x86::reg32 strrchr(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 s, int c)
{
    RC_USE(app);
    RC_USE(cpu);
    const char* p = std::strrchr(guestChars(s), char(c));
    return p ? guestAddress(p) : 0;
}

x86::reg32 strstr(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 s, x86::reg32 find)
{
    RC_USE(app);
    RC_USE(cpu);
    const char* p = std::strstr(guestChars(s), guestChars(find));
    return p ? guestAddress(p) : 0;
}

UINT strcspn(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 s, x86::reg32 reject)
{
    RC_USE(app);
    RC_USE(cpu);
    return UINT(std::strcspn(guestChars(s), guestChars(reject)));
}

// The position after the last token is kept per thread.
x86::reg32 strtok(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 s, x86::reg32 delimiters)
{
    ThreadData& data = threadDataOf(app, cpu);
    x86::reg32 p = s ? s : data.strtokNext;
    if (!p)
        return 0;
    const char* set = guestChars(delimiters);
    while (*guestChars(p) && std::strchr(set, *guestChars(p)))
        ++p;
    if (!*guestChars(p))
    {
        data.strtokNext = p;
        return 0;
    }
    const x86::reg32 token = p;
    while (*guestChars(p) && !std::strchr(set, *guestChars(p)))
        ++p;
    if (*guestChars(p))
    {
        *guestChars(p) = 0;
        ++p;
    }
    data.strtokNext = p;
    return token;
}

x86::reg32 strerror(WinApplication* app, x86::CPU& cpu, Cdecl, int error)
{
    RC_USE(app);
    RC_USE(cpu);
    const x86::reg32 index = error >= 0 && x86::reg32(error) < ErrorCount - 1 ? x86::reg32(error) : ErrorCount - 1;
    pf::LwLockGuard guard(s_mutex);
    if (!s_errorStrings[index])
    {
        const size_t length = std::strlen(s_errors[index]) + 1;
        s_errorStrings[index] = heap::allocate(x86::reg32(length), false);
        std::memcpy(guestChars(s_errorStrings[index]), s_errors[index], length);
    }
    return s_errorStrings[index];
}

UINT wcslen(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 s)
{
    RC_USE(app);
    RC_USE(cpu);
    UINT n = 0;
    while (rc::ld16(s + 2 * n))
        ++n;
    return n;
}

x86::reg32 wcscpy(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 dst, x86::reg32 src)
{
    RC_USE(app);
    RC_USE(cpu);
    for (x86::reg32 i = 0;; ++i)
    {
        const x86::reg16 unit = rc::ld16(src + 2 * i);
        rc::st16(dst + 2 * i, unit);
        if (!unit)
            break;
    }
    return dst;
}

int atoi(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 s)
{
    RC_USE(app);
    RC_USE(cpu);
    const char* p = guestChars(s);
    while (isSpaceC(x86::reg8(*p)))
        ++p;
    bool negative = false;
    if (*p == '-' || *p == '+')
        negative = *p++ == '-';
    x86::reg32 value = 0;
    while (isDigitC(x86::reg8(*p)))
        value = value * 10 + x86::reg32(*p++ - '0');
    return int(negative ? 0u - value : value);
}

x86::sreg32 strtol(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 s, x86::reg32 end, int base)
{
    return x86::sreg32(parseInteger(app, cpu, s, end, base, false));
}

UINT strtoul(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 s, x86::reg32 end, int base)
{
    return parseInteger(app, cpu, s, end, base, true);
}

int tolower(WinApplication* app, x86::CPU& cpu, Cdecl, int c)
{
    RC_USE(app);
    RC_USE(cpu);
    return lowerC(c);
}

int toupper(WinApplication* app, x86::CPU& cpu, Cdecl, int c)
{
    RC_USE(app);
    RC_USE(cpu);
    return upperC(c);
}

// The ctype functions return the runtime's class bits of the "C" locale: _DIGIT 0x4, _SPACE 0x8, and for isprint
// the classes _BLANK, _PUNCT, _UPPER, _LOWER, _DIGIT.
int isdigit(WinApplication* app, x86::CPU& cpu, Cdecl, int c)
{
    RC_USE(app);
    RC_USE(cpu);
    return isDigitC(c) ? 0x4 : 0;
}

int isspace(WinApplication* app, x86::CPU& cpu, Cdecl, int c)
{
    RC_USE(app);
    RC_USE(cpu);
    return isSpaceC(c) ? 0x8 : 0;
}

int isprint(WinApplication* app, x86::CPU& cpu, Cdecl, int c)
{
    RC_USE(app);
    RC_USE(cpu);
    if (c < 0x20 || c > 0x7e)
        return 0;
    if (c == ' ')
        return 0x40;                                    // _BLANK
    if (isDigitC(c))
        return 0x4;                                     // _DIGIT
    if (c >= 'A' && c <= 'Z')
        return 0x1;                                     // _UPPER
    if (c >= 'a' && c <= 'z')
        return 0x2;                                     // _LOWER
    return 0x10;                                        // _PUNCT
}

int abs(WinApplication* app, x86::CPU& cpu, Cdecl, int value)
{
    RC_USE(app);
    RC_USE(cpu);
    return value < 0 ? int(0u - x86::reg32(value)) : value;
}

// ---- Mathematics ---------------------------------------------------------------------------------------------------

void ceil(WinApplication* app, x86::CPU& cpu, Cdecl)
{
    RC_USE(app);
    pushResult(cpu, std::ceil(argumentDouble(cpu, 0)));
}

void floor(WinApplication* app, x86::CPU& cpu, Cdecl)
{
    RC_USE(app);
    pushResult(cpu, std::floor(argumentDouble(cpu, 0)));
}

void sqrt(WinApplication* app, x86::CPU& cpu, Cdecl)
{
    const double x = argumentDouble(cpu, 0);
    pushResult(cpu, domainChecked(app, cpu, x, std::sqrt(x), x < 0));
}

void cos(WinApplication* app, x86::CPU& cpu, Cdecl)
{
    const double x = argumentDouble(cpu, 0);
    pushResult(cpu, domainChecked(app, cpu, x, std::cos(x), std::isinf(x)));
}

void _CIacos(WinApplication* app, x86::CPU& cpu, Cdecl)
{
    const double x = double(cpu.fpu.pop());
    pushResult(cpu, domainChecked(app, cpu, x, std::acos(x), x < -1.0 || x > 1.0));
}

void _CIasin(WinApplication* app, x86::CPU& cpu, Cdecl)
{
    const double x = double(cpu.fpu.pop());
    pushResult(cpu, domainChecked(app, cpu, x, std::asin(x), x < -1.0 || x > 1.0));
}

void _CIcosh(WinApplication* app, x86::CPU& cpu, Cdecl)
{
    const double x = double(cpu.fpu.pop());
    pushResult(cpu, domainChecked(app, cpu, x, std::cosh(x), false));
}

void _CIsinh(WinApplication* app, x86::CPU& cpu, Cdecl)
{
    const double x = double(cpu.fpu.pop());
    pushResult(cpu, domainChecked(app, cpu, x, std::sinh(x), false));
}

void _CItanh(WinApplication* app, x86::CPU& cpu, Cdecl)
{
    const double x = double(cpu.fpu.pop());
    pushResult(cpu, domainChecked(app, cpu, x, std::tanh(x), false));
}

// pow(st1, st0): a negative base with a non-integral exponent, and 0 to a negative power, are domain errors.
void _CIpow(WinApplication* app, x86::CPU& cpu, Cdecl)
{
    const double y = double(cpu.fpu.pop());
    const double x = double(cpu.fpu.pop());
    double result;
    if (std::isnan(x) || std::isnan(y))
    {
        result = (y == 0.0) ? 1.0 : std::isnan(x) ? x : y;
    }
    else if ((x < 0 && std::isfinite(y) && y != std::floor(y)) || (x == 0 && y < 0))
    {
        setErrno(app, cpu, ErrDom);
        result = x == 0 ? HUGE_VAL : indefinite();
    }
    else
    {
        result = std::pow(x, y);
        if (std::isinf(result) && std::isfinite(x) && std::isfinite(y))
            setErrno(app, cpu, ErrRange);
    }
    pushResult(cpu, result);
}

// fmod(st1, st0): exact; a zero divisor or an infinite dividend is a domain error.
void _CIfmod(WinApplication* app, x86::CPU& cpu, Cdecl)
{
    const double y = double(cpu.fpu.pop());
    const double x = double(cpu.fpu.pop());
    double result;
    if (std::isnan(x) || std::isnan(y))
    {
        result = std::isnan(x) ? x : y;
    }
    else if (y == 0 || std::isinf(x))
    {
        setErrno(app, cpu, ErrDom);
        result = indefinite();
    }
    else
    {
        result = std::fmod(x, y);
    }
    pushResult(cpu, result);
}

int _isnan(WinApplication* app, x86::CPU& cpu, Cdecl)
{
    RC_USE(app);
    return std::isnan(argumentDouble(cpu, 0)) ? 1 : 0;
}

UINT _control87(WinApplication* app, x86::CPU& cpu, Cdecl, UINT value, UINT mask)
{
    RC_USE(app);
    return control(cpu, value, mask);
}

// _controlfp leaves the denormal mask alone.
UINT _controlfp(WinApplication* app, x86::CPU& cpu, Cdecl, UINT value, UINT mask)
{
    RC_USE(app);
    return control(cpu, value, mask & ~0x80000u);
}

// ---- Miscellaneous -------------------------------------------------------------------------------------------------

int rand(WinApplication* app, x86::CPU& cpu, Cdecl)
{
    ThreadData& data = threadDataOf(app, cpu);
    data.randSeed = data.randSeed * 214013u + 2531011u;
    return int((data.randSeed >> 16) & 0x7fff);
}

void srand(WinApplication* app, x86::CPU& cpu, Cdecl, UINT seed)
{
    threadDataOf(app, cpu).randSeed = seed;
}

// The runtime's quicksort: middle element as pivot, insertion of the maximum below 9 elements, the smaller part
// first on an explicit stack.
void qsort(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 base, UINT count, UINT width, x86::reg32 compare)
{
    if (count < 2 || !width)
        return;
    x86::reg32 lowStack[32], highStack[32];
    int top = 0;
    x86::reg32 lo = base;
    x86::reg32 hi = base + width * (count - 1);
    for (;;)
    {
        const x86::reg32 size = (hi - lo) / width + 1;
        if (size <= 8)
        {
            for (x86::reg32 h = hi; h > lo; h -= width)
            {
                x86::reg32 max = lo;
                for (x86::reg32 p = lo + width; p <= h; p += width)
                {
                    if (compareElements(app, cpu, compare, p, max) > 0)
                        max = p;
                    if (cpu.terminate)
                        return;
                }
                swapElements(max, h, width);
            }
        }
        else
        {
            const x86::reg32 mid = lo + (size / 2) * width;
            swapElements(mid, lo, width);
            x86::reg32 loguy = lo, higuy = hi + width;
            for (;;)
            {
                do
                {
                    loguy += width;
                } while (loguy <= hi && compareElements(app, cpu, compare, loguy, lo) <= 0 && !cpu.terminate);
                do
                {
                    higuy -= width;
                } while (higuy > lo && compareElements(app, cpu, compare, higuy, lo) >= 0 && !cpu.terminate);
                if (cpu.terminate)
                    return;
                if (higuy < loguy)
                    break;
                swapElements(loguy, higuy, width);
            }
            swapElements(lo, higuy, width);
            if (higuy - 1 - lo >= hi - loguy)
            {
                if (lo + width < higuy)
                {
                    lowStack[top] = lo;
                    highStack[top] = higuy - width;
                    ++top;
                }
                if (loguy < hi)
                {
                    lo = loguy;
                    continue;
                }
            }
            else
            {
                if (loguy < hi)
                {
                    lowStack[top] = loguy;
                    highStack[top] = hi;
                    ++top;
                }
                if (lo + width < higuy)
                {
                    hi = higuy - width;
                    continue;
                }
            }
        }
        if (--top < 0)
            return;
        lo = lowStack[top];
        hi = highStack[top];
    }
}

x86::reg32 bsearch(WinApplication* app, x86::CPU& cpu, Cdecl, x86::reg32 key, x86::reg32 base, UINT count, UINT width,
                   x86::reg32 compare)
{
    x86::reg32 lo = base;
    x86::reg32 hi = base + (count - 1) * width;
    UINT num = count;
    while (count && lo <= hi)
    {
        const UINT half = num / 2;
        if (half)
        {
            const x86::reg32 mid = lo + ((num & 1) ? half : half - 1) * width;
            const int result = compareElements(app, cpu, compare, key, mid);
            if (cpu.terminate)
                return 0;
            if (!result)
                return mid;
            if (result < 0)
            {
                hi = mid - width;
                num = (num & 1) ? half : half - 1;
            }
            else
            {
                lo = mid + width;
                num = half;
            }
        }
        else if (num)
        {
            return compareElements(app, cpu, compare, key, lo) ? 0 : lo;
        }
        else
        {
            break;
        }
    }
    return 0;
}

x86::reg32 _errno(WinApplication* app, x86::CPU& cpu, Cdecl)
{
    return threadData(app, cpu) + x86::reg32(offsetof(ThreadData, errnoValue));
}

}}
