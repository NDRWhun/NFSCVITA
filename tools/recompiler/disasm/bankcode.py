"""Recompiles the x86 routines of the ABKC sound banks (SOUND/**/*.abk) into <module>_bankcode.cpp.

The bank loader 0x898a13 rewrites each call's rel32 word, which holds an index into the library function table
0xa7d6c0, to reach that function, and runs the routines in place; src/nfsc/bankcode.cpp interprets them. Every
routine that decodes as the interpreter decodes it becomes a C++ function with the interpreter's semantics (memory,
registers, lazy flags, preemption points, return addresses at the routine's load address). A routine is keyed by an
FNV-1a 64 hash of its reachable instructions in address order (offset, then bytes with call operands replaced by the
absolute target), which bankcode.cpp computes the same way after decoding a loaded routine; identical routines share
one function. Absolute fixups never fall inside reachable code; a routine where one does is left to the interpreter.

Bank header: entry count u16 +0x0A, first entry +0x1C, call fixups at [+0x30] (count, bank offsets of rel32 words),
absolute fixups at [+0x34]. Entry: routine +0x28 .. end +0x2C (bank offsets), next entry after
0x3C + 4 * (byte +0x24 + byte +0x27).
"""

import os
import struct

from .codegen import ctx as C

FUNCTION_TABLE = 0xa7d6c0
FUNCTION_COUNT = 40
# The routines' only call site (call [eax+8] in the module tick 0x89e2e3).
ROUTINE_CALL = (0x89e2e3, 0x89e2f2)
MAGIC = b'ABKC'
MAX_ROUTINE_BYTES = 0x10000
REGS = ('eax', 'ecx', 'edx', 'ebx', 'esp', 'ebp', 'esi', 'edi')
FNV_BASIS = 0xcbf29ce484222325
FNV_PRIME = 0x100000001b3
M32 = 0xffffffff
M64 = 0xffffffffffffffff

PRODUCERS = {'add': 'add', 'sub': 'sub', 'cmp': 'sub', 'addimm': 'add'}


class Unsupported(Exception):
    pass


class Ins:
    __slots__ = ('op', 'reg', 'rm', 'memory', 'base', 'index', 'scale', 'disp', 'imm', 'address', 'next', 'target')

    def __init__(self, address):
        self.op = None
        self.reg = self.rm = 0
        self.memory = False
        self.base = self.index = None
        self.scale = 0
        self.disp = self.imm = 0
        self.address = address
        self.next = address
        self.target = None


def s8(v):
    return v - 0x100 if v & 0x80 else v


def decode(d, a):
    """The instruction at bank offset a, in the forms bankcode.cpp's decoder accepts."""
    ins = Ins(a)
    p = [a]

    def fetch8():
        if p[0] >= len(d):
            raise Unsupported('past the bank end')
        v = d[p[0]]
        p[0] += 1
        return v

    def fetch32():
        if p[0] + 4 > len(d):
            raise Unsupported('past the bank end')
        v = struct.unpack_from('<I', d, p[0])[0]
        p[0] += 4
        return v

    def modrm():
        b = fetch8()
        mod, rm = b >> 6, b & 7
        ins.reg = (b >> 3) & 7
        if mod == 3:
            ins.rm = rm
            return
        ins.memory = True
        if rm == 4:
            sib = fetch8()
            base, index = sib & 7, (sib >> 3) & 7
            if base == 5 and mod == 0:
                ins.disp = fetch32()
            else:
                ins.base = base
            if index != 4:
                ins.index = index
                ins.scale = sib >> 6
        elif rm == 5 and mod == 0:
            ins.disp = fetch32()
        else:
            ins.base = rm
        if mod == 1:
            ins.disp = (ins.disp + s8(fetch8())) & M32
        elif mod == 2:
            ins.disp = (ins.disp + fetch32()) & M32

    op = fetch8()
    if 0x50 <= op <= 0x57:
        ins.op, ins.reg = 'push', op & 7
    elif 0x58 <= op <= 0x5f:
        ins.op, ins.reg = 'pop', op & 7
    elif op in (0x89, 0x8b, 0x03, 0x2b, 0x3b):
        ins.op = {0x89: 'store', 0x8b: 'load', 0x03: 'add', 0x2b: 'sub', 0x3b: 'cmp'}[op]
        modrm()
    elif op in (0x81, 0x83):
        ins.op = 'addimm'
        modrm()
        if ins.reg != 0:
            raise Unsupported('group 1 operation')
        ins.imm = fetch32() if op == 0x81 else s8(fetch8()) & M32
    elif op == 0x0f:
        if fetch8() != 0xaf:
            raise Unsupported('two-byte opcode')
        ins.op = 'imul'
        modrm()
    elif op == 0xc6:
        ins.op = 'storebyte'
        modrm()
        if ins.reg != 0 or not ins.memory:
            raise Unsupported('C6 form')
        ins.imm = fetch8()
    elif op in (0x7c, 0x7f):
        ins.op = 'jl' if op == 0x7c else 'jg'
        rel = s8(fetch8())
        ins.imm = p[0] + rel
    elif op == 0xe8:
        ins.op = 'call'
        fetch32()
    elif op == 0xc3:
        ins.op = 'ret'
    else:
        raise Unsupported('instruction %02x' % op)
    ins.next = p[0]
    return ins


