// x86 routines inside the ABKC sound banks, run by module instances from the bank tick (0x0089E2F2). The loader
// (0x00898A13) patches their calls to the library's function table, then makes each routine executable.

#include "bankcode.h"

#include <lib/foreigncode.h>
#include <lib/memmap.h>
#include <lib/winapp.h>
#include <platform/config.h>
#include <platform/log.h>
#include <recomp.h>
#include <x86.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace nfsc { namespace bankcode {

namespace {

const x86::reg32 LoadedBanks = 0x00BBEE14;  // first loaded bank's list node (the node lies at bank + 0x50)
const x86::reg32 BankNode    = 0x50;
const x86::reg32 BankName    = 0x48;        // copy of the name the bank was added with, or 0
const x86::reg32 BankMagic   = 0x434B4241;  // "ABKC"
const int        MaxBanks    = 4096;        // list walk bound
const x86::reg32 MaxRoutineBytes = 0x10000;

[[noreturn]] void unsupported(x86::reg32 bank, x86::reg32 ip, const char* what)
{
    pf::logError("bank code: %s at 0x%08x (bank 0x%08x + 0x%x: %02x %02x %02x %02x)", what, ip, bank, ip - bank,
                 rc::ld8(ip), rc::ld8(ip + 1), rc::ld8(ip + 2), rc::ld8(ip + 3));
    x86::guestTrap(ip, "bank code");
}

// One routine of a loaded bank: bytes [begin, end), entry index in the bank.
struct Location
{
    x86::reg32 bank;
    x86::reg32 entry;
    x86::reg32 index;
    x86::reg32 begin;
    x86::reg32 end;
};

// The loaded bank routine that starts at address. Header: entry count u16 +0x0A, first entry +0x1C; entry: routine
// +0x28 and its end +0x2C (absolute once loaded), next entry after 0x3C + 4 * (byte +0x24 + byte +0x27).
bool findRoutine(x86::reg32 address, Location& location)
{
    int banks = 0;
    for (x86::reg32 node = rc::ld32(LoadedBanks); node && banks < MaxBanks; node = rc::ld32(node), ++banks)
    {
        const x86::reg32 bank = node - BankNode;
        if (rc::ld32(bank) != BankMagic)
            continue;
        const x86::reg32 count = rc::ld16(bank + 0x0A);
        x86::reg32 entry = bank + rc::ld32(bank + 0x1C);
        for (x86::reg32 i = 0; i < count; ++i)
        {
            const x86::reg32 begin = rc::ld32(entry + 0x28);
            if (address == begin)
            {
                location = Location{ bank, entry, i, begin, rc::ld32(entry + 0x2C) };
                return true;
            }
            entry += 0x3C + 4 * (x86::reg32(rc::ld8(entry + 0x24)) + rc::ld8(entry + 0x27));
        }
    }
    return false;
}

enum class Op : uint8_t
{
    Push,       // 50+r  push r32
    Pop,        // 58+r  pop r32
    Store,      // 89    mov r/m32, r32
    Load,       // 8B    mov r32, r/m32
    Add,        // 03    add r32, r/m32
    Sub,        // 2B    sub r32, r/m32
    Cmp,        // 3B    cmp r32, r/m32
    AddImm,     // 81 /0, 83 /0  add r/m32, imm
    Imul,       // 0F AF imul r32, r/m32
    StoreByte,  // C6 /0 mov m8, imm8
    Jl,         // 7C    jl rel8
    Jg,         // 7F    jg rel8
    Call,       // E8    call rel32
    Ret,        // C3    ret
};

const uint8_t NoRegister = 0xff;

// One decoded instruction. A memory operand is base + (index << scale) + displacement, without base or index when
// their field says none. Jumps hold the index of their target instruction, calls the target address.
struct Instruction
{
    Op         op;
    uint8_t    reg;
    uint8_t    rm;          // register operand when !memory
    bool       memory;
    uint8_t    base;
    uint8_t    index;
    uint8_t    scale;
    x86::reg32 displacement;
    x86::reg32 immediate;
    x86::reg32 next;        // address of the following instruction
};

struct Routine
{
    Location                 location;
    std::vector<Instruction> code;     // by address; falling through goes to the next element
    uint64_t                 hash;     // of the reachable code, as the generator keys g_nativeRoutines
    win32::MethodPtr         native;   // recompiled routine, or nullptr
};

const uint64_t HashBasis = 0xcbf29ce484222325ull;
const uint64_t HashPrime = 0x100000001b3ull;

// FNV-1a 64 step.
inline uint64_t hashByte(uint64_t hash, x86::reg8 byte)
{
    return (hash ^ byte) * HashPrime;
}

// Decodes every instruction reachable from the routine's start and links the jumps.
class Decoder
{
public:
    explicit Decoder(const Location& location) : m_location(location) {}

