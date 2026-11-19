// Memory lifecycle test: exercises every public API path — compilation,
// code generation, symbol tables, namespaces, tiered release and error
// recovery — and then releases everything. Run under LeakSanitizer (build
// with -DWIDCC_SANITIZE=ON) to verify that the runtime reclaims all memory.
#include "widcc_jit.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static int host_add(int a, int b) { return a + b; }

static int ns_count_cb(WidccNamespace *ns, const char *name, void *addr,
                       void *user) {
  (void)ns;
  (void)name;
  (void)addr;
  (*(int *)user)++;
  return 0;
}

int main(void) {
  // 1. Empty instance lifecycle.
  WidccJIT *jit = widcc_jit_new();
  assert(jit);
  widcc_jit_free(jit);
  widcc_jit_free(NULL); // NULL-safe

  // 2. Repeated compile / execute / destroy rounds.
  for (int round = 0; round < 3; round++) {
    jit = widcc_jit_new();
    assert(jit);
    assert(widcc_jit_compile(jit, "t.c",
      "int g;\n"
      "int f(int x){ g+=x; return g*g; }\n"
      "double d(double a,double b){ return a*b+1.0; }\n") == 0);
    int (*f)(int) = widcc_jit_symbol(jit, "f");
    double (*d)(double, double) = widcc_jit_symbol(jit, "d");
    assert(f && d);
    assert(f(3) == 9);
    assert(f(1) == 16);
    assert(d(2.0, 3.0) == 7.0);
    widcc_jit_free(jit);
  }

  // 3. Tiered release (code -> rt -> arena), then recompile.
  jit = widcc_jit_new();
  assert(widcc_jit_compile(jit, "a.c", "int one(void){ return 1; }\n") == 0);
  int (*one)(void) = widcc_jit_symbol(jit, "one");
  assert(one() == 1);
  widcc_jit_free_code(jit);
  widcc_jit_free_rt(jit);
  widcc_jit_free_arena(jit);
  assert(widcc_jit_compile(jit, "b.c", "int two(void){ return 2; }\n") == 0);
  int (*two)(void) = widcc_jit_symbol(jit, "two");
  assert(two() == 2);
  widcc_jit_free(jit); // free again after the tiers were already released

  // 4. Host symbol mapping and forward declarations across units.
  jit = widcc_jit_new();
  assert(widcc_jit_map_symbol(jit, "host_add", (void *)host_add) == 0);
  assert(widcc_jit_compile(jit, "u1.c",
    "int host_add(int,int);\n"
    "int callee(int);\n"
    "int caller(int x){ return callee(x)+host_add(x,1); }\n") == 0);
  assert(widcc_jit_compile(jit, "u2.c",
    "int callee(int x){ return x*10; }\n") == 0);
  int (*caller)(int) = widcc_jit_symbol(jit, "caller");
  assert(caller && caller(4) == 45);
  widcc_jit_free(jit);

  // 5. Namespace lifecycle, attach/detach, sync push, compile_ns.
  WidccNamespace *ns = widcc_ns_new();
  assert(ns);
  assert(widcc_ns_put(ns, "host_add", (void *)host_add) == 0);
  assert(widcc_ns_get(ns, "host_add") == (void *)host_add);
  assert(widcc_ns_del(ns, "host_add") == 0);
  assert(widcc_ns_del(ns, "host_add") == -1);
  assert(widcc_ns_put(ns, "host_add", (void *)host_add) == 0);

  jit = widcc_jit_new();
  widcc_jit_attach_ns(jit, ns, 1);
  assert(widcc_jit_compile(jit, "n1.c",
    "int pubfn(int x){ return x+7; }\n") == 0);
  assert(widcc_jit_compile(jit, "n2.c",
    "int host_add(int,int);\n"
    "int pubfn(int);\n"
    "int use(int x){ return pubfn(x)+host_add(x,x); }\n") == 0);
  int (*use)(int) = widcc_jit_symbol(jit, "use");
  assert(use && use(3) == 10 + 6);

  int count = 0;
  widcc_ns_foreach(ns, ns_count_cb, &count);
  assert(count >= 2); // pubfn pushed back by sync mode, host_add present
  assert(widcc_jit_detach_ns(jit) == ns);

  // Per-call namespace override.
  assert(widcc_jit_compile_ns(jit, "n3.c",
    "int host_add(int,int);\n"
    "int addone(int x){ return host_add(x,1); }\n", ns) == 0);
  int (*addone)(int) = widcc_jit_symbol(jit, "addone");
  assert(addone && addone(41) == 42);
  widcc_jit_free(jit);

  widcc_ns_clear(ns);
  assert(widcc_ns_get(ns, "host_add") == NULL);
  widcc_ns_free(ns);

  // 6. Error recovery: failed compiles must release their scratch state.
  jit = widcc_jit_new();
  for (int i = 0; i < 5; i++) {
    assert(widcc_jit_compile(jit, "bad.c", "int broken( {") == -1);
    assert(strlen(widcc_jit_error(jit)) > 0);
  }
  assert(widcc_jit_compile(jit, "good.c",
    "int ok(void){ return 42; }\n") == 0);
  int (*ok)(void) = widcc_jit_symbol(jit, "ok");
  assert(ok && ok() == 42);
  widcc_jit_free(jit);

  // 7. Several live instances do not share ownership.
  WidccJIT *a = widcc_jit_new();
  WidccJIT *b = widcc_jit_new();
  assert(a && b);
  assert(widcc_jit_compile(a, "a.c", "int fa(void){ return 1; }\n") == 0);
  assert(widcc_jit_compile(b, "b.c", "int fb(void){ return 2; }\n") == 0);
  assert(((int (*)(void))widcc_jit_symbol(a, "fa"))() == 1);
  assert(((int (*)(void))widcc_jit_symbol(b, "fb"))() == 2);
  widcc_jit_free(b);
  widcc_jit_free(a);

  printf("mem: all tests passed\n");
  return 0;
}