#include <lib/shimgate.h>

namespace win32 { namespace shimgate
{

std::atomic<int> g_open{0};
EnterFn g_enter = nullptr;
LeaveFn g_leave = nullptr;

void install(EnterFn enter, LeaveFn leave)
{
    g_enter = enter;
    g_leave = leave;
}

}}
