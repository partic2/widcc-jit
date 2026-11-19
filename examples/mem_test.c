// Memory lifecycle test: exercises every public API path — compilation,
// code generation, symbol tables, tiered release and error recovery — and
// then releases everything. Run under LeakSanitizer (build with
// -DWIDCC_SANITIZE=ON) to verify that the runtime reclaims all memory.
#include "widcc_jit.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static int host_add(int a, int b) { return a + b; }

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

  // 5. Error recovery: failed compiles must release their scratch state.
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

  // 6. Several live instances do not share ownership.
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