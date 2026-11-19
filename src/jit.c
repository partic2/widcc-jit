#ifndef WIDCC_JIT_C
#define WIDCC_JIT_C

// widcc JIT: Runtime API (symbol map + generated code/data ownership)
// and Compiler API (frontend arena + compilation pipeline).
#include "widcc.h"
#include "ccctx.h"
#include "jit_internal.h"
#include "sljitLir.h"
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#else
#include <dlfcn.h>
#endif

// ------------------------------------------------------------------
// Runtime heap helpers
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

static void *rt_alloc(size_t sz) {
  void *p = malloc(sz ? sz : 1);
  if (!p)
    error("widcc-jit: out of memory");
  return p;
}

static char *rt_strdup(const char *s) {
  size_t n = strlen(s) + 1;
  char *p = rt_alloc(n);
  memcpy(p, s, n);
  return p;
}

static void *rt_alloc_tracked(JitMallocBlock **list, size_t sz) {
  if (sz == 0)
    sz = 1;
  void *p = calloc(1, sz);
  if (!p)
    error("widcc-jit: out of memory");
  JitMallocBlock *b = rt_alloc(sizeof(*b));
  b->ptr = p;
  b->next = *list;
  *list = b;
  return p;
}

static void *rt_alloc_func(WidccRT *rt, size_t sz) {
  return rt_alloc_tracked(&rt->func_aux, sz);
}

static void *rt_alloc_data(WidccRT *rt, size_t sz) {
  return rt_alloc_tracked(&rt->data, sz);
}

static void rt_free_blocks(JitMallocBlock **list) {
  JitMallocBlock *b = *list;
  while (b) {
    JitMallocBlock *nx = b->next;
    free(b->ptr);
    free(b);
    b = nx;
  }
  *list = NULL;
}

static void free_path_array(char **paths, int count) {
  for (int i = 0; i < count; i++)
    free(paths[i]);
  free(paths);
}

static void **rt_cell_alloc(void) {
  void **p = calloc(1, sizeof(void *));
  if (!p)
    error("widcc-jit: out of memory");
  return p;
}

// ------------------------------------------------------------------
// Runtime symbol table
// ------------------------------------------------------------------

static JitSym *jit_sym(WidccRT *rt, const char *name, bool is_func) {
  JitSym *s = hashmap_get(&rt->syms, (char *)name);
  if (s) {
    if (is_func)
      s->is_func = true;
    return s;
  }
  s = rt_alloc(sizeof(*s));
  memset(s, 0, sizeof(*s));
  s->name = rt_strdup(name);
  s->is_func = is_func;
  hashmap_put(&rt->syms, s->name, s);
  s->next = rt->symlist;
  rt->symlist = s;
  return s;
}

static void **jit_sym_cell(JitSym *s) {
  if (!s->cell)
    s->cell = rt_cell_alloc();
  return s->cell;
}

// Remove every symbol of one class, deleting map entries and releasing
// per-symbol memory (name, indirection cell, entry itself).
static void rt_drop_symbols(WidccRT *rt, bool funcs) {
  JitSym **pp = &rt->symlist;
  while (*pp) {
    JitSym *s = *pp;
    if (s->is_func == funcs) {
      *pp = s->next;
      hashmap_delete(&rt->syms, s->name);
      free(s->cell);
      free(s->name);
      free(s);
    } else {
      pp = &s->next;
    }
  }
}


void *widcc_rt_default_host_resolver(WidccRT *rt, const char *name) {
  JitSym *s = hashmap_get(&rt->syms, (char *)name);
  if (s && s->addr)
    return s->addr;
#ifdef _WIN32
  void *addr = (void *)GetProcAddress(GetModuleHandleA(NULL), name);
  if (addr)
    return addr;
  HMODULE mods[1024];
  DWORD needed = 0;
  HANDLE proc = GetCurrentProcess();
  if (EnumProcessModules(proc, mods, sizeof(mods), &needed)) {
    int n = (int)(needed / sizeof(HMODULE));
    if (n > 1024)
      n = 1024;
    for (int i = 0; i < n; i++) {
      addr = (void *)GetProcAddress(mods[i], name);
      if (addr)
        return addr;
    }
  }
  return NULL;
#else
  return dlsym(RTLD_DEFAULT, name);
#endif
}

static void *jit_resolve_host(WidccRT *rt, const char *name) {
  if (rt->host_resolve)
    return rt->host_resolve(rt, name);
  return widcc_rt_default_host_resolver(rt, name);
}

void widcc_rt_set_host_resolver(WidccRT *rt,
                                void *(*resolve)(WidccRT *, const char *)) {
  if (rt)
    rt->host_resolve = resolve;
}


// ---------- public runtime API ----------

WidccRT *widcc_rt_new(void) {
  WidccRT *rt = calloc(1, sizeof(WidccRT));
  if (!rt)
    return NULL;
  hashmap_init_heap(&rt->syms);
  return rt;
}

void widcc_rt_free(WidccRT *rt) {
  if (!rt)
    return;
  widcc_rt_free_funcs(rt);
  widcc_rt_free_vars(rt);
  free_path_array(rt->inc_paths, rt->inc_count);
  hashmap_destroy(&rt->syms);
  free(rt);
}

int widcc_rt_add_func(WidccRT *rt, const char *name, void *fn) {
  if (!rt || !name || !fn)
    return -1;
  JitSym *s = jit_sym(rt, name, true);
  s->addr = fn;
  s->is_tentative = false;
  if (s->cell)
    *s->cell = fn;
  return 0;
}

int widcc_rt_add_var(WidccRT *rt, const char *name, void *var) {
  if (!rt || !name || !var)
    return -1;
  JitSym *s = jit_sym(rt, name, false);
  s->addr = var;
  s->is_tentative = false;
  if (s->cell)
    *s->cell = var;
  return 0;
}

