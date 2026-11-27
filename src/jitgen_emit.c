#ifndef WIDCC_JITGEN_EMIT_C
#define WIDCC_JITGEN_EMIT_C

// Low-level emitters: 64-bit integer emulation on a 32-bit host,
// condition/logical branching, and the call ABI (native registers and the
// Block ABI). Included by jitgen.c after the value-stack helpers and before
// gen_expr, in the original single-file order.
//
// 64-bit integers on a 32-bit host
//
// A 32-bit sljit backend has only 32-bit integer registers, so a 64-bit
// value is represented like an aggregate: the expression result is the
// *address* of an 8-byte scratch slot in the frame. Addition, subtraction,
// bitwise ops and comparisons are open-coded with 32-bit instructions plus
// SLJIT_ADDC/SUBC; multiplication, division, modulo, shifts and float
// conversions call small helper functions (seen by the host C compiler,
// which has native 64-bit arithmetic).
//
// Every helper and state field in this section is prefixed `m32i64` (`m32`
// for the host machine word): on a 64-bit host none of it is generated and
// `is_m32i64_type` is always false, so the names must not be mistaken for
// general 64-bit support.
//

// Materialise the address of a local-area slot into `dst` (virtual
// registers are fine; sljit uses its own internal temp for the base). Only
// used by the 32-bit-host 64-bit emulation below.
static void m32i64_local_addr(JGen *g, sljit_s32 dst, int ofs) {
  JE(g, sljit_get_local_base(g->C, dst, 0, ofs));
}

// Low / high 32-bit word accessors for an 8-byte value.
static void m32i64_ld_lo(JGen *g, sljit_s32 dst, sljit_s32 addr) {
  JE(g, sljit_emit_op1(g->C, SLJIT_MOV32, dst, 0, SLJIT_MEM1(addr), 0));
}
static void m32i64_ld_hi(JGen *g, sljit_s32 dst, sljit_s32 addr) {
  JE(g, sljit_emit_op1(g->C, SLJIT_MOV32, dst, 0, SLJIT_MEM1(addr), 4));
}
static void m32i64_st_lo(JGen *g, int ofs, sljit_s32 src) {
  JE(g, sljit_emit_op1(g->C, SLJIT_MOV32, SLJIT_MEM1(SLJIT_SP), ofs, src, 0));
}
static void m32i64_st_hi(JGen *g, int ofs, sljit_s32 src) {
  JE(g, sljit_emit_op1(g->C, SLJIT_MOV32, SLJIT_MEM1(SLJIT_SP), ofs + 4, src, 0));
}

// Allocate an 8-byte temporary; the returned value is a local offset.
static int m32i64_alloc_temp(JGen *g) {
  if (g->m32i64_top >= g->m32i64_cap)
    error("jitgen: too many 64-bit temporaries in one statement");
  int ofs = g->m32i64_base + g->m32i64_top++ * 8;
  g->m32i64_max = MAX(g->m32i64_max, g->m32i64_top);
  return ofs;
}

// Push the address of the 8-byte temporary at `ofs` as an i64 value.
static void m32i64_push_local(JGen *g, int ofs) {
  int r = sv_new_reg(g, ty_int);
  m32i64_local_addr(g, SLJIT_R(r), ofs);
  sv_push(g, ty_int, SV_REG, r, 0);
}

// Copy 8 bytes from [src] to the stack slot at `ofs`.
static void m32i64_copy_to_local(JGen *g, sljit_s32 src, int ofs) {
  m32i64_ld_lo(g, SLJIT_R2, src);
  m32i64_st_lo(g, ofs, SLJIT_R2);
  m32i64_ld_hi(g, SLJIT_R2, src);
  m32i64_st_hi(g, ofs, SLJIT_R2);
}

// Copy 8 bytes from [src] to [dst]. The copy temp must not alias either
// address register.
static void m32i64_copy8(JGen *g, sljit_s32 src, sljit_s32 dst) {
  sljit_s32 tmp = SLJIT_R2;
  if (tmp == src || tmp == dst)
    tmp = (SLJIT_R1 != src && SLJIT_R1 != dst) ? SLJIT_R1 : SLJIT_R0;
  m32i64_ld_lo(g, tmp, src);
  JE(g, sljit_emit_op1(g->C, SLJIT_MOV32, SLJIT_MEM1(dst), 0, tmp, 0));
  m32i64_ld_hi(g, tmp, src);
  JE(g, sljit_emit_op1(g->C, SLJIT_MOV32, SLJIT_MEM1(dst), 4, tmp, 0));
}

// dst_temp = a + b. `a` is a scratch register holding the lhs address and is
// clobbered; `b` is only read.
static void m32i64_emit_add(JGen *g, sljit_s32 a, sljit_s32 b, int ofs) {
  m32i64_ld_lo(g, SLJIT_R2, a);
  JE(g, sljit_emit_op2(g->C, SLJIT_ADD | SLJIT_32 | SLJIT_SET_CARRY,
                       SLJIT_R2, 0, SLJIT_R2, 0, SLJIT_MEM1(b), 0));
  m32i64_ld_hi(g, a, a);   // flags preserved: a MOV does not touch them
  JE(g, sljit_emit_op2(g->C, SLJIT_ADDC | SLJIT_32,
                       a, 0, a, 0, SLJIT_MEM1(b), 4));
  m32i64_st_lo(g, ofs, SLJIT_R2);
  m32i64_st_hi(g, ofs, a);
}

static void m32i64_emit_sub(JGen *g, sljit_s32 a, sljit_s32 b, int ofs) {
  m32i64_ld_lo(g, SLJIT_R2, a);
  JE(g, sljit_emit_op2(g->C, SLJIT_SUB | SLJIT_32 | SLJIT_SET_CARRY,
                       SLJIT_R2, 0, SLJIT_R2, 0, SLJIT_MEM1(b), 0));
  m32i64_ld_hi(g, a, a);
  JE(g, sljit_emit_op2(g->C, SLJIT_SUBC | SLJIT_32,
                       a, 0, a, 0, SLJIT_MEM1(b), 4));
  m32i64_st_lo(g, ofs, SLJIT_R2);
  m32i64_st_hi(g, ofs, a);
}

