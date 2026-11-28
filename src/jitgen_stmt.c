#ifndef WIDCC_JITGEN_STMT_C
#define WIDCC_JITGEN_STMT_C

#include "jitgen_emit.c"

// Statement code generation: loops, labels, jumps and the statement
// dispatcher. Included by jitgen.c.
//
// Control flow helpers
//

// Evaluate an expression only for its side effects. Statement results and
// the trailing value of a desugared `x++` chain are never materialized.
static void gen_expr_discard(JGen *g, Node *node) {
  switch (node->kind) {
  case ND_CHAIN: {
    // A discarded post-increment chain `tmp = x; x = x + step; tmp` only
    // needs the update: the saved old value has no reader left.
    Node *tmp = node->rhs;
    if (tmp && tmp->kind == ND_VAR && !tmp->var->name && node->lhs &&
        node->lhs->kind == ND_CHAIN && node->lhs->lhs &&
        node->lhs->lhs->kind == ND_ASSIGN && node->lhs->lhs->lhs &&
        node->lhs->lhs->lhs->kind == ND_VAR &&
        node->lhs->lhs->lhs->var == tmp->var && node->lhs->rhs &&
        node->lhs->rhs->kind == ND_ASSIGN) {
      gen_expr_discard(g, node->lhs->rhs);
      return;
    }
    gen_expr_discard(g, node->lhs);
    gen_expr_discard(g, node->rhs);
    return;
  }
  case ND_COMMA:
    gen_expr_discard(g, node->lhs);
    gen_expr_discard(g, node->rhs);
    return;
  case ND_CAST:
    if (node->lhs->ty && node->lhs->ty->kind != TY_VOID) {
      gen_expr_discard(g, node->lhs);
      return;
    }
    break;
  case ND_NUM:
  case ND_VAR:
    return;
  default:
    break;
  }
  gen_expr(g, node);
  if (node->ty && node->ty->kind != TY_VOID)
    sv_pop(g);
}

static void loop_push(JGen *g, bool is_switch) {
  if (g->nloops >= 32)
    error("jitgen: loops nested too deeply");
  JLoop *l = &g->loops[g->nloops++];
  memset(l, 0, sizeof(*l));
  l->is_switch = is_switch;
}

static void jvec_push(struct sljit_jump ***vec, int *n, int *cap,
                      struct sljit_jump *j) {
  if (*n == *cap) {
    *cap = *cap ? *cap * 2 : 8;
    *vec = cc_realloc(*vec, sizeof(void *) * *cap);
  }
  (*vec)[(*n)++] = j;
}

// Record an unconditional pending break jump of the current frame.
static void gen_brk_jump(JGen *g) {
  JLoop *l = &g->loops[g->nloops - 1];
  struct sljit_jump *j = sljit_emit_jump(g->C, SLJIT_JUMP);
  jvec_push(&l->brk, &l->nbrk, &l->capbrk, j);
}

// Emit a label and, when it has a name, make it the resolution target for
// any goto/break/continue that refers to that name (both already-emitted
// pending jumps and future ones). Every jump source spills before jumping,
// so the fall-through path must reach the label with all values in memory
// too.
static struct sljit_label *gen_named_label(JGen *g, const char *name) {
  sv_spill_all(g);
  struct sljit_label *lab = lchk(sljit_emit_label(g->C));
  if (name) {
    hashmap_put(&g->flabels, (char *)name, lab);
    for (JPendingGoto *pg = g->pgotos; pg; pg = pg->next)
      if (!strcmp(pg->label, name))
        sljit_set_label(pg->jump, lab);
  }
  return lab;
}

// Emit a label and resolve all pending break jumps of the current frame.
// True when `n` contains no calls, assignments or statement expressions;
// such conditions are cheap enough to emit twice for loop rotation.
static bool simple_cond(Node *n) {
  for (; n; n = n->next) {
    switch (n->kind) {
    case ND_NULL_EXPR:
    case ND_NUM:
    case ND_VAR:
      continue;
    case ND_FUNCALL:
    case ND_ASSIGN:
    case ND_COND:
    case ND_COMMA:
    case ND_CHAIN:
    case ND_STMT_EXPR:
    case ND_MEMZERO:
    case ND_VA_ARG:
    case ND_VA_START:
    case ND_VA_COPY:
    case ND_ALLOCA:
      return false;
    default:
      break;
    }
    if (!simple_cond(n->lhs) || !simple_cond(n->rhs) || !simple_cond(n->cond) ||
        !simple_cond(n->then) || !simple_cond(n->els) || !simple_cond(n->body))
      return false;
  }
  return true;
}

