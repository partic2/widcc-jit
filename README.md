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

The public API is `include/widcc_jit.h` and is split into a runtime half
and a compiler half.

A **runtime** (`WidccRT`) owns a symbol map from name to symbol
information (global functions and global variables), the generated machine
code pages and the generated global data blocks. A **compiler**
(`WidccCompiler`) is bound to one runtime and owns only its own
compile-time memory.

```c
WidccRT *rt = widcc_rt_new();
widcc_rt_add_func(rt, "host_fn", (void *)host_fn);   // optional host symbols

WidccCompiler *cc = widcc_cc_new(rt);
const char *err = widcc_cc_compile(cc, "prog.c", source);
if (err) {
  fprintf(stderr, "%s\n", err);
} else {
  int (*f)(int) = widcc_rt_lookup(rt, "f");
  f(42);
}

widcc_cc_free(cc);   // compile-time memory; generated code stays callable
widcc_rt_free(rt);   // code, data and symbol entries
```

`widcc_cc_compile_file(cc, "prog.c")` compiles a file read from disk
instead of an in-memory string. In both forms the path is the unit's file
name: quoted `#include "..."` directives resolve relative to it. Angle
includes `#include <...>` search the list installed with
`widcc_rt_set_include_paths()` (empty by default).

All source reading goes through the hooks installed by
`widcc_set_file_ops()`: open/read/close for file contents and exists for
include resolution. The default uses stdio and `stat()` on the host
filesystem; overriding them lets the frontend compile from an in-memory
or virtual file system. Omitted callbacks keep their defaults, and passing
NULL restores all of them. The hooks are process-global and must be set
before compiling.

Properties:

- Fallible calls return NULL on success and an error message on failure;
  constructors return NULL on failure. Error strings are owned by the
  library (compiler messages stay valid until the next compile or
  `widcc_cc_free()`).
- Instances are independent; one process may hold any number of
  runtime/compiler pairs.
- One runtime may be shared by several compilers, and one compiler may
  compile many translation units. Definitions from one unit are visible to
  later units through the runtime.
- Compile-time memory is arena-owned by the compiler and released by
  `widcc_cc_free()`. To bound frontend memory when compiling many units,
  create and free one compiler per unit while keeping the runtime.
- Extern symbols resolve through the runtime map first, then through the
  host process (`dlsym(RTLD_DEFAULT, ...)` on Unix, exported modules on
  Windows). `widcc_rt_set_host_resolver()` installs a per-runtime
  replacement.
- The runtime outlives any compiler: `widcc_cc_free()` never invalidates
  symbols, code or data. Use `widcc_rt_free_funcs()` followed by
  `widcc_rt_free_vars()` to release generated output while keeping the
  runtime itself alive.

## Layout

- `src/tokenize.c` `src/preprocess.c` `src/parse.c` `src/type.c` — C frontend
- `src/ccctx.c` / `src/ccctx.h` — per-instance compiler context and arena
  allocator
- `src/fileops.c` — default host file access behind `widcc_set_file_ops()`
- `src/jitgen.c` — sljit code generation backend
- `src/jit.c` `src/jit_internal.h` — runtime/compiler implementation and
  internals
- `src/all.c` — single-translation-unit build root; internals stay static
- `include/widcc_jit.h` — public API
- `examples/` — jit_test, stress_svalue, mem_test
- `sljit/` — platform-independent JIT backend