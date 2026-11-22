#ifndef WIDCC_JITGEN_C
#define WIDCC_JITGEN_C

// sljit-based JIT code generator for widcc.
//
// Consumes the parsed AST (Obj list) of one translation unit and produces
// executable machine code in memory. Value model (tcc-style): `gen_expr`
// pushes one SValue per result onto an evaluation stack. Each SValue knows
// where it lives — a scratch register, its home slot in the local area, or
// a compile-time immediate — and registers are allocated on demand, with
// the oldest register-held value spilled to its home slot when the supply
// runs out. No scratch register is statically reserved.
// Scalar locals whose address never escapes are kept in saved registers.
//
// Not supported yet (reported as compile errors): floating varargs,
// va_list/va_arg, VLAs, alloca, bitfields, long double, struct arguments
// or returns passed by value, computed goto, inline asm, TLS.
#include "widcc.h"
#include "ccctx.h"
#include "jit_internal.h"
#include "sljitLir.h"

// The backend emits code for the host itself, so pointers and sljit words
// always have the same width: 8 bytes on LP64/LLP64, 4 bytes on ILP32.
_Static_assert(sizeof(sljit_sw) == sizeof(void *),
               "sljit word size must match the host pointer size");

// A 32-bit sljit backend has no 64-bit integer registers. Such hosts get
// 32-bit native integers plus emulated 64-bit expressions (see the
// "64-bit integers on a 32-bit host" section); 64-bit parameters and
// return values are rejected explicitly rather than silently truncated.
#if defined(__LP64__) || defined(_LP64) || defined(_WIN64)
#define WIDCC_HOST_64BIT 1
#else
#define WIDCC_HOST_64BIT 0
#endif

// The sljit register budget is chosen per function (not a global
// constant) so that scratch registers expand to fill whatever the saved
// set doesn't need. See `compile_function` for the sizing formula.
//
// Hardware ABI we're targeting: up to 4 word + 4 float register arguments,
// matching x86-64 SysV and MinGW/x64.
enum { JMAX_INT_ARGS = 4, JMAX_FLT_ARGS = 4, JMAX_ARGS = 4 };

typedef struct JLoop JLoop;
struct JLoop {
  bool is_switch;
  struct sljit_jump **brk;  // pending break jumps
  int nbrk, capbrk;
  struct sljit_jump **cont; // pending continue jumps
  int ncont, capcont;
};

// Pending goto jump whose target label has not been placed yet.
typedef struct JPendingGoto JPendingGoto;
struct JPendingGoto {
  JPendingGoto *next;
  char *label;
  struct sljit_jump *jump;
};

// Indirection cell for a call to a static function defined later in the TU.
typedef struct JStaticCell JStaticCell;
struct JStaticCell {
  JStaticCell *next;
  Obj *fn;
  void **cell;
};

// Relocation of a data initializer that references a not-yet-compiled
// function of this TU.
typedef struct JPendingRel JPendingRel;
struct JPendingRel {
  JPendingRel *next;
  char *buf;      // data block base
  int offset;
  Obj *target;    // function Obj (same TU) or NULL for name lookup
  char *name;
  long addend;
};

// Call to a function of this TU whose code does not exist yet (forward
// reference, self-recursion, or a callee defined later in source order).
// The call instruction is emitted with SLJIT_REWRITABLE_JUMP and a
// placeholder target; the real address is written once the TU is compiled.
typedef struct JPendingCall JPendingCall;
struct JPendingCall {
  JPendingCall *next;
  sljit_uw addr;        // patchable slot inside the call instruction
  sljit_sw exec_offset; // executable offset of that code block
  Obj *target;
};

typedef struct JTU {
  WidccCompiler *cc;
  WidccRT *rt;
  JStaticCell *static_cells;
  JPendingRel *pending_rels;
  JPendingCall *pending_calls;
} JTU;

typedef struct JCaseJump JCaseJump;
struct JCaseJump {
  JCaseJump *next;
  Node *cnode;             // ND_CASE/ND_DEFAULT node owning this jump
  struct sljit_jump *jump; // patched when the case label is emitted
};

// Rewritable call emitted while compiling a function, converted into a
// JPendingCall after code generation (sljit jump nodes are freed with the
// compiler, their addresses only exist once the code is generated).
typedef struct JCallJump JCallJump;
struct JCallJump {
  JCallJump *next;
  struct sljit_jump *jump;
  Obj *target;
};

// Evaluation-stack value with location information (tcc's SValue).
// SV_REG: lives in SLJIT_R(r) / SLJIT_FR(r) depending on the type class.
// SV_MEM: lives in its home slot, eval_base + slot*8.
// SV_IMM: compile-time integer constant (floats are always materialized).
// SV_SREG: reads a local variable held in the saved register SLJIT_S(r).
//          The variable itself is the storage, so the value is copied out
//          before the variable is assigned.
enum { SV_REG, SV_MEM, SV_IMM, SV_SREG };

typedef struct SValue {
  Type *ty;
  int loc;
  int r;         // SV_REG: scratch register index; SV_SREG: saved register index
  int slot;      // home spill slot; -1 until the value first leaves a register
  sljit_sw imm;  // SV_IMM: constant value
} SValue;

typedef struct JGen {
  JTU *tu;
  struct sljit_compiler *C;
  Obj *fn;

  // Per-function register budget (decided in compile_function, stored so
  // downstream helpers like gen_funcall can reference the right icall
  // target register without re-deriving it).
  int saved_int, saved_flt;

  // Evaluation value stack. A home slot is owned by a value from its first
  // spill until it is popped, so the frame only reserves slots for values
  // that are actually spilled at the same time, not one slot per position.
  SValue *vstack;
  int vtop, vcap;
  int scratch_int;   // total scratch integer registers available
  int scratch_flt;   // total scratch float registers available
  int eval_base;     // local-area offset of the home-slot area

  unsigned char *spill_used; // per-slot occupancy of the home-slot area
  int spill_cap;             // number of slots reserved (measured capacity)
  int spill_live;            // slots currently in use
  int spill_max;             // peak of spill_live over this emission

  // 64-bit integer temporaries on a 32-bit host. An i64 value is
  // represented by the address of an 8-byte slot in this frame area, so
  // each i64 expression result occupies one ordinary integer stack slot.
  int m32i64_base;      // local-area offset of the i64 scratch area
  int m32i64_top;       // live 8-byte temporaries in the current statement
  int m32i64_cap;       // capacity in 8-byte temporaries
  int m32i64_max;       // peak of m32i64_top over this emission

  HashMap flabels;
  JPendingGoto *pgotos;
  JCallJump *call_jumps; // rewritable calls patched at end of TU
  JLoop loops[32];
  int nloops;
  JCaseJump *pjumps;
} JGen;

static void gen_expr(JGen *g, Node *node);
static void gen_stmt(JGen *g, Node *node);
static void gen_stmt_impl(JGen *g, Node *node);
static bool scaled_mem_operand(JGen *g, Node *deref, sljit_s32 *mem,
                               sljit_sw *offw);

// sljit call wrapper with error reporting (longjmps through ccctx).
#define JE(g, call) do { \
  sljit_s32 _e = (call); \
  if (_e) \
    error("jitgen: sljit error %d at %s:%d", _e, __FILE__, __LINE__); \
} while (0)

