#ifndef NFSC_MIXSPLIT_H_
#define NFSC_MIXSPLIT_H_

// The audio mixer's SND11 block service on a worker thread (mixsplit.cpp).
namespace nfsc { namespace mixsplit {

// Host priority of the block service worker; 0 before it exists.
int workerPriority();

}}

#endif