static void m32i64_emit_bitop(JGen *g, sljit_s32 op, sljit_s32 a, sljit_s32 b, int ofs) {
  sljit_s32 w = op | SLJIT_32;
  m32i64_ld_lo(g, SLJIT_R2, a);
  JE(g, sljit_emit_op2(g->C, w, SLJIT_R2, 0, SLJIT_R2, 0, SLJIT_MEM1(b), 0));
  m32i64_st_lo(g, ofs, SLJIT_R2);
  m32i64_ld_hi(g, SLJIT_R2, a);
  JE(g, sljit_emit_op2(g->C, w, SLJIT_R2, 0, SLJIT_R2, 0, SLJIT_MEM1(b), 4));
  m32i64_st_hi(g, ofs, SLJIT_R2);
}

static void m32i64_emit_neg(JGen *g, sljit_s32 a, int ofs) {
  JE(g, sljit_emit_op1(g->C, SLJIT_MOV, SLJIT_R2, 0, SLJIT_IMM, 0));
  JE(g, sljit_emit_op2(g->C, SLJIT_SUB | SLJIT_32 | SLJIT_SET_CARRY,
                       SLJIT_R2, 0, SLJIT_R2, 0, SLJIT_MEM1(a), 0));
  JE(g, sljit_emit_op1(g->C, SLJIT_MOV, SLJIT_R1, 0, SLJIT_IMM, 0));
  JE(g, sljit_emit_op2(g->C, SLJIT_SUBC | SLJIT_32,
                       SLJIT_R1, 0, SLJIT_R1, 0, SLJIT_MEM1(a), 4));
  m32i64_st_lo(g, ofs, SLJIT_R2);
  m32i64_st_hi(g, ofs, SLJIT_R1);
}

static void m32i64_emit_bitnot(JGen *g, sljit_s32 a, int ofs) {
  m32i64_ld_lo(g, SLJIT_R2, a);
  JE(g, sljit_emit_op2(g->C, SLJIT_XOR | SLJIT_32, SLJIT_R2, 0, SLJIT_R2, 0, SLJIT_IMM, -1));
  m32i64_st_lo(g, ofs, SLJIT_R2);
  m32i64_ld_hi(g, SLJIT_R2, a);
  JE(g, sljit_emit_op2(g->C, SLJIT_XOR | SLJIT_32, SLJIT_R2, 0, SLJIT_R2, 0, SLJIT_IMM, -1));
  m32i64_st_hi(g, ofs, SLJIT_R2);
}

// Write 0/1 into `dst` depending on whether the 8-byte value at `addr` is
// zero (cond == SLJIT_EQUAL) or non-zero. `addr` must not be R2.
static void m32i64_test_zero(JGen *g, sljit_s32 addr, sljit_s32 dst, sljit_s32 cond) {
  m32i64_ld_lo(g, SLJIT_R2, addr);
  JE(g, sljit_emit_op2(g->C, SLJIT_OR | SLJIT_32,
                       SLJIT_R2, 0, SLJIT_R2, 0, SLJIT_MEM1(addr), 4));
  JE(g, sljit_emit_op2u(g->C, SLJIT_SUB | SLJIT_SET_Z, SLJIT_R2, 0, SLJIT_IMM, 0));
  JE(g, sljit_emit_op_flags(g->C, SLJIT_MOV, dst, 0, cond));
}

// Write 0/1 into `dst` for the 64-bit comparison a < b (with optional
// operand swap and result inversion to cover <=, > and >=).
static void m32i64_emit_less(JGen *g, sljit_s32 dst, sljit_s32 a, sljit_s32 b,
                          bool is_unsigned, bool swapped, bool invert) {
  sljit_s32 x = swapped ? b : a;
  sljit_s32 y = swapped ? a : b;

  if (is_unsigned) {
    // The borrow out of x - y is exactly (x < y) unsigned.
    m32i64_ld_lo(g, SLJIT_R2, x);
    JE(g, sljit_emit_op2(g->C, SLJIT_SUB | SLJIT_32 | SLJIT_SET_CARRY,
                         SLJIT_R2, 0, SLJIT_R2, 0, SLJIT_MEM1(y), 0));
    m32i64_ld_hi(g, x, x);
    JE(g, sljit_emit_op2(g->C, SLJIT_SUBC | SLJIT_32 | SLJIT_SET_CARRY,
                         x, 0, x, 0, SLJIT_MEM1(y), 4));
    JE(g, sljit_emit_op_flags(g->C, SLJIT_MOV, dst, 0,
                              invert ? SLJIT_NOT_CARRY : SLJIT_CARRY));
    return;
  }

  // Signed: the high words decide unless they are equal, in which case the
  // low words are compared as unsigned.
  m32i64_ld_hi(g, SLJIT_R2, x);
  JE(g, sljit_emit_op2u(g->C, SLJIT_SUB | SLJIT_32 | SLJIT_SET_Z,
                        SLJIT_R2, 0, SLJIT_MEM1(y), 4));
  struct sljit_jump *jeq = jchk(sljit_emit_jump(g->C, SLJIT_EQUAL));

  JE(g, sljit_emit_op2u(g->C, SLJIT_SUB | SLJIT_32 | SLJIT_SET(SLJIT_SIG_LESS),
                        SLJIT_R2, 0, SLJIT_MEM1(y), 4));
  JE(g, sljit_emit_op_flags(g->C, SLJIT_MOV, dst, 0,
                            invert ? SLJIT_SIG_GREATER_EQUAL : SLJIT_SIG_LESS));
  struct sljit_jump *jend = jchk(sljit_emit_jump(g->C, SLJIT_JUMP));

  struct sljit_label *leq = lchk(sljit_emit_label(g->C));
  sljit_set_label(jeq, leq);
  m32i64_ld_lo(g, SLJIT_R2, x);
  JE(g, sljit_emit_op2(g->C, SLJIT_SUB | SLJIT_32 | SLJIT_SET_CARRY,
                       SLJIT_R2, 0, SLJIT_R2, 0, SLJIT_MEM1(y), 0));
  JE(g, sljit_emit_op_flags(g->C, SLJIT_MOV, dst, 0,
                            invert ? SLJIT_NOT_CARRY : SLJIT_CARRY));

  struct sljit_label *lend = lchk(sljit_emit_label(g->C));
  sljit_set_label(jend, lend);
}

