#ifndef WIDCC_TOKENIZE_C
#define WIDCC_TOKENIZE_C

#include "widcc.h"
#include "ccctx.h"

// Per-instance tokenizer state (see ccctx.h).
#define g_current_file (ccctx()->current_file)
#define g_input_files  (ccctx()->input_files)
#define g_at_bol       (ccctx()->tok_at_bol)
#define g_has_space    (ccctx()->tok_has_space)

// Reports an error. When a JIT instance armed an error handler, formats the
// message into the context and longjmps back instead of exiting.
static void error(char *fmt, ...) {
  CCCtx *ctx = ccctx();
  va_list ap;
  va_start(ap, fmt);
  if (ctx->err_active) {
    vsnprintf(ctx->err_msg, sizeof(ctx->err_msg), fmt, ap);
    va_end(ap);
    if (ctx->err_print)
      fprintf(stderr, "%s\n", ctx->err_msg);
    longjmp(ctx->err_jmp, 1);
  }
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fprintf(stderr, "\n");
  exit(1);
}

// Reports an error message in the following format.
//
// foo.c:10: x = y + 1;
//               ^ <error message here>
void verror_at(char *filename, char *input, int line_no,
               char *loc, char *fmt, va_list ap) {
  // Find a line containing `loc`.
  char *line = loc;
  while (input < line && line[-1] != '\n')
    line--;

  char *end = loc;
  while (*end && *end != '\n')
    end++;

  // Print out the line.
  fprintf(stderr, "%s:%d: \n", filename, line_no);
  fprintf(stderr, "%.*s\n", (int)(end - line), line);

  // Show the error message.
  int pos = display_width(line, loc - line);

  fprintf(stderr, "%*s", pos, ""); // print pos spaces.
  fprintf(stderr, "^ ");
  vfprintf(stderr, fmt, ap);
  fprintf(stderr, "\n");
}

static void error_at(char *loc, char *fmt, ...) {
  int line_no = 1;
  for (char *p = g_current_file->contents; p < loc; p++)
    if (*p == '\n')
      line_no++;

  CCCtx *ctx = ccctx();
  va_list ap;
  va_start(ap, fmt);
  if (ctx->err_active) {
    char msg[1600];
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    snprintf(ctx->err_msg, sizeof(ctx->err_msg), "%s:%d: %s",
             g_current_file->name, line_no, msg);
    if (ctx->err_print)
      fprintf(stderr, "%s:%d: %s\n", g_current_file->name, line_no, msg);
    longjmp(ctx->err_jmp, 1);
  }
  verror_at(g_current_file->name, g_current_file->contents, line_no, loc, fmt, ap);
  va_end(ap);
  exit(1);
}

static void error_tok(Token *tok, char *fmt, ...) {
  CCCtx *ctx = ccctx();
  va_list ap;
  va_start(ap, fmt);
  if (ctx->err_active) {
    char msg[1600];
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    snprintf(ctx->err_msg, sizeof(ctx->err_msg), "%s:%d: %s",
             tok->file->name, tok->line_no, msg);
    if (ctx->err_print) {
      va_start(ap, fmt);
      verror_at(tok->file->name, tok->file->contents, tok->line_no, tok->loc, fmt, ap);
      va_end(ap);
      if (tok->origin)
        warn_tok(tok->origin, "in expansion of macro");
    }
    longjmp(ctx->err_jmp, 1);
  }
  verror_at(tok->file->name, tok->file->contents, tok->line_no, tok->loc, fmt, ap);
  va_end(ap);

  if (tok->origin)
    warn_tok(tok->origin, "in expansion of macro");
  exit(1);
}

static void warn_tok(Token *tok, char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  verror_at(tok->file->name, tok->file->contents, tok->line_no, tok->loc, fmt, ap);
  va_end(ap);
  if (ccctx()->opt_werror)
    exit(1);
}

// Consumes the current token if it matches `op`.
static bool equal(Token *tok, char *op) {
  return strlen(op) == tok->len && !memcmp(tok->loc, op, tok->len);
}

// Ensure that the current token is `op`.
static Token *skip(Token *tok, char *op) {
  if (!equal(tok, op))
    error_tok(tok, "expected '%s'", op);
  return tok->next;
}

static bool consume(Token **rest, Token *tok, char *str) {
  if (equal(tok, str)) {
    *rest = tok->next;
    return true;
  }
  return false;
}