class Routine:
    def __init__(self, bank, index, begin, end, code, key):
        self.bank = bank
        self.index = index
        self.begin = begin
        self.end = end
        self.code = code        # Ins in address order
        self.key = key          # FNV-1a 64 of the normalized reachable code


def decode_routine(d, begin, end, calls, absolute, table):
    """Recursive descent from begin with the interpreter's checks; call targets from the function table."""
    if end <= begin or end - begin > MAX_ROUTINE_BYTES or end > len(d):
        raise Unsupported('bad routine range')
    at = {}
    inside = set()
    pending = [begin]
    while pending:
        a = pending.pop()
        while True:
            if not begin <= a < end:
                raise Unsupported('jump out of the routine')
            if a in at:
                break
            if a in inside:
                raise Unsupported('jump into an instruction')
            ins = decode(d, a)
            if ins.next > end:
                raise Unsupported('instruction past the routine end')
            for x in range(a, ins.next):
                if x in at or x in inside:
                    raise Unsupported('overlapping instructions')
                if x in absolute:
                    raise Unsupported('absolute fixup in code')
            inside.update(range(a + 1, ins.next))
            at[a] = ins
            if ins.op == 'call':
                if a + 1 not in calls:
                    raise Unsupported('call without fixup')
                index = struct.unpack_from('<I', d, a + 1)[0]
                if index >= len(table):
                    raise Unsupported('call fixup index %d' % index)
                ins.target = table[index]
            if ins.op == 'ret':
                break
            if ins.op in ('jl', 'jg'):
                pending.append(ins.imm)
            a = ins.next
    code = [at[a] for a in sorted(at)]
    h = FNV_BASIS
    for ins in code:
        raw = bytearray(d[ins.address:ins.next])
        if ins.op == 'call':
            struct.pack_into('<I', raw, 1, ins.target)
        for b in struct.pack('<I', ins.address - begin) + bytes(raw):
            h = ((h ^ b) * FNV_PRIME) & M64
    return code, h


def entries(d):
    count = struct.unpack_from('<H', d, 0x0a)[0]
    e = struct.unpack_from('<I', d, 0x1c)[0]
    for k in range(count):
        begin, end = struct.unpack_from('<II', d, e + 0x28)
        yield k, begin, end
        e += 0x3c + 4 * (d[e + 0x24] + d[e + 0x27])


def fixups(d, offset):
    table = struct.unpack_from('<I', d, offset)[0]
    count = struct.unpack_from('<I', d, table)[0]
    return set(struct.unpack_from('<%dI' % count, d, table + 4))