// Host helper functions for the operations a 32-bit register cannot do.
// They take addresses so the JIT only ever passes plain words.
static void hj_m32i64_mul(int64_t *d, const int64_t *a, const int64_t *b) { *d = *a * *b; }
static void hj_m32u64_mul(uint64_t *d, const uint64_t *a, const uint64_t *b) { *d = *a * *b; }
static void hj_m32i64_div(int64_t *d, const int64_t *a, const int64_t *b) { *d = *b ? *a / *b : 0; }
static void hj_m32u64_div(uint64_t *d, const uint64_t *a, const uint64_t *b) { *d = *b ? *a / *b : 0; }
static void hj_m32i64_mod(int64_t *d, const int64_t *a, const int64_t *b) { *d = *b ? *a % *b : 0; }
static void hj_m32u64_mod(uint64_t *d, const uint64_t *a, const uint64_t *b) { *d = *b ? *a % *b : 0; }
static void hj_m32i64_shl(int64_t *d, const int64_t *a, uint32_t s) {
  *d = (int64_t)((uint64_t)*a << (s & 63));
}
static void hj_m32i64_shr(int64_t *d, const int64_t *a, uint32_t s) {
  *d = (int64_t)((uint64_t)*a >> (s & 63));
}
static void hj_m32i64_sar(int64_t *d, const int64_t *a, uint32_t s) {
  *d = *a >> (s & 63);
}
static double hj_m32i64_to_f64(const int64_t *a) { return (double)*a; }
static double hj_m32u64_to_f64(const uint64_t *a) { return (double)*a; }
static void hj_m32f64_to_i64(int64_t *d, double v) { *d = (int64_t)v; }
static void hj_m32f64_to_u64(uint64_t *d, double v) { *d = (uint64_t)v; }

// Call f(out, a, b): `out` points at the i64 temporary at `ofs`, while `a`
// and `b` hold the addresses of the two source values. The caller must
// have spilled all live values (icall clobbers every scratch register).
static void m32i64_call3(JGen *g, void *fn, int ofs, sljit_s32 a, sljit_s32 b) {
  JE(g, sljit_emit_op1(g->C, SLJIT_MOV, SLJIT_R2, 0, b, 0));
  JE(g, sljit_emit_op1(g->C, SLJIT_MOV, SLJIT_R1, 0, a, 0));
  m32i64_local_addr(g, SLJIT_R0, ofs);
  sljit_s32 at = SLJIT_ARG_RETURN(SLJIT_ARG_TYPE_RET_VOID)
               | SLJIT_ARG_VALUE(SLJIT_ARG_TYPE_W, 1)
               | SLJIT_ARG_VALUE(SLJIT_ARG_TYPE_W, 2)
               | SLJIT_ARG_VALUE(SLJIT_ARG_TYPE_W, 3);
  struct sljit_jump *call = jchk(sljit_emit_call(g->C, SLJIT_CALL, at));
  sljit_set_target(call, (sljit_uw)fn);
}

// Call double f(const int64_t *a); the result arrives in FR0.
// Caller must have spilled all live values.
static void m32i64_call_f64_ret(JGen *g, void *fn, sljit_s32 a) {
  JE(g, sljit_emit_op1(g->C, SLJIT_MOV, SLJIT_R0, 0, a, 0));
  sljit_s32 at = SLJIT_ARG_RETURN(SLJIT_ARG_TYPE_F64)
               | SLJIT_ARG_VALUE(SLJIT_ARG_TYPE_W, 1);
  struct sljit_jump *call = jchk(sljit_emit_call(g->C, SLJIT_CALL, at));
  sljit_set_target(call, (sljit_uw)fn);
}

// Call void f(int64_t *out, double v); the source value must be in FR0.
// Caller must have spilled all live values.
static void m32i64_call_from_f64(JGen *g, void *fn, int ofs) {
  m32i64_local_addr(g, SLJIT_R0, ofs);
  sljit_s32 at = SLJIT_ARG_RETURN(SLJIT_ARG_TYPE_RET_VOID)
               | SLJIT_ARG_VALUE(SLJIT_ARG_TYPE_W, 1)
               | SLJIT_ARG_VALUE(SLJIT_ARG_TYPE_F64, 2);
  struct sljit_jump *call = jchk(sljit_emit_call(g->C, SLJIT_CALL, at));
  sljit_set_target(call, (sljit_uw)fn);
}

// Evaluate an i64 binary arithmetic / bitwise node into a fresh temporary.
static void gen_m32i64_binop(JGen *g, Node *node) {
  int mark = g->m32i64_top;
  int ofs = m32i64_alloc_temp(g);
  gen_expr(g, node->lhs);
  gen_expr(g, node->rhs);
  int li = g->vtop - 2, ri = g->vtop - 1;

  bool u = node->ty->is_unsigned;
  switch (node->kind) {
  case ND_ADD:
  case ND_SUB:
  case ND_BITAND:
  case ND_BITOR:
  case ND_BITXOR:
    // Open-coded: only R0..R2 and the operand registers are clobbered.
    sv_to_conv2(g, li, 0, ri, 1);
    sv_pop(g);
    sv_pop(g);
    switch (node->kind) {
    case ND_ADD: m32i64_emit_add(g, SLJIT_R0, SLJIT_R1, ofs); break;
    case ND_SUB: m32i64_emit_sub(g, SLJIT_R0, SLJIT_R1, ofs); break;
    case ND_BITAND: m32i64_emit_bitop(g, SLJIT_AND, SLJIT_R0, SLJIT_R1, ofs); break;
    case ND_BITOR:  m32i64_emit_bitop(g, SLJIT_OR,  SLJIT_R0, SLJIT_R1, ofs); break;
    default:        m32i64_emit_bitop(g, SLJIT_XOR, SLJIT_R0, SLJIT_R1, ofs); break;
    }
    break;
  case ND_MUL:
  case ND_DIV:
  case ND_MOD: {
    void *fn;
    if (node->kind == ND_MUL)
      fn = u ? (void *)hj_m32u64_mul : (void *)hj_m32i64_mul;
    else if (node->kind == ND_DIV)
      fn = u ? (void *)hj_m32u64_div : (void *)hj_m32i64_div;
    else
      fn = u ? (void *)hj_m32u64_mod : (void *)hj_m32i64_mod;
    sv_spill_all(g);
    sv_force(g, li, 0);
    sv_force(g, ri, 1);
    sv_pop(g);
    sv_pop(g);
    m32i64_call3(g, fn, ofs, SLJIT_R0, SLJIT_R1);
    break;
  }
  default:
    error_tok(node->tok, "jitgen: unsupported 64-bit operator");
  }
  g->m32i64_top = mark + 1;
  m32i64_push_local(g, ofs);
}

