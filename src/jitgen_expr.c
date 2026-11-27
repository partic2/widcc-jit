#ifndef WIDCC_JITGEN_EXPR_C
#define WIDCC_JITGEN_EXPR_C

// Expression code generation: constant folding, scaled addressing, the
// va_list forms on the Block ABI, and the gen_expr dispatcher. Included by
// jitgen.c.
//
// Expression code generation
//

// log2 of a positive power of two, or -1.
static int pow2_log(sljit_sw v) {
  if (v <= 0 || ((sljit_uw)v & ((sljit_uw)v - 1)) != 0)
    return -1;
  int k = 0;
  while (((sljit_uw)v >> k) != 1)
    k++;
  return k;
}

// Apply an integer type conversion to a compile-time constant.
static sljit_sw conv_const(sljit_sw v, Type *ty) {
  if (ty->kind == TY_BOOL)
    return v != 0;
  switch (ty->size) {
  case 1: return ty->is_unsigned ? (sljit_sw)(sljit_u8)v : (sljit_sw)(sljit_s8)v;
  case 2: return ty->is_unsigned ? (sljit_sw)(sljit_u16)v : (sljit_sw)(sljit_s16)v;
  case 4: return ty->is_unsigned ? (sljit_sw)(sljit_u32)v : (sljit_sw)(sljit_s32)v;
  default: return v;
  }
}

// Sign-extend from the low `w` bits.
static int64_t sext_bits(sljit_sw v, int w) {
  if (w >= 64)
    return (int64_t)v;
  uint64_t m = (uint64_t)1 << (w - 1);
  return (int64_t)(((uint64_t)v & (m * 2 - 1)) ^ m) - (int64_t)m;
}

// Fold a literal-only integer expression into a single constant, so the
// value can stay an immediate operand.
static bool fold_int_const(Node *n, sljit_sw *out) {
  if (n->kind == ND_NUM) {
    if (is_flonum(n->ty))
      return false; // fval, not val, holds the literal
    *out = (sljit_sw)n->val;
    return true;
  }
  if (!n->ty || is_flonum(n->ty) || n->ty->kind == TY_VOID)
    return false;

  switch (n->kind) {
  case ND_CAST: {
    sljit_sw v;
    if (!fold_int_const(n->lhs, &v))
      return false;
    *out = conv_const(v, n->ty);
    return true;
  }
  case ND_NEG: {
    sljit_sw v;
    if (!fold_int_const(n->lhs, &v))
      return false;
    *out = conv_const(-(uint64_t)v, n->ty);
    return true;
  }
  case ND_BITNOT: {
    sljit_sw v;
    if (!fold_int_const(n->lhs, &v))
      return false;
    *out = conv_const(~(uint64_t)v, n->ty);
    return true;
  }
  case ND_ADD: case ND_SUB: case ND_MUL: case ND_DIV: case ND_MOD:
  case ND_SHL: case ND_SHR: case ND_SAR:
  case ND_BITAND: case ND_BITOR: case ND_BITXOR:
    break;
  default:
    return false;
  }

  sljit_sw a, b;
  if (!fold_int_const(n->lhs, &a) || !fold_int_const(n->rhs, &b))
    return false;
  int w = n->ty->size * 8;
  uint64_t r;
  switch (n->kind) {
  case ND_ADD: r = (uint64_t)a + (uint64_t)b; break;
  case ND_SUB: r = (uint64_t)a - (uint64_t)b; break;
  case ND_MUL: r = (uint64_t)a * (uint64_t)b; break;
  case ND_DIV: case ND_MOD: {
    uint64_t ub = conv_const(b, n->ty);
    if (ub == 0)
      return false;
    if (n->ty->is_unsigned) {
      uint64_t ua = conv_const(a, n->ty);
      r = n->kind == ND_DIV ? ua / ub : ua % ub;
    } else {
      int64_t sa = sext_bits(conv_const(a, n->ty), w);
      int64_t sb = (int64_t)ub;
      if (sa == INT64_MIN && sb == -1)
        return false; // the hardware would trap; C says undefined
      r = n->kind == ND_DIV ? (uint64_t)(sa / sb) : (uint64_t)(sa % sb);
    }
    break;
  }
  case ND_SHL: case ND_SHR: case ND_SAR:
    if (b < 0 || b >= w)
      return false; // undefined behaviour; leave it to the generic path
    if (n->kind == ND_SHL) {
      r = (uint64_t)a << b;
    } else {
      uint64_t ua = conv_const(a, n->ty);
      if (n->kind == ND_SHR)
        r = ua >> b;
      else
        r = (uint64_t)(sext_bits((sljit_sw)ua, w) >> b);
    }
    break;
  case ND_BITAND: r = (uint64_t)a & (uint64_t)b; break;
  case ND_BITOR: r = (uint64_t)a | (uint64_t)b; break;
  default: r = (uint64_t)a ^ (uint64_t)b; break;
  }
  *out = conv_const((sljit_sw)r, n->ty);
  return true;
}

