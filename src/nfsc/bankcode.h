#ifndef NFSC_BANKCODE_H_
#define NFSC_BANKCODE_H_

// Runs the x86 routines inside the loaded ABKC sound banks (bankcode.cpp).

#include <x86.h>

#include <cstdint>

namespace win32 { class WinApplication; }

namespace nfsc { namespace bankcode {

// Main thread, before guest code runs: lets calls through pointers reach the routines of loaded banks.
// bank_native=0 interprets every routine.
void install();

// A recompiled bank routine (generated nfsc_bankcode.cpp), keyed by the hash of its reachable code.
struct NativeRoutine
{
    uint64_t hash;
    void (*method)(win32::WinApplication* app, x86::CPU& cpu);
};

// Sorted by hash.
extern const NativeRoutine g_nativeRoutines[];
extern const unsigned g_nativeRoutineCount;

// Library functions the recompiled routines call natively (banklib.cpp): args is the guest address of the stack
// argument; returns eax. Registers, flags and the x87 state are left as they were.
x86::reg32 lib_897ba8(win32::WinApplication* app, x86::CPU& cpu, x86::reg32 args);
x86::reg32 lib_897bb7(win32::WinApplication* app, x86::CPU& cpu, x86::reg32 args);
x86::reg32 lib_897e88(win32::WinApplication* app, x86::CPU& cpu, x86::reg32 args);
x86::reg32 lib_897eca(win32::WinApplication* app, x86::CPU& cpu, x86::reg32 args);
x86::reg32 lib_897fda(win32::WinApplication* app, x86::CPU& cpu, x86::reg32 args);
x86::reg32 lib_898014(win32::WinApplication* app, x86::CPU& cpu, x86::reg32 args);
x86::reg32 lib_89805f(win32::WinApplication* app, x86::CPU& cpu, x86::reg32 args);
x86::reg32 lib_898090(win32::WinApplication* app, x86::CPU& cpu, x86::reg32 args);
x86::reg32 lib_89819f(win32::WinApplication* app, x86::CPU& cpu, x86::reg32 args);
x86::reg32 lib_89832d(win32::WinApplication* app, x86::CPU& cpu, x86::reg32 args);
x86::reg32 lib_89834b(win32::WinApplication* app, x86::CPU& cpu, x86::reg32 args);
x86::reg32 lib_8983b8(win32::WinApplication* app, x86::CPU& cpu, x86::reg32 args);
x86::reg32 lib_8983f4(win32::WinApplication* app, x86::CPU& cpu, x86::reg32 args);
x86::reg32 lib_898423(win32::WinApplication* app, x86::CPU& cpu, x86::reg32 args);
x86::reg32 lib_898440(win32::WinApplication* app, x86::CPU& cpu, x86::reg32 args);
x86::reg32 lib_898487(win32::WinApplication* app, x86::CPU& cpu, x86::reg32 args);
x86::reg32 lib_89849f(win32::WinApplication* app, x86::CPU& cpu, x86::reg32 args);
x86::reg32 lib_8984ec(win32::WinApplication* app, x86::CPU& cpu, x86::reg32 args);
x86::reg32 lib_8985d6(win32::WinApplication* app, x86::CPU& cpu, x86::reg32 args);
x86::reg32 lib_898675(win32::WinApplication* app, x86::CPU& cpu, x86::reg32 args);
x86::reg32 lib_89869f(win32::WinApplication* app, x86::CPU& cpu, x86::reg32 args);
x86::reg32 lib_8986b5(win32::WinApplication* app, x86::CPU& cpu, x86::reg32 args);

// Main thread, from install: banklib_native and shadow_banklib.
void installLibrary();

// Logs the shadow_banklib counts.
void logShadow();

}}

#endif /* !NFSC_BANKCODE_H_ */
