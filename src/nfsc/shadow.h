#pragma once

#include <native.h>
#include <recomp.h>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <vector>

namespace nfsc
{
namespace shadow
{

// Guest memory a compared function may write.
struct Region
{
    x86::reg32 address, size;
};

// Registers compared, by bit in the order eax, ecx, edx, ebx, esp, ebp, esi, edi.
constexpr x86::reg32 AllRegisters = 0xff;

// A group of native ports compared with their translated bodies under one switch (shadow_<group>): each call runs
// the translated body on a copy of the CPU, then the native one from the same guest memory, and compares the
// registers, the x87 depth and st0, and the memory either may write.
class Group
{
public:
    // After the translated run: whether the call can run again (false when the guest callees it reached changed
    // memory outside the compared regions); context is the caller's.
    using Repeatable = bool (*)(const void* context);

    explicit Group(const char* key) : key_(key) {}

    // The switch is on and no call of this group is being compared.
    bool active();

    // With repeatable returning false the translated run's result stands and the call counts as not compared.
    // scratch: guest memory the translated body may write and the port reads, dead after the call (an argument
    // slot): restored before the port runs, not compared.
    void call(const char* name, win32::WinApplication* app, x86::CPU& cpu, win32::MethodPtr guest,
              win32::MethodPtr port, const Region* regions, size_t count, x86::reg32 live = AllRegisters,
              Repeatable repeatable = nullptr, const void* context = nullptr, const Region* scratch = nullptr,
              size_t scratchCount = 0);

    void call(const char* name, win32::WinApplication* app, x86::CPU& cpu, win32::MethodPtr guest,
              win32::MethodPtr port, std::initializer_list<Region> regions, x86::reg32 live = AllRegisters,
              Repeatable repeatable = nullptr, const void* context = nullptr)
    {
        call(name, app, cpu, guest, port, regions.begin(), regions.size(), live, repeatable, context);
    }

    void call(const char* name, win32::WinApplication* app, x86::CPU& cpu, win32::MethodPtr guest,
              win32::MethodPtr port, const std::vector<Region>& regions, x86::reg32 live = AllRegisters,
              Repeatable repeatable = nullptr, const void* context = nullptr)
    {
        call(name, app, cpu, guest, port, regions.data(), regions.size(), live, repeatable, context);
    }

    // Logs how many calls were compared and how many differed (with the switch on).
    void log() const;

    // For ports that compare their results themselves: logs a difference of the current call (counted in found),
    // then tally counts the call.
    void report(const char* name, const char* what, x86::reg32 guest, x86::reg32 native, int& found);
    void tally(int found, bool nanOnly);

private:
    const char* key_;
    int on_ = -1;
    bool running_ = false;
    uint32_t compared_ = 0, differ_ = 0, nanOnly_ = 0, kept_ = 0;
};

}
}