// Create a new token.
static Token *new_token(TokenKind kind, char *start, char *end) {
  Token *tok = cc_calloc(1, sizeof(Token));
  tok->kind = kind;
  tok->loc = start;
  tok->len = end - start;
  tok->file = g_current_file;
  tok->at_bol = g_at_bol;
  tok->has_space = g_has_space;

  g_at_bol = g_has_space = false;
  return tok;
}

static bool startswith(char *p, char *q) {
  return strncmp(p, q, strlen(q)) == 0;
}

// Read an identifier and returns the length of it.
// If p does not point to a valid identifier, 0 is returned.
static int read_ident(char *start) {
  char *p = start;
  uint32_t c = decode_utf8(&p, p);
  if (!is_ident1(c) || (c == '$' && ccctx()->opt_cc1_asm_pp))
    return 0;

  for (;;) {
    char *q;
    c = decode_utf8(&q, p);
    if (!is_ident2(c))
      return p - start;
    p = q;
  }
}

static int from_hex(char c) {
  if ('0' <= c && c <= '9')
    return c - '0';
  if ('a' <= c && c <= 'f')
    return c - 'a' + 10;
  return c - 'A' + 10;
}

// Read a punctuator token from p and returns its length.
static int read_punct(char *p) {
  bool is_repeat = p[1] == *p;
  bool is_assign = p[1] == '=';

  switch (*p) {
  case '-':
    if (p[1] == '>')
      return 2;
  case '&':
  case '+':
  case '=':
  case '|':
    return (is_repeat | is_assign) + 1;
  case '<':
  case '>':
    if (is_repeat)
      return (p[2] == '=') + 2;
  case '!':
  case '%':
  case '*':
  case '/':
  case '^':
    return is_assign + 1;
  case '#':
    return is_repeat + 1;
  case '.':
    return (is_repeat && p[2] == *p) ? 3 : 1;
  case '$':
    return ccctx()->opt_cc1_asm_pp;
  case '(':
  case ')':
  case ',':
  case ':':
  case ';':
  case '?':
  case '@':
  case '[':
  case '\\':
  case ']':
  case '`':
  case '{':
  case '}':
  case '~':
    return 1;
  }
  return 0;
}

static bool is_keyword(Token *tok) {
  HashMap *map = &ccctx()->keyword_map;

  if (map->capacity == 0) {
    static char *kw[] = {
      "return", "if", "else", "for", "while", "int", "sizeof", "char",
      "struct", "union", "short", "long", "void", "typedef", "_Bool",
      "enum", "static", "goto", "break", "continue", "switch", "case",
      "default", "extern", "_Alignof", "do", "signed",
      "unsigned", "const", "volatile", "auto", "register", "restrict",
      "__restrict", "__restrict__", "_Noreturn", "float", "double",
      "_Thread_local", "__thread", "__attribute__",
      "__asm", "__asm__", "__typeof", "__typeof__", "inline",
    };

    for (int i = 0; i < sizeof(kw) / sizeof(*kw); i++)
      hashmap_put(map, kw[i], (void *)1);

    if (ccctx()->opt_std == STD_NONE)
      hashmap_put(map, "asm", (void *)1);
    if (ccctx()->opt_std == STD_NONE || ccctx()->opt_std >= STD_C23)
      hashmap_put(map, "typeof", (void *)1);      
  }

  return hashmap_get2(map, tok->loc, tok->len);
}

static int read_escaped_char(char **new_pos, char *p) {
  if ('0' <= *p && *p <= '7') {
    // Read an octal number.
    int c = *p++ - '0';
    if ('0' <= *p && *p <= '7') {
      c = (c << 3) + (*p++ - '0');
      if ('0' <= *p && *p <= '7')
        c = (c << 3) + (*p++ - '0');
    }
    *new_pos = p;
    return c;
  }

  if (*p == 'x') {
    // Read a hexadecimal number.
    p++;
    if (!isxdigit(*p))
      error_at(p, "invalid hex escape sequence");

    int c = 0;
    for (; isxdigit(*p); p++)
      c = ((unsigned)c << 4) + from_hex(*p);
    *new_pos = p;
    return c;
  }

  *new_pos = p + 1;

  // Escape sequences are defined using themselves here. E.g.
  // '\n' is implemented using '\n'. This tautological definition
  // works because the compiler that compiles our compiler knows
  // what '\n' actually is. In other words, we "inherit" the ASCII
  // code of '\n' from the compiler that compiles our compiler,
  // so we don't have to teach the actual code here.
  //
  // This fact has huge implications not only for the correctness
  // of the compiler but also for the security of the generated code.
  // For more info, read "Reflections on Trusting Trust" by Ken Thompson.
  // https://github.com/rui314/chibicc/wiki/thompson1984.pdf
  switch (*p) {
  case 'a': return '\a';
  case 'b': return '\b';
  case 't': return '\t';
  case 'n': return '\n';
  case 'v': return '\v';
  case 'f': return '\f';
  case 'r': return '\r';
  // [GNU] \e for the ASCII escape character is a GNU C extension.
  case 'e': return 27;
  default: return *p;
  }
}