    std::shared_ptr<Routine> run()
    {
        const x86::reg32 size = m_location.end - m_location.begin;
        if (m_location.end <= m_location.begin || size > MaxRoutineBytes)
            unsupported(m_location.bank, m_location.begin, "bad routine range");
        std::vector<int32_t> at(size, -1);       // instruction index by offset; -2 inside an instruction
        std::vector<Instruction> decoded;
        std::vector<x86::reg32> pending{ m_location.begin };
        while (!pending.empty())
        {
            x86::reg32 address = pending.back();
            pending.pop_back();
            for (;;)
            {
                if (address < m_location.begin || address >= m_location.end)
                    unsupported(m_location.bank, address, "jump out of the routine");
                const x86::reg32 offset = address - m_location.begin;
                if (at[offset] >= 0)
                    break;
                if (at[offset] == -2)
                    unsupported(m_location.bank, address, "jump into an instruction");
                Instruction in = decode(address);
                if (in.next > m_location.end)
                    unsupported(m_location.bank, address, "instruction past the routine end");
                for (x86::reg32 i = offset; i < in.next - m_location.begin; ++i)
                {
                    if (at[i] != -1)
                        unsupported(m_location.bank, address, "overlapping instructions");
                    at[i] = -2;
                }
                at[offset] = int32_t(decoded.size());
                decoded.push_back(in);
                if (in.op == Op::Ret)
                    break;
                if (in.op == Op::Jl || in.op == Op::Jg)
                    pending.push_back(in.immediate);
                address = in.next;
            }
        }
        std::shared_ptr<Routine> routine(new Routine);
        routine->location = m_location;
        routine->code.reserve(decoded.size());
        routine->native = nullptr;
        std::vector<int32_t> order(decoded.size());
        // Hash: per instruction in address order, its offset (4 bytes) and bytes, call operands as the target.
        uint64_t hash = HashBasis;
        for (x86::reg32 offset = 0; offset < size; ++offset)
        {
            if (at[offset] >= 0)
            {
                const Instruction& in = decoded[at[offset]];
                order[at[offset]] = int32_t(routine->code.size());
                routine->code.push_back(in);
                for (int i = 0; i < 4; ++i)
                    hash = hashByte(hash, x86::reg8(offset >> (8 * i)));
                const x86::reg32 address = m_location.begin + offset;
                for (x86::reg32 a = address; a < in.next; ++a)
                {
                    const x86::reg32 k = a - address - 1;
                    const bool operand = in.op == Op::Call && k < 4;
                    hash = hashByte(hash, operand ? x86::reg8(in.immediate >> (8 * k)) : rc::ld8(a));
                }
            }
        }
        routine->hash = hash;
        for (Instruction& in : routine->code)
        {
            if (in.op == Op::Jl || in.op == Op::Jg)
                in.immediate = x86::reg32(order[at[in.immediate - m_location.begin]]);
        }
        return routine;
    }

private:
    x86::reg8 fetch8()
    {
        return rc::ld8(m_ip++);
    }

    x86::reg32 fetch32()
    {
        const x86::reg32 value = rc::ld32(m_ip);
        m_ip += 4;
        return value;
    }

