#ifndef WIDCC_JITGEN_FUNC_C
#define WIDCC_JITGEN_FUNC_C

// Function compilation: evaluation-stack depth prescan, saved-register
// allocation, frame layout, prologue/parameter moves, per-function code
// generation, and the translation-unit driver. Included by jitgen.c.
//
// Evaluation-stack depth prescan
//
// Computes how many value-stack slots an expression or statement needs
// beyond the current depth, mirroring the push/pop structure of the code
// generator. Overestimation is safe (only costs stack space).

static int expr_depth(Node *node);

static int addr_depth(Node *node) {
  switch (node->kind) {
  case ND_VAR:
    return 1; // gen_addr pushes the address as a value
  case ND_DEREF:
    return expr_depth(node->lhs);
  case ND_CHAIN:
  case ND_COMMA:
    return MAX(expr_depth(node->lhs), addr_depth(node->rhs));
  case ND_MEMBER:
    return addr_depth(node->lhs);
  default:
    return 0;
  }
}

static int stmt_depth(Node *node) {
  if (!node)
    return 0;
  switch (node->kind) {
  case ND_BLOCK:
  case ND_STMT_EXPR: {
    int d = 0;
    for (Node *n = node->body; n; n = n->next)
      d = MAX(d, stmt_depth(n));
    return d;
  }
  case ND_EXPR_STMT:
    return expr_depth(node->lhs);
  case ND_RETURN:
    return node->lhs ? expr_depth(node->lhs) : 0;
  case ND_IF:
    return MAX(expr_depth(node->cond),
               MAX(stmt_depth(node->then), stmt_depth(node->els)));
  case ND_FOR: {
    int d = stmt_depth(node->init);
    if (node->cond)
      d = MAX(d, expr_depth(node->cond));
    d = MAX(d, stmt_depth(node->then));
    if (node->inc)
      d = MAX(d, expr_depth(node->inc));
    return d;
  }
  case ND_DO:
    return MAX(stmt_depth(node->then), expr_depth(node->cond));
  case ND_SWITCH:
    return MAX(expr_depth(node->cond), stmt_depth(node->then));
  case ND_CASE:
  case ND_LABEL:
    return stmt_depth(node->lhs);
  default:
    return 0; // ND_GOTO, ND_NULL_EXPR
  }
}

static int expr_depth(Node *node) {
  switch (node->kind) {
  case ND_NULL_EXPR:
  case ND_MEMZERO:
    return 0;
  case ND_NUM:
    return 1; // even an immediate occupies a value-stack slot
  case ND_VAR:
  case ND_MEMBER:
    return addr_depth(node);
  case ND_ADDR:
    return addr_depth(node->lhs);
  case ND_DEREF:
  case ND_POS:
  case ND_NEG:
  case ND_NOT:
  case ND_BITNOT:
  case ND_CAST:
    return expr_depth(node->lhs);
  case ND_LOGAND:
  case ND_LOGOR:
  case ND_COMMA:
  case ND_CHAIN:
    return MAX(expr_depth(node->lhs), expr_depth(node->rhs));
  case ND_COND:
    return MAX(expr_depth(node->cond),
               MAX(expr_depth(node->then), expr_depth(node->els)));
  case ND_ASSIGN:
    // rhs is evaluated first, then the lhs address is pushed above it.
    return MAX(expr_depth(node->rhs), 1 + addr_depth(node->lhs));
  case ND_STMT_EXPR: {
    int d = 0;
    Node *last = NULL;
    for (Node *n = node->body; n; n = n->next)
      last = n;
    for (Node *n = node->body; n; n = n->next)
      d = MAX(d, (n == last && n->kind == ND_EXPR_STMT)
                   ? expr_depth(n->lhs) : stmt_depth(n));
    return d;
  }
  case ND_FUNCALL: {
    int d = expr_depth(node->lhs); // callee, then pushed
    int n = 1;
    for (Obj *a = node->args; a; a = a->param_next) {
      d = MAX(d, n + expr_depth(a->arg_expr));
      n++;
    }
    return d;
  }
  default:
    break;
  }
  // ND_ADD..ND_GE are contiguous: binary ops and comparisons, which
  // evaluate lhs, push it, then evaluate rhs.
  if (node->kind >= ND_ADD && node->kind <= ND_GE)
    return MAX(expr_depth(node->lhs), 1 + expr_depth(node->rhs));
  return 0; // unsupported kinds are rejected during codegen
}

//
// Local variable layout (positive offsets into the sljit local area)
//

// Per-function saved-register allocation state.
typedef struct {
  unsigned used_int, used_flt; // allocation masks, bit i = S(i)/FS(i)
  int max_int;
  int max_flt;                 // number of float saved registers to declare
} JRegAlloc;