// sljit has no absolute-addressing operand form; dereference an absolute
// address by materializing it in a scratch register first.
// R(r) := *addr
static void load_abs(JGen *g, void *addr, int r) {
  JE(g, sljit_emit_op1(g->C, SLJIT_MOV, SLJIT_R(r), 0, SLJIT_IMM, (sljit_sw)addr));
  JE(g, sljit_emit_op1(g->C, SLJIT_MOV, SLJIT_R(r), 0, SLJIT_MEM1(SLJIT_R(r)), 0));
}

// jump/label creation returns NULL on allocation failure.
static struct sljit_jump *jchk(struct sljit_jump *j) {
  if (!j)
    error("jitgen: out of memory");
  return j;
}

static struct sljit_label *lchk(struct sljit_label *l) {
  if (!l)
    error("jitgen: out of memory");
  return l;
}

static void NORETURN unsupported(Node *node, char *what) {
  error_tok(node->tok, "not supported by the JIT backend: %s", what);
}

//
// Value stack and dynamic register allocation
//
// gen_expr pushes one SValue per result. A value stays in its register
// until the register is needed elsewhere; then it is spilled to a home
// slot (eval_base + slot*8), which is fixed for the value's lifetime and
// freed when the value is popped. Emitters with hard register conventions
// (DIVMOD wants R0/R1, icall wants R0..R3/FR0..FR3, absolute loads clobber
// R0) evict the current holders of those registers first instead of relying
// on a reserved scratch set.
//

// On x86-32 only R0..R2 are real registers; R3+ are virtual and cannot
// serve as a memory base. Values used as addresses must be pulled into a
// real register.
#if WIDCC_HOST_64BIT
#define REAL_INT_REGS(g) ((g)->scratch_int)
#else
#define REAL_INT_REGS(g) MIN((g)->scratch_int, 3)
#endif

// True when saved register SLJIT_S(s) may appear inside a memory operand.
// x86-32 has only S0..S2 as real saved registers (operand numbers 10..12);
// S3..S8 are virtual frame slots, which sljit rejects as a MEM base or
// index. Callers copy such a value into a real scratch register instead.
#if WIDCC_HOST_64BIT
#define SREG_ADDR_OK(s) 1
#else
#define SREG_ADDR_OK(s) ((s) < 3)
#endif

static bool sv_flt(SValue *v) {
  return is_flonum(v->ty);
}

// Claim the lowest free home slot for a value that is leaving its register.
// The slot is held until the value is popped, so at most one slot per live
// spilled value is reserved and the frame sizing pass sees the true peak.
static int spill_slot(JGen *g, SValue *v) {
  if (v->slot >= 0)
    return v->slot;
  for (int i = 0; i < g->spill_cap; i++)
    if (!g->spill_used[i]) {
      g->spill_used[i] = 1;
      g->spill_live++;
      g->spill_max = MAX(g->spill_max, g->spill_live);
      v->slot = i;
      return i;
    }
  error("jitgen: out of home slots for spilled values");
}

static int sv_home(JGen *g, int i) {
  SValue *v = &g->vstack[i];
  if (v->slot < 0)
    error("jitgen: memory operand for a value without a home slot");
  return g->eval_base + v->slot * 8;
}

static SValue *sv_push(JGen *g, Type *ty, int loc, int r, sljit_sw imm) {
  if (g->vtop >= g->vcap)
    error("jitgen: value stack overflow");
  SValue *v = &g->vstack[g->vtop++];
  v->ty = ty;
  v->loc = loc;
  v->r = r;
  v->slot = -1;
  v->imm = imm;
  return v;
}

static void sv_pop(JGen *g) {
  assert(g->vtop > 0);
  SValue *v = &g->vstack[--g->vtop];
  if (v->slot >= 0) {
    g->spill_used[v->slot] = 0;
    g->spill_live--;
    v->slot = -1;
  }
}

// Write a register-held value to its home slot; it becomes SV_MEM.
// Floats round-trip as F64 even for TY_FLOAT: the low 32 bits are the
// float value and F32 ops only read those.
static void sv_spill(JGen *g, int i) {
  SValue *v = &g->vstack[i];
  if (v->loc != SV_REG && v->loc != SV_SREG)
    return;
  spill_slot(g, v);
  if (v->loc == SV_SREG) {
    // Join points read the home slot; a saved register must be written
    // back even though it needs no scratch register.
    JE(g, sljit_emit_op1(g->C, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP),
                          sv_home(g, i), SLJIT_S(v->r), 0));
  } else if (sv_flt(v)) {
    JE(g, sljit_emit_fop1(g->C, SLJIT_MOV_F64, SLJIT_MEM1(SLJIT_SP),
                          sv_home(g, i), SLJIT_FR(v->r), 0));
  } else {
    JE(g, sljit_emit_op1(g->C, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP),
                          sv_home(g, i), SLJIT_R(v->r), 0));
  }
  v->loc = SV_MEM;
}

static void sv_spill_all(JGen *g) {
  for (int i = 0; i < g->vtop; i++)
    sv_spill(g, i);
}

// Spill only the values below `base` (everything that stays live across a
// call); callee/argument slots are consumed by the call itself.
static void sv_spill_below(JGen *g, int base) {
  for (int i = 0; i < base; i++)
    sv_spill(g, i);
}

static bool reg_in_use(JGen *g, int r, bool flt) {
  for (int i = 0; i < g->vtop; i++) {
    SValue *v = &g->vstack[i];
    if (v->loc == SV_REG && sv_flt(v) == flt && v->r == r)
      return true;
  }
  return false;
}

// Spill the oldest register-held value of class `flt`, never touching
// stack indices keep1/keep2 (-1 for none).
static void sv_evict_oldest(JGen *g, bool flt, int keep1, int keep2) {
  for (int i = 0; i < g->vtop; i++) {
    if (i == keep1 || i == keep2)
      continue;
    SValue *v = &g->vstack[i];
    if (v->loc == SV_REG && sv_flt(v) == flt) {
      sv_spill(g, i);
      return;
    }
  }
  error("jitgen: register allocation deadlock");
}

// Make `n` registers of class `flt` available, spilling the oldest
// register-held values as needed. keep1/keep2 are never spilled.
static void sv_make_free(JGen *g, bool flt, int n, int keep1, int keep2) {
  int nregs = flt ? g->scratch_flt : g->scratch_int;
  assert(nregs > 0);
  for (;;) {
    int used = 0;
    for (int r = 0; r < nregs; r++)
      if (reg_in_use(g, r, flt))
        used++;
    if (nregs - used >= n)
      return;
    sv_evict_oldest(g, flt, keep1, keep2);
  }
}

static int sv_pick_free(JGen *g, bool flt) {
  int nregs = flt ? g->scratch_flt : g->scratch_int;
  for (int r = 0; r < nregs; r++)
    if (!reg_in_use(g, r, flt))
      return r;
  error("jitgen: register allocation deadlock");
}

// Allocate a register for a value that is about to be pushed. No gen_expr
// may run between this call and the push that claims the register.
static int sv_new_reg(JGen *g, Type *ty) {
  bool flt = is_flonum(ty);
  sv_make_free(g, flt, 1, -1, -1);
  return sv_pick_free(g, flt);
}