// Strength-reduce `v * c`, `v / c`, `v % c` by a power-of-two constant (and
// the trivial 0/1 constants) into shifts, masks and adds. `r` holds the
// variable operand and receives the result in place; `keep` is its value
// stack index, which must not be evicted. Returns false when the generic
// instruction should be used instead.
static bool emit_const_muldiv(JGen *g, Node *node, Type *ty, bool is32,
                              int keep, int r, sljit_sw c) {
  // Signed 32-bit operands are canonical in word-wide registers, so these
  // shift sequences can run word-wide and keep the canonical form.
  sljit_s32 op32 = is32 && ty->is_unsigned ? SLJIT_32 : 0;
  int k = pow2_log(c);
  int w = is32 ? 32 : 64;
  if (k >= w)
    return false; // cannot encode this shift count in the operand width

  switch (node->kind) {
  case ND_MUL:
    if (c == 0) {
      JE(g, sljit_emit_op1(g->C, SLJIT_MOV, SLJIT_R(r), 0, SLJIT_IMM, 0));
      return true;
    }
    if (c == 1)
      return true;
    if (k < 1)
      return false;
    JE(g, sljit_emit_op2(g->C, SLJIT_SHL | op32, SLJIT_R(r), 0,
                         SLJIT_R(r), 0, SLJIT_IMM, k));
    return true;
  case ND_DIV:
    if (c == 1)
      return true;
    if (k < 1)
      return false;
    if (ty->is_unsigned || node->ptr_diff_div) {
      // An unsigned dividend shifts logically. A pointer difference is exact
      // by construction, so its signed quotient needs no rounding correction
      // either: a plain arithmetic shift gives the truncated result.
      JE(g, sljit_emit_op2(g->C, (ty->is_unsigned ? SLJIT_LSHR : SLJIT_ASHR) | op32,
                           SLJIT_R(r), 0, SLJIT_R(r), 0, SLJIT_IMM, k));
      return true;
    }
    // Signed division truncates toward zero: add (v < 0 ? c-1 : 0)
    // before the arithmetic shift.
    sv_make_free(g, false, 1, keep, -1);
    {
      int t = sv_pick_free(g, false);
      JE(g, sljit_emit_op2(g->C, SLJIT_ASHR | op32, SLJIT_R(t), 0,
                           SLJIT_R(r), 0, SLJIT_IMM, w - 1));
      JE(g, sljit_emit_op2(g->C, SLJIT_AND | op32, SLJIT_R(t), 0,
                           SLJIT_R(t), 0, SLJIT_IMM, c - 1));
      JE(g, sljit_emit_op2(g->C, SLJIT_ADD | op32, SLJIT_R(r), 0,
                           SLJIT_R(r), 0, SLJIT_R(t), 0));
      JE(g, sljit_emit_op2(g->C, SLJIT_ASHR | op32, SLJIT_R(r), 0,
                           SLJIT_R(r), 0, SLJIT_IMM, k));
    }
    return true;
  case ND_MOD:
    if (c == 1 || c == -1) {
      JE(g, sljit_emit_op1(g->C, SLJIT_MOV, SLJIT_R(r), 0, SLJIT_IMM, 0));
      return true;
    }
    if (k < 1)
      return false;
    if (ty->is_unsigned) {
      JE(g, sljit_emit_op2(g->C, SLJIT_AND | op32, SLJIT_R(r), 0,
                           SLJIT_R(r), 0, SLJIT_IMM, c - 1));
      return true;
    }
    // Signed remainder: r = ((v + bias) & (c-1)) - bias, where bias is
    // c-1 for negative v and 0 otherwise.
    sv_make_free(g, false, 1, keep, -1);
    {
      int t = sv_pick_free(g, false);
      JE(g, sljit_emit_op2(g->C, SLJIT_ASHR | op32, SLJIT_R(t), 0,
                           SLJIT_R(r), 0, SLJIT_IMM, w - 1));
      JE(g, sljit_emit_op2(g->C, SLJIT_AND | op32, SLJIT_R(t), 0,
                           SLJIT_R(t), 0, SLJIT_IMM, c - 1));
      JE(g, sljit_emit_op2(g->C, SLJIT_ADD | op32, SLJIT_R(r), 0,
                           SLJIT_R(r), 0, SLJIT_R(t), 0));
      JE(g, sljit_emit_op2(g->C, SLJIT_AND | op32, SLJIT_R(r), 0,
                           SLJIT_R(r), 0, SLJIT_IMM, c - 1));
      JE(g, sljit_emit_op2(g->C, SLJIT_SUB | op32, SLJIT_R(r), 0,
                           SLJIT_R(r), 0, SLJIT_R(t), 0));
    }
    return true;
  default:
    return false;
  }
}

// Casts between equally wide non-float types leave the bits unchanged
// (add_type wraps pointer arithmetic in int<->pointer and signedness
// conversions); look through them when pattern matching.
static Node *skip_same_size_casts(Node *n) {
  while (n && n->kind == ND_CAST && n->lhs && n->lhs->ty && n->ty &&
         !is_flonum(n->ty) && !is_flonum(n->lhs->ty) &&
         n->ty->size == n->lhs->ty->size)
    n = n->lhs;
  return n;
}

// Match the product the parser inserts for pointer arithmetic: one
// operand folds to a power-of-two element size 1, 2, 4 or 8.
static bool match_scale_mul(Node *mul, Node **idx, int *shift) {
  if (mul->kind != ND_MUL || !mul->ty)
    return false;
  sljit_sw c;
  if (fold_int_const(mul->rhs, &c)) {
    int k = pow2_log(c);
    if (k >= 0 && k <= 3) {
      *idx = mul->lhs;
      *shift = k;
      return true;
    }
  }
  if (fold_int_const(mul->lhs, &c)) {
    int k = pow2_log(c);
    if (k >= 0 && k <= 3) {
      *idx = mul->rhs;
      *shift = k;
      return true;
    }
  }
  return false;
}

// Match `*(base + idx * 2^k)`.
static bool match_scaled_deref(Node *node, Node **base, Node **idx,
                               int *shift) {
  if (node->kind != ND_DEREF || !node->lhs || !node->ty || !node->lhs->ty)
    return false;
  Node *add = node->lhs;
  if (add->kind != ND_ADD || !add->ty->base) // pointer or array decay
    return false;
  if (!match_scale_mul(skip_same_size_casts(add->rhs), idx, shift))
    return false;
  *base = add->lhs;
  return true;
}

// Evaluate the base and index operands of a scaled dereference and push
// them as (base, index) SValues; *shift receives log2 of the element size.
static bool gen_scaled_operands(JGen *g, Node *deref, int *shift) {
  Node *base, *idx;
  int k;
  if (!match_scaled_deref(deref, &base, &idx, &k))
    return false;
  gen_expr(g, base);
  gen_expr(g, idx);
  *shift = k;
  return true;
}

// Build the memory operand for a scaled dereference. The base and index
// SValues are left on the stack for the caller to pop after the access.
static bool scaled_mem_operand(JGen *g, Node *deref, sljit_s32 *mem,
                               sljit_sw *offw) {
  int k;
  if (!gen_scaled_operands(g, deref, &k))
    return false;
  int bi = g->vtop - 2, xi = g->vtop - 1;
  SValue *xv = &g->vstack[xi];
  sljit_s32 bop = sv_base_reg(g, bi, xi);
  if (xv->loc == SV_IMM) {
    *mem = SLJIT_MEM1(bop);
    *offw = (sljit_sw)((sljit_uw)xv->imm << k);
  } else {
    sljit_s32 xop = sv_base_reg(g, xi, bi);
    *mem = SLJIT_MEM2(bop, xop);
    *offw = k;
  }
  return true;
}