static bool reg_candidate(Obj *var) {
  // Unnamed variables are parser temporaries (e.g. the saved old value of
  // `x++`); they live on the value stack for one expression, never needing
  // a persistent register.
  if (!var->is_local || !var->name || var->address_taken || var->arg_expr)
    return false;
  if (is_m32i64_type(var->ty))
    return false;  // i64 values are addressed, not held in one register
  TypeKind k = var->ty->kind;
  return is_integer(var->ty) || k == TY_FLOAT || k == TY_DOUBLE ||
         k == TY_PTR;
}

static int alloc_bit(unsigned *used, int budget) {
  for (int i = 0; i < budget; i++)
    if (!(*used & (1u << i))) {
      *used |= 1u << i;
      return i;
    }
  return -1;
}

// The operand of `&`: walk to the variable whose address is exposed.
static void mark_addr_of(Node *node) {
  for (;;) {
    if (node->kind == ND_VAR) {
      node->var->address_taken = true;
      return;
    }
    if (node->kind == ND_MEMBER) {
      node = node->lhs;
      continue;
    }
    if (node->kind == ND_COMMA || node->kind == ND_CHAIN) {
      node = node->rhs;
      continue;
    }
    return;
  }
}

static void mark_stmt_addrs(Node *node);

static void mark_expr_addrs(Node *node) {
  if (!node)
    return;
  switch (node->kind) {
  case ND_ADDR:
    mark_addr_of(node->lhs);
    mark_expr_addrs(node->lhs);
    return;
  case ND_FUNCALL:
    mark_expr_addrs(node->lhs);
    for (Obj *a = node->args; a; a = a->param_next)
      mark_expr_addrs(a->arg_expr);
    return;
  case ND_STMT_EXPR:
    for (Node *n = node->body; n; n = n->next)
      mark_stmt_addrs(n);
    return;
  case ND_COND:
    mark_expr_addrs(node->cond);
    mark_expr_addrs(node->then);
    mark_expr_addrs(node->els);
    return;
  default:
    mark_expr_addrs(node->lhs);
    mark_expr_addrs(node->rhs);
    return;
  }
}

static void mark_stmt_addrs(Node *node) {
  if (!node)
    return;
  switch (node->kind) {
  case ND_BLOCK:
    for (Node *n = node->body; n; n = n->next)
      mark_stmt_addrs(n);
    return;
  case ND_EXPR_STMT:
  case ND_RETURN:
    mark_expr_addrs(node->lhs);
    return;
  case ND_CASE:
  case ND_LABEL:
    mark_stmt_addrs(node->lhs);
    return;
  case ND_IF:
    mark_expr_addrs(node->cond);
    mark_stmt_addrs(node->then);
    mark_stmt_addrs(node->els);
    return;
  case ND_FOR:
    mark_stmt_addrs(node->init);
    mark_expr_addrs(node->cond);
    mark_stmt_addrs(node->then);
    mark_expr_addrs(node->inc);
    return;
  case ND_DO:
    mark_stmt_addrs(node->then);
    mark_expr_addrs(node->cond);
    return;
  case ND_SWITCH:
    mark_expr_addrs(node->cond);
    mark_stmt_addrs(node->then);
    return;
  default:
    return;
  }
}

// --- Use-weighted local allocation --------------------------------------
// Saved registers are scarce, so they go to the locals that are read and
// written most often rather than to the first ones declared. Locals declared
// `register` outrank all of them.

typedef struct {
  Obj *var;
  int uses;  // loop-weighted
  int total; // raw occurrences, tie-breaker: shorter live ranges win
  int order;
} VarUse;

// Uses inside a loop body run once per iteration; weight them so a variable
// touched on every pass outranks one that is live around the loop.
#define LOOP_WEIGHT 16
#define WEIGHT_CAP 4096

static void count_uses_expr(Node *n, VarUse *tab, int nv, int w);

static void count_uses_stmt(Node *n, VarUse *tab, int nv, int w) {
  for (; n; n = n->next) {
    switch (n->kind) {
    case ND_BLOCK:
    case ND_STMT_EXPR:
      count_uses_stmt(n->body, tab, nv, w);
      break;
    case ND_IF:
      count_uses_expr(n->cond, tab, nv, w);
      count_uses_stmt(n->then, tab, nv, w);
      count_uses_stmt(n->els, tab, nv, w);
      break;
    case ND_FOR: {
      int lw = MIN(w * LOOP_WEIGHT, WEIGHT_CAP);
      // The condition runs on every pass, the body may not.
      int cw = MIN(lw * 4, WEIGHT_CAP * 4);
      count_uses_stmt(n->init, tab, nv, w);
      count_uses_expr(n->cond, tab, nv, cw);
      count_uses_stmt(n->then, tab, nv, lw);
      count_uses_expr(n->inc, tab, nv, lw);
      break;
    }
    case ND_DO: {
      int lw = MIN(w * LOOP_WEIGHT, WEIGHT_CAP);
      int cw = MIN(lw * 4, WEIGHT_CAP * 4);
      count_uses_stmt(n->then, tab, nv, lw);
      count_uses_expr(n->cond, tab, nv, cw);
      break;
    }
    case ND_SWITCH:
      count_uses_expr(n->cond, tab, nv, w);
      count_uses_stmt(n->then, tab, nv, w);
      break;
    case ND_MEMZERO:
    case ND_GOTO:
    case ND_LABEL_VAL:
      break;
    default:
      count_uses_expr(n->lhs, tab, nv, w);
      break;
    }
  }
}