// Find a closing double-quote.
static char *string_literal_end(char *p) {
  char *start = p;
  for (; *p != '"'; p++) {
    if (*p == '\n' || *p == '\0')
      error_at(start, "unclosed string literal");
    if (*p == '\\')
      p++;
  }
  return p;
}

static Token *read_string_literal(char *start, char *quote) {
  char *end = string_literal_end(quote + 1);
  char *buf = cc_calloc(1, end - quote);
  int len = 0;

  for (char *p = quote + 1; p < end;) {
    if (*p == '\\')
      buf[len++] = read_escaped_char(&p, p + 1);
    else
      buf[len++] = *p++;
  }

  Token *tok = new_token(TK_STR, start, end + 1);
  tok->ty = array_of(ty_pchar, len + 1);
  tok->str = buf;
  return tok;
}

// Read a UTF-8-encoded string literal and transcode it in UTF-16.
//
// UTF-16 is yet another variable-width encoding for Unicode. Code
// points smaller than U+10000 are encoded in 2 bytes. Code points
// equal to or larger than that are encoded in 4 bytes. Each 2 bytes
// in the 4 byte sequence is called "surrogate", and a 4 byte sequence
// is called a "surrogate pair".
static Token *read_utf16_string_literal(char *start, char *quote) {
  char *end = string_literal_end(quote + 1);
  uint16_t *buf = cc_calloc(2, end - start);
  int len = 0;

  for (char *p = quote + 1; p < end;) {
    if (*p == '\\') {
      buf[len++] = read_escaped_char(&p, p + 1);
      continue;
    }

    uint32_t c = decode_utf8(&p, p);
    if (c < 0x10000) {
      // Encode a code point in 2 bytes.
      buf[len++] = c;
    } else {
      // Encode a code point in 4 bytes.
      c -= 0x10000;
      buf[len++] = 0xd800 + ((c >> 10) & 0x3ff);
      buf[len++] = 0xdc00 + (c & 0x3ff);
    }
  }

  Token *tok = new_token(TK_STR, start, end + 1);
  tok->ty = array_of(ty_ushort, len + 1);
  tok->str = (char *)buf;
  return tok;
}

// Read a UTF-8-encoded string literal and transcode it in UTF-32.
//
// UTF-32 is a fixed-width encoding for Unicode. Each code point is
// encoded in 4 bytes.
static Token *read_utf32_string_literal(char *start, char *quote, Type *ty) {
  char *end = string_literal_end(quote + 1);
  uint32_t *buf = cc_calloc(4, end - quote);
  int len = 0;

  for (char *p = quote + 1; p < end;) {
    if (*p == '\\')
      buf[len++] = read_escaped_char(&p, p + 1);
    else
      buf[len++] = decode_utf8(&p, p);
  }

  Token *tok = new_token(TK_STR, start, end + 1);
  tok->ty = array_of(ty, len + 1);
  tok->str = (char *)buf;
  return tok;
}

static Token *read_char_literal(char *start, char *quote, Type *ty) {
  char *p = quote + 1;
  if (*p == '\0')
    error_at(start, "unclosed char literal");

  int c;
  if (*p == '\\')
    c = read_escaped_char(&p, p + 1);
  else
    c = decode_utf8(&p, p);

  char *end = strchr(p, '\'');
  if (!end)
    error_at(p, "unclosed char literal");

  Token *tok = new_token(TK_NUM, start, end + 1);
  tok->val = c;
  tok->ty = ty;
  return tok;
}

