#ifndef NFSC_MIXRATE_H_
#define NFSC_MIXRATE_H_

// Sample rate of the game's audio mixer (mixrate.cpp).

namespace nfsc { namespace mixrate {

// Main thread, before guest code runs: the mixer runs at mix_rate (nfscvita.cfg, 24000 by default).
void install();

}}

#endif /* !NFSC_MIXRATE_H_ */
