#ifndef NFSC_DEFER_H_
#define NFSC_DEFER_H_

// The sim posts a world, a render and a camera update event at the end of every sim step, and the event flush runs
// each between the steps. With display_defer=1 (the default) the three updates are queued with their step's time
// instead and run in the same order once the flush that ran the steps returns, so they see the state after the
// frame's last step. display_defer=0 runs them between the steps.
namespace nfsc { namespace defer {

// Reads the setting; before guest code runs.
void configure();

}}

#endif
