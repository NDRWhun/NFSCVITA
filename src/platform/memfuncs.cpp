#include <psp2/kernel/clib.h>
#include <cstddef>

// The compiler lowers memmove to these run-time helpers; the toolchain's own version copies one byte at a time.
// They forward to the system library's copy, as memmove itself already does.
extern "C"
{

void __aeabi_memmove(void* destination, const void* source, size_t count)
{
    sceClibMemmove(destination, source, count);
}

void __aeabi_memmove4(void* destination, const void* source, size_t count)
{
    sceClibMemmove(destination, source, count);
}

void __aeabi_memmove8(void* destination, const void* source, size_t count)
{
    sceClibMemmove(destination, source, count);
}

}
