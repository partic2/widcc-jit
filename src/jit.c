#ifndef WIDCC_JIT_C
#define WIDCC_JIT_C

// widcc JIT runtime: public API, symbol table and compilation pipeline.
// See widcc_jit.h for the memory model.
#include "widcc.h"
#include "ccctx.h"
#include "jit_internal.h"
#include "sljitLir.h"
#include <string.h>
#include <assert.h>
#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#else
#include <dlfcn.h>
#endif

// ------------------------------------------------------------------
// Tiered release helpers (all public, declared in widcc_jit.h)
// ------------------------------------------------------------------

// va_list layout injected into every TU.
#ifdef _WIN64
static char jit_builtins[] = "typedef char *__builtin_va_list[1];";
#else
static char jit_builtins[] =
  "typedef struct {"
  "  unsigned int gp_offset;"
  "  unsigned int fp_offset;"
  "  void *overflow_arg_area;"
  "  void *reg_save_area;"
  "} __builtin_va_list[1];";
#endif

static void jit_undefined_fn(void) {
  fprintf(stderr, "widcc-jit: call to an unresolved function\n");
  abort();
}
static void *jit_undefined_fn_addr(void) { return (void *)jit_undefined_fn; }

// ---------- runtime-owned heap helpers ----------

static void *jit_malloc(WidccJIT *jit, size_t sz) {
  void *p = malloc(sz);
  if (!p) {
    CCCtx *ctx = ccctx();
    if (ctx->err_active) {
      snprintf(ctx->err_msg, sizeof(ctx->err_msg),
               "widcc-jit: runtime heap exhausted (%zu bytes)", sz);
      longjmp(ctx->err_jmp, 1);
    }
    fprintf(stderr, "widcc-jit: runtime heap exhausted (%zu bytes)\n", sz);
    abort();
  }
  JitMallocBlock *b = malloc(sizeof(*b));
  if (!b) { free(p); abort(); }
  b->ptr = p; b->next = jit->rt_blocks;
  jit->rt_blocks = b;
  return p;
}

static void *jit_calloc(WidccJIT *jit, size_t n, size_t sz) {
  if (n == 0) n = 1; if (sz == 0) sz = 1;
  void *p = jit_malloc(jit, n * sz);
  memset(p, 0, n * sz);
  return p;
}

static void jit_free_rt(WidccJIT *jit) {
  JitMallocBlock *b = jit->rt_blocks;
  while (b) {
    free(b->ptr);
    JitMallocBlock *nx = b->next; free(b);
    b = nx;
  }
  jit->rt_blocks = NULL;
}

// ---------- tiered release implementations ----------

void widcc_jit_free_code(WidccJIT *jit) {
  if (!jit) return;
  for (JitCode *c = jit->codes; c; c = c->next)
    sljit_free_code(c->code, NULL);
  jit->codes = NULL;
}

void widcc_jit_free_rt(WidccJIT *jit) {
  if (!jit) return;
  jit_free_rt(jit);
}

void widcc_jit_free_arena(WidccJIT *jit) {
  if (!jit) return;
  ccctx_destroy(&jit->cc);
  ccctx_init(&jit->cc);
  memset(&jit->syms, 0, sizeof(jit->syms));
  jit->symlist = NULL;
}

// ---------- external-linkage symbol table ----------

static JitSym *jit_sym(WidccJIT *jit, char *name) {
  JitSym *s = hashmap_get(&jit->syms, name);
  if (s) return s;
  s = cc_calloc(1, sizeof(JitSym));
  s->name = cc_strdup(name);
  hashmap_put(&jit->syms, s->name, s);
  s->next = jit->symlist; jit->symlist = s;
  return s;
}

static void **jit_sym_cell(JitSym *s, WidccJIT *jit) {
  if (!s->cell)
    s->cell = jit_calloc(jit, 1, sizeof(void *));
  return s->cell;
}

// ---------- host resolution: instance → host ----------

