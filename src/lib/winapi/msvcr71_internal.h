#ifndef WINAPI_MSVCR71_INTERNAL_H_
#define WINAPI_MSVCR71_INTERNAL_H_

// Shared parts of the msvcr71 shims: guest strings, per-thread runtime data, the printf and scanf engines and the
// stream table.

#include <winapi/msvcr71.h>
#include <lib/memmap.h>
#include <lib/winapp.h>
#include <string>

namespace win32 { namespace msvcr71
{

// Host pointer of a guest address.
inline char* guestChars(x86::reg32 address)
{
    return reinterpret_cast<char*>(g_guestBase + address);
}

// Guest address of a host pointer into guest memory.
inline x86::reg32 guestAddress(const void* pointer)
{
    return x86::reg32(static_cast<const x86::reg8*>(pointer) - g_guestBase);
}

// Per-thread runtime data in guest memory, keyed by the guest thread id of cpu.
struct ThreadData
{
    x86::reg32 errnoValue;
    x86::reg32 strtokNext;
    x86::reg32 randSeed;
    x86::reg32 tm[9];
};
// Guest address of the calling thread's data.
x86::reg32 threadData(WinApplication* app, const x86::CPU& cpu);
inline ThreadData& threadDataOf(WinApplication* app, const x86::CPU& cpu)
{
    return *reinterpret_cast<ThreadData*>(g_guestBase + threadData(app, cpu));
}
void setErrno(WinApplication* app, const x86::CPU& cpu, int value);

// errno values of the Microsoft C runtime.
enum : int
{
    ErrNoEnt = 2, ErrBadF = 9, ErrNoMem = 12, ErrAccess = 13, ErrExist = 17, ErrInval = 22, ErrMFile = 24,
    ErrNoSpc = 28, ErrDom = 33, ErrRange = 34, ErrNotEmpty = 41
};
// errno of a newlib errno value.
int crtErrno(int hostErrno);

// Guest variadic arguments read in order from a guest va_list.
class GuestArgs
{
public:
    explicit GuestArgs(x86::reg32 address) : m_address(address) {}
    x86::reg32 next32()
    {
        x86::reg32 value;
        __builtin_memcpy(&value, g_guestBase + m_address, 4);
        m_address += 4;
        return value;
    }
    x86::reg64 next64()
    {
        x86::reg64 value;
        __builtin_memcpy(&value, g_guestBase + m_address, 8);
        m_address += 8;
        return value;
    }
    double nextDouble()
    {
        const x86::reg64 bits = next64();
        double value;
        __builtin_memcpy(&value, &bits, 8);
        return value;
    }

private:
    x86::reg32 m_address;
};

// printf formatting of the Microsoft C runtime 7.1 for a narrow or a wide (UTF-16) format string at a guest
// address; the result is in narrow bytes or UTF-16 units.
std::string formatNarrow(WinApplication* app, x86::reg32 format, GuestArgs& args);
std::u16string formatWide(WinApplication* app, x86::reg32 format, GuestArgs& args);
// scanf over a narrow string; returns the number of assigned fields, or -1 at input end before the first.
int scanNarrow(const char* input, const char* format, GuestArgs& args);

// The stdio FILE table and the value of _fmode (msvcr71.cpp; the variables exist once the imports are bound).
x86::reg32 iobAddress();
int fileMode();

// Writes text to a guest stream (FILE*); false on an error.
bool streamWrite(WinApplication* app, const x86::CPU& cpu, x86::reg32 stream, const char* data, size_t length);

}}

#endif
