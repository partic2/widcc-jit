#ifndef WIDCC_JIT_C
#define WIDCC_JIT_C

// widcc JIT: runtime API (symbol map + generated code/data ownership) and
// the per-load compilation pipeline.
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

static void *rt_alloc_func(WidccCompiler *cc, size_t sz) {
  return rt_alloc_tracked(&cc->mod->func_aux, sz);
}

static void *rt_alloc_data(WidccCompiler *cc, size_t sz) {
  return rt_alloc_tracked(&cc->mod->data, sz);
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

// A module owns every runtime allocation made while its unit compiles.
static WidccCode *rt_module_new(WidccRT *rt) {
  WidccCode *m = rt_alloc(sizeof(*m));
  memset(m, 0, sizeof(*m));
  m->next = rt->modules;
  rt->modules = m;
  return m;
}

static void rt_module_free(WidccRT *rt, WidccCode *m) {
  WidccCode **pp = &rt->modules;
  while (*pp && *pp != m)
    pp = &(*pp)->next;
  if (*pp)
    *pp = m->next;

  for (JitCode *c = m->codes; c;) {
    JitCode *nx = c->next;
    sljit_free_code(c->code, NULL);
    free(c);
    c = nx;
  }
  rt_free_blocks(&m->func_aux);
  rt_free_blocks(&m->data);
  free(m);
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

// Release every symbol entry (names, cells, entries). Used only when the
// runtime itself is destroyed; unloading a module leaves symbols alone.
static void rt_free_symbols(WidccRT *rt) {
  JitSym *s = rt->symlist;
  while (s) {
    JitSym *nx = s->next;
    free(s->cell);
    free(s->name);
    free(s);
    s = nx;
  }
  rt->symlist = NULL;
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
  while (rt->modules)
    rt_module_free(rt, rt->modules);
  rt_free_symbols(rt);
  free_path_array(rt->inc_paths, rt->inc_count);
  hashmap_destroy(&rt->syms);
  free(rt);
}

const char *widcc_rt_add_func(WidccRT *rt, const char *name, void *fn) {
  if (!rt || !name || !fn)
    return "widcc_rt_add_func: invalid argument";
  JitSym *s = jit_sym(rt, name, true);
  s->addr = fn;
  s->is_tentative = false;
  if (s->cell)
    *s->cell = fn;
  return NULL;
}

const char *widcc_rt_add_var(WidccRT *rt, const char *name, void *var) {
  if (!rt || !name || !var)
    return "widcc_rt_add_var: invalid argument";
  JitSym *s = jit_sym(rt, name, false);
  s->addr = var;
  s->is_tentative = false;
  if (s->cell)
    *s->cell = var;
  return NULL;
}

void *widcc_rt_lookup(WidccRT *rt, const char *name) {
  if (!rt || !name)
    return NULL;
  JitSym *s = hashmap_get(&rt->syms, (char *)name);
  return s ? s->addr : NULL;
}

const char *widcc_rt_set_include_paths(WidccRT *rt, const char *const *paths,
                                       int count) {
  if (!rt || count < 0 || (count > 0 && !paths))
    return "widcc_rt_set_include_paths: invalid argument";

  char **copy = NULL;
  if (count > 0) {
    copy = calloc((size_t)count, sizeof(char *));
    if (!copy)
      return "widcc_rt_set_include_paths: out of memory";
    for (int i = 0; i < count; i++) {
      if (!paths[i]) {
        free_path_array(copy, i);
        return "widcc_rt_set_include_paths: invalid argument";
      }
      size_t n = strlen(paths[i]) + 1;
      copy[i] = malloc(n);
      if (!copy[i]) {
        free_path_array(copy, i);
        return "widcc_rt_set_include_paths: out of memory";
      }
      memcpy(copy[i], paths[i], n);
    }
  }

  free_path_array(rt->inc_paths, rt->inc_count);
  rt->inc_paths = copy;
  rt->inc_count = count;
  return NULL;
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

// ------------------------------------------------------------------
// Load
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

// Compile one translation unit into `cc->mod`. When `source` is
// non-NULL it is the file's contents and `path` is only its name;
// otherwise the unit is read from `path` on disk.
static const char *jit_compile_core(WidccCompiler *cc, const char *path,
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
    return ctx->err_msg;
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
  return NULL;
}

// A load is one ephemeral compiler whose allocations go to a module.
static const char *rt_load(WidccRT *rt, const char *path, const char *source,
                           WidccCode **out) {
  WidccCompiler cc = {0};
  ccctx_init(&cc.cc);
  cc.cc.err_print = false;
  cc.rt = rt;
  cc.mod = rt_module_new(rt);

  const char *err = jit_compile_core(&cc, path, source);
  if (err) {
    // The message lives in the compiler arena that is about to go away.
    snprintf(rt->err_msg, sizeof(rt->err_msg), "%s", err);
    rt_module_free(rt, cc.mod);
    ccctx_destroy(&cc.cc);
    return rt->err_msg;
  }

  if (out)
    *out = cc.mod;
  ccctx_destroy(&cc.cc);
  return NULL;
}

const char *widcc_rt_load_code(WidccRT *rt, const char *path,
                               const char *source, WidccCode **out) {
  if (!rt || !path || !source)
    return "widcc_rt_load_code: invalid argument";
  return rt_load(rt, path, source, out);
}

const char *widcc_rt_load_code_file(WidccRT *rt, const char *path,
                                    WidccCode **out) {
  if (!rt || !path)
    return "widcc_rt_load_code_file: invalid argument";
  return rt_load(rt, path, NULL, out);
}

const char *widcc_rt_unload_code(WidccRT *rt, WidccCode *code) {
  if (!code)
    return NULL;
  if (!rt)
    return "widcc_rt_unload_code: invalid argument";
  for (WidccCode *m = rt->modules; m; m = m->next) {
    if (m == code) {
      rt_module_free(rt, code);
      return NULL;
    }
  }
  return "widcc_rt_unload_code: module is not loaded in this runtime";
}

#endif // WIDCC_JIT_C
