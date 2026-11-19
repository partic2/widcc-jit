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

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct WidccRT WidccRT;
typedef struct WidccCompiler WidccCompiler;

// ========== File I/O hooks ==========

// Callbacks the frontend uses to read source files and probe paths.
// Every field is optional: NULL keeps the default, which uses stdio and
// stat() on the host filesystem. Override them to compile from a virtual
// file system. `open` returns an opaque handle (owned by the callbacks)
// or NULL if the path cannot be opened; `read` fills `buf` with up to
// `size` bytes and returns the byte count, 0 at end of file; `exists`
// returns nonzero if the path exists. The hooks are used for #include
// resolution and by widcc_cc_compile_file().
typedef struct WidccFileOps {
  void *(*open)(const char *path);
  size_t (*read)(void *file, void *buf, size_t size);
  void (*close)(void *file);
  int (*exists)(const char *path);
} WidccFileOps;

// Replace the file access hooks. The table is copied; pass NULL to
// restore the host defaults. Process-global and not thread-safe: set it
// before starting compilations.
void widcc_set_file_ops(const WidccFileOps *ops);

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
// return the total number of symbols in the runtime. Pass names=NULL to
// just count.
int widcc_rt_list_symbols(WidccRT *rt, char **names, int bufsize);

// Set the default directory search list for #include used by later
// compilations of this runtime. The paths are copied. A quoted
// #include "..." first searches the including file's directory and then
// this list; #include <...> searches only this list. Pass NULL with
// count 0 to clear it. Returns 0 on success, -1 on invalid arguments or
// allocation failure.
int widcc_rt_set_include_paths(WidccRT *rt, const char *const *paths,
                               int count);

// Replace how this runtime resolves names that are missing from its
// symbol map. The default looks the name up in the host process (dlsym
// on Unix, exported modules on Windows); callers can chain to it with
// widcc_rt_default_host_resolver(). Pass NULL to restore the default.
// The resolver applies to later compilations of this runtime.
void widcc_rt_set_host_resolver(WidccRT *rt,
                                void *(*resolve)(WidccRT *, const char *));

// The default resolver: runtime symbols first, then the host process.
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
// `path` names the unit: it is used in diagnostics and as the base
// directory for quoted #include directives, but the file is not read
// (the string is the file's contents). Symbols registered in the
// compiler's runtime are visible to the unit; unresolved externs are
// then looked up in the host process (dlsym on Unix, exported symbols on
// Windows). Definitions produced by the unit are added to the runtime.
// Returns 0 on success, -1 on error (see widcc_cc_error()).
int widcc_cc_compile(WidccCompiler *cc, const char *path, const char *source);

// Compile a translation unit read from `path` through the file hooks
// (the host filesystem by default). The path is used in diagnostics and
// as the base directory for quoted #include directives. Returns 0 on
// success, -1 on error (unreadable file, syntax error, ...; see
// widcc_cc_error()).
int widcc_cc_compile_file(WidccCompiler *cc, const char *path);

// The last error message of this compiler.
const char *widcc_cc_error(WidccCompiler *cc);

#ifdef __cplusplus
}
#endif

#endif