    // The instruction at address; traps on any form the routines do not use.
    Instruction decode(x86::reg32 address)
    {
        Instruction in{};
        in.base = NoRegister;
        in.index = NoRegister;
        m_ip = address;
        const x86::reg8 op = fetch8();
        if (op >= 0x50 && op <= 0x57)
        {
            in.op = Op::Push;
            in.reg = op & 7;
        }
        else if (op >= 0x58 && op <= 0x5F)
        {
            in.op = Op::Pop;
            in.reg = op & 7;
        }
        else
        {
            switch (op)
            {
            case 0x89: in.op = Op::Store; modrm(in); break;
            case 0x8B: in.op = Op::Load;  modrm(in); break;
            case 0x03: in.op = Op::Add;   modrm(in); break;
            case 0x2B: in.op = Op::Sub;   modrm(in); break;
            case 0x3B: in.op = Op::Cmp;   modrm(in); break;
            case 0x81:
            case 0x83:
                in.op = Op::AddImm;
                modrm(in);
                if (in.reg != 0)
                    unsupported(m_location.bank, address, "group 1 operation");
                in.immediate = op == 0x81 ? fetch32() : x86::reg32(x86::sreg32(x86::sreg8(fetch8())));
                break;
            case 0x0F:
                if (fetch8() != 0xAF)
                    unsupported(m_location.bank, address, "two-byte opcode");
                in.op = Op::Imul;
                modrm(in);
                break;
            case 0xC6:
                in.op = Op::StoreByte;
                modrm(in);
                if (in.reg != 0 || !in.memory)
                    unsupported(m_location.bank, address, "C6 form");
                in.immediate = fetch8();
                break;
            case 0x7C:
            case 0x7F:
            {
                in.op = op == 0x7C ? Op::Jl : Op::Jg;
                const x86::reg32 rel = x86::reg32(x86::sreg32(x86::sreg8(fetch8())));
                in.immediate = m_ip + rel;
                break;
            }
            case 0xE8:
            {
                in.op = Op::Call;
                const x86::reg32 rel = fetch32();
                in.immediate = m_ip + rel;
                break;
            }
            case 0xC3:
                in.op = Op::Ret;
                break;
            default:
                unsupported(m_location.bank, address, "instruction");
            }
        }
        in.next = m_ip;
        return in;
    }

    // ModRM (and SIB, displacement) at m_ip.
    void modrm(Instruction& in)
    {
        const x86::reg8 byte = fetch8();
        const int mod = byte >> 6;
        const int rm = byte & 7;
        in.reg = (byte >> 3) & 7;
        if (mod == 3)
        {
            in.memory = false;
            in.rm = uint8_t(rm);
            return;
        }
        in.memory = true;
        if (rm == 4)
        {
            const x86::reg8 sib = fetch8();
            const int base = sib & 7;
            const int index = (sib >> 3) & 7;
            if (base == 5 && mod == 0)
                in.displacement = fetch32();
            else
                in.base = uint8_t(base);
            if (index != 4)
            {
                in.index = uint8_t(index);
                in.scale = uint8_t(sib >> 6);
            }
        }
        else if (rm == 5 && mod == 0)
        {
            in.displacement = fetch32();
        }
        else
        {
            in.base = uint8_t(rm);
        }
        if (mod == 1)
            in.displacement += x86::reg32(x86::sreg32(x86::sreg8(fetch8())));
        else if (mod == 2)
            in.displacement += fetch32();
    }

    Location   m_location;
    x86::reg32 m_ip = 0;
};

// Arithmetic flags kept as the last operation's operands until something reads them.
enum class Pending : uint8_t { None, Add, Sub };

class Interpreter
{
public:
    Interpreter(win32::WinApplication* app, x86::CPU& cpu)
        :   m_app(app)
        ,   m_cpu(cpu)
        ,   m_regs{ &cpu.eax, &cpu.ecx, &cpu.edx, &cpu.ebx, &cpu.esp, &cpu.ebp, &cpu.esi, &cpu.edi }
    {
    }

    void run(const Routine& routine)
    {
        const Instruction* const code = routine.code.data();
        const Instruction* in = code;
        for (;;)
        {
            switch (in->op)
            {
            case Op::Push:
            {
                const x86::reg32 value = *m_regs[in->reg];
                m_cpu.esp -= 4;
                rc::st32(m_cpu.esp, value);
                break;
            }
            case Op::Pop:
            {
                const x86::reg32 value = rc::ld32(m_cpu.esp);
                m_cpu.esp += 4;
                *m_regs[in->reg] = value;
                break;
            }
            case Op::Store:
                write32(*in, effectiveAddress(*in), *m_regs[in->reg]);
                break;
            case Op::Load:
                *m_regs[in->reg] = read32(*in, effectiveAddress(*in));
                break;
            case Op::Add:
                *m_regs[in->reg] = add(*m_regs[in->reg], read32(*in, effectiveAddress(*in)));
                break;
            case Op::Sub:
                *m_regs[in->reg] = subtract(*m_regs[in->reg], read32(*in, effectiveAddress(*in)));
                break;
            case Op::Cmp:
                subtract(*m_regs[in->reg], read32(*in, effectiveAddress(*in)));
                break;
            case Op::AddImm:
            {
                const x86::reg32 address = effectiveAddress(*in);
                write32(*in, address, add(read32(*in, address), in->immediate));
                break;
            }
            case Op::Imul:
            {
                const x86::sreg64 product = x86::sreg64(x86::sreg32(*m_regs[in->reg]))
                                          * x86::sreg32(read32(*in, effectiveAddress(*in)));
                materialize();
                m_cpu.flags.cf = m_cpu.flags.of = product != x86::sreg32(product);
                *m_regs[in->reg] = x86::reg32(product);
                break;
            }
            case Op::StoreByte:
                rc::st8(effectiveAddress(*in), x86::reg8(in->immediate));
                break;
            case Op::Jl:
                if (less())
                {
                    in = jump(code, *in);
                    continue;
                }
                break;
            case Op::Jg:
                if (!less() && !zero())
                {
                    in = jump(code, *in);
                    continue;
                }
                break;
            case Op::Call:
                materialize();
                m_cpu.esp -= 4;
                rc::st32(m_cpu.esp, in->next);
                m_cpu.ip = in->immediate;
                m_app->dynamic_call(in->immediate, m_cpu);
                if (m_cpu.terminate)
                    return;
                break;
            case Op::Ret:
                materialize();
                m_cpu.esp += 4;
                return;
            }
            ++in;
        }
    }

private:
    const Instruction* jump(const Instruction* code, const Instruction& in)
    {
        const Instruction* target = code + in.immediate;
        if (target <= &in)
            m_app->allowPreemption(m_cpu);
        return target;
    }

