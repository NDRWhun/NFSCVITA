"""SSE single-precision instructions on cpu.xmm (include/sse.h), and the SSE instructions that work on MMX
registers. Arithmetic goes through x86::sse helpers so NaN results and conversions match an x86 CPU."""

from capstone import x86 as X


def uses_xmm(ins):
    """True when an operand is an SSE register."""
    return any(op.type == X.X86_OP_REG and ins.reg_name(op.reg).startswith('xmm')
               for op in getattr(ins, 'operands', []))


def xmm(ctx, op):
    return 'cpu.xmm[%s]' % ctx.ins.reg_name(op.reg)[3:]


def mm(ctx, op):
    return 'cpu.mmx.%s' % ctx.ins.reg_name(op.reg)


def lane(ctx, op, i):
    """Float lane i of a register or memory operand."""
    if op.type == X.X86_OP_REG:
        return '%s.f[%d]' % (xmm(ctx, op), i)
    return 'rc::ldf32(%s + %d)' % (ctx.address(op), 4 * i)


def source128(ctx, op):
    """Lines and the name of a 128-bit copy of a register or memory source."""
    if op.type == X.X86_OP_REG:
        return ['const x86::Xmm s = %s;' % xmm(ctx, op)]
    return ['const x86::Xmm s = x86::sse::load128(rc::host(%s));' % ctx.address(op)]


def block(lines):
    return ['{'] + ['    ' + l for l in lines] + ['}']


# ---- moves --------------------------------------------------------------------------------------------------------

def cg_movaps(ctx, dst, src):
    if dst.type == X.X86_OP_MEM:
        return ['x86::sse::store128(rc::host(%s), %s);' % (ctx.address(dst), xmm(ctx, src))]
    if src.type == X.X86_OP_MEM:
        return ['%s = x86::sse::load128(rc::host(%s));' % (xmm(ctx, dst), ctx.address(src))]
    return ['%s = %s;' % (xmm(ctx, dst), xmm(ctx, src))]


cg_movups = cg_movaps
cg_movntps = cg_movaps


def cg_movss(ctx, dst, src):
    if dst.type == X.X86_OP_MEM:
        return ['rc::stf32(%s, %s.f[0]);' % (ctx.address(dst), xmm(ctx, src))]
    if src.type == X.X86_OP_MEM:
        d = xmm(ctx, dst)
        return ['%s.f[0] = rc::ldf32(%s);' % (d, ctx.address(src)), '%s.u[1] = %s.u[2] = %s.u[3] = 0;' % (d, d, d)]
    return ['%s.f[0] = %s.f[0];' % (xmm(ctx, dst), xmm(ctx, src))]


def cg_movlps(ctx, dst, src):
    if dst.type == X.X86_OP_MEM:
        return ['rc::st64(%s, %s.q[0]);' % (ctx.address(dst), xmm(ctx, src))]
    return ['%s.q[0] = rc::ld64(%s);' % (xmm(ctx, dst), ctx.address(src))]


def cg_movhps(ctx, dst, src):
    if dst.type == X.X86_OP_MEM:
        return ['rc::st64(%s, %s.q[1]);' % (ctx.address(dst), xmm(ctx, src))]
    return ['%s.q[1] = rc::ld64(%s);' % (xmm(ctx, dst), ctx.address(src))]


def cg_movlhps(ctx, dst, src):
    return ['%s.q[1] = %s.q[0];' % (xmm(ctx, dst), xmm(ctx, src))]


def cg_movhlps(ctx, dst, src):
    return ['%s.q[0] = %s.q[1];' % (xmm(ctx, dst), xmm(ctx, src))]


def cg_movntq(ctx, dst, src):
    return ctx.store(64, ctx.address(dst), 'x86::reg64(%s)' % mm(ctx, src))


# ---- arithmetic ---------------------------------------------------------------------------------------------------

def _scalar(function):
    def cg(ctx, dst, src):
        d = xmm(ctx, dst)
        return ['%s.f[0] = x86::sse::%s(%s.f[0], %s);' % (d, function, d, lane(ctx, src, 0))]
    return cg


def _packed(function):
    def cg(ctx, dst, src):
        d = xmm(ctx, dst)
        return block(source128(ctx, src) + ['%s.f[%d] = x86::sse::%s(%s.f[%d], s.f[%d]);' % (d, i, function, d, i, i)
                                            for i in range(4)])
    return cg


cg_addss = _scalar('add')
cg_subss = _scalar('sub')
cg_mulss = _scalar('mul')
cg_divss = _scalar('div')
cg_minss = _scalar('min')
cg_maxss = _scalar('max')
cg_addps = _packed('add')
cg_subps = _packed('sub')
cg_mulps = _packed('mul')
cg_divps = _packed('div')
cg_minps = _packed('min')
cg_maxps = _packed('max')


def cg_sqrtss(ctx, dst, src):
    return ['%s.f[0] = x86::sse::sqrt(%s);' % (xmm(ctx, dst), lane(ctx, src, 0))]


def cg_sqrtps(ctx, dst, src):
    d = xmm(ctx, dst)
    return block(source128(ctx, src) + ['%s.f[%d] = x86::sse::sqrt(s.f[%d]);' % (d, i, i) for i in range(4)])