// Evaluate an i64 shift. The shift count is an ordinary word, not an i64.
static void gen_m32i64_shift(JGen *g, Node *node) {
  int mark = g->m32i64_top;
  int ofs = m32i64_alloc_temp(g);
  gen_expr(g, node->lhs);
  gen_expr(g, node->rhs);
  int li = g->vtop - 2, ri = g->vtop - 1;
  void *fn = node->kind == ND_SHL ? (void *)hj_m32i64_shl
           : node->kind == ND_SHR ? (void *)hj_m32i64_shr
                                  : (void *)hj_m32i64_sar;
  sv_spill_all(g);
  sv_force(g, li, 0);
  sv_force(g, ri, 1);
  sv_pop(g);
  sv_pop(g);
  m32i64_call3(g, fn, ofs, SLJIT_R0, SLJIT_R1);
  g->m32i64_top = mark + 1;
  m32i64_push_local(g, ofs);
}

// Evaluate an i64 comparison; the 0/1 result is an ordinary int value.
static void gen_m32i64_cmp(JGen *g, Node *node) {
  int mark = g->m32i64_top;
  gen_expr(g, node->lhs);
  gen_expr(g, node->rhs);
  int li = g->vtop - 2, ri = g->vtop - 1;
  sv_to_conv2(g, li, 0, ri, 1);
  sv_pop(g);
  sv_pop(g);
  g->m32i64_top = mark;           // operands consumed

  int dst = sv_new_reg(g, node->ty);
  if (node->kind == ND_EQ || node->kind == ND_NE) {
    m32i64_ld_lo(g, SLJIT_R2, SLJIT_R0);
    JE(g, sljit_emit_op2(g->C, SLJIT_XOR | SLJIT_32,
                         SLJIT_R2, 0, SLJIT_R2, 0, SLJIT_MEM1(SLJIT_R1), 0));
    m32i64_ld_hi(g, SLJIT_R0, SLJIT_R0);
    JE(g, sljit_emit_op2(g->C, SLJIT_XOR | SLJIT_32,
                         SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_MEM1(SLJIT_R1), 4));
    JE(g, sljit_emit_op2(g->C, SLJIT_OR | SLJIT_32,
                         SLJIT_R2, 0, SLJIT_R2, 0, SLJIT_R0, 0));
    JE(g, sljit_emit_op2u(g->C, SLJIT_SUB | SLJIT_SET_Z, SLJIT_R2, 0, SLJIT_IMM, 0));
    JE(g, sljit_emit_op_flags(g->C, SLJIT_MOV, SLJIT_R(dst), 0,
                              node->kind == ND_EQ ? SLJIT_EQUAL : SLJIT_NOT_EQUAL));
  } else {
    bool u = node->lhs->ty->is_unsigned;
    bool swapped = (node->kind == ND_GT || node->kind == ND_LE);
    bool invert = (node->kind == ND_GE || node->kind == ND_LE);
    m32i64_emit_less(g, SLJIT_R(dst), SLJIT_R0, SLJIT_R1, u, swapped, invert);
  }
  sv_push(g, node->ty, SV_REG, dst, 0);
}

// Convert between a 64-bit integer and another scalar type.
static void gen_m32i64_cast(JGen *g, Node *node) {
  Type *from = node->lhs->ty;
  Type *to = node->ty;
  if (is_m32i64_type(from) && is_m32i64_type(to)) {
    // Same 8-byte representation; just produce the operand's address.
    gen_expr(g, node->lhs);
    return;
  }

  int mark = g->m32i64_top;
  gen_expr(g, node->lhs);
  int idx = g->vtop - 1;

  if (is_m32i64_type(from)) {
    if (to->kind == TY_VOID) {
      sv_pop(g);
      g->m32i64_top = mark;
      return;
    }
    if (is_flonum(to)) {
      void *fn = from->is_unsigned ? (void *)hj_m32u64_to_f64 : (void *)hj_m32i64_to_f64;
      sv_spill_all(g);
      sv_force(g, idx, 0);
      sv_pop(g);
      m32i64_call_f64_ret(g, fn, SLJIT_R0);
      if (to->kind == TY_FLOAT)
        JE(g, sljit_emit_fop1(g->C, SLJIT_CONV_F32_FROM_F64,
                              SLJIT_FR0, 0, SLJIT_FR0, 0));
      g->m32i64_top = mark;
      sv_push(g, to, SV_REG, 0, 0);
      return;
    }
    sv_to_conv(g, idx, 0);
    sv_pop(g);
    sljit_s32 op = to->kind == TY_BOOL ? SLJIT_MOV_U8 : int_load_op(to);
    JE(g, sljit_emit_op1(g->C, op, SLJIT_R0, 0, SLJIT_MEM1(SLJIT_R0), 0));
    if (to->kind == TY_BOOL)
      int_to_bool(g);
    g->m32i64_top = mark;
    sv_push(g, to, SV_REG, 0, 0);
    return;
  }

  if (is_flonum(from)) {
    sv_spill_all(g);
    sv_force(g, idx, 0);
    sv_pop(g);
    int ofs = m32i64_alloc_temp(g);
    if (from->kind == TY_FLOAT)
      JE(g, sljit_emit_fop1(g->C, SLJIT_CONV_F64_FROM_F32,
                            SLJIT_FR0, 0, SLJIT_FR0, 0));
    void *fn = to->is_unsigned ? (void *)hj_m32f64_to_u64 : (void *)hj_m32f64_to_i64;
    m32i64_call_from_f64(g, fn, ofs);
    g->m32i64_top = mark + 1;
    m32i64_push_local(g, ofs);
    return;
  }

  // Narrow integer / pointer / bool -> 64-bit.
  sv_clear_scratch(g, idx, -1);
  int r = sv_gv(g, idx);
  sv_pop(g);
  int ofs = m32i64_alloc_temp(g);
  m32i64_st_lo(g, ofs, SLJIT_R(r));
  if (from->is_unsigned || from->kind == TY_BOOL || from->kind == TY_PTR) {
    JE(g, sljit_emit_op1(g->C, SLJIT_MOV,
                         SLJIT_MEM1(SLJIT_SP), ofs + 4, SLJIT_IMM, 0));
  } else {
    JE(g, sljit_emit_op2(g->C, SLJIT_ASHR | SLJIT_32,
                         SLJIT_R2, 0, SLJIT_R(r), 0, SLJIT_IMM, 31));
    m32i64_st_hi(g, ofs, SLJIT_R2);
  }
  g->m32i64_top = mark + 1;
  m32i64_push_local(g, ofs);
}

