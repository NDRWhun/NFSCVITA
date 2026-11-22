"""Jumps, calls and returns."""

from capstone import x86 as X

from ..image import FAKE_IMPORT_BASE


def _import_fake(ctx, op):
    """Fake address of the import whose address table slot the memory operand is, else None."""
    if op.type != X.X86_OP_MEM or op.mem.base or op.mem.index or op.mem.segment:
        return None
    return ctx.fn.program.import_fakes.get(op.mem.disp & 0xffffffff)


def _shim_call(fake, target, tail):
    """While target is the import's fake address, the call dynamic_call makes there: the preemption point, then the
    import's shim from importShims (generated with the method registration)."""
    shim = '%simportShims[%d](app, cpu);' % ('return ' if tail else '', fake - FAKE_IMPORT_BASE)
    return ['if (%s == 0x%xu)' % (target, fake), '{', '    app->allowPreemption(cpu);', '    ' + shim, '}']


def _target(ctx, edge):
    """Statements transferring control to the target of the current jump."""
    kind = ctx.fn.branch[ctx.index]
    if kind[0] == 'label':
        jump = ['goto L_0x%08x;' % kind[1]]
        program = ctx.fn.program
        if kind[1] <= ctx.ins.address and program.preempt_loops:
            # A loop: preemption point, so a spin on state that a timer callback or a higher-priority thread
            # changes lets that thread take the execution lock.
            jump.insert(0, 'app->%s(cpu);' % ('allowPreemptionInLoop' if program.preempt_stub else 'allowPreemption'))
        return jump
    if kind[0] == 'tail':
        return ctx.exit_sync(edge) + ['return %s(app, cpu);' % ctx.function_name(kind[1])]
    op = ctx.ins.operands[0]
    if kind[0] == 'switch':
        return ['cpu.ip = %s;' % ctx.val(op, 32), 'goto dynamic_jump;']
    target = ctx.val(op, 32)
    ctx.boundary('dyncall')
    buffer = ctx.fn.program.longjmp_jumps.get(ctx.ins.address)
    if buffer:
        # longjmp: resumes after the setjmp call that filled the guest jmp_buf.
        return ctx.exit_sync(edge) + ['rc::longJump(app, cpu, %s);' % ctx.reg(buffer)]
    fake = _import_fake(ctx, op)
    if fake is not None:
        lines = ['const x86::reg32 t = %s;' % target] + _shim_call(fake, 't', True) + \
            ['return app->dynamic_call(t, cpu);']
        return ctx.exit_sync(edge) + ['{'] + ['    ' + l for l in lines] + ['}']
    return ctx.exit_sync(edge) + ['return app->dynamic_call(%s, cpu);' % target]


def cg_jmp(ctx, *operands):
    return _target(ctx, 'exit')


def cg_jcc(ctx, cc):
    return ['if (%s)' % ctx.cond(cc), '{'] + ['    ' + l for l in _target(ctx, 'taken')] + ['}']


def cg_jcxz(ctx, *operands):
    return ['if (%s == 0)' % ctx.reg('cx'), '{'] + ['    ' + l for l in _target(ctx, 'taken')] + ['}']


def cg_jecxz(ctx, *operands):
    return ['if (%s == 0)' % ctx.reg('ecx'), '{'] + ['    ' + l for l in _target(ctx, 'taken')] + ['}']


def _loop(ctx, condition):
    """loop/loope/loopne: ecx is decremented without changing flags."""
    jump = ['if (%s != 0%s)' % (ctx.reg('ecx'), condition), '{'] + ['    ' + l for l in _target(ctx, 'taken')] + ['}']
    return ctx.set_reg('ecx', '%s - 1' % ctx.reg('ecx')) + jump


def cg_loop(ctx, *operands):
    return _loop(ctx, '')


def cg_loope(ctx, *operands):
    return _loop(ctx, ' && %s' % ctx.flag('zf'))


def cg_loopne(ctx, *operands):
    return _loop(ctx, ' && !%s' % ctx.flag('zf'))


def cg_call(ctx, destination):
    # Guest callees take their arguments relative to esp; only calls that may reach host code (indirect calls, import
    # thunks and native overrides) write the return address, which the shims and native code read as their caller.
    def push(write):
        lines = ctx.set_reg('esp', '%s - 4' % ctx.reg('esp'))
        if write:
            lines += ctx.store(32, ctx.reg('esp'), '0x%xu' % (ctx.ins.address + ctx.ins.size))
        return lines

    if destination.type == X.X86_OP_IMM:
        ctx.boundary('call')
        call = ['%s(app, cpu);' % ctx.function_name(destination.imm), 'if (cpu.terminate) return;']
        if destination.imm == ctx.fn.program.setjmp:
            # setjmp(buffer): the guest jmp_buf is the argument above the return address. A longjmp to it comes back
            # here with the registers in cpu.
            call = ['if (setjmp(rc::jumpBuffer(app, cpu, rc::ld32(%s + 4))) == 0)' % ctx.reg('esp'),
                    '    %s(app, cpu);' % ctx.function_name(destination.imm), 'if (cpu.terminate) return;']
        host = destination.imm in ctx.fn.program.import_thunks or destination.imm in ctx.fn.program.native_overrides
        return push(host) + ctx.exit_sync('call') + call + ctx.after_call()
    ctx.boundary('dyncall')
    target = ['cpu.ip = %s;' % ctx.val(destination, 32)]
    call = ['app->dynamic_call(cpu.ip, cpu);', 'if (cpu.terminate) return;']
    fake = _import_fake(ctx, destination)
    if fake is not None:
        call = _shim_call(fake, 'cpu.ip', False) + ['else', '    ' + call[0], call[1]]
    return target + push(True) + ctx.exit_sync('call') + call + ctx.after_call()


def cg_ret(ctx, *value):
    pop = 4 + (value[0].imm if value else 0)
    return ctx.set_reg('esp', '%s + %d' % (ctx.reg('esp'), pop)) + ctx.exit_sync('exit') + ['return;']
