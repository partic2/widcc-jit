// Functional test for the widcc JIT runtime (widcc_jit.h).
//
// Covers: arithmetic, control flow, functions/recursion, forward references
// across translation units, structs, arrays, pointers, strings, global
// variables, host symbol mapping, repeated compilation, namespace-based
// external symbol sharing across multiple JIT instances, and tiered
// (code / runtime / arena) release.
#include "widcc_jit.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK_COMPILE(jit, name, src)                                       \
  do {                                                                      \
    if (widcc_jit_compile(jit, name, src) != 0) {                          \
      fprintf(stderr, "compile %s failed: %s\n", name,                     \
              widcc_jit_error(jit));                                       \
      exit(1);                                                              \
    }                                                                       \
  } while (0)

// The JIT follows the host data model: LP64/LLP64 (8-byte pointers) or
// ILP32 (4-byte pointers and long).
#if defined(__LP64__) || defined(_LP64) || defined(_WIN64)
#define HOST_64BIT 1
#else
#define HOST_64BIT 0
#endif

// Host symbols.
static int host_add(int a, int b) { return a + b; }
static volatile int host_counter = 0;
static void host_bump(void) { host_counter++; }

static void test_arith_control(WidccJIT *jit) {
  CHECK_COMPILE(jit, "arith.c",
    "int arith(int a, int b) { return a * b + (a - b) % 7; }\n"
    "int branches(int x) {\n"
    "  int r = 0;\n"
    "  if (x > 10) r = 1; else r = 2;\n"
    "  for (int i = 0; i < x; i++) r += i;\n"
    "  while (x > 0) { x -= 3; r++; }\n"
    "  do r++; while (r < 0);\n"
    "  return r;\n"
    "}\n"
    "int sw(int x) {\n"
    "  switch (x) {\n"
    "  case 1: return 11;\n"
    "  case 2: case 3: return 23;\n"
    "  case 5 ... 8: return 58;\n"
    "  default: return 99;\n"
    "  }\n"
    "}\n"
    "int gto(int x) {\n"
    "  int r = 0;\n"
    "again:\n"
    "  if (x > 0) { r += x; x--; goto again; }\n"
    "  return r;\n"
    "}\n"
    "int tern(int x) { return x > 0 ? x : -x; }\n"
    "int logi(int x) { return (x > 0 && x < 10) || x == 100; }\n");

  int (*arith)(int, int) = widcc_jit_symbol(jit, "arith");
  int (*branches)(int) = widcc_jit_symbol(jit, "branches");
  int (*sw)(int) = widcc_jit_symbol(jit, "sw");
  int (*gto)(int) = widcc_jit_symbol(jit, "gto");
  int (*tern)(int) = widcc_jit_symbol(jit, "tern");
  int (*logi)(int) = widcc_jit_symbol(jit, "logi");
  assert(arith && branches && sw && gto && tern && logi);

  assert(arith(5, 3) == 5 * 3 + (5 - 3) % 7);
  // r=2(if)+6(for:0+1+2+3)+2(while, 4->1->-2)+1(do) = 11
  assert(branches(4) == 2 + (0 + 1 + 2 + 3) + 2 + 1);
  assert(sw(1) == 11);
  assert(sw(3) == 23);
  assert(sw(7) == 58);
  assert(sw(42) == 99);
  assert(gto(5) == 15);
  assert(tern(-9) == 9);
  assert(logi(5) == 1 && logi(100) == 1 && logi(50) == 0);
}

static void test_func_recursion(WidccJIT *jit) {
  CHECK_COMPILE(jit, "rec.c",
    "int fib(int n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }\n"
    "static int helper(int x) { return x * 3; }\n"
    "int use_static(int x) { return helper(x) + 1; }\n");

  int (*fib)(int) = widcc_jit_symbol(jit, "fib");
  int (*use_static)(int) = widcc_jit_symbol(jit, "use_static");
  assert(fib && use_static);
  assert(fib(10) == 55);
  assert(use_static(5) == 16);
  assert(widcc_jit_symbol(jit, "helper") == NULL);
}

static void test_forward_ref(WidccJIT *jit) {
  CHECK_COMPILE(jit, "fwd1.c",
    "int is_even(int n);\n"
    "int is_odd(int n) { return n == 0 ? 0 : is_even(n - 1); }\n");
  CHECK_COMPILE(jit, "fwd2.c",
    "int is_odd(int n);\n"
    "int is_even(int n) { return n == 0 ? 1 : is_odd(n - 1); }\n");

  int (*is_even)(int) = widcc_jit_symbol(jit, "is_even");
  int (*is_odd)(int) = widcc_jit_symbol(jit, "is_odd");
  assert(is_even && is_odd);
  assert(is_even(10) == 1 && is_even(7) == 0);
  assert(is_odd(9) == 1 && is_odd(4) == 0);
}

