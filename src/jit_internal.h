// Internal interfaces shared by the JIT runtime (jit.c) and the sljit
// backend (jitgen.c).
#ifndef JIT_INTERNAL_H
#define JIT_INTERNAL_H

#include "widcc_jit.h"

// Forward: Obj lives in widcc.h; we only use it through opaque pointers
// here, while the backend includes widcc.h before this file.
struct Obj;
typedef struct Obj Obj;

// Runtime-owned heap blocks. A loaded module tracks the scratch used by
// its functions (static call cells) and its global data blocks; both
// lists are released by widcc_rt_unload_code.
typedef struct JitMallocBlock JitMallocBlock;
struct JitMallocBlock {
  JitMallocBlock *next;
  void *ptr;
};

// Runtime-owned symbol entry. Everything in here is malloc'd by the
// runtime, so entries outlive the module that defined them.
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

// One loaded translation unit. Owns every allocation made while
// compiling it.
struct WidccCode {
  WidccCode *next;              // loaded modules of the runtime, newest first
  JitCode *codes;               // generated machine code blocks
  JitMallocBlock *func_aux;     // static call cells used by this module
  JitMallocBlock *data;         // generated global data blocks
};

struct WidccRT {
  HashMap syms;                 // char *name -> JitSym * (heap-mode map)
  JitSym *symlist;              // all symbols, newest first
  WidccCode *modules;           // loaded modules, newest first
  char err_msg[2048];           // last load error (see widcc_rt_load_code)
  // Default #include search list; runtime-owned copies.
  char **inc_paths;
  int inc_count;
  // Optional per-runtime host resolver; NULL means the default
  // process-wide lookup.
  void *(*host_resolve)(WidccRT *rt, const char *name);
};

// Compiler state for one load. Never exposed publicly; created and
// destroyed by widcc_rt_load_code.
typedef struct WidccCompiler WidccCompiler;
struct WidccCompiler {
  CCCtx cc;                     // frontend context + compile-time arena
  WidccRT *rt;                  // borrowed; not owned by the compiler
  WidccCode *mod;               // module receiving code/data allocations
  struct sljit_compiler *active_compiler; // for error-path cleanup
};

// The runtime is built as a single translation unit (src/all.c); every
// symbol declared here has internal linkage.

// Plain runtime-heap allocations (symbol entries, names, cells, code
// nodes, modules). OOM reports through error(): longjmp while a compile
// armed the handler, process exit otherwise.
static void *rt_alloc(size_t sz);
static char *rt_strdup(const char *s);

// Tracked runtime-heap allocations, zero-filled on allocation and owned
// by the compiler's current module. Released when the module is unloaded.
static void *rt_alloc_func(WidccCompiler *cc, size_t sz);
static void *rt_alloc_data(WidccCompiler *cc, size_t sz);
static void rt_free_blocks(JitMallocBlock **list);

// Lazily materialize the runtime-owned indirection cell for a symbol.
static void **rt_cell_alloc(void);

// Look up (or create) a runtime symbol entry. An existing entry keeps its
// function flag once set.
static JitSym *jit_sym(WidccRT *rt, const char *name, bool is_func);
static void **jit_sym_cell(JitSym *s);
static void *jit_resolve_host(WidccRT *rt, const char *name);


// Address of the trap executed when JIT code calls an unresolved function.
static void *jit_undefined_fn_addr(void);

// Generate machine code for a whole parsed translation unit into cc->mod.
static void jitgen_tu(WidccCompiler *cc, Obj *prog);

#endif