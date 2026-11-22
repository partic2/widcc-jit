#ifndef WIDCC_CCCTX_C
#define WIDCC_CCCTX_C

// Per-instance compiler context implementation: arena allocator and
// context lifecycle. See ccctx.h.
#include "widcc.h"
#include "ccctx.h"

static _Thread_local CCCtx *cc_ctx;
static CCCtx cc_default_ctx;

// Compiler instances run on their own thread; the default context only
// backs frontend calls made outside a compilation (where errors exit).
static CCCtx *ccctx(void) { return cc_ctx ? cc_ctx : &cc_default_ctx; }

static void *cc_malloc(size_t size) {
  CCArenaBlk *blk = malloc(sizeof(CCArenaBlk) + size);
  if (!blk)
    error("out of memory");
  blk->size = size;
  CCCtx *ctx = ccctx();
  blk->next = ctx->arena;
  ctx->arena = blk;
  return blk->data;
}

static void *cc_calloc(size_t n, size_t size) {
  void *p = cc_malloc(n * size);
  memset(p, 0, n * size);
  return p;
}

static void *cc_realloc(void *p, size_t newsize) {
  if (!p)
    return cc_malloc(newsize);
  CCArenaBlk *blk = (CCArenaBlk *)p - 1;
  if (blk->size >= newsize)
    return p;
  void *np = cc_malloc(newsize);
  memcpy(np, p, blk->size);
  return np; // old block stays in the arena until instance teardown
}

static char *cc_strdup(char *s) {
  return cc_strndup(s, strlen(s));
}

static char *cc_strndup(char *s, size_t n) {
  char *buf = cc_malloc(n + 1);
  memcpy(buf, s, n);
  buf[n] = '\0';
  return buf;
}

static void ccctx_init(CCCtx *ctx) {
  memset(ctx, 0, sizeof(*ctx));
  ctx->opt_reuse_stack = true;
  ctx->err_print = true;
}

// Reset all per-compilation frontend state. The arena and everything
// allocated from it (including the instance symbol tables) is kept.
static void ccctx_reset_frontend(CCCtx *ctx) {
  ctx->err_msg[0] = '\0';

  ctx->current_file = NULL;
  ctx->input_files = NULL;
  ctx->tok_at_bol = true;
  ctx->tok_has_space = false;
  memset(&ctx->input_files_map, 0, sizeof(HashMap));
  ctx->file_no = 0;

  ctx->locked_macros = NULL;
  memset(&ctx->macros, 0, sizeof(HashMap));
  ctx->cond_incl = NULL;
  memset(&ctx->pragma_once, 0, sizeof(HashMap));
  memset(&ctx->include_guards, 0, sizeof(HashMap));
  memset(&ctx->incl_path_cache, 0, sizeof(HashMap));
  ctx->base_file = NULL;
  ctx->pp_counter = 0;

  ctx->globals = NULL;
  ctx->scope = NULL;
  ctx->current_fn = NULL;
  ctx->gotos = NULL;
  ctx->labels = NULL;
  ctx->brk_label = NULL;
  ctx->cont_label = NULL;
  ctx->current_switch = NULL;
  ctx->current_vla = NULL;
  ctx->brk_vla = NULL;
  ctx->cont_vla = NULL;
  ctx->fn_use_vla = false;
  ctx->dont_dealloc_vla = false;
  ctx->eval_recover = NULL;
}

static void ccctx_destroy(CCCtx *ctx) {
  CCArenaBlk *blk = ctx->arena;
  while (blk) {
    CCArenaBlk *next = blk->next;
    free(blk);
    blk = next;
  }
  ctx->arena = NULL;
}

#endif // WIDCC_CCCTX_C
