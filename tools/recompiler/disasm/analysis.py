"""Control flow and dataflow analyses over the recompiled functions.

Flags live in C++ locals (f_cf...) inside a function; cpu.flags is the copy other code sees. A flag is stored to
cpu.flags at a boundary (call, return, tail call, pushfd) only when code outside can read it there (interprocedural
liveness) and this function changed it since cpu.flags was last valid (a local producer reaches the boundary).

Every flag read is served either from the local or, when exactly one lazy producer reaches it, from that producer's
saved operands (cmp a, b; jl -> a < b). Producers write their local only for reads that cannot be served lazily.

State masks: bits 0-6 are FLAGS (ctx.FLAG_BIT), bits 8-15 general registers (ctx.REG_BIT << 8).
"""

import re

from capstone import x86 as X

from .codegen import ctx as C
from . import codegen, fpustack, stack


FCOM = ('fcom', 'fcomp', 'fcompp', 'fucom', 'fucomp', 'fucompp', 'ftst')
JCC = set('j' + cc for cc in C.CC_ALIASES) | {'jcxz', 'jecxz', 'loop', 'loope', 'loopne'}
REG_SHIFT = 8
ALL_REG_STATE = C.ALL_REGS << REG_SHIFT
ALL_STATE = C.ALL_FLAGS | ALL_REG_STATE
# Registers a callee may write without saving them (Program.must_def).
CALL_CLOBBERED = C.REG_BIT['eax'] | C.REG_BIT['ecx'] | C.REG_BIT['edx']
MEMORY_DEFS = ('popfd', 'popf')     # write cpu.flags directly
# pf and af are produced only by sahf, popf and x87 compares; these capstone flags mark instructions that set them on
# x86 (integer arithmetic) without the generator modelling it.
X86_SETS = {'pf': X.X86_EFLAGS_MODIFY_PF | X.X86_EFLAGS_RESET_PF | X.X86_EFLAGS_SET_PF,
            'af': X.X86_EFLAGS_MODIFY_AF | X.X86_EFLAGS_RESET_AF | X.X86_EFLAGS_SET_AF}


class Exit:
    """An edge leaving the function (or a call out of it) at one instruction."""
    __slots__ = ('edge', 'kind', 'target')

    def __init__(self, edge, kind, target=None):
        self.edge = edge        # 'exit' (ret, tail, fall-off), 'taken' (jcc to a tail target), 'call'
        self.kind = kind        # 'ret', 'tail', 'dyntail', 'call', 'dyncall'
        self.target = target    # entry address for 'tail' and 'call'


