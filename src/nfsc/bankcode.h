#ifndef NFSC_BANKCODE_H_
#define NFSC_BANKCODE_H_

// Runs the x86 routines inside the loaded ABKC sound banks (bankcode.cpp).

#include <cstdint>

namespace win32 { class WinApplication; }
namespace x86 { struct CPU; }

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

}}

#endif /* !NFSC_BANKCODE_H_ */
