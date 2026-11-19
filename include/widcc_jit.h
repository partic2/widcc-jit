// widcc JIT runtime public API.
//
// A WidccJIT instance compiles C source code in memory via sljit and
// produces directly callable machine code. Instances are independent:
// one process may hold any number of them, each instance may compile
// any number of translation units, and memory is exposed through three
// tiers so callers can choose how much to release.
//
//   Tier 1 (machine code):  sljit code pages — runnable JIT output
//   Tier 2 (runtime data):  extern variable blocks + indirection cells
//   Tier 3 (compile arena): tokens/AST/Types/static data — build scratch
//
// Each tier can be released independently via widcc_jit_free_{code,rt,arena}
// as long as lower-numbered tiers have been released first.
#ifndef WIDCC_JIT_H
#define WIDCC_JIT_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct WidccJIT WidccJIT;

// ---------- lifecycle ----------

// Create a new JIT instance. Returns NULL on allocation failure.
WidccJIT *widcc_jit_new(void);

// Destroy everything the instance owns. Equivalent to calling
// widcc_jit_free_code / widcc_jit_free_rt / widcc_jit_free_arena in
// order and then free()ing the instance itself.
void widcc_jit_free(WidccJIT *jit);

// ---------- tiered release ----------
// Tiers must be released in ascending order (code first, then rt, then
// arena). Releasing a higher tier when a lower one still holds live
// pointers is a use-after-free and will crash. widcc_jit_free() handles
// the ordering automatically.

// Tier 1: release generated machine code pages. Any function pointers
// previously obtained from widcc_jit_symbol() become invalid.
// Safe to call even if no code has been generated.
void widcc_jit_free_code(WidccJIT *jit);

// Tier 2: release runtime-owned extern data blocks and indirection
// cells. Must not be called while any generated code is still runnable.
void widcc_jit_free_rt(WidccJIT *jit);

// Tier 3: release the compile-time arena (tokens, AST, Types, static
// data blocks, static indirection cells). Safe to recompile the same
// instance afterward — a fresh arena is minted automatically on the
// next compile.
void widcc_jit_free_arena(WidccJIT *jit);

// ---------- compilation ----------

// Compile a translation unit from a NUL-terminated C source string.
// `name` is used in diagnostics. Symbols defined by earlier compilations
// are visible; unresolved externs are looked up through the instance
// symbol table, then the host process (dlsym on Unix, exported symbols
// on Windows).
// Returns 0 on success, -1 on error (see widcc_jit_error()).
int widcc_jit_compile(WidccJIT *jit, const char *name, const char *source);

// The last error message of this instance.
const char *widcc_jit_error(WidccJIT *jit);

// ---------- symbol lookup & host mapping ----------

// Address of a symbol (function entry point or global variable).
// Returns NULL if not found.
void *widcc_jit_symbol(WidccJIT *jit, const char *name);

// Register a host symbol so JIT code can reference it. Registration
// overrides automatic host-process symbol resolution and is recorded in
// the instance symbol table.
// Returns 0 on success, -1 on error.
int widcc_jit_map_symbol(WidccJIT *jit, const char *name, void *addr);

int widcc_jit_symbols_count(WidccJIT *jit);
 
void widcc_jit_list_symbols(WidccJIT *jit, char **names);

#ifdef __cplusplus
}
#endif

#endif
