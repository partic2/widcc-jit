// Single-translation-unit build of the widcc runtime.
//
// Every implementation file is included here and compiled as one object.
// All internal symbols have static linkage, so the only names the library
// exports are the widcc_jit.h API (plus the bundled sljit object library,
// which is third-party and stays as-is).
#include "widcc.h"

#include "ccctx.c"
#include "fileops.c"
#include "hashmap.c"
#include "strings.c"
#include "unicode.c"
#include "type.c"
#include "tokenize.c"
#include "preprocess.c"
#include "parse.c"


//jitgen.c
#include "jitgen.c"
#include "jitgen_emit.c"
#include "jitgen_expr.c"
#include "jitgen_stmt.c"
#include "jitgen_func.c"
#undef WIDCC_HOST_64BIT
#undef JE
#undef REAL_INT_REGS
#undef SREG_ADDR_OK

#include "jit.c"