    x86::reg32 effectiveAddress(const Instruction& in) const
    {
        x86::reg32 address = in.base != NoRegister ? *m_regs[in.base] : 0;
        if (in.index != NoRegister)
            address += *m_regs[in.index] << in.scale;
        return address + in.displacement;
    }

    x86::reg32 read32(const Instruction& in, x86::reg32 address) const
    {
        return in.memory ? rc::ld32(address) : *m_regs[in.rm];
    }

    void write32(const Instruction& in, x86::reg32 address, x86::reg32 value)
    {
        if (in.memory)
            rc::st32(address, value);
        else
            *m_regs[in.rm] = value;
    }

    x86::reg32 add(x86::reg32 a, x86::reg32 b)
    {
        m_pending = Pending::Add;
        m_a = a;
        m_b = b;
        m_r = a + b;
        return m_r;
    }

    x86::reg32 subtract(x86::reg32 a, x86::reg32 b)
    {
        m_pending = Pending::Sub;
        m_a = a;
        m_b = b;
        m_r = a - b;
        return m_r;
    }

    bool overflow() const
    {
        if (m_pending == Pending::Add)
            return ((m_a ^ m_r) & (m_b ^ m_r)) >> 31;
        if (m_pending == Pending::Sub)
            return ((m_a ^ m_b) & (m_a ^ m_r)) >> 31;
        return m_cpu.flags.of;
    }

    // SF != OF.
    bool less() const
    {
        if (m_pending == Pending::Sub)
            return x86::sreg32(m_a) < x86::sreg32(m_b);
        const bool sign = m_pending == Pending::None ? bool(m_cpu.flags.sf) : bool(m_r >> 31);
        return sign != overflow();
    }

    bool zero() const
    {
        return m_pending == Pending::None ? bool(m_cpu.flags.zf) : m_r == 0;
    }

    // Writes the pending CF, PF, AF, ZF, SF and OF to cpu.flags.
    void materialize()
    {
        if (m_pending == Pending::None)
            return;
        m_cpu.flags.cf = m_pending == Pending::Add ? m_r < m_a : m_a < m_b;
        m_cpu.flags.pf = !x86::parity8(x86::reg8(m_r));
        m_cpu.flags.af = ((m_a ^ m_b ^ m_r) >> 4) & 1;
        m_cpu.flags.zf = m_r == 0;
        m_cpu.flags.sf = m_r >> 31;
        m_cpu.flags.of = overflow();
        m_pending = Pending::None;
    }