// Materialize stack value i into register r of its class.
static void sv_load_into(JGen *g, int i, int r) {
  SValue *v = &g->vstack[i];
  bool flt = sv_flt(v);
  if (v->loc == SV_REG && v->r == r)
    return;
  if (v->loc == SV_REG) {
    if (flt)
      JE(g, sljit_emit_fop1(g->C, SLJIT_MOV_F64, SLJIT_FR(r), 0,
                            SLJIT_FR(v->r), 0));
    else
      JE(g, sljit_emit_op1(g->C, SLJIT_MOV, SLJIT_R(r), 0, SLJIT_R(v->r), 0));
  } else if (v->loc == SV_SREG) {
    JE(g, sljit_emit_op1(g->C, SLJIT_MOV, SLJIT_R(r), 0, SLJIT_S(v->r), 0));
  } else if (v->loc == SV_IMM) {
    assert(!flt);
    JE(g, sljit_emit_op1(g->C, SLJIT_MOV, SLJIT_R(r), 0, SLJIT_IMM, v->imm));
  } else {
    if (flt)
      JE(g, sljit_emit_fop1(g->C, SLJIT_MOV_F64, SLJIT_FR(r), 0,
                            SLJIT_MEM1(SLJIT_SP), sv_home(g, i)));
    else
      JE(g, sljit_emit_op1(g->C, SLJIT_MOV, SLJIT_R(r), 0,
                            SLJIT_MEM1(SLJIT_SP), sv_home(g, i)));
  }
  v->loc = SV_REG;
  v->r = r;
}

// Ensure stack value i lives in some register (gv in tcc terms).
static int sv_gv(JGen *g, int i) {
  SValue *v = &g->vstack[i];
  if (v->loc == SV_REG)
    return v->r;
  bool flt = sv_flt(v);
  sv_make_free(g, flt, 1, i, -1);
  int r = sv_pick_free(g, flt);
  sv_load_into(g, i, r);
  return r;
}

// Ensure stack value i lives in an integer scratch register, never
// spilling the values at keep1/keep2 (-1 for none).
static int sv_gv_keep(JGen *g, int i, int keep1, int keep2) {
  SValue *v = &g->vstack[i];
  if (v->loc == SV_REG)
    return v->r;
  sv_make_free(g, false, 1, keep1, keep2);
  int r = sv_pick_free(g, false);
  sv_load_into(g, i, r);
  return r;
}

// Ensure both operands live in registers. Same-class pairs are allocated
// together so neither load can evict the other.
static void sv_gv2(JGen *g, int i, int j) {
  SValue *a = &g->vstack[i], *b = &g->vstack[j];
  bool fa = sv_flt(a), fb = sv_flt(b);
  if (fa != fb) {
    sv_gv(g, i);
    sv_gv(g, j);
    return;
  }
  int need = (a->loc != SV_REG) + (b->loc != SV_REG);
  if (!need)
    return;
  sv_make_free(g, fa, need, i, j);
  if (a->loc != SV_REG)
    sv_load_into(g, i, sv_pick_free(g, fa));
  if (b->loc != SV_REG)
    sv_load_into(g, j, sv_pick_free(g, fb));
}

// Spill every value holding register r of class flt.
static void sv_free_reg(JGen *g, bool flt, int r) {
  for (int i = 0; i < g->vtop; i++) {
    SValue *v = &g->vstack[i];
    if (v->loc == SV_REG && sv_flt(v) == flt && v->r == r)
      sv_spill(g, i);
  }
}

// Move stack value i into the specific register r, spilling any other
// value currently holding it.
static void sv_force(JGen *g, int i, int r) {
  SValue *v = &g->vstack[i];
  if (v->loc == SV_REG && v->r == r)
    return;
  sv_free_reg(g, sv_flt(v), r);
  sv_load_into(g, i, r);
}

// Ensure stack value i lives in a real register usable as a memory base,
// never spilling stack index keep (-1 for none).
static int sv_gv_real_keep(JGen *g, int i, int keep) {
  SValue *v = &g->vstack[i];
  if (v->loc == SV_REG && v->r < REAL_INT_REGS(g))
    return v->r;
  for (;;) {
    int r;
    for (r = 0; r < REAL_INT_REGS(g); r++)
      if (!reg_in_use(g, r, false))
        break;
    if (r < REAL_INT_REGS(g)) {
      sv_load_into(g, i, r);
      return r;
    }
    int victim = -1;
    for (int k = 0; k < g->vtop; k++) {
      if (k == i || k == keep)
        continue;
      SValue *w = &g->vstack[k];
      if (w->loc == SV_REG && !sv_flt(w) && w->r < REAL_INT_REGS(g)) {
        victim = k;
        break;
      }
    }
    if (victim < 0)
      error("jitgen: register allocation deadlock");
    sv_spill(g, victim);
  }
}

static int sv_gv_real(JGen *g, int i) {
  return sv_gv_real_keep(g, i, -1);
}

// Register operand useful as a memory base, allocating a scratch register
// only when the value is not already a local in a real saved register.
static sljit_s32 sv_base_reg(JGen *g, int i, int keep) {
  SValue *v = &g->vstack[i];
  if (v->loc == SV_SREG && !sv_flt(v) && SREG_ADDR_OK(v->r))
    return SLJIT_S(v->r);
  return SLJIT_R(sv_gv_real_keep(g, i, keep));
}

// A live value reading saved register `sreg` must be copied out before the
// variable in that register is overwritten.
static void sv_inval_sreg(JGen *g, int sreg) {
  for (int i = 0; i < g->vtop; i++) {
    SValue *v = &g->vstack[i];
    if (v->loc != SV_SREG || v->r != sreg)
      continue;
    spill_slot(g, v);
    JE(g, sljit_emit_op1(g->C, SLJIT_MOV, SLJIT_MEM1(SLJIT_SP),
                         sv_home(g, i), SLJIT_S(sreg), 0));
    v->loc = SV_MEM;
  }
}

// Evict values sitting in registers that hard-convention emitters clobber
// (R0..R2, FR0..FR1), except stack indices keep1/keep2.
static void sv_clear_scratch(JGen *g, int keep1, int keep2) {
  for (int i = 0; i < g->vtop; i++) {
    if (i == keep1 || i == keep2)
      continue;
    SValue *v = &g->vstack[i];
    if (v->loc != SV_REG)
      continue;
    if (sv_flt(v) ? v->r <= 1 : v->r <= 2)
      sv_spill(g, i);
  }
}

// Move value i into hard-convention register r (R(r) or FR(r) by class),
// spilling every other value in a convention-clobbered register.
static void sv_to_conv(JGen *g, int i, int r) {
  sv_clear_scratch(g, i, -1);
  sv_force(g, i, r);
}

// Two-operand version: i → ri, j → rj.
static void sv_to_conv2(JGen *g, int i, int ri, int j, int rj) {
  sv_clear_scratch(g, i, j);
  sv_force(g, i, ri);
  sv_force(g, j, rj);
}

//
// Loads, stores, integer canonicalization
//

static sljit_s32 int_load_op(Type *ty) {
  switch (ty->size) {
  case 1: return ty->is_unsigned ? SLJIT_MOV_U8 : SLJIT_MOV_S8;
  case 2: return ty->is_unsigned ? SLJIT_MOV_U16 : SLJIT_MOV_S16;
  case 4: return ty->is_unsigned ? SLJIT_MOV_U32 : SLJIT_MOV_S32;
  default: return SLJIT_MOV;
  }
}

// Sized store op; the value's low bits are stored.
static sljit_s32 int_store_op(Type *ty) {
  switch (ty->size) {
  case 1: return SLJIT_MOV_U8;
  case 2: return SLJIT_MOV_U16;
  case 4: return SLJIT_MOV32;
  default: return SLJIT_MOV;
  }
}