static void gen_brk_label(JGen *g, const char *name) {
  struct sljit_label *lab = gen_named_label(g, name);
  JLoop *l = &g->loops[g->nloops - 1];
  for (int i = 0; i < l->nbrk; i++)
    sljit_set_label(l->brk[i], lab);
  l->nbrk = 0;
}

static void gen_cont_label(JGen *g, const char *name) {
  struct sljit_label *lab = gen_named_label(g, name);
  JLoop *l = &g->loops[g->nloops - 1];
  for (int i = 0; i < l->ncont; i++)
    sljit_set_label(l->cont[i], lab);
  l->ncont = 0;
}

//
// Statements
//

static void gen_stmt(JGen *g, Node *node) {
  // 64-bit temporaries never outlive the statement that created them.
  int save = g->m32i64_top;
  gen_stmt_impl(g, node);
  g->m32i64_top = save;
}

static void gen_stmt_impl(JGen *g, Node *node) {
  switch (node->kind) {
  case ND_NULL_EXPR:
  case ND_BLOCK: {
    if (node->kind == ND_NULL_EXPR && !node->body)
      return;
    for (Node *n = node->body; n; n = n->next)
      gen_stmt(g, n);
    return;
  }
  case ND_EXPR_STMT:
    gen_expr_discard(g, node->lhs);
    return;
  case ND_RETURN: {
    Type *rt = g->fn->ty->return_ty;
    if (!node->lhs || rt->kind == TY_VOID) {
      if (node->lhs) {
        gen_expr(g, node->lhs);
        if (node->lhs->ty->kind != TY_VOID)
          sv_pop(g);
      }
      JE(g, sljit_emit_return_void(g->C));
      return;
    }
    gen_expr(g, node->lhs);
    int r = sv_gv(g, g->vtop - 1);
    if (is_flonum(rt))
      JE(g, sljit_emit_return(g->C,
                              rt->kind == TY_FLOAT ? SLJIT_MOV_F32 : SLJIT_MOV_F64,
                              SLJIT_FR(r), 0));
    else
      JE(g, sljit_emit_return(g->C, SLJIT_MOV, SLJIT_R(r), 0));
    sv_pop(g);
    return;
  }
  case ND_IF: {
    // Statements run with all enclosing values in memory so that every
    // join point sees the same register state on all paths.
    sv_spill_all(g);
    struct sljit_jump *jels = gen_branch(g, node->cond, false);
    gen_stmt(g, node->then);
    if (!node->els) {
      struct sljit_label *lend = lchk(sljit_emit_label(g->C));
      sljit_set_label(jels, lend);
      return;
    }
    struct sljit_jump *jend = jchk(sljit_emit_jump(g->C, SLJIT_JUMP));
    struct sljit_label *lels = lchk(sljit_emit_label(g->C));
    sljit_set_label(jels, lels);
    gen_stmt(g, node->els);
    struct sljit_label *lend = lchk(sljit_emit_label(g->C));
    sljit_set_label(jend, lend);
    return;
  }
  case ND_FOR: {
    if (node->init)
      gen_stmt(g, node->init);
    loop_push(g, false);
    sv_spill_all(g); // back-edge and entry must agree: all values in memory
    // Rotate the loop so the back edge is the conditional test itself: the
    // body falls through to the condition and only the one-time entry needs
    // an unconditional jump. Cheaper conditions are duplicated; heavy ones
    // are emitted once at the top to keep code size down.
    bool rotate = node->cond && simple_cond(node->cond);
    struct sljit_jump *jpre = NULL;
    struct sljit_label *ltop = NULL;
    if (node->cond && !rotate) {
      // A condition with side effects is emitted once, above the body. The
      // back edge must re-enter the test, not the body, or the loop would
      // repeat without ever re-evaluating it.
      ltop = lchk(sljit_emit_label(g->C));
      struct sljit_jump *j = gen_branch(g, node->cond, false);
      jvec_push(&g->loops[g->nloops - 1].brk,
                &g->loops[g->nloops - 1].nbrk,
                &g->loops[g->nloops - 1].capbrk, j);
    } else if (node->cond) {
      struct sljit_jump *j = gen_branch(g, node->cond, false);
      jvec_push(&g->loops[g->nloops - 1].brk,
                &g->loops[g->nloops - 1].nbrk,
                &g->loops[g->nloops - 1].capbrk, j);
      jpre = jchk(sljit_emit_jump(g->C, SLJIT_JUMP));
    }
    struct sljit_label *lbody = lchk(sljit_emit_label(g->C));
    if (jpre)
      sljit_set_label(jpre, lbody);
    gen_stmt(g, node->then);
    gen_cont_label(g, node->cont_label);
    if (node->inc)
      gen_expr_discard(g, node->inc);
    if (rotate) {
      struct sljit_jump *j = gen_branch(g, node->cond, true);
      sljit_set_label(j, lbody);
    } else {
      struct sljit_jump *jback = jchk(sljit_emit_jump(g->C, SLJIT_JUMP));
      sljit_set_label(jback, ltop ? ltop : lbody);
    }
    gen_brk_label(g, node->brk_label);
    g->nloops--;
    return;
  }
  case ND_DO: {
    loop_push(g, false);
    sv_spill_all(g);
    struct sljit_label *lbegin = lchk(sljit_emit_label(g->C));
    gen_stmt(g, node->then);
    gen_cont_label(g, node->cont_label);
    struct sljit_jump *j = gen_branch(g, node->cond, true);
    sljit_set_label(j, lbegin);
    gen_brk_label(g, node->brk_label);
    g->nloops--;
    return;
  }
  case ND_SWITCH: {
    if (is_m32i64_type(node->cond->ty))
      unsupported(node, "64-bit switch control expressions");
    gen_expr(g, node->cond);
    sv_spill_all(g); // dispatch jumps into the body: agree on all-memory
    sv_force(g, g->vtop - 1, 0);
    if (node->cond->ty->size == 4)
      extend32_to(g, node->cond->ty, SLJIT_R0);
    sv_pop(g);
    loop_push(g, true);
    // Compare the controlling expression against each case; the target
    // labels are placed later when the matching ND_CASE node is emitted.
    bool is32 = node->cond->ty->size == 4;
    for (Node *c = node->case_next; c; c = c->case_next) {
      struct sljit_jump *j;
      if (c->begin == c->end) {
        j = jchk(sljit_emit_cmp(g->C,
                                   is32 ? (SLJIT_EQUAL | SLJIT_32) : SLJIT_EQUAL,
                                   SLJIT_R0, 0, SLJIT_IMM, c->begin));
      } else {
        JE(g, sljit_emit_op2(g->C, is32 ? SLJIT_SUB32 : SLJIT_SUB,
                             SLJIT_R1, 0, SLJIT_R0, 0, SLJIT_IMM, c->begin));
        j = jchk(sljit_emit_cmp(g->C,
                                   is32 ? (SLJIT_LESS_EQUAL | SLJIT_32) : SLJIT_LESS_EQUAL,
                                   SLJIT_R1, 0, SLJIT_IMM, c->end - c->begin));
      }
      JCaseJump *cj = cc_calloc(1, sizeof(JCaseJump));
      cj->cnode = c;
      cj->jump = j;
      cj->next = g->pjumps;
      g->pjumps = cj;
    }
    if (node->default_case) {
      JCaseJump *cj = cc_calloc(1, sizeof(JCaseJump));
      cj->cnode = node->default_case;
      cj->jump = jchk(sljit_emit_jump(g->C, SLJIT_JUMP));
      cj->next = g->pjumps;
      g->pjumps = cj;
    } else {
      gen_brk_jump(g);
    }
    gen_stmt(g, node->then);
    gen_brk_label(g, node->brk_label);
    g->nloops--;
    return;
  }
  case ND_CASE: {
    sv_spill_all(g);
    struct sljit_label *lab = lchk(sljit_emit_label(g->C));
    for (JCaseJump *cj = g->pjumps; cj; cj = cj->next)
      if (cj->cnode == node)
        sljit_set_label(cj->jump, lab);
    if (node->lhs)
      gen_stmt(g, node->lhs);
    return;
  }
  case ND_LABEL: {
    gen_named_label(g, node->unique_label);
    if (node->lhs)
      gen_stmt(g, node->lhs);
    return;
  }
  case ND_GOTO: {
    sv_spill_all(g);
    struct sljit_label *lab = hashmap_get(&g->flabels, node->unique_label);
    struct sljit_jump *j = jchk(sljit_emit_jump(g->C, SLJIT_JUMP));
    if (lab) {
      sljit_set_label(j, lab);
    } else {
      JPendingGoto *pg = cc_calloc(1, sizeof(JPendingGoto));
      pg->label = node->unique_label;
      pg->jump = j;
      pg->next = g->pgotos;
      g->pgotos = pg;
    }
    return;
  }
  default:
    break;
  }
  error_tok(node->tok, "jitgen: unsupported statement kind %d", node->kind);
}


#endif // WIDCC_JITGEN_STMT_C