class Function:
    ENTRY = -1

    def __init__(self, module, subroutine, thread_segments, skip_instructions, function_names):
        self.module = module
        self.sub = subroutine
        self.instructions = subroutine.instructions
        self.entry_address = subroutine.get_entry_point()
        self.thread_segments = thread_segments
        self.skip = set(skip_instructions)
        self.function_names = function_names
        self.index_of = {}
        for i, ins in enumerate(self.instructions):
            self.index_of.setdefault(ins.address, i)
        self.static_targets = set(x for _, x in subroutine.static_labels)
        self.dynamic_targets = set(x for _, x in subroutine.dynamic_labels)
        self.entry = self.index_of[self.entry_address]
        self.n = len(self.instructions)
        self.program = None

        self.branch = [None] * self.n
        self.succ = [[] for _ in range(self.n)]
        self.pred = None
        self.exits = [[] for _ in range(self.n)]
        self.effects = [None] * self.n
        self.use = [0] * self.n     # state read (flags | regs << 8)
        self.kill = [0] * self.n    # state always written
        self.unmodeled = [0] * self.n   # pf/af set on x86 but not by the generated code
        self.fused = {}             # index of fcom / fnstsw / sahf -> index of the sahf of its triple
        self.live_in = None
        self.live_out = None
        self._rd_cache = {}
        self.local_needed = set()   # (producer index, flag): the producer writes f_<flag>
        self.lazy_readers = set()   # producers with lazily served reads
        self.reloads = set()        # (memory producer, flag): reload f_<flag> from cpu.flags after it
        self.stores = {}            # (index, edge) -> flag mask stored to cpu.flags there
        self.reg_stores = {}        # (index, edge) -> registers stored to cpu there
        self.reg_reloads = {}       # call index -> registers reloaded from cpu after it

    @property
    def local_registers(self):
        """Registers live in C++ locals (else every access goes to cpu)."""
        p = self.program
        return p.local_registers and self.entry_address not in p.memory_functions

    # ---- structure ----------------------------------------------------------------------------------------
    def is_label(self, i):
        a = self.instructions[i].address
        return a in self.static_targets or a in self.dynamic_targets or a == self.entry_address

    def in_function(self, address):
        return address in self.index_of

    def classify(self):
        """Control flow of every instruction: successors and exit edges."""
        for i, ins in enumerate(self.instructions):
            nxt = [i + 1] if i + 1 < self.n else []
            fall_off = [] if nxt else [Exit('exit', 'ret')]
            mnemonic = ins.mnemonic.split()[-1]
            if ins.address in self.skip:
                self.branch[i] = ('skip',)
                self.succ[i] = nxt
                self.exits[i] = fall_off
            elif mnemonic == 'jmp' or mnemonic in JCC:
                target = self.jump_target(ins)
                conditional = mnemonic != 'jmp'
                self.branch[i] = target
                succ = list(nxt) if conditional else []
                exits = list(fall_off) if conditional else []
                edge = 'taken' if conditional else 'exit'
                if target[0] == 'label':
                    succ.append(self.index_of[target[1]])
                elif target[0] == 'tail':
                    exits.append(Exit(edge, 'tail', target[1]))
                elif target[0] == 'dyntail':
                    exits.append(Exit(edge, 'dyntail'))
                elif target[0] == 'switch':
                    succ.extend(sorted(set(self.index_of[d] for d in ins.potential_destinations)))
                self.succ[i] = succ
                self.exits[i] = exits
            elif mnemonic == 'ret':
                self.branch[i] = ('ret',)
                self.exits[i] = [Exit('exit', 'ret')]
            elif mnemonic == 'call':
                op = ins.operands[0]
                if op.type == X.X86_OP_IMM:
                    self.branch[i] = ('call', op.imm)
                    self.exits[i] = [Exit('call', 'call', op.imm)] + fall_off
                else:
                    self.branch[i] = ('dyncall',)
                    self.exits[i] = [Exit('call', 'dyncall')] + fall_off
                self.succ[i] = nxt
            else:
                self.branch[i] = ('next',)
                self.succ[i] = nxt
                self.exits[i] = fall_off
        self.pred = [[] for _ in range(self.n)]
        for i, s in enumerate(self.succ):
            for j in s:
                self.pred[j].append(i)
        self.find_fused_compares()

    def jump_target(self, ins):
        """('label', a) inside the function, ('tail', a) to another function's entry, ('switch',) through a jump
        table whose cases are all in the function, ('dyntail',) for any other indirect jump."""
        op = ins.operands[0]
        if op.type == X.X86_OP_IMM:
            target = op.imm & 0xffffffff
            return ('label', target) if target in self.static_targets else ('tail', target)
        destinations = getattr(ins, 'potential_destinations', None)
        if op.type == X.X86_OP_REG or not destinations:
            return ('dyntail',)
        if all(self.in_function(d) for d in destinations):
            return ('switch',)
        return ('dyntail',)

    def find_fused_compares(self):
        """fcom*; fnstsw ax; sahf (or test ah, mask of C0, C2, C3) with no label inside: the flags come from the
        compared values."""
        for i in range(self.n - 2):
            a, b, c = self.instructions[i:i + 3]
            if a.mnemonic not in FCOM or b.mnemonic not in ('fnstsw', 'fstsw'):
                continue
            if c.mnemonic != 'sahf' and self.ftest_mask(c) is None:
                continue
            if b.operands[0].type != X.X86_OP_REG or b.reg_name(b.operands[0].reg) != 'ax':
                continue
            if self.is_label(i + 1) or self.is_label(i + 2):
                continue
            if any(x.address in self.thread_segments or x.address in self.skip for x in (a, b, c)):
                continue
            self.fused[i] = self.fused[i + 1] = self.fused[i + 2] = i + 2

    @staticmethod
    def ftest_mask(ins):
        """The mask of test ah, imm when it tests only x87 condition codes C0, C2 and C3, else None."""
        if ins.mnemonic != 'test' or len(ins.operands) != 2:
            return None
        dst, src = ins.operands
        if dst.type != X.X86_OP_REG or ins.reg_name(dst.reg) != 'ah' or src.type != X.X86_OP_IMM:
            return None
        mask = src.imm & 0xff
        return mask if mask and not mask & ~C.FTEST_BITS else None

    def is_call(self, i):
        return self.branch[i][0] in ('call', 'dyncall')

    def is_fused_fnstsw(self, i):
        return self.fused.get(i) is not None and self.instructions[i].mnemonic in ('fnstsw', 'fstsw')

    # ---- effects ------------------------------------------------------------------------------------------
    def record(self):
        ctx = C.Ctx(self, self.function_names, record=True)
        for i, ins in enumerate(self.instructions):
            ctx.begin(i)
            if self.branch[i][0] != 'skip':
                codegen.generate(ctx, ins)
            e = ctx.effects
            self.effects[i] = e
            # A partial register write merges into the register's value: it reads it where the register is live after
            # it (the write does not kill it), or always with partial_reads.
            partial = e.reg_partial if self.program.partial_reads else 0
            self.use[i] = e.flag_use | e.flag_sync | (e.reg_use | partial) << REG_SHIFT
            self.kill[i] = e.flag_def | e.reg_def << REG_SHIFT
            eflags = getattr(ins, 'eflags', 0) if self.branch[i][0] != 'skip' else 0
            for f, sets in X86_SETS.items():
                if eflags & sets and not (e.flag_def | e.flag_maydef) & C.FLAG_BIT[f]:
                    self.unmodeled[i] |= C.FLAG_BIT[f]
            if self.is_fused_fnstsw(i) or self.masks_condition_codes(i):
                self.use[i] &= ~C.FLAG_BIT['fc']    # fused: read only when eax is live (see liveness)
        # A failed assertion traps: nothing runs after an instruction the generator cannot translate.
        for i in range(self.n):
            if self.effects[i].boundary == 'trap':
                self.succ[i] = []
                self.exits[i] = []
        self.pred = [[] for _ in range(self.n)]
        for i, s in enumerate(self.succ):
            for j in s:
                self.pred[j].append(i)
        self.reachable = set()
        stack = [self.entry]
        while stack:
            i = stack.pop()
            if i not in self.reachable:
                self.reachable.add(i)
                stack.extend(self.succ[i])

    def masks_condition_codes(self, i):
        """fnstsw ax directly followed by and eax/ax, imm clearing C0, C2 and C3."""
        ins = self.instructions[i]
        if ins.mnemonic not in ('fnstsw', 'fstsw') or i + 1 >= self.n or self.is_label(i + 1):
            return False
        op = ins.operands[0]
        if op.type != X.X86_OP_REG or ins.reg_name(op.reg) != 'ax':
            return False
        nxt = self.instructions[i + 1]
        if nxt.mnemonic != 'and' or nxt.operands[0].type != X.X86_OP_REG or nxt.operands[1].type != X.X86_OP_IMM:
            return False
        return nxt.reg_name(nxt.operands[0].reg) in ('eax', 'ax') and not (nxt.operands[1].imm & 0x4500)

    def may_def_local(self):
        """State (flags, registers) this function may change, before its callees."""
        mask = 0
        for i in self.reachable:
            e = self.effects[i]
            mask |= e.flag_def | e.flag_maydef | (e.reg_def | e.reg_partial) << REG_SHIFT
            if self.instructions[i].mnemonic in MEMORY_DEFS:
                mask |= C.ALL_FLAGS
            for x in self.exits[i]:
                if x.kind in ('dyncall', 'dyntail'):
                    mask |= ALL_STATE
        return mask

    # ---- liveness -----------------------------------------------------------------------------------------
    def exit_use(self, x):
        p = self.program
        if x.kind == 'ret':
            return p.ret_live[self.entry_address]
        if x.kind == 'tail':
            # What the target always writes before reading reaches the caller from the target.
            passed = p.ret_live[self.entry_address] & ~(p.must_def.get(x.target, 0) << REG_SHIFT)
            return p.live_in.get(x.target, ALL_STATE) | passed
        if x.kind == 'dyntail':
            return p.indirect_live_in | p.ret_live[self.entry_address]
        if x.kind == 'call':
            return p.live_in.get(x.target, ALL_STATE)
        if x.kind == 'dyncall':
            return p.indirect_live_in
        raise AssertionError(x.kind)

    def entry_defs(self):
        """eax, ecx and edx this function writes before reading them on every path: the straight run from the entry
        (through direct calls, by their live-in and must_def) up to the first branch, return or other exit."""
        p = self.program
        regs = lambda state: (state >> REG_SHIFT) & C.ALL_REGS
        read = defined = 0
        i = self.entry
        while True:
            if self.branch[i][0] == 'skip' or self.effects[i].boundary == 'trap':
                break
            read |= regs(self.use[i]) & ~defined
            defined |= regs(self.kill[i]) & ~read
            stop = self.succ[i] != [i + 1]
            for x in self.exits[i]:
                if x.kind == 'call' or (x.kind == 'tail' and x.edge == 'exit'):
                    read |= regs(p.live_in.get(x.target, ALL_STATE)) & ~defined
                    defined |= p.must_def.get(x.target, 0) & ~read
                if x.kind != 'call':
                    stop = True
            if stop:
                break
            i += 1
        return defined & CALL_CLOBBERED & regs(p.may_def.get(self.entry_address, ALL_STATE))

    def restored_pairs(self):
        """pushfd index -> popfd index of the flags saved around a call and restored (Program.flags_restored)."""
        pairs = {}
        for push, pop in self.program.flags_restored.items():
            if push in self.index_of:
                i, j = self.index_of[push], self.index_of[pop]
                assert self.instructions[i].mnemonic == 'pushfd' and self.instructions[j].mnemonic == 'popfd', \
                    '0x%x/0x%x is no pushfd/popfd pair' % (push, pop)
                pairs[i] = j
        return pairs

    def liveness(self):
        """Backward liveness of flags and registers; calls pass live state through. A pushfd whose word only its
        popfd reads uses the flags live after that popfd."""
        n = self.n
        live_in = self.live_in if self.live_in else [0] * n
        out = [0] * n
        eax = C.REG_BIT['eax'] << REG_SHIFT
        fc = C.FLAG_BIT['fc']
        restored = self.restored_pairs()
        changed = True
        while changed:
            changed = False
            for i in range(n - 1, -1, -1):
                o = 0
                for s in self.succ[i]:
                    o |= live_in[s]
                call_use = 0
                call_kill = 0
                for x in self.exits[i]:
                    if x.edge == 'call':
                        call_use |= self.exit_use(x)
                        if x.kind == 'call':
                            call_kill |= self.program.must_def.get(x.target, 0) << REG_SHIFT
                    else:
                        o |= self.exit_use(x)
                use = self.use[i]
                if i in restored:
                    use = (use & ~C.CPU_FLAG_MASK) | (out[restored[i]] & C.CPU_FLAG_MASK)
                if (o & eax) and self.is_fused_fnstsw(i):
                    use |= fc
                v = use | call_use | (o & ~(self.kill[i] | call_kill))
                out[i] = o
                if v != live_in[i]:
                    live_in[i] = v
                    changed = True
        self.live_in = live_in
        self.live_out = out
        return live_in[self.entry]

    def writes_ax(self, i):
        return bool(self.live_out[i] & (C.REG_BIT['eax'] << REG_SHIFT))

    def flag_live_out(self, i, f):
        return bool(self.live_out[i] & C.FLAG_BIT[f])

    def reg_live_out(self, i, r):
        return bool(self.live_out[i] & (C.REG_BIT[r] << REG_SHIFT))

    # ---- reaching definitions -----------------------------------------------------------------------------
    def callee_delta(self, i):
        """esp change of a direct call (the call's own -4 included), or None."""
        b = self.branch[i]
        if b[0] != 'call':
            return None
        info = self.program.stack.get(b[1])
        return None if info is None or info.delta is None else info.delta - 4

    def callee_may_def(self, i):
        b = self.branch[i]
        if b[0] == 'call':
            return self.program.may_def.get(b[1], ALL_STATE)
        return ALL_STATE

    def is_memory_producer(self, p, f):
        """p leaves cpu.flags authoritative for f: entry, a call that may change f, popfd."""
        if p == self.ENTRY:
            return True
        return self.is_call(p) or self.instructions[p].mnemonic in MEMORY_DEFS

    def reaching(self, i, f):
        """Producers of flag f reaching the start of instruction i (indices or ENTRY)."""
        key = (i, f)
        cached = self._rd_cache.get(key)
        if cached is not None:
            return cached
        bit = C.FLAG_BIT[f]
        result = set()
        seen = set()
        stack = list(self.pred[i])
        if i == self.entry:
            result.add(self.ENTRY)
        while stack:
            p = stack.pop()
            if p in seen:
                continue
            seen.add(p)
            if self.is_call(p):
                if self.callee_may_def(p) & bit:
                    result.add(p)
                    continue
            elif self.instructions[p].mnemonic in MEMORY_DEFS:
                result.add(p)
                continue
            else:
                e = self.effects[p]
                if e.flag_def & bit:
                    result.add(p)
                    continue
                if e.flag_maydef & bit:
                    result.add(p)
            if p == self.entry:
                result.add(self.ENTRY)
            stack.extend(self.pred[p])
        result = frozenset(result)
        self._rd_cache[key] = result
        return result

    def check_modeled(self, i, f):
        """Fails when a read of pf/af at i can see a value an unmodelled producer set on x86."""
        bit = C.FLAG_BIT[f]
        seen = set()
        stack = list(self.pred[i])
        while stack:
            p = stack.pop()
            if p in seen:
                continue
            seen.add(p)
            if self.unmodeled[p] & bit:
                raise AssertionError('0x%x (%s) reads %s set by 0x%x (%s), which the generator does not model'
                                     % (self.instructions[i].address, self.instructions[i].mnemonic, f,
                                        self.instructions[p].address, self.instructions[p].mnemonic))
            if not (self.is_call(p) or self.instructions[p].mnemonic in MEMORY_DEFS or self.effects[p].flag_def & bit):
                stack.extend(self.pred[p])

    def lazy_producer(self, i, f):
        """The single lazy producer serving flag f at instruction i, or None."""
        rd = self.reaching(i, f)
        if len(rd) != 1:
            return None
        (p,) = rd
        if self.is_memory_producer(p, f):
            return None
        e = self.effects[p]
        if not e.lazy or e.flag_maydef:
            return None
        if f not in C.LAZY_KINDS[e.lazy[0]][2]:
            return None
        return p

    # ---- decisions ----------------------------------------------------------------------------------------
    def boundary_mask(self, i, x):
        """CPU flags code outside may read on exit edge x of instruction i."""
        if x.edge == 'call':
            after = self.live_out[i]
            return (self.exit_use(x) | (after & self.callee_may_def(i))) & C.CPU_FLAG_MASK
        return self.exit_use(x) & C.CPU_FLAG_MASK

    def need_valid(self, i, f):
        """Every producer reaching a read of f at i must provide a valid f_<flag>. A lazy producer keeps its
        operands only for flags computed from them (not for the constant cf and of of a logic producer)."""
        p = self.lazy_producer(i, f)
        if p is not None:
            kind, bits = self.effects[p].lazy
            if C.lazy_flag_uses_operands(kind, bits, f):
                self.lazy_readers.add(p)
            return
        for q in self.reaching(i, f):
            if self.is_memory_producer(q, f):
                self.reloads.add((q, f))
            else:
                self.local_needed.add((q, f))

    def dirty(self, i, f):
        return any(not self.is_memory_producer(q, f) for q in self.reaching(i, f))

    def decide(self):
        restored = self.restored_pairs()
        for i in range(self.n):
            mask = self.effects[i].flag_use & C.CPU_FLAG_MASK
            for f in C.CPU_FLAGS:
                if mask & C.FLAG_BIT[f]:
                    if f in X86_SETS and i in self.reachable:
                        self.check_modeled(i, f)
                    self.need_valid(i, f)
            stores = {}
            for x in self.exits[i]:
                mask = self.boundary_mask(i, x)
                for f in C.CPU_FLAGS:
                    if mask & C.FLAG_BIT[f] and self.dirty(i, f):
                        stores[x.edge] = stores.get(x.edge, 0) | C.FLAG_BIT[f]
                        self.need_valid(i, f)
            if self.effects[i].boundary == 'flags':
                synced = self.live_out[restored[i]] if i in restored else C.CPU_FLAG_MASK
                for f in C.CPU_FLAGS:
                    if synced & C.FLAG_BIT[f] and self.dirty(i, f):
                        stores['flags'] = stores.get('flags', 0) | C.FLAG_BIT[f]
                        self.need_valid(i, f)
            for edge, mask in stores.items():
                self.stores[(i, edge)] = mask
        if self.local_registers:
            self.decide_registers()

    def boundary_registers(self, i, x):
        """Registers code outside may read on exit edge x of instruction i."""
        if x.edge == 'call':
            mask = self.exit_use(x) | (self.live_out[i] & self.callee_may_def(i))
        else:
            mask = self.exit_use(x)
        return (mask >> REG_SHIFT) & C.ALL_REGS

    def decide_registers(self):
        """Registers live in locals: store the ones changed since cpu was last valid where code outside reads
        them, reload after a call the ones it may change that are read later.

        Callers keep the registers this function preserves in their locals and may still count them as valid
        in cpu. So when cpu's copy of a preserved register was overwritten here (stored at a boundary, or
        changed by a callee), it is stored back before leaving: the local holds the entry value there."""
        info = self.program.stack.get(self.entry_address)
        preserved = info.preserved if info else 0
        state_in = [None] * self.n
        state_in[self.entry] = (0, 0)       # (dirty: local newer than cpu, clobbered: cpu differs from entry)
        work = [self.entry]
        while work:
            i = work.pop()
            e = self.effects[i]
            dirty, clobbered = state_in[i]
            if e.boundary == 'runtime':
                # cpuid/rdtsc: their inputs are stored to cpu and their outputs written there.
                clobbered |= e.reg_use | e.reg_def
            dirty |= e.reg_def | e.reg_partial
            for x in self.exits[i]:
                boundary = self.boundary_registers(i, x)
                if x.kind == 'ret':
                    # A preserved register holds its entry value at a return: cpu has it unless clobbered.
                    boundary &= ~preserved
                stored = dirty & boundary
                if x.edge != 'call':
                    stored |= clobbered & preserved
                if stored:
                    key = (i, x.edge)
                    self.reg_stores[key] = self.reg_stores.get(key, 0) | stored
                if x.edge == 'call':
                    changed = (self.callee_may_def(i) >> REG_SHIFT) & C.ALL_REGS
                    self.reg_reloads[i] = changed & (self.live_out[i] >> REG_SHIFT)
                    clobbered |= stored | changed
                    dirty &= ~stored & ~changed
                    if self.callee_delta(i) is not None:
                        # esp is moved by the callee's known delta here, not reloaded: cpu.esp may be stale.
                        dirty |= C.REG_BIT['esp']
            out = (dirty, clobbered)
            for s in self.succ[i]:
                old = state_in[s]
                new = out if old is None else (old[0] | dirty, old[1] | clobbered)
                if new != old:
                    state_in[s] = new
                    work.append(s)

    # ---- queries used by the code generators --------------------------------------------------------------
    def needs_local(self, i, f):
        return (i, f) in self.local_needed

    def saves_operands(self, i):
        return i in self.lazy_readers

    def fcmp_saved(self, sahf):
        return sahf in self.lazy_readers or any(self.needs_local(sahf, f) for f in ('cf', 'pf', 'zf', 'sf', 'of'))

    def lazy_values(self, p):
        kind, bits = self.effects[p].lazy
        address = self.instructions[p].address
        return kind, bits, {name: C.lazy_local(kind, address, name) for name in C.LAZY_KINDS[kind][0]}

    def flag_source(self, i, f):
        p = self.lazy_producer(i, f)
        if p is None:
            return 'f_' + f
        kind, bits, values = self.lazy_values(p)
        return C.lazy_flags(kind, bits, values)[f]

    def condition(self, i, cc):
        flags = C.CONDITIONS[cc]
        producers = set(self.lazy_producer(i, f) for f in flags)
        if len(producers) == 1:
            (p,) = producers
            if p is not None:
                kind, bits, values = self.lazy_values(p)
                direct = C.lazy_condition(kind, bits, values, cc)
                if direct:
                    return direct
        return C.generic_condition(cc, {f: self.flag_source(i, f) for f in flags})

    def store_lines(self, i, edge, fdepth=None):
        """Stores before leaving on edge; fdepth is the x87 depth there when the stack is in locals."""
        mask = self.stores.get((i, edge), 0)
        regs = self.reg_stores.get((i, edge), 0)
        lines = ['cpu.%s = %s;' % (r, r) for r in C.REGS if regs & C.REG_BIT[r]]
        lines += ['cpu.flags.%s = %s;' % (f, self.flag_source(i, f)) for f in C.CPU_FLAGS if mask & C.FLAG_BIT[f]]
        if fdepth is not None and edge != 'flags':
            for j in range(1, fdepth + 1):
                lines.append('cpu.fpu.regs[(fpu_count + %d) & 7] = fp%d;' % (j, j))
            lines.append('cpu.fpu.count = fpu_count %s %d;' % ('+' if fdepth >= 0 else '-', abs(fdepth)))
        return lines

    def reload_lines(self, i):
        regs = self.reg_reloads.get(i, 0)
        lines = ['%s = cpu.%s;' % (r, r) for r in C.REGS if regs & C.REG_BIT[r]]
        delta = self.callee_delta(i) if self.local_registers else None
        if delta is not None:
            lines.append('esp = esp + %d;' % (delta + 4))
        lines += ['f_%s = cpu.flags.%s;' % (f, f) for f in C.CPU_FLAGS if (i, f) in self.reloads]
        d = self.fpu.depth[i]
        after = self.fpu_after_call(i)
        if self.program.fpu_check and after is not None:
            lines.append('RC_ASSERT(cpu.fpu.count == fpu_count %s %d);' % ('+' if after >= 0 else '-', abs(after)))
        elif self.branch[i][0] == 'dyncall' and self.program.indirect_fpu_checked and d is not None \
                and self.fpu.evidence.get(i) == 'inferred':
            # The callee keeps the x87 depth or pushes the float result the analysis inferred; the runtime logs and
            # corrects any other depth. Import calls need no check; open calls keep the stack in cpu.fpu.
            d += self.indirect_fpu_delta(i)
            lines.append('RC_FPU_CALL(cpu, fpu_count %s %d, 0x%xu);'
                         % ('+' if d >= 0 else '-', abs(d), self.instructions[i].address))
        if after is not None:
            for j in range(1, after + 1):
                lines.append('fp%d = cpu.fpu.regs[(fpu_count + %d) & 7];' % (j, j))
        return lines

    # ---- x87 stack in locals ------------------------------------------------------------------------------
    def fpu_depth(self, i):
        """x87 depth above the entry stack before instruction i, when the function keeps it in locals."""
        if not self.fpu.local:
            return None
        return self.fpu.depth[i]

    def fpu_after_call(self, i):
        d = self.fpu_depth(i)
        if d is None:
            return None
        b = self.branch[i]
        delta = self.program.fpu_deltas.get(b[1]) if b[0] == 'call' else self.indirect_fpu_delta(i)
        if delta is None or delta == fpustack.NEVER:
            return None
        return d + delta

    def indirect_fpu_delta(self, i):
        """Net x87 pushes of the callee of the indirect call at i."""
        return self.fpu.sites.get(i, self.program.indirect_fpu_delta)

    def fpu_exit_depth(self, i):
        """x87 depth when control falls off the end after instruction i."""
        d = self.fpu_depth(i)
        if d is None:
            return None
        if self.is_call(i):
            return self.fpu_after_call(i)
        return d + fpustack.effect(self.instructions[i].mnemonic.split()[-1])

    def entry_loads(self):
        return [f for f in C.CPU_FLAGS if (self.ENTRY, f) in self.reloads]

    def lazy_locals(self):
        """Declarations of the saved operands of lazy producers."""
        names = []
        for p in sorted(self.lazy_readers):
            kind, _, values = self.lazy_values(p)
            if C.is_fcompare_kind(kind):
                continue
            for name, local in values.items():
                names.append((C.lazy_local_type(name), local))
        for i, s in self.fused.items():
            if i == s and self.fcmp_saved(s):
                address = self.instructions[s].address
                for name in ('fa', 'fb'):
                    names.append(('double', C.lazy_local('fcmp', address, name)))
        return sorted(set(names), key=lambda x: x[1])