// Re-canonicalize an arbitrary integer register `reg` after a 32-bit op.
static void extend32_to(JGen *g, Type *ty, sljit_s32 reg) {
  // SLJIT_MOV32 leaves the upper half unspecified, so it cannot extend.
  JE(g, sljit_emit_op1(g->C, ty->is_unsigned ? SLJIT_MOV_U32 : SLJIT_MOV_S32,
                       reg, 0, reg, 0));
}

// True when every value canonical for `from` is also canonical for `to`,
// i.e. storing it without a re-extend preserves the representation. Values
// are kept sign/zero-extended to word width for their own static type.
static bool same_canon_repr(Type *from, Type *to) {
  if (!from || is_flonum(from) || is_flonum(to))
    return false;
  if (to->size == 8)
    return true;
  if (from->kind == TY_BOOL)
    return true;
  if (from->size == to->size)
    return from->is_unsigned == to->is_unsigned;
  // A narrower value converted to a wider signed type, or an unsigned
  // narrower value converted to any wider unsigned type, keeps its
  // extension pattern.
  if (from->size < to->size)
    return !to->is_unsigned || from->is_unsigned;
  return false;
}

// Aggregate/array/function values are represented by their address, which
// the struct-copy assignment path reads straight out of the value's register.
static bool is_aggregate_ty(Type *ty) {
  switch (ty->kind) {
  case TY_ARRAY:
  case TY_STRUCT:
  case TY_UNION:
  case TY_FUNC:
  case TY_VLA:
    return true;
  default:
    return false;
  }
}

// 64-bit integers are natively supported by a 64-bit host. On a 32-bit
// host they are emulated as an address pointing at an 8-byte scratch slot.
static bool is_m32i64_type(Type *ty) {
#if WIDCC_HOST_64BIT
  (void)ty;
  return false;
#else
  return ty && is_integer(ty) && ty->size == 8;
#endif
}

// Load a value of type `ty` from the memory pointed to by `addr_reg`
// into the integer or float scratch register `dst`.
static void load_to(JGen *g, sljit_s32 dst, sljit_s32 addr_reg, Type *ty) {
  switch (ty->kind) {
  case TY_ARRAY:
  case TY_STRUCT:
  case TY_UNION:
  case TY_FUNC:
  case TY_VLA:
    // The address itself is the value: if dst != addr_reg, copy.
    if (dst != addr_reg)
      JE(g, sljit_emit_op1(g->C, SLJIT_MOV, dst, 0, addr_reg, 0));
    return;
  case TY_FLOAT:
    JE(g, sljit_emit_fop1(g->C, SLJIT_MOV_F32,
                          dst, 0, SLJIT_MEM1(addr_reg), 0));
    return;
  case TY_DOUBLE:
    JE(g, sljit_emit_fop1(g->C, SLJIT_MOV_F64,
                          dst, 0, SLJIT_MEM1(addr_reg), 0));
    return;
  case TY_BOOL:
    JE(g, sljit_emit_op1(g->C, SLJIT_MOV_U8,
                         dst, 0, SLJIT_MEM1(addr_reg), 0));
    return;
  default:
    JE(g, sljit_emit_op1(g->C, int_load_op(ty),
                         dst, 0, SLJIT_MEM1(addr_reg), 0));
    return;
  }
}

// Load a scalar from a frame slot directly (no address register).
static void load_mem_to(JGen *g, sljit_s32 dst, int ofs, Type *ty) {
  switch (ty->kind) {
  case TY_FLOAT:
    JE(g, sljit_emit_fop1(g->C, SLJIT_MOV_F32, dst, 0,
                          SLJIT_MEM1(SLJIT_SP), ofs));
    return;
  case TY_DOUBLE:
    JE(g, sljit_emit_fop1(g->C, SLJIT_MOV_F64, dst, 0,
                          SLJIT_MEM1(SLJIT_SP), ofs));
    return;
  case TY_BOOL:
    JE(g, sljit_emit_op1(g->C, SLJIT_MOV_U8, dst, 0,
                         SLJIT_MEM1(SLJIT_SP), ofs));
    return;
  default:
    JE(g, sljit_emit_op1(g->C, int_load_op(ty), dst, 0,
                         SLJIT_MEM1(SLJIT_SP), ofs));
    return;
  }
}

// Store a scalar to a frame slot directly (no address register).
static void store_mem_from(JGen *g, Type *ty, sljit_s32 val_reg, int ofs) {
  switch (ty->kind) {
  case TY_FLOAT:
    JE(g, sljit_emit_fop1(g->C, SLJIT_MOV_F32, SLJIT_MEM1(SLJIT_SP), ofs,
                          val_reg, 0));
    return;
  case TY_DOUBLE:
    JE(g, sljit_emit_fop1(g->C, SLJIT_MOV_F64, SLJIT_MEM1(SLJIT_SP), ofs,
                          val_reg, 0));
    return;
  default:
    JE(g, sljit_emit_op1(g->C, int_store_op(ty), SLJIT_MEM1(SLJIT_SP), ofs,
                         val_reg, 0));
    return;
  }
}

// Frame offset of an lvalue that is addressed directly by the local area
// (a non-register local or a member chain rooted at one), or -1.
static int local_lvalue_ofs(Node *node) {
  int ofs = 0;
  while (node->kind == ND_MEMBER) {
    if (node->member->is_bitfield)
      return -1;
    ofs += node->member->offset;
    node = node->lhs;
  }
  if (node->kind != ND_VAR)
    return -1;
  Obj *var = node->var;
  if (!var->is_local || var->jreg > 0)
    return -1;
  return (int)var->ofs + ofs;
}

// Store the value in `val_reg` to the address in `addr_reg`, respecting
// the size of `ty`. For struct/union, val_reg is the source address.
static void store_from(JGen *g, Type *ty, sljit_s32 val_reg, sljit_s32 addr_reg) {
  switch (ty->kind) {
  case TY_STRUCT:
  case TY_UNION: {
    // Inline copy: from val_reg to addr_reg, ty->size bytes. Chunk by machine
    // word width so a 32-bit host copies 4 bytes at a time instead of skipping
    // half of every 8-byte block.
    //
    // The copy temp must not alias either address register, and the
    // caller must have evicted any other live value from R0..R2.
    sljit_s32 tmp = SLJIT_R2;
    if (tmp == val_reg || tmp == addr_reg)
      tmp = (SLJIT_R1 != val_reg && SLJIT_R1 != addr_reg) ? SLJIT_R1 : SLJIT_R0;

    const int word = (int)sizeof(sljit_sw);
    int ofs = 0;
    for (; ofs + word <= ty->size; ofs += word) {
      JE(g, sljit_emit_op1(g->C, SLJIT_MOV, tmp, 0, SLJIT_MEM1(val_reg), ofs));
      JE(g, sljit_emit_op1(g->C, SLJIT_MOV, SLJIT_MEM1(addr_reg), ofs, tmp, 0));
    }
    if (ofs + 4 <= ty->size) {
      JE(g, sljit_emit_op1(g->C, SLJIT_MOV32, tmp, 0, SLJIT_MEM1(val_reg), ofs));
      JE(g, sljit_emit_op1(g->C, SLJIT_MOV32, SLJIT_MEM1(addr_reg), ofs, tmp, 0));
      ofs += 4;
    }
    if (ofs + 2 <= ty->size) {
      JE(g, sljit_emit_op1(g->C, SLJIT_MOV_U16, tmp, 0, SLJIT_MEM1(val_reg), ofs));
      JE(g, sljit_emit_op1(g->C, SLJIT_MOV_U16, SLJIT_MEM1(addr_reg), ofs, tmp, 0));
      ofs += 2;
    }
    if (ofs < ty->size) {
      JE(g, sljit_emit_op1(g->C, SLJIT_MOV_U8, tmp, 0, SLJIT_MEM1(val_reg), ofs));
      JE(g, sljit_emit_op1(g->C, SLJIT_MOV_U8, SLJIT_MEM1(addr_reg), ofs, tmp, 0));
    }
    return;
  }
  case TY_FLOAT:
    JE(g, sljit_emit_fop1(g->C, SLJIT_MOV_F32,
                          SLJIT_MEM1(addr_reg), 0, val_reg, 0));
    return;
  case TY_DOUBLE:
    JE(g, sljit_emit_fop1(g->C, SLJIT_MOV_F64,
                          SLJIT_MEM1(addr_reg), 0, val_reg, 0));
    return;
  default:
    JE(g, sljit_emit_op1(g->C, int_store_op(ty),
                         SLJIT_MEM1(addr_reg), 0, val_reg, 0));
    return;
  }
}

//
// Symbol / address resolution
//

// Address of a function designator into R(r). `var` has function type.
static void func_addr(JGen *g, Obj *var, int r) {
  if (var->jit_addr) {
    JE(g, sljit_emit_op1(g->C, SLJIT_MOV, SLJIT_R(r), 0,
                         SLJIT_IMM, (sljit_sw)var->jit_addr));
    return;
  }
  if (!var->is_static) {
    // External linkage: known symbol, host symbol, or a cell that is
    // filled once the function is compiled (possibly later in this TU).
    WidccRT *rt = g->tu->rt;
    JitSym *s = jit_sym(rt, var->name, true);
    if (s->addr) {
      JE(g, sljit_emit_op1(g->C, SLJIT_MOV, SLJIT_R(r), 0,
                           SLJIT_IMM, (sljit_sw)s->addr));
      return;
    }
    void *host = jit_resolve_host(rt, var->name);
    if (host) {
      s->addr = host;
      JE(g, sljit_emit_op1(g->C, SLJIT_MOV, SLJIT_R(r), 0,
                           SLJIT_IMM, (sljit_sw)host));
      return;
    }
    void **cell = jit_sym_cell(s);
    load_abs(g, cell, r);
    return;
  }
  // Static function: the Obj is shared across the TU, so the address is
  // patched through a per-TU cell keyed by the Obj identity.
  for (JStaticCell *sc = g->tu->static_cells; sc; sc = sc->next) {
    if (sc->fn == var) {
      load_abs(g, sc->cell, r);
      return;
    }
  }
  WidccCompiler *cc = g->tu->cc;
  JStaticCell *sc = rt_alloc_func(cc, sizeof(JStaticCell));
  sc->fn = var;
  sc->cell = rt_alloc_func(cc, sizeof(void *));
  *sc->cell = jit_undefined_fn_addr();
  sc->next = g->tu->static_cells;
  g->tu->static_cells = sc;
  load_abs(g, sc->cell, r);
}

// Address of a global variable into R(r).
static void global_addr(JGen *g, Obj *var, int r) {
  if (var->jit_addr) {
    JE(g, sljit_emit_op1(g->C, SLJIT_MOV, SLJIT_R(r), 0,
                         SLJIT_IMM, (sljit_sw)var->jit_addr));
    return;
  }
  if (var->is_tls)
    error("jitgen: TLS variables are not supported: %s", var->name);
  if (var->is_static)
    error("jitgen: undefined static variable: %s", var->name);

  // Extern: resolve from the runtime table or the host process,
  // otherwise defer through a runtime-owned indirection cell filled at
  // definition time.
  WidccRT *rt = g->tu->rt;
  JitSym *s = jit_sym(rt, var->name, false);
  void *addr = s->addr;
  if (!addr) {
    addr = jit_resolve_host(rt, var->name);
    if (addr)
      s->addr = addr;
  }
  if (addr) {
    JE(g, sljit_emit_op1(g->C, SLJIT_MOV, SLJIT_R(r), 0,
                         SLJIT_IMM, (sljit_sw)addr));
    return;
  }
  void **cell = jit_sym_cell(s);
  load_abs(g, cell, r);
}

//
// Address generation (lvalues)
//
// gen_addr pushes the effective address of an lvalue as an integer-class
// SValue. Consumers that need to dereference it pull it into a real
// register with sv_gv_real.
//

static void gen_addr(JGen *g, Node *node) {
  switch (node->kind) {
  case ND_VAR: {
    Obj *var = node->var;
    if (var->ty->kind == TY_VLA)
      unsupported(node, "variable-length arrays");
    if (var->is_local) {
      if (var->jreg > 0)
        error("jitgen: internal error: address of register-allocated variable");
      int r = sv_new_reg(g, ty_int);
      JE(g, sljit_get_local_base(g->C, SLJIT_R(r), 0, var->ofs));
      sv_push(g, ty_int, SV_REG, r, 0);
      return;
    }
    // The resolved address lands in any free scratch register.
    sv_make_free(g, false, 1, -1, -1);
    int r = sv_pick_free(g, false);
    if (var->ty->kind == TY_FUNC)
      func_addr(g, var, r);
    else
      global_addr(g, var, r);
    sv_push(g, ty_int, SV_REG, r, 0);
    return;
  }
  case ND_DEREF:
    // The pointer value already is the address.
    gen_expr(g, node->lhs);
    return;
  case ND_CHAIN:
  case ND_COMMA:
    gen_expr(g, node->lhs);
    if (node->lhs->ty && node->lhs->ty->kind != TY_VOID)
      sv_pop(g);
    gen_addr(g, node->rhs);
    return;
  case ND_MEMBER: {
    if (node->member->is_bitfield)
      unsupported(node, "bitfields");
    int lofs = local_lvalue_ofs(node);
    if (lofs >= 0) {
      int r = sv_new_reg(g, ty_int);
      JE(g, sljit_get_local_base(g->C, SLJIT_R(r), 0, lofs));
      sv_push(g, ty_int, SV_REG, r, 0);
      return;
    }
    gen_addr(g, node->lhs);
    if (node->member->offset) {
      int r = sv_gv_real(g, g->vtop - 1);
      JE(g, sljit_emit_op2(g->C, SLJIT_ADD, SLJIT_R(r), 0,
                           SLJIT_R(r), 0, SLJIT_IMM, node->member->offset));
    }
    return;
  }
  default:
    break;
  }
  error_tok(node->tok, "not an lvalue");
}