    win32::WinApplication* m_app;
    x86::CPU&              m_cpu;
    x86::reg32*            m_regs[8];
    Pending                m_pending = Pending::None;
    x86::reg32             m_a = 0;
    x86::reg32             m_b = 0;
    x86::reg32             m_r = 0;
};

// Decoded routines by start address, dropped when the game makes their bytes executable again or, past
// MaxRoutines, when their bank is no longer loaded.
const size_t MaxRoutines = 128;
std::mutex s_lock;
std::unordered_map<x86::reg32, std::shared_ptr<Routine>> s_routines;

// The methods resolve returned for routines in s_routines: the address in the low word, the method in the high
// word. Read without s_lock; stored and cleared with it held, whenever s_routines drops or replaces a routine.
const x86::reg32 ResolvedBits = 6;
std::atomic<uint64_t> s_resolved[1u << ResolvedBits];

std::atomic<uint64_t>& resolvedEntry(x86::reg32 address)
{
    return s_resolved[(address >> 2 ^ address >> 8) & ((1u << ResolvedBits) - 1)];
}

void clearResolved()
{
    for (std::atomic<uint64_t>& entry : s_resolved)
        entry.store(0, std::memory_order_relaxed);
}

std::string bankName(x86::reg32 bank)
{
    std::string name;
    x86::reg32 text = rc::ld32(bank + BankName);
    while (text && name.size() < 64 && win32::MemMap::isGuestRange(text, 1) && rc::ld8(text))
        name += char(rc::ld8(text++));
    return name;
}

bool s_native = true;

// The recompiled routine with this hash, or nullptr.
win32::MethodPtr nativeRoutine(uint64_t hash)
{
    const NativeRoutine* const end = g_nativeRoutines + g_nativeRoutineCount;
    const NativeRoutine* found = std::lower_bound(g_nativeRoutines, end, hash,
                                                  [](const NativeRoutine& r, uint64_t h) { return r.hash < h; });
    return found != end && found->hash == hash ? found->method : nullptr;
}

// The decoded routine starting at address, if it lies in a loaded bank. Logs each routine's first use per load.
std::shared_ptr<Routine> routineAt(x86::reg32 address)
{
    {
        std::lock_guard<std::mutex> lock(s_lock);
        auto found = s_routines.find(address);
        if (found != s_routines.end())
            return found->second;
    }
    Location location;
    if (!findRoutine(address, location))
        return nullptr;
    std::shared_ptr<Routine> routine = Decoder(location).run();
    if (s_native)
        routine->native = nativeRoutine(routine->hash);
    unsigned calls = 0;
    for (const Instruction& in : routine->code)
        calls += in.op == Op::Call;
    pf::logInfo("bank code: bank 0x%08x \"%s\" routine %u at +0x%x: %u bytes, %u instructions, %u calls, hash "
                "%016llx %s", location.bank, bankName(location.bank).c_str(), location.index,
                location.begin - location.bank, location.end - location.begin, unsigned(routine->code.size()), calls,
                static_cast<unsigned long long>(routine->hash), routine->native ? "native" : "interpreted");
    std::lock_guard<std::mutex> lock(s_lock);
    if (s_routines.size() >= MaxRoutines)
    {
        clearResolved();
        for (auto it = s_routines.begin(); it != s_routines.end();)
        {
            Location loaded;
            const Location& cached = it->second->location;
            const bool stale = !findRoutine(cached.begin, loaded) || loaded.end != cached.end;
            it = stale ? s_routines.erase(it) : std::next(it);
        }
    }
    resolvedEntry(address).store(0, std::memory_order_relaxed);
    s_routines[address] = routine;
    return routine;
}

// Entered like a translated function: cpu.ip is the routine, cpu.esp points at the return address slot.
void runRoutine(win32::WinApplication* app, x86::CPU& cpu)
{
    const std::shared_ptr<Routine> routine = routineAt(cpu.ip);
    if (!routine)
        unsupported(0, cpu.ip, "no loaded bank routine");
    Interpreter(app, cpu).run(*routine);
}

win32::MethodPtr resolve(x86::reg32 address)
{
    std::atomic<uint64_t>& resolved = resolvedEntry(address);
    const uint64_t entry = resolved.load(std::memory_order_relaxed);
    if (address && x86::reg32(entry) == address)
        return reinterpret_cast<win32::MethodPtr>(uintptr_t(entry >> 32));
    {
        std::lock_guard<std::mutex> lock(s_lock);
        auto found = s_routines.find(address);
        if (found != s_routines.end())
        {
            const win32::MethodPtr method = found->second->native ? found->second->native : &runRoutine;
            if (address)
                resolved.store(uint64_t(reinterpret_cast<uintptr_t>(method)) << 32 | address,
                               std::memory_order_relaxed);
            return method;
        }
    }
    const std::shared_ptr<Routine> routine = routineAt(address);
    if (!routine)
        return nullptr;
    return routine->native ? routine->native : &runRoutine;
}

void changed(x86::reg32 address, x86::reg32 size)
{
    std::lock_guard<std::mutex> lock(s_lock);
    clearResolved();
    for (auto it = s_routines.begin(); it != s_routines.end();)
    {
        const Location& location = it->second->location;
        const bool overlaps = location.begin < address + size && address < location.end;
        it = overlaps ? s_routines.erase(it) : std::next(it);
    }
}

}

void install()
{
    s_native = pf::Config::getBool("bank_native", true);
    win32::setForeignCodeResolver(&resolve);
    win32::setForeignCodeObserver(&changed);
}

}}