// Evaluate a condition and leave 0/1 in R0 (handles float and i64
// conditions). The value is consumed; R0 is left unowned.
static void gen_cond_truth(JGen *g, Node *cond) {
  int mark = g->m32i64_top;
  gen_expr(g, cond);
  int idx = g->vtop - 1;
  if (is_m32i64_type(cond->ty)) {
    sv_to_conv(g, idx, 0);
    sv_pop(g);
    m32i64_test_zero(g, SLJIT_R0, SLJIT_R0, SLJIT_NOT_EQUAL);
  } else if (is_flonum(cond->ty)) {
    sv_clear_scratch(g, idx, -1);
    sv_force(g, idx, 0);
    load_dbl_zero(g, SLJIT_FR1);
    sv_pop(g);
    sljit_s32 op = cond->ty->kind == TY_FLOAT ? SLJIT_CMP_F32 : SLJIT_CMP_F64;
    JE(g, sljit_emit_fop1(g->C, op | SLJIT_SET_UNORDERED_OR_NOT_EQUAL,
                          SLJIT_FR0, 0, SLJIT_FR1, 0));
    JE(g, sljit_emit_op_flags(g->C, SLJIT_MOV, SLJIT_R0, 0,
                              SLJIT_UNORDERED_OR_NOT_EQUAL));
  } else {
    sv_force(g, idx, 0);
    sv_pop(g);
  }
  g->m32i64_top = mark;
}

// Inverse of an integer jump condition (jump-if-X -> jump-if-not-X).
static sljit_s32 invert_int_cond(sljit_s32 c) {
  switch (c) {
  case SLJIT_EQUAL: return SLJIT_NOT_EQUAL;
  case SLJIT_NOT_EQUAL: return SLJIT_EQUAL;
  case SLJIT_LESS: return SLJIT_GREATER_EQUAL;
  case SLJIT_GREATER_EQUAL: return SLJIT_LESS;
  case SLJIT_GREATER: return SLJIT_LESS_EQUAL;
  case SLJIT_LESS_EQUAL: return SLJIT_GREATER;
  case SLJIT_SIG_LESS: return SLJIT_SIG_GREATER_EQUAL;
  case SLJIT_SIG_GREATER_EQUAL: return SLJIT_SIG_LESS;
  case SLJIT_SIG_GREATER: return SLJIT_SIG_LESS_EQUAL;
  case SLJIT_SIG_LESS_EQUAL: return SLJIT_SIG_GREATER;
  default: return SLJIT_EQUAL;
  }
}

// Inverse of a floating-point jump condition.
static sljit_s32 invert_flt_cond(sljit_s32 c) {
  switch (c) {
  case SLJIT_ORDERED_EQUAL: return SLJIT_UNORDERED_OR_NOT_EQUAL;
  case SLJIT_UNORDERED_OR_NOT_EQUAL: return SLJIT_ORDERED_EQUAL;
  case SLJIT_ORDERED_LESS: return SLJIT_UNORDERED_OR_GREATER_EQUAL;
  case SLJIT_UNORDERED_OR_GREATER_EQUAL: return SLJIT_ORDERED_LESS;
  case SLJIT_ORDERED_LESS_EQUAL: return SLJIT_UNORDERED_OR_GREATER;
  case SLJIT_UNORDERED_OR_GREATER: return SLJIT_ORDERED_LESS_EQUAL;
  case SLJIT_ORDERED_GREATER: return SLJIT_UNORDERED_OR_LESS_EQUAL;
  case SLJIT_UNORDERED_OR_LESS_EQUAL: return SLJIT_ORDERED_GREATER;
  case SLJIT_ORDERED_GREATER_EQUAL: return SLJIT_UNORDERED_OR_LESS;
  case SLJIT_UNORDERED_OR_LESS: return SLJIT_ORDERED_GREATER_EQUAL;
  default: return SLJIT_UNORDERED_OR_NOT_EQUAL;
  }
}

// Emit a jump taken when `cond` evaluates to `jump_on_true`, consuming the
// condition's operands. Integer and float comparisons branch on their own
// flags instead of materializing a 0/1 value first.
static struct sljit_jump *gen_branch(JGen *g, Node *cond, bool jump_on_true) {
  if (cond->kind == ND_NOT)
    return gen_branch(g, cond->lhs, !jump_on_true);
  // The parser coerces conditions to _Bool; branching on the operand's own
  // flags is equivalent to branching on `operand != 0`.
  if (cond->kind == ND_CAST && cond->ty->kind == TY_BOOL)
    return gen_branch(g, cond->lhs, jump_on_true);

  bool is_cmp = cond->kind >= ND_EQ && cond->kind <= ND_GE;
  if (is_cmp && !is_m32i64_type(cond->lhs->ty)) {
    Type *ty = cond->lhs->ty;
    gen_expr(g, cond->lhs);
    gen_expr(g, cond->rhs);
    int li = g->vtop - 2, ri = g->vtop - 1;
    SValue *lv = &g->vstack[li], *rv = &g->vstack[ri];

    if (is_flonum(ty)) {
      sljit_s32 c = flt_cond(cond->kind);
      if (!jump_on_true)
        c = invert_flt_cond(c);
      if (ty->kind == TY_FLOAT)
        c |= SLJIT_32;
      sv_gv2(g, li, ri);
      struct sljit_jump *j = jchk(sljit_emit_fcmp(g->C, c, SLJIT_FR(lv->r), 0,
                                                  SLJIT_FR(rv->r), 0));
      sv_pop(g);
      sv_pop(g);
      return j;
    }

    bool u = ty->is_unsigned || ty->kind == TY_PTR;
    sljit_s32 c = int_cond(cond->kind, u);
    if (!jump_on_true)
      c = invert_int_cond(c);
    if (ty->size == 4)
      c |= SLJIT_32;
    struct sljit_jump *j;
    if (rv->loc == SV_IMM) {
      if (lv->loc == SV_SREG)
        j = jchk(sljit_emit_cmp(g->C, c, SLJIT_S(lv->r), 0,
                                SLJIT_IMM, rv->imm));
      else {
        int rl = sv_gv(g, li);
        j = jchk(sljit_emit_cmp(g->C, c, SLJIT_R(rl), 0, SLJIT_IMM, rv->imm));
      }
    } else {
      sljit_s32 lop;
      if (lv->loc == SV_SREG)
        lop = SLJIT_S(lv->r);
      else if (lv->loc == SV_REG)
        lop = SLJIT_R(lv->r);
      else
        lop = SLJIT_R(sv_gv_keep(g, li, ri, -1));
      if (rv->loc == SV_MEM)
        j = jchk(sljit_emit_cmp(g->C, c, lop, 0, SLJIT_MEM1(SLJIT_SP),
                                sv_home(g, ri)));
      else if (rv->loc == SV_SREG)
        j = jchk(sljit_emit_cmp(g->C, c, lop, 0, SLJIT_S(rv->r), 0));
      else
        j = jchk(sljit_emit_cmp(g->C, c, lop, 0, SLJIT_R(rv->r), 0));
    }
    sv_pop(g);
    sv_pop(g);
    return j;
  }

