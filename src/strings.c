#ifndef WIDCC_STRINGS_C
#define WIDCC_STRINGS_C

#include "widcc.h"
#include "ccctx.h"

// Round up `n` to the nearest multiple of `align`. For instance,
// align_to(5, 8) returns 8 and align_to(11, 8) returns 16.
static int align_to(int n, int align) {
  return (n + align - 1) / align * align;
}

// Returns true if a given file exists.
static bool file_exists(char *path) {
  struct stat st;
  return !stat(path, &st);
}

static void strarray_push(StringArray *arr, char *s) {
  if (!arr->data) {
    arr->data = cc_calloc(8, sizeof(char *));
    arr->capacity = 8;
  }

  if (arr->capacity == arr->len) {
    arr->data = cc_realloc(arr->data, sizeof(char *) * arr->capacity * 2);
    arr->capacity *= 2;
    for (int i = arr->len; i < arr->capacity; i++)
      arr->data[i] = NULL;
  }

  arr->data[arr->len++] = s;
}

// Takes a printf-style format string and returns a formatted string.
static char *format(char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  va_list ap2;
  va_copy(ap2, ap);

  // First pass: compute the required length.
  int len = vsnprintf(NULL, 0, fmt, ap);
  va_end(ap);

  if (len < 0) {
    va_end(ap2);
    return NULL;
  }

  char *buf = (char *)malloc((size_t)len + 1);
  if (!buf) {
    va_end(ap2);
    return NULL;
  }

  // Second pass: actually format.
  vsnprintf(buf, (size_t)len + 1, fmt, ap2);
  va_end(ap2);

  // Move to the instance arena so it is released with the instance.
  char *ret = cc_strdup(buf);
  free(buf);
  return ret;
}
#endif // WIDCC_STRINGS_C