void *widcc_rt_lookup(WidccRT *rt, const char *name) {
  if (!rt || !name)
    return NULL;
  JitSym *s = hashmap_get(&rt->syms, (char *)name);
  return s ? s->addr : NULL;
}

int widcc_rt_set_include_paths(WidccRT *rt, const char *const *paths,
                               int count) {
  if (!rt || count < 0 || (count > 0 && !paths))
    return -1;

  char **copy = NULL;
  if (count > 0) {
    copy = calloc((size_t)count, sizeof(char *));
    if (!copy)
      return -1;
    for (int i = 0; i < count; i++) {
      if (!paths[i]) {
        free_path_array(copy, i);
        return -1;
      }
      size_t n = strlen(paths[i]) + 1;
      copy[i] = malloc(n);
      if (!copy[i]) {
        free_path_array(copy, i);
        return -1;
      }
      memcpy(copy[i], paths[i], n);
    }
  }

  free_path_array(rt->inc_paths, rt->inc_count);
  rt->inc_paths = copy;
  rt->inc_count = count;
  return 0;
}

int widcc_rt_list_symbols(WidccRT *rt, char **names, int bufsize) {
  if (!rt)
    return 0;
  int n = 0;
  for (JitSym *s = rt->symlist; s; s = s->next) {
    if (names && n < bufsize)
      names[n] = s->name;
    n++;
  }
  return n;
}

void widcc_rt_free_funcs(WidccRT *rt) {
  if (!rt)
    return;
  JitCode *c = rt->codes;
  while (c) {
    JitCode *nx = c->next;
    sljit_free_code(c->code, NULL);
    free(c);
    c = nx;
  }
  rt->codes = NULL;
  rt_free_blocks(&rt->func_aux);
  rt_drop_symbols(rt, true);
}

void widcc_rt_free_vars(WidccRT *rt) {
  if (!rt)
    return;
  rt_free_blocks(&rt->data);
  rt_drop_symbols(rt, false);
}

// ------------------------------------------------------------------
// Compiler
// ------------------------------------------------------------------

static File *jit_add_file(CCCtx *ctx, char *name, char *contents) {
  int n = ctx->file_no;
  File *file = new_file(name, n + 1, contents);
  ctx->input_files = cc_realloc(ctx->input_files, sizeof(File *) * (n + 2));
  ctx->input_files[n] = file;
  ctx->input_files[n + 1] = NULL;
  ctx->file_no = n + 1;
  return file;
}

// Copy the runtime's default #include search list into the frontend
// context for this compilation.
static void jit_apply_include_paths(CCCtx *ctx, WidccRT *rt) {
  ctx->include_paths = (StringArray){0};
  for (int i = 0; i < rt->inc_count; i++)
    strarray_push(&ctx->include_paths, cc_strdup(rt->inc_paths[i]));
}

WidccCompiler *widcc_cc_new(WidccRT *rt) {
  if (!rt)
    return NULL;
  WidccCompiler *cc = calloc(1, sizeof(WidccCompiler));
  if (!cc)
    return NULL;
  ccctx_init(&cc->cc);
  cc->cc.err_print = false;
  cc->rt = rt;
  return cc;
}

void widcc_cc_free(WidccCompiler *cc) {
  if (!cc)
    return;
  if (cc->active_compiler) {
    sljit_free_compiler(cc->active_compiler);
    cc->active_compiler = NULL;
  }
  ccctx_destroy(&cc->cc);
  free(cc);
}

// Compile one translation unit into the runtime. When `source` is
// non-NULL it is the file's contents and `path` is only its name;
// otherwise the unit is read from `path` on disk.
static int jit_compile_core(WidccCompiler *cc, const char *path,
                            const char *source) {
  CCCtx *ctx = &cc->cc;
  cc_ctx = ctx;
  ccctx_reset_frontend(ctx);
  ctx->err_active = true;

  if (setjmp(ctx->err_jmp)) {
    if (cc->active_compiler) {
      sljit_free_compiler(cc->active_compiler);
      cc->active_compiler = NULL;
    }
    ctx->err_active = false;
    cc_ctx = NULL;
    return -1;
  }

  init_ty();
  init_macros();
  jit_apply_include_paths(ctx, cc->rt);

  Token head = {0}; Token *cur = &head; Token *last = NULL;
  cur->next = tokenize(add_input_file("widcc_builtins", jit_builtins, NULL), &last);
  cur = last;

  if (source) {
    File *file = jit_add_file(ctx, cc_strdup((char *)path),
                              cc_strdup((char *)source));
    cur->next = tokenize(file, NULL);
  } else {
    char *p = cc_strdup((char *)path);
    cur->next = tokenize_file(p, &last, NULL);
    if (!cur->next)
      error("cannot open %s", p);
  }

  Token *tok = preprocess(head.next, (char *)path);
  Obj *prog = parse(tok);
  jitgen_tu(cc, prog);

  ctx->err_active = false;
  cc_ctx = NULL;
  return 0;
}

int widcc_cc_compile(WidccCompiler *cc, const char *path,
                     const char *source) {
  if (!cc || !path || !source)
    return -1;
  return jit_compile_core(cc, path, source);
}

int widcc_cc_compile_file(WidccCompiler *cc, const char *path) {
  if (!cc || !path)
    return -1;
  return jit_compile_core(cc, path, NULL);
}

const char *widcc_cc_error(WidccCompiler *cc) {
  if (!cc)
    return "widcc-jit: no compiler";
  return cc->cc.err_msg;
}

#endif // WIDCC_JIT_C
