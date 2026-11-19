#ifndef WIDCC_JIT_C
#define WIDCC_JIT_C

// widcc JIT runtime: public API, symbol table, namespace and compilation
// pipeline. See widcc_jit.h for the memory / namespace model.
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
// WidccNamespace: malloc-backed open-addressing hash table.
// Independent of any JIT instance. Simple variant — deletions use
// tombstones and the table rehashes / compacts itself on insertion
// when load factor exceeds the HIGH watermark.
// ------------------------------------------------------------------

#define NS_INIT_CAP 16
#define NS_HIGH     70  // percent
#define NS_LOW      50  // percent (target after rehash, to keep capacity)

typedef struct NSEntry {
  char *key;
  void *addr;
} NSEntry;

struct WidccNamespace {
  NSEntry *buckets;
  int capacity;  // allocated bucket count
  int live;      // keys present (non-tombstone)
  int used;      // live + tombstones — probing stops at `used` consecutive
                 // slots or an empty (key==NULL) slot, whichever hits first
};

#define NS_TOMB ((char *)1)

static uint64_t ns_fnv(const char *s, int len) {
  uint64_t h = 0xcbf29ce484222325ULL;
  for (int i = 0; i < len; i++) {
    h *= 0x100000001b3ULL;
    h ^= (unsigned char)s[i];
  }
  return h;
}

// Find `key` in the table. On match returns the entry pointer.
// On miss returns NULL. The second form additionally writes the empty-slot
// position to *out_slot (if out_slot != NULL) so the caller can insert
// without re-probing.
static NSEntry *ns_probe(WidccNamespace *ns, const char *key, int keylen,
                         int *out_slot) {
  if (out_slot) *out_slot = -1;
  if (!ns->buckets) return NULL;
  uint64_t h = ns_fnv(key, keylen);
  for (int i = 0; i < ns->capacity; i++) {
    int idx = (h + i) % ns->capacity;
    NSEntry *e = &ns->buckets[idx];
    if (!e->key) {
      if (out_slot && *out_slot < 0) *out_slot = idx;
      return NULL;
    }
    if (e->key == NS_TOMB) {
      if (out_slot && *out_slot < 0) *out_slot = idx;
      continue;
    }
    if ((int)strlen(e->key) == keylen && memcmp(e->key, key, keylen) == 0)
      return e;
  }
  return NULL;
}

static void ns_rehash(WidccNamespace *ns) {
  if (ns->capacity == 0) return;
  int new_cap = ns->capacity;
  while ((ns->live * 100) / new_cap >= NS_LOW) new_cap *= 2;
  if (new_cap == ns->capacity) return; // nothing to do
  WidccNamespace tmp = {0};
  tmp.capacity = new_cap;
  tmp.buckets = calloc(new_cap, sizeof(NSEntry));
  if (!tmp.buckets) return; // leave the old table in place
  int old_live = ns->live;
  for (int i = 0; i < ns->capacity; i++) {
    NSEntry *e = &ns->buckets[i];
    if (e->key && e->key != NS_TOMB)
      widcc_ns_put(&tmp, e->key, e->addr);
  }
  free(ns->buckets);
  *ns = tmp;
  // tmp's buckets transferred via assignment; re-live assertion:
  assert(ns->live == old_live);
}

// --- public WidccNamespace API ---

WidccNamespace *widcc_ns_new(void) {
  WidccNamespace *ns = calloc(1, sizeof(*ns));
  if (!ns) return NULL;
  ns->capacity = NS_INIT_CAP;
  ns->buckets = calloc(ns->capacity, sizeof(NSEntry));
  if (!ns->buckets) { free(ns); return NULL; }
  return ns;
}

void widcc_ns_free(WidccNamespace *ns) {
  if (!ns) return;
  for (int i = 0; i < ns->capacity; i++) {
    NSEntry *e = &ns->buckets[i];
    if (e->key && e->key != NS_TOMB) free(e->key);
  }
  free(ns->buckets);
  free(ns);
}

int widcc_ns_put(WidccNamespace *ns, const char *name, void *addr) {
  if (!ns || !name || !addr) return -1;
  int keylen = (int)strlen(name);

  // If load is too high, compact / grow before inserting.
  if (ns->capacity > 0 && (ns->used * 100) / ns->capacity >= NS_HIGH)
    ns_rehash(ns);

  int slot = -1;
  NSEntry *e = ns_probe(ns, name, keylen, &slot);
  if (e) {
    // overwrite
    e->addr = addr;
    return 0;
  }
  if (slot < 0) {
    // table full of tombstones — rehash and retry.
    ns_rehash(ns);
    e = ns_probe(ns, name, keylen, NULL);
    if (e) { e->addr = addr; return 0; }
    // now there must be a real empty slot because rehash allocates new cap
    slot = -1;
    for (int i = 0; i < ns->capacity && slot < 0; i++)
      if (!ns->buckets[i].key) slot = i;
    if (slot < 0) return -1;
  }
  NSEntry *s = &ns->buckets[slot];
  int was_tomb = (s->key == NS_TOMB);
  s->key = malloc(keylen + 1);
  if (!s->key) return -1;
  memcpy(s->key, name, keylen + 1);
  s->addr = addr;
  ns->used++;   // incremented whether we re-used a tombstone or filled NULL
  if (!was_tomb) ns->live++;
  return 0;
}

