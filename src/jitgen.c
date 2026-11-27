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
// Not supported yet (reported as compile errors): VLAs, alloca, bitfields,
// long double, struct arguments or returns passed by value, computed goto,
// inline asm, TLS.
//
// This file holds the shared types, the value stack / register allocator,
// and the load, store, address and cast primitives. The rest of the backend
// lives in jitgen_emit.c (low-level emitters and calls), jitgen_expr.c,
// jitgen_stmt.c and jitgen_func.c, which are included at the end so the
// whole backend is still one translation unit.
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
// matching x86-64 SysV and MinGW/x64. These limits apply to the native C
// ABI only; functions with more than JMAX_ARGS parameters (and all variadic
// functions) use the Block ABI instead, which passes a pointer to an argument
// block plus the argument count and therefore never exceeds sljit's
// four-argument limit.
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

  // Block ABI: functions with more than JMAX_ARGS parameters, and all
  // variadic functions, are compiled to take a pointer to an argument block
  // plus the total argument count (see "Block ABI" below). sljit then never
  // sees more than two arguments.
  bool block_abi;        // this function uses the block ABI
  int argblock_base;     // caller: local-area offset of its argument block
  int argblock_max;      // caller: peak block size in bytes

  // Variadic entry points keep the incoming block pointer in this frame slot
  // so va_start can locate the first unnamed argument long after the
  // prologue has clobbered the arrival register. -1 when not reserved.
  int va_block_ofs;

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


// The rest of the backend is split across the files below to keep each one
// readable. They are included here, in order, so the backend remains a single
// translation unit (see all.c): every symbol stays static and the include
// order is exactly the original single-file layout.
#include "jitgen_emit.c"
#include "jitgen_expr.c"
#include "jitgen_stmt.c"
#include "jitgen_func.c"

#undef WIDCC_HOST_64BIT
#undef JE
#undef REAL_INT_REGS
#undef SREG_ADDR_OK
#endif // WIDCC_JITGEN_C