  gen_cond_truth(g, cond);
  return jchk(sljit_emit_cmp(g->C,
                             jump_on_true ? SLJIT_NOT_EQUAL : SLJIT_EQUAL,
                             SLJIT_R0, 0, SLJIT_IMM, 0));
}

// i64 ? : with both branches yielding 64-bit values.
static void gen_m32i64_cond(JGen *g, Node *node) {
  gen_cond_truth(g, node->cond);
  struct sljit_jump *jels = jchk(sljit_emit_cmp(g->C, SLJIT_EQUAL,
                                                   SLJIT_R0, 0, SLJIT_IMM, 0));
  sv_spill_all(g);
  int ofs = m32i64_alloc_temp(g);        // shared result slot
  int branch_mark = g->m32i64_top;

  gen_expr(g, node->then);
  sv_to_conv(g, g->vtop - 1, 0);
  sv_pop(g);
  m32i64_copy_to_local(g, SLJIT_R0, ofs);
  struct sljit_jump *jend = jchk(sljit_emit_jump(g->C, SLJIT_JUMP));

  struct sljit_label *lels = lchk(sljit_emit_label(g->C));
  sljit_set_label(jels, lels);
  g->m32i64_top = branch_mark;
  gen_expr(g, node->els);
  sv_to_conv(g, g->vtop - 1, 0);
  sv_pop(g);
  m32i64_copy_to_local(g, SLJIT_R0, ofs);
  struct sljit_label *lend = lchk(sljit_emit_label(g->C));
  sljit_set_label(jend, lend);

  g->m32i64_top = branch_mark;
  m32i64_push_local(g, ofs);
}

// Short-circuit `&&` / `||` with an int result. Handles i64 operands via
// gen_cond_truth. All enclosing values are spilled first so both the
// short-circuit and fall-through paths reach the join in the same state.
static void gen_logical(JGen *g, Node *node) {
  int mark = g->m32i64_top;
  bool is_and = node->kind == ND_LOGAND;
  bool stop_when = !is_and; // && stops on false, || stops on true

  sv_spill_all(g);
  struct sljit_jump *j1 = gen_branch(g, node->lhs, stop_when);
  struct sljit_jump *j2 = gen_branch(g, node->rhs, stop_when);

  int dst = sv_new_reg(g, node->ty);
  JE(g, sljit_emit_op1(g->C, SLJIT_MOV, SLJIT_R(dst), 0, SLJIT_IMM, is_and ? 1 : 0));
  struct sljit_jump *jend = jchk(sljit_emit_jump(g->C, SLJIT_JUMP));

  struct sljit_label *lstop = lchk(sljit_emit_label(g->C));
  sljit_set_label(j1, lstop);
  sljit_set_label(j2, lstop);
  JE(g, sljit_emit_op1(g->C, SLJIT_MOV, SLJIT_R(dst), 0, SLJIT_IMM, is_and ? 0 : 1));

  struct sljit_label *lend = lchk(sljit_emit_label(g->C));
  sljit_set_label(jend, lend);
  g->m32i64_top = mark;
  sv_push(g, node->ty, SV_REG, dst, 0);
}

//
// Function calls
//

static sljit_s32 arg_type_of(Type *ty) {
  switch (ty->kind) {
  case TY_FLOAT: return SLJIT_ARG_TYPE_F32;
  case TY_DOUBLE: return SLJIT_ARG_TYPE_F64;
  default: return SLJIT_ARG_TYPE_W;
  }
}

// Code address of a callee that can be called directly, or NULL when the
// function is not resolved yet and must be reached through an indirection
// cell (forward-defined functions get their address patched in later).
static void *direct_callee_target(JTU *tu, Node *lhs) {
  Obj *var = lhs->var;
  if (var->jit_addr)
    return var->jit_addr;
  if (var->is_static)
    return NULL;
  WidccRT *rt = tu->rt;
  JitSym *s = jit_sym(rt, var->name, true);
  if (s->addr)
    return s->addr;
  void *host = jit_resolve_host(rt, var->name);
  if (host)
    s->addr = host;
  return host;
}

// Arguments of one register class (integers or floats) land in R0../FR0..
// in declaration order. A register-held argument may itself sit in another
// argument's destination register, so the moves are scheduled: one is
// emitted only when no pending move still reads its destination; a cycle
// parks one source in its home slot.
static void schedule_arg_moves(JGen *g, int base, Obj *args, bool flt) {
  int n = 0, slot_of[JMAX_ARGS];
  {
    int slot = base + 1;
    for (Obj *a = args; a; a = a->param_next, slot++)
      if (is_flonum(a->arg_expr->ty) == flt)
        slot_of[n++] = slot;
  }
  bool done[JMAX_ARGS] = {false};
  for (int left = n; left > 0;) {
    int ran = 0;
    for (int i = 0; i < n; i++) {
      if (done[i])
        continue;
      SValue *v = &g->vstack[slot_of[i]];
      // The move into dst i may only run once no pending move still reads
      // that destination register.
      bool busy = false;
      for (int j = 0; j < n; j++) {
        if (j == i || done[j])
          continue;
        SValue *w = &g->vstack[slot_of[j]];
        if (w->loc == SV_REG && w->r == i) {
          busy = true;
          break;
        }
      }
      if (busy)
        continue;
      if (flt) {
        if (v->loc == SV_REG) {
          if (v->r != i)
            JE(g, sljit_emit_fop1(g->C, SLJIT_MOV_F64, SLJIT_FR(i), 0,
                                  SLJIT_FR(v->r), 0));
        } else if (v->loc == SV_IMM) {
          if (v->ty->kind == TY_FLOAT)
            JE(g, sljit_emit_fset32(g->C, SLJIT_FR(i), (sljit_f32)v->imm));
          else
            JE(g, sljit_emit_fset64(g->C, SLJIT_FR(i), (sljit_f64)v->imm));
        } else {
          // Home slots hold F64-spilled values; the low 32 bits are the
          // float value, so F64 moves serve F32 arguments too.
          JE(g, sljit_emit_fop1(g->C, SLJIT_MOV_F64, SLJIT_FR(i), 0,
                                SLJIT_MEM1(SLJIT_SP), sv_home(g, slot_of[i])));
        }
      } else if (v->loc == SV_IMM) {
        JE(g, sljit_emit_op1(g->C, SLJIT_MOV, SLJIT_R(i), 0,
                             SLJIT_IMM, v->imm));
      } else if (v->loc == SV_REG) {
        if (v->r != i)
          JE(g, sljit_emit_op1(g->C, SLJIT_MOV, SLJIT_R(i), 0,
                               SLJIT_R(v->r), 0));
      } else if (v->loc == SV_SREG) {
        JE(g, sljit_emit_op1(g->C, SLJIT_MOV, SLJIT_R(i), 0,
                             SLJIT_S(v->r), 0));
      } else {
        JE(g, sljit_emit_op1(g->C, SLJIT_MOV, SLJIT_R(i), 0,
                             SLJIT_MEM1(SLJIT_SP), sv_home(g, slot_of[i])));
      }
      done[i] = true;
      left--;
      ran++;
    }
    if (!ran) {
      for (int i = 0; i < n; i++) {
        SValue *v = &g->vstack[slot_of[i]];
        if (!done[i] && v->loc == SV_REG) {
          sv_spill(g, slot_of[i]);
          break;
        }
      }
    }
  }
}