def _bitwise(expression):
    def cg(ctx, dst, src):
        d = xmm(ctx, dst)
        return block(source128(ctx, src) + ['%s.u[%d] = %s;' % (d, i, expression % {'d': '%s.u[%d]' % (d, i),
                                                                                      's': 's.u[%d]' % i})
                                            for i in range(4)])
    return cg


cg_andps = _bitwise('%(d)s & %(s)s')
cg_andnps = _bitwise('~%(d)s & %(s)s')
cg_orps = _bitwise('%(d)s | %(s)s')
cg_xorps = _bitwise('%(d)s ^ %(s)s')


def _compare_packed(predicate):
    def cg(ctx, dst, src):
        d = xmm(ctx, dst)
        return block(source128(ctx, src) + ['%s.u[%d] = x86::sse::compare(%s.f[%d], s.f[%d], %d);'
                                            % (d, i, d, i, i, predicate) for i in range(4)])
    return cg


def _compare_scalar(predicate):
    def cg(ctx, dst, src):
        d = xmm(ctx, dst)
        return ['%s.u[0] = x86::sse::compare(%s.f[0], %s, %d);' % (d, d, lane(ctx, src, 0), predicate)]
    return cg


for _i, _name in enumerate(('eq', 'lt', 'le', 'unord', 'neq', 'nlt', 'nle', 'ord')):
    globals()['cg_cmp%sps' % _name] = _compare_packed(_i)
    globals()['cg_cmp%sss' % _name] = _compare_scalar(_i)


def _ordered_compare(ctx, dst, src):
    lines = ['const float fa = %s.f[0];' % xmm(ctx, dst), 'const float fb = %s;' % lane(ctx, src, 0)]
    lines += ctx.set_flags({'cf': '!(fa >= fb)', 'zf': '!__builtin_islessgreater(fa, fb)',
                            'pf': '__builtin_isunordered(fa, fb)', 'of': 'false', 'sf': 'false', 'af': 'false'})
    return block(lines)


cg_comiss = _ordered_compare
cg_ucomiss = _ordered_compare


# ---- shuffles -----------------------------------------------------------------------------------------------------

def cg_shufps(ctx, dst, src, imm):
    d = xmm(ctx, dst)
    m = imm.imm
    lines = source128(ctx, src) + ['const x86::Xmm t = %s;' % d]
    lines += ['%s.u[0] = t.u[%d];' % (d, m & 3), '%s.u[1] = t.u[%d];' % (d, (m >> 2) & 3),
              '%s.u[2] = s.u[%d];' % (d, (m >> 4) & 3), '%s.u[3] = s.u[%d];' % (d, (m >> 6) & 3)]
    return block(lines)


def cg_pshufd(ctx, dst, src, imm):
    d = xmm(ctx, dst)
    m = imm.imm
    return block(source128(ctx, src) + ['%s.u[%d] = s.u[%d];' % (d, i, (m >> (2 * i)) & 3) for i in range(4)])


def cg_unpcklps(ctx, dst, src):
    d = xmm(ctx, dst)
    return block(source128(ctx, src) + ['const x86::Xmm t = %s;' % d,
                                        '%s.u[0] = t.u[0];' % d, '%s.u[1] = s.u[0];' % d,
                                        '%s.u[2] = t.u[1];' % d, '%s.u[3] = s.u[1];' % d])


def cg_unpckhps(ctx, dst, src):
    d = xmm(ctx, dst)
    return block(source128(ctx, src) + ['const x86::Xmm t = %s;' % d,
                                        '%s.u[0] = t.u[2];' % d, '%s.u[1] = s.u[2];' % d,
                                        '%s.u[2] = t.u[3];' % d, '%s.u[3] = s.u[3];' % d])


# ---- conversions --------------------------------------------------------------------------------------------------

def cg_cvtsi2ss(ctx, dst, src):
    return ['%s.f[0] = float(x86::sreg32(%s));' % (xmm(ctx, dst), ctx.val(src, 32))]


def cg_cvtss2si(ctx, dst, src):
    return ctx.put(dst, 'x86::sse::toInt(%s)' % lane(ctx, src, 0))


def cg_cvttss2si(ctx, dst, src):
    return ctx.put(dst, 'x86::sse::toIntTruncate(%s)' % lane(ctx, src, 0))


def _pair64(ctx, op):
    """The 64-bit value of an MMX register or memory operand."""
    if op.type == X.X86_OP_REG:
        return 'x86::reg64(%s)' % mm(ctx, op)
    return 'rc::ld64(%s)' % ctx.address(op)


def cg_cvtpi2ps(ctx, dst, src):
    d = xmm(ctx, dst)
    return block(['const x86::reg64 v = %s;' % _pair64(ctx, src),
                  '%s.f[0] = float(x86::sreg32(x86::reg32(v)));' % d,
                  '%s.f[1] = float(x86::sreg32(x86::reg32(v >> 32)));' % d])


def _to_pair(function):
    def cg(ctx, dst, src):
        return block(['const x86::reg64 lo = x86::sse::%s(%s);' % (function, lane(ctx, src, 0)),
                      'const x86::reg64 hi = x86::sse::%s(%s);' % (function, lane(ctx, src, 1)),
                      '%s = x86::from_reg64(lo | hi << 32);' % mm(ctx, dst)])
    return cg


cg_cvtps2pi = _to_pair('toInt')
cg_cvttps2pi = _to_pair('toIntTruncate')
