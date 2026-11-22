"""Decoded x86 instructions as plain Python objects: a copy of the capstone fields the generator reads.

Attribute reads are plain slot reads; capstone goes through ctypes on each one.
"""

from capstone import x86 as X

_REG_NAMES = []     # capstone register id -> name (None for ids without one)


class Memory:
    __slots__ = ('segment', 'base', 'index', 'scale', 'disp')

    def __init__(self, m):
        self.segment = m.segment
        self.base = m.base
        self.index = m.index
        self.scale = m.scale
        self.disp = m.disp


class Operand:
    """reg, imm and mem are copied for every operand type, as capstone reads them from one union."""
    __slots__ = ('type', 'reg', 'imm', 'mem', 'size', 'access')

    def __init__(self, op):
        self.type = op.type
        self.reg = op.reg
        self.imm = op.imm
        self.mem = Memory(op.mem)
        self.size = op.size
        self.access = op.access


class Instruction:
    __slots__ = ('address', 'size', 'mnemonic', 'op_str', 'bytes', 'prefix', 'opcode', 'eflags', 'groups',
                 'operands', '_regs_access', 'potential_destinations')

    def __init__(self, ins):
        if not _REG_NAMES:
            _REG_NAMES.extend(ins.reg_name(r) for r in range(X.X86_REG_ENDING))
        self.address = ins.address
        self.size = ins.size
        self.mnemonic = ins.mnemonic
        self.op_str = ins.op_str
        self.bytes = bytes(ins.bytes)
        self.prefix = ins.prefix
        self.opcode = ins.opcode
        self.eflags = ins.eflags
        self.groups = ins.groups
        self.operands = [Operand(op) for op in ins.operands]
        regs_read, regs_written = ins.regs_access()
        self._regs_access = (list(regs_read), list(regs_written))

    def reg_name(self, reg_id, default=None):
        name = _REG_NAMES[reg_id] if 0 <= reg_id < len(_REG_NAMES) else None
        return default if name is None else name

    def regs_access(self):
        return self._regs_access
