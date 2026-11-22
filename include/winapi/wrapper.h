#ifndef WINAPI_WRAPPER_H_
#define WINAPI_WRAPPER_H_

#include <lib/shimgate.h>
#include <lib/teb.h>
#include <lib/winapp.h>
#include <type_traits>
#include <utility>


namespace win32
{

// First parameter of a cdecl shim: the caller removes the arguments, the shim pops only the return address. A
// 64-bit result goes to edx:eax.
struct Cdecl
{
};

template< typename T >
struct Unpacker
{
    static inline T unpack(WinApplication* app, x86::reg32 address);
};

// Keeps the guest return address of the call in the thread block, for the hang watchdog.
inline void noteShimCall(WinApplication* app, const x86::CPU& cpu)
{
    app->getMemory<x86::reg32>(cpu.efs + teb::LastShimCall) = app->getMemory<x86::reg32>(cpu.esp);
}

template< typename T, T t >
struct Wrapper;


template< typename ...Us>
struct Length;

template< typename U, typename ...Us>
struct Length<U, Us...>
{
    enum { L = 1 + Length<Us...>::L };
};
template<>
struct Length<>
{
    enum { L = 0 };
};

template< typename T, unsigned I >
struct Pair
{
    typedef T Type;
    enum { Index = I };
};

template< typename R, typename Owner, typename F, F f, unsigned I, typename ...Us >
struct Packer;

template< typename R, typename Owner, typename F, F f, unsigned I, typename U, typename ...Us >
struct Packer<R, Owner, F, f, I, U, Us...>
{
    template< typename ...Pairs >
    struct Pack : public Packer<R, Owner, F, f, I+1, Us...>::template Pack< Pairs..., Pair<U, I> >
    {
    };
};

template< typename R, typename Owner, typename F, F f, unsigned I >
struct Packer<R, Owner, F, f, I>
{
    template< typename ...Pairs >
    struct Pack
    {
        static void stdcall(WinApplication* app, x86::CPU& cpu)
        {
            noteShimCall(app, cpu);
            static shimgate::Slot slot{-1};
            const ShimGate gate(app, cpu, &stdcall, slot);
            Owner* owner = Unpacker<Owner*>::unpack(app, cpu.esp + 4);
            cpu.eax = (owner->*f)(app, cpu, Unpacker<typename Pairs::Type>::unpack(app, cpu.esp + 8 + 4*Pairs::Index)...);
            cpu.esp += 8 + 4*Length<Pairs...>::L;
        }
    };
};

template< typename Owner, typename F, F f, unsigned I >
struct Packer<void, Owner, F, f, I>
{
    template< typename ...Pairs >
    struct Pack
    {
        static void stdcall(WinApplication* app, x86::CPU& cpu)
        {
            noteShimCall(app, cpu);
            static shimgate::Slot slot{-1};
            const ShimGate gate(app, cpu, &stdcall, slot);
            Owner* owner = Unpacker<Owner*>::unpack(app, cpu.esp + 4);
            (owner->*f)(app, cpu, Unpacker<typename Pairs::Type>::unpack(app, cpu.esp + 8 + 4*Pairs::Index)...);
            cpu.esp += 8 + 4*Length<Pairs...>::L;
        }
    };
};

template< typename R, typename F, F f, unsigned I >
struct Packer<R, void, F, f, I>
{
    template< typename ...Pairs >
    struct Pack
    {
        static void stdcall(WinApplication* app, x86::CPU& cpu)
        {
            noteShimCall(app, cpu);
            static shimgate::Slot slot{-1};
            const ShimGate gate(app, cpu, &stdcall, slot);
            cpu.eax = (*f)(app, cpu, Unpacker<typename Pairs::Type>::unpack(app, cpu.esp + 4 + 4*Pairs::Index)...);
            cpu.esp += 4 + 4*Length<Pairs...>::L;
        }
    };
};

template< typename F, F f, unsigned I >
struct Packer<void, void, F, f, I>
{
    template< typename ...Pairs >
    struct Pack
    {
        static void stdcall(WinApplication* app, x86::CPU& cpu)
        {
            noteShimCall(app, cpu);
            static shimgate::Slot slot{-1};
            const ShimGate gate(app, cpu, &stdcall, slot);
            (*f)(app, cpu, Unpacker<typename Pairs::Type>::unpack(app, cpu.esp + 4 + 4*Pairs::Index)...);
            cpu.esp += 4 + 4*Length<Pairs...>::L;
        }
    };
};


template< typename R, typename ...T, R (*F)(WinApplication*, x86::CPU&, T...) >
struct Wrapper<R (*)(WinApplication*, x86::CPU&, T...), F>
    :   public Packer<R, void, R (*)(WinApplication*, x86::CPU&, T...), F, 0, T...>::template Pack<>
{
};

template< typename R, typename Owner, typename ...T, R (Owner::*F)(WinApplication*, x86::CPU&, T...) >
struct Wrapper<R (Owner::*)(WinApplication*, x86::CPU&, T...), F>
    :   public Packer<R, Owner, R (Owner::*)(WinApplication*, x86::CPU&, T...), F, 0, T...>::template Pack<>
{
};

template< typename R, typename Owner, typename ...T, R (Owner::*F)(WinApplication*, x86::CPU&, T...) const >
struct Wrapper<R (Owner::*)(WinApplication*, x86::CPU&, T...) const, F>
    :   public Packer<R, const Owner, R (Owner::*)(WinApplication*, x86::CPU&, T...), F, 0, T...>::template Pack<>
{
};

template< typename R, typename ...T, R (*F)(WinApplication*, x86::CPU&, Cdecl, T...) >
struct Wrapper<R (*)(WinApplication*, x86::CPU&, Cdecl, T...), F>
{
    static void stdcall(WinApplication* app, x86::CPU& cpu)
    {
        noteShimCall(app, cpu);
        static shimgate::Slot slot{-1};
        const ShimGate gate(app, cpu, &stdcall, slot);
        call(app, cpu, std::index_sequence_for<T...>{});
    }

private:
    template< size_t ...I >
    static void call(WinApplication* app, x86::CPU& cpu, std::index_sequence<I...>)
    {
        if constexpr (std::is_void<R>::value)
            (*F)(app, cpu, Cdecl{}, Unpacker<T>::unpack(app, cpu.esp + 4 + 4 * I)...);
        else if constexpr (sizeof(R) == 8)
            cpu.edx_eax = x86::reg64((*F)(app, cpu, Cdecl{}, Unpacker<T>::unpack(app, cpu.esp + 4 + 4 * I)...));
        else
            cpu.eax = x86::reg32((*F)(app, cpu, Cdecl{}, Unpacker<T>::unpack(app, cpu.esp + 4 + 4 * I)...));
        cpu.esp += 4;
    }
};


template< >
struct Unpacker<x86::reg8>
{
    static inline x86::reg8 unpack(WinApplication* app, x86::reg32 address)
    {
        return app->getMemory<x86::reg8>(address);
    }
};

template< >
struct Unpacker<x86::reg16>
{
    static inline x86::reg16 unpack(WinApplication* app, x86::reg32 address)
    {
        return app->getMemory<x86::reg16>(address);
    }
};

template< >
struct Unpacker<x86::reg32>
{
    static inline x86::reg32 unpack(WinApplication* app, x86::reg32 address)
    {
        return app->getMemory<x86::reg32>(address);
    }
};

template< >
struct Unpacker<x86::sreg8>
{
    static inline x86::sreg8 unpack(WinApplication* app, x86::reg32 address)
    {
        return app->getMemory<x86::sreg8>(address);
    }
};

template< >
struct Unpacker<x86::sreg16>
{
    static inline x86::sreg16 unpack(WinApplication* app, x86::reg32 address)
    {
        return app->getMemory<x86::sreg16>(address);
    }
};

template< >
struct Unpacker<x86::sreg32>
{
    static inline x86::sreg32 unpack(WinApplication* app, x86::reg32 address)
    {
        return app->getMemory<x86::sreg32>(address);
    }
};

template< >
struct Unpacker<float>
{
    static inline float unpack(WinApplication* app, x86::reg32 address)
    {
        return app->getMemory<float>(address);
    }
};

template< typename T >
struct Unpacker< Packed<T> >
{
    static inline Packed<T> unpack(WinApplication* app, x86::reg32 address)
    {
        return Packed<T>(app->getMemory<x86::reg32>(address));
    }
};

template< typename T >
struct Unpacker<T*>
{
    static inline T* unpack(WinApplication* app, x86::reg32 address)
    {
        x86::reg32 a = app->getMemory<x86::reg32>(address);
        return a == 0 ? nullptr : &app->getMemory<T>(a);
    }
};

template< typename T >
struct Unpacker<const T*>
{
    static inline const T* unpack(WinApplication* app, x86::reg32 address)
    {
        x86::reg32 a = app->getMemory<x86::reg32>(address);
        return a == 0 ? nullptr : &app->getMemory<const T>(a);
    }
};

}

#endif