def scan(sound_dir, table):
    """Every routine of every bank under sound_dir: (routines, [(bank, index, reason)] left to the interpreter)."""
    routines, skipped = [], []
    for directory, subdirs, files in os.walk(sound_dir):
        subdirs.sort()
        for name in sorted(files):
            if not name.lower().endswith('.abk'):
                continue
            path = os.path.join(directory, name)
            with open(path, 'rb') as f:
                d = f.read()
            if d[:4] != MAGIC:
                continue
            bank = os.path.relpath(path, sound_dir).replace(os.sep, '/')
            try:
                calls, absolute = fixups(d, 0x30), fixups(d, 0x34)
                listed = list(entries(d))
            except (struct.error, IndexError):
                skipped.append((bank, -1, 'bad bank header'))
                continue
            for k, begin, end in listed:
                try:
                    code, key = decode_routine(d, begin, end, calls, absolute, table)
                except (Unsupported, struct.error, IndexError) as e:
                    skipped.append((bank, k, str(e)))
                    continue
                routines.append(Routine(bank, k, begin, end, code, key))
    return routines, skipped


def flow(code):
    """Pending flag producer kind before each instruction: 'n' (cpu.flags holds them), 'add', 'sub', '?' (mixed)."""
    position = dict((ins.address, i) for i, ins in enumerate(code))
    state = [None] * len(code)
    state[0] = 'n'
    work = [0]
    while work:
        i = work.pop()
        ins = code[i]
        kind = state[i]
        if ins.op in PRODUCERS:
            kind = PRODUCERS[ins.op]
        elif ins.op in ('imul', 'call', 'ret'):
            kind = 'n'
        successors = []
        if ins.op != 'ret':
            successors.append(i + 1)
        if ins.op in ('jl', 'jg'):
            successors.append(position[ins.imm])
        for s in successors:
            merged = kind if state[s] is None or state[s] == kind else '?'
            if merged != state[s]:
                state[s] = merged
                work.append(s)
    return state


