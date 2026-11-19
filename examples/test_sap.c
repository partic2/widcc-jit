#include <stdio.h>
#include "widcc_jit.h"

int main(void) {
  WidccRT *rt = widcc_rt_new();
  WidccCompiler *cc = widcc_cc_new(rt);
  if (!rt || !cc) { fprintf(stderr, "new failed\n"); return 1; }

  if (widcc_cc_compile(cc, "sap.c",
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
    "}\n") != 0) {
    fprintf(stderr, "compile failed: %s\n", widcc_cc_error(cc));
    return 1;
  }
  
  int (*local_arr)(void) = widcc_rt_lookup(rt, "local_arr");
  fprintf(stderr, "local_arr()=%d expected=30\n", local_arr());
  widcc_cc_free(cc);
  widcc_rt_free(rt);
  return 0;
}
