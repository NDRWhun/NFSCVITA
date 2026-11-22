#ifndef LIB_SHIMGATE_H_
#define LIB_SHIMGATE_H_

// A gate every shim call passes (win32::Wrapper) while one is installed: it may take a lock for the call and release
// it after. The game installs one while its display call runs without the execution lock.

#include <lib/winapp.h>
#include <atomic>
#include <cstdint>

namespace win32 { namespace shimgate
{

// Each shim's slot for the installer's class of it, -1 until the first call fills it.
typedef std::atomic<int8_t> Slot;

// Called before the shim with its wrapper function and slot; a nonzero token is handed to leave after the shim
// returns.
typedef int (*EnterFn)(WinApplication* app, const x86::CPU& cpu, MethodPtr shim, Slot& slot);
typedef void (*LeaveFn)(WinApplication* app, const x86::CPU& cpu, int token);

// Nonzero while the gate is open for checks; set and cleared by the installer's thread.
extern std::atomic<int> g_open;
extern EnterFn g_enter;
extern LeaveFn g_leave;

// Before guest code runs.
void install(EnterFn enter, LeaveFn leave);

}

class ShimGate
{
public:
    ShimGate(WinApplication* app, const x86::CPU& cpu, MethodPtr shim, shimgate::Slot& slot)
        :   m_app(app)
        ,   m_cpu(cpu)
        ,   m_token(0)
    {
        if (__builtin_expect(shimgate::g_open.load(std::memory_order_relaxed) != 0, 0))
            m_token = shimgate::g_enter(app, cpu, shim, slot);
    }
    ~ShimGate()
    {
        if (m_token)
            shimgate::g_leave(m_app, m_cpu, m_token);
    }

private:
    ShimGate(const ShimGate&) = delete;
    ShimGate& operator=(const ShimGate&) = delete;
    WinApplication*     m_app;
    const x86::CPU&     m_cpu;
    int                 m_token;
};

}

#endif
