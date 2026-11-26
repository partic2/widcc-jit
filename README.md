# widcc-jit

A JIT runtime for C, built from the [`widcc`](https://github.com/fuhsnn/widcc)
compiler frontend (tokenizer, preprocessor, parser, types) with an
[sljit](https://github.com/zherczeg/sljit) backend that emits directly
callable machine code in memory. No assembler, linker or ELF output is
involved.

`widcc` is a stable branch of `slimcc`, which is based on
[Rui Ueyama's `chibicc`](https://github.com/rui314/chibicc).

## Building & running

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Options:

- `-DWIDCC_BUILD_TESTS=OFF` — skip jit_test, stress_svalue and mem_test.
- `-DWIDCC_M32=ON` — build for a 32-bit (ILP32) host.
- `-DWIDCC_SANITIZE=ON` — build with AddressSanitizer/LeakSanitizer; a leak
  then fails `mem_test` under `ctest`.

## API

The public API is `include/widcc_jit.h`: one runtime type, one module
handle and the file hooks.

A **runtime** (`WidccRT`) owns the symbol map from name to symbol
information (global functions and global variables) and the set of loaded
modules. A **module** (`WidccCode`) is one loaded translation unit and
owns the machine code pages, global data blocks and static call cells
produced by it.

```c
WidccRT *rt = widcc_rt_new();
widcc_rt_add_func(rt, "host_fn", (void *)host_fn);   // optional host symbols

WidccCode *mod = NULL;
const char *err = widcc_rt_load_code(rt, "prog.c", source, &mod);
if (err) {
  fprintf(stderr, "%s\n", err);
} else {
  int (*f)(int) = widcc_rt_lookup(rt, "f");
  f(42);
  widcc_rt_unload_code(rt, mod);   // code and data of this unit
}

widcc_rt_free(rt);   // unloads remaining modules, drops symbol entries
```

`widcc_rt_load_code_file(rt, "prog.c", &mod)` reads the unit through the
file hooks instead of taking an in-memory string. In both forms the path
is the unit's file name: quoted `#include "..."` directives resolve
relative to it. Angle includes `#include <...>` search the list installed
with `widcc_rt_set_include_paths()` (empty by default).

Loading follows the dynamic-loader model: a later module overrides earlier
definitions of the same name, and unloading never restores them. Symbols
keep their last definition even after the module that produced them is
unloaded; calling through such a symbol is undefined, as with `dlclose()`.

### Calling convention and the Block ABI

Functions with at most four parameters use the host C ABI, so the host can
call them through the natural prototype. Functions with more than four
parameters, and all variadic functions, are compiled with the **Block ABI**
to stay within sljit's four-argument limit:

- The JIT entry takes a single `void *` pointing to an argument block.
- The block is an array of 8-byte slots in declaration order. Integer and
  pointer arguments occupy a full slot; a `float` uses 4 bytes and a
  `double` 8 bytes at the start of its slot.
- JIT-to-JIT calls (direct, indirect, recursive, forward-referenced) all
  agree on this ABI because it is a property of the function type.

JIT-to-JIT calls need no host involvement. To call such a function from the
host, use a one-pointer prototype and fill the slots:

```c
int (*sum5)(void *) = widcc_rt_lookup(rt, "sum5");   // int sum5(int,int,int,int,int)
long long args[5] = {1, 2, 3, 4, 5};
int r = sum5(args);
```

A function pointer to a block-ABI function must point to JIT code; do not
call it with its original C prototype.

Inside a variadic JIT function, `va_start`, `va_arg`, `va_copy` and `va_end`
(the `__builtin_va_*` forms) operate on that same argument block:
`__builtin_va_list` is a one-element `char *` array holding a cursor, so
`va_start` points it just past the named parameters and each `va_arg` reads
one 8-byte slot and advances by 8. This is a widcc-private layout, not the
host's `va_list`, so it cannot be handed to a host variadic function such as
`printf`. Two further limits follow from the block format: a `va_arg` of a
`float` converts the stored promoted `double` back to `float`, and types
wider than 8 bytes (structs, unions, `long double`) are rejected.

All source reading goes through the hooks installed by
`widcc_set_file_ops()`: open/read/close for file contents and exists for
include resolution. The default uses stdio and `stat()` on the host
filesystem; overriding them lets the frontend compile from an in-memory
or virtual file system. Omitted callbacks keep their defaults, and passing
NULL restores all of them. The hooks are process-global and must be set
before loading.

Properties:

- Fallible calls return NULL on success and an error message on failure;
  constructors return NULL on failure. Load messages stay valid until the
  next call on that runtime; all other messages are static.
- Runtimes are independent; one process may hold any number of them.
- Each load owns everything its unit allocated and releases it on unload,
  including static functions, static variables and string literals.
- Frontend memory (tokens, AST, types) is arena-owned by an internal
  per-load compiler and released when the load returns, so it does not
  accumulate across modules.
- Definitions from one module are visible to modules loaded after it, so
  forward declarations across translation units work.
- Extern symbols resolve through the runtime map first, then through the
  host process (`dlsym(RTLD_DEFAULT, ...)` on Unix, exported modules on
  Windows). `widcc_rt_set_host_resolver()` installs a per-runtime
  replacement.

## Layout

- `src/tokenize.c` `src/preprocess.c` `src/parse.c` `src/type.c` — C frontend
- `src/ccctx.c` / `src/ccctx.h` — per-instance compiler context and arena
  allocator
- `src/fileops.c` — default host file access behind `widcc_set_file_ops()`
- `src/jitgen.c` — sljit code generation backend
- `src/jit.c` `src/jit_internal.h` — runtime, module load/unload and
  symbol-map internals
- `src/all.c` — single-translation-unit build root; internals stay static
- `include/widcc_jit.h` — public API
- `examples/` — jit_test, stress_svalue, mem_test
- `sljit/` — platform-independent JIT backend