static bool convert_pp_int(Token *tok) {
  char *p = tok->loc;

  // Read a binary, octal, decimal or hexadecimal number.
  int base = 10;
  if (!strncasecmp(p, "0x", 2) && isxdigit(p[2])) {
    p += 2;
    base = 16;
  } else if (!strncasecmp(p, "0b", 2) && (p[2] == '0' || p[2] == '1')) {
    p += 2;
    base = 2;
  } else if (*p == '0') {
    base = 8;
  }

  // Use strtoull so that 64-bit literals survive on ILP32 hosts, where
  // `unsigned long` is only 32 bits wide.
  int64_t val = strtoull(p, &p, base);

  // Read U, L or LL suffixes.
  bool ll = false;
  bool l = false;
  bool u = false;

  if (startswith(p, "LLU") || startswith(p, "LLu") ||
      startswith(p, "llU") || startswith(p, "llu") ||
      startswith(p, "ULL") || startswith(p, "Ull") ||
      startswith(p, "uLL") || startswith(p, "ull")) {
    p += 3;
    ll = u = true;
  } else if (!strncasecmp(p, "lu", 2) || !strncasecmp(p, "ul", 2)) {
    p += 2;
    l = u = true;
  } else if (startswith(p, "LL") || startswith(p, "ll")) {
    p += 2;
    ll = true;
  } else if (*p == 'L' || *p == 'l') {
    p++;
    l = true;
  } else if (*p == 'U' || *p == 'u') {
    p++;
    u = true;
  }

  if (p != tok->loc + tok->len)
    return false;

  // Infer a type. The result depends on the host data model: where `long`
  // is only 32 bits (ILP32 / LLP64) values above the 32-bit range promote
  // to `long long` instead of `long`.
  const bool wide_long = sizeof(long) >= 8;
  Type *ty;
  if (base == 10) {
    // int -> long -> long long, or unsigned int -> unsigned long ->
    // unsigned long long when the `u` suffix is present.
    if (ll && u) {
      ty = ty_ullong;
    } else if (l && u) {
      ty = (wide_long || !(val >> 32)) ? ty_ulong : ty_ullong;
    } else if (ll) {
      ty = ty_llong;
    } else if (l) {
      ty = (wide_long || !(val >> 31)) ? ty_long : ty_llong;
    } else if (u) {
      ty = !(val >> 32) ? ty_uint : (wide_long ? ty_ulong : ty_ullong);
    } else {
      ty = !(val >> 31) ? ty_int : (wide_long ? ty_long : ty_llong);
    }
  } else {
    // int -> unsigned int -> long -> unsigned long -> long long ->
    // unsigned long long.
    if (ll && u) {
      ty = ty_ullong;
    } else if (l && u) {
      ty = (wide_long || !(val >> 32)) ? ty_ulong : ty_ullong;
    } else if (ll) {
      ty = (val >> 63) ? ty_ullong : ty_llong;
    } else if (l) {
      if (wide_long)
        ty = (val >> 63) ? ty_ulong : ty_long;
      else if (!(val >> 31)) ty = ty_long;
      else if (!(val >> 32)) ty = ty_ulong;
      else if (!(val >> 63)) ty = ty_llong;
      else ty = ty_ullong;
    } else if (u) {
      ty = !(val >> 32) ? ty_uint : (wide_long ? ty_ulong : ty_ullong);
    } else if (!(val >> 31)) {
      ty = ty_int;
    } else if (!(val >> 32)) {
      ty = ty_uint;
    } else if (!(val >> 63)) {
      ty = wide_long ? ty_long : ty_llong;
    } else {
      ty = wide_long ? ty_ulong : ty_ullong;
    }
  }

  tok->kind = TK_NUM;
  tok->val = val;
  tok->ty = ty;
  return true;
}

// The definition of the numeric literal at the preprocessing stage
// is more relaxed than the definition of that at the later stages.
// In order to handle that, a numeric literal is tokenized as a
// "pp-number" token first and then converted to a regular number
// token after preprocessing.
//
// This function converts a pp-number token to a regular number token.
static void convert_pp_number(Token *tok) {
  // Try to parse as an integer constant.
  if (convert_pp_int(tok))
    return;

  // If it's not an integer, it must be a floating point constant.
  char *end;
  long double val = strtold(tok->loc, &end);

  Type *ty;
  if (*end == 'f' || *end == 'F') {
    ty = ty_float;
    end++;
  } else if (*end == 'l' || *end == 'L') {
    ty = ty_ldouble;
    end++;
  } else {
    ty = ty_double;
  }

  if (tok->loc + tok->len != end)
    error_tok(tok, "invalid numeric constant");

  tok->kind = TK_NUM;
  tok->fval = val;
  tok->ty = ty;
}