class Emitter:
    def __init__(self, routine, flag_readers=None, exit_flags=True):
        """flag_readers: library functions that read the flags on entry (None: all); exit_flags: the caller reads
        the flags after the routine returns."""
        self.routine = routine
        self.code = routine.code
        self.state = flow(self.code)
        # A jump that reads cpu.flags (no compare before it) needs every producer written there.
        keep = flag_readers is None or any(self.state[i] in ('n', '?') for i, ins in enumerate(self.code)
                                           if ins.op in ('jl', 'jg'))
        self.flag_readers = None if keep else set(flag_readers)
        self.exit_flags = keep or exit_flags
        self.imul_flags = self.exit_flags or bool(self.flag_readers)
        used = set([4])
        for ins in self.code:
            if ins.op in ('push', 'pop', 'store', 'load', 'add', 'sub', 'cmp', 'imul'):
                used.add(ins.reg)
            if ins.memory:
                used.update(r for r in (ins.base, ins.index) if r is not None)
            elif ins.op in ('store', 'load', 'add', 'sub', 'cmp', 'addimm', 'imul'):
                used.add(ins.rm)
        self.used = [REGS[r] for r in range(8) if r in used]
        # The lazy kind is tracked at run time only when some reader cannot know it statically.
        self.dynamic = any(self.state[i] == '?' and ins.op in ('jl', 'jg', 'imul', 'call', 'ret')
                           for i, ins in enumerate(self.code))

    def ea(self, ins):
        parts = []
        if ins.base is not None:
            parts.append(REGS[ins.base])
        if ins.index is not None:
            parts.append('(%s << %d)' % (REGS[ins.index], ins.scale) if ins.scale else REGS[ins.index])
        if ins.disp or not parts:
            parts.append('0x%xu' % ins.disp)
        return 'x86::reg32(%s)' % ' + '.join(parts)

    def source(self, ins):
        return 'rc::ld32(%s)' % self.ea(ins) if ins.memory else REGS[ins.rm]

    def produce(self, kind, a, b):
        lines = ['fa = %s;' % a, 'fb = %s;' % b]
        if self.dynamic:
            lines.append('fk = %d;' % (1 if kind == 'add' else 2))
        return lines

    def materialize(self, i):
        kind = self.state[i]
        if kind == 'add':
            return ['bankFlagsAdd(cpu, fa, fb);']
        if kind == 'sub':
            return ['bankFlagsSub(cpu, fa, fb);']
        if kind == '?':
            return ['bankFlags(cpu, fk, fa, fb);']
        return []

    def condition(self, i, op):
        kind = self.state[i]
        if kind == 'sub':
            less, zero = 'x86::sreg32(fa) < x86::sreg32(fb)', 'fa == fb'
        elif kind == 'add':
            less, zero = 'bankLessAdd(fa, fb)', 'fa + fb == 0'
        elif kind == 'n':
            less, zero = 'cpu.flags.sf != cpu.flags.of', 'cpu.flags.zf'
        else:
            less, zero = 'bankLess(cpu, fk, fa, fb)', 'bankZero(cpu, fk, fa, fb)'
        if op == 'jl':
            return less
        return '!(%s) && !(%s)' % (less, zero)

    def sync_out(self):
        return ['cpu.%s = %s;' % (r, r) for r in self.used]

    def sync_in(self):
        return ['%s = cpu.%s;' % (r, r) for r in self.used]

    def instruction(self, i, ins):
        r = REGS[ins.reg]
        op = ins.op
        if op == 'push':
            if r == 'esp':
                return ['rc::st32(esp - 4, esp);', 'esp -= 4;']
            return ['esp -= 4;', 'rc::st32(esp, %s);' % r]
        if op == 'pop':
            if r == 'esp':
                return ['esp = rc::ld32(esp);']
            return ['%s = rc::ld32(esp);' % r, 'esp += 4;']
        if op == 'store':
            if ins.memory:
                return ['rc::st32(%s, %s);' % (self.ea(ins), r)]
            return ['%s = %s;' % (REGS[ins.rm], r)]
        if op == 'load':
            return ['%s = %s;' % (r, self.source(ins))]
        if op in ('add', 'sub', 'cmp'):
            lines = self.produce(PRODUCERS[op], r, self.source(ins))
            if op != 'cmp':
                lines.append('%s = fa %s fb;' % (r, '+' if op == 'add' else '-'))
            return lines
        if op == 'addimm':
            if ins.memory:
                return (['{', '    const x86::reg32 address = %s;' % self.ea(ins)]
                        + ['    ' + x for x in self.produce('add', 'rc::ld32(address)', '0x%xu' % ins.imm)]
                        + ['    rc::st32(address, fa + fb);', '}'])
            return self.produce('add', REGS[ins.rm], '0x%xu' % ins.imm) + ['%s = fa + fb;' % REGS[ins.rm]]
        if op == 'imul':
            if not self.imul_flags:
                return ['%s = %s * %s;' % (r, r, self.source(ins))] + (['fk = 0;'] if self.dynamic else [])
            lines = ['{', '    const x86::sreg64 product = x86::sreg64(x86::sreg32(%s)) * x86::sreg32(%s);'
                     % (r, self.source(ins))]
            lines += ['    ' + x for x in self.materialize(i)]
            lines += ['    bankFlagsImul(cpu, product != x86::sreg32(product));',
                      '    %s = x86::reg32(product);' % r]
            if self.dynamic:
                lines.append('    fk = 0;')
            return lines + ['}']
        if op == 'storebyte':
            return ['rc::st8(%s, x86::reg8(0x%xu));' % (self.ea(ins), ins.imm)]
        if op in ('jl', 'jg'):
            target = ins.imm - self.routine.begin
            body = []
            if ins.imm <= ins.address:
                body += self.sync_out() + ['app->allowPreemption(cpu);']
            body.append('goto L_%x;' % target)
            return ['if (%s)' % self.condition(i, op), '{'] + ['    ' + x for x in body] + ['}']
        if op == 'call':
            lines = self.materialize(i) if self.flag_readers is None or ins.target in self.flag_readers else []
            lines += ['esp -= 4;', 'rc::st32(esp, base + 0x%xu);' % (ins.next - self.routine.begin)]
            lines += self.sync_out()
            lines += ['cpu.ip = 0x%xu;' % ins.target, 'app->allowPreemption(cpu);', 'sub_%x(app, cpu);' % ins.target,
                      'if (cpu.terminate)', '    return;']
            if self.dynamic:
                lines.append('fk = 0;')
            return lines + self.sync_in()
        if op == 'ret':
            return (self.materialize(i) if self.exit_flags else []) + ['esp += 4;'] + self.sync_out() + ['return;']
        raise AssertionError(op)

    def function(self, name):
        labels = set(ins.imm for ins in self.code if ins.op in ('jl', 'jg'))
        out = ['void %s(win32::WinApplication* app, x86::CPU& cpu)' % name, '{',
               '    [[maybe_unused]] const x86::reg32 base = cpu.ip;']
        out += ['    x86::reg32 %s = cpu.%s;' % (r, r) for r in self.used]
        out.append('    [[maybe_unused]] x86::reg32 fa = 0;')
        out.append('    [[maybe_unused]] x86::reg32 fb = 0;')
        if self.dynamic:
            out.append('    [[maybe_unused]] int fk = 0;')
        for i, ins in enumerate(self.code):
            if ins.address in labels:
                out.append('L_%x:' % (ins.address - self.routine.begin))
            out += ['    ' + x for x in self.instruction(i, ins)]
        out.append('}')
        return out


