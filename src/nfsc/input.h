#ifndef NFSC_INPUT_H_
#define NFSC_INPUT_H_

// Vita pad bindings in the game's controller config, inputs held across a change between the front-end and driving
// controls, and the game's text fields on the on-screen keyboard. Layout in README.md.

namespace nfsc { namespace input {

// Main thread, before guest code runs: installs the pad poll hook.
void install();

}}

#endif /* !NFSC_INPUT_H_ */