// Load an lvalue. The address becomes the value itself for aggregates and
// emulated i64 values; scalars are loaded through the address register.
static void gen_load_lvalue(JGen *g, Node *node) {
  // Scalars held in the local area are loaded straight from their frame
  // slot; no address register is needed.
  if (!is_aggregate_ty(node->ty) && !is_m32i64_type(node->ty)) {
    int ofs = local_lvalue_ofs(node);
    if (ofs >= 0) {
      if (is_flonum(node->ty)) {
        int fd = sv_new_reg(g, node->ty);
        load_mem_to(g, SLJIT_FR(fd), ofs, node->ty);
        sv_push(g, node->ty, SV_REG, fd, 0);
      } else {
        int r = sv_new_reg(g, node->ty);
        load_mem_to(g, SLJIT_R(r), ofs, node->ty);
        sv_push(g, node->ty, SV_REG, r, 0);
      }
      return;
    }
  }
  // `a[i]` loads with a scaled index operand: [base + i*2^k].
  if (!is_aggregate_ty(node->ty) && !is_m32i64_type(node->ty)) {
    sljit_s32 mem;
    sljit_sw off;
    if (scaled_mem_operand(g, node, &mem, &off)) {
      bool flt = is_flonum(node->ty);
      int bi = g->vtop - 2, xi = g->vtop - 1;
      sv_make_free(g, flt, 1, bi, xi);
      int dst = sv_pick_free(g, flt);
      if (flt)
        JE(g, sljit_emit_fop1(g->C, node->ty->kind == TY_FLOAT ? SLJIT_MOV_F32
                                                              : SLJIT_MOV_F64,
                              SLJIT_FR(dst), 0, mem, off));
      else
        JE(g, sljit_emit_op1(g->C, node->ty->kind == TY_BOOL
                                      ? SLJIT_MOV_U8 : int_load_op(node->ty),
                             SLJIT_R(dst), 0, mem, off));
      sv_pop(g);
      sv_pop(g);
      sv_push(g, node->ty, SV_REG, dst, 0);
      return;
    }
  }
  gen_addr(g, node);
  int idx = g->vtop - 1;
  SValue *v = &g->vstack[idx];
  if (is_m32i64_type(node->ty) || is_aggregate_ty(node->ty)) {
    v->ty = node->ty;
    return;
  }
  if (is_flonum(node->ty)) {
    sljit_s32 base = sv_base_reg(g, idx, -1);
    int fd = sv_new_reg(g, node->ty);
    load_to(g, SLJIT_FR(fd), base, node->ty);
    sv_pop(g);
    sv_push(g, node->ty, SV_REG, fd, 0);
    return;
  }
  if (v->loc == SV_SREG && SREG_ADDR_OK(v->r)) {
    // The pointer lives in a real saved register: load straight from it.
    int dst = sv_new_reg(g, node->ty);
    load_to(g, SLJIT_R(dst), SLJIT_S(v->r), node->ty);
    sv_pop(g);
    sv_push(g, node->ty, SV_REG, dst, 0);
    return;
  }
  int ra = sv_gv_real(g, idx);
  load_to(g, SLJIT_R(ra), SLJIT_R(ra), node->ty);
  v->ty = node->ty;
}

//
// Casts
//

static void load_dbl_zero(JGen *g, sljit_s32 freg) {
  JE(g, sljit_emit_fset64(g->C, freg, 0.0));
}

// R0 (int/ptr) := (R0 != 0)
static void int_to_bool(JGen *g) {
  JE(g, sljit_emit_op2u(g->C, SLJIT_SUB | SLJIT_SET_Z, SLJIT_R0, 0, SLJIT_IMM, 0));
  JE(g, sljit_emit_op_flags(g->C, SLJIT_MOV, SLJIT_R0, 0, SLJIT_NOT_EQUAL));
}

// A cast that leaves the canonical machine representation untouched. Such
// casts must not force their operand into R0 (that would spill neighbours),
// and the value keeps whatever register it already lives in.
static bool cast_is_noop(Type *from, Type *to) {
  if (to->kind == TY_VOID || to->kind == TY_BOOL)
    return false;
  if (is_flonum(from) || is_flonum(to))
    return is_flonum(from) && is_flonum(to) && from->kind == to->kind;
  if (from->kind == TY_BOOL)
    return false;
  if (to->size >= 8)
    return true; // pointers and 64-bit integers are already canonical
  return from->size == to->size && from->is_unsigned == to->is_unsigned;
}

static void cast_to(JGen *g, Type *from, Type *to) {
  if (to->kind == TY_VOID)
    return;

  // Values are kept canonical (sign/zero extended to word width), so many
  // casts need no code at all.
  if (cast_is_noop(from, to))
    return;

  if (to->kind == TY_BOOL) {
    if (is_flonum(from)) {
      sljit_s32 op = from->kind == TY_FLOAT ? SLJIT_CMP_F32 : SLJIT_CMP_F64;
      load_dbl_zero(g, SLJIT_FR1);
      JE(g, sljit_emit_fop1(g->C, op | SLJIT_SET_UNORDERED_OR_NOT_EQUAL,
                            SLJIT_FR0, 0, SLJIT_FR1, 0));
      JE(g, sljit_emit_op_flags(g->C, SLJIT_MOV, SLJIT_R0, 0,
                                SLJIT_UNORDERED_OR_NOT_EQUAL));
    } else {
      int_to_bool(g);
    }
    return;
  }

  if (to->kind == TY_FLOAT || to->kind == TY_DOUBLE) {
    bool to32 = to->kind == TY_FLOAT;
    if (from->kind == TY_FLOAT || from->kind == TY_DOUBLE) {
      bool from32 = from->kind == TY_FLOAT;
      if (to32 == from32)
        return;
      JE(g, sljit_emit_fop1(g->C, to32 ? SLJIT_CONV_F32_FROM_F64
                                       : SLJIT_CONV_F64_FROM_F32,
                            SLJIT_FR0, 0, SLJIT_FR0, 0));
      return;
    }
    // integer -> float; R0 holds the canonical 64-bit value
    sljit_s32 op;
    if (from->is_unsigned || from->kind == TY_PTR)
      op = to32 ? SLJIT_CONV_F32_FROM_UW : SLJIT_CONV_F64_FROM_UW;
    else
      op = to32 ? SLJIT_CONV_F32_FROM_SW : SLJIT_CONV_F64_FROM_SW;
    JE(g, sljit_emit_fop1(g->C, op, SLJIT_FR0, 0, SLJIT_R0, 0));
    return;
  }

  // integer/pointer target
  if (from->kind == TY_FLOAT || from->kind == TY_DOUBLE) {
    bool from32 = from->kind == TY_FLOAT;
    sljit_s32 op = from32 ? SLJIT_CONV_SW_FROM_F32 : SLJIT_CONV_SW_FROM_F64;
    JE(g, sljit_emit_fop1(g->C, op, SLJIT_R0, 0, SLJIT_FR0, 0));
    // fall through to truncation for narrow targets
    if (to->size == 8)
      return;
    extend32_to(g, to->is_unsigned ? ty_uint : ty_int, SLJIT_R0);
    if (to->size < 4)
      JE(g, sljit_emit_op1(g->C, int_load_op(to), SLJIT_R0, 0, SLJIT_R0, 0));
    return;
  }

  if (from->kind == TY_BOOL && to->size > 1) {
    JE(g, sljit_emit_op1(g->C, SLJIT_MOV_U8, SLJIT_R0, 0, SLJIT_R0, 0));
    return;
  }
  if (to->size >= 8)
    return; // pointers and 64-bit integers are already canonical
  if (to->size == 4) {
    extend32_to(g, to, SLJIT_R0);
    return;
  }
  // Truncate to 1 or 2 bytes.
  JE(g, sljit_emit_op1(g->C, int_load_op(to), SLJIT_R0, 0, SLJIT_R0, 0));
}

//
// Comparisons
//

