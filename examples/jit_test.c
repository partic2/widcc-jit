// Functional test for the widcc JIT runtime (widcc_jit.h).
//
// Covers: arithmetic, control flow, functions/recursion, forward references
// across translation units, structs, arrays, pointers, strings, global
// variables, host symbol registration (including per-runtime resolvers),
// repeated loads, per-module unload, includes and virtual file I/O.
//
// The "broader language coverage" groups at the end add operator side
// effects, integer conversions, aggregates and static initializers with
// cross-object addresses, indirect calls in both directions, statement-level
// control flow, the preprocessor, loop conditions with side effects,
// ternaries nested in larger expressions, wide expression trees, spill-heavy
// frames, the float calling convention and the diagnostics of rejected
// constructs.
#include "widcc_jit.h"
#include <stdint.h>
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
    "double ddiv(double a, double b) { return a / b; }\n"
    // Float arguments arrive in caller-saved registers, so a float parameter
    // must survive an ordinary call issued from inside the function.
    "double keepf(double d, float f) { double r = dadd(1.0, 1.0);"
    " return d + f + r; }\n");

  double (*dadd)(double, double) = widcc_rt_lookup(jit->rt, "dadd");
  float (*fmul)(float, float) = widcc_rt_lookup(jit->rt, "fmul");
  double (*mix)(int, double, float) = widcc_rt_lookup(jit->rt, "mix");
  int (*dcmp)(double, double) = widcc_rt_lookup(jit->rt, "dcmp");
  double (*ddiv)(double, double) = widcc_rt_lookup(jit->rt, "ddiv");
  double (*keepf)(double, float) = widcc_rt_lookup(jit->rt, "keepf");
  CHECK(dadd && fmul && mix && dcmp && ddiv && keepf);

  CHECK(dadd(1.5, 2.25) == 3.75);
  CHECK(fmul(2.0f, 3.5f) == 7.0f);
  CHECK(mix(1, 0.5, 0.25f) == 1.75);
  CHECK(dcmp(1.0, 2.0) == -1 && dcmp(2.0, 1.0) == 1 && dcmp(1.0, 1.0) == 0);
  CHECK(ddiv(7.0, 2.0) == 3.5);
  CHECK(keepf(1.5, 2.0f) == 5.5);
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
    "long long wide_divmod(long long a, long long b) { return a / b + a % b; }\n"
    "long long wide_cond_ctx(int c) {\n"
    "  long long a = 0x100000000LL;\n"
    "  return a + (c ? 1 : 2);\n"
    "}\n");

  long long (*wide_mul)(long long, long long) = widcc_rt_lookup(jit->rt, "wide_mul");
  long long (*wide_divmod)(long long, long long) =
      widcc_rt_lookup(jit->rt, "wide_divmod");
  long long (*wide_cond_ctx)(int) = widcc_rt_lookup(jit->rt, "wide_cond_ctx");
  CHECK(wide_mul && wide_divmod && wide_cond_ctx);

  // 64-bit operands still use the wide path.
  CHECK(wide_mul(3037000499LL, 3LL) == 3037000499LL * 3LL);
  CHECK(wide_divmod(10000000000LL, 7LL) == 10000000000LL / 7LL + 10000000000LL % 7LL);
  CHECK(wide_cond_ctx(1) == 0x100000001LL);
  CHECK(wide_cond_ctx(0) == 0x100000002LL);
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
    "int i64_cond_ctx(int c) {\n"
    "  long long a = 0x100000000;\n"
    "  long long r = a + (c ? 1 : 2);\n"
    "  return (int)(r >> 32) * 10 + (int)r;\n"
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
  int (*i64_cond_ctx)(int) = widcc_rt_lookup(jit->rt, "i64_cond_ctx");
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
  // The ternary sits inside a larger 64-bit expression, so the value on the
  // left must survive whichever arm runs.
  CHECK(i64_cond_ctx(1) == 11);
  CHECK(i64_cond_ctx(0) == 12);
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
// the Block ABI: the JIT entry takes a pointer to an array of 8-byte argument
// slots plus the total argument count, so the host must call such functions
// through a two-argument prototype and fill the slots itself.
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

  // Block ABI entry points: one pointer to an array of 8-byte slots, plus the
  // total argument count as a machine word.
  int (*sum5)(void *, intptr_t) = widcc_rt_lookup(jit->rt, "sum5");
  int (*sum8)(void *, intptr_t) = widcc_rt_lookup(jit->rt, "sum8");
  int (*call5)(void *, intptr_t) = widcc_rt_lookup(jit->rt, "call5");
  int (*via_ptr)(void *, intptr_t) = widcc_rt_lookup(jit->rt, "via_ptr");
  int (*usev)(void *, intptr_t) = widcc_rt_lookup(jit->rt, "usev");
  double (*mix6)(void *, intptr_t) = widcc_rt_lookup(jit->rt, "mix6");
  // Four parameters keep the native ABI, so the host calls these directly.
  int (*usev2)(int, int, int) = widcc_rt_lookup(jit->rt, "usev2");
  int (*start_rec)(int, int, int, int) = widcc_rt_lookup(jit->rt, "start_rec");
  CHECK(sum5 && sum8 && call5 && via_ptr && usev && mix6 && usev2 && start_rec);

  long long a5[5] = {1, 2, 3, 4, 5};
  CHECK(sum5(a5, 5) == 15);
  CHECK(call5(a5, 5) == 30);    // JIT caller builds the block for sum5
  CHECK(usev(a5, 5) == 2);      // variadic callee reads only its fixed slot
  CHECK(via_ptr(a5, 5) == 15);  // indirect block call through a function pointer

  long long a8[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  CHECK(sum8(a8, 8) == 36);

  // start_rec is a 4-parameter native function; it forward-references and
  // recurses into the 5-parameter block-ABI recsum.
  CHECK(start_rec(10, 20, 30, 40) == 112);

  // Mixed integer/double arguments follow declaration order.
  union { double d; long long w; } b, d, f;
  b.d = 2.5; d.d = 4.5; f.d = 6.5;
  long long m6[6] = {1, b.w, 3, d.w, 5, f.w};
  double r = mix6(m6, 6);
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
  // any other Block ABI function, passing the total argument count.
  int (*vsum)(void *, intptr_t) = widcc_rt_lookup(jit->rt, "vsum");
  CHECK(vsum != NULL);
  long long vs[5] = {3, 100, 200, 300}; // slot 3 is unused by vsum(3, ...)
  CHECK(vsum(vs, 4) == 600);

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

// --- Broader language coverage -------------------------------------------
//
// These groups cover constructs the earlier tests do not: operator side
// effects, integer conversions, aggregates and static initializers,
// indirect calls, statement-level control flow, the preprocessor, loop
// conditions with side effects, ternaries inside larger expressions, very
// wide expressions, spill-heavy frames and the diagnostics of constructs
// the backend rejects.

static void test_operators(JIT *jit) {
  CHECK_COMPILE(jit, "ops.c",
    "int compound(int a) {\n"
    "  a += 5; a -= 3; a *= 2; a /= 7; a %= 5;\n"
    "  int b = 0xF0; b &= 0x3C; b |= 1; b ^= 0x7; b <<= 2; b >>= 1;\n"
    "  return a * 1000 + b;\n"
    "}\n"
    "double dcompound(double d) {\n"
    "  d += 2.25; d *= 2.0; d -= 0.5; d /= 2.0; return d;\n"
    "}\n"
    "int incdec(void) {\n"
    "  int i = 5; int a = i++; int b = ++i; int c = i--; int d = --i;\n"
    "  char ch = 127; ch++;\n"
    "  double x = 1.5; double y = x++; double z = ++x;\n"
    "  return a * 100000 + b * 10000 + c * 1000 + d * 100 + ch\n"
    "       + (int)((x + y + z) * 10);\n"
    "}\n"
    "int ptr_incdec(void) {\n"
    "  int a[3]; a[0] = 1; a[1] = 2; a[2] = 3;\n"
    "  int *p = a; int v = *p++; int w = *++p;\n"
    "  return v * 10 + w;\n"
    "}\n"
    "int comma(int x) { return (x, x + 1, x * 4); }\n"
    "int assign_value(void) {\n"
    "  int a, b, c; a = b = c = 4; int x; int y = (x = 7) + x;\n"
    "  return a + b + c + x + y;\n"
    "}\n");

  int (*compound)(int) = widcc_rt_lookup(jit->rt, "compound");
  double (*dcompound)(double) = widcc_rt_lookup(jit->rt, "dcompound");
  int (*incdec)(void) = widcc_rt_lookup(jit->rt, "incdec");
  int (*ptr_incdec)(void) = widcc_rt_lookup(jit->rt, "ptr_incdec");
  int (*comma)(int) = widcc_rt_lookup(jit->rt, "comma");
  int (*assign_value)(void) = widcc_rt_lookup(jit->rt, "assign_value");
  CHECK(compound && dcompound && incdec && ptr_incdec && comma && assign_value);

  CHECK(compound(10) == 3 * 1000 + 108);
  CHECK(dcompound(1.5) == 3.5);
  // a=5 b=7 c=7 d=5, char wraps to -128, x=3.5 y=1.5 z=3.5.
  CHECK(incdec() == 500000 + 70000 + 7000 + 500 - 128 + 85);
  CHECK(ptr_incdec() == 13);
  CHECK(comma(3) == 12);
  CHECK(assign_value() == 33);
}

static void test_conversions(JIT *jit) {
  CHECK_COMPILE(jit, "conv.c",
    "int cast_double(double d) { return (int)d; }\n"
    "int cast_char(int x) { char c = x; return c; }\n"
    "int cast_short(int x) { short s = x; return s; }\n"
    "int cast_ushort(int x) { unsigned short u = x; return u; }\n"
    "int cast_unsigned(void) { unsigned u = (unsigned)-1; return u == 0xFFFFFFFFu; }\n"
    "int cast_bool(double d) { _Bool b = d; return b; }\n"
    "int cast_float_int(void) { float f = 3.99f; return (int)f; }\n"
    "int cast_rounding(double d) { return (int)(d * 100.0); }\n"
    "int promote_short(short s, unsigned short u) { return s * 2 + (u & 0xFF); }\n"
    "int widen_unsigned(void) {\n"
    "  unsigned u = 0xFFFFFFF0u;\n"
    "  long long w = u;\n"
    "  return u == 0xFFFFFFF0u && w == 0xFFFFFFF0ll;\n"
    "}\n"
    "int trunc_long(void) { long long w = 0x123456789LL; return (int)w; }\n");

  int (*cast_double)(double) = widcc_rt_lookup(jit->rt, "cast_double");
  int (*cast_char)(int) = widcc_rt_lookup(jit->rt, "cast_char");
  int (*cast_short)(int) = widcc_rt_lookup(jit->rt, "cast_short");
  int (*cast_ushort)(int) = widcc_rt_lookup(jit->rt, "cast_ushort");
  int (*cast_unsigned)(void) = widcc_rt_lookup(jit->rt, "cast_unsigned");
  int (*cast_bool)(double) = widcc_rt_lookup(jit->rt, "cast_bool");
  int (*cast_float_int)(void) = widcc_rt_lookup(jit->rt, "cast_float_int");
  int (*cast_rounding)(double) = widcc_rt_lookup(jit->rt, "cast_rounding");
  int (*promote_short)(short, unsigned short) =
      widcc_rt_lookup(jit->rt, "promote_short");
  int (*widen_unsigned)(void) = widcc_rt_lookup(jit->rt, "widen_unsigned");
  int (*trunc_long)(void) = widcc_rt_lookup(jit->rt, "trunc_long");
  CHECK(cast_double && cast_char && cast_short && cast_ushort);
  CHECK(cast_unsigned && cast_bool && cast_float_int && cast_rounding);
  CHECK(promote_short && widen_unsigned && trunc_long);

  // Truncation and conversion results are compared against the host's own
  // semantics rather than restated here.
  CHECK(cast_double(-7.9) == (int)-7.9);
  CHECK(cast_char(300) == (char)300);
  CHECK(cast_short(70000) == (short)70000);
  CHECK(cast_ushort(70000) == (unsigned short)70000);
  CHECK(cast_unsigned() == 1);
  CHECK(cast_bool(0.5) == 1 && cast_bool(0.0) == 0 && cast_bool(-2.0) == 1);
  CHECK(cast_float_int() == (int)3.99f);
  CHECK(cast_rounding(-2.75) == (int)(-2.75 * 100.0));
  CHECK(promote_short((short)-300, (unsigned short)70000) ==
        (short)-300 * 2 + ((unsigned short)70000 & 0xFF));
  CHECK(widen_unsigned() == 1);
  CHECK(trunc_long() == (int)0x123456789LL);
}

static void test_aggregates(JIT *jit) {
  CHECK_COMPILE(jit, "agg.c",
    "union U { int i; float f; unsigned char b[4]; };\n"
    "enum E { A, B = 5, C, D = -2 };\n"
    "struct In { int a; char b[4]; };\n"
    "struct Out { struct In in; int c; struct In *p; };\n"
    "struct P { int x, y, z; };\n"
    "struct Row { int v[3]; };\n"
    "struct Grid { struct Row rows[2]; };\n"
    "int union_pun(void) {\n"
    "  union U u; u.i = 0; u.b[0] = 1; u.b[1] = 2; return u.i;\n"
    "}\n"
    "int enum_vals(void) { return A * 1000 + B * 100 + C * 10 + D; }\n"
    "int nested(void) {\n"
    "  struct Out o; o.in.a = 7; o.in.b[0] = 1; o.in.b[1] = 2;\n"
    "  o.in.b[2] = 3; o.in.b[3] = 4; o.c = 9; o.p = &o.in;\n"
    "  struct Out q = o; q.c = 10;\n"
    "  return q.in.a + q.in.b[0] + q.in.b[1] + q.in.b[2] + q.in.b[3]\n"
    "       + q.c + q.p->a + o.c;\n"
    "}\n"
    "int designated(void) {\n"
    "  int a[5] = {[1] = 10, [3] = 30}; struct P p = {.y = 2, .z = 3};\n"
    "  int b[] = {1, 2, 3, 4}; struct P c = {7};\n"
    "  return a[0] + a[1] + a[2] + a[3] + p.x + p.y + p.z + b[3] + c.x + c.y;\n"
    "}\n"
    "int layout(void) {\n"
    "  int off = (int)((char *)&((struct P *)0)->y - (char *)0);\n"
    "  return off + _Alignof(struct P) + (int)sizeof(struct P);\n"
    "}\n"
    "int compound_literal(void) {\n"
    "  struct P p = (struct P){3, 4};\n"
    "  int *a = (int[]){5, 6, 7};\n"
    "  return p.x + p.y + a[0] + a[1] + a[2];\n"
    "}\n"
    "int nested_array(void) {\n"
    "  struct Grid g; struct Grid h;\n"
    "  for (int i = 0; i < 2; i++)\n"
    "    for (int j = 0; j < 3; j++) g.rows[i].v[j] = i * 3 + j;\n"
    "  h = g;\n"
    "  int s = 0;\n"
    "  for (int i = 0; i < 2; i++)\n"
    "    for (int j = 0; j < 3; j++) s += h.rows[i].v[j];\n"
    "  return s;\n"
    "}\n"
    "int multi_array(void) {\n"
    "  int m[3][4];\n"
    "  for (int i = 0; i < 3; i++) for (int j = 0; j < 4; j++) m[i][j] = i * 4 + j;\n"
    "  int (*row)[4] = m; int *flat = &m[0][0];\n"
    "  return m[2][3] + row[1][2] + flat[5] + *(*(m + 1) + 1);\n"
    "}\n");

  int (*union_pun)(void) = widcc_rt_lookup(jit->rt, "union_pun");
  int (*enum_vals)(void) = widcc_rt_lookup(jit->rt, "enum_vals");
  int (*nested)(void) = widcc_rt_lookup(jit->rt, "nested");
  int (*designated)(void) = widcc_rt_lookup(jit->rt, "designated");
  int (*layout)(void) = widcc_rt_lookup(jit->rt, "layout");
  int (*compound_literal)(void) = widcc_rt_lookup(jit->rt, "compound_literal");
  int (*nested_array)(void) = widcc_rt_lookup(jit->rt, "nested_array");
  int (*multi_array)(void) = widcc_rt_lookup(jit->rt, "multi_array");
  CHECK(union_pun && enum_vals && nested && designated && layout);
  CHECK(compound_literal && nested_array && multi_array);

  union { int i; unsigned char b[4]; } pun;
  pun.i = 0;
  pun.b[0] = 1;
  pun.b[1] = 2;
  CHECK(union_pun() == pun.i); // the host's own byte order
  CHECK(enum_vals() == 0 + 500 + 60 - 2);
  CHECK(nested() == 7 + 1 + 2 + 3 + 4 + 10 + 7 + 9);
  CHECK(designated() == 0 + 10 + 0 + 30 + 0 + 2 + 3 + 4 + 7 + 0);
  CHECK(layout() == 4 + 4 + 12);
  CHECK(compound_literal() == 3 + 4 + 5 + 6 + 7);
  CHECK(nested_array() == 0 + 1 + 2 + 3 + 4 + 5);
  CHECK(multi_array() == 11 + 6 + 5 + 5);
}

static void test_initializers(JIT *jit) {
  // A static initializer may take the address of another global of the same
  // unit, whichever order the objects are emitted in.
  CHECK_COMPILE(jit, "init.c",
    "int g = 7;\n"
    "int *gp = &g;\n"
    "int arr[3] = {1, 2, 3};\n"
    "int *ap = &arr[1];\n"
    "int *ap0 = arr;\n"
    "char *names[3] = {\"aa\", \"bb\", \"cc\"};\n"
    "struct Q { int a; int b[3]; char *s; };\n"
    "struct Q gq = {4, {5, 6, 7}, \"hi\"};\n"
    "char gs[] = \"abcd\";\n"
    "int garr5[5] = {1, 2, 3};\n"
    "int statics(void) {\n"
    "  static int calls;\n"
    "  static int tab[3] = {1, 2, 3};\n"
    "  calls++;\n"
    "  tab[0] += calls;\n"
    "  return calls * 100 + tab[0];\n"
    "}\n"
    "int initializer_sum(void) {\n"
    "  *gp += 1; *ap += 10;\n"
    "  return g + arr[1] + gq.a + gq.b[2] + gq.s[1] + names[1][1]\n"
    "       + gs[3] + garr5[2] + garr5[4] + (ap0 == arr);\n"
    "}\n");

  int (*statics)(void) = widcc_rt_lookup(jit->rt, "statics");
  int (*initializer_sum)(void) = widcc_rt_lookup(jit->rt, "initializer_sum");
  CHECK(statics && initializer_sum);
  CHECK(statics() == 102);
  CHECK(statics() == 204);
  // g=8, arr[1]=12, gq.a=4, gq.b[2]=7, 'i', 'b', gs[3]='d',
  // garr5[2]=3, garr5[4]=0, ap0==arr.
  CHECK(initializer_sum() == 8 + 12 + 4 + 7 + 'i' + 'b' + 'd' + 3 + 0 + 1);
}

static int host_cb(int a, int b) { return a * 100 + b; }
static int host_apply(int (*fn)(int, int), int a, int b) { return fn(a, b); }

static void test_indirect_calls(JIT *jit) {
  CHECK(widcc_rt_add_func(jit->rt, "host_cb", (void *)host_cb) == NULL);
  CHECK(widcc_rt_add_func(jit->rt, "host_apply", (void *)host_apply) == NULL);

  CHECK_COMPILE(jit, "fnptr.c",
    "typedef int (*Bin)(int, int);\n"
    "int add(int a, int b) { return a + b; }\n"
    "int sub(int a, int b) { return a - b; }\n"
    "Bin pick(int which) { return which ? sub : add; }\n"
    "struct Ops { Bin fn; int bias; };\n"
    "static int sq(int x) { return x * x; }\n"
    "static int twice(int (*f)(int), int x) { return f(f(x)); }\n"
    "int table(void) {\n"
    "  Bin tab[2]; tab[0] = add; tab[1] = sub;\n"
    "  struct Ops o; o.fn = tab[0]; o.bias = 100;\n"
    "  int (*raw)(int, int) = pick(1);\n"
    "  return tab[0](3, 4) + tab[1](3, 4) + o.fn(5, 6) + o.bias\n"
    "       + raw(10, 3) + pick(0)(1, 1);\n"
    "}\n"
    "int static_ptr(void) { return twice(sq, 3); }\n"
    "int cmp_ptrs(void) {\n"
    "  int (*p)(int, int) = add; int (*q)(int, int) = sub;\n"
    "  int r = (p == add) + (q != add) + (p != q);\n"
    "  if (p) r += 10;\n"
    "  return r + p(2, 3);\n"
    "}\n"
    "int host_cb(int, int);\n"
    "int host_apply(int (*)(int, int), int, int);\n"
    "int call_host_through_ptr(void) {\n"
    "  return host_apply(host_cb, 4, 5);\n"
    "}\n"
    "int pass_ptr_out(int (*fn)(int, int), int a) { return fn(a, a + 1); }\n");

  int (*table)(void) = widcc_rt_lookup(jit->rt, "table");
  int (*static_ptr)(void) = widcc_rt_lookup(jit->rt, "static_ptr");
  int (*cmp_ptrs)(void) = widcc_rt_lookup(jit->rt, "cmp_ptrs");
  int (*call_host_through_ptr)(void) =
      widcc_rt_lookup(jit->rt, "call_host_through_ptr");
  int (*pass_ptr_out)(int (*)(int, int), int) =
      widcc_rt_lookup(jit->rt, "pass_ptr_out");
  CHECK(table && static_ptr && cmp_ptrs && call_host_through_ptr && pass_ptr_out);

  CHECK(table() == 7 - 1 + 11 + 100 + 7 + 2);
  CHECK(static_ptr() == 81);
  CHECK(cmp_ptrs() == 3 + 10 + 5);
  // A host callback reaches the JIT as a plain function pointer and the JIT
  // passes one of its own back out to the host.
  CHECK(call_host_through_ptr() == 405);
  CHECK(pass_ptr_out(host_cb, 6) == 607);
}

static void test_control_flow(JIT *jit) {
  CHECK_COMPILE(jit, "flow.c",
    "int fallthrough(void) {\n"
    "  int r = 0;\n"
    "  for (int i = 0; i < 5; i++) {\n"
    "    switch (i) {\n"
    "    case 0: r += 1;\n"
    "    case 1: r += 10; break;\n"
    "    case 2: continue;\n"
    "    case 3: for (int j = 0; j < 3; j++) {\n"
    "              if (j == 1) continue; r += 100; } break;\n"
    "    default: r += 1000;\n"
    "    }\n"
    "    r += 1;\n"
    "  }\n"
    "  return r;\n"
    "}\n"
    "int goto_nested(void) {\n"
    "  int r = 0, i = 0;\n"
    "  for (int a = 0; a < 3; a++)\n"
    "    for (int b = 0; b < 3; b++) {\n"
    "      if (a == 1 && b == 1) goto out;\n"
    "      r += a * 3 + b;\n"
    "    }\n"
    "out:\n"
    "  r += 1000;\n"
    "  i = 0;\n"
    "again:\n"
    "  if (i < 3) { r += i; i++; goto again; }\n"
    "  return r;\n"
    "}\n"
    "int shadowing(void) {\n"
    "  int x = 1, r = 0;\n"
    "  { int x = 10; r += x; { int x = 100; r += x; } r += x; }\n"
    "  r += x;\n"
    "  for (int x = 0; x < 2; x++) r += x;\n"
    "  r += x;\n"
    "  return r * 1000 + x;\n"
    "}\n"
    "int for_comma(void) {\n"
    "  int s = 0;\n"
    "  for (int i = 0, j = 10; i < 10; i++, j--) s += i * j;\n"
    "  return s;\n"
    "}\n"
    "int break_continue_deep(void) {\n"
    "  int s = 0;\n"
    "  for (int i = 0; i < 10; i++) {\n"
    "    if (i % 3 == 0) continue;\n"
    "    int j = 0;\n"
    "    while (1) {\n"
    "      if (j > i) break;\n"
    "      j += 2;\n"
    "      if (j == 4) { s += 1000; continue; }\n"
    "      s += i * 10 + j;\n"
    "      if (j > 6) break;\n"
    "    }\n"
    "  }\n"
    "  return s;\n"
    "}\n"
    "int switch_values(int x) {\n"
    "  switch (x) {\n"
    "  case -5: return 1;\n"
    "  case 0: return 2;\n"
    "  case 1000000: return 3;\n"
    "  case 2147483647: return 4;\n"
    "  }\n"
    "  return 0;\n"
    "}\n"
    "int returns_paths(int x) {\n"
    "  if (x < 0) return -1;\n"
    "  if (x == 0) return 0;\n"
    "  for (int i = 0; i < x; i++) if (i == 5) return i * 100;\n"
    "  return x * 7;\n"
    "}\n");

  int (*fallthrough)(void) = widcc_rt_lookup(jit->rt, "fallthrough");
  int (*goto_nested)(void) = widcc_rt_lookup(jit->rt, "goto_nested");
  int (*shadowing)(void) = widcc_rt_lookup(jit->rt, "shadowing");
  int (*for_comma)(void) = widcc_rt_lookup(jit->rt, "for_comma");
  int (*break_continue_deep)(void) =
      widcc_rt_lookup(jit->rt, "break_continue_deep");
  int (*switch_values)(int) = widcc_rt_lookup(jit->rt, "switch_values");
  int (*returns_paths)(int) = widcc_rt_lookup(jit->rt, "returns_paths");
  CHECK(fallthrough && goto_nested && shadowing && for_comma);
  CHECK(break_continue_deep && switch_values && returns_paths);

  CHECK(fallthrough() == 1225);
  CHECK(goto_nested() == 1009);
  CHECK(shadowing() == 123 * 1000 + 1);
  CHECK(for_comma() == 165); // sum i * (10 - i) for i in 0..9
  CHECK(break_continue_deep() == 5712);
  CHECK(switch_values(-5) == 1 && switch_values(0) == 2);
  CHECK(switch_values(1000000) == 3 && switch_values(2147483647) == 4);
  CHECK(switch_values(7) == 0);
  CHECK(returns_paths(-5) == -1 && returns_paths(0) == 0);
  CHECK(returns_paths(8) == 500 && returns_paths(3) == 21);
}

static void test_preprocessor(JIT *jit) {
  CHECK_COMPILE(jit, "pp.c",
    "#define SQ(x) ((x) * (x))\n"
    "#define CAT(a, b) a##b\n"
    "#define STR(x) #x\n"
    "#define VAL 3\n"
    "#if VAL > 5\n"
    "#define PICK 1\n"
    "#elif VAL > 2\n"
    "#define PICK 2\n"
    "#else\n"
    "#define PICK 3\n"
    "#endif\n"
    "#undef VAL\n"
    "#define VAL 7\n"
    "int probe(void) {\n"
    "  int CAT(v, 1) = SQ(VAL);\n"
    "  int n = 0;\n"
    "  if (STR(abc)[0] == 'a') n = 1;\n"
    "  return v1 * 10 + PICK + n;\n"
    "}\n"
    "int counter_macro(void) {\n"
    "  return __COUNTER__ * 10 + __COUNTER__;\n"
    "}\n"
    "int line_macro(void) { return __LINE__ > 0; }\n");

  int (*probe)(void) = widcc_rt_lookup(jit->rt, "probe");
  int (*counter_macro)(void) = widcc_rt_lookup(jit->rt, "counter_macro");
  int (*line_macro)(void) = widcc_rt_lookup(jit->rt, "line_macro");
  CHECK(probe && counter_macro && line_macro);
  CHECK(probe() == 49 * 10 + 2 + 1);
  CHECK(counter_macro() == 1); // serials 0 and 1
  CHECK(line_macro() == 1);
}

// Loop conditions with side effects are emitted once, above the body, so
// the back edge has to re-enter the test instead of jumping into the body.
static void test_loop_conditions(JIT *jit) {
  CHECK_COMPILE(jit, "loops2.c",
    "int postdec(int n) { int s = 0; while (n-- > 0) s += n; return s; }\n"
    "int postdec_plain(int n) { int c = 0; while (n--) c++; return c * 10 + n; }\n"
    "int postinc_for(int n) { int c = 0; for (; c++ < n; ) { } return c; }\n"
    "int comma_cond(int n) { int i = 0, s = 0; while ((i += 2) <= n) s += i; return s; }\n"
    "int assign_cond(int n) {\n"
    "  int v = 0, c = 0;\n"
    "  while ((v = n - c) > 0) c++;\n"
    "  return c * 10 + v;\n"
    "}\n"
    "int next(int *p) { return (*p)++; }\n"
    "int call_cond(void) {\n"
    "  int x = 0, s = 0;\n"
    "  while (next(&x) < 4) s += x;\n"
    "  return s * 10 + x;\n"
    "}\n"
    "int ternary_cond(int n) {\n"
    "  int i = 0, s = 0;\n"
    "  while (i < n ? (i++, 1) : 0) s++;\n"
    "  return s * 10 + i;\n"
    "}\n"
    "int dowhile_post(int n) { int c = 0; do { c++; } while (n-- > 0); return c * 100 + n; }\n"
    "int nested_call_cond(int n) {\n"
    "  int s = 0;\n"
    "  for (int i = 0; i < n; i++) {\n"
    "    int j = 0;\n"
    "    while (next(&j) < 3) s += j;\n"
    "  }\n"
    "  return s;\n"
    "}\n");

  int (*postdec)(int) = widcc_rt_lookup(jit->rt, "postdec");
  int (*postdec_plain)(int) = widcc_rt_lookup(jit->rt, "postdec_plain");
  int (*postinc_for)(int) = widcc_rt_lookup(jit->rt, "postinc_for");
  int (*comma_cond)(int) = widcc_rt_lookup(jit->rt, "comma_cond");
  int (*assign_cond)(int) = widcc_rt_lookup(jit->rt, "assign_cond");
  int (*call_cond)(void) = widcc_rt_lookup(jit->rt, "call_cond");
  int (*ternary_cond)(int) = widcc_rt_lookup(jit->rt, "ternary_cond");
  int (*dowhile_post)(int) = widcc_rt_lookup(jit->rt, "dowhile_post");
  int (*nested_call_cond)(int) = widcc_rt_lookup(jit->rt, "nested_call_cond");
  CHECK(postdec && postdec_plain && postinc_for && comma_cond && assign_cond);
  CHECK(call_cond && ternary_cond && dowhile_post && nested_call_cond);

  // Reference values computed with the same loop on the host.
  for (int n = 0; n <= 6; n++) {
    int s = 0, m = n;
    while (m-- > 0) s += m;
    CHECK(postdec(n) == s);

    int c = 0, k = n;
    while (k--) c++;
    CHECK(postdec_plain(n) == c * 10 + k);

    int c2 = 0;
    for (; c2++ < n; ) { }
    CHECK(postinc_for(n) == c2);

    int i = 0, s2 = 0;
    while ((i += 2) <= n) s2 += i;
    CHECK(comma_cond(n) == s2);

    int v = 0, c3 = 0;
    while ((v = n - c3) > 0) c3++;
    CHECK(assign_cond(n) == c3 * 10 + v);
  }

  CHECK(call_cond() == 10 * 10 + 5); // 1+2+3+4, x ends at 5
  CHECK(ternary_cond(5) == 5 * 10 + 5);
  CHECK(dowhile_post(3) == 4 * 100 - 1);
  CHECK(nested_call_cond(2) == 2 * (1 + 2 + 3));
}

// A ternary nested inside a larger expression must leave enclosing values
// intact on both arms; the spill stores belong before the branch.
static void test_ternary_expressions(JIT *jit) {
  CHECK_COMPILE(jit, "ternary.c",
    "int tern_ge(int a, int b) { return a >= (b ? b : 7); }\n"
    "int tern_in_expr(int a, int b) {\n"
    "  return (a ? a + 1 : b * 3) + (b ? 100 : 200) * 2 + (a ? 1 : 0);\n"
    "}\n"
    "int tern_ptr(int c) {\n"
    "  int a = 11, b = 22;\n"
    "  int *p = c ? &a : &b;\n"
    "  int arr[2]; arr[0] = 5; arr[1] = 6;\n"
    "  int *q = c ? arr : arr + 1;\n"
    "  return *p + *q;\n"
    "}\n"
    "int tern_chain(int n) {\n"
    "  return n < 0 ? -1 : (n == 0 ? 0 : (n < 10 ? n * 10 : n * 100));\n"
    "}\n"
    "int tern_assign(int c) { int x = 0; x = c ? 10 : 20; return x; }\n");

  int (*tern_ge)(int, int) = widcc_rt_lookup(jit->rt, "tern_ge");
  int (*tern_in_expr)(int, int) = widcc_rt_lookup(jit->rt, "tern_in_expr");
  int (*tern_ptr)(int) = widcc_rt_lookup(jit->rt, "tern_ptr");
  int (*tern_chain)(int) = widcc_rt_lookup(jit->rt, "tern_chain");
  int (*tern_assign)(int) = widcc_rt_lookup(jit->rt, "tern_assign");
  CHECK(tern_ge && tern_in_expr && tern_ptr && tern_chain && tern_assign);

  CHECK(tern_ge(1, 0) == 0);  // 1 >= 7 is false
  CHECK(tern_ge(9, 0) == 1);
  CHECK(tern_ge(1, 1) == 1);  // b is 1, so compare against b
  CHECK(tern_in_expr(2, 0) == 3 + 200 * 2 + 1);
  CHECK(tern_in_expr(0, 5) == 15 + 100 * 2 + 0);
  CHECK(tern_ptr(1) == 11 + 5);
  CHECK(tern_ptr(0) == 22 + 6);
  CHECK(tern_chain(-1) == -1 && tern_chain(0) == 0);
  CHECK(tern_chain(3) == 30 && tern_chain(30) == 3000);
  CHECK(tern_assign(1) == 10 && tern_assign(0) == 20);
}

// A wide left-leaning expression tree used to make the value-stack depth
// estimation exponential; it must compile in linear time and be correct.
static void test_wide_expressions(JIT *jit) {
  char src[4096];
  int n = 0;
  n += snprintf(src + n, sizeof src - (size_t)n,
                "int wide_add(void) { int a = 1; return ");
  for (int i = 0; i < 40; i++)
    n += snprintf(src + n, sizeof src - (size_t)n, i ? "+a" : "a");
  n += snprintf(src + n, sizeof src - (size_t)n, "; }\nint wide_mixed(void) { int a = 1; return ");
  for (int i = 0; i < 24; i++)
    n += snprintf(src + n, sizeof src - (size_t)n, i ? "+a" : "a");
  for (int i = 0; i < 24; i++)
    n += snprintf(src + n, sizeof src - (size_t)n, "+a*a");
  n += snprintf(src + n, sizeof src - (size_t)n,
                "; }\nint deep_parens(void) { int a = 7; return ");
  for (int i = 0; i < 100; i++)
    n += snprintf(src + n, sizeof src - (size_t)n, "(");
  n += snprintf(src + n, sizeof src - (size_t)n, "a");
  for (int i = 0; i < 100; i++)
    n += snprintf(src + n, sizeof src - (size_t)n, ")");
  snprintf(src + n, sizeof src - (size_t)n, "; }\n");

  CHECK_COMPILE(jit, "wide.c", src);
  int (*wide_add)(void) = widcc_rt_lookup(jit->rt, "wide_add");
  int (*wide_mixed)(void) = widcc_rt_lookup(jit->rt, "wide_mixed");
  int (*deep_parens)(void) = widcc_rt_lookup(jit->rt, "deep_parens");
  CHECK(wide_add && wide_mixed && deep_parens);
  CHECK(wide_add() == 40);
  CHECK(wide_mixed() == 24 + 24);
  CHECK(deep_parens() == 7);
}

// Every local has to survive a loop that also calls out, which forces the
// register allocator through its spill paths.
static void test_many_locals(JIT *jit) {
  CHECK_COMPILE(jit, "locals.c",
    "int add(int a, int b) { return a + b; }\n"
    "int twelve(void) {\n"
    "  int a = 1, b = 2, c = 3, d = 4, e = 5, f = 6;\n"
    "  int g = 7, h = 8, i = 9, j = 10, k = 11, l = 12;\n"
    "  int s = 0;\n"
    "  for (int n = 0; n < 3; n++) {\n"
    "    s += a + b + c + d + e + f + g + h + i + j + k + l;\n"
    "    a++; b++; c++; d++; e++; f++; g++; h++; i++; j++; k++; l++;\n"
    "  }\n"
    "  return s + a + b + c + d + e + f + g + h + i + j + k + l;\n"
    "}\n"
    "int with_calls(void) {\n"
    "  int a = 1, b = 2, c = 3, d = 4, e = 5, f = 6, g = 7, h = 8, i = 9, j = 10;\n"
    "  for (int n = 0; n < 3; n++) {\n"
    "    a = add(a, 1); b = add(b, 2); c = add(c, 3); d = add(d, 4);\n"
    "    e = add(e, 5); f = add(f, 6); g = add(g, 7); h = add(h, 8);\n"
    "    i = add(i, 9); j = add(j, 10);\n"
    "  }\n"
    "  return a * 1000 + b * 100 + c * 10 + d + e + f + g + h + i + j;\n"
    "}\n"
    "int twenty(void) {\n"
    "  int v0=1,v1=2,v2=3,v3=4,v4=5,v5=6,v6=7,v7=8,v8=9,v9=10;\n"
    "  int w0=11,w1=12,w2=13,w3=14,w4=15,w5=16,w6=17,w7=18,w8=19,w9=20;\n"
    "  return v0+v1+v2+v3+v4+v5+v6+v7+v8+v9\n"
    "       + w0+w1+w2+w3+w4+w5+w6+w7+w8+w9;\n"
    "}\n"
    "int six_params(int a, int b, int c, int d, int e, int f) {\n"
    "  return a + b + c + d + e + f;\n"
    "}\n"
    "int params_and_locals(void) {\n"
    "  int x1=1,x2=2,x3=3,x4=4,x5=5,x6=6,x7=7,x8=8;\n"
    "  return six_params(x1,x2,x3,x4,x5,x6) + x7 * 10 + x8;\n"
    "}\n"
    "int twelve_params(int a,int b,int c,int d,int e,int f,\n"
    "                  int g,int h,int i,int j,int k,int l) {\n"
    "  return a+b*2+c*3+d*4+e*5+f*6+g*7+h*8+i*9+j*10+k*11+l*12;\n"
    "}\n");

  int (*twelve)(void) = widcc_rt_lookup(jit->rt, "twelve");
  int (*with_calls)(void) = widcc_rt_lookup(jit->rt, "with_calls");
  int (*twenty)(void) = widcc_rt_lookup(jit->rt, "twenty");
  int (*params_and_locals)(void) = widcc_rt_lookup(jit->rt, "params_and_locals");
  // Twelve parameters put this function on the Block ABI.
  int (*twelve_params)(void *, intptr_t) =
      widcc_rt_lookup(jit->rt, "twelve_params");
  CHECK(twelve && with_calls && twenty && params_and_locals && twelve_params);

  // The same loops on the host.
  {
    int a = 1, b = 2, c = 3, d = 4, e = 5, f = 6;
    int g = 7, h = 8, i = 9, j = 10, k = 11, l = 12;
    int s = 0;
    for (int n = 0; n < 3; n++) {
      s += a + b + c + d + e + f + g + h + i + j + k + l;
      a++; b++; c++; d++; e++; f++; g++; h++; i++; j++; k++; l++;
    }
    CHECK(twelve() == s + a + b + c + d + e + f + g + h + i + j + k + l);

    int a2 = 1, b2 = 2, c2 = 3, d2 = 4, e2 = 5;
    int f2 = 6, g2 = 7, h2 = 8, i2 = 9, j2 = 10;
    for (int n = 0; n < 3; n++) {
      a2 += 1; b2 += 2; c2 += 3; d2 += 4; e2 += 5;
      f2 += 6; g2 += 7; h2 += 8; i2 += 9; j2 += 10;
    }
    CHECK(with_calls() == a2 * 1000 + b2 * 100 + c2 * 10 + d2 +
                             e2 + f2 + g2 + h2 + i2 + j2);
  }
  CHECK(twenty() == 210);
  CHECK(params_and_locals() == 21 + 70 + 8);
  long long args12[12] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
  CHECK(twelve_params(args12, 12) == 650);
}

// Float parameters, float returns and the Block ABI with mixed classes.
static void test_float_abi(JIT *jit) {
  CHECK_COMPILE(jit, "fabi.c",
    "double four(double a, double b, double c, double d) {\n"
    "  return a + b * 2 + c * 3 + d * 4;\n"
    "}\n"
    "double five(double a, double b, double c, double d, double e) {\n"
    "  return a + b * 2 + c * 3 + d * 4 + e * 5;\n"
    "}\n"
    "double mixed(int a, double b, int c, double d) {\n"
    "  return a + b * 2 + c * 3 + d * 4;\n"
    "}\n"
    "double paths(int x) { return x > 0 ? x * 1.5 : -x * 0.5; }\n"
    "int nan_cmp(double a, double b) {\n"
    "  return (a < b) + (a <= b) * 2 + (a > b) * 4 + (a >= b) * 8\n"
    "       + (a == b) * 16 + (a != b) * 32;\n"
    "}\n"
    "int d2i(double d) { return (int)d; }\n"
    "double i2d(int i, unsigned u) { return i * 0.5 + u * 0.25; }\n"
    "struct V { double x; float y; };\n"
    "double struct_arr(void) {\n"
    "  struct V vs[3];\n"
    "  for (int i = 0; i < 3; i++) { vs[i].x = i * 1.5; vs[i].y = i * 0.5f; }\n"
    "  struct V t = vs[2];\n"
    "  return t.x + t.y + vs[0].x + vs[1].y;\n"
    "}\n");

  double (*four)(double, double, double, double) =
      widcc_rt_lookup(jit->rt, "four");
  double (*five)(void *, intptr_t) = widcc_rt_lookup(jit->rt, "five");
  double (*mixed)(int, double, int, double) =
      widcc_rt_lookup(jit->rt, "mixed");
  double (*paths)(int) = widcc_rt_lookup(jit->rt, "paths");
  int (*nan_cmp)(double, double) = widcc_rt_lookup(jit->rt, "nan_cmp");
  int (*d2i)(double) = widcc_rt_lookup(jit->rt, "d2i");
  double (*i2d)(int, unsigned) = widcc_rt_lookup(jit->rt, "i2d");
  double (*struct_arr)(void) = widcc_rt_lookup(jit->rt, "struct_arr");
  CHECK(four && five && mixed && paths && nan_cmp && d2i && i2d && struct_arr);

  CHECK(four(1.5, 2.5, 3.5, 4.5) == 35.0);
  CHECK(mixed(1, 2.5, 3, 4.5) == 33.0);
  CHECK(paths(3) == 4.5 && paths(-3) == 1.5);
  CHECK(d2i(-7.9) == -7);
  CHECK(i2d(-4, 8u) == -2.0 + 2.0);
  CHECK(struct_arr() == 4.5);

  // NaN is unordered: it is not less, equal or greater than anything, so
  // only != is true.
  CHECK(nan_cmp(1.0, 2.0) == 1 + 2 + 32);
  CHECK(nan_cmp(2.0, 1.0) == 4 + 8 + 32);
  CHECK(nan_cmp(1.0, 1.0) == 2 + 8 + 16);
  double nan = 0.0 / 0.0;
  CHECK(nan != nan);
  CHECK(nan_cmp(nan, 1.0) == 32);
  CHECK(nan_cmp(1.0, nan) == 32);

  // Block ABI: five doubles arrive through the argument block.
  union { double d; long long w; } a, b, c, d, e;
  a.d = 1.0; b.d = 2.0; c.d = 3.0; d.d = 4.0; e.d = 5.0;
  long long args[5] = {a.w, b.w, c.w, d.w, e.w};
  CHECK(five(args, 5) == 55.0);
}

// Unsupported constructs must be rejected with a diagnostic, and the runtime
// must stay usable afterwards.
static void test_unsupported_diagnostics(JIT *jit) {
  static const struct {
    const char *name;
    const char *src;
    const char *needle;
  } bad[] = {
    {"bad_bitfield.c",
     "struct B { int a : 3; };\nint f(void) { struct B b; b.a = 1; return b.a; }\n",
     "bitfield"},
    {"bad_vla.c", "int f(int n) { int a[n]; a[0] = 1; return a[0]; }\n", "VLA"},
    {"bad_struct_arg.c",
     "struct P { int x; };\nint f(struct P p) { return p.x; }\n", "argument"},
    {"bad_struct_ret.c",
     "struct P { int x; };\nstruct P f(void) { struct P p; return p; }\n",
     "return"},
    {"bad_long_double.c", "long double f(void) { return 1.0L; }\n",
     "long double"},
    {"bad_alloca.c", "int f(void) { char *p = __builtin_alloca(8); return p != 0; }\n",
     "alloca"},
    {"bad_tls.c", "_Thread_local int t;\nint f(void) { return t; }\n", "TLS"},
    {"bad_fmod.c", "double f(double a, double b) { return a % b; }\n", "%"},
  };

  for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
    const char *err = widcc_rt_load_code(jit->rt, bad[i].name, bad[i].src, NULL);
    if (!err) {
      fprintf(stderr, "%s: expected a compile error\n", bad[i].name);
      exit(1);
    }
    if (!strstr(err, bad[i].needle)) {
      fprintf(stderr, "%s: unexpected diagnostic: %s\n", bad[i].name, err);
      exit(1);
    }
  }

  // A failed load leaves the runtime usable.
  CHECK_COMPILE(jit, "after_bad.c", "int recovered(void) { return 5; }\n");
  int (*recovered)(void) = widcc_rt_lookup(jit->rt, "recovered");
  CHECK(recovered && recovered() == 5);
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

  test_operators(&jit);
  test_conversions(&jit);
  test_aggregates(&jit);
  test_initializers(&jit);
  test_indirect_calls(&jit);
  test_control_flow(&jit);
  test_preprocessor(&jit);
  test_loop_conditions(&jit);
  test_ternary_expressions(&jit);
  test_wide_expressions(&jit);
  test_many_locals(&jit);
  test_float_abi(&jit);
  test_unsupported_diagnostics(&jit);

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
