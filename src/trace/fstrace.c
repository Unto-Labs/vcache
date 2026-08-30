/* SPDX-FileCopyrightText: 2026 Unto Labs
 * SPDX-License-Identifier: Apache-2.0
 *
 * LD_PRELOAD file-access tracer for link steps.
 *
 * A link has no -E equivalent, so the only way to learn its true input set is
 * to watch it. This records both halves of the dependency question:
 *
 *   R  a path that was opened or stat'd successfully -- its CONTENT is an input
 *   M  a path probed and not found (ENOENT)         -- its ABSENCE is an input
 *   P  an executable that ran                       -- part of the toolchain
 *   D  a directory that was listed
 *
 * The second is the half a "record what was read" manifest cannot express, and
 * it is what makes a later hit unsound: -lfoo resolves to /usr/lib/libfoo.so
 * only because /opt/lib/libfoo.so was absent, so a manifest that records only
 * what it read will happily hit after someone installs the latter.
 *
 * Processes announce themselves from a constructor rather than by interposing
 * exec*. glibc's execvp calls execve internally without crossing the PLT, so
 * exec interposition sees only some of the tree: measured on one LTO link, it
 * caught 3 of the 11 distinct executables, while the constructor caught all 11.
 *
 * Writes go out through raw syscalls so tracing never re-enters our own hooks.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

static int log_fd = -1;

static void ensure_log(void) {
  if (log_fd != -1) return;
  const char *p = getenv("VCACHE_TRACE_LOG");
  if (p == NULL || *p == '\0') { log_fd = -2; return; }
  log_fd = (int)syscall(SYS_openat, AT_FDCWD, p,
                        O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
  if (log_fd < 0) log_fd = -2;
}

/* One write per record. O_APPEND makes concurrent writes from the process tree
   atomic up to PIPE_BUF, which every path we emit is comfortably below. */
static void emit(char kind, const char *path) {
  if (path == NULL || *path == '\0') return;
  ensure_log();
  if (log_fd < 0) return;
  char buf[4352];
  size_t n = 0;
  buf[n++] = kind;
  buf[n++] = '\t';
  for (const char *q = path; *q != '\0' && n < sizeof buf - 2; ++q) {
    buf[n++] = (*q == '\n' || *q == '\t') ? ' ' : *q;
  }
  buf[n++] = '\n';
  (void)!syscall(SYS_write, log_fd, buf, n);
}

/* Only ENOENT counts as a negative input. A permission error is a different
   question, and treating it as "absent" would invalidate entries for reasons
   that have nothing to do with the link. */
static void emit_result(const char *path, int failed) {
  if (!failed) { emit('R', path); return; }
  if (errno == ENOENT) emit('M', path);
}

#define NEXT(sym)                                     \
  ({                                                  \
    static __typeof__(&sym) next_##sym;                   \
    if (next_##sym == NULL)                           \
      next_##sym = (__typeof__(&sym))dlsym(RTLD_NEXT, #sym); \
    next_##sym;                                       \
  })

__attribute__((constructor)) static void announce_self(void) {
  char exe[4096];
  long n = syscall(SYS_readlinkat, AT_FDCWD, "/proc/self/exe", exe, sizeof exe - 1);
  if (n > 0) { exe[n] = '\0'; emit('P', exe); }
}

int open(const char *path, int flags, ...) {
  mode_t m = 0;
  if (flags & O_CREAT) { va_list a; va_start(a, flags); m = (mode_t)va_arg(a, int); va_end(a); }
  int r = NEXT(open)(path, flags, m);
  emit_result(path, r < 0);
  return r;
}

int open64(const char *path, int flags, ...) {
  mode_t m = 0;
  if (flags & O_CREAT) { va_list a; va_start(a, flags); m = (mode_t)va_arg(a, int); va_end(a); }
  int r = NEXT(open64)(path, flags, m);
  emit_result(path, r < 0);
  return r;
}

int openat(int dirfd, const char *path, int flags, ...) {
  mode_t m = 0;
  if (flags & O_CREAT) { va_list a; va_start(a, flags); m = (mode_t)va_arg(a, int); va_end(a); }
  int r = NEXT(openat)(dirfd, path, flags, m);
  emit_result(path, r < 0);
  return r;
}

FILE *fopen(const char *path, const char *mode) {
  FILE *r = NEXT(fopen)(path, mode);
  emit_result(path, r == NULL);
  return r;
}

int stat(const char *path, struct stat *st) {
  int r = NEXT(stat)(path, st);
  emit_result(path, r != 0);
  return r;
}

int lstat(const char *path, struct stat *st) {
  int r = NEXT(lstat)(path, st);
  emit_result(path, r != 0);
  return r;
}

int access(const char *path, int mode) {
  int r = NEXT(access)(path, mode);
  emit_result(path, r != 0);
  return r;
}

DIR *opendir(const char *path) {
  DIR *r = NEXT(opendir)(path);
  if (r != NULL) emit('D', path); else emit_result(path, 1);
  return r;
}

ssize_t readlink(const char *path, char *buf, size_t n) {
  ssize_t r = NEXT(readlink)(path, buf, n);
  emit_result(path, r < 0);
  return r;
}