static void *jit_host_resolve(WidccJIT *jit, char *name) {
  JitSym *s = hashmap_get(&jit->syms, name);
  if (s && s->addr) return s->addr;
#ifdef _WIN32
  void *addr = (void *)GetProcAddress(GetModuleHandleA(NULL), name);
  if (addr) return addr;
  HMODULE mods[1024]; DWORD needed = 0;
  HANDLE proc = GetCurrentProcess();
  if (EnumProcessModules(proc, mods, sizeof(mods), &needed)) {
    int n = (int)(needed / sizeof(HMODULE));
    if (n > 1024) n = 1024;
    for (int i = 0; i < n; i++) {
      addr = (void *)GetProcAddress(mods[i], name);
      if (addr) return addr;
    }
  }
  return NULL;
#else
  return dlsym(RTLD_DEFAULT, name);
#endif
}

// ---------- host-to-internal glue ----------

static File *jit_add_file(CCCtx *ctx, char *name, char *contents) {
  int n = ctx->file_no;
  File *file = new_file(name, n + 1, contents);
  ctx->input_files = cc_realloc(ctx->input_files, sizeof(File *) * (n + 2));
  ctx->input_files[n] = file;
  ctx->input_files[n + 1] = NULL;
  ctx->file_no = n + 1;
  return file;
}

WidccJIT *widcc_jit_new(void) {
  WidccJIT *jit = calloc(1, sizeof(WidccJIT));
  if (!jit) return NULL;
  ccctx_init(&jit->cc);
  jit->cc.err_print = false;
  return jit;
}

void widcc_jit_free(WidccJIT *jit) {
  if (!jit) return;
  widcc_jit_free_code(jit);
  widcc_jit_free_rt(jit);
  if (jit->active_compiler) {
    sljit_free_compiler(jit->active_compiler);
    jit->active_compiler = NULL;
  }
  ccctx_destroy(&jit->cc);
  free(jit);
}

static int jit_compile_core(WidccJIT *jit, const char *name,
                            const char *source) {
  CCCtx *ctx = &jit->cc;
  cc_ctx = ctx;
  ccctx_reset_frontend(ctx);
  ctx->err_active = true;

  if (setjmp(ctx->err_jmp)) {
    if (jit->active_compiler) {
      sljit_free_compiler(jit->active_compiler);
      jit->active_compiler = NULL;
    }
    ctx->err_active = false;
    cc_ctx = NULL;
    return -1;
  }

  init_ty();
  init_macros();

  Token head = {0}; Token *cur = &head; Token *last = NULL;
  cur->next = tokenize(add_input_file("widcc_builtins", jit_builtins, NULL), &last);
  cur = last;

  File *file = jit_add_file(ctx, cc_strdup((char *)name),
                            cc_strdup((char *)source));
  cur->next = tokenize(file, NULL);

  Token *tok = preprocess(head.next, (char *)name);
  Obj *prog = parse(tok);
  jitgen_tu(jit, prog);

  ctx->err_active = false;
  cc_ctx = NULL;
  return 0;
}

int widcc_jit_compile(WidccJIT *jit, const char *name, const char *source) {
  if (!jit || !source) return -1;
  return jit_compile_core(jit, name, source);
}

const char *widcc_jit_error(WidccJIT *jit) {
  if (!jit) return "widcc-jit: no instance";
  return jit->cc.err_msg;
}

void *widcc_jit_symbol(WidccJIT *jit, const char *name) {
  if (!jit || !name) return NULL;
  JitSym *s = hashmap_get(&jit->syms, (char *)name);
  return (s && s->addr) ? s->addr : NULL;
}

int widcc_jit_map_symbol(WidccJIT *jit, const char *name, void *addr) {
  if (!jit || !name || !addr) return -1;
  cc_ctx = &jit->cc;
  JitSym *s = jit_sym(jit, (char *)name);
  s->addr = addr;
  if (s->cell) *s->cell = addr;
  else         *jit_sym_cell(s, jit) = addr;
  cc_ctx = NULL;
  return 0;
}

int widcc_jit_symbols_count(WidccJIT *jit) {
  if (!jit) return -1;
  return hashmap_size(&jit->syms);
}

void widcc_jit_list_symbols(WidccJIT *jit, char **names) {
  if (!jit) return;
  hashmap_keys(&jit->syms, names);
}


#endif // WIDCC_JIT_C