int widcc_ns_del(WidccNamespace *ns, const char *name) {
  if (!ns || !name) return -1;
  int keylen = (int)strlen(name);
  NSEntry *e = ns_probe(ns, name, keylen, NULL);
  if (!e) return -1;
  free(e->key);
  e->key = NS_TOMB;
  ns->live--;
  return 0;
}

void *widcc_ns_get(WidccNamespace *ns, const char *name) {
  if (!ns || !name || !ns->buckets) return NULL;
  int keylen = (int)strlen(name);
  NSEntry *e = ns_probe(ns, name, keylen, NULL);
  return e ? e->addr : NULL;
}

void widcc_ns_clear(WidccNamespace *ns) {
  if (!ns) return;
  for (int i = 0; i < ns->capacity; i++) {
    NSEntry *e = &ns->buckets[i];
    if (e->key && e->key != NS_TOMB) { free(e->key); e->key = NULL; }
  }
  ns->live = 0;
  ns->used = 0;
}

void widcc_ns_foreach(WidccNamespace *ns, widcc_ns_iter_cb cb, void *user) {
  if (!ns || !cb) return;
  for (int i = 0; i < ns->capacity; i++) {
    NSEntry *e = &ns->buckets[i];
    if (e->key && e->key != NS_TOMB) {
      if (cb(ns, e->key, e->addr, user)) return;
    }
  }
}

// --- namespace hooks used by the JIT ---

static void *jit_ns_lookup(WidccJIT *jit, char *name) {
  if (!jit || !jit->ns || !name) return NULL;
  return widcc_ns_get(jit->ns, name);
}

static void jit_ns_push(WidccJIT *jit, const char *name, void *addr) {
  if (!jit || !jit->ns || !jit->sync_ns || !name || !addr) return;
  widcc_ns_put(jit->ns, name, addr);
}

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

// ---------- host resolution: instance → namespace → dlsym ----------

static void *jit_host_resolve(WidccJIT *jit, char *name) {
  JitSym *s = hashmap_get(&jit->syms, name);
  if (s && s->addr) return s->addr;
  void *n = jit_ns_lookup(jit, name);
  if (n) return n;
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
  // Note: attached namespace is owned by the caller — we only drop the ref.
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

int widcc_jit_compile_ns(WidccJIT *jit, const char *name,
                         const char *source, void *ns) {
  if (!jit || !source) return -1;
  WidccNamespace *prev = jit->ns;
  int prev_sync = jit->sync_ns;
  if (ns) { jit->ns = (WidccNamespace *)ns; jit->sync_ns = 1; }
  int r = jit_compile_core(jit, name, source);
  jit->ns = prev; jit->sync_ns = prev_sync;
  return r;
}

const char *widcc_jit_error(WidccJIT *jit) {
  if (!jit) return "widcc-jit: no instance";
  return jit->cc.err_msg;
}

void *widcc_jit_symbol(WidccJIT *jit, const char *name) {
  if (!jit || !name) return NULL;
  JitSym *s = hashmap_get(&jit->syms, (char *)name);
  if (s && s->addr) return s->addr;
  return widcc_ns_get(jit->ns, name);
}

int widcc_jit_map_symbol(WidccJIT *jit, const char *name, void *addr) {
  if (!jit || !name || !addr) return -1;
  cc_ctx = &jit->cc;
  JitSym *s = jit_sym(jit, (char *)name);
  s->addr = addr;
  if (s->cell) *s->cell = addr;
  else         *jit_sym_cell(s, jit) = addr;
  cc_ctx = NULL;
  if (jit->ns) widcc_ns_put(jit->ns, name, addr);
  return 0;
}

void widcc_jit_attach_ns(WidccJIT *jit, WidccNamespace *ns, int sync) {
  if (!jit) return;
  jit->ns = ns;
  jit->sync_ns = sync;
}

WidccNamespace *widcc_jit_detach_ns(WidccJIT *jit) {
  if (!jit) return NULL;
  WidccNamespace *ns = jit->ns;
  jit->ns = NULL; jit->sync_ns = 0;
  return ns;
}


#undef NS_INIT_CAP
#undef NS_HIGH
#undef NS_LOW
#undef NS_TOMB
#endif // WIDCC_JIT_C
