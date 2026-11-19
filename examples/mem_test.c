// Memory lifecycle test: exercises every public API path — runtime symbol
// registration, module load/unload, forward declarations across modules
// and error recovery — then releases everything. Run under LeakSanitizer
// (build with -DWIDCC_SANITIZE=ON) to verify that runtimes and modules
// reclaim all memory independently.
#include "widcc_jit.h"
#include <stdio.h>
#include <string.h>

// Always-evaluated check: unlike assert(), it survives -DNDEBUG, so API
// calls placed inside it still run in release builds.
#define CHECK(expr)                                                         \
  do {                                                                      \
    if (!(expr)) {                                                          \
      fprintf(stderr, "check failed at %s:%d: %s\n", __FILE__, __LINE__,    \
              #expr);                                                       \
      return 1;                                                             \
    }                                                                       \
  } while (0)

static int host_add(int a, int b) { return a + b; }

int main(void) {
  // 1. Empty runtime lifecycle (release is NULL-safe).
  WidccRT *rt = widcc_rt_new();
  CHECK(rt != NULL);
  widcc_rt_free(rt);
  widcc_rt_free(NULL);

  // 2. Repeated load / execute / destroy rounds.
  for (int round = 0; round < 3; round++) {
    rt = widcc_rt_new();
    CHECK(rt != NULL);
    CHECK(widcc_rt_load_code(rt, "t.c",
      "int g;\n"
      "int f(int x){ g+=x; return g*g; }\n"
      "double d(double a,double b){ return a*b+1.0; }\n", NULL) == NULL);
    int (*f)(int) = widcc_rt_lookup(rt, "f");
    double (*d)(double, double) = widcc_rt_lookup(rt, "d");
    CHECK(f && d);
    CHECK(f(3) == 9);
    CHECK(f(1) == 16);
    CHECK(d(2.0, 3.0) == 7.0);
    widcc_rt_free(rt);
  }

  // 3. Per-module unload, then reuse the runtime for another load.
  rt = widcc_rt_new();
  CHECK(rt != NULL);
  WidccCode *mod = NULL;
  CHECK(widcc_rt_load_code(rt, "a.c", "int one(void){ return 1; }\n",
                           &mod) == NULL);
  CHECK(mod != NULL);
  int (*one)(void) = widcc_rt_lookup(rt, "one");
  CHECK(one() == 1);
  CHECK(widcc_rt_unload_code(rt, mod) == NULL);
  // Symbols keep their last definition even though the code is gone.
  CHECK(widcc_rt_lookup(rt, "one") != NULL);
  CHECK(widcc_rt_load_code(rt, "b.c", "int two(void){ return 2; }\n",
                           NULL) == NULL);
  int (*two)(void) = widcc_rt_lookup(rt, "two");
  CHECK(two() == 2);
  widcc_rt_free(rt);

  // 4. Host registration, forward declarations across modules, and
  // module/runtime independence: code stays callable after the load that
  // produced it returns.
  rt = widcc_rt_new();
  CHECK(rt != NULL);
  CHECK(widcc_rt_add_func(rt, "host_add", (void *)host_add) == NULL);
  CHECK(widcc_rt_load_code(rt, "u1.c",
    "int host_add(int,int);\n"
    "int callee(int);\n"
    "int caller(int x){ return callee(x)+host_add(x,1); }\n", NULL) == NULL);
  CHECK(widcc_rt_load_code(rt, "u2.c",
    "int callee(int x){ return x*10; }\n"
    "int tag(void){ static char s[] = \"abc\"; return s[1]; }\n",
    NULL) == NULL);
  CHECK(widcc_rt_list_symbols(rt, NULL, 0) >= 4);
  int (*caller)(int) = widcc_rt_lookup(rt, "caller");
  CHECK(caller && caller(4) == 45);
  // Static data produced during compilation is module-owned too.
  int (*tag)(void) = widcc_rt_lookup(rt, "tag");
  CHECK(tag && tag() == 'b');
  widcc_rt_free(rt);

  // 5. Error recovery: failed loads release their scratch state while the
  // runtime stays usable.
  rt = widcc_rt_new();
  CHECK(rt != NULL);
  for (int i = 0; i < 5; i++) {
    const char *err = widcc_rt_load_code(rt, "bad.c", "int broken( {", NULL);
    CHECK(err && strlen(err) > 0);
  }
  CHECK(widcc_rt_load_code(rt, "good.c",
    "int ok(void){ return 42; }\n", NULL) == NULL);
  int (*ok)(void) = widcc_rt_lookup(rt, "ok");
  CHECK(ok && ok() == 42);
  widcc_rt_free(rt);

  // 6. Several live runtimes do not share ownership.
  WidccRT *ra = widcc_rt_new();
  WidccRT *rb = widcc_rt_new();
  CHECK(ra && rb);
  CHECK(widcc_rt_load_code(ra, "a.c", "int fa(void){ return 1; }\n",
                           NULL) == NULL);
  CHECK(widcc_rt_load_code(rb, "b.c", "int fb(void){ return 2; }\n",
                           NULL) == NULL);
  CHECK(((int (*)(void))widcc_rt_lookup(ra, "fa"))() == 1);
  CHECK(((int (*)(void))widcc_rt_lookup(rb, "fb"))() == 2);
  CHECK(widcc_rt_lookup(ra, "fb") == NULL);
  CHECK(widcc_rt_lookup(rb, "fa") == NULL);
  widcc_rt_free(rb);
  widcc_rt_free(ra);

  printf("mem: all tests passed\n");
  return 0;
}