static sljit_s32 int_cond(NodeKind kind, bool is_unsigned) {
  switch (kind) {
  case ND_EQ: return SLJIT_EQUAL;
  case ND_NE: return SLJIT_NOT_EQUAL;
  case ND_LT: return is_unsigned ? SLJIT_LESS : SLJIT_SIG_LESS;
  case ND_LE: return is_unsigned ? SLJIT_LESS_EQUAL : SLJIT_SIG_LESS_EQUAL;
  case ND_GT: return is_unsigned ? SLJIT_GREATER : SLJIT_SIG_GREATER;
  case ND_GE: return is_unsigned ? SLJIT_GREATER_EQUAL : SLJIT_SIG_GREATER_EQUAL;
  default: return 0;
  }
}

// sljit encodes "these flags were set" with an even condition type: the odd
// counterpart (e.g. GREATER_EQUAL) is only meaningful at the flag-reading
// site (sljit_emit_op_flags / _emit_jump). Passing SLJIT_SET(odd) would put
// a bit into position 10, which is outside VARIABLE_FLAG_MASK and trips the
// opcode validator. Canonicalize to the even type.
static sljit_s32 set_flag_for(sljit_s32 cond) {
  if (cond <= SLJIT_NOT_EQUAL)
    return SLJIT_SET_Z;
  return SLJIT_SET(cond & ~1);
}

static sljit_s32 flt_cond(NodeKind kind) {
  switch (kind) {
  case ND_EQ: return SLJIT_ORDERED_EQUAL;
  case ND_NE: return SLJIT_UNORDERED_OR_NOT_EQUAL;
  case ND_LT: return SLJIT_ORDERED_LESS;
  case ND_LE: return SLJIT_ORDERED_LESS_EQUAL;
  case ND_GT: return SLJIT_ORDERED_GREATER;
  case ND_GE: return SLJIT_ORDERED_GREATER_EQUAL;
  default: return 0;
  }
}

// lhs in lhs_reg (int) / FR(lhs_reg_idx) (float), rhs in rhs_reg / FR(rhs_reg_idx).
// Result 0/1 is written into `dst`. `dst` may alias `lhs_reg`: the flags op
// only reads its operands before op_flags overwrites them.
static void gen_cmp(JGen *g, sljit_s32 dst, sljit_s32 lhs_reg, sljit_s32 rhs,
                    sljit_sw rhsw, Node *node) {
  Type *ty = node->lhs->ty;
  if (is_flonum(ty)) {
    sljit_s32 op = ty->kind == TY_FLOAT ? SLJIT_CMP_F32 : SLJIT_CMP_F64;
    sljit_s32 cond = flt_cond(node->kind);
    JE(g, sljit_emit_fop1(g->C, op | set_flag_for(cond),
                          lhs_reg, 0, rhs, rhsw));
    JE(g, sljit_emit_op_flags(g->C, SLJIT_MOV, dst, 0, cond));
    return;
  }
  bool u = ty->is_unsigned || ty->kind == TY_PTR;
  sljit_s32 cond = int_cond(node->kind, u);
  sljit_s32 op = SLJIT_SUB | set_flag_for(cond);
  if (ty->size == 4)
    op |= SLJIT_32;
  JE(g, sljit_emit_op2u(g->C, op, lhs_reg, 0, rhs, rhsw));
  JE(g, sljit_emit_op_flags(g->C, SLJIT_MOV, dst, 0, cond));
}

// Integer comparison with an immediate right operand. Same result contract
// as gen_cmp (0/1 written to `dst`), but avoids materializing the literal
// into a pool slot or temporary register.
static void gen_cmp_imm(JGen *g, sljit_s32 dst, sljit_s32 lhs_reg, sljit_sw imm,
                        Node *node) {
  Type *ty = node->lhs->ty;
  bool u = ty->is_unsigned || ty->kind == TY_PTR;
  sljit_s32 cond = int_cond(node->kind, u);
  sljit_s32 op = SLJIT_SUB | set_flag_for(cond);
  if (ty->size == 4)
    op |= SLJIT_32;
  JE(g, sljit_emit_op2u(g->C, op, lhs_reg, 0, SLJIT_IMM, imm));
  JE(g, sljit_emit_op_flags(g->C, SLJIT_MOV, dst, 0, cond));
}

// Signed 32-bit arithmetic may run word-wide: canonical operands are
// sign-extended, so whenever the C-level result is defined (signed overflow
// is UB) the 64-bit result already equals the sign-extended 32-bit one.
// Unsigned 32-bit ops must keep the SLJIT_32 prefix for wrap semantics.
static bool binop_widen_signed(int kind) {
  switch (kind) {
  case ND_ADD: case ND_SUB: case ND_MUL:
  case ND_BITAND: case ND_BITOR: case ND_BITXOR:
  case ND_SHL: case ND_SAR:
    return true;
  default:
    return false;
  }
}

// Integer binary operator → sljit opcode. Returns -1 for unknown kinds.
static sljit_s32 binop_op(int kind, sljit_s32 op32) {
  switch (kind) {
  case ND_ADD:    return SLJIT_ADD | op32;
  case ND_SUB:    return SLJIT_SUB | op32;
  case ND_MUL:    return SLJIT_MUL | op32;
  case ND_BITAND: return SLJIT_AND | op32;
  case ND_BITOR:  return SLJIT_OR | op32;
  case ND_BITXOR: return SLJIT_XOR | op32;
  case ND_SHL:    return SLJIT_SHL | op32;
  case ND_SHR:    return SLJIT_LSHR | op32;
  case ND_SAR:    return SLJIT_ASHR | op32;
  default:        return -1;
  }
}

// Integer divide/modulo. Operands must already be in R0 (dividend) and
// R1 (divisor), matching sljit's DIVMOD hardware contract. The quotient
// (DIV) or remainder (MOD) is left in R0 for the caller to push.
static void emit_divmod(JGen *g, Node *node, Type *ty, bool is32) {
  sljit_s32 op;
  if (ty->is_unsigned)
    op = is32 ? SLJIT_DIVMOD_U32 : SLJIT_DIVMOD_UW;
  else
    op = is32 ? SLJIT_DIVMOD_S32 : SLJIT_DIVMOD_SW;
  JE(g, sljit_emit_op0(g->C, op));
  if (node->kind == ND_MOD)
    JE(g, sljit_emit_op1(g->C, SLJIT_MOV, SLJIT_R0, 0, SLJIT_R1, 0));
  if (is32) extend32_to(g, ty, SLJIT_R0);
}

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

