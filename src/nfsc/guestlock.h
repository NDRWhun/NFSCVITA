#ifndef NFSC_GUESTLOCK_H_
#define NFSC_GUESTLOCK_H_

#include "audioupdate.h"
#include "display.h"

#include <lib/winapp.h>
#include <platform/perf.h>
#include <platform/sync.h>

namespace nfsc
{

// Lock attempts before a contended host lock waits in the kernel.
const int LockSpinTries = 64;

// Releases every level of the execution lock the calling thread holds for its lifetime (none on a thread running
// without it).
class ContextRelease
{
public:
    ContextRelease(win32::WinApplication* app, const x86::CPU& cpu)
        :   m_app(app)
        ,   m_cpu(cpu)
        ,   m_depth(app->contextDepth())
    {
        for (int i = 0; i < m_depth; ++i)
            m_app->unlockContext(m_cpu);
    }
    ~ContextRelease()
    {
        for (int i = 0; i < m_depth; ++i)
            m_app->lockContext(m_cpu);
    }

private:
    ContextRelease(const ContextRelease&) = delete;
    ContextRelease& operator=(const ContextRelease&) = delete;
    win32::WinApplication*  m_app;
    const x86::CPU&         m_cpu;
    const int               m_depth;
};

// Takes a host lock; a thread holding the execution lock waits for it with the execution lock released, as for a
// guest critical section: the owner may need the execution lock to finish. Waits are counted for perf_log; device:
// the Direct3D layer.
template< typename M >
inline void lockReleasingContext(M& mutex, win32::WinApplication* app, const x86::CPU& cpu, bool device = false)
{
    // A short spin first: the other thread's sections are short, a kernel wait and hand-over is not.
    for (int i = 0; i < LockSpinTries; ++i)
    {
        if (mutex.tryLock())
            return;
        __asm__ volatile("yield");
    }
    const bool timed = pf::perf::enabled();
    const uint32_t start = timed ? pf::perf::now() : 0;
    pf::perf::blocking(pf::perf::BlockCritSec);
    {
        const ContextRelease release(app, cpu);
        mutex.lock();
    }
    if (!timed)
        return;
    const bool display = display::onDisplayThread(cpu);
    const pf::perf::Overlap count = device ? pf::perf::OverlapDeviceWaits
                                  : display ? pf::perf::OverlapDisplayWaits : pf::perf::OverlapGameWaits;
    pf::perf::overlap(count, 1);
    pf::perf::overlap(pf::perf::Overlap(count + 1), pf::perf::now() - start);
}

// Holds a recursive host lock that guards game state.
class GuestLockGuard
{
public:
    GuestLockGuard(pf::LwMutex& mutex, win32::WinApplication* app, const x86::CPU& cpu)
        :   m_mutex(mutex)
    {
        lockReleasingContext(m_mutex, app, cpu);
    }
    ~GuestLockGuard()
    {
        m_mutex.unlock();
    }

private:
    GuestLockGuard(const GuestLockGuard&) = delete;
    GuestLockGuard& operator=(const GuestLockGuard&) = delete;
    pf::LwMutex& m_mutex;
};

// Holds the lock only while a display call (display_overlap) or an audio update (audio_parallel) runs beside the game
// thread; the flags change only on the game thread outside such guards.
class ConcurrentLockGuard
{
public:
    ConcurrentLockGuard(pf::LwMutex& mutex, win32::WinApplication* app, const x86::CPU& cpu)
        :   m_mutex(mutex)
        ,   m_locked(display::concurrent() || audioupdate::concurrent())
    {
        if (!m_locked)
            return;
        lockReleasingContext(m_mutex, app, cpu);
        if (pf::perf::enabled() && display::concurrent())
            pf::perf::overlap(pf::perf::OverlapStateLocks, 1);
    }
    ~ConcurrentLockGuard()
    {
        if (m_locked)
            m_mutex.unlock();
    }

private:
    ConcurrentLockGuard(const ConcurrentLockGuard&) = delete;
    ConcurrentLockGuard& operator=(const ConcurrentLockGuard&) = delete;
    pf::LwMutex& m_mutex;
    const bool m_locked;
};

}

#endif
