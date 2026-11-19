#ifndef WIDCC_FILEOPS_C
#define WIDCC_FILEOPS_C

#include "widcc.h"
#include <sys/stat.h>

static void *host_open(const char *path) { return fopen(path, "r"); }

static size_t host_read(void *file, void *buf, size_t size) {
  return fread(buf, 1, size, (FILE *)file);
}

static void host_close(void *file) { fclose((FILE *)file); }

static int host_exists(const char *path) {
  struct stat st;
  return stat(path, &st) == 0;
}

static const WidccFileOps host_ops = {host_open, host_read, host_close,
                                      host_exists};
static WidccFileOps file_ops = {host_open, host_read, host_close,
                                host_exists};

void widcc_set_file_ops(const WidccFileOps *ops) {
  file_ops = host_ops;
  if (!ops)
    return;
  if (ops->open)
    file_ops.open = ops->open;
  if (ops->read)
    file_ops.read = ops->read;
  if (ops->close)
    file_ops.close = ops->close;
  if (ops->exists)
    file_ops.exists = ops->exists;
}

static const WidccFileOps *widcc_file_ops(void) { return &file_ops; }

#endif // WIDCC_FILEOPS_C