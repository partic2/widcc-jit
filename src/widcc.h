#ifndef WIDCC_H
#define WIDCC_H

#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include <assert.h>
#include <ctype.h>
#ifndef _WIN32
#include <libgen.h>
#endif
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#ifndef _WIN32
#include <unistd.h>
#endif

#include "widcc_jit.h"

#define MAX(x, y) ((x) < (y) ? (y) : (x))
#define MIN(x, y) ((x) < (y) ? (x) : (y))

#if defined(__GNUC__) && (__GNUC__ >= 3)
#define FMTCHK(x,y) __attribute__((format(printf,(x),(y))))
#define NORETURN __attribute__((noreturn))
#elif defined(__has_attribute)
#if __has_attribute(format)
#define FMTCHK(x,y) __attribute__((format(printf,(x),(y))))
#endif
#if __has_attribute(noreturn)
#define NORETURN __attribute__((noreturn))
#endif
#endif

#ifndef FMTCHK
#define FMTCHK(x,y)
#endif
#ifndef NORETURN
#define NORETURN
#endif

typedef struct Type Type;
typedef struct Node Node;
typedef struct Member Member;
typedef struct Relocation Relocation;

//
// hashmap.c
//

typedef struct {
  char *key;
  int keylen;
  void *val;
} HashEntry;

typedef struct {
  HashEntry *buckets;
  int capacity;
  int used;
  bool heap; // buckets are malloc'd (outlive the compiler arena)
} HashMap;

// The runtime is built as a single translation unit (src/all.c); every
// symbol declared here is internal to the library, i.e. static.
static void *hashmap_get(HashMap *map, char *key);
static void *hashmap_get2(HashMap *map, char *key, int keylen);
static void hashmap_put(HashMap *map, char *key, void *val);
static void hashmap_put2(HashMap *map, char *key, int keylen, void *val);
static void hashmap_delete(HashMap *map, char *key);
static void hashmap_delete2(HashMap *map, char *key, int keylen);
static void hashmap_init_heap(HashMap *map);
static void hashmap_destroy(HashMap *map);


//
// strings.c
//

typedef struct {
  char **data;
  int capacity;
  int len;
} StringArray;

static void strarray_push(StringArray *arr, char *s);
static char *format(char *fmt, ...) FMTCHK(1,2);
static int align_to(int n, int align);
static const WidccFileOps *widcc_file_ops(void);
static bool file_exists(const char *path);

//
// tokenize.c
//

// Token
typedef enum {
  TK_IDENT,   // Identifiers
  TK_PUNCT,   // Punctuators
  TK_KEYWORD, // Keywords
  TK_STR,     // String literals
  TK_NUM,     // Numeric literals
  TK_PP_NUM,  // Preprocessing numbers
  TK_FMARK,   // Filemarkers for -E
  TK_PMARK,   // Placermarkers
  TK_ATTR,    // GNU attribute
  TK_EOF,     // End-of-file markers
} TokenKind;

typedef struct File File;
struct File {
  char *name;
  int file_no;
  int incl_no;
  char *contents;

  // For #line directive
  File *display_file;
  int line_delta;
  bool is_input;
};

// Token type
typedef struct Token Token;
struct Token {
  TokenKind kind;   // Token kind
  Token *next;      // Next token
  int64_t val;      // If kind is TK_NUM, its value
  long double fval; // If kind is TK_NUM, its value
  char *loc;        // Token location
  int len;          // Token length
  Type *ty;         // Used if TK_NUM or TK_STR
  char *str;        // String literal contents including terminating '\0'

  File *file;       // Source location
  int line_no;      // Line number
  int display_line_no;
  int display_file_no;
  bool at_bol;      // True if this token is at beginning of line
  bool has_space;   // True if this token follows a space character
  bool dont_expand; // True if a macro token is encountered during the macro's expansion
  Token *origin;    // If this is expanded from a macro, the original token
  char *guard_file; // The path of a potentially include-guarded file
  Token *attr_next;
};

static void error(char *fmt, ...) FMTCHK(1,2) NORETURN;
static void error_at(char *loc, char *fmt, ...) FMTCHK(2,3) NORETURN;
static void error_tok(Token *tok, char *fmt, ...) FMTCHK(2,3) NORETURN;
static void warn_tok(Token *tok, char *fmt, ...) FMTCHK(2,3);
static void verror_at(char *filename, char *input, int line_no, char *loc, char *fmt, va_list ap);
static bool equal(Token *tok, char *op);
static Token *skip(Token *tok, char *op);
static bool consume(Token **rest, Token *tok, char *str);
static File *new_file(char *name, int file_no, char *contents);
static Token *tokenize_string_literal(Token *tok, Type *basety);
static Token *tokenize(File *file, Token **end);
static Token *tokenize_file(char *filename, Token **end, int *incl_no);
static File *add_input_file(char *path, char *content, int *incl_no);
static void convert_pp_number(Token *tok);
static bool is_keyword(Token *tok);

#define internal_error() \
  error("internal error at %s:%d", __FILE__, __LINE__)

//
// preprocess.c
//

static void init_macros(void);
static void define_macro(char *name, char *buf);
static void undef_macro(char *name);
static Token *preprocess(Token *tok,  char *input_file);

//
// parse.c
//

// Variable or function
typedef struct Obj Obj;
struct Obj {
  Obj *next;
  char *name;    // Variable name
  Type *ty;      // Type
  bool is_local; // local or global/function

  // Local variable
  int ofs;
  // JIT backend: 1-based sljit saved-register index when the variable is
  // register-allocated (0 = lives on the stack), and whether its address
  // escapes via `&`.
  int jreg;
  bool address_taken;
  Obj *param_next;
  Obj *param_promoted;
  Obj *vla_next;
  Node *arg_expr;

  // Global variable or function
  bool is_definition;
  bool is_static;
  Obj *static_lvars;

  // Global variable
  bool is_tentative;
  bool is_tls;
  char *init_data;
  Relocation *rel;

  // Function
  bool is_inline;
  bool dealloc_vla;
  Node *body;

  // Static inline function
  bool is_live;
  bool is_referenced;
  StringArray refs;

  // JIT backend: final address of the definition (data block or
  // generated code entry), NULL when not emitted yet.
  void *jit_addr;
};

// Global variable can be initialized either by a constant expression
// or a pointer to another global variable. This struct represents the
// latter.
struct Relocation {
  Relocation *next;
  int offset;
  char **label;
  long addend;
};

// AST node
typedef enum {
  ND_NULL_EXPR, // Do nothing
  ND_ADD,       // +
  ND_SUB,       // -
  ND_MUL,       // *
  ND_DIV,       // /
  ND_POS,       // unary +
  ND_NEG,       // unary -
  ND_MOD,       // %
  ND_BITAND,    // &
  ND_BITOR,     // |
  ND_BITXOR,    // ^
  ND_SHL,       // <<
  ND_SHR,       // >>
  ND_SAR,       // >>
  ND_EQ,        // ==
  ND_NE,        // !=
  ND_LT,        // <
  ND_LE,        // <=
  ND_GT,        // >
  ND_GE,        // >=
  ND_ASSIGN,    // =
  ND_COND,      // ?:
  ND_COMMA,     // ,
  ND_MEMBER,    // . (struct member access)
  ND_ADDR,      // unary &
  ND_DEREF,     // unary *
  ND_NOT,       // !
  ND_BITNOT,    // ~
  ND_LOGAND,    // &&
  ND_LOGOR,     // ||
  ND_RETURN,    // "return"
  ND_IF,        // "if"
  ND_FOR,       // "for" or "while"
  ND_DO,        // "do"
  ND_SWITCH,    // "switch"
  ND_CASE,      // "case"
  ND_BLOCK,     // { ... }
  ND_GOTO,      // "goto"
  ND_GOTO_EXPR, // "goto" labels-as-values
  ND_LABEL,     // Labeled statement
  ND_LABEL_VAL, // [GNU] Labels-as-values
  ND_FUNCALL,   // Function call
  ND_EXPR_STMT, // Expression statement
  ND_STMT_EXPR, // Statement expression
  ND_VAR,       // Variable
  ND_NUM,       // Integer
  ND_CAST,      // Type cast
  ND_MEMZERO,   // Zero-clear a stack variable
  ND_ASM,       // "asm"
  ND_VA_START,  // "va_start"
  ND_VA_COPY,   // "va_copy"
  ND_VA_ARG,    // "va_arg"
  ND_CHAIN,     // ND_COMMA without array-to-pointer conversion
  ND_ALLOCA
} NodeKind;

// AST node type
struct Node {
  NodeKind kind; // Node kind
  Node *next;    // Next node
  Type *ty;      // Type, e.g. int or pointer to int
  Token *tok;    // Representative token

  Node *lhs;     // Left-hand side
  Node *rhs;     // Right-hand side

  // "if" or "for" statement
  Node *cond;
  Node *then;
  Node *els;
  Node *init;
  Node *inc;

  // "break" and "continue" labels
  char *brk_label;
  char *cont_label;

  // Block or statement expression
  Node *body;

  // Struct member access
  Member *member;

  // Function call
  Obj *ret_buffer;
  Obj *args;

  // Goto or labeled statement, or labels-as-values
  char *label;
  char *unique_label;
  Node *goto_next;

  // Switch
  Node *case_next;
  Node *default_case;

  // Case
  long begin;
  long end;

  Obj *target_vla;
  Obj *top_vla;

  // "asm" string literal
  char *asm_str;

  // Variable
  Obj *var;

  // Numeric literal
  int64_t val;
  long double fval;
};

// Represents a block scope.
typedef struct Scope Scope;
struct Scope {
  Scope *parent;
  Scope *children;
  Scope *sibling_next;

  Obj *locals;
  bool is_temporary;
  // C has two block scopes; one is for variables/typedefs and
  // the other is for struct/union/enum tags.
  HashMap vars;
  HashMap tags;
};

static Node *new_cast(Node *expr, Type *ty);
static int64_t const_expr(Token **rest, Token *tok);
static Obj *parse(Token *tok);
static Token *skip_paren(Token *tok);
//
// type.c
//

typedef enum {
  TY_VOID,
  TY_BOOL,
  TY_PCHAR,
  TY_CHAR,
  TY_SHORT,
  TY_INT,
  TY_LONG,
  TY_LONGLONG,
  TY_FLOAT,
  TY_DOUBLE,
  TY_LDOUBLE,
  TY_ENUM,
  TY_PTR,
  TY_FUNC,
  TY_ARRAY,
  TY_VLA, // variable-length array
  TY_STRUCT,
  TY_UNION,
} TypeKind;

struct Type {
  TypeKind kind;
  int size;           // sizeof() value
  int align;          // alignment
  bool is_unsigned;   // unsigned or signed
  bool is_unspec_enum;
  Type *origin;       // for type compatibility check

  // Pointer-to or array-of type. We intentionally use the same member
  // to represent pointer/array duality in C.
  //
  // In many contexts in which a pointer is expected, we examine this
  // member instead of "kind" member to determine whether a type is a
  // pointer or not. That means in many contexts "array of T" is
  // naturally handled as if it were "pointer to T", as required by
  // the C spec.
  Type *base;

  // Array
  int array_len;

  // Variable-length array
  Node *vla_len; // # of elements
  Obj *vla_size; // sizeof() value

  // Struct
  Member *members;
  bool is_flexible;
  bool is_packed;

  // Function type
  Scope *scopes;
  Type *return_ty;
  Obj *param_list;
  Node *pre_calc;
  bool is_variadic;
  bool is_oldstyle;
};

// Struct member
struct Member {
  Member *next;
  Type *ty;
  Token *name;
  int idx;
  int offset;

  // Bitfield
  bool is_bitfield;
  int bit_offset;
  int bit_width;
};

static Type *ty_void;
static Type *ty_bool;

static Type *ty_pchar;

static Type *ty_char;
static Type *ty_short;
static Type *ty_int;
static Type *ty_long;
static Type *ty_llong;

static Type *ty_uchar;
static Type *ty_ushort;
static Type *ty_uint;
static Type *ty_ulong;
static Type *ty_ullong;

static Type *ty_float;
static Type *ty_double;
static Type *ty_ldouble;

static Type *ty_size_t;
static Type *ty_intptr_t;
static Type *ty_ptrdiff_t;

static bool is_integer(Type *ty);
static bool is_flonum(Type *ty);
static bool is_numeric(Type *ty);
static bool is_bitfield(Node *node);
static bool is_compatible(Type *t1, Type *t2);
static bool is_const_expr(Node *node, int64_t *val);
static void init_ty(void);
static Type *copy_type(Type *ty);
static Type *pointer_to(Type *base);
static Type *func_type(Type *return_ty);
static Type *array_of(Type *base, int size);
static Type *vla_of(Type *base, Node *expr);
static void add_type(Node *node);
static Type *new_type(TypeKind kind, int size, int align);

//
// unicode.c
//

static uint32_t decode_utf8(char **new_pos, char *p);
static bool is_ident1(uint32_t c);
static bool is_ident2(uint32_t c);
static int display_width(char *p, int len);

// C standard version, used by the frontend (ccctx.h, tokenize.c,
// preprocess.c, parse.c).
typedef enum {
  STD_NONE = 0,
  STD_C89,
  STD_C99,
  STD_C11,
  STD_C17,
  STD_C23
} StdVer;

#endif // WIDCC_H