// Lowest scratch register of the given class that no live value holds, or
// -1 when all of them are busy.
static int pick_scratch(JGen *g, bool flt) {
  int n = flt ? g->scratch_flt : g->scratch_int;
  for (int r = 0; r < n; r++)
    if (!reg_in_use(g, r, flt))
      return r;
  return -1;
}

// Fill the caller-side argument block of a Block ABI call. `base` is the
// value-stack index of the callee, so the arguments are base+1 .. base+narg;
// argument j goes to byte offset j*8. Each slot holds one machine word
// (float: 4 bytes, double: 8 bytes).
//
// sljit has no memory-to-memory move, so arguments living in their home
// slot need a scratch register. Register-held arguments are stored first,
// which releases their registers for that purpose.
static void build_arg_block(JGen *g, int base, int narg) {
  // Pass 1: arguments already in a register, saved register or immediate.
  for (int j = 0; j < narg; j++) {
    SValue *v = &g->vstack[base + 1 + j];
    if (v->loc == SV_MEM)
      continue;
    sljit_sw dst = g->argblock_base + j * 8;
    if (sv_flt(v)) {
      sljit_s32 op = v->ty->kind == TY_FLOAT ? SLJIT_MOV_F32 : SLJIT_MOV_F64;
      if (v->loc == SV_REG) {
        JE(g, sljit_emit_fop1(g->C, op, SLJIT_MEM1(SLJIT_SP), dst,
                              SLJIT_FR(v->r), 0));
      } else if (v->loc == SV_SREG) {
        JE(g, sljit_emit_fop1(g->C, op, SLJIT_MEM1(SLJIT_SP), dst,
                              SLJIT_FS(v->r), 0));
      } else {
        // Floats are always materialized, so SV_IMM is not expected here.
        JE(g, sljit_emit_fset64(g->C, SLJIT_FR0, (sljit_f64)v->imm));
        JE(g, sljit_emit_fop1(g->C, op, SLJIT_MEM1(SLJIT_SP), dst,
                              SLJIT_FR0, 0));
      }
    } else if (v->loc == SV_REG) {
      JE(g, sljit_emit_op1(g->C, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP), dst,
                           SLJIT_R(v->r), 0));
    } else if (v->loc == SV_SREG) {
      JE(g, sljit_emit_op1(g->C, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP), dst,
                           SLJIT_S(v->r), 0));
    } else { // SV_IMM
      JE(g, sljit_emit_op1(g->C, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP), dst,
                           SLJIT_IMM, v->imm));
    }
    v->loc = SV_MEM; // consumed: its register is now free
  }

  // Pass 2: arguments living in their home slot (slot >= 0).
  for (int j = 0; j < narg; j++) {
    int vi = base + 1 + j;
    SValue *v = &g->vstack[vi];
    if (v->loc != SV_MEM || v->slot < 0)
      continue;
    sljit_sw dst = g->argblock_base + j * 8;
    if (sv_flt(v)) {
      if (v->ty->kind == TY_FLOAT) {
        // 4-byte raw copy; no float scratch needed.
        int r = pick_scratch(g, false);
        if (r < 0)
          error("jitgen: out of scratch registers filling argument block");
        JE(g, sljit_emit_op1(g->C, SLJIT_MOV32, SLJIT_R(r), 0,
                             SLJIT_MEM1(SLJIT_SP), sv_home(g, vi)));
        JE(g, sljit_emit_op1(g->C, SLJIT_MOV32, SLJIT_MEM1(SLJIT_SP), dst,
                             SLJIT_R(r), 0));
      } else {
        // 8-byte copy: a word move is only 4 bytes on a 32-bit host.
        int fr = pick_scratch(g, true);
        if (fr < 0)
          error("jitgen: out of scratch float registers filling argument block");
        JE(g, sljit_emit_fop1(g->C, SLJIT_MOV_F64, SLJIT_FR(fr), 0,
                              SLJIT_MEM1(SLJIT_SP), sv_home(g, vi)));
        JE(g, sljit_emit_fop1(g->C, SLJIT_MOV_F64, SLJIT_MEM1(SLJIT_SP), dst,
                              SLJIT_FR(fr), 0));
      }
    } else {
      int r = pick_scratch(g, false);
      if (r < 0)
        error("jitgen: out of scratch integer registers filling argument block");
      JE(g, sljit_emit_op1(g->C, SLJIT_MOV, SLJIT_R(r), 0,
                           SLJIT_MEM1(SLJIT_SP), sv_home(g, vi)));
      JE(g, sljit_emit_op1(g->C, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP), dst,
                           SLJIT_R(r), 0));
    }
  }
}

