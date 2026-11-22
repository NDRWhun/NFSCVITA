#ifndef NFSC_POOLS_H_
#define NFSC_POOLS_H_

// The game's memory pools: the car pool's size, and the pools in the guest memory log lines (pools.cpp).

namespace nfsc { namespace pools {

// Main thread, before guest code runs: sets the car pool's size (car_pool_kb); every guest memory log line is
// followed by the pools' sizes.
void install();

}}

#endif /* !NFSC_POOLS_H_ */