class Program:
    def __init__(self, modules, thread_segments, skip_instructions, function_names, flags_preserved=(), not_code=(),
                 native_overrides=(), local_registers=True, memory_functions=(), local_x87=True, setjmp=None,
                 longjmp_jumps=None, fpu_check=False, fpu_deltas=None, preempt_loops=False, address_taken=None,
                 import_fpu_deltas=None, flags_restored=None, not_function_pointers=(), partial_reads=False,
                 constant_sections=(), mutable_constants=(), resource_sections=(), code_tables=None,
                 restrict_cpu=False, preempt_stub=False):
        self.functions = {}
        # Loop preemption points yield through a stub that keeps the caller-saved registers.
        self.preempt_stub = preempt_stub
        # Translated functions take cpu as a restrict reference.
        self.restrict_cpu = restrict_cpu
        # A partial register write (al, ah, ax) reads the whole register, else only where it is live after the write.
        self.partial_reads = partial_reads
        # x87 operands at fixed addresses of constant_sections become constants (find_constants); mutable_constants:
        # (address, length) ranges the port writes; code_tables: table start -> end in code sections (None: unknown).
        self.constant_sections = tuple(constant_sections)
        self.mutable_constants = tuple(mutable_constants)
        self.resource_sections = tuple(resource_sections)
        self.code_tables = dict(code_tables or {})
        self.constant_bounds = ()
        self.not_constant = frozenset()
        # Backward jumps are preemption points (WinApplication::allowPreemption).
        self.preempt_loops = preempt_loops
        # Check the x87 depth after every call with a known result depth (diagnostic builds).
        self.fpu_check = fpu_check
        # Declared net x87 pushes of functions whose exits the analysis cannot follow.
        self.declared_fpu_deltas = dict(fpu_deltas or {})
        # setjmp: calls to it keep a host jump buffer, and the code after them reloads every register it reads
        # (a longjmp resumes there with the registers setjmp saved). longjmp_jumps: indirect jumps that resume
        # after a setjmp call (address -> register holding the guest jmp_buf).
        self.setjmp = setjmp
        self.longjmp_jumps = dict(longjmp_jumps or {})
        self.local_registers = local_registers
        self.local_x87 = local_x87
        self.memory_functions = set(memory_functions)
        self.native_overrides = set(native_overrides)
        # Functions that save the caller's flags on entry (pushfd) and restore them before returning (popfd):
        # callers neither provide nor lose flags across a call to them.
        self.flags_preserved = set(flags_preserved)
        # pushfd address -> popfd address: the flags saved around a call and restored, the word read only by the popfd.
        self.flags_restored = dict(flags_restored or {})
        # Entries that are not reached by indirect calls: neither registered for them nor given their state.
        self.not_function_pointers = set(not_function_pointers)
        for module in modules:
            for sub in module.subroutines:
                fn = Function(module, sub, thread_segments, skip_instructions, function_names)
                fn.program = self
                self.functions[fn.entry_address] = fn
        # Import address table slots: indirect calls through them reach host shims.
        self.import_slots = set(imp.iat_address for module in modules for imp in module.image.imports)
        # Slot -> fake address of the import: the runtime stores it there and registers the import's shim at it.
        self.import_fakes = dict((imp.iat_address, imp.fake_address) for module in modules
                                 for imp in module.image.imports)
        # Entries that only jump through an import slot (jmp [slot]): calls to them reach host shims.
        self.import_thunks = set(a for a, fn in self.functions.items() if self.import_thunk(fn))
        # Net x87 pushes of imports by slot (C runtime functions with x87 arguments or a float result).
        self.import_fpu_deltas = dict(import_fpu_deltas or {})
        if address_taken is None:
            address_taken = self.find_address_taken(modules)
        self.address_taken = (set(address_taken) & set(self.functions)) - set(not_code)
        self.live_in = {}
        self.ret_live = {}
        self.may_def = {}
        self.must_def = {}
        self.indirect_live_in = 0

    def import_thunk(self, fn):
        """True when the function's entry instruction is jmp [slot] through an import address table slot."""
        ins = fn.instructions[fn.entry]
        if ins.mnemonic != 'jmp' or not ins.operands or ins.operands[0].type != X.X86_OP_MEM:
            return False
        mem = ins.operands[0].mem
        return mem.base == 0 and mem.index == 0 and (mem.disp & 0xffffffff) in self.import_slots

    def find_address_taken(self, modules):
        """Entries that may be called indirectly, for an image with relocations: module entry points and every entry
        a relocated dword points to (function pointers in data, pushed or stored immediates, table slots). Images
        without relocations pass the set pointer discovery found (Program(address_taken=...))."""
        entries = set(self.functions)
        taken = set()
        for module in modules:
            taken.add(module.entry_point)
            image = module.image
            for r in image.relocations:
                value = image.dword(r)
                if value in entries:
                    taken.add(value)
        return taken & entries

    def compute_may_def(self, fns, stack_infos=None):
        """State each function may change, its callees included. With stack_infos, registers a function restores
        before returning and an esp change known to its callers do not count."""
        def kept(a):
            info = stack_infos.get(a) if stack_infos else None
            if not info:
                return 0
            mask = info.preserved << REG_SHIFT
            if info.delta is not None:
                mask |= C.REG_BIT['esp'] << REG_SHIFT
            return mask
        local = dict((fn.entry_address, fn.may_def_local() & ~kept(fn.entry_address)) for fn in fns)
        callees = dict((fn.entry_address, set(x.target for i in fn.reachable for x in fn.exits[i]
                                              if x.kind in ('call', 'tail'))) for fn in fns)
        may_def = dict(local)
        changed = True
        while changed:
            changed = False
            for a, targets in callees.items():
                v = may_def[a]
                for t in targets:
                    v |= may_def.get(t, ALL_STATE)
                v &= ~kept(a)
                if a in self.flags_preserved:
                    v &= ~C.ALL_FLAGS
                if v != may_def[a]:
                    may_def[a] = v
                    changed = True
        self.may_def = may_def

    def analyze(self, log=print, decide=True):
        """decide=False leaves the per-function decisions (Function.decide) to the caller."""
        fns = list(self.functions.values())
        if self.constant_sections:
            self.find_constants(log)
        for fn in fns:
            fn.classify()
            fn.record()
        self.compute_may_def(fns)
        self.stack = stack.analyze(self, fns)
        fpu = fpustack.analyze(self, fns)
        self.fpu_deltas = dict((a, info.delta) for a, info in fpu.items())
        for fn in fns:
            fn.fpu = fpu[fn.entry_address]
        self.compute_may_def(fns, self.stack)
        if self.setjmp in self.functions:
            self.stack[self.setjmp] = stack.StackInfo()
            self.may_def[self.setjmp] = ALL_STATE
        self.solve_liveness(fns, log)
        # Registers a callee always writes before reading end their liveness at the call: from the first solution
        # (a superset of the next one's live-in), then liveness again.
        self.compute_must_def(fns)
        if any(self.must_def.values()):
            for fn in fns:
                fn.live_in = None
            self.solve_liveness(fns, log)
        if decide:
            for fn in fns:
                fn.decide()

    def find_constants(self, log):
        """Addresses of the constant sections that may change: written, taken as a value or table base, held by a data
        dword (outside text and resources; in code sections only in undecoded runs from data references), mutable."""
        images = []
        for fn in self.functions.values():
            if fn.module.image not in images:
                images.append(fn.module.image)
        bounds = []
        for image in images:
            for s in image.sections:
                if s.name in self.constant_sections:
                    bounds.append((s.address, s.end))
        self.constant_bounds = tuple(bounds)

        def inside(a):
            return any(lo <= a < hi for lo, hi in bounds)

        changing = set()
        for slot in self.import_slots:
            changing.update(range(slot, slot + 4))
        for a, n in self.mutable_constants:
            changing.update(range(a, a + n))
        decoded = dict((image, bytearray(image.end_address() - image.base)) for image in images)
        data_starts = dict((image, set()) for image in images)
        writes = 0
        for fn in self.functions.values():
            image = fn.module.image
            mark = decoded[image]
            starts = data_starts[image]
            for ins in fn.instructions:
                if not ins.size:
                    continue
                mark[ins.address - image.base:ins.address - image.base + ins.size] = b'\1' * ins.size
                branch = ins.mnemonic.startswith('j') or ins.mnemonic in ('call', 'loop', 'loope', 'loopne')
                for op in ins.operands:
                    if op.type == X.X86_OP_MEM:
                        a = op.mem.disp & 0xffffffff
                        if image.is_code(a):
                            starts.add(a)
                        if not inside(a):
                            continue
                        if op.access & 2:       # CS_AC_WRITE
                            changing.update(range(a, a + max(op.size, 1)))
                            writes += 1
                        if ins.mnemonic == 'lea' or op.mem.base or op.mem.index or op.mem.segment:
                            changing.add(a)
                    elif op.type == X.X86_OP_IMM and not branch:
                        a = op.imm & 0xffffffff
                        if inside(a):
                            changing.add(a)
                        elif image.is_code(a):
                            starts.add(a)
        pointers = 0
        for image in images:
            mark = decoded[image]
            starts = data_starts[image]
            sections = [s for s in image.sections if s.name not in self.resource_sections]

            def words(s):
                """Aligned dwords of a section outside text: (address, value)."""
                text = text_bytes(s.data)
                for offset in range(0, len(s.data) - 3, 4):
                    if not (text[offset] and text[offset + 3]):
                        yield s.address + offset, int.from_bytes(s.data[offset:offset + 4], 'little')

            for s in sections:
                if not s.code:
                    starts.update(v for _, v in words(s) if image.is_code(v))
            # Code-section data: undecoded bytes from each start to the next decoded instruction (3) or table end (2);
            # a run stops where an earlier unbounded run already marked.
            for a in starts:
                i = a - image.base
                end = self.code_tables.get(a)
                value = 3 if end is None else 2
                end = len(mark) if end is None else min(len(mark), end - image.base)
                while 0 <= i < end and mark[i] not in (1, 3):
                    mark[i] = value
                    i += 1
            for s in sections:
                for a, v in words(s):
                    if inside(v) and (not s.code or mark[a - image.base] >= 2):
                        changing.add(v)
                        pointers += 1
        self.not_constant = frozenset(changing)
        log('constant sections %s: %d instruction writes, %d data pointers, %d addresses that may change'
            % (' '.join(self.constant_sections), writes, pointers, len(changing)))

    def constant_bits(self, image, address, size):
        """The little-endian value of size bytes at a guest address that never changes (find_constants), or None."""
        if not any(lo <= address and address + size <= hi for lo, hi in self.constant_bounds):
            return None
        if any(a in self.not_constant for a in range(address, address + size)):
            return None
        data = image.read(address, size)
        return int.from_bytes(data, 'little') if len(data) == size else None

    def compute_must_def(self, fns):
        """eax, ecx and edx each function writes on every path before reading them (Function.entry_defs)."""
        self.must_def = {}
        changed = True
        while changed:
            changed = False
            for fn in fns:
                v = fn.entry_defs()
                if v != self.must_def.get(fn.entry_address, 0):
                    self.must_def[fn.entry_address] = v
                    changed = True

    def solve_liveness(self, fns, log):
        """Live-in, return liveness and the indirect live-in of every function, to a fixed point."""
        self.indirect_live_in = 0
        for fn in fns:
            self.live_in[fn.entry_address] = 0
            self.ret_live[fn.entry_address] = 0
        # Host code (threads, callbacks, window procedures) may read any register after an indirect call.
        host_ret = ALL_REG_STATE
        indirect_entries = self.address_taken - self.not_function_pointers
        iteration = 0
        # A function whose callees' live-in, return liveness and indirect live-in are unchanged keeps its result.
        tracker = stack.Dependencies(fns)
        inputs = {}
        while True:
            iteration += 1
            changed = False
            for fn in fns:
                now = (self.ret_live[fn.entry_address], self.indirect_live_in)
                if not tracker.stale(fn) and inputs[fn.entry_address] == now:
                    continue
                inputs[fn.entry_address] = now
                v = fn.liveness()
                if fn.entry_address in self.flags_preserved:
                    v &= ~C.ALL_FLAGS
                if v != self.live_in[fn.entry_address]:
                    self.live_in[fn.entry_address] = v
                    tracker.changed(fn.entry_address)
                    changed = True
            indirect_live_in = ALL_REG_STATE
            for a in indirect_entries:
                indirect_live_in |= self.live_in[a]
            ret_live = dict((a, 0) for a in self.functions)
            indirect_ret = host_ret
            for fn in fns:
                for i in fn.reachable:
                    for x in fn.exits[i]:
                        if x.kind == 'call':
                            if x.target in ret_live:
                                ret_live[x.target] |= fn.live_out[i]
                        elif x.kind == 'dyncall':
                            indirect_ret |= fn.live_out[i]
                        elif x.kind == 'tail':
                            if x.target in ret_live:
                                ret_live[x.target] |= self.ret_live[fn.entry_address]
                        elif x.kind == 'dyntail':
                            indirect_ret |= self.ret_live[fn.entry_address]
            for a in indirect_entries:
                ret_live[a] |= indirect_ret
            if indirect_live_in != self.indirect_live_in or ret_live != self.ret_live:
                changed = True
            self.indirect_live_in = indirect_live_in
            self.ret_live = ret_live
            if not changed:
                break
        log('liveness: %d iterations; %d address-taken functions; indirect live-in %s; indirect return %s'
            % (iteration, len(self.address_taken), state_names(self.indirect_live_in), state_names(indirect_ret)))


def state_names(mask):
    names = [f for f in C.FLAGS if mask & C.FLAG_BIT[f]]
    names += [r for r in C.REGS if mask & (C.REG_BIT[r] << REG_SHIFT)]
    return ','.join(names) or '-'


TEXT = re.compile(rb'[\t\n\r\x20-\x7e]{4,}\x00')


def text_bytes(data):
    """1 for each byte of NUL-terminated text (four or more printable characters and the NUL), else 0."""
    mark = bytearray(len(data))
    for m in TEXT.finditer(data):
        mark[m.start():m.end()] = b'\1' * (m.end() - m.start())
    return mark