PRELUDE = '''namespace
{

// CF, PF, AF, ZF, SF and OF of r = a + b or r = a - b, written to cpu.flags.
inline void bankFlagsSet(x86::CPU& cpu, x86::reg32 a, x86::reg32 b, x86::reg32 r, bool cf, bool of)
{
    const unsigned bits = unsigned(cf) | unsigned(!x86::parity8(x86::reg8(r))) << 2 | ((a ^ b ^ r) >> 4 & 1) << 4
                        | unsigned(r == 0) << 6 | (r >> 31) << 7 | unsigned(of) << 11;
    cpu.flags.word = x86::reg16((cpu.flags.word & ~0x8d5u) | bits);
}

inline void bankFlagsAdd(x86::CPU& cpu, x86::reg32 a, x86::reg32 b)
{
    const x86::reg32 r = a + b;
    bankFlagsSet(cpu, a, b, r, r < a, ((a ^ r) & (b ^ r)) >> 31);
}

inline void bankFlagsSub(x86::CPU& cpu, x86::reg32 a, x86::reg32 b)
{
    const x86::reg32 r = a - b;
    bankFlagsSet(cpu, a, b, r, a < b, ((a ^ b) & (a ^ r)) >> 31);
}

// imul: CF = OF = the product overflowed 32 bits.
inline void bankFlagsImul(x86::CPU& cpu, bool overflow)
{
    cpu.flags.word = x86::reg16((cpu.flags.word & ~0x801u) | unsigned(overflow) | unsigned(overflow) << 11);
}

// SF != OF of a + b.
inline bool bankLessAdd(x86::reg32 a, x86::reg32 b)
{
    const x86::reg32 r = a + b;
    return (r >> 31) != (((a ^ r) & (b ^ r)) >> 31);
}

// Pending producer kind 0 (flags in cpu.flags), 1 (add) or 2 (sub).
inline void bankFlags(x86::CPU& cpu, int kind, x86::reg32 a, x86::reg32 b)
{
    if (kind == 1)
        bankFlagsAdd(cpu, a, b);
    else if (kind == 2)
        bankFlagsSub(cpu, a, b);
}

inline bool bankLess(const x86::CPU& cpu, int kind, x86::reg32 a, x86::reg32 b)
{
    if (kind == 2)
        return x86::sreg32(a) < x86::sreg32(b);
    return kind == 1 ? bankLessAdd(a, b) : cpu.flags.sf != cpu.flags.of;
}

inline bool bankZero(const x86::CPU& cpu, int kind, x86::reg32 a, x86::reg32 b)
{
    if (kind == 2)
        return a == b;
    return kind == 1 ? a + b == 0 : bool(cpu.flags.zf);
}

}
'''


def function_table(image):
    return [image.dword(FUNCTION_TABLE + 4 * i) for i in range(FUNCTION_COUNT)]