// Initialize line info for all tokens.
static void add_line_numbers(Token *tok) {
  char *p = g_current_file->contents;
  int n = 1;

  do {
    if (p == tok->loc) {
      tok->line_no = n;
      tok = tok->next;
    }
    if (*p == '\n')
      n++;
  } while (*p++);
}

static Token *tokenize_string_literal(Token *tok, Type *basety) {
  Token *t;
  if (basety->size == 2)
    t = read_utf16_string_literal(tok->loc, tok->loc);
  else
    t = read_utf32_string_literal(tok->loc, tok->loc, basety);
  t->next = tok->next;
  return t;
}

// Tokenize a given string and returns new tokens.
static Token *tokenize(File *file, Token **end) {
  g_current_file = file;

  char *p = file->contents;
  Token head = {0};
  Token *cur = &head;

  g_at_bol = true;
  g_has_space = false;

  while (*p) {
    // Skip newline.
    if (*p == '\n') {
      p++;
      g_at_bol = true;
      g_has_space = false;
      continue;
    }

    // Skip whitespace characters.
    if (isspace(*p)) {
      p++;
      g_has_space = true;
      continue;
    }

    // Skip line comments.
    if (startswith(p, "//")) {
      p += 2;
      while (*p != '\n')
        p++;
      g_has_space = true;
      continue;
    }

    // Skip block comments.
    if (startswith(p, "/*")) {
      char *q = strstr(p + 2, "*/");
      if (!q)
        error_at(p, "unclosed block comment");
      p = q + 2;
      g_has_space = true;
      continue;
    }

    // Numeric literal
    if (isdigit(*p) || (*p == '.' && isdigit(p[1]))) {
      char *q = p++;
      for (;;) {
        if (p[0] && p[1] && strchr("eEpP", p[0]) && strchr("+-", p[1])) {
          p += 2;
        } else if (*p == '.') {
          p++;
        } else if (*p == '$' && ccctx()->opt_cc1_asm_pp) {
          break;
        } else {
          char *pos;
          if (!is_ident2(decode_utf8(&pos, p)))
            break;
          p = pos;
        }
      }
      cur = cur->next = new_token(TK_PP_NUM, q, p);
      continue;
    }

    // String literal
    if (*p == '"') {
      cur = cur->next = read_string_literal(p, p);
      p += cur->len;
      continue;
    }

    // UTF-8 string literal
    if (startswith(p, "u8\"")) {
      cur = cur->next = read_string_literal(p, p + 2);
      p += cur->len;
      continue;
    }

    // UTF-16 string literal
    if (startswith(p, "u\"")) {
      cur = cur->next = read_utf16_string_literal(p, p + 1);
      p += cur->len;
      continue;
    }

    // Wide string literal
    if (startswith(p, "L\"")) {
      cur = cur->next = read_utf32_string_literal(p, p + 1, ty_int);
      p += cur->len;
      continue;
    }

    // UTF-32 string literal
    if (startswith(p, "U\"")) {
      cur = cur->next = read_utf32_string_literal(p, p + 1, ty_uint);
      p += cur->len;
      continue;
    }

    // Character literal
    if (*p == '\'') {
      cur = cur->next = read_char_literal(p, p, ty_int);
      cur->val = (char)cur->val;
      p += cur->len;
      continue;
    }

    // UTF-16 character literal
    if (startswith(p, "u'")) {
      cur = cur->next = read_char_literal(p, p + 1, ty_ushort);
      cur->val &= 0xffff;
      p += cur->len;
      continue;
    }

    // Wide character literal
    if (startswith(p, "L'")) {
      cur = cur->next = read_char_literal(p, p + 1, ty_int);
      p += cur->len;
      continue;
    }

    // UTF-32 character literal
    if (startswith(p, "U'")) {
      cur = cur->next = read_char_literal(p, p + 1, ty_uint);
      p += cur->len;
      continue;
    }

    // Identifier or keyword
    int ident_len = read_ident(p);
    if (ident_len) {
      cur = cur->next = new_token(TK_IDENT, p, p + ident_len);
      p += cur->len;
      continue;
    }

    // Punctuators
    int punct_len = read_punct(p);
    if (punct_len) {
      cur = cur->next = new_token(TK_PUNCT, p, p + punct_len);
      p += cur->len;
      continue;
    }

    error_at(p, "invalid token");
  }

  if (end && cur != &head)
    *end = cur;
  cur->next = new_token(TK_EOF, p, p);
  add_line_numbers(head.next);
  return head.next;
}