static void gen_funcall(JGen *g, Node *node) {
  Type *fty = node->lhs->ty;
  if (fty->kind == TY_PTR)
    fty = fty->base;
  if (fty->kind != TY_FUNC)
    error_tok(node->tok, "callee is not a function");
  if (node->lhs->kind == ND_VAR && !strcmp(node->lhs->var->name, "alloca"))
    unsupported(node, "alloca");
  if (fty->is_oldstyle)
    unsupported(node, "calls to unprototyped functions");
  if (node->ret_buffer)
    unsupported(node, "struct/union return values");
  if (is_m32i64_type(node->ty))
    unsupported(node, "64-bit integer return values");

  int nint = 0, nflt = 0, narg = 0;
  for (Obj *a = node->args; a; a = a->param_next) {
    Type *t = a->arg_expr->ty;
    if (is_m32i64_type(t))
      unsupported(node, "64-bit integer arguments");
    if (t->kind == TY_STRUCT || t->kind == TY_UNION)
      unsupported(node, "struct/union arguments passed by value");
    if (t->kind == TY_LDOUBLE)
      unsupported(node, "long double");
    if (is_flonum(t))
      nflt++;
    else
      nint++;
    narg++;
  }
  // Variadic functions always use the Block ABI: their callee cannot know
  // the argument count otherwise. Otherwise only more than JMAX_ARGS
  // arguments force it; four arguments or fewer keep the native C ABI.
  bool block = fty->is_variadic || narg > JMAX_ARGS;
  if (!block && (nint > JMAX_INT_ARGS || nflt > JMAX_FLT_ARGS))
    error_tok(node->tok, "too many arguments in JIT-compiled call");

  int base = g->vtop;

  // A callee whose address is known at code generation time is called
  // directly. A function defined in this TU but not compiled yet (forward
  // reference or self-recursion) gets a rewritable call patched at end of
  // TU; only symbols outside the TU need the indirection cell.
  void *target = NULL;
  Obj *patch_target = NULL;
  if (node->lhs->kind == ND_VAR && node->lhs->var->ty->kind == TY_FUNC) {
    Obj *var = node->lhs->var;
    if (!var->jit_addr && var->is_definition)
      patch_target = var;
    else
      target = direct_callee_target(g->tu, node->lhs);
  }

  if (target || patch_target) {
    sv_push(g, node->lhs->ty, SV_IMM, 0, (sljit_sw)target);
  } else {
    gen_expr(g, node->lhs);
    // Park the indirect callee in its home slot right away: the argument
    // expressions then allocate the low argument registers, and the call
    // itself reads the target from memory.
    sv_spill(g, base);
  }
  for (Obj *a = node->args; a; a = a->param_next)
    gen_expr(g, a->arg_expr);

  // The call clobbers every scratch register: values that stay live across
  // it must sit in their home slots. Callee and argument slots are consumed
  // by the call itself, so they are not spilled here.
  sv_spill_below(g, base);

  // Callee address → its home slot (or read from its saved register), before
  // any argument move can clobber the register holding it.
  SValue *callee = &g->vstack[base];
  sljit_s32 call_src = SLJIT_MEM1(SLJIT_SP);
  sljit_sw call_srcw = 0;
  if (!target && !patch_target) {
    if (callee->loc == SV_SREG) {
      call_src = SLJIT_S(callee->r);
    } else if (!block && callee->loc == SV_REG && callee->r >= nint) {
      // The high scratch register is never an argument destination.
      call_src = SLJIT_R(callee->r);
    } else {
      // For a Block ABI call R0 carries the argument-block pointer, so a
      // register-held callee must be parked in memory first.
      spill_slot(g, callee);
      if (callee->loc == SV_REG) {
        JE(g, sljit_emit_op1(g->C, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP),
                             sv_home(g, base), SLJIT_R(callee->r), 0));
      } else if (callee->loc == SV_IMM) {
        JE(g, sljit_emit_op1(g->C, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP),
                             sv_home(g, base), SLJIT_IMM, callee->imm));
      }
      call_srcw = sv_home(g, base);
    }
  }

  sljit_s32 at;
  if (block) {
    // Block ABI: fill the caller-side argument block, then pass its address
    // in R0 and the total argument count in R1. sljit sees a two-argument
    // function, so its four-argument limit never applies.
    if (narg > 0)
      build_arg_block(g, base, narg);
    g->argblock_max = MAX(g->argblock_max, narg * 8);
    at = SLJIT_ARG_RETURN(arg_type_of(node->ty));
    if (node->ty->kind == TY_VOID)
      at = SLJIT_ARG_RETURN(SLJIT_ARG_TYPE_RET_VOID);
    at |= SLJIT_ARG_VALUE(SLJIT_ARG_TYPE_P, 1);
    at |= SLJIT_ARG_VALUE(SLJIT_ARG_TYPE_W, 2);
    JE(g, sljit_get_local_base(g->C, SLJIT_R0, 0, g->argblock_base));
    JE(g, sljit_emit_op1(g->C, SLJIT_MOV, SLJIT_R1, 0, SLJIT_IMM,
                         (sljit_sw)narg));
  } else {
    at = SLJIT_ARG_RETURN(arg_type_of(node->ty));
    if (node->ty->kind == TY_VOID)
      at = SLJIT_ARG_RETURN(SLJIT_ARG_TYPE_RET_VOID);
    {
      int pos = 1;
      for (Obj *a = node->args; a; a = a->param_next, pos++)
        at |= SLJIT_ARG_VALUE(arg_type_of(a->arg_expr->ty), pos);
    }
    schedule_arg_moves(g, base, node->args, true);
    schedule_arg_moves(g, base, node->args, false);
  }

  if (target) {
    struct sljit_jump *call = jchk(sljit_emit_call(g->C, SLJIT_CALL, at));
    sljit_set_target(call, (sljit_uw)target);
  } else if (patch_target) {
    // Placeholder target; the imm slot of the call is rewritten once the
    // callee has been compiled.
    struct sljit_jump *call =
      jchk(sljit_emit_call(g->C, SLJIT_CALL | SLJIT_REWRITABLE_JUMP, at));
    sljit_set_target(call, 0);
    JCallJump *cj = cc_calloc(1, sizeof(JCallJump));
    cj->jump = call;
    cj->target = patch_target;
    cj->next = g->call_jumps;
    g->call_jumps = cj;
  } else {
    JE(g, sljit_emit_icall(g->C, SLJIT_CALL, at, call_src, call_srcw));
  }

  for (int i = 0; i < narg + 1; i++)
    sv_pop(g);

  // Canonicalize a narrow integer return value.
  Type *rt = node->ty;
  if (rt->kind == TY_VOID)
    return;
  if (is_integer(rt) && rt->size < 8)
    JE(g, sljit_emit_op1(g->C, int_load_op(rt), SLJIT_R0, 0, SLJIT_R0, 0));
  sv_push(g, rt, SV_REG, 0, 0);
}


#endif // WIDCC_JITGEN_EMIT_C