static void count_uses_expr(Node *n, VarUse *tab, int nv, int w) {
  if (!n)
    return;
  switch (n->kind) {
  case ND_VAR:
    for (int i = 0; i < nv; i++)
      if (tab[i].var == n->var) {
        tab[i].uses += w;
        tab[i].total++;
        break;
      }
    return;
  case ND_NUM:
  case ND_NULL_EXPR:
    return;
  case ND_FUNCALL:
    count_uses_expr(n->lhs, tab, nv, w);
    for (Obj *a = n->args; a; a = a->param_next)
      count_uses_expr(a->arg_expr, tab, nv, w);
    return;
  case ND_STMT_EXPR:
    count_uses_stmt(n->body, tab, nv, w);
    return;
  default:
    count_uses_expr(n->lhs, tab, nv, w);
    count_uses_expr(n->rhs, tab, nv, w);
    count_uses_expr(n->cond, tab, nv, w);
    count_uses_expr(n->then, tab, nv, w);
    count_uses_expr(n->els, tab, nv, w);
    return;
  }
}

static int collect_int_candidates(Scope *sc, VarUse *tab, int n, int cap) {
  for (Obj *v = sc->locals; v; v = v->next) {
    if (v->jreg == 0 && !v->is_param && !is_flonum(v->ty) && reg_candidate(v) &&
        n < cap) {
      tab[n].var = v;
      tab[n].uses = 0;
      tab[n].total = 0;
      tab[n].order = n;
      n++;
    }
  }
  for (Scope *s = sc->children; s; s = s->sibling_next)
    n = collect_int_candidates(s, tab, n, cap);
  return n;
}

static int count_int_candidates(Scope *sc) {
  int n = 0;
  for (Obj *v = sc->locals; v; v = v->next)
    if (v->jreg == 0 && !v->is_param && !is_flonum(v->ty) && reg_candidate(v))
      n++;
  for (Scope *s = sc->children; s; s = s->sibling_next)
    n += count_int_candidates(s);
  return n;
}

static int lay_out_locals(Scope *sc, int top, JRegAlloc *ra,
                          int int_budget, int flt_budget) {
  // `register` locals take a saved register before ordinary ones; inside each
  // pass the order is unchanged.
  for (int pass = 0; pass < 2; pass++) {
    bool want = pass == 0;
    for (Obj *var = sc->locals; var; var = var->next) {
      if (var->jreg > 0 || var->is_param || var->is_register != want ||
          !reg_candidate(var))
        continue;
      bool flt = is_flonum(var->ty);
      int n = alloc_bit(flt ? &ra->used_flt : &ra->used_int,
                        flt ? flt_budget : int_budget);
      if (n >= 0) {
        var->jreg = n + 1;
        if (flt)
          ra->max_flt = MAX(ra->max_flt, n + 1);
        else
          ra->max_int = MAX(ra->max_int, n + 1);
      }
    }
  }

  for (Obj *var = sc->locals; var; var = var->next) {
    if (var->jreg > 0)
      continue;
    Type *ty = var->ty;
    if (ty->kind == TY_VLA || ty->size < 0)
      error("jitgen: variable-length arrays are not supported");
    int align = (ty->kind == TY_ARRAY && ty->size >= 16)
      ? MAX(16, ty->align) : ty->align;
    if (align > 16)
      align = 16;
    top = align_to(top, align);
    var->ofs = top;
    top += ty->size;
  }

  int max_top = top;
  for (Scope *sub = sc->children; sub; sub = sub->sibling_next)
    max_top = MAX(max_top, lay_out_locals(sub, top, ra,
                                          int_budget, flt_budget));
  return max_top;
}

