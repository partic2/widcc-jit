// Internal interfaces shared by the JIT runtime (jit.c) and the sljit
// backend (jitgen.c).
#ifndef JIT_INTERNAL_H
#define JIT_INTERNAL_H

#include "widcc_jit.h"

// Forward: Obj lives in widcc.h; we only use it through opaque pointers
// here, while the backend includes widcc.h before this file.
struct Obj;
typedef struct Obj Obj;

// Forward: WidccNamespace lives in widcc_jit.h; jit.c owns the implementation.
struct WidccNamespace;
typedef struct WidccNamespace WidccNamespace;

// Doubly-linked block for runtime-owned allocations so they can be released
// together with sljit code before the arena is torn down.
typedef struct JitMallocBlock JitMallocBlock;
struct JitMallocBlock {
  JitMallocBlock *next;
  void *ptr;
};

typedef struct JitSym JitSym;
struct JitSym {
  JitSym *next;         // instance symbol list (extern linkage only)
  char *name;           // arena-owned
  void *addr;           // resolved address or NULL
  void **cell;          // pending indirection cell for forward function refs
  bool is_tentative;    // data symbol defined without initializer (common)
};

typedef struct JitCode JitCode;
struct JitCode {
  JitCode *next;
  void *code;           // sljit_generate_code result, freed via sljit_free_code
};

struct WidccJIT {
  CCCtx cc;                      // frontend context + compile-time arena
  HashMap syms;                  // char *name -> JitSym * (extern linkage)
  JitSym *symlist;
  JitCode *codes;                // machine code blocks
  JitMallocBlock *rt_blocks;     // runtime-owned data/cell blocks
  struct sljit_compiler *active_compiler;

  // Attached namespace (weak reference — callers own the object).
  WidccNamespace *ns;
  int sync_ns;                   // if non-zero, push extern defs back to ns
};

// The runtime is built as a single translation unit (src/all.c); every
// symbol declared here has internal linkage.

// Ask the attached namespace (if sync_ns is set) to record a newly
// defined extern symbol. Called from jitgen_tu after each function
// compile and each data definition in emit_data.
static void jit_ns_push(WidccJIT *jit, const char *name, void *addr);

// Runtime-heap helpers (malloc + chain). JIT-owned memory that must outlive
// the frontend arena goes through these instead of cc_malloc/cc_calloc.
static void *jit_malloc(WidccJIT *jit, size_t sz);
static void *jit_calloc(WidccJIT *jit, size_t n, size_t sz);

// Release all runtime-owned data/cell blocks. Called after sljit_free_code
// so that machine code — which references these blocks via absolute
// addresses — is no longer running.
static void jit_free_rt(WidccJIT *jit);

// Look up (or create) an external-linkage symbol table entry.
// JitSym itself and its `name` are arena-owned; `cell` is lazily created
// runtime-owned because generated code must keep the indirection cell
// alive across arena resets.
static JitSym *jit_sym(WidccJIT *jit, char *name);

// Lazily materialize the runtime-owned indirection cell for a symbol.
// Generates the cell on first call; subsequent calls return the same ptr.
static void **jit_sym_cell(JitSym *s, WidccJIT *jit);

// Resolve a host symbol through the instance table or the host process.
static void *jit_host_resolve(WidccJIT *jit, char *name);

// Address of the trap executed when JIT code calls an unresolved function.
static void *jit_undefined_fn_addr(void);

// Generate machine code for a whole parsed translation unit.
static void jitgen_tu(WidccJIT *jit, Obj *prog);

#endif
