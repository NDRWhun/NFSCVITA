// msvcp71.dll: std::basic_string<char> of the C++ library 7.1. Layout: +0 allocator, +4 the buffer (capacity below
// 16) or the pointer to the heap block, +20 size, +24 capacity. Blocks come from the C runtime's heap, so code in
// the executable that frees or grows them inline gets the same blocks.

#include <winapi/msvcp71.h>
#include <winapi/msvcr71.h>
#include <lib/memmap.h>
#include <recomp.h>
#include <cstring>

namespace win32 { namespace msvcp71
{

namespace
{

enum : x86::reg32
{
    BufferSize = 16,
    OffsetBuffer = 4,
    OffsetSize = 20,
    OffsetCapacity = 24
};

// _Tidy(): the empty string in the buffer.
void tidy(x86::reg32 self, bool built)
{
    if (built && rc::ld32(self + OffsetCapacity) >= BufferSize)
        msvcr71::release(rc::ld32(self + OffsetBuffer));
    rc::st32(self + OffsetCapacity, BufferSize - 1);
    rc::st32(self + OffsetSize, 0);
    rc::st8(self + OffsetBuffer, 0);
}

// assign(chars, count) on an empty string: _Grow takes count | 15 as the capacity.
void assign(x86::reg32 self, const char* chars, x86::reg32 count)
{
    x86::reg32 data = self + OffsetBuffer;
    if (count >= BufferSize)
    {
        const x86::reg32 capacity = count | (BufferSize - 1);
        data = msvcr71::allocate(capacity + 1);
        if (!data)
            x86::guestTrap(self, "basic_string: guest heap exhausted");
        rc::st32(self + OffsetBuffer, data);
        rc::st32(self + OffsetCapacity, capacity);
    }
    std::memmove(g_guestBase + data, chars, count);
    rc::st8(data + count, 0);
    rc::st32(self + OffsetSize, count);
}

const char* chars(x86::reg32 string)
{
    const x86::reg32 data = rc::ld32(string + OffsetCapacity) >= BufferSize ? rc::ld32(string + OffsetBuffer)
                                                                            : string + OffsetBuffer;
    return reinterpret_cast<const char*>(g_guestBase + data);
}

}

void string_from_chars(WinApplication* app, x86::CPU& cpu, x86::reg32 text)
{
    RC_USE(app);
    const x86::reg32 self = cpu.ecx;
    tidy(self, false);
    const char* source = reinterpret_cast<const char*>(g_guestBase + text);
    assign(self, source, x86::reg32(std::strlen(source)));
    cpu.eax = self;
}

void string_copy(WinApplication* app, x86::CPU& cpu, x86::reg32 other)
{
    RC_USE(app);
    const x86::reg32 self = cpu.ecx;
    tidy(self, false);
    assign(self, chars(other), rc::ld32(other + OffsetSize));
    cpu.eax = self;
}

void string_destroy(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    tidy(cpu.ecx, true);
}

}}