// A scaled pointer add `p + i * 2^k` becomes one LEA. Returns true when
// the node was handled (the result SValue is pushed).
static bool gen_scaled_add(JGen *g, Node *node) {
  Node *base, *idx;
  int k;
  if (node->kind != ND_ADD || !node->ty || !node->ty->base)
    return false;
  if (!match_scale_mul(skip_same_size_casts(node->rhs), &idx, &k))
    return false;
  base = node->lhs;

  gen_expr(g, base);
  gen_expr(g, idx);
  int bi = g->vtop - 2, xi = g->vtop - 1;
  SValue *bv = &g->vstack[bi], *xv = &g->vstack[xi];

  if (bv->loc == SV_IMM) {
    int dst = sv_new_reg(g, node->ty);
    sv_force(g, bi, dst);
    g->vstack[bi].ty = node->ty;
    if (xv->loc == SV_IMM) {
      sljit_sw off = (sljit_sw)((sljit_uw)xv->imm << k);
      JE(g, sljit_emit_op2(g->C, SLJIT_ADD, SLJIT_R(dst), 0,
                           SLJIT_R(dst), 0, SLJIT_IMM, off));
    } else {
      sljit_s32 xop = sv_base_reg(g, xi, bi);
      JE(g, sljit_emit_op2_shift(g->C, SLJIT_ADD | SLJIT_SHL_IMM,
                                 SLJIT_R(dst), 0, SLJIT_R(dst), 0,
                                 xop, 0, k));
    }
    sv_pop(g);
    return true;
  }

  // A variable living in a saved register must not be overwritten, so the
  // sum goes to a fresh scratch register instead of the base operand.
  if (bv->loc == SV_SREG) {
    sljit_s32 bop = SLJIT_S(bv->r);
    sljit_s32 xop = 0;
    sljit_sw off = 0;
    bool ximm = xv->loc == SV_IMM;
    if (ximm)
      off = (sljit_sw)((sljit_uw)xv->imm << k);
    else
      xop = sv_base_reg(g, xi, bi);
    sv_make_free(g, false, 1, bi, xi);
    int dst = sv_pick_free(g, false);
    if (ximm)
      JE(g, sljit_emit_op2(g->C, SLJIT_ADD, SLJIT_R(dst), 0, bop, 0,
                           SLJIT_IMM, off));
    else
      JE(g, sljit_emit_op2_shift(g->C, SLJIT_ADD | SLJIT_SHL_IMM,
                                 SLJIT_R(dst), 0, bop, 0, xop, 0, k));
    sv_pop(g);
    sv_pop(g);
    sv_push(g, node->ty, SV_REG, dst, 0);
    return true;
  }

  int br = sv_gv_real_keep(g, bi, xi);
  if (xv->loc == SV_IMM) {
    sljit_sw off = (sljit_sw)((sljit_uw)xv->imm << k);
    JE(g, sljit_emit_op2(g->C, SLJIT_ADD, SLJIT_R(br), 0,
                         SLJIT_R(br), 0, SLJIT_IMM, off));
  } else {
    sljit_s32 xop = sv_base_reg(g, xi, bi);
    JE(g, sljit_emit_op2_shift(g->C, SLJIT_ADD | SLJIT_SHL_IMM,
                               SLJIT_R(br), 0, SLJIT_R(br), 0,
                               xop, 0, k));
  }
  sv_pop(g);
  sv_pop(g);
  sv_push(g, node->ty, SV_REG, br, 0);
  return true;
}

// ------------------------------------------------------------------
// va_list on the Block ABI
//
// Variadic functions are compiled with the Block ABI: the entry receives a
// pointer to a flat array of 8-byte slots, in declaration order, plus the
// total argument count. va_list is therefore just a cursor into that array
// (see __builtin_va_list in jit.c).
// va_start points it at the first slot past the named parameters, and each
// va_arg reads one slot and advances by the fixed 8-byte stride. There is no
// register-save area and no gp/fp offset bookkeeping: sljit never has to
// spill the incoming argument registers.
// ------------------------------------------------------------------

// Number of slots occupied by the named parameters of the current function.
static int va_named_slots(JGen *g) {
  int n = 0;
  for (Obj *p = g->fn->ty->param_list; p; p = p->param_next)
    n++;
  return n;
}

// __builtin_va_start(ap, last): *ap = block + named_slots * 8.
static void gen_va_start(JGen *g, Node *node) {
  if (g->va_block_ofs < 0)
    unsupported(node, "va_start outside a variadic function");

  gen_expr(g, node->lhs); // address of the va_list object
  int ai = g->vtop - 1;
  sv_clear_scratch(g, ai, -1);
  sv_force(g, ai, 0); // R0 = &ap

  JE(g, sljit_emit_op1(g->C, SLJIT_MOV, SLJIT_R1, 0,
                       SLJIT_MEM1(SLJIT_SP), g->va_block_ofs));
  int named = va_named_slots(g);
  if (named)
    JE(g, sljit_emit_op2(g->C, SLJIT_ADD, SLJIT_R1, 0, SLJIT_R1, 0,
                         SLJIT_IMM, (sljit_sw)named * 8));
  JE(g, sljit_emit_op1(g->C, SLJIT_MOV, SLJIT_MEM1(SLJIT_R0), 0,
                       SLJIT_R1, 0));
  sv_pop(g); // the expression is void
}

// __builtin_va_arg(ap, T): load one slot, store it into the parser's
// temporary, and leave the value on the stack for the enclosing ND_CHAIN.
static void gen_va_arg(JGen *g, Node *node) {
  Type *ty = node->ty;
  if (!ty)
    error("jitgen: internal error: va_arg without a type");
  if (is_m32i64_type(ty))
    unsupported(node, "64-bit integers in va_arg");
  if (ty->kind == TY_STRUCT || ty->kind == TY_UNION)
    unsupported(node, "struct/union values in va_arg");
  if (ty->kind == TY_LDOUBLE)
    unsupported(node, "long double");
  if (ty->size > 8)
    unsupported(node, "va_arg of a type larger than 8 bytes");
  if (node->var->jreg > 0)
    error("jitgen: internal error: register-allocated va_arg temporary");

  gen_expr(g, node->lhs); // address of the va_list object
  int ai = g->vtop - 1;
  sv_clear_scratch(g, ai, -1);
  sv_force(g, ai, 0); // R0 = &ap

  // R1 = current slot, then *ap = current + 8 (uniform slot stride).
  JE(g, sljit_emit_op1(g->C, SLJIT_MOV, SLJIT_R1, 0, SLJIT_MEM1(SLJIT_R0), 0));
  JE(g, sljit_emit_op2(g->C, SLJIT_ADD, SLJIT_R2, 0, SLJIT_R1, 0,
                       SLJIT_IMM, 8));
  JE(g, sljit_emit_op1(g->C, SLJIT_MOV, SLJIT_MEM1(SLJIT_R0), 0,
                       SLJIT_R2, 0));

  Obj *var = node->var;
  if (is_flonum(ty)) {
    // Variadic float arguments are promoted to double by the frontend, so the
    // slot holds a double even when the requested type is float.
    JE(g, sljit_emit_fop1(g->C, SLJIT_MOV_F64, SLJIT_FR0, 0,
                          SLJIT_MEM1(SLJIT_R1), 0));
    if (ty->kind == TY_FLOAT)
      JE(g, sljit_emit_fop1(g->C, SLJIT_CONV_F32_FROM_F64, SLJIT_FR0, 0,
                            SLJIT_FR0, 0));
    JE(g, sljit_emit_fop1(g->C, ty->kind == TY_FLOAT ? SLJIT_MOV_F32
                                                     : SLJIT_MOV_F64,
                          SLJIT_MEM1(SLJIT_SP), var->ofs, SLJIT_FR0, 0));
    sv_pop(g);
    sv_push(g, ty, SV_REG, 0, 0);
    return;
  }

  JE(g, sljit_emit_op1(g->C, int_load_op(ty), SLJIT_R0, 0,
                       SLJIT_MEM1(SLJIT_R1), 0));
  JE(g, sljit_emit_op1(g->C, int_store_op(ty), SLJIT_MEM1(SLJIT_SP),
                       var->ofs, SLJIT_R0, 0));
  sv_pop(g);
  sv_push(g, ty, SV_REG, 0, 0);
}

