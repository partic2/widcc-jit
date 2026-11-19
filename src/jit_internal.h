// Internal interfaces shared by the JIT runtime (jit.c) and the sljit
// backend (jitgen.c).
#ifndef JIT_INTERNAL_H
#define JIT_INTERNAL_H

#include "widcc_jit.h"

// Forward: Obj lives in widcc.h; we only use it through opaque pointers
// here, while the backend includes widcc.h before this file.
struct Obj;
typedef struct Obj Obj;

// Runtime-owned heap blocks. `func_aux` holds scratch referenced by
// generated functions (per-TU static call cells); `data` holds global
// data blocks. They are released by widcc_rt_free_funcs /
// widcc_rt_free_vars respectively.
typedef struct JitMallocBlock JitMallocBlock;
struct JitMallocBlock {
  JitMallocBlock *next;
  void *ptr;
};

// Runtime-owned symbol entry. Everything in here is malloc'd by the
// runtime, so entries outlive the compiler that created them.
typedef struct JitSym JitSym;
struct JitSym {
  JitSym *next;         // all runtime symbols, newest first
  char *name;           // runtime-owned copy
  void *addr;           // resolved address or NULL
  void **cell;          // lazily created indirection cell (forward refs)
  bool is_tentative;    // data symbol defined without initializer (common)
  bool is_func;         // global function symbol vs global variable symbol
};

typedef struct JitCode JitCode;
struct JitCode {
  JitCode *next;
  void *code;           // sljit_generate_code result, freed via sljit_free_code
};

struct WidccRT {
  HashMap syms;                 // char *name -> JitSym * (heap-mode map)
  JitSym *symlist;              // all symbols, newest first
  JitCode *codes;               // generated machine code blocks
  JitMallocBlock *func_aux;     // runtime-owned scratch used by functions
  JitMallocBlock *data;         // generated global data blocks
};

struct WidccCompiler {
  CCCtx cc;                     // frontend context + compile-time arena
  WidccRT *rt;                  // borrowed; not owned by the compiler
  struct sljit_compiler *active_compiler; // for error-path cleanup
};

// The runtime is built as a single translation unit (src/all.c); every
// symbol declared here has internal linkage.

// Plain runtime-heap allocations (symbol entries, names, cells, code
// nodes). OOM reports through error(): longjmp while a compile armed the
// handler, process exit otherwise.
static void *rt_alloc(size_t sz);
static char *rt_strdup(const char *s);

// Tracked runtime-heap allocations, zero-filled on allocation.
// rt_alloc_func blocks are freed by widcc_rt_free_funcs; rt_alloc_data
// blocks by widcc_rt_free_vars.
static void *rt_alloc_func(WidccRT *rt, size_t sz);
static void *rt_alloc_data(WidccRT *rt, size_t sz);
static void rt_free_blocks(JitMallocBlock **list);

// Lazily materialize the runtime-owned indirection cell for a symbol.
static void **rt_cell_alloc(void);

// Look up (or create) a runtime symbol entry. `is_func` classifies the
// symbol for the per-kind release functions; an existing entry keeps its
// function flag once set.
static JitSym *jit_sym(WidccRT *rt, const char *name, bool is_func);
static void **jit_sym_cell(JitSym *s);


// Address of the trap executed when JIT code calls an unresolved function.
static void *jit_undefined_fn_addr(void);

// Generate machine code for a whole parsed translation unit.
static void jitgen_tu(WidccCompiler *cc, Obj *prog);

void *(*jit_host_resolver)(WidccRT *rt, const char *name)=widcc_rt_default_host_resolver;

#endif