//
// Function compilation
//
// The sljit register budget is negotiated per function so that scratch
// registers grow to fill whatever the saved set does not need, while the
// saved set is sized to hold incoming parameters and spill-register local
// variables.
// -----------------------------------------------------------------
//   saved_int  = max(nint_params, ra.max_int)
//                clamped to [2, SLJIT_NUMBER_OF_SAVED_REGISTERS]
//   scratch_int = SLJIT_NUMBER_OF_REGISTERS - saved_int, min 4
//   saved_flt   = max(ra.max_flt, 1), capped at SLJIT_NUMBER_OF_SAVED_FLOAT_REGISTERS
//   scratch_flt = max(SLJIT_NUMBER_OF_FLOAT_REGISTERS - saved_flt, 0)
// -----------------------------------------------------------------

// Emit one complete function body: prologue, parameter moves, statements
// and the fall-through return. Run twice per function: first with a
// throwaway compiler to measure the spill area, then for real.
static void emit_function_body(JGen *g, Type *fty, sljit_s32 at,
                               sljit_s32 local_size) {
  JE(g, sljit_emit_enter(g->C, 0, at,
                         g->scratch_int | SLJIT_ENTER_FLOAT(g->scratch_flt),
                         g->saved_int | SLJIT_ENTER_FLOAT(g->saved_flt),
                         local_size));

  if (g->block_abi) {
    // Block ABI: the incoming arguments are a pointer to the caller-built
    // argument block (S0) and the total argument count (S1). The block pointer
    // is moved to scratch R0 because the saved registers are free for locals;
    // scratch and saved ranges are disjoint, so R0/R1 stay valid throughout.
    JE(g, sljit_emit_op1(g->C, SLJIT_MOV, SLJIT_R0, 0, SLJIT_S0, 0));
    // Variadic functions need the block base later for va_start, after the
    // body has clobbered R0.
    if (g->va_block_ofs >= 0)
      JE(g, sljit_emit_op1(g->C, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP),
                           g->va_block_ofs, SLJIT_R0, 0));
    // Copy each argument into its frame slot. sljit has no memory-to-memory
    // move, so the copy goes through a scratch register: a float is 4 bytes
    // (raw copy through an integer register) while a double is 8 bytes and,
    // since a word move is only 4 bytes on a 32-bit host, uses a float scratch.
    int slot = 0;
    for (Obj *p = fty->param_list; p; p = p->param_next, slot++) {
      Type *t = p->ty;
      sljit_sw off = (sljit_sw)slot * 8;
      if (t->kind == TY_FLOAT) {
        JE(g, sljit_emit_op1(g->C, SLJIT_MOV32, SLJIT_R1, 0,
                             SLJIT_MEM1(SLJIT_R0), off));
        JE(g, sljit_emit_op1(g->C, SLJIT_MOV32, SLJIT_MEM1(SLJIT_SP),
                             p->ofs, SLJIT_R1, 0));
      } else if (t->kind == TY_DOUBLE) {
        JE(g, sljit_emit_fop1(g->C, SLJIT_MOV_F64, SLJIT_FR0, 0,
                              SLJIT_MEM1(SLJIT_R0), off));
        JE(g, sljit_emit_fop1(g->C, SLJIT_MOV_F64, SLJIT_MEM1(SLJIT_SP),
                              p->ofs, SLJIT_FR0, 0));
      } else {
        JE(g, sljit_emit_op1(g->C, int_load_op(t), SLJIT_R1, 0,
                             SLJIT_MEM1(SLJIT_R0), off));
        JE(g, sljit_emit_op1(g->C, int_store_op(t), SLJIT_MEM1(SLJIT_SP),
                             p->ofs, SLJIT_R1, 0));
      }
    }
  } else {
    // Spill the arrival registers S(ii)/FR(fi) into the frame slots; the local
    // allocator then owns every saved register.
    int ii = 0, fi = 0;
    for (Obj *p = fty->param_list; p; p = p->param_next) {
      Type *t = p->ty;
      if (is_flonum(t)) {
        sljit_s32 op = t->kind == TY_FLOAT ? SLJIT_MOV_F32 : SLJIT_MOV_F64;
        JE(g, sljit_emit_fop1(g->C, op, SLJIT_MEM1(SLJIT_SP), p->ofs, SLJIT_FR(fi), 0));
        fi++;
      } else {
        JE(g, sljit_emit_op1(g->C, int_store_op(t),
                             SLJIT_MEM1(SLJIT_SP), p->ofs, SLJIT_S(ii), 0));
        ii++;
      }
    }
  }

  gen_stmt(g, g->fn->body);
  assert(g->vtop == 0);

  // Falling off the end: return 0 (undefined per C11 for non-void main,
  // but the generated code must still return cleanly).
  Type *rt = fty->return_ty;
  if (rt->kind == TY_VOID) {
    JE(g, sljit_emit_return_void(g->C));
  } else if (rt->kind == TY_FLOAT) {
    JE(g, sljit_emit_fset32(g->C, SLJIT_FR0, (sljit_f32)0.0));
    JE(g, sljit_emit_return(g->C, SLJIT_MOV_F32, SLJIT_FR0, 0));
  } else if (rt->kind == TY_DOUBLE) {
    JE(g, sljit_emit_fset64(g->C, SLJIT_FR0, 0.0));
    JE(g, sljit_emit_return(g->C, SLJIT_MOV_F64, SLJIT_FR0, 0));
  } else {
    JE(g, sljit_emit_op1(g->C, SLJIT_MOV, SLJIT_R0, 0, SLJIT_IMM, 0));
    JE(g, sljit_emit_return(g->C, SLJIT_MOV, SLJIT_R0, 0));
  }
}

