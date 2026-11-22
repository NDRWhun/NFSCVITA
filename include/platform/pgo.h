#ifndef PLATFORM_PGO_H_
#define PLATFORM_PGO_H_

// Instrumented build (NFSC_PGO_GENERATE): writes the profile counters to a .profraw file (clang) or a gcov-tool
// merge-stream file (gcc, NFSC_PGO_GCOV).
namespace pf { namespace pgo
{

#ifdef NFSC_PGO_GCOV
constexpr const char* FileName = "nfsc.gcov";
#else
constexpr const char* FileName = "nfsc.profraw";
#endif

// Writes the profile to path every 30 seconds from a low-priority thread.
void start(const char* path);
// Writes the profile now.
void write();

}}

#endif
