# widcc-sljit

A JIT runtime for C, built from the [`widcc`](https://github.com/fuhsnn/widcc)
compiler frontend (tokenizer, preprocessor, parser, types) with an
[sljit](https://github.com/zherczeg/sljit) backend that emits directly
callable machine code in memory. No assembler, linker or ELF output is
involved.

`widcc` is a stable branch of `slimcc`, which is based on
[Rui Ueyama's `chibicc`](https://github.com/rui314/chibicc).

# Building & Running

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j          # builds libwidcc_jit.a + jit_test
./build/jit_test                # runs the functional test suite
cmake --install build --prefix /usr/local
```

The standalone `widcc_standalone` compiler driver target is enabled by
default on Linux (`-DWIDCC_BUILD_DRIVER=ON`) if the upstream widcc sources
(`codegen.c`, `main.c`, `platform/linux-*.c`) are present. It is skipped
silently otherwise — the JIT library and `jit_test` still build fine.

# Usage

Link `libwidcc_jit.a` into a host program (with `-ldl` on Linux) and use the
public API in `widcc_jit.h`:

```c
WidccJIT *jit = widcc_jit_new();
widcc_jit_map_symbol(jit, "host_fn", host_fn);        // optional host symbols
if (widcc_jit_compile(jit, "prog.c", source) == 0) {  // compile C source
  int (*f)(int) = widcc_jit_symbol(jit, "f");         // look up entry points
  f(42);
}
widcc_jit_free(jit);
```

- Instances are independent: one process may hold any number of them.
- One instance may compile many translation units; symbols defined by earlier
  compilations are visible to later ones.
- Unresolved symbols are resolved via `dlsym(RTLD_DEFAULT, ...)` unless
  registered beforehand with `widcc_jit_map_symbol()`.
- All memory owned by an instance (frontend arena, data sections, generated
  code) is released by `widcc_jit_free()`.

# Layout

- `tokenize.c` `preprocess.c` `parse.c` `type.c` — C frontend
- `ccctx.c`/`ccctx.h` — per-instance compiler context and arena allocator
- `jitgen.c` — sljit code generation backend
- `jit.c` `widcc_jit.h` `jit_internal.h` — JIT runtime public API and internals
- `sljit/` — platform-independent JIT compiler backend
- `examples/jit_test.c` — functional test of the JIT runtime
