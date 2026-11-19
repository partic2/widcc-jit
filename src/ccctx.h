// Per-instance compiler context for widcc JIT runtime.
//
// All mutable global state of the widcc frontend lives in CCCtx so that
// multiple compiler instances can coexist in one process and each instance
// can compile many translation units. All frontend allocations go through
// the per-context arena and are released in bulk when the instance is
// destroyed.
#ifndef CCCTX_H
#define CCCTX_H

#include <setjmp.h>

typedef struct CCArenaBlk CCArenaBlk;
struct CCArenaBlk {
  CCArenaBlk *next;
  size_t size;
  char data[];
};

typedef struct CCCtx {
  // Arena: every block is individually malloc'd and linked; all are freed
  // when the owning instance is destroyed.
  CCArenaBlk *arena;

  // Error recovery: when err_active is set, error*() formats the message
  // into err_msg and longjmps instead of exiting the process.
  jmp_buf err_jmp;
  bool err_active;
  bool err_print; // also print errors to stderr
  char err_msg[2048];

  // Driver options used by the frontend (owned by the instance for JIT use).
  StdVer opt_std;
  bool opt_werror;
  bool opt_g;
  bool opt_E;
  bool opt_cc1_asm_pp;
  bool opt_reuse_stack;
  StringArray include_paths;
  StringArray iquote_paths;

  // tokenize.c state
  File *current_file;
  File **input_files;
  bool tok_at_bol;
  bool tok_has_space;
  HashMap input_files_map; // add_input_file path -> File
  int file_no;             // add_input_file counter
  HashMap keyword_map;     // is_keyword lazy table

  // preprocess.c state
  struct Macro *locked_macros;
  HashMap macros;
  struct CondIncl *cond_incl;
  HashMap pragma_once;
  HashMap include_guards;
  HashMap incl_path_cache; // search_include_paths3 cache
  char *base_file;
  int pp_counter;          // __COUNTER__ serial

  // parse.c state
  Obj *globals;
  Scope *scope;
  Obj *current_fn;
  Node *gotos;
  Node *labels;
  char *brk_label;
  char *cont_label;
  Node *current_switch;
  Obj *current_vla;
  Obj *brk_vla;
  Obj *cont_vla;
  bool fn_use_vla;
  bool dont_dealloc_vla;
  bool *eval_recover;

  // parse.c misc
  int unique_id;        // new_unique_name serial (monotonic per instance)
  HashMap typename_map; // is_typename lazy table
} CCCtx;

// The runtime is built as a single translation unit (src/all.c), so every
// internal symbol has internal linkage: only the widcc_jit.h API is global.
static CCCtx *ccctx(void);

// Arena allocation API (uses ccctx()).
static void *cc_malloc(size_t size);
static void *cc_calloc(size_t n, size_t size);
static void *cc_realloc(void *p, size_t newsize);
static char *cc_strdup(char *s);
static char *cc_strndup(char *s, size_t n);

static void ccctx_init(CCCtx *ctx);
static void ccctx_reset_frontend(CCCtx *ctx);
static void ccctx_destroy(CCCtx *ctx);

#endif