static void test_struct_array_ptr(WidccJIT *jit) {
  CHECK_COMPILE(jit, "sap.c",
    "struct Point { int x; int y; char tag; };\n"
    "int sum_points(struct Point *ps, int n) {\n"
    "  int s = 0;\n"
    "  for (int i = 0; i < n; i++) s += ps[i].x + ps[i].y + ps[i].tag;\n"
    "  return s;\n"
    "}\n"
    "int deref(int **pp) { return **pp; }\n"
    "int ptr_arith(int *arr) { return *(arr + 2) - *(arr - 1); }\n"
    "int local_arr(void) {\n"
    "  int a[5];\n"
    "  for (int i = 0; i < 5; i++) a[i] = i * i;\n"
    "  int s = 0;\n"
    "  for (int i = 0; i < 5; i++) s += a[i];\n"
    "  return s;\n"
    "}\n"
    "struct Point gpoint;\n"
    "int copy_point(struct Point *src) {\n"
    "  gpoint = *src;\n"
    "  return gpoint.x + gpoint.y + gpoint.tag;\n"
    "}\n"
    "int copy_point_local(struct Point *src) {\n"
    "  struct Point tmp = *src;\n"
    "  return tmp.x * 100 + tmp.y * 10 + tmp.tag;\n"
    "}\n");

  struct { int x, y; char tag; } ps[3] = {{1, 2, 3}, {4, 5, 6}, {7, 8, 9}};
  int (*sum_points)(void *, int) = widcc_jit_symbol(jit, "sum_points");
  int (*deref)(int **) = widcc_jit_symbol(jit, "deref");
  int (*ptr_arith)(int *) = widcc_jit_symbol(jit, "ptr_arith");
  int (*local_arr)(void) = widcc_jit_symbol(jit, "local_arr");
  int (*copy_point)(void *) = widcc_jit_symbol(jit, "copy_point");
  int (*copy_point_local)(void *) = widcc_jit_symbol(jit, "copy_point_local");
  assert(sum_points && deref && ptr_arith && local_arr);
  assert(copy_point && copy_point_local);
  assert(sum_points(ps, 3) == 1 + 2 + 3 + 4 + 5 + 6 + 7 + 8 + 9);

  int v = 1234;
  int *p = &v;
  assert(deref(&p) == 1234);

  int arr[5] = {10, 20, 30, 40, 50};
  assert(ptr_arith(&arr[1]) == 40 - 10);
  assert(local_arr() == 0 + 1 + 4 + 9 + 16);

  // Whole-struct assignment: RHS is a dereference, so the destination address
  // and the copy temp must not collide.
  struct { int x, y; char tag; } one = {3, 4, 5};
  assert(copy_point(&one) == 3 + 4 + 5);
  assert(copy_point_local(&one) == 3 * 100 + 4 * 10 + 5);
}

static void test_strings_globals(WidccJIT *jit) {
  CHECK_COMPILE(jit, "str.c",
    "char *greeting = \"hello, jit\";\n"
    "int counter = 41;\n"
    "int counter2;\n"
    "char buf[32];\n"
    "int bump(void) { counter++; return counter; }\n"
    "char first_char(void) { return greeting[0]; }\n"
    "int str_len(char *s) { int n = 0; while (s[n]) n++; return n; }\n"
    "int *counter_addr(void) { return &counter; }\n"
    "void fill_buf(void) { buf[0] = 'x'; buf[1] = 'y'; buf[2] = 0; }\n");
  CHECK_COMPILE(jit, "str2.c",
    "int counter2;\n"
    "void set_counter2(int v) { counter2 = v; }\n");

  int (*bump)(void) = widcc_jit_symbol(jit, "bump");
  char (*first_char)(void) = widcc_jit_symbol(jit, "first_char");
  int (*str_len)(char *) = widcc_jit_symbol(jit, "str_len");
  int *(*counter_addr)(void) = widcc_jit_symbol(jit, "counter_addr");
  void (*fill_buf)(void) = widcc_jit_symbol(jit, "fill_buf");
  void (*set_counter2)(int) = widcc_jit_symbol(jit, "set_counter2");
  int *counter2p = widcc_jit_symbol(jit, "counter2");
  char *buf = widcc_jit_symbol(jit, "buf");
  assert(bump && first_char && str_len && counter_addr && fill_buf);
  assert(set_counter2 && counter2p && buf);

  assert(first_char() == 'h');
  assert(str_len("abcdef") == 6);
  assert(bump() == 42 && bump() == 43);
  assert(*counter_addr() == 43);
  fill_buf();
  assert(buf[0] == 'x' && buf[1] == 'y' && buf[2] == 0);
  set_counter2(777);
  assert(*counter2p == 777);
}

static void test_float(WidccJIT *jit) {
  CHECK_COMPILE(jit, "float.c",
    "double dadd(double a, double b) { return a + b; }\n"
    "float fmul(float a, float b) { return a * b; }\n"
    "double mix(int i, double d, float f) { return i + d + f; }\n"
    "int dcmp(double a, double b) { return a < b ? -1 : (a > b ? 1 : 0); }\n"
    "double ddiv(double a, double b) { return a / b; }\n");

  double (*dadd)(double, double) = widcc_jit_symbol(jit, "dadd");
  float (*fmul)(float, float) = widcc_jit_symbol(jit, "fmul");
  double (*mix)(int, double, float) = widcc_jit_symbol(jit, "mix");
  int (*dcmp)(double, double) = widcc_jit_symbol(jit, "dcmp");
  double (*ddiv)(double, double) = widcc_jit_symbol(jit, "ddiv");
  assert(dadd && fmul && mix && dcmp && ddiv);

  assert(dadd(1.5, 2.25) == 3.75);
  assert(fmul(2.0f, 3.5f) == 7.0f);
  assert(mix(1, 0.5, 0.25f) == 1.75);
  assert(dcmp(1.0, 2.0) == -1 && dcmp(2.0, 1.0) == 1 && dcmp(1.0, 1.0) == 0);
  assert(ddiv(7.0, 2.0) == 3.5);
}

static void test_host_symbols(WidccJIT *jit) {
  assert(widcc_jit_map_symbol(jit, "host_add", host_add) == 0);
  assert(widcc_jit_map_symbol(jit, "host_bump", host_bump) == 0);
  assert(widcc_jit_map_symbol(jit, "host_counter", (void *)&host_counter) == 0);

  CHECK_COMPILE(jit, "host.c",
    "int host_add(int a, int b);\n"
    "void host_bump(void);\n"
    "extern int host_counter;\n"
    "int call_host(int x) { return host_add(x, 100); }\n"
    "int bump_host(void) { host_bump(); host_bump(); return host_counter; }\n");

  int (*call_host)(int) = widcc_jit_symbol(jit, "call_host");
  int (*bump_host)(void) = widcc_jit_symbol(jit, "bump_host");
  assert(call_host && bump_host);
  assert(call_host(23) == 123);
  int before = host_counter;
  assert(bump_host() == before + 2);
}

static void test_recompile(WidccJIT *jit) {
  CHECK_COMPILE(jit, "v1.c", "int version(void) { return 1; }\n");
  int (*v1)(void) = widcc_jit_symbol(jit, "version");
  assert(v1 && v1() == 1);

  CHECK_COMPILE(jit, "v2.c", "int version(void) { return 2; }\n");
  int (*v2)(void) = widcc_jit_symbol(jit, "version");
  assert(v2 && v2() == 2);
  assert(v1() == 1);
}

static void test_error_recovery(WidccJIT *jit) {
  assert(widcc_jit_compile(jit, "bad.c", "int broken( { return ;") == -1);
  assert(strlen(widcc_jit_error(jit)) > 0);
  CHECK_COMPILE(jit, "ok.c", "int still_ok(void) { return 7; }\n");
  int (*still_ok)(void) = widcc_jit_symbol(jit, "still_ok");
  assert(still_ok && still_ok() == 7);
}

// --- Namespace tests ---

// Static helper for test_namespace_basic's foreach callback (C has no lambdas).
static int ns_count_cb(WidccNamespace *ns, const char *name,
                       void *addr, void *user) {
  (void)ns; (void)name; (void)addr;
  int *s = (int *)user;
  (*s)++;
  return 0;
}

static void test_namespace_basic(void) {
  WidccNamespace *ns = widcc_ns_new();
  assert(ns);

  // Put / get / del.
  int magic = 0xDEAD;
  assert(widcc_ns_put(ns, "magic", &magic) == 0);
  assert(widcc_ns_get(ns, "magic") == &magic);
  assert(widcc_ns_del(ns, "magic") == 0);
  assert(widcc_ns_get(ns, "magic") == NULL);
  assert(widcc_ns_del(ns, "magic") == -1);

  // Overwrite.
  int a = 1, b = 2;
  widcc_ns_put(ns, "x", &a);
  widcc_ns_put(ns, "x", &b);
  assert(widcc_ns_get(ns, "x") == &b);

  // Clear.
  widcc_ns_clear(ns);
  assert(widcc_ns_get(ns, "x") == NULL);

  // Iterate.
  widcc_ns_put(ns, "a", &a);
  widcc_ns_put(ns, "b", &b);
  int count = 0;
  widcc_ns_foreach(ns, ns_count_cb, &count);
  assert(count == 2);

  widcc_ns_free(ns);
}

static void test_namespace_cross_jit(void) {
  // Two JIT instances share one namespace, no attach sync.
  WidccNamespace *ns = widcc_ns_new();

  WidccJIT *jit1 = widcc_jit_new();
  WidccJIT *jit2 = widcc_jit_new();

  // jit1 defines a function; compile pushes it into ns because sync=1.
  widcc_jit_attach_ns(jit1, ns, /*sync=*/1);
  CHECK_COMPILE(jit1, "shared.c",
    "int double_it(int x) { return x * 2; }\n"
    "int shared_counter = 100;\n");

  // jit2 (attached with sync=0) can still resolve via the shared ns
  // because resolution falls back to ns regardless of sync setting.
  widcc_jit_attach_ns(jit2, ns, /*sync=*/0);
  CHECK_COMPILE(jit2, "use_shared.c",
    "int double_it(int x);\n"
    "extern int shared_counter;\n"
    "int use(int x) { return double_it(x) + shared_counter; }\n");

  int (*use)(int) = widcc_jit_symbol(jit2, "use");
  assert(use && use(5) == 110);

  widcc_jit_free(jit1);
  widcc_jit_free(jit2);
  widcc_ns_free(ns);
}

static void test_compile_ns_override(void) {
  // compile_ns overrides the attached namespace for one call.
  WidccJIT *jit = widcc_jit_new();
  WidccNamespace *ns = widcc_ns_new();

  widcc_jit_attach_ns(jit, ns, 0);

  // Put a stub in the ns before compile so the TU sees it.
  static int stub_sentinel = 12345;
  widcc_ns_put(ns, "sentinel", &stub_sentinel);
  assert(widcc_jit_compile_ns(jit, "use_ns.c",
    "extern int sentinel;\n"
    "int pick_sentinel(void) { return sentinel; }\n",
    ns) == 0);

  int *sentinel_addr = widcc_jit_symbol(jit, "sentinel");
  // compile_ns forced sync, so the JIT's own table should have it.
  assert(sentinel_addr == &stub_sentinel);
  (void)sentinel_addr;

  widcc_jit_free(jit);
  widcc_ns_free(ns);
}

static void test_tiered_release(void) {
  WidccJIT *jit = widcc_jit_new();

  // Compile something — tier 1 (code) + tier 2 (rt data) both produced.
  CHECK_COMPILE(jit, "tiers.c",
    "int counter = 5;\n"
    "int add1(int x) { return x + 1; }\n");

  int (*add1)(int) = widcc_jit_symbol(jit, "add1");
  assert(add1 && add1(9) == 10);

  // Tier 1 only: code is gone, rt data still alive.
  widcc_jit_free_code(jit);

  // External symbol lookup still reaches data via rt-owned cell.
  int *c = widcc_jit_symbol(jit, "counter");
  assert(c && *c == 5);

  // Tier 2 only: rt data gone, instance symbol table still exists
  // (backed by arena). Recompilation re-fills both tiers.
  widcc_jit_free_rt(jit);

  // Recompile after tier-1+tier-2 release: should succeed and produce
  // fresh code + fresh rt data.
  CHECK_COMPILE(jit, "tiers2.c",
    "int counter2 = 42;\n"
    "int times2(int x) { return x * 2; }\n");
  int (*times2)(int) = widcc_jit_symbol(jit, "times2");
  int *c2 = widcc_jit_symbol(jit, "counter2");
  assert(times2 && times2(21) == 42);
  assert(c2 && *c2 == 42);

  // Tier 3 (arena): must release code + rt first.
  widcc_jit_free_code(jit);
  widcc_jit_free_rt(jit);
  widcc_jit_free_arena(jit);

  // After arena release, instance is reusable (sym table was reset).
  CHECK_COMPILE(jit, "tiers3.c", "int hello(void) { return 99; }\n");
  int (*hello)(void) = widcc_jit_symbol(jit, "hello");
  assert(hello && hello() == 99);

  widcc_jit_free(jit);
}

// --- Additional algorithm / integer-width tests ---

static void test_sorting(WidccJIT *jit) {
  CHECK_COMPILE(jit, "sort.c",
    "void bubble(int *a, int n) {\n"
    "  for (int i = 0; i < n - 1; i++)\n"
    "    for (int j = 0; j < n - 1 - i; j++)\n"
    "      if (a[j] > a[j + 1]) {\n"
    "        int t = a[j]; a[j] = a[j + 1]; a[j + 1] = t;\n"
    "      }\n"
    "}\n"
    "void insertion(int *a, int n) {\n"
    "  for (int i = 1; i < n; i++) {\n"
    "    int key = a[i];\n"
    "    int j = i - 1;\n"
    "    while (j >= 0 && a[j] > key) { a[j + 1] = a[j]; j--; }\n"
    "    a[j + 1] = key;\n"
    "  }\n"
    "}\n"
    "int is_sorted(int *a, int n) {\n"
    "  for (int i = 1; i < n; i++)\n"
    "    if (a[i - 1] > a[i]) return 0;\n"
    "  return 1;\n"
    "}\n"
    "int local_bubble_ok(void) {\n"
    "  int a[10];\n"
    "  for (int i = 0; i < 10; i++) a[i] = (i * 7 + 3) % 11;\n"
    "  bubble(a, 10);\n"
    "  return is_sorted(a, 10);\n"
    "}\n"
    "int sum_after_sort(int *a, int n) {\n"
    "  bubble(a, n);\n"
    "  int s = 0;\n"
    "  for (int i = 0; i < n; i++) s += a[i];\n"
    "  return s;\n"
    "}\n");

  void (*bubble)(int *, int) = widcc_jit_symbol(jit, "bubble");
  void (*insertion)(int *, int) = widcc_jit_symbol(jit, "insertion");
  int (*is_sorted)(int *, int) = widcc_jit_symbol(jit, "is_sorted");
  int (*local_bubble_ok)(void) = widcc_jit_symbol(jit, "local_bubble_ok");
  int (*sum_after_sort)(int *, int) = widcc_jit_symbol(jit, "sum_after_sort");
  assert(bubble && insertion && is_sorted && local_bubble_ok && sum_after_sort);

  enum { N = 64 };
  int a[N], b[N], c[N];
  int total = 0;
  for (int i = 0; i < N; i++) {
    a[i] = ((i * 37 + 11) % 101) - 50;
    b[i] = a[i];
    c[i] = a[i];
    total += a[i];
  }
  bubble(a, N);
  insertion(b, N);
  assert(is_sorted(a, N) == 1);
  assert(is_sorted(b, N) == 1);
  for (int i = 0; i < N; i++)
    assert(a[i] == b[i]);

  // Sorting must be a permutation: the sum is unchanged.
  assert(sum_after_sort(c, N) == total);
  assert(local_bubble_ok() == 1);
}

static void test_nested_loops(WidccJIT *jit) {
  CHECK_COMPILE(jit, "loops.c",
    "int nested(int n) {\n"
    "  int c = 0;\n"
    "  for (int i = 0; i < n; i++) {\n"
    "    if (i % 2 == 0) continue;\n"
    "    for (int j = 0; j < n; j++) {\n"
    "      if (j > i) break;\n"
    "      c += i * j + 1;\n"
    "    }\n"
    "  }\n"
    "  return c;\n"
    "}\n"
    "int dowhile(int n) {\n"
    "  int s = 0;\n"
    "  do { s += n; n -= 1; } while (n > 0 && s < 100);\n"
    "  return s;\n"
    "}\n"
    "int matrix_trace(int n) {\n"
    "  int m[5][5];\n"
    "  for (int i = 0; i < n; i++)\n"
    "    for (int j = 0; j < n; j++)\n"
    "      m[i][j] = i * n + j;\n"
    "  int t = 0;\n"
    "  for (int i = 0; i < n; i++) t += m[i][i];\n"
    "  return t;\n"
    "}\n");

  int (*nested)(int) = widcc_jit_symbol(jit, "nested");
  int (*dowhile)(int) = widcc_jit_symbol(jit, "dowhile");
  int (*matrix_trace)(int) = widcc_jit_symbol(jit, "matrix_trace");
  assert(nested && dowhile && matrix_trace);

  // Reference values computed with the same semantics on the host.
  for (int n = 0; n <= 9; n++) {
    int c = 0;
    for (int i = 0; i < n; i++) {
      if (i % 2 == 0) continue;
      for (int j = 0; j < n; j++) {
        if (j > i) break;
        c += i * j + 1;
      }
    }
    assert(nested(n) == c);
  }

  int s = 0, m = 13;
  do { s += m; m -= 1; } while (m > 0 && s < 100);
  assert(dowhile(13) == s);

  assert(matrix_trace(5) == 0 + 6 + 12 + 18 + 24);
}

static void test_int32_semantics(WidccJIT *jit) {
  CHECK_COMPILE(jit, "int32.c",
    "unsigned udiv(unsigned a, unsigned b) { return a / b; }\n"
    "unsigned umod(unsigned a, unsigned b) { return a % b; }\n"
    "int ult(unsigned a, unsigned b) { return a < b; }\n"
    "int slt(int a, int b) { return a < b; }\n"
    "unsigned ushr(unsigned x, int n) { return x >> n; }\n"
    "int sshr(int x, int n) { return x >> n; }\n"
    "int shl_wrap(int x) { return x << 4; }\n"
    "int add_wrap(int x) { return x + 1; }\n"
    "int trunc_char(int x) { char c = x; return c; }\n"
    "int trunc_short(int x) { short s = x; return s; }\n"
    "int uchar_wrap(void) { unsigned char c = 250; c = c + 10; return c; }\n");

  unsigned (*udiv)(unsigned, unsigned) = widcc_jit_symbol(jit, "udiv");
  unsigned (*umod)(unsigned, unsigned) = widcc_jit_symbol(jit, "umod");
  int (*ult)(unsigned, unsigned) = widcc_jit_symbol(jit, "ult");
  int (*slt)(int, int) = widcc_jit_symbol(jit, "slt");
  unsigned (*ushr)(unsigned, int) = widcc_jit_symbol(jit, "ushr");
  int (*sshr)(int, int) = widcc_jit_symbol(jit, "sshr");
  int (*shl_wrap)(int) = widcc_jit_symbol(jit, "shl_wrap");
  int (*add_wrap)(int) = widcc_jit_symbol(jit, "add_wrap");
  int (*trunc_char)(int) = widcc_jit_symbol(jit, "trunc_char");
  int (*trunc_short)(int) = widcc_jit_symbol(jit, "trunc_short");
  int (*uchar_wrap)(void) = widcc_jit_symbol(jit, "uchar_wrap");
  assert(udiv && umod && ult && slt && ushr && sshr && shl_wrap && add_wrap &&
         trunc_char && trunc_short && uchar_wrap);

  // Unsigned 32-bit division/modulo must not be emitted as signed.
  assert(udiv(0xFFFFFFF0u, 3u) == 0xFFFFFFF0u / 3u);
  assert(udiv(0x80000000u, 2u) == 0x40000000u);
  assert(umod(0xFFFFFFF5u, 7u) == 0xFFFFFFF5u % 7u);
  assert(udiv(100u, 0xFFFFFFFFu) == 0u);

  // Signed vs unsigned comparison.
  assert(ult(0xFFFFFFFFu, 1u) == 0);
  assert(slt(-1, 1) == 1);

  // Logical vs arithmetic right shift, and 32-bit wraparound.
  assert(ushr(0xFFFFFFFFu, 4) == 0x0FFFFFFFu);
  assert(sshr(-16, 2) == -4);
  assert(shl_wrap(0x10000000) == 0);
  assert(add_wrap(0x7FFFFFFF) == (int)0x80000000);

  // Narrow integer store truncation and zero/sign extension on load.
  assert(trunc_char(300) == 44);
  assert(trunc_short(70000) == 4464);
  assert(uchar_wrap() == 4);
}

// 64-bit integers go through the wide path on a 64-bit host. On a 32-bit
// host the sljit backend emulates them: add/sub use ADDC/SUBC, bitwise ops
// and comparisons are open-coded, and mul/div/mod/shift/float conversions
// call host helpers. Parameters and return values stay rejected there, so
// the tests expose int/double interfaces and publish results through globals.
static void test_int64_semantics(WidccJIT *jit) {
#if HOST_64BIT
  CHECK_COMPILE(jit, "int64.c",
    "long long wide_mul(long long a, long long b) { return a * b; }\n"
    "long long wide_divmod(long long a, long long b) { return a / b + a % b; }\n");

  long long (*wide_mul)(long long, long long) = widcc_jit_symbol(jit, "wide_mul");
  long long (*wide_divmod)(long long, long long) =
      widcc_jit_symbol(jit, "wide_divmod");
  assert(wide_mul && wide_divmod);

  // 64-bit operands still use the wide path.
  assert(wide_mul(3037000499LL, 3LL) == 3037000499LL * 3LL);
  assert(wide_divmod(10000000000LL, 7LL) == 10000000000LL / 7LL + 10000000000LL % 7LL);
#else
  // i64 parameters/returns are still a compile-time error on a 32-bit host.
  assert(widcc_jit_compile(jit, "int64_param.c",
    "long long wide_mul(long long a, long long b) { return a * b; }\n") == -1);
  assert(strstr(widcc_jit_error(jit), "64-bit") != NULL);

  CHECK_COMPILE(jit, "int64_32.c",
    "long long g_add, g_sub, g_mul, g_div, g_mod;\n"
    "long long g_and, g_or, g_xor, g_neg, g_not, g_shl, g_shr, g_sar;\n"
    "long long g_loop, g_chain;\n"
    "void i64_binops(void) {\n"
    "  long long a = 0x123456789abcdef;\n"
    "  long long b = 0x111111111111111;\n"
    "  g_add = a + b;\n"
    "  g_sub = a - b;\n"
    "  g_mul = a * b;\n"
    "  g_div = a / b;\n"
    "  g_mod = a % b;\n"
    "  g_and = a & b;\n"
    "  g_or  = a | b;\n"
    "  g_xor = a ^ b;\n"
    "  g_neg = -a;\n"
    "  g_not = ~a;\n"
    "  g_shl = a << 5;\n"
    "  g_shr = a >> 5;\n"
    "  g_sar = (-a) >> 5;\n"
    "}\n"
    "int i64_cmps(void) {\n"
    "  long long a = 0x200000000;\n"
    "  long long b = 0x1ffffffff;\n"
    "  unsigned long long ua = a, ub = b;\n"
    "  int r = 0;\n"
    "  if (a > b) r += 1;\n"
    "  if (b < a) r += 2;\n"
    "  if (a != b) r += 4;\n"
    "  if (!(a == b)) r += 8;\n"
    "  if (ua > ub) r += 16;\n"
    "  if (ub <= ua) r += 32;\n"
    "  return r;\n"
    "}\n"
    "void i64_sum(int n) {\n"
    "  long long s = 0;\n"
    "  for (int i = 1; i <= n; i++) s = s + i;\n"
    "  g_loop = s * 0x100000000;\n"
    "}\n"
    "void i64_assign(void) {\n"
    "  long long x, y;\n"
    "  x = y = 0x100000007;\n"
    "  g_chain = x + y;\n"
    "}\n"
    "int i64_logic(void) {\n"
    "  long long z = 0;\n"
    "  long long n = 1;\n"
    "  int r = 0;\n"
    "  if (z || n) r += 1;\n"
    "  if (z && n) r += 2;\n"
    "  if (n && (z + 1)) r += 4;\n"
    "  return r;\n"
    "}\n"
    "int i64_cond(int c) {\n"
    "  long long x = c ? 0x100000000 : 0x2;\n"
    "  return (int)(x >> 32);\n"
    "}\n"
    "int i64_trunc(void) {\n"
    "  long long x = 0x123456789;\n"
    "  return (int)x;\n"
    "}\n"
    "int i64_from_int(int v) {\n"
    "  long long x = v;\n"
    "  return (int)(x >> 32);\n"
    "}\n"
    "double i64_to_d(void) {\n"
    "  long long x = 0x100000001;\n"
    "  return (double)x;\n"
    "}\n"
    "int i64_from_d(double d) {\n"
    "  long long x = (long long)d;\n"
    "  return (int)(x >> 32);\n"
    "}\n"
    "int g_index[4];\n"
    "int i64_index(void) {\n"
    "  for (int i = 0; i < 4; i++) g_index[i] = (i + 1) * 10;\n"
    "  long long i = 2;\n"
    "  return g_index[i] + g_index[i - 1];\n"
    "}\n");

  void (*i64_binops)(void) = widcc_jit_symbol(jit, "i64_binops");
  int (*i64_cmps)(void) = widcc_jit_symbol(jit, "i64_cmps");
  void (*i64_sum)(int) = widcc_jit_symbol(jit, "i64_sum");
  void (*i64_assign)(void) = widcc_jit_symbol(jit, "i64_assign");
  int (*i64_logic)(void) = widcc_jit_symbol(jit, "i64_logic");
  int (*i64_cond)(int) = widcc_jit_symbol(jit, "i64_cond");
  int (*i64_trunc)(void) = widcc_jit_symbol(jit, "i64_trunc");
  int (*i64_from_int)(int) = widcc_jit_symbol(jit, "i64_from_int");
  double (*i64_to_d)(void) = widcc_jit_symbol(jit, "i64_to_d");
  int (*i64_from_d)(double) = widcc_jit_symbol(jit, "i64_from_d");
  int (*i64_index)(void) = widcc_jit_symbol(jit, "i64_index");
  long long *g_add = widcc_jit_symbol(jit, "g_add");
  long long *g_sub = widcc_jit_symbol(jit, "g_sub");
  long long *g_mul = widcc_jit_symbol(jit, "g_mul");
  long long *g_div = widcc_jit_symbol(jit, "g_div");
  long long *g_mod = widcc_jit_symbol(jit, "g_mod");
  long long *g_and = widcc_jit_symbol(jit, "g_and");
  long long *g_or = widcc_jit_symbol(jit, "g_or");
  long long *g_xor = widcc_jit_symbol(jit, "g_xor");
  long long *g_neg = widcc_jit_symbol(jit, "g_neg");
  long long *g_not = widcc_jit_symbol(jit, "g_not");
  long long *g_shl = widcc_jit_symbol(jit, "g_shl");
  long long *g_shr = widcc_jit_symbol(jit, "g_shr");
  long long *g_sar = widcc_jit_symbol(jit, "g_sar");
  long long *g_loop = widcc_jit_symbol(jit, "g_loop");
  long long *g_chain = widcc_jit_symbol(jit, "g_chain");
  assert(i64_binops && i64_cmps && i64_sum && i64_assign && i64_logic &&
         i64_cond && i64_trunc && i64_from_int && i64_to_d && i64_from_d &&
         i64_index);
  assert(g_add && g_sub && g_mul && g_div && g_mod && g_and && g_or && g_xor &&
         g_neg && g_not && g_shl && g_shr && g_sar && g_loop && g_chain);

  const long long a = 0x123456789abcdefLL;
  const long long b = 0x111111111111111LL;
  i64_binops();
  assert(*g_add == a + b);
  assert(*g_sub == a - b);
  assert(*g_mul == a * b);
  assert(*g_div == a / b);
  assert(*g_mod == a % b);
  assert(*g_and == (a & b));
  assert(*g_or == (a | b));
  assert(*g_xor == (a ^ b));
  assert(*g_neg == -a);
  assert(*g_not == ~a);
  assert(*g_shl == (long long)((unsigned long long)a << 5));
  assert(*g_shr == a >> 5);
  assert(*g_sar == (-a) >> 5);

  assert(i64_cmps() == 63);
  i64_sum(100);
  assert(*g_loop == (long long)(5050LL * 0x100000000LL));
  i64_assign();
  assert(*g_chain == 0x20000000eLL);
  assert(i64_logic() == 5);
  assert(i64_cond(1) == 1);
  assert(i64_cond(0) == 0);
  assert(i64_trunc() == 0x23456789);
  assert(i64_from_int(-1) == -1);
  assert(i64_from_int(5) == 0);
  assert(i64_to_d() == 4294967297.0);
  assert(i64_from_d(4294967297.0) == 1);
  assert(i64_index() == 50);  // pointer arithmetic with a 64-bit index
#endif
}

static void test_data_model(WidccJIT *jit) {
  CHECK_COMPILE(jit, "model.c",
    "int model_sizes(void) {\n"
    "  return sizeof(void *) + sizeof(long) + sizeof(long long)\n"
    "       + sizeof(short) + sizeof(char);\n"
    "}\n"
    "int ptr_step(void) {\n"
    "  int a[4];\n"
    "  int *p = a;\n"
    "  int *q = p + 3;\n"
    "  return q - p;\n"
    "}\n");
  int (*model_sizes)(void) = widcc_jit_symbol(jit, "model_sizes");
  int (*ptr_step)(void) = widcc_jit_symbol(jit, "ptr_step");
  assert(model_sizes && ptr_step);

  // Pointers and `long` are 8 bytes on LP64/LLP64 and 4 bytes on ILP32;
  // `long long` is always 8.

  assert(model_sizes() == sizeof(void *) + sizeof(long) + sizeof(long long)
           + sizeof(short) + sizeof(char));

  assert(ptr_step() == 3);
}

int main(void) {
  WidccJIT *jit = widcc_jit_new();
  assert(jit);

  test_arith_control(jit);
  test_func_recursion(jit);
  test_forward_ref(jit);
  test_struct_array_ptr(jit);
  test_strings_globals(jit);
  test_float(jit);
  test_host_symbols(jit);
  test_recompile(jit);
  test_error_recovery(jit);
  test_sorting(jit);
  test_nested_loops(jit);
  test_int32_semantics(jit);
  test_int64_semantics(jit);
  test_data_model(jit);

  // Namespace tests.
  test_namespace_basic();
  test_namespace_cross_jit();
  test_compile_ns_override();
  test_tiered_release();

  // Two separate instances still work as before.
  WidccJIT *jit2 = widcc_jit_new();
  assert(jit2);
  CHECK_COMPILE(jit2, "other.c",
    "int version(void) { return 42; }\n"
    "int triple(int x) { return x * 3; }\n");
  int (*version2)(void) = widcc_jit_symbol(jit2, "version");
  int (*triple)(int) = widcc_jit_symbol(jit2, "triple");
  assert(version2 && version2() == 42);
  assert(triple && triple(5) == 15);

  int (*v)(void) = widcc_jit_symbol(jit, "version");
  assert(v && v() == 2);

  widcc_jit_free(jit2);
  widcc_jit_free(jit);

  printf("jit_test: all tests passed\n");
  return 0;
}
