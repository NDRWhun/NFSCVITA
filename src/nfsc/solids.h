#ifndef NFSC_SOLIDS_H_
#define NFSC_SOLIDS_H_

// NotifySolidLoader natively (solids.cpp).

#include <lib/winapp.h>

namespace nfsc { namespace solids {

// NotifySolidLoader (0x55dc20, cdecl: the solid list header): with solid_notify_native=1 natively, else the
// translated body.
void notifySolidLoader(win32::WinApplication* app, x86::CPU& cpu);

}}

#endif /* !NFSC_SOLIDS_H_ */