// __builtin_va_copy(dst, src): both are cursors, so copy one word.
static void gen_va_copy(JGen *g, Node *node) {
  gen_expr(g, node->lhs); // &dst
  gen_expr(g, node->rhs); // &src
  int si = g->vtop - 2, di = g->vtop - 1;
  sv_clear_scratch(g, si, di);
  sv_force(g, si, 0); // R0 = &dst
  sv_force(g, di, 1); // R1 = &src
  JE(g, sljit_emit_op1(g->C, SLJIT_MOV, SLJIT_R2, 0, SLJIT_MEM1(SLJIT_R1), 0));
  JE(g, sljit_emit_op1(g->C, SLJIT_MOV, SLJIT_MEM1(SLJIT_R0), 0,
                       SLJIT_R2, 0));
  sv_pop(g);
  sv_pop(g);
}

static void gen_expr(JGen *g, Node *node) {
  switch (node->kind) {
  case ND_ADD: case ND_SUB: case ND_MUL: case ND_DIV: case ND_MOD:
  case ND_SHL: case ND_SHR: case ND_SAR:
  case ND_BITAND: case ND_BITOR: case ND_BITXOR:
  case ND_NEG: case ND_BITNOT: {
    // A literal-only subtree (1 << 2, 3 * 4, ...) folds to one immediate.
    sljit_sw cv;
    if (!is_m32i64_type(node->ty) && !is_flonum(node->ty) &&
        node->ty->kind != TY_VOID && fold_int_const(node, &cv)) {
      sv_push(g, node->ty, SV_IMM, 0, cv);
      return;
    }
    break;
  }
  default:
    break;
  }

  switch (node->kind) {
  case ND_NULL_EXPR:
    return;
  case ND_NUM: {
    Type *ty = node->ty;
    if (is_m32i64_type(ty)) {
      int ofs = m32i64_alloc_temp(g);
      uint64_t v = (uint64_t)node->val;
      JE(g, sljit_emit_op1(g->C, SLJIT_MOV32,
                           SLJIT_MEM1(SLJIT_SP), ofs, SLJIT_IMM, (uint32_t)v));
      JE(g, sljit_emit_op1(g->C, SLJIT_MOV32,
                           SLJIT_MEM1(SLJIT_SP), ofs + 4,
                           SLJIT_IMM, (uint32_t)(v >> 32)));
      m32i64_push_local(g, ofs);
      return;
    }
    if (ty->kind == TY_FLOAT || ty->kind == TY_DOUBLE) {
      int fd = sv_new_reg(g, ty);
      if (ty->kind == TY_FLOAT)
        JE(g, sljit_emit_fset32(g->C, SLJIT_FR(fd), (sljit_f32)node->fval));
      else
        JE(g, sljit_emit_fset64(g->C, SLJIT_FR(fd), (sljit_f64)node->fval));
      sv_push(g, ty, SV_REG, fd, 0);
      return;
    }
    // Integer literals stay compile-time constants until needed.
    sv_push(g, ty, SV_IMM, 0, (sljit_sw)node->val);
    return;
  }
  case ND_VAR: {
    Obj *var = node->var;
    if (var->jreg > 0) {
      if (!is_flonum(var->ty)) {
        // The saved register is the variable's storage; consumers read it
        // in place and assignment copies live reads out first.
        sv_push(g, var->ty, SV_SREG, var->jreg - 1, 0);
        return;
      }
      int r = sv_new_reg(g, var->ty);
      sljit_s32 op = var->ty->kind == TY_FLOAT ? SLJIT_MOV_F32 : SLJIT_MOV_F64;
      JE(g, sljit_emit_fop1(g->C, op, SLJIT_FR(r), 0, SLJIT_FS(var->jreg - 1), 0));
      sv_push(g, var->ty, SV_REG, r, 0);
      return;
    }
    gen_load_lvalue(g, node);
    return;
  }
  case ND_MEMBER:
  case ND_DEREF:
    gen_load_lvalue(g, node);
    return;
  case ND_ADDR:
    gen_addr(g, node->lhs);
    g->vstack[g->vtop - 1].ty = node->ty;
    return;
  case ND_POS:
    gen_expr(g, node->lhs);
    return;
  case ND_NEG:
    if (is_m32i64_type(node->ty)) {
      int mark = g->m32i64_top;
      int ofs = m32i64_alloc_temp(g);
      gen_expr(g, node->lhs);
      sv_to_conv(g, g->vtop - 1, 0);
      sv_pop(g);
      m32i64_emit_neg(g, SLJIT_R0, ofs);
      g->m32i64_top = mark + 1;
      m32i64_push_local(g, ofs);
      return;
    }
    gen_expr(g, node->lhs);
    if (is_flonum(node->ty)) {
      int r = sv_gv(g, g->vtop - 1);
      JE(g, sljit_emit_fop1(g->C, node->ty->kind == TY_FLOAT ? SLJIT_NEG_F32 : SLJIT_NEG_F64,
                            SLJIT_FR(r), 0, SLJIT_FR(r), 0));
      return;
    }
    {
      int r = sv_gv(g, g->vtop - 1);
      // Negation of a canonical signed value stays canonical word-wide.
      bool widen = node->ty->size == 4 && !node->ty->is_unsigned;
      sljit_s32 op = node->ty->size == 4 && !widen ? SLJIT_SUB32 : SLJIT_SUB;
      JE(g, sljit_emit_op2(g->C, op, SLJIT_R(r), 0, SLJIT_IMM, 0, SLJIT_R(r), 0));
      if (node->ty->size == 4 && !widen)
        extend32_to(g, node->ty, SLJIT_R(r));
    }
    return;
  case ND_NOT:
    if (is_m32i64_type(node->lhs->ty)) {
      int mark = g->m32i64_top;
      gen_expr(g, node->lhs);
      sv_to_conv(g, g->vtop - 1, 0);
      sv_pop(g);
      m32i64_test_zero(g, SLJIT_R0, SLJIT_R0, SLJIT_EQUAL);
      g->m32i64_top = mark;
      sv_push(g, node->ty, SV_REG, 0, 0);
      return;
    }
    gen_expr(g, node->lhs);
    if (is_flonum(node->lhs->ty)) {
      int idx = g->vtop - 1;
      sv_clear_scratch(g, idx, -1);
      sv_force(g, idx, 0);
      load_dbl_zero(g, SLJIT_FR1);
      sv_pop(g);
      sljit_s32 op = node->lhs->ty->kind == TY_FLOAT ? SLJIT_CMP_F32 : SLJIT_CMP_F64;
      JE(g, sljit_emit_fop1(g->C, op | SLJIT_SET_ORDERED_EQUAL,
                            SLJIT_FR0, 0, SLJIT_FR1, 0));
      JE(g, sljit_emit_op_flags(g->C, SLJIT_MOV, SLJIT_R0, 0, SLJIT_ORDERED_EQUAL));
      sv_push(g, node->ty, SV_REG, 0, 0);
      return;
    }
    {
      int r = sv_gv(g, g->vtop - 1);
      JE(g, sljit_emit_op2u(g->C, SLJIT_SUB | SLJIT_SET_Z, SLJIT_R(r), 0, SLJIT_IMM, 0));
      JE(g, sljit_emit_op_flags(g->C, SLJIT_MOV, SLJIT_R(r), 0, SLJIT_EQUAL));
      g->vstack[g->vtop - 1].ty = node->ty;
    }
    return;
  case ND_BITNOT:
    if (is_m32i64_type(node->ty)) {
      int mark = g->m32i64_top;
      int ofs = m32i64_alloc_temp(g);
      gen_expr(g, node->lhs);
      sv_to_conv(g, g->vtop - 1, 0);
      sv_pop(g);
      m32i64_emit_bitnot(g, SLJIT_R0, ofs);
      g->m32i64_top = mark + 1;
      m32i64_push_local(g, ofs);
      return;
    }
    gen_expr(g, node->lhs);
    {
      int r = sv_gv(g, g->vtop - 1);
      bool widen = node->ty->size == 4 && !node->ty->is_unsigned;
      sljit_s32 op = node->ty->size == 4 && !widen ? SLJIT_XOR32 : SLJIT_XOR;
      JE(g, sljit_emit_op2(g->C, op, SLJIT_R(r), 0, SLJIT_R(r), 0, SLJIT_IMM, -1));
      if (node->ty->size == 4 && !widen)
        extend32_to(g, node->ty, SLJIT_R(r));
    }
    return;
  case ND_LOGAND:
  case ND_LOGOR:
    gen_logical(g, node);
    return;
  case ND_COND: {
    if (is_m32i64_type(node->ty)) {
      gen_m32i64_cond(g, node);
      return;
    }
    bool is_void = node->ty->kind == TY_VOID;
    struct sljit_jump *jels = gen_branch(g, node->cond, false);
    // Both arms deliver their result in the same register, so the join is
    // register-based and needs no memory round trip.
    sv_spill_all(g);
    gen_expr(g, node->then);
    if (!is_void) {
      sv_to_conv(g, g->vtop - 1, 0);
      sv_pop(g);
    }
    struct sljit_jump *jend = jchk(sljit_emit_jump(g->C, SLJIT_JUMP));
    struct sljit_label *lels = lchk(sljit_emit_label(g->C));
    sljit_set_label(jels, lels);
    gen_expr(g, node->els);
    if (!is_void) {
      sv_to_conv(g, g->vtop - 1, 0);
      sv_pop(g);
      sv_push(g, node->ty, SV_REG, 0, 0);
    }
    struct sljit_label *lend = lchk(sljit_emit_label(g->C));
    sljit_set_label(jend, lend);
    return;
  }
  case ND_COMMA:
  case ND_CHAIN:
    gen_expr(g, node->lhs);
    if (node->lhs->ty && node->lhs->ty->kind != TY_VOID)
      sv_pop(g);
    gen_expr(g, node->rhs);
    return;
  case ND_CAST: {
    sljit_sw cv;
    if (!is_m32i64_type(node->ty) && node->ty->kind != TY_VOID &&
        !is_flonum(node->ty) && fold_int_const(node, &cv)) {
      // A converted literal needs no code; keep it an immediate.
      sv_push(g, node->ty, SV_IMM, 0, cv);
      return;
    }
    if (is_m32i64_type(node->lhs->ty) || is_m32i64_type(node->ty)) {
      gen_m32i64_cast(g, node);
      return;
    }
    gen_expr(g, node->lhs);
    if (node->ty->kind == TY_VOID) {
      if (node->lhs->ty && node->lhs->ty->kind != TY_VOID)
        sv_pop(g);
      return;
    }
    if (node->lhs->ty && node->lhs->ty->kind != TY_VOID) {
      if (cast_is_noop(node->lhs->ty, node->ty)) {
        g->vstack[g->vtop - 1].ty = node->ty;
        return;
      }
      sv_to_conv(g, g->vtop - 1, 0);
      sv_pop(g);
    }
    cast_to(g, node->lhs->ty, node->ty);
    sv_push(g, node->ty, SV_REG, 0, 0);
    return;
  }
  case ND_ASSIGN: {
    if (node->lhs->kind == ND_MEMBER && node->lhs->member->is_bitfield)
      unsupported(node, "bitfields");
    if (is_m32i64_type(node->lhs->ty)) {
      // Park the rhs value in a scratch slot first: computing the destination
      // address may itself evaluate expressions.
      int mark = g->m32i64_top;
      int tmp = m32i64_alloc_temp(g);
      gen_expr(g, node->rhs);
      sv_to_conv(g, g->vtop - 1, 0);
      sv_pop(g);
      m32i64_copy_to_local(g, SLJIT_R0, tmp);
      gen_addr(g, node->lhs);
      int ai = g->vtop - 1;
      sv_clear_scratch(g, ai, -1);
      sv_force(g, ai, 2);
      m32i64_local_addr(g, SLJIT_R1, tmp);
      m32i64_copy8(g, SLJIT_R1, SLJIT_R2);
      g->vstack[ai].ty = node->ty; // the assignment's value is the lhs object
      g->m32i64_top = mark;
      return;
    }
    Obj *lv = node->lhs->kind == ND_VAR ? node->lhs->var : NULL;
    if (lv && lv->jreg > 0) {
      // `x = x + k` where x lives in a saved register updates it in place.
      // Signed overflow is undefined, so a word-wide add/sub is exact for
      // signed 32-bit variables; unsigned ones need the 32-bit wrap.
      Node *upd = node->rhs;
      if (upd->kind == ND_CAST && upd->lhs && upd->lhs->ty &&
          cast_is_noop(upd->lhs->ty, upd->ty) &&
          upd->ty->size == lv->ty->size &&
          upd->ty->is_unsigned == lv->ty->is_unsigned)
        upd = upd->lhs;
      // The usual arithmetic conversions wrap both operands in casts.
      Node *self = NULL;
      if (upd->kind == ND_ADD || upd->kind == ND_SUB) {
        self = upd->lhs;
        if (self->kind == ND_CAST && self->lhs && self->lhs->ty &&
            cast_is_noop(self->lhs->ty, self->ty))
          self = self->lhs;
      }
      if (!is_flonum(lv->ty) && !is_m32i64_type(lv->ty) &&
          (upd->kind == ND_ADD || upd->kind == ND_SUB) &&
          self && self->kind == ND_VAR && self->var == lv) {
        // Unsigned 32-bit variables must wrap like the generic path, and a
        // 32-bit sljit op leaves the canonical zero-extended form in place.
        sljit_s32 wop = (upd->kind == ND_ADD ? SLJIT_ADD : SLJIT_SUB) |
                        (lv->ty->size == 4 && lv->ty->is_unsigned ? SLJIT_32 : 0);
        sljit_sw k;
        if (fold_int_const(upd->rhs, &k)) {
          sv_inval_sreg(g, lv->jreg - 1);
          JE(g, sljit_emit_op2(g->C, wop, SLJIT_S(lv->jreg - 1), 0,
                               SLJIT_S(lv->jreg - 1), 0, SLJIT_IMM, k));
          sv_push(g, node->ty, SV_SREG, lv->jreg - 1, 0);
          return;
        }
        // `x = x op y`: evaluate y and fold it into the saved register.
        gen_expr(g, upd->rhs);
        int ri = g->vtop - 1;
        SValue *rv = &g->vstack[ri];
        sljit_s32 o2;
        sljit_sw o2w = 0;
        if (rv->loc == SV_SREG)
          o2 = SLJIT_S(rv->r);
        else if (rv->loc == SV_REG)
          o2 = SLJIT_R(rv->r);
        else {
          o2 = SLJIT_MEM1(SLJIT_SP);
          o2w = sv_home(g, ri);
        }
        sv_inval_sreg(g, lv->jreg - 1);
        JE(g, sljit_emit_op2(g->C, wop, SLJIT_S(lv->jreg - 1), 0,
                             SLJIT_S(lv->jreg - 1), 0, o2, o2w));
        sv_pop(g);
        sv_push(g, node->ty, SV_SREG, lv->jreg - 1, 0);
        return;
      }
      gen_expr(g, node->rhs);
      Type *rt = g->vstack[g->vtop - 1].ty;
      int r = sv_gv(g, g->vtop - 1);
      if (is_flonum(lv->ty)) {
        JE(g, sljit_emit_fop1(g->C, lv->ty->kind == TY_FLOAT ? SLJIT_MOV_F32
                                                             : SLJIT_MOV_F64,
                              SLJIT_FS(lv->jreg - 1), 0, SLJIT_FR(r), 0));
      } else {
        if (lv->ty->size < 8 && !same_canon_repr(rt, lv->ty))
          JE(g, sljit_emit_op1(g->C, int_load_op(lv->ty), SLJIT_R(r), 0,
                               SLJIT_R(r), 0));
        sv_inval_sreg(g, lv->jreg - 1);
        JE(g, sljit_emit_op1(g->C, SLJIT_MOV, SLJIT_S(lv->jreg - 1), 0,
                             SLJIT_R(r), 0));
      }
      return;
    }
    // Evaluate rhs first, then the destination address; the rhs value
    // stays on the stack as the assignment's result.
    gen_expr(g, node->rhs);
    // A scalar destination held directly in the local area is stored
    // straight to its frame slot; no address register is needed.
    if (!is_aggregate_ty(node->lhs->ty) && !is_m32i64_type(node->lhs->ty)) {
      int lofs = local_lvalue_ofs(node->lhs);
      if (lofs >= 0) {
        int ri = g->vtop - 1;
        SValue *rv = &g->vstack[ri];
        Type *lt = node->lhs->ty;
        if (is_flonum(lt)) {
          int fr = sv_gv(g, ri);
          store_mem_from(g, lt, SLJIT_FR(fr), lofs);
        } else if (rv->loc == SV_IMM) {
          JE(g, sljit_emit_op1(g->C, int_store_op(lt),
                               SLJIT_MEM1(SLJIT_SP), lofs, SLJIT_IMM, rv->imm));
        } else {
          int vr = sv_gv(g, ri);
          store_mem_from(g, lt, SLJIT_R(vr), lofs);
        }
        rv->ty = node->ty;
        return;
      }
    }
    // `a[i] = v` stores through a scaled index operand: [base + i*2^k].
    if (!is_aggregate_ty(node->lhs->ty) && !is_m32i64_type(node->lhs->ty)) {
      sljit_s32 mem;
      sljit_sw off;
      int vi = g->vtop - 1;
      if (scaled_mem_operand(g, node->lhs, &mem, &off)) {
        SValue *rv = &g->vstack[vi];
        Type *lt = node->lhs->ty;
        int bi = g->vtop - 2, xi = g->vtop - 1;
        if (is_flonum(lt)) {
          int fr = sv_gv_keep(g, vi, bi, xi);
          JE(g, sljit_emit_fop1(g->C, lt->kind == TY_FLOAT ? SLJIT_MOV_F32
                                                           : SLJIT_MOV_F64,
                                mem, off, SLJIT_FR(fr), 0));
        } else if (rv->loc == SV_IMM) {
          JE(g, sljit_emit_op1(g->C, int_store_op(lt), mem, off,
                               SLJIT_IMM, rv->imm));
        } else {
          int vr = sv_gv_keep(g, vi, bi, xi);
          JE(g, sljit_emit_op1(g->C, int_store_op(lt), mem, off,
                               SLJIT_R(vr), 0));
        }
        sv_pop(g);
        sv_pop(g);
        rv->ty = node->ty;
        return;
      }
    }
    gen_addr(g, node->lhs);
    int ri = g->vtop - 2, ai = g->vtop - 1;
    SValue *rv = &g->vstack[ri];
    Type *lhs_ty = node->lhs->ty;
    if (lhs_ty->kind == TY_STRUCT || lhs_ty->kind == TY_UNION) {
      // Both operands are addresses; the inline copy's temp register
      // must not hold any other live value.
      sv_clear_scratch(g, ri, ai);
      int src = sv_gv_real_keep(g, ri, ai);
      int dst = sv_gv_real_keep(g, ai, ri);
      store_from(g, lhs_ty, SLJIT_R(src), SLJIT_R(dst));
      JE(g, sljit_emit_op1(g->C, SLJIT_MOV, SLJIT_R(src), 0, SLJIT_R(dst), 0));
      sv_pop(g);
      rv->ty = node->ty;
      return;
    }
    if (is_flonum(lhs_ty)) {
      int fr = sv_gv(g, ri);
      sljit_s32 base = sv_base_reg(g, ai, ri);
      store_from(g, lhs_ty, SLJIT_FR(fr), base);
      sv_pop(g);
      return;
    }
    if (rv->loc == SV_IMM) {
      sljit_s32 base = sv_base_reg(g, ai, ri);
      JE(g, sljit_emit_op1(g->C, int_store_op(lhs_ty),
                           SLJIT_MEM1(base), 0, SLJIT_IMM, rv->imm));
      sv_pop(g);
      return;
    }
    {
      int vr = sv_gv_keep(g, ri, ai, -1);
      sljit_s32 base = sv_base_reg(g, ai, ri);
      store_from(g, lhs_ty, SLJIT_R(vr), base);
      sv_pop(g);
    }
    return;
  }
  case ND_STMT_EXPR: {
    Node *last = NULL;
    for (Node *n = node->body; n; n = n->next)
      last = n;
    for (Node *n = node->body; n; n = n->next) {
      if (n == last && n->kind == ND_EXPR_STMT)
        gen_expr(g, n->lhs);
      else
        gen_stmt(g, n);
    }
    return;
  }
  case ND_MEMZERO: {
    Obj *var = node->var;
    if (var->jreg > 0) {
      if (is_flonum(var->ty)) {
        sv_clear_scratch(g, -1, -1);
        load_dbl_zero(g, SLJIT_FR0);
        JE(g, sljit_emit_fop1(g->C, var->ty->kind == TY_FLOAT ? SLJIT_MOV_F32
                                                              : SLJIT_MOV_F64,
                             SLJIT_FS(var->jreg - 1), 0, SLJIT_FR0, 0));
      } else {
        sv_inval_sreg(g, var->jreg - 1);
        JE(g, sljit_emit_op1(g->C, SLJIT_MOV, SLJIT_S(var->jreg - 1), 0,
                             SLJIT_IMM, 0));
      }
      return;
    }
    const int word = (int)sizeof(sljit_sw);
    int ofs = 0;
    for (; ofs + word <= var->ty->size; ofs += word)
      JE(g, sljit_emit_op1(g->C, SLJIT_MOV,
                           SLJIT_MEM1(SLJIT_SP), var->ofs + ofs, SLJIT_IMM, 0));
    if (ofs + 4 <= var->ty->size) {
      JE(g, sljit_emit_op1(g->C, SLJIT_MOV32,
                           SLJIT_MEM1(SLJIT_SP), var->ofs + ofs, SLJIT_IMM, 0));
      ofs += 4;
    }
    if (ofs + 2 <= var->ty->size) {
      JE(g, sljit_emit_op1(g->C, SLJIT_MOV_U16,
                           SLJIT_MEM1(SLJIT_SP), var->ofs + ofs, SLJIT_IMM, 0));
      ofs += 2;
    }
    if (ofs < var->ty->size)
      JE(g, sljit_emit_op1(g->C, SLJIT_MOV_U8,
                           SLJIT_MEM1(SLJIT_SP), var->ofs + ofs, SLJIT_IMM, 0));
    return;
  }
  case ND_FUNCALL:
    gen_funcall(g, node);
    return;
  case ND_ASM:
    unsupported(node, "inline assembly");
  case ND_VA_START:
    gen_va_start(g, node);
    return;
  case ND_VA_COPY:
    gen_va_copy(g, node);
    return;
  case ND_VA_ARG:
    gen_va_arg(g, node);
    return;
  case ND_GOTO_EXPR:
  case ND_LABEL_VAL:
    unsupported(node, "computed goto");
  case ND_ALLOCA:
    unsupported(node, "alloca");
  default:
    break;
  }

  // Comparisons (ND_EQ..ND_GE) and binary operators (ND_ADD..ND_SAR).
  bool is_cmp = (node->kind >= ND_EQ && node->kind <= ND_GE);
  bool is_binary = (node->kind >= ND_ADD && node->kind <= ND_SAR);
  if (!is_cmp && !is_binary)
    error_tok(node->tok, "jitgen: unsupported expression kind %d", node->kind);

  // 64-bit integers: comparisons read two addresses, arithmetic and shifts
  // produce a new 8-byte temporary.
  if (is_cmp && is_m32i64_type(node->lhs->ty)) {
    gen_m32i64_cmp(g, node);
    return;
  }
  if (is_binary && is_m32i64_type(node->ty)) {
    if (node->kind == ND_SHL || node->kind == ND_SHR || node->kind == ND_SAR)
      gen_m32i64_shift(g, node);
    else
      gen_m32i64_binop(g, node);
    return;
  }

  // Pointer arithmetic with a 64-bit index on a 32-bit host: the index is
  // represented by the address of an 8-byte slot, so it must be truncated
  // to the host word size before the ordinary 32-bit add/sub path runs.
  if (is_binary && !is_m32i64_type(node->ty) &&
      (is_m32i64_type(node->lhs->ty) || is_m32i64_type(node->rhs->ty))) {
    Node cast_l, cast_r, copy;
    Node *l = node->lhs, *r = node->rhs;
    if (is_m32i64_type(l->ty)) {
      memset(&cast_l, 0, sizeof(cast_l));
      cast_l.kind = ND_CAST;
      cast_l.tok = l->tok;
      cast_l.lhs = l;
      cast_l.ty = ty_long;
      l = &cast_l;
    }
    if (is_m32i64_type(r->ty)) {
      memset(&cast_r, 0, sizeof(cast_r));
      cast_r.kind = ND_CAST;
      cast_r.tok = r->tok;
      cast_r.lhs = r;
      cast_r.ty = ty_long;
      r = &cast_r;
    }
    copy = *node;
    copy.lhs = l;
    copy.rhs = r;
    node = &copy;
  }

  // `p + i*2^k` pointer arithmetic becomes a single LEA.
  if (is_binary && gen_scaled_add(g, node))
    return;

  Type *ty = node->ty;
  bool flt = is_flonum(ty);
  bool is32 = ty->size == 4 && !flt;
  bool widen = is32 && !ty->is_unsigned && binop_widen_signed(node->kind);
  sljit_s32 op32 = is32 && !widen ? SLJIT_32 : 0;

  gen_expr(g, node->lhs);
  gen_expr(g, node->rhs);
  int li = g->vtop - 2, ri = g->vtop - 1;
  SValue *lv = &g->vstack[li], *rv = &g->vstack[ri];

  if (is_cmp) {
    bool cmp_flt = is_flonum(node->lhs->ty);
    if (cmp_flt) {
      sv_gv2(g, li, ri);
      int dst = sv_new_reg(g, node->ty); // comparison result is an int
      gen_cmp(g, SLJIT_R(dst), SLJIT_FR(lv->r), SLJIT_FR(rv->r), 0, node);
      sv_pop(g);
      sv_pop(g);
      sv_push(g, node->ty, SV_REG, dst, 0);
      return;
    }
    sljit_s32 lop, rop = 0;
    bool rmem = false;
    if (rv->loc == SV_IMM)
      lop = lv->loc == SV_SREG ? SLJIT_S(lv->r)
                               : SLJIT_R(sv_gv_keep(g, li, ri, -1));
    else {
      if (lv->loc == SV_SREG)
        lop = SLJIT_S(lv->r);
      else if (lv->loc == SV_REG)
        lop = SLJIT_R(lv->r);
      else
        lop = SLJIT_R(sv_gv_keep(g, li, ri, -1));
      if (rv->loc == SV_SREG)
        rop = SLJIT_S(rv->r);
      else if (rv->loc == SV_REG)
        rop = SLJIT_R(rv->r);
      else
        rmem = true;
    }
    sv_make_free(g, false, 1, li, ri);
    int dst = sv_pick_free(g, false);
    if (rv->loc == SV_IMM)
      gen_cmp_imm(g, SLJIT_R(dst), lop, rv->imm, node);
    else if (rmem)
      gen_cmp(g, SLJIT_R(dst), lop, SLJIT_MEM1(SLJIT_SP), sv_home(g, ri), node);
    else
      gen_cmp(g, SLJIT_R(dst), lop, rop, 0, node);
    sv_pop(g);
    sv_pop(g);
    sv_push(g, node->ty, SV_REG, dst, 0);
    return;
  }

  // Strength-reduce constant multiplication, division and modulo.
  if (!flt && rv->loc == SV_IMM &&
      (node->kind == ND_MUL || node->kind == ND_DIV || node->kind == ND_MOD)) {
    if (rv->imm == 1 && (node->kind == ND_MUL || node->kind == ND_DIV)) {
      // Identity: the operand is already canonical for the result type.
      sv_pop(g);
      lv->ty = ty;
      return;
    }
    int rl = sv_gv(g, li);
    if (emit_const_muldiv(g, node, ty, is32, li, rl, rv->imm)) {
      sv_pop(g);
      // Signed 32-bit sequences run word-wide and stay canonical.
      if (is32 && ty->is_unsigned)
        extend32_to(g, ty, SLJIT_R(rl));
      lv->ty = ty;
      return;
    }
  }

  // Integer DIVMOD has a hard register contract: R0 dividend, R1 divisor.
  if (!flt && (node->kind == ND_DIV || node->kind == ND_MOD)) {
    sv_to_conv2(g, li, 0, ri, 1);
    sv_pop(g);
    sv_pop(g);
    emit_divmod(g, node, ty, is32);
    sv_push(g, ty, SV_REG, 0, 0);
    return;
  }
  if (flt && node->kind == ND_MOD)
    unsupported(node, "float % (use fmod/fmodf)");

  sljit_s32 op = flt ? 0 : binop_op(node->kind, op32);
  sljit_s32 fop = 0;
  if (flt) {
    bool f32 = ty->kind == TY_FLOAT;
    switch (node->kind) {
    case ND_ADD: fop = f32 ? SLJIT_ADD_F32 : SLJIT_ADD_F64; break;
    case ND_SUB: fop = f32 ? SLJIT_SUB_F32 : SLJIT_SUB_F64; break;
    case ND_MUL: fop = f32 ? SLJIT_MUL_F32 : SLJIT_MUL_F64; break;
    case ND_DIV: fop = f32 ? SLJIT_DIV_F32 : SLJIT_DIV_F64; break;
    default: error_tok(node->tok, "jitgen: unsupported expression kind %d", node->kind);
    }
  } else if (op < 0) {
    error_tok(node->tok, "jitgen: unsupported expression kind %d", node->kind);
  }

  // Fold an immediate or home-slot operand straight into the instruction;
  // otherwise compute into a scratch register so a saved-register variable
  // can feed the ALU directly without being copied first.
  if (!flt && rv->loc == SV_IMM) {
    if (lv->loc == SV_SREG) {
      sv_make_free(g, false, 1, li, ri);
      int d = sv_pick_free(g, false);
      JE(g, sljit_emit_op2(g->C, op, SLJIT_R(d), 0, SLJIT_S(lv->r), 0,
                           SLJIT_IMM, rv->imm));
      sv_pop(g);
      lv->loc = SV_REG;
      lv->r = d;
    } else {
      int rl = sv_gv(g, li);
      JE(g, sljit_emit_op2(g->C, op, SLJIT_R(rl), 0, SLJIT_R(rl), 0,
                           SLJIT_IMM, rv->imm));
      sv_pop(g);
    }
  } else if (!flt && lv->loc == SV_IMM) {
    int rr = sv_gv(g, ri);
    JE(g, sljit_emit_op2(g->C, op, SLJIT_R(rr), 0, SLJIT_IMM, lv->imm,
                         SLJIT_R(rr), 0));
    sv_pop(g);
    lv->loc = SV_REG;
    lv->r = rr;
  } else if (rv->loc == SV_MEM) {
    if (!flt && lv->loc == SV_SREG) {
      sv_make_free(g, false, 1, li, ri);
      int d = sv_pick_free(g, false);
      JE(g, sljit_emit_op2(g->C, op, SLJIT_R(d), 0, SLJIT_S(lv->r), 0,
                           SLJIT_MEM1(SLJIT_SP), sv_home(g, ri)));
      sv_pop(g);
      lv->loc = SV_REG;
      lv->r = d;
    } else {
      int rl = sv_gv(g, li);
      if (flt)
        JE(g, sljit_emit_fop2(g->C, fop, SLJIT_FR(rl), 0, SLJIT_FR(rl), 0,
                              SLJIT_MEM1(SLJIT_SP), sv_home(g, ri)));
      else
        JE(g, sljit_emit_op2(g->C, op, SLJIT_R(rl), 0, SLJIT_R(rl), 0,
                             SLJIT_MEM1(SLJIT_SP), sv_home(g, ri)));
      sv_pop(g);
    }
  } else if (flt) {
    sv_gv2(g, li, ri);
    JE(g, sljit_emit_fop2(g->C, fop, SLJIT_FR(lv->r), 0,
                          SLJIT_FR(lv->r), 0, SLJIT_FR(rv->r), 0));
    sv_pop(g);
  } else if (lv->loc == SV_SREG) {
    sv_make_free(g, false, 1, li, ri);
    int d = sv_pick_free(g, false);
    sljit_s32 o2 = rv->loc == SV_SREG ? SLJIT_S(rv->r) : SLJIT_R(rv->r);
    JE(g, sljit_emit_op2(g->C, op, SLJIT_R(d), 0, SLJIT_S(lv->r), 0, o2, 0));
    sv_pop(g);
    lv->loc = SV_REG;
    lv->r = d;
  } else if (rv->loc == SV_SREG) {
    int rl = sv_gv(g, li);
    JE(g, sljit_emit_op2(g->C, op, SLJIT_R(rl), 0, SLJIT_R(rl), 0,
                         SLJIT_S(rv->r), 0));
    sv_pop(g);
  } else {
    sv_gv2(g, li, ri);
    JE(g, sljit_emit_op2(g->C, op, SLJIT_R(lv->r), 0,
                         SLJIT_R(lv->r), 0, SLJIT_R(rv->r), 0));
    sv_pop(g);
  }
  if (is32 && !widen) {
    int rl = sv_gv(g, li);
    extend32_to(g, ty, SLJIT_R(rl));
  }
  lv->ty = ty;
}


#endif // WIDCC_JITGEN_EXPR_C