static void compile_function(JTU *tu, Obj *fn) {
  Type *fty = fn->ty;
  if (fty->is_oldstyle)
    error("jitgen: unprototyped function is not supported: %s", fn->name);
  if (fty->return_ty->kind == TY_STRUCT || fty->return_ty->kind == TY_UNION)
    error("jitgen: struct/union return value is not supported: %s", fn->name);
  if (fty->return_ty->kind == TY_LDOUBLE)
    error("jitgen: long double is not supported: %s", fn->name);
  if (fn->dealloc_vla)
    error("jitgen: VLAs are not supported: %s", fn->name);

#if !WIDCC_HOST_64BIT
  if (is_integer(fty->return_ty) && fty->return_ty->size == 8)
    error("jitgen: 64-bit integer return values are not supported on a "
          "32-bit host: %s", fn->name);
  for (Obj *p = fty->param_list; p; p = p->param_next)
    if (is_integer(p->ty) && p->ty->size == 8)
      error("jitgen: 64-bit integer parameters are not supported on a "
            "32-bit host: %s", fn->name);
#endif

  int nint = 0, nflt = 0, narg = 0;
  for (Obj *p = fty->param_list; p; p = p->param_next) {
    Type *t = p->ty;
    if (t->kind == TY_STRUCT || t->kind == TY_UNION)
      error("jitgen: struct/union arguments are not supported: %s", fn->name);
    if (t->kind == TY_LDOUBLE)
      error("jitgen: long double is not supported: %s", fn->name);
    if (is_flonum(t))
      nflt++;
    else
      nint++;
    narg++;
  }

  // Variadic functions and those with more than JMAX_ARGS parameters use
  // the Block ABI: the native entry takes a pointer to an argument block plus
  // the total argument count, so sljit never sees more than two arguments.
  bool block_abi = fty->is_variadic || narg > JMAX_ARGS;
  if (!block_abi && (nint > JMAX_INT_ARGS || nflt > JMAX_FLT_ARGS))
    error("jitgen: too many parameters: %s", fn->name);

  sljit_s32 at = SLJIT_ARG_RETURN(arg_type_of(fty->return_ty));
  if (fty->return_ty->kind == TY_VOID)
    at = SLJIT_ARG_RETURN(SLJIT_ARG_TYPE_RET_VOID);
  if (block_abi) {
    at |= SLJIT_ARG_VALUE(SLJIT_ARG_TYPE_P, 1);
    at |= SLJIT_ARG_VALUE(SLJIT_ARG_TYPE_W, 2);
  } else {
    int pos = 0;
    for (Obj *p = fty->param_list; p; p = p->param_next)
      at |= SLJIT_ARG_VALUE(arg_type_of(p->ty), ++pos);
  }

  JGen g;
  memset(&g, 0, sizeof(g));
  g.tu = tu;
  g.fn = fn;
  g.block_abi = block_abi;
  mark_stmt_addrs(fn->body);

  // --- Parameters stay in the frame ---------------------------------------
  // sljit delivers parameters in the first nint/nflt saved registers, but
  // those registers are the scarcest resource in the function: one held by a
  // parameter for its whole live range is one a hot loop local cannot use.
  // Parameters are therefore spilled to their frame slots in the prologue
  // (see emit_function_body) and the saved registers are left to the local
  // allocator. The budget below still reserves nint/nflt of them so the
  // incoming arguments have somewhere to land.
  JRegAlloc ra = {0};

  // --- Lay out locals into as many saved registers as allowed --------------
  // Locals are placed before the final budget is known; the saved set then
  // grows to hold them.
  int local_budget = SLJIT_NUMBER_OF_SAVED_REGISTERS;
  {
    int ncand = count_int_candidates(fty->scopes);
    if (ncand > 0) {
      VarUse *vu = cc_calloc(ncand, sizeof(VarUse));
      int n = collect_int_candidates(fty->scopes, vu, 0, ncand);
      count_uses_stmt(fn->body, vu, n, 1);
      // `register` locals come first; everything else keeps the use-weighted
      // order.
      for (int i = 1; i < n; i++) {
        VarUse key = vu[i];
        int j = i - 1;
        while (j >= 0 &&
               (vu[j].var->is_register < key.var->is_register ||
                (vu[j].var->is_register == key.var->is_register &&
                 (vu[j].uses < key.uses ||
                  (vu[j].uses == key.uses && vu[j].total > key.total) ||
                  (vu[j].uses == key.uses && vu[j].total == key.total &&
                   vu[j].order > key.order))))) {
          vu[j + 1] = vu[j];
          j--;
        }
        vu[j + 1] = key;
      }
      for (int i = 0; i < n; i++) {
        Obj *v = vu[i].var;
        if (v->jreg || v->is_param)
          continue; // parameters keep their frame slot
        int b = alloc_bit(&ra.used_int, local_budget);
        if (b < 0)
          break;
        v->jreg = b + 1;
        ra.max_int = MAX(ra.max_int, b + 1);
      }
    }
  }
  int lsz = lay_out_locals(fty->scopes, 0, &ra, local_budget,
                           SLJIT_NUMBER_OF_SAVED_FLOAT_REGISTERS);

  // --- Decide the per-function register budget ----------------------------
  {
    // Parameters are spilled to their frame slots in the prologue, but they
    // still arrive in the first nint/nflt saved registers, so that many must
    // stay in the ABI.
    int need_saved_int = block_abi ? ra.max_int : MAX(nint, ra.max_int);
    int need_saved_flt = block_abi ? ra.max_flt : MAX(nflt, ra.max_flt);
    g.saved_int = MIN(need_saved_int, SLJIT_NUMBER_OF_SAVED_REGISTERS);
    g.saved_int = MAX(g.saved_int, 2); // always at least one scratch pair
    g.saved_int = MIN(g.saved_int, SLJIT_NUMBER_OF_REGISTERS - 4);
    g.saved_flt = MIN(need_saved_flt, SLJIT_NUMBER_OF_SAVED_FLOAT_REGISTERS);
  }

  g.eval_base = align_to(lsz, 8);
  // One value-stack entry per stack position: stmt_depth is a tight bound on
  // the height codegen reaches, plus a few slots of margin for temporary
  // pushes inside emitters (sv_push fails loudly if ever exceeded).
  g.vcap = stmt_depth(fn->body) + 4;
  g.vstack = cc_calloc(g.vcap, sizeof(SValue));
  g.spill_used = cc_calloc(g.vcap, 1);

  g.scratch_int = SLJIT_NUMBER_OF_REGISTERS - g.saved_int;
  g.scratch_int = MAX(g.scratch_int, 4); // sljit needs at least a few
  g.scratch_flt = SLJIT_NUMBER_OF_FLOAT_REGISTERS - g.saved_flt;
  g.scratch_flt = MAX(g.scratch_flt, 0);

  // --- Measure the spill area ----------------------------------------------
  // Home slots go only to values that actually leave their register, and a
  // slot is released as soon as the value is popped, so the frame reserves
  // the peak number of simultaneously spilled values instead of one slot per
  // value-stack position. That peak depends on the register-allocation
  // decisions, so emit the body once into a throwaway compiler and keep its
  // slot accounting; the real pass then runs with exactly as many slots.
  {
    JGen m = g;
    m.C = sljit_create_compiler(NULL);
    if (!m.C)
      error("jitgen: sljit compiler allocation failed");
    tu->cc->active_compiler = m.C;
    m.spill_cap = g.vcap;
    m.m32i64_cap = g.vcap;
    m.m32i64_base = align_to(g.eval_base + g.vcap * 8, 8);
    // Reserve an upper-bound argument-block area; the real size is the peak
    // recorded while emitting the body.
    m.argblock_base = align_to(m.m32i64_base + m.m32i64_cap * 8, 8);
    m.argblock_max = 0;
    sljit_s32 probe = align_to(m.argblock_base + g.vcap * 8, 16);
    // The saved block pointer lives at the top of this throwaway frame; the
    // real pass places it after the (smaller) argument-block area.
    m.va_block_ofs = fty->is_variadic ? (int)probe - 8 : -1;
    emit_function_body(&m, fty, at, probe);
    sljit_free_compiler(m.C);
    tu->cc->active_compiler = NULL;
    hashmap_destroy(&m.flabels);
    g.spill_max = m.spill_max;
    g.m32i64_max = m.m32i64_max;
    g.argblock_max = m.argblock_max;
  }

  // --- Exact frame layout --------------------------------------------------
#if WIDCC_HOST_64BIT
  // Native 64-bit integers need no scratch area.
  g.m32i64_base = 0;
  g.argblock_base = align_to(g.eval_base + g.spill_max * 8, 8);
#else
  // 8-byte temporaries live above the home-slot area.
  g.m32i64_base = align_to(g.eval_base + g.spill_max * 8, 8);
  g.argblock_base = align_to(g.m32i64_base + g.m32i64_max * 8, 8);
#endif
  g.argblock_max = align_to(g.argblock_max, 8);
  // A variadic frame also needs one word for the saved block pointer.
  int frame = g.argblock_base + g.argblock_max;
  if (fty->is_variadic) {
    g.va_block_ofs = frame;
    frame += 8;
  } else {
    g.va_block_ofs = -1;
  }
  sljit_s32 local_size = align_to(frame, 16);
  g.spill_cap = g.spill_max;
  g.m32i64_cap = g.m32i64_max;
  g.m32i64_top = 0;
  g.spill_live = 0;
  memset(g.spill_used, 0, g.vcap);

  g.C = sljit_create_compiler(NULL);
  if (!g.C)
    error("jitgen: sljit compiler allocation failed");
  tu->cc->active_compiler = g.C;
  if (getenv("WIDCC_JIT_VERBOSE"))
    sljit_compiler_verbose(g.C, stderr);

  emit_function_body(&g, fty, at, local_size);

  void *code = sljit_generate_code(g.C, 0, NULL);
  if (!code) {
    // On error the compiler is released by the load error path through
    // cc->active_compiler.
    sljit_s32 e = sljit_get_compiler_error(g.C);
    error("jitgen: code generation failed for %s (sljit error %d)", fn->name, e);
  }

  // Convert the rewritable calls of this function into end-of-TU patches.
  // The jump nodes only carry their code address after generation.
  {
    sljit_sw exec_off = sljit_get_executable_offset(g.C);
    for (JCallJump *cj = g.call_jumps; cj; cj = cj->next) {
      JPendingCall *pc = cc_calloc(1, sizeof(JPendingCall));
      pc->addr = sljit_get_jump_addr(cj->jump);
      pc->exec_offset = exec_off;
      pc->target = cj->target;
      pc->next = tu->pending_calls;
      tu->pending_calls = pc;
    }
  }
  sljit_free_compiler(g.C);
  tu->cc->active_compiler = NULL;

  WidccRT *jrt = tu->rt;
  JitCode *jc = rt_alloc(sizeof(JitCode));
  jc->code = code;
  jc->next = tu->cc->mod->codes;
  tu->cc->mod->codes = jc;

  fn->jit_addr = code;
  if (fn->is_static)
    return; // per-TU cells are filled at end of TU

  // A later definition replaces an earlier one: new lookups resolve to the
  // new code and indirection cells are repointed. Old code stays alive (and
  // callable through previously obtained pointers) until its module is
  // unloaded.
  JitSym *s = jit_sym(jrt, fn->name, true);
  s->addr = code;
  if (s->cell)
    *s->cell = code;
}