// Returns the contents of a given file.
static char *read_file(char *path) {
  FILE *fp;

  if (strcmp(path, "-") == 0) {
    // By convention, read from stdin if a given filename is "-".
    fp = stdin;
  } else {
    fp = fopen(path, "r");
    if (!fp)
      return NULL;
  }

  size_t buflen = 0;
  size_t cap = 4096;
  char *buf = (char *)cc_malloc(cap);
  if (!buf) {
    if (fp != stdin) fclose(fp);
    return NULL;
  }

  // Read the entire file.
  for (;;) {
    char buf2[4096];
    size_t n = fread(buf2, 1, sizeof(buf2), fp);
    if (n == 0)
      break;

    // Ensure capacity: +2 for potential trailing '\n' and '\0'.
    if (buflen + n + 2 > cap) {
      while (buflen + n + 2 > cap)
        cap *= 2;
      char *newbuf = (char *)cc_realloc(buf, cap);
      if (!newbuf) {
        free(buf);
        if (fp != stdin) fclose(fp);
        return NULL;
      }
      buf = newbuf;
    }

    memcpy(buf + buflen, buf2, n);
    buflen += n;
  }

  if (fp != stdin)
    fclose(fp);

  // Make sure that the last line is properly terminated with '\n'.
  if (buflen == 0 || buf[buflen - 1] != '\n')
    buf[buflen++] = '\n';
  buf[buflen] = '\0';

  return buf;
}

static File *new_file(char *name, int file_no, char *contents) {
  File *file = cc_calloc(1, sizeof(File));
  file->name = name;
  file->display_file = file;
  file->file_no = file_no;
  file->contents = contents;
  return file;
}

// Replaces \r or \r\n with \n.
static void canonicalize_newline(char *p) {
  int i = 0, j = 0;

  while (p[i]) {
    if (p[i] == '\r' && p[i + 1] == '\n') {
      i += 2;
      p[j++] = '\n';
    } else if (p[i] == '\r') {
      i++;
      p[j++] = '\n';
    } else {
      p[j++] = p[i++];
    }
  }

  p[j] = '\0';
}

// Removes backslashes followed by a newline.
static void remove_backslash_newline(char *p) {
  int i = 0, j = 0;

  // We want to keep the number of newline characters so that
  // the logical line number matches the physical one.
  // This counter maintain the number of newlines we have removed.
  int n = 0;

  while (p[i]) {
    if (p[i] == '\\' && p[i + 1] == '\n') {
      i += 2;
      n++;
    } else if (p[i] == '\n') {
      p[j++] = p[i++];
      for (; n > 0; n--)
        p[j++] = '\n';
    } else {
      p[j++] = p[i++];
    }
  }

  for (; n > 0; n--)
    p[j++] = '\n';
  p[j] = '\0';
}

static File *add_input_file(char *path, char *contents, int *incl_no) {
  HashMap *map = &ccctx()->input_files_map;

  File *file = hashmap_get(map, path);
  if (file)
    return file;

  int file_no = ccctx()->file_no;
  file = new_file(path, file_no + 1, contents);
  if (incl_no) {
    file->is_input = true;
    file->incl_no = *incl_no;
  }

  g_input_files = cc_realloc(g_input_files, sizeof(File *) * (file_no + 2));
  g_input_files[file_no] = file;
  g_input_files[file_no + 1] = NULL;
  ccctx()->file_no = file_no + 1;

  hashmap_put(map, path, file);
  return file;
}

static Token *tokenize_file(char *path, Token **end, int *incl_no) {
  char *p = read_file(path);
  if (!p)
    return NULL;

  // UTF-8 texts may start with a 3-byte "BOM" marker sequence.
  // If exists, just skip them because they are useless bytes.
  // (It is actually not recommended to add BOM markers to UTF-8
  // texts, but it's not uncommon particularly on Windows.)
  if (!memcmp(p, "\xef\xbb\xbf", 3))
    p += 3;

  canonicalize_newline(p);
  remove_backslash_newline(p);

  return tokenize(add_input_file(path, p, incl_no), end);
}


#undef g_current_file
#undef g_input_files
#undef g_at_bol
#undef g_has_space
#endif // WIDCC_TOKENIZE_C
