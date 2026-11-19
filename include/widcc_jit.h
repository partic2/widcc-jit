// widcc JIT public API: a Runtime half and a Compiler half.
//
// Runtime (widcc_rt_*)
//   A runtime owns a symbol map from name to symbol information. Symbols
//   are either global functions or global variables. Entries are added by
//   the host (widcc_rt_add_func / widcc_rt_add_var) or registered
//   automatically when a compiler produces definitions. The runtime owns
//   everything produced by compilation that generated code references —
//   machine code pages and global data blocks — and each symbol class has
//   its own release function:
//
//     widcc_rt_free_funcs()  releases generated code and function entries
//     widcc_rt_free_vars()   releases generated data and variable entries
//
//   A runtime is independent of any compiler: symbols registered in it
//   remain valid after the compiler that produced them is destroyed.
//
// Compiler (widcc_cc_*)
//   A compiler compiles C source into the runtime it was created with.
//   Extern symbols are resolved through that runtime (then through the
//   host process); definitions it produces are added to the runtime's
//   symbol map. The compiler owns only its own compile-time memory
//   (tokens, AST, types). widcc_cc_free() releases that memory without
//   touching the runtime.
//
// Thread-safety: a runtime and its compilers must not be used from
// multiple threads concurrently.
#ifndef WIDCC_JIT_H
#define WIDCC_JIT_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct WidccRT WidccRT;
typedef struct WidccCompiler WidccCompiler;

// ========== Runtime API ==========

// Create / destroy a runtime. Both are NULL-safe on free.
// Returns NULL on allocation failure.
WidccRT *widcc_rt_new(void);
void widcc_rt_free(WidccRT *rt);

// Register a host-provided global function or variable under `name`.
// The runtime stores the address and never frees it. Registration
// overrides automatic host-process resolution. A later compiled
// definition of the same name may replace the registered address.
// Returns 0 on success, -1 on invalid arguments.
int widcc_rt_add_func(WidccRT *rt, const char *name, void *fn);
int widcc_rt_add_var(WidccRT *rt, const char *name, void *var);

// Address of a symbol (function entry point or variable block).
// Returns NULL if the name is not in the runtime map (or unresolved).
void *widcc_rt_lookup(WidccRT *rt, const char *name);

// Copy up to `bufsize` symbol names into `names` (newest first) and
// return the total number of symbols in the runtime.
int widcc_rt_list_symbols(WidccRT *rt, char **names, int bufsize);

// Get the total number of symbols in the runtime.
int widcc_rt_get_symbols_count(WidccRT *rt);



void widcc_rt_set_host_resolver(void * (*resolve)(WidccRT *rt, const char *name));

void *widcc_rt_default_host_resolver(WidccRT *rt, const char *name);

// Release generated function code and drop all function entries
// (including host-registered ones). Previously obtained function
// pointers become invalid. Variables stay accessible.
void widcc_rt_free_funcs(WidccRT *rt);

// Release generated global data blocks and drop all variable entries
// (including host-registered ones). Must be called after
// widcc_rt_free_funcs() — generated code references these blocks.
void widcc_rt_free_vars(WidccRT *rt);

// ========== Compiler API ==========

// Create a compiler bound to `rt`. Returns NULL if `rt` is NULL or on
// allocation failure.
WidccCompiler *widcc_cc_new(WidccRT *rt);

// Destroy a compiler and its compile-time memory. Symbols previously
// produced by it in the runtime are unaffected.
void widcc_cc_free(WidccCompiler *cc);

// Compile a translation unit from a NUL-terminated C source string.
// `name` is used in diagnostics. Symbols registered in the compiler's
// runtime are visible to the unit; unresolved externs are then looked
// up in the host process (dlsym on Unix, exported symbols on Windows).
// Definitions produced by the unit are added to the runtime.
// Returns 0 on success, -1 on error (see widcc_cc_error()).
int widcc_cc_compile(WidccCompiler *cc, const char *name, const char *source);

// The last error message of this compiler.
const char *widcc_cc_error(WidccCompiler *cc);

#ifdef __cplusplus
}
#endif

#endif