//
// Data emission and relocations
//

// Find a global object of this TU by name (functions, globals, string
// literals and static locals). Prefers definitions over declarations.
static Obj *find_tu_obj(Obj *prog, char *name) {
  Obj *found = NULL;
  for (Obj *var = prog; var; var = var->next) {
    if (var->name && !strcmp(var->name, name)) {
      if (var->is_definition)
        return var;
      found = var;
    }
    if (var->ty->kind == TY_FUNC && var->is_definition) {
      for (Obj *sv = var->static_lvars; sv; sv = sv->next)
        if (sv->name && !strcmp(sv->name, name))
          return sv;
    }
  }
  return found;
}

// Resolve a data-relocation target name to an address.
static void *resolve_reloc_target(JTU *tu, Obj *prog, char *name, Obj **target_out) {
  Obj *target = find_tu_obj(prog, name);
  if (target_out)
    *target_out = target;
  if (target && target->jit_addr)
    return target->jit_addr;
  JitSym *s = jit_sym(tu->rt, name, target && target->ty->kind == TY_FUNC);
  if (s->addr)
    return s->addr;
  void *host = jit_resolve_host(tu->rt, name);
  if (host) {
    s->addr = host;
    return host;
  }
  return NULL;
}

static void emit_data(JTU *tu, Obj *prog) {
  for (Obj *var = prog; var; var = var->next) {
    if (!var->is_definition)
      continue;

    if (var->ty->kind == TY_FUNC) {
      if (var->is_live)
        emit_data(tu, var->static_lvars);
      continue;
    }

    if (var->is_tls)
      error("jitgen: TLS variables are not supported: %s", var->name);

    int64_t sz = var->ty->size;
    if (sz < 0 && var->is_tentative && var->ty->kind == TY_ARRAY)
      sz = var->ty->base->size;
    if (sz < 0)
      error("jitgen: object '%s' has incomplete type", var->name);

    WidccRT *rt = tu->rt;
    JitSym *s = var->is_static ? NULL : jit_sym(rt, var->name, false);

    char *buf;
    if (s && s->addr) {
      // Symbol already defined: merge tentative definitions like .comm.
      if (var->is_tentative) {
        var->jit_addr = s->addr;
        continue;
      }
      if (!s->is_tentative)
        error("jitgen: duplicate definition: %s", var->name);
      buf = s->addr; // replace the tentative zero block in place
      memset(buf, 0, sz);
      s->is_tentative = false;
    } else {
      // All generated data lives on the runtime heap: machine code keeps
      // referring to it after the compiler (and its arena) is destroyed.
      buf = rt_alloc_data(tu->cc, MAX(sz, 1));
      if (s) {
        s->addr = buf;
        s->is_tentative = var->is_tentative;
        // If any call site has already asked for an indirection cell
        // (e.g. a forward reference), repoint it.
        if (s->cell)
          *s->cell = buf;
      }
    }
    var->jit_addr = buf;

    if (var->init_data)
      memcpy(buf, var->init_data, sz);

    for (Relocation *rel = var->rel; rel; rel = rel->next) {
      char *name = *rel->label;
      Obj *target = NULL;
      void *addr = resolve_reloc_target(tu, prog, name, &target);
      if (!addr && target && target->ty->kind == TY_FUNC &&
          target->is_definition && !target->jit_addr) {
        // A function of this TU compiled later: patch after codegen.
        JPendingRel *pr = cc_calloc(1, sizeof(JPendingRel));
        pr->buf = buf;
        pr->offset = rel->offset;
        pr->target = target;
        pr->name = name;
        pr->addend = rel->addend;
        pr->next = tu->pending_rels;
        tu->pending_rels = pr;
        continue;
      }
      if (!addr)
        error("jitgen: undefined symbol in initializer: %s", name);
      uintptr_t val = (uintptr_t)addr + (uintptr_t)rel->addend;
      memcpy(buf + rel->offset, &val, sizeof(val));
    }
  }
}

//
// Translation unit entry point
//

static void jitgen_tu(WidccCompiler *cc, Obj *prog) {
  JTU tu;
  memset(&tu, 0, sizeof(tu));
  tu.cc = cc;
  tu.rt = cc->rt;

  // Data first: function initializers may reference functions compiled below.
  emit_data(&tu, prog);

  // Compile in source order. The parser prepends definitions, so `prog` is
  // in reverse source order; compiling backwards means calls to earlier
  // definitions bind directly, leaving only forward references and
  // self-recursion for the rewritable-call patches.
  int nfn = 0;
  for (Obj *fn = prog; fn; fn = fn->next) {
    if (fn->ty->kind != TY_FUNC || !fn->is_definition || !fn->is_live)
      continue;
    nfn++;
  }
  Obj **fns = cc_calloc(nfn ? nfn : 1, sizeof(Obj *));
  int nf = 0;
  for (Obj *fn = prog; fn; fn = fn->next) {
    if (fn->ty->kind != TY_FUNC || !fn->is_definition || !fn->is_live)
      continue;
    fns[nf++] = fn;
  }
  while (nf > 0)
    compile_function(&tu, fns[--nf]);

  // Rewrite the placeholder targets of calls that referenced functions
  // compiled after the call site.
  for (JPendingCall *pc = tu.pending_calls; pc; pc = pc->next) {
    if (!pc->target->jit_addr)
      error("jitgen: function '%s' is called but not compiled",
            pc->target->name);
    sljit_set_jump_addr(pc->addr, (sljit_uw)pc->target->jit_addr,
                        pc->exec_offset);
  }

  // Patch data initializers that referenced not-yet-compiled functions.
  for (JPendingRel *pr = tu.pending_rels; pr; pr = pr->next) {
    void *addr = NULL;
    if (pr->target)
      addr = pr->target->jit_addr;
    if (!addr && pr->name)
      addr = resolve_reloc_target(&tu, prog, pr->name, NULL);
    if (!addr)
      error("jitgen: undefined symbol in initializer: %s", pr->name);
    uintptr_t val = (uintptr_t)addr + (uintptr_t)pr->addend;
    memcpy(pr->buf + pr->offset, &val, sizeof(val));
  }

  // Fill indirection cells of static functions.
  for (JStaticCell *sc = tu.static_cells; sc; sc = sc->next) {
    if (!sc->fn->jit_addr)
      error("jitgen: static function '%s' is used but not defined", sc->fn->name);
    *sc->cell = sc->fn->jit_addr;
  }
}


#endif // WIDCC_JITGEN_FUNC_C