static void gen_funcall(JGen *g, Node *node) {
  Type *fty = node->lhs->ty;
  if (fty->kind == TY_PTR)
    fty = fty->base;
  if (fty->kind != TY_FUNC)
    error_tok(node->tok, "callee is not a function");
  if (node->lhs->kind == ND_VAR && !strcmp(node->lhs->var->name, "alloca"))
    unsupported(node, "alloca");
  if (fty->is_variadic)
    unsupported(node, "calls to variadic functions");
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
  if (nint > JMAX_INT_ARGS || nflt > JMAX_FLT_ARGS || narg > JMAX_ARGS)
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
    } else if (callee->loc == SV_REG && callee->r >= nint) {
      // The high scratch register is never an argument destination.
      call_src = SLJIT_R(callee->r);
    } else {
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

  sljit_s32 at = SLJIT_ARG_RETURN(arg_type_of(node->ty));
  if (node->ty->kind == TY_VOID)
    at = SLJIT_ARG_RETURN(SLJIT_ARG_TYPE_RET_VOID);
  {
    int pos = 1;
    for (Obj *a = node->args; a; a = a->param_next, pos++)
      at |= SLJIT_ARG_VALUE(arg_type_of(a->arg_expr->ty), pos);
  }

  schedule_arg_moves(g, base, node->args, true);
  schedule_arg_moves(g, base, node->args, false);

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
    if (ty->is_unsigned) {
      JE(g, sljit_emit_op2(g->C, SLJIT_LSHR | op32, SLJIT_R(r), 0,
                           SLJIT_R(r), 0, SLJIT_IMM, k));
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
  case ND_VA_COPY:
  case ND_VA_ARG:
    unsupported(node, "va_list/va_arg");
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
    if (node->cond && !rotate) {
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
      sljit_set_label(jback, lbody);
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
// written most often rather than to the first ones declared.

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
    if (v->jreg == 0 && !is_flonum(v->ty) && reg_candidate(v) && n < cap) {
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
    if (v->jreg == 0 && !is_flonum(v->ty) && reg_candidate(v))
      n++;
  for (Scope *s = sc->children; s; s = s->sibling_next)
    n += count_int_candidates(s);
  return n;
}

static int lay_out_locals(Scope *sc, int top, JRegAlloc *ra,
                          int int_budget, int flt_budget) {
  for (Obj *var = sc->locals; var; var = var->next) {
    if (var->jreg > 0)
      continue;
    Type *ty = var->ty;
    if (ty->kind == TY_VLA || ty->size < 0)
      error("jitgen: variable-length arrays are not supported");
    if (reg_candidate(var)) {
      bool flt = is_flonum(ty);
      int n = alloc_bit(flt ? &ra->used_flt : &ra->used_int,
                        flt ? flt_budget : int_budget);
      if (n >= 0) {
        var->jreg = n + 1;
        if (flt)
          ra->max_flt = MAX(ra->max_flt, n + 1);
        else
          ra->max_int = MAX(ra->max_int, n + 1);
        continue;
      }
    }
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

  // Move incoming parameters from the arrival registers S(ii)/FR(fi) into
  // their saved register or local slot.
  int ii = 0, fi = 0;
  for (Obj *p = fty->param_list; p; p = p->param_next) {
    Type *t = p->ty;
    if (is_flonum(t)) {
      sljit_s32 op = t->kind == TY_FLOAT ? SLJIT_MOV_F32 : SLJIT_MOV_F64;
      if (p->jreg > 0)
        JE(g, sljit_emit_fop1(g->C, op, SLJIT_FS(p->jreg - 1), 0, SLJIT_FR(fi), 0));
      else
        JE(g, sljit_emit_fop1(g->C, op, SLJIT_MEM1(SLJIT_SP), p->ofs, SLJIT_FR(fi), 0));
      fi++;
    } else {
      if (p->jreg > 0) {
        if (p->jreg - 1 != ii)
          JE(g, sljit_emit_op1(g->C, SLJIT_MOV, SLJIT_S(p->jreg - 1), 0,
                               SLJIT_S(ii), 0));
        // The ABI leaves the high bits of word-size arguments unspecified;
        // re-extend so every SValue is canonical for its type.
        if (t->size < 8)
          JE(g, sljit_emit_op1(g->C, int_load_op(t), SLJIT_S(p->jreg - 1), 0,
                               SLJIT_S(p->jreg - 1), 0));
      } else {
        JE(g, sljit_emit_op1(g->C, int_store_op(t),
                             SLJIT_MEM1(SLJIT_SP), p->ofs, SLJIT_S(ii), 0));
      }
      ii++;
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
  if (fty->is_variadic)
    error("jitgen: variadic function is not supported: %s", fn->name);
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

  sljit_s32 at = SLJIT_ARG_RETURN(arg_type_of(fty->return_ty));
  if (fty->return_ty->kind == TY_VOID)
    at = SLJIT_ARG_RETURN(SLJIT_ARG_TYPE_RET_VOID);

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
    at |= SLJIT_ARG_VALUE(arg_type_of(t), narg);
  }
  if (nint > JMAX_INT_ARGS || nflt > JMAX_FLT_ARGS || narg > JMAX_ARGS)
    error("jitgen: too many parameters: %s", fn->name);

  JGen g;
  memset(&g, 0, sizeof(g));
  g.tu = tu;
  g.fn = fn;
  mark_stmt_addrs(fn->body);

  // --- First pass: pin parameters to their arrival registers --------------
  // sljit delivers integer parameters in the first nint saved registers and
  // float parameters in the first nflt float saved registers, so keeping a
  // parameter there costs no entry move. Measured on the quicksort benchmark,
  // pinning beats ranking params like ordinary locals (fewer cycles despite
  // slightly more instructions).
  JRegAlloc ra = {0};
  {
    int ii = 0;
    for (Obj *p = fty->param_list; p; p = p->param_next) {
      if (is_flonum(p->ty))
        continue;
      if (reg_candidate(p)) {
        p->jreg = ii + 1;
        ra.used_int |= 1u << ii;
        ra.max_int = MAX(ra.max_int, ii + 1);
      }
      ii++;
    }
  }
  {
    int fi = 0;
    for (Obj *p = fty->param_list; p; p = p->param_next) {
      if (is_flonum(p->ty)) {
        if (reg_candidate(p)) {
          p->jreg = fi + 1;
          ra.used_flt |= 1u << fi;
          ra.max_flt = MAX(ra.max_flt, fi + 1);
        }
        fi++;
      }
    }
  }

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
      for (int i = 1; i < n; i++) {
        VarUse key = vu[i];
        int j = i - 1;
        while (j >= 0 && (vu[j].uses < key.uses ||
                          (vu[j].uses == key.uses && vu[j].total > key.total) ||
                          (vu[j].uses == key.uses && vu[j].total == key.total &&
                           vu[j].order > key.order))) {
          vu[j + 1] = vu[j];
          j--;
        }
        vu[j + 1] = key;
      }
      for (int i = 0; i < n; i++) {
        Obj *v = vu[i].var;
        if (v->jreg)
          continue; // parameter: stays in its arrival register
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
    int need_saved_int = MAX(nint, ra.max_int);
    g.saved_int = MIN(need_saved_int, SLJIT_NUMBER_OF_SAVED_REGISTERS);
    g.saved_int = MAX(g.saved_int, 2); // always at least one scratch pair
    g.saved_int = MIN(g.saved_int, SLJIT_NUMBER_OF_REGISTERS - 4);
    g.saved_flt = MIN(ra.max_flt, SLJIT_NUMBER_OF_SAVED_FLOAT_REGISTERS);
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
    sljit_s32 probe = align_to(m.m32i64_base + m.m32i64_cap * 8, 16);
    emit_function_body(&m, fty, at, probe);
    sljit_free_compiler(m.C);
    tu->cc->active_compiler = NULL;
    hashmap_destroy(&m.flabels);
    g.spill_max = m.spill_max;
    g.m32i64_max = m.m32i64_max;
  }

  // --- Exact frame layout --------------------------------------------------
#if WIDCC_HOST_64BIT
  // Native 64-bit integers need no scratch area.
  g.m32i64_base = 0;
  sljit_s32 local_size = align_to(g.eval_base + g.spill_max * 8, 16);
#else
  // 8-byte temporaries live above the home-slot area.
  g.m32i64_base = align_to(g.eval_base + g.spill_max * 8, 8);
  sljit_s32 local_size = align_to(g.m32i64_base + g.m32i64_max * 8, 16);
#endif
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


#undef WIDCC_HOST_64BIT
#undef JE
#undef REAL_INT_REGS
#undef SREG_ADDR_OK
#endif // WIDCC_JITGEN_C