PART_BYTES = 1 << 20

HEADER = ['// Generated by tools/recompiler: the x86 routines of the ABKC sound banks, keyed by the hash of their',
          '// reachable code (src/nfsc/bankcode.cpp).',
          '#include <x86.h>', '#include <lib/winapp.h>', '#include <recomp.h>', '#include <bankcode.h>', '',
          'namespace nfsc', '{', '']


def write(directory, module, sound_dir, image, functions):
    """Writes <module>_bankcode*.cpp into directory from the banks under sound_dir (none when it is missing);
    functions: the generated guest function entries. Returns a summary."""
    table = function_table(image)
    # Flags are computed only where something reads them: library functions with flags live on entry, the caller
    # after the routine returns (liveness of the generated code).
    flag_readers = set(t for t in table if t in functions and
                       functions[t].program.live_in.get(t, C.CPU_FLAG_MASK) & C.CPU_FLAG_MASK)
    flag_readers |= set(t for t in table if t not in functions)
    caller = functions.get(ROUTINE_CALL[0])
    exit_flags = (caller is None or ROUTINE_CALL[1] not in caller.index_of or
                  bool(caller.live_out[caller.index_of[ROUTINE_CALL[1]]] & C.CPU_FLAG_MASK))
    routines, skipped = scan(sound_dir, table) if os.path.isdir(sound_dir) else ([], [])
    total = len(routines) + len(skipped)
    unique = {}
    for routine in routines:
        missing = [ins.target for ins in routine.code if ins.op == 'call' and ins.target not in functions]
        if missing:
            skipped.append((routine.bank, routine.index, 'call to 0x%x, no generated function' % missing[0]))
            continue
        unique.setdefault(routine.key, routine)
    # Routines go to <module>_bankcode.<n>.cpp files of about PART_BYTES each, the table to <module>_bankcode.cpp.
    parts = [[]]
    size = 0
    for key in sorted(unique):
        routine = unique[key]
        text = ['// %s routine %d (+0x%x, %d bytes)' % (routine.bank, routine.index, routine.begin,
                                                      routine.end - routine.begin)]
        text += Emitter(routine, flag_readers, exit_flags).function('bank_%016x' % key) + ['']
        if size >= PART_BYTES:
            parts.append([])
            size = 0
        parts[-1].append((routine, text))
        size += sum(len(x) + 1 for x in text)
    names = []
    for n, part in enumerate(p for p in parts if p):
        targets = sorted(set(ins.target for r, _ in part for ins in r.code if ins.op == 'call'))
        lines = HEADER + ['void sub_%x(win32::WinApplication* app, x86::CPU& cpu);' % t for t in targets]
        lines += ['', 'namespace bankcode', '{', '', PRELUDE]
        for _, text in part:
            lines += text
        lines += ['}', '', '}', '']
        name = '%s_bankcode.%d.cpp' % (module, n)
        names.append(name)
        with open(os.path.join(directory, name), 'w', newline='\n') as f:
            f.write('\n'.join(lines))
    lines = HEADER + ['namespace bankcode', '{', '']
    lines += ['void bank_%016x(win32::WinApplication* app, x86::CPU& cpu);' % key for key in sorted(unique)]
    lines += ['', 'extern const NativeRoutine g_nativeRoutines[] = {']
    lines += ['    { 0x%016xull, &bank_%016x },' % (key, key) for key in sorted(unique)]
    if not unique:
        lines.append('    { 0, nullptr },')
    lines += ['};', 'extern const unsigned g_nativeRoutineCount = %d;' % len(unique), '', '}', '', '}', '']
    with open(os.path.join(directory, '%s_bankcode.cpp' % module), 'w', newline='\n') as f:
        f.write('\n'.join(lines))
    summary = 'bank code: %d routines in the banks, %d distinct recompiled, %d left to the interpreter' % (
        total, len(unique), len(skipped))
    for bank, index, reason in skipped[:20]:
        summary += '\n  interpreted: %s routine %d: %s' % (bank, index, reason)
    return summary
