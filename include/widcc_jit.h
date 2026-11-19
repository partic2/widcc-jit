// widcc JIT public API: a runtime that owns symbols and loaded modules.
//
// Runtime (widcc_rt_*)
//   A runtime owns a symbol map from name to symbol information (global
//   functions and global variables) and the set of loaded code modules.
//   Symbols are added by the host (widcc_rt_add_func / widcc_rt_add_var)
//   or registered automatically when a module defines them. External
//   symbols are resolved through the runtime, then through the host
//   process.
//
// Load API (widcc_rt_load_* / widcc_rt_unload_code)
//   A load compiles one translation unit into a module handle that owns
//   the generated machine code, global data blocks and static call cells
//   produced by that unit. Release a module with widcc_rt_unload_code(),
//   or release all remaining modules with widcc_rt_free().
//
//   Loading follows the dynamic-loader model: a definition in a newly
//   loaded module overrides earlier definitions of the same name, and
//   unloading never restores them. A symbol keeps its last definition
//   even after the module that produced it is unloaded; calling through
//   such a symbol is undefined, as with dlclose().
//
// Error reporting: operations that can fail return NULL on success and a
// message string on failure. The constructor widcc_rt_new() returns NULL
// on failure. Messages from load calls stay valid until the next call on
// that runtime; all other messages are static.
//
// Thread-safety: a runtime and its modules must not be used from multiple
// threads concurrently.
#ifndef WIDCC_JIT_H
#define WIDCC_JIT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct WidccRT WidccRT;
typedef struct WidccCode WidccCode;

// ========== File I/O hooks ==========

// Callbacks the frontend uses to read source files and probe paths.
// Every field is optional: NULL keeps the default, which uses stdio and
// stat() on the host filesystem. Override them to compile from a virtual
// file system. `open` returns an opaque handle (owned by the callbacks)
// or NULL if the path cannot be opened; `read` fills `buf` with up to
// `size` bytes and returns the byte count, 0 at end of file; `exists`
// returns nonzero if the path exists. The hooks are used for #include
// resolution and by widcc_rt_load_code_file().
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

// Create / destroy a runtime. widcc_rt_free() is NULL-safe and releases
// every module still loaded as well as all symbols.
// Returns NULL on allocation failure.
WidccRT *widcc_rt_new(void);
void widcc_rt_free(WidccRT *rt);

// Register a host-provided global function or variable under `name`.
// The runtime stores the address and never frees it. Registration
// overrides automatic host-process resolution. A later loaded
// definition of the same name may replace the registered address.
// Returns NULL on success, or a static error message on invalid
// arguments.
const char *widcc_rt_add_func(WidccRT *rt, const char *name, void *fn);
const char *widcc_rt_add_var(WidccRT *rt, const char *name, void *var);

// Address of a symbol (function entry point or variable block).
// Returns NULL if the name is not in the runtime map (or unresolved).
void *widcc_rt_lookup(WidccRT *rt, const char *name);

// Copy up to `bufsize` symbol names into `names` (newest first) and
// return the total number of symbols in the runtime. Pass names=NULL to
// just count.
int widcc_rt_list_symbols(WidccRT *rt, char **names, int bufsize);

// Set the default directory search list for #include used by later
// loads of this runtime. The paths are copied. A quoted
// #include "..." first searches the including file's directory and then
// this list; #include <...> searches only this list. Pass NULL with
// count 0 to clear it. Returns NULL on success, or a static error
// message on invalid arguments or allocation failure.
const char *widcc_rt_set_include_paths(WidccRT *rt, const char *const *paths,
                                       int count);

// Replace how this runtime resolves names that are missing from its
// symbol map. The default looks the name up in the host process (dlsym
// on Unix, exported modules on Windows); callers can chain to it with
// widcc_rt_default_host_resolver(). Pass NULL to restore the default.
// The resolver applies to later loads of this runtime.
void widcc_rt_set_host_resolver(WidccRT *rt,
                                void *(*resolve)(WidccRT *, const char *));

// The default resolver: runtime symbols first, then the host process.
void *widcc_rt_default_host_resolver(WidccRT *rt, const char *name);

// ========== Load API ==========

// Compile one translation unit from a NUL-terminated C source string and
// load it as a module. `path` names the unit: it is used in diagnostics
// and as the base directory for quoted #include directives, but the file
// is not read (the string is the file's contents).
//
// On success returns NULL and, when `out` is non-NULL, stores the module
// handle there. Pass out=NULL to load without keeping a handle; the
// module is then released by widcc_rt_free(). On failure returns the
// error message and releases everything the unit allocated; the message
// stays valid until the next call on `rt`.
const char *widcc_rt_load_code(WidccRT *rt, const char *path,
                               const char *source, WidccCode **out);

// Compile a translation unit read from `path` through the file hooks
// (the host filesystem by default). Same result and handle semantics as
// widcc_rt_load_code().
const char *widcc_rt_load_code_file(WidccRT *rt, const char *path,
                                    WidccCode **out);

// Release a module: generated code, global data blocks and static call
// cells produced by that load. Symbol entries are neither removed nor
// restored; the map keeps the last definition even though its code is
// gone. Passing NULL is a no-op. Returns NULL on success, or a static
// error message if `code` is not a module of this runtime.
const char *widcc_rt_unload_code(WidccRT *rt, WidccCode *code);

#ifdef __cplusplus
}
#endif

#endif