// Functional test for the widcc JIT runtime (widcc_jit.h).
//
// Covers: arithmetic, control flow, functions/recursion, forward references
// across translation units, structs, arrays, pointers, strings, global
// variables, host symbol registration (including per-runtime resolvers),
// repeated loads, per-module unload, includes and virtual file I/O.
#include "widcc_jit.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The runtime under test; modules are loaded into it.
typedef struct {
  WidccRT *rt;
} JIT;

// Always-evaluated check: unlike assert(), it survives -DNDEBUG, so API
// calls placed inside it are never compiled out.
#define CHECK(expr)                                                         \
  do {                                                                      \
    if (!(expr)) {                                                          \
      fprintf(stderr, "check failed at %s:%d: %s\n", __FILE__, __LINE__,    \
              #expr);                                                       \
      exit(1);                                                              \
    }                                                                       \
  } while (0)

#define CHECK_COMPILE(jit, name, src)                                       \
  do {                                                                      \
    const char *err = widcc_rt_load_code((jit)->rt, name, src, NULL);       \
    if (err) {                                                              \
      fprintf(stderr, "load %s failed: %s\n", name, err);                   \
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
static int host_sub(int a, int b) { return a - b; }
static volatile int host_counter = 0;
static void host_bump(void) { host_counter++; }

// Per-runtime resolvers: both claim "picked", with different targets.
static void *resolve_add(WidccRT *rt, const char *name) {
  if (!strcmp(name, "picked"))
    return (void *)host_add;
  return widcc_rt_default_host_resolver(rt, name);
}

static void *resolve_sub(WidccRT *rt, const char *name) {
  if (!strcmp(name, "picked"))
    return (void *)host_sub;
  return widcc_rt_default_host_resolver(rt, name);
}

static void test_arith_control(JIT *jit) {
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

  int (*arith)(int, int) = widcc_rt_lookup(jit->rt, "arith");
  int (*branches)(int) = widcc_rt_lookup(jit->rt, "branches");
  int (*sw)(int) = widcc_rt_lookup(jit->rt, "sw");
  int (*gto)(int) = widcc_rt_lookup(jit->rt, "gto");
  int (*tern)(int) = widcc_rt_lookup(jit->rt, "tern");
  int (*logi)(int) = widcc_rt_lookup(jit->rt, "logi");
  CHECK(arith && branches && sw && gto && tern && logi);

  CHECK(arith(5, 3) == 5 * 3 + (5 - 3) % 7);
  // r=2(if)+6(for:0+1+2+3)+2(while, 4->1->-2)+1(do) = 11
  CHECK(branches(4) == 2 + (0 + 1 + 2 + 3) + 2 + 1);
  CHECK(sw(1) == 11);
  CHECK(sw(3) == 23);
  CHECK(sw(7) == 58);
  CHECK(sw(42) == 99);
  CHECK(gto(5) == 15);
  CHECK(tern(-9) == 9);
  CHECK(logi(5) == 1 && logi(100) == 1 && logi(50) == 0);
}

static void test_func_recursion(JIT *jit) {
  CHECK_COMPILE(jit, "rec.c",
    "int fib(int n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }\n"
    "static int helper(int x) { return x * 3; }\n"
    "int use_static(int x) { return helper(x) + 1; }\n");

  int (*fib)(int) = widcc_rt_lookup(jit->rt, "fib");
  int (*use_static)(int) = widcc_rt_lookup(jit->rt, "use_static");
  CHECK(fib && use_static);
  CHECK(fib(10) == 55);
  CHECK(use_static(5) == 16);
  CHECK(widcc_rt_lookup(jit->rt, "helper") == NULL);
}

static void test_forward_ref(JIT *jit) {
  CHECK_COMPILE(jit, "fwd1.c",
    "int is_even(int n);\n"
    "int is_odd(int n) { return n == 0 ? 0 : is_even(n - 1); }\n");
  CHECK_COMPILE(jit, "fwd2.c",
    "int is_odd(int n);\n"
    "int is_even(int n) { return n == 0 ? 1 : is_odd(n - 1); }\n");

  int (*is_even)(int) = widcc_rt_lookup(jit->rt, "is_even");
  int (*is_odd)(int) = widcc_rt_lookup(jit->rt, "is_odd");
  CHECK(is_even && is_odd);
  CHECK(is_even(10) == 1 && is_even(7) == 0);
  CHECK(is_odd(9) == 1 && is_odd(4) == 0);
}

static void test_struct_array_ptr(JIT *jit) {
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
  int (*sum_points)(void *, int) = widcc_rt_lookup(jit->rt, "sum_points");
  int (*deref)(int **) = widcc_rt_lookup(jit->rt, "deref");
  int (*ptr_arith)(int *) = widcc_rt_lookup(jit->rt, "ptr_arith");
  int (*local_arr)(void) = widcc_rt_lookup(jit->rt, "local_arr");
  int (*copy_point)(void *) = widcc_rt_lookup(jit->rt, "copy_point");
  int (*copy_point_local)(void *) = widcc_rt_lookup(jit->rt, "copy_point_local");
  CHECK(sum_points && deref && ptr_arith && local_arr);
  CHECK(copy_point && copy_point_local);
  CHECK(sum_points(ps, 3) == 1 + 2 + 3 + 4 + 5 + 6 + 7 + 8 + 9);

  int v = 1234;
  int *p = &v;
  CHECK(deref(&p) == 1234);

  int arr[5] = {10, 20, 30, 40, 50};
  CHECK(ptr_arith(&arr[1]) == 40 - 10);
  CHECK(local_arr() == 0 + 1 + 4 + 9 + 16);

  // Whole-struct assignment: RHS is a dereference, so the destination address
  // and the copy temp must not collide.
  struct { int x, y; char tag; } one = {3, 4, 5};
  CHECK(copy_point(&one) == 3 + 4 + 5);
  CHECK(copy_point_local(&one) == 3 * 100 + 4 * 10 + 5);
}

static void test_strings_globals(JIT *jit) {
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

  int (*bump)(void) = widcc_rt_lookup(jit->rt, "bump");
  char (*first_char)(void) = widcc_rt_lookup(jit->rt, "first_char");
  int (*str_len)(char *) = widcc_rt_lookup(jit->rt, "str_len");
  int *(*counter_addr)(void) = widcc_rt_lookup(jit->rt, "counter_addr");
  void (*fill_buf)(void) = widcc_rt_lookup(jit->rt, "fill_buf");
  void (*set_counter2)(int) = widcc_rt_lookup(jit->rt, "set_counter2");
  int *counter2p = widcc_rt_lookup(jit->rt, "counter2");
  char *buf = widcc_rt_lookup(jit->rt, "buf");
  CHECK(bump && first_char && str_len && counter_addr && fill_buf);
  CHECK(set_counter2 && counter2p && buf);

  CHECK(first_char() == 'h');
  CHECK(str_len("abcdef") == 6);
  CHECK(bump() == 42 && bump() == 43);
  CHECK(*counter_addr() == 43);
  fill_buf();
  CHECK(buf[0] == 'x' && buf[1] == 'y' && buf[2] == 0);
  set_counter2(777);
  CHECK(*counter2p == 777);
}

static void test_float(JIT *jit) {
  CHECK_COMPILE(jit, "float.c",
    "double dadd(double a, double b) { return a + b; }\n"
    "float fmul(float a, float b) { return a * b; }\n"
    "double mix(int i, double d, float f) { return i + d + f; }\n"
    "int dcmp(double a, double b) { return a < b ? -1 : (a > b ? 1 : 0); }\n"
    "double ddiv(double a, double b) { return a / b; }\n");

  double (*dadd)(double, double) = widcc_rt_lookup(jit->rt, "dadd");
  float (*fmul)(float, float) = widcc_rt_lookup(jit->rt, "fmul");
  double (*mix)(int, double, float) = widcc_rt_lookup(jit->rt, "mix");
  int (*dcmp)(double, double) = widcc_rt_lookup(jit->rt, "dcmp");
  double (*ddiv)(double, double) = widcc_rt_lookup(jit->rt, "ddiv");
  CHECK(dadd && fmul && mix && dcmp && ddiv);

  CHECK(dadd(1.5, 2.25) == 3.75);
  CHECK(fmul(2.0f, 3.5f) == 7.0f);
  CHECK(mix(1, 0.5, 0.25f) == 1.75);
  CHECK(dcmp(1.0, 2.0) == -1 && dcmp(2.0, 1.0) == 1 && dcmp(1.0, 1.0) == 0);
  CHECK(ddiv(7.0, 2.0) == 3.5);
}

static void test_host_symbols(JIT *jit) {
  CHECK(widcc_rt_add_func(jit->rt, "host_add", host_add) == NULL);
  CHECK(widcc_rt_add_func(jit->rt, "host_bump", host_bump) == NULL);
  CHECK(widcc_rt_add_var(jit->rt, "host_counter", (void *)&host_counter) ==
        NULL);

  CHECK_COMPILE(jit, "host.c",
    "int host_add(int a, int b);\n"
    "void host_bump(void);\n"
    "extern int host_counter;\n"
    "int call_host(int x) { return host_add(x, 100); }\n"
    "int bump_host(void) { host_bump(); host_bump(); return host_counter; }\n");

  int (*call_host)(int) = widcc_rt_lookup(jit->rt, "call_host");
  int (*bump_host)(void) = widcc_rt_lookup(jit->rt, "bump_host");
  CHECK(call_host && bump_host);
  CHECK(call_host(23) == 123);
  int before = host_counter;
  CHECK(bump_host() == before + 2);
}

static void test_host_resolver(void) {
  // The resolver is per-runtime: two runtimes resolve the same extern to
  // different host functions without affecting each other.
  WidccRT *ra = widcc_rt_new();
  WidccRT *rb = widcc_rt_new();
  CHECK(ra && rb);
  widcc_rt_set_host_resolver(ra, resolve_add);
  widcc_rt_set_host_resolver(rb, resolve_sub);

  const char *src =
    "int picked(int, int);\n"
    "int use_pick(int a, int b) { return picked(a, b); }\n";
  CHECK(widcc_rt_load_code(ra, "pick_a.c", src, NULL) == NULL);
  CHECK(widcc_rt_load_code(rb, "pick_b.c", src, NULL) == NULL);

  int (*ua)(int, int) = widcc_rt_lookup(ra, "use_pick");
  int (*ub)(int, int) = widcc_rt_lookup(rb, "use_pick");
  CHECK(ua && ub);
  CHECK(ua(10, 3) == 13);
  CHECK(ub(10, 3) == 7);

  widcc_rt_free(rb);
  widcc_rt_free(ra);
}

static void test_recompile(JIT *jit) {
  CHECK_COMPILE(jit, "v1.c", "int version(void) { return 1; }\n");
  int (*v1)(void) = widcc_rt_lookup(jit->rt, "version");
  CHECK(v1 && v1() == 1);

  CHECK_COMPILE(jit, "v2.c", "int version(void) { return 2; }\n");
  int (*v2)(void) = widcc_rt_lookup(jit->rt, "version");
  CHECK(v2 && v2() == 2);
  CHECK(v1() == 1);
}

static void test_error_recovery(JIT *jit) {
  const char *err =
      widcc_rt_load_code(jit->rt, "bad.c", "int broken( { return ;", NULL);
  CHECK(err && strlen(err) > 0);
  CHECK_COMPILE(jit, "ok.c", "int still_ok(void) { return 7; }\n");
  int (*still_ok)(void) = widcc_rt_lookup(jit->rt, "still_ok");
  CHECK(still_ok && still_ok() == 7);
}

static void test_compile_file(JIT *jit) {
  const char *hdr = "widcc_cf_hdr.h";
  const char *src = "widcc_cf_main.c";

  FILE *f = fopen(hdr, "w");
  CHECK(f != NULL);
  fputs("#ifndef WIDCC_CF_HDR_H\n"
        "#define WIDCC_CF_HDR_H\n"
        "static int hdr_val(void) { return 7; }\n"
        "#endif\n", f);
  fclose(f);

  f = fopen(src, "w");
  CHECK(f != NULL);
  fputs("#include \"widcc_cf_hdr.h\"\n"
        "int file_sum(int a, int b) { return a + b + hdr_val(); }\n", f);
  fclose(f);

  // Quoted #include directives resolve relative to the compiled file.
  CHECK(widcc_rt_load_code_file(jit->rt, src, NULL) == NULL);
  int (*file_sum)(int, int) = widcc_rt_lookup(jit->rt, "file_sum");
  CHECK(file_sum && file_sum(2, 3) == 12);

  // An unreadable path is a normal compile error, not a process exit.
  const char *err =
      widcc_rt_load_code_file(jit->rt, "widcc_no_such_file_xyz.c", NULL);
  CHECK(err && strlen(err) > 0);
  CHECK(widcc_rt_load_code_file(jit->rt, NULL, NULL) != NULL);

  remove(hdr);
  remove(src);
}

static void test_include_paths(JIT *jit) {
  const char *hdr = "widcc_inc_hdr.h";
  FILE *f = fopen(hdr, "w");
  CHECK(f != NULL);
  fputs("static int inc_val(void) { return 9; }\n", f);
  fclose(f);

  const char *angle =
    "#include <widcc_inc_hdr.h>\n"
    "int inc_angle(void) { return inc_val(); }\n";
  const char *quote =
    "#include \"widcc_inc_hdr.h\"\n"
    "int inc_quote(void) { return inc_val(); }\n";

  // The string form treats its path as the file name: a quoted include
  // resolves relative to it even though the file does not exist on disk.
  CHECK(widcc_rt_load_code(jit->rt, "widcc_inc_main.c", quote, NULL) == NULL);
  int (*inc_quote)(void) = widcc_rt_lookup(jit->rt, "inc_quote");
  CHECK(inc_quote && inc_quote() == 9);

  // Angle includes search only the configured list, which starts empty.
  CHECK(widcc_rt_load_code(jit->rt, "widcc_inc_main.c", angle, NULL) != NULL);
  const char *paths[] = {"."};
  CHECK(widcc_rt_set_include_paths(jit->rt, paths, 1) == NULL);
  CHECK(widcc_rt_load_code(jit->rt, "widcc_inc_main.c", angle, NULL) == NULL);
  int (*inc_angle)(void) = widcc_rt_lookup(jit->rt, "inc_angle");
  CHECK(inc_angle && inc_angle() == 9);

  // Invalid arguments are rejected; NULL/0 clears the list.
  CHECK(widcc_rt_set_include_paths(NULL, paths, 1) != NULL);
  CHECK(widcc_rt_set_include_paths(jit->rt, NULL, 1) != NULL);
  const char *bad[] = {NULL};
  CHECK(widcc_rt_set_include_paths(jit->rt, bad, 1) != NULL);
  CHECK(widcc_rt_set_include_paths(jit->rt, NULL, 0) == NULL);
  CHECK(widcc_rt_load_code(jit->rt, "widcc_inc_main.c", angle, NULL) != NULL);

  remove(hdr);
}

// Minimal in-memory file system used to exercise the file hooks.
typedef struct {
  const char *path;
  const char *data;
  size_t size;
  size_t pos;
} VfsFile;

#define VFS_ENTRY(path, text) {path, text, sizeof(text) - 1, 0}
static VfsFile vfs_files[] = {
  VFS_ENTRY("/vfs/vfs.h", "static int vfs_val(void) { return 42; }\n"),
  VFS_ENTRY("/vfs/main.c",
            "#include <vfs.h>\n"
            "int vfs_answer(void) { return vfs_val(); }\n"),
};
#undef VFS_ENTRY

static VfsFile *vfs_find(const char *path) {
  for (size_t i = 0; i < sizeof(vfs_files) / sizeof(vfs_files[0]); i++)
    if (!strcmp(vfs_files[i].path, path))
      return &vfs_files[i];
  return NULL;
}

static void *vfs_open(const char *path) {
  VfsFile *f = vfs_find(path);
  if (f)
    f->pos = 0;
  return f;
}

static size_t vfs_read(void *file, void *buf, size_t size) {
  VfsFile *f = file;
  size_t n = f->size - f->pos;
  if (n > size)
    n = size;
  memcpy(buf, f->data + f->pos, n);
  f->pos += n;
  return n;
}

static void vfs_close(void *file) { (void)file; }

static int vfs_exists(const char *path) { return vfs_find(path) != NULL; }

static void test_file_ops(JIT *jit) {
  WidccFileOps ops = {vfs_open, vfs_read, vfs_close, vfs_exists};
  widcc_set_file_ops(&ops);

  const char *paths[] = {"/vfs"};
  CHECK(widcc_rt_set_include_paths(jit->rt, paths, 1) == NULL);

  // Both the unit itself and its #include go through the hooks.
  CHECK(widcc_rt_load_code_file(jit->rt, "/vfs/main.c", NULL) == NULL);
  int (*answer)(void) = widcc_rt_lookup(jit->rt, "vfs_answer");
  CHECK(answer && answer() == 42);

  // "-" is an ordinary path, not stdin; it is not in the virtual unit.
  CHECK(widcc_rt_load_code_file(jit->rt, "-", NULL) != NULL);

  widcc_set_file_ops(NULL);
  CHECK(widcc_rt_set_include_paths(jit->rt, NULL, 0) == NULL);

  // Back on the host filesystem, the virtual unit does not exist.
  CHECK(widcc_rt_load_code_file(jit->rt, "/vfs/main.c", NULL) != NULL);
}

static void test_runtime_release(void) {
  WidccRT *rt = widcc_rt_new();
  CHECK(rt != NULL);

  // A module owns the code and data produced by its unit.
  WidccCode *mod = NULL;
  const char *err = widcc_rt_load_code(rt, "tiers.c",
    "int counter = 5;\n"
    "int add1(int x) { return x + 1; }\n", &mod);
  CHECK(err == NULL && mod != NULL);
  CHECK(widcc_rt_list_symbols(rt, NULL, 0) >= 2);

  int (*add1)(int) = widcc_rt_lookup(rt, "add1");
  CHECK(add1 && add1(9) == 10);
  int *c = widcc_rt_lookup(rt, "counter");
  CHECK(c && *c == 5);

  // Unloading releases them. Symbols are neither removed nor restored.
  CHECK(widcc_rt_unload_code(rt, mod) == NULL);
  CHECK(widcc_rt_unload_code(rt, NULL) == NULL);
  CHECK(widcc_rt_unload_code(rt, mod) != NULL); // module is gone
  CHECK(widcc_rt_lookup(rt, "add1") != NULL);   // stale; never call it

  // A module of another runtime is not ours to unload.
  WidccRT *other = widcc_rt_new();
  WidccCode *foreign = NULL;
  CHECK(widcc_rt_load_code(other, "foreign.c",
    "int foreign_fn(void) { return 3; }\n", &foreign) == NULL);
  CHECK(widcc_rt_unload_code(rt, foreign) != NULL);
  CHECK(widcc_rt_unload_code(other, foreign) == NULL);
  widcc_rt_free(other);

  // The runtime stays usable for new loads.
  CHECK(widcc_rt_load_code(rt, "tiers2.c",
    "int counter2 = 42;\n"
    "int times2(int x) { return x * 2; }\n", NULL) == NULL);
  int (*times2)(int) = widcc_rt_lookup(rt, "times2");
  int *c2 = widcc_rt_lookup(rt, "counter2");
  CHECK(times2 && times2(21) == 42);
  CHECK(c2 && *c2 == 42);

  widcc_rt_free(rt);
}

static void test_platform_macros(JIT *jit) {
  // Platform macros must describe the host the JIT was built for, not a
  // fixed architecture.
#if defined(__x86_64__) || defined(_M_X64)
  CHECK_COMPILE(jit, "arch.c",
    "#if !defined(__x86_64__) || !defined(__amd64__) || defined(__i386__)\n"
    "#error wrong architecture macros\n"
    "#endif\n"
    "int arch_ok(void) { return 1; }\n");
#elif defined(__i386__) || defined(_M_IX86)
  CHECK_COMPILE(jit, "arch.c",
    "#if !defined(__i386__) || defined(__x86_64__)\n"
    "#error wrong architecture macros\n"
    "#endif\n"
    "int arch_ok(void) { return 1; }\n");
#else
  CHECK_COMPILE(jit, "arch.c", "int arch_ok(void) { return 1; }\n");
#endif
  int (*arch_ok)(void) = widcc_rt_lookup(jit->rt, "arch_ok");
  CHECK(arch_ok && arch_ok() == 1);

  // Byte order follows the host.
#if defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__) && \
    (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
  CHECK_COMPILE(jit, "endian.c",
    "#if __BYTE_ORDER__ != __ORDER_BIG_ENDIAN__\n"
    "#error wrong byte order\n"
    "#endif\n"
    "int endian_ok(void) { return 1; }\n");
#else
  CHECK_COMPILE(jit, "endian.c",
    "#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__\n"
    "#error wrong byte order\n"
    "#endif\n"
    "int endian_ok(void) { return 1; }\n");
#endif
  int (*endian_ok)(void) = widcc_rt_lookup(jit->rt, "endian_ok");
  CHECK(endian_ok && endian_ok() == 1);

  // LP64 markers only on non-Windows 64-bit hosts; Linux/ELF markers on
  // Linux.
#if !defined(_WIN32) && (defined(__LP64__) || defined(_LP64))
  CHECK_COMPILE(jit, "lp64.c",
    "#if !defined(__LP64__) || !defined(_LP64)\n"
    "#error missing LP64 macros\n"
    "#endif\n"
    "int lp64_ok(void) { return 1; }\n");
  int (*lp64_ok)(void) = widcc_rt_lookup(jit->rt, "lp64_ok");
  CHECK(lp64_ok && lp64_ok() == 1);
#endif
#ifdef __linux__
  CHECK_COMPILE(jit, "linux.c",
    "#if !defined(__linux__) || !defined(__ELF__) || !defined(__unix__)\n"
    "#error missing linux/unix macros\n"
    "#endif\n"
    "int linux_ok(void) { return 1; }\n");
  int (*linux_ok)(void) = widcc_rt_lookup(jit->rt, "linux_ok");
  CHECK(linux_ok && linux_ok() == 1);
#endif
#ifdef __ANDROID__
  // Android inherits the Linux/unix markers and carries the NDK API level.
  CHECK_COMPILE(jit, "android.c",
    "#if !defined(__ANDROID__) || !defined(__ANDROID_API__)\n"
    "#error missing android macros\n"
    "#endif\n"
    "#if !defined(__linux__) || !defined(__unix__)\n"
    "#error android must keep linux/unix markers\n"
    "#endif\n"
    "int android_api(void) { return __ANDROID_API__; }\n");
  int (*android_api)(void) = widcc_rt_lookup(jit->rt, "android_api");
  CHECK(android_api && android_api() == __ANDROID_API__);
#ifdef __ANDROID_MIN_SDK_VERSION__
  CHECK_COMPILE(jit, "android_sdk.c",
    "int android_sdk(void) { return __ANDROID_MIN_SDK_VERSION__; }\n");
  int (*android_sdk)(void) = widcc_rt_lookup(jit->rt, "android_sdk");
  CHECK(android_sdk && android_sdk() == __ANDROID_MIN_SDK_VERSION__);
#endif
#endif
}

// --- Additional algorithm / integer-width tests ---

static void test_sorting(JIT *jit) {
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

  void (*bubble)(int *, int) = widcc_rt_lookup(jit->rt, "bubble");
  void (*insertion)(int *, int) = widcc_rt_lookup(jit->rt, "insertion");
  int (*is_sorted)(int *, int) = widcc_rt_lookup(jit->rt, "is_sorted");
  int (*local_bubble_ok)(void) = widcc_rt_lookup(jit->rt, "local_bubble_ok");
  int (*sum_after_sort)(int *, int) = widcc_rt_lookup(jit->rt, "sum_after_sort");
  CHECK(bubble && insertion && is_sorted && local_bubble_ok && sum_after_sort);

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
  CHECK(is_sorted(a, N) == 1);
  CHECK(is_sorted(b, N) == 1);
  for (int i = 0; i < N; i++)
    CHECK(a[i] == b[i]);

  // Sorting must be a permutation: the sum is unchanged.
  CHECK(sum_after_sort(c, N) == total);
  CHECK(local_bubble_ok() == 1);
}

static void test_nested_loops(JIT *jit) {
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

  int (*nested)(int) = widcc_rt_lookup(jit->rt, "nested");
  int (*dowhile)(int) = widcc_rt_lookup(jit->rt, "dowhile");
  int (*matrix_trace)(int) = widcc_rt_lookup(jit->rt, "matrix_trace");
  CHECK(nested && dowhile && matrix_trace);

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
    CHECK(nested(n) == c);
  }

  int s = 0, m = 13;
  do { s += m; m -= 1; } while (m > 0 && s < 100);
  CHECK(dowhile(13) == s);

  CHECK(matrix_trace(5) == 0 + 6 + 12 + 18 + 24);
}

static void test_int32_semantics(JIT *jit) {
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

  unsigned (*udiv)(unsigned, unsigned) = widcc_rt_lookup(jit->rt, "udiv");
  unsigned (*umod)(unsigned, unsigned) = widcc_rt_lookup(jit->rt, "umod");
  int (*ult)(unsigned, unsigned) = widcc_rt_lookup(jit->rt, "ult");
  int (*slt)(int, int) = widcc_rt_lookup(jit->rt, "slt");
  unsigned (*ushr)(unsigned, int) = widcc_rt_lookup(jit->rt, "ushr");
  int (*sshr)(int, int) = widcc_rt_lookup(jit->rt, "sshr");
  int (*shl_wrap)(int) = widcc_rt_lookup(jit->rt, "shl_wrap");
  int (*add_wrap)(int) = widcc_rt_lookup(jit->rt, "add_wrap");
  int (*trunc_char)(int) = widcc_rt_lookup(jit->rt, "trunc_char");
  int (*trunc_short)(int) = widcc_rt_lookup(jit->rt, "trunc_short");
  int (*uchar_wrap)(void) = widcc_rt_lookup(jit->rt, "uchar_wrap");
  CHECK(udiv && umod && ult && slt && ushr && sshr && shl_wrap && add_wrap &&
         trunc_char && trunc_short && uchar_wrap);

  // Unsigned 32-bit division/modulo must not be emitted as signed.
  CHECK(udiv(0xFFFFFFF0u, 3u) == 0xFFFFFFF0u / 3u);
  CHECK(udiv(0x80000000u, 2u) == 0x40000000u);
  CHECK(umod(0xFFFFFFF5u, 7u) == 0xFFFFFFF5u % 7u);
  CHECK(udiv(100u, 0xFFFFFFFFu) == 0u);

  // Signed vs unsigned comparison.
  CHECK(ult(0xFFFFFFFFu, 1u) == 0);
  CHECK(slt(-1, 1) == 1);

  // Logical vs arithmetic right shift, and 32-bit wraparound.
  CHECK(ushr(0xFFFFFFFFu, 4) == 0x0FFFFFFFu);
  CHECK(sshr(-16, 2) == -4);
  CHECK(shl_wrap(0x10000000) == 0);
  CHECK(add_wrap(0x7FFFFFFF) == (int)0x80000000);

  // Narrow integer store truncation and zero/sign extension on load.
  CHECK(trunc_char(300) == 44);
  CHECK(trunc_short(70000) == 4464);
  CHECK(uchar_wrap() == 4);
}

// 64-bit integers go through the wide path on a 64-bit host. On a 32-bit
// host the sljit backend emulates them: add/sub use ADDC/SUBC, bitwise ops
// and comparisons are open-coded, and mul/div/mod/shift/float conversions
// call host helpers. Parameters and return values stay rejected there, so
// the tests expose int/double interfaces and publish results through globals.
static void test_int64_semantics(JIT *jit) {
#if HOST_64BIT
  CHECK_COMPILE(jit, "int64.c",
    "long long wide_mul(long long a, long long b) { return a * b; }\n"
    "long long wide_divmod(long long a, long long b) { return a / b + a % b; }\n");

  long long (*wide_mul)(long long, long long) = widcc_rt_lookup(jit->rt, "wide_mul");
  long long (*wide_divmod)(long long, long long) =
      widcc_rt_lookup(jit->rt, "wide_divmod");
  CHECK(wide_mul && wide_divmod);

  // 64-bit operands still use the wide path.
  CHECK(wide_mul(3037000499LL, 3LL) == 3037000499LL * 3LL);
  CHECK(wide_divmod(10000000000LL, 7LL) == 10000000000LL / 7LL + 10000000000LL % 7LL);
#else
  // i64 parameters/returns are still a compile-time error on a 32-bit host.
  const char *err = widcc_rt_load_code(jit->rt, "int64_param.c",
    "long long wide_mul(long long a, long long b) { return a * b; }\n", NULL);
  CHECK(err && strstr(err, "64-bit") != NULL);

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

  void (*i64_binops)(void) = widcc_rt_lookup(jit->rt, "i64_binops");
  int (*i64_cmps)(void) = widcc_rt_lookup(jit->rt, "i64_cmps");
  void (*i64_sum)(int) = widcc_rt_lookup(jit->rt, "i64_sum");
  void (*i64_assign)(void) = widcc_rt_lookup(jit->rt, "i64_assign");
  int (*i64_logic)(void) = widcc_rt_lookup(jit->rt, "i64_logic");
  int (*i64_cond)(int) = widcc_rt_lookup(jit->rt, "i64_cond");
  int (*i64_trunc)(void) = widcc_rt_lookup(jit->rt, "i64_trunc");
  int (*i64_from_int)(int) = widcc_rt_lookup(jit->rt, "i64_from_int");
  double (*i64_to_d)(void) = widcc_rt_lookup(jit->rt, "i64_to_d");
  int (*i64_from_d)(double) = widcc_rt_lookup(jit->rt, "i64_from_d");
  int (*i64_index)(void) = widcc_rt_lookup(jit->rt, "i64_index");
  long long *g_add = widcc_rt_lookup(jit->rt, "g_add");
  long long *g_sub = widcc_rt_lookup(jit->rt, "g_sub");
  long long *g_mul = widcc_rt_lookup(jit->rt, "g_mul");
  long long *g_div = widcc_rt_lookup(jit->rt, "g_div");
  long long *g_mod = widcc_rt_lookup(jit->rt, "g_mod");
  long long *g_and = widcc_rt_lookup(jit->rt, "g_and");
  long long *g_or = widcc_rt_lookup(jit->rt, "g_or");
  long long *g_xor = widcc_rt_lookup(jit->rt, "g_xor");
  long long *g_neg = widcc_rt_lookup(jit->rt, "g_neg");
  long long *g_not = widcc_rt_lookup(jit->rt, "g_not");
  long long *g_shl = widcc_rt_lookup(jit->rt, "g_shl");
  long long *g_shr = widcc_rt_lookup(jit->rt, "g_shr");
  long long *g_sar = widcc_rt_lookup(jit->rt, "g_sar");
  long long *g_loop = widcc_rt_lookup(jit->rt, "g_loop");
  long long *g_chain = widcc_rt_lookup(jit->rt, "g_chain");
  CHECK(i64_binops && i64_cmps && i64_sum && i64_assign && i64_logic &&
         i64_cond && i64_trunc && i64_from_int && i64_to_d && i64_from_d &&
         i64_index);
  CHECK(g_add && g_sub && g_mul && g_div && g_mod && g_and && g_or && g_xor &&
         g_neg && g_not && g_shl && g_shr && g_sar && g_loop && g_chain);

  const long long a = 0x123456789abcdefLL;
  const long long b = 0x111111111111111LL;
  i64_binops();
  CHECK(*g_add == a + b);
  CHECK(*g_sub == a - b);
  CHECK(*g_mul == (long long)((unsigned long long)a * (unsigned long long)b));
  CHECK(*g_div == a / b);
  CHECK(*g_mod == a % b);
  CHECK(*g_and == (a & b));
  CHECK(*g_or == (a | b));
  CHECK(*g_xor == (a ^ b));
  CHECK(*g_neg == -a);
  CHECK(*g_not == ~a);
  CHECK(*g_shl == (long long)((unsigned long long)a << 5));
  CHECK(*g_shr == a >> 5);
  CHECK(*g_sar == (-a) >> 5);

  CHECK(i64_cmps() == 63);
  i64_sum(100);
  CHECK(*g_loop == (long long)(5050LL * 0x100000000LL));
  i64_assign();
  CHECK(*g_chain == 0x20000000eLL);
  CHECK(i64_logic() == 5);
  CHECK(i64_cond(1) == 1);
  CHECK(i64_cond(0) == 0);
  CHECK(i64_trunc() == 0x23456789);
  CHECK(i64_from_int(-1) == -1);
  CHECK(i64_from_int(5) == 0);
  CHECK(i64_to_d() == 4294967297.0);
  CHECK(i64_from_d(4294967297.0) == 1);
  CHECK(i64_index() == 50);  // pointer arithmetic with a 64-bit index
#endif
}

static void test_data_model(JIT *jit) {
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
  int (*model_sizes)(void) = widcc_rt_lookup(jit->rt, "model_sizes");
  int (*ptr_step)(void) = widcc_rt_lookup(jit->rt, "ptr_step");
  CHECK(model_sizes && ptr_step);

  // Pointers and `long` are 8 bytes on LP64/LLP64 and 4 bytes on ILP32;
  // `long long` is always 8.

  CHECK(model_sizes() == sizeof(void *) + sizeof(long) + sizeof(long long)
           + sizeof(short) + sizeof(char));

  CHECK(ptr_step() == 3);
}

// Functions with more than four parameters (and all variadic functions) use
// the Block ABI: the JIT entry takes a single pointer to an array of 8-byte
// argument slots, so the host must call such functions through a
// one-pointer prototype and fill the slots itself.
static void test_many_args(JIT *jit) {
  CHECK_COMPILE(jit, "manyargs.c",
    "int sum5(int a, int b, int c, int d, int e) { return a+b+c+d+e; }\n"
    "int sum8(int a,int b,int c,int d,int e,int f,int g,int h) {\n"
    "  return a+b+c+d+e+f+g+h;\n"
    "}\n"
    "double mix6(int a, double b, int c, double d, int e, double f) {\n"
    "  return a + b + c + d + e + f;\n"
    "}\n"
    "int recsum(int n, int a, int b, int c, int d);\n"
    "int start_rec(int a, int b, int c, int d) {\n"
    "  return recsum(3, a, b, c, d);\n"
    "}\n"
    "int recsum(int n, int a, int b, int c, int d) {\n"
    "  if (n <= 0) return a + b + c + d;\n"
    "  return recsum(n - 1, a + 1, b + 1, c + 1, d + 1);\n"
    "}\n"
    "int apply5(int (*fn)(int,int,int,int,int), int a, int b, int c, int d, int e) {\n"
    "  return fn(a, b, c, d, e);\n"
    "}\n"
    "int via_ptr(int a, int b, int c, int d, int e) {\n"
    "  return apply5(sum5, a, b, c, d, e);\n"
    "}\n"
    "int call5(int a, int b, int c, int d, int e) { return sum5(a,b,c,d,e) * 2; }\n"
    "int vhead(int a, ...) { return a + 1; }\n"
    "int usev(int a, int b, int c, int d, int e) { return vhead(a,b,c,d,e); }\n"
    "int usev2(int a, int b, int c) { return vhead(a,b,c); }\n");

  // Block ABI entry points: one pointer to an array of 8-byte slots.
  int (*sum5)(void *) = widcc_rt_lookup(jit->rt, "sum5");
  int (*sum8)(void *) = widcc_rt_lookup(jit->rt, "sum8");
  int (*call5)(void *) = widcc_rt_lookup(jit->rt, "call5");
  int (*via_ptr)(void *) = widcc_rt_lookup(jit->rt, "via_ptr");
  int (*usev)(void *) = widcc_rt_lookup(jit->rt, "usev");
  double (*mix6)(void *) = widcc_rt_lookup(jit->rt, "mix6");
  // Four parameters keep the native ABI, so the host calls these directly.
  int (*usev2)(int, int, int) = widcc_rt_lookup(jit->rt, "usev2");
  int (*start_rec)(int, int, int, int) = widcc_rt_lookup(jit->rt, "start_rec");
  CHECK(sum5 && sum8 && call5 && via_ptr && usev && mix6 && usev2 && start_rec);

  long long a5[5] = {1, 2, 3, 4, 5};
  CHECK(sum5(a5) == 15);
  CHECK(call5(a5) == 30);     // JIT caller builds the block for sum5
  CHECK(usev(a5) == 2);       // variadic callee reads only its fixed slot
  CHECK(via_ptr(a5) == 15);   // indirect block call through a function pointer

  long long a8[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  CHECK(sum8(a8) == 36);

  // start_rec is a 4-parameter native function; it forward-references and
  // recurses into the 5-parameter block-ABI recsum.
  CHECK(start_rec(10, 20, 30, 40) == 112);

  // Mixed integer/double arguments follow declaration order.
  union { double d; long long w; } b, d, f;
  b.d = 2.5; d.d = 4.5; f.d = 6.5;
  long long m6[6] = {1, b.w, 3, d.w, 5, f.w};
  double r = mix6(m6);
  CHECK(r > 22.49 && r < 22.51);

  // A variadic callee is block ABI even when called with fewer than five
  // arguments; the caller here is a plain three-parameter native function.
  CHECK(usev2(7, 8, 9) == 8);
}

// va_list on the Block ABI: a variadic callee receives a flat array of
// 8-byte slots, and __builtin_va_arg walks it. va_list is a one-element
// array holding the cursor (see __builtin_va_list in the runtime).
static void test_va_list(JIT *jit) {
  CHECK_COMPILE(jit, "valist.c",
    "typedef __builtin_va_list va_list;\n"
    "#define va_start(ap, last) __builtin_va_start(ap, last)\n"
    "#define va_arg(ap, ty) __builtin_va_arg(ap, ty)\n"
    "#define va_end(ap) __builtin_va_end(ap)\n"
    "#define va_copy(dst, src) __builtin_va_copy(dst, src)\n"
    "int vsum(int n, ...) {\n"
    "  va_list ap; va_start(ap, n);\n"
    "  int s = 0;\n"
    "  for (int i = 0; i < n; i++) s += va_arg(ap, int);\n"
    "  va_end(ap);\n"
    "  return s;\n"
    "}\n"
    "double vmix(int n, ...) {\n"
    "  va_list ap; va_start(ap, n);\n"
    "  double s = 0;\n"
    "  for (int i = 0; i < n; i++) s += va_arg(ap, double);\n"
    "  va_end(ap);\n"
    "  return s;\n"
    "}\n"
    "int vmixed(int n, ...) {\n"
    "  va_list ap; va_start(ap, n);\n"
    "  int a = va_arg(ap, int);\n"
    "  double b = va_arg(ap, double);\n"
    "  char *s = va_arg(ap, char *);\n"
    "  va_end(ap);\n"
    "  return a + (int)b + (int)s[0];\n"
    "}\n"
    "int vcopy(int n, ...) {\n"
    "  va_list ap; va_start(ap, n);\n"
    "  va_list aq; va_copy(aq, ap);\n"
    "  int a = va_arg(ap, int);\n"
    "  int b = va_arg(aq, int);\n"
    "  return a == b ? a : -1;\n"
    "}\n"
    "int vhead2(int a, int b, ...) {\n"
    "  va_list ap; va_start(ap, b);\n"
    "  return a + b + va_arg(ap, int);\n"
    "}\n"
    "int driver(void) {\n"
    "  return vsum(4, 10, 20, 30, 40) + vmixed(3, 5, 2.5, \"A\")\n"
    "       + vcopy(1, 7) + vhead2(1, 2, 3) + (int)vmix(2, 1.5, 2.5);\n"
    "}\n");

  // 100 + (5 + 2 + 'A') + 7 + (1 + 2 + 3) + 4 = 189
  int (*driver)(void) = widcc_rt_lookup(jit->rt, "driver");
  CHECK(driver != NULL);
  CHECK(driver() == 189);

  // A host caller fills the block for a variadic entry point just like for
  // any other Block ABI function.
  int (*vsum)(void *) = widcc_rt_lookup(jit->rt, "vsum");
  CHECK(vsum != NULL);
  long long vs[5] = {3, 100, 200, 300}; // slot 3 is unused by vsum(3, ...)
  CHECK(vsum(vs) == 600);

  // float arguments are promoted to double by the frontend, and
  // va_arg(ap, float) converts the stored double back.
  CHECK_COMPILE(jit, "vafloat.c",
    "typedef __builtin_va_list va_list;\n"
    "#define va_start(ap, last) __builtin_va_start(ap, last)\n"
    "#define va_arg(ap, ty) __builtin_va_arg(ap, ty)\n"
    "float vf(int n, ...) {\n"
    "  va_list ap; va_start(ap, n);\n"
    "  return va_arg(ap, float) + va_arg(ap, float);\n"
    "}\n"
    "float calld(void) { return vf(2, 1.25f, 2.25f); }\n");
  float (*calld)(void) = widcc_rt_lookup(jit->rt, "calld");
  CHECK(calld != NULL);
  CHECK(calld() > 3.49f && calld() < 3.51f);
}

int main(void) {
  WidccRT *rt = widcc_rt_new();
  CHECK(rt != NULL);
  JIT jit = {rt};

  test_arith_control(&jit);
  test_func_recursion(&jit);
  test_forward_ref(&jit);
  test_struct_array_ptr(&jit);
  test_strings_globals(&jit);
  test_float(&jit);
  test_host_symbols(&jit);
  test_host_resolver();
  test_recompile(&jit);
  test_error_recovery(&jit);
  test_compile_file(&jit);
  test_include_paths(&jit);
  test_file_ops(&jit);
  test_sorting(&jit);
  test_nested_loops(&jit);
  test_int32_semantics(&jit);
  test_int64_semantics(&jit);
  test_data_model(&jit);
  test_platform_macros(&jit);
  test_many_args(&jit);
  test_va_list(&jit);

  test_runtime_release();

  // A second, fully independent runtime.
  WidccRT *rt2 = widcc_rt_new();
  CHECK(rt2 != NULL);
  JIT jit2 = {rt2};
  CHECK_COMPILE(&jit2, "other.c",
    "int version(void) { return 42; }\n"
    "int triple(int x) { return x * 3; }\n");
  int (*version2)(void) = widcc_rt_lookup(rt2, "version");
  int (*triple)(int) = widcc_rt_lookup(rt2, "triple");
  CHECK(version2 && version2() == 42);
  CHECK(triple && triple(5) == 15);

  int (*v)(void) = widcc_rt_lookup(rt, "version");
  CHECK(v && v() == 2);

  widcc_rt_free(rt2);
  widcc_rt_free(rt);

  printf("jit_test: all tests passed\n");
  return 0;
}
