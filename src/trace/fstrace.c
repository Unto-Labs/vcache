/* SPDX-FileCopyrightText: 2026 Unto Labs
 * SPDX-License-Identifier: Apache-2.0
 *
 * LD_PRELOAD file-access tracer for link steps, and for the C/C++ compiles a
 * manifest state is recorded from.
 *
 * A link has no -E equivalent, so the only way to learn its true input set is
 * to watch it. A compile has a depfile, which names what it read but not what
 * the preprocessor looked for and did not find. This records both halves of
 * the dependency question:
 *
 *   R  a path that was opened or stat'd successfully -- its CONTENT is an input
 *   M  a path probed and not found (ENOENT)         -- its ABSENCE is an input
 *   P  an executable that ran                       -- part of the toolchain
 *   L  a shared object a process that ran loaded    -- part of the toolchain
 *   W  a path opened for writing only               -- an output; a C/C++
 *                                                       manifest drops it, the
 *                                                       link cache keeps it as
 *                                                       an input all the same
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
#include <limits.h>
#include <link.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

static int log_fd = -1;

/* Records must fit in one write. A path is bounded by PATH_MAX, but the tracer
   also sees strings that merely occupy a path argument, so the bound is
   enforced rather than assumed. */
#define VCACHE_RECORD_MAX 4096

/* VCACHE_TRACE_LOG, or NULL when tracing is off. A relative name counts as
   unset: the log and its marker would land in whatever directory each process
   of the tree is in when it first records. */
static const char *log_path(void) {
  const char *p = getenv("VCACHE_TRACE_LOG");
  return p != NULL && p[0] == '/' ? p : NULL;
}

static void ensure_log(void) {
  if (log_fd != -1) return;
  const char *p = log_path();
  if (p == NULL) { log_fd = -2; return; }
  log_fd = (int)syscall(SYS_openat, AT_FDCWD, p,
                        O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
  if (log_fd < 0) log_fd = -2;
}

/* A "!" line, which readers refuse the whole log on. */
static void write_incomplete_sentinel(void) {
  ensure_log();
  if (log_fd < 0) return;
  for (;;) {
    long r = syscall(SYS_write, log_fd, "!\n", 2);
    if (r >= 0 || errno != EINTR) return;
  }
}

/* A lost record is not a lost log line: it is an input missing from the set
   that decides whether a later hit is sound. So any failure to record is made
   loud by creating "<log>.incomplete", which the caller checks before storing
   an entry. A marker whose name does not fit, or that its directory refuses,
   would leave the log looking whole -- possibly with other processes' P
   records in it -- so the sentinel in the log is its backstop. */
static void mark_incomplete(void) {
  const char *log = log_path();
  if (log == NULL) return;
  static const char suffix[] = ".incomplete";
  char marker[PATH_MAX];
  size_t n = strlen(log);
  if (n + sizeof suffix <= sizeof marker) {
    memcpy(marker, log, n);
    memcpy(marker + n, suffix, sizeof suffix);
    int fd = (int)syscall(SYS_openat, AT_FDCWD, marker,
                          O_WRONLY | O_CREAT | O_CLOEXEC, 0600);
    if (fd >= 0) { syscall(SYS_close, fd); return; }
  }
  write_incomplete_sentinel();
}

static void emit(char kind, const char *path) {
  if (path == NULL || *path == '\0') return;
  ensure_log();
  if (log_fd < 0) return;

  char buf[VCACHE_RECORD_MAX];
  size_t n = 0;
  buf[n++] = kind;
  buf[n++] = '\t';
  const char *q = path;
  for (; *q != '\0' && n < sizeof buf - 1; ++q) {
    if (*q == '\n' || *q == '\t') {
      /* The line protocol cannot represent these bytes. Substitution would
         name a different file and make later validation unsound. */
      mark_incomplete();
      return;
    }
    buf[n++] = *q;
  }
  if (*q != '\0') {
    /* Truncating would record a path that is not the one that was touched,
       and validating it later would check the wrong file. Drop the record and
       say so instead. */
    mark_incomplete();
    return;
  }
  buf[n++] = '\n';

  /* One record is one write. The whole process tree appends to one regular
     file, where O_APPEND makes each write land whole and contiguous; a second
     write for the rest of a short one can land after another process's record
     and tear both. So a short write is a lost record, and is made loud. */
  for (;;) {
    long r = syscall(SYS_write, log_fd, buf, n);
    if (r == (long)n) return;
    if (r < 0 && errno == EINTR) continue;
    mark_incomplete();
    return;
  }
}

/* Only ENOENT counts as a negative input. A permission error is a different
   question, and treating it as "absent" would invalidate entries for reasons
   that have nothing to do with the link. */
static void emit_result(const char *path, int failed, int saved_errno) {
  if (!failed) { emit('R', path); return; }
  if (saved_errno == ENOENT) emit('M', path);
}

/* An open that cannot read says nothing about the file's content as an input.
   Read-write opens stay R: the process may read what it finds. */
static void emit_open_result(const char *path, int failed, int saved_errno,
                             int written_only) {
  if (!failed && written_only) { emit('W', path); return; }
  emit_result(path, failed, saved_errno);
}

static int fopen_mode_written_only(const char *mode) {
  return mode != NULL && (mode[0] == 'w' || mode[0] == 'a') && strchr(mode, '+') == NULL;
}

/* Resolve a relative openat path against its directory descriptor. Treating it
   as cwd-relative records a different file and can omit the real input. */
static int resolve_at_path(int dirfd, const char *path, char *out, size_t size) {
  if (path == NULL || *path == '\0') return 0;
  if (*path == '/' || dirfd == AT_FDCWD) {
    size_t n = strlen(path);
    if (n >= size) return 0;
    memcpy(out, path, n + 1);
    return 1;
  }
  char proc_path[64];
  int pn = snprintf(proc_path, sizeof proc_path, "/proc/self/fd/%d", dirfd);
  if (pn <= 0 || (size_t)pn >= sizeof proc_path) return 0;
  long n = syscall(SYS_readlinkat, AT_FDCWD, proc_path, out, size - 1);
  if (n <= 0 || (size_t)n >= size - 1) return 0;
  size_t used = (size_t)n;
  if (out[used - 1] != '/') out[used++] = '/';
  size_t rest = strlen(path);
  if (used + rest >= size) return 0;
  memcpy(out + used, path, rest + 1);
  return 1;
}

static void emit_at_result(int dirfd, const char *path, int failed,
                           int saved_errno, int written_only) {
  if (failed && saved_errno != ENOENT) return;
  char resolved[VCACHE_RECORD_MAX];
  if (resolve_at_path(dirfd, path, resolved, sizeof resolved)) {
    emit_open_result(resolved, failed, saved_errno, written_only);
  } else {
    mark_incomplete();
  }
}

#define NEXT(sym)                                     \
  ({                                                  \
    static __typeof__(&sym) next_##sym;                   \
    if (next_##sym == NULL)                           \
      next_##sym = (__typeof__(&sym))dlsym(RTLD_NEXT, #sym); \
    next_##sym;                                       \
  })

/* Our own load address, so we can leave ourselves out of the input set.
   Compared by base rather than by name: dladdr and dl_iterate_phdr can spell
   the same object differently, and a name comparison would quietly stop
   matching. */
static ElfW(Addr) self_base(void) {
  static ElfW(Addr) base;
  static int resolved;
  if (!resolved) {
    resolved = 1;
    Dl_info info;
    if (dladdr((void *)(uintptr_t)&self_base, &info) != 0) {
      base = (ElfW(Addr))(uintptr_t)info.dli_fbase;
    }
  }
  return base;
}

static int announce_object(struct dl_phdr_info *info, size_t size, void *data) {
  (void)size;
  (void)data;
  if (info->dlpi_name == NULL || info->dlpi_name[0] != '/') return 0;

  /* The tracer observes the link; it does not take part in it. Recording
     ourselves would make every rebuild of vcache invalidate every link entry,
     for a file that cannot affect the output. */
  if (info->dlpi_addr == self_base()) return 0;

  /* Dependencies are loaded before interposition is active. Include every
     other already-loaded DSO: for the common linkers the implementation is a
     shared library -- ld.bfd's logic lives in libbfd, lld's in libLLVM -- and
     libz/libzstd emit output bytes directly for compressed debug sections.
     Its own kind, so a reader that keys the toolchain another way can tell
     these apart from the files the process opened. */
  emit('L', info->dlpi_name);
  return 0;
}

__attribute__((constructor)) static void announce_self(void) {
  char exe[4096];
  long n = syscall(SYS_readlinkat, AT_FDCWD, "/proc/self/exe", exe, sizeof exe - 1);
  if (n > 0) { exe[n] = '\0'; emit('P', exe); }
  dl_iterate_phdr(announce_object, NULL);
}

int open(const char *path, int flags, ...) {
  mode_t m = 0;
  if (flags & O_CREAT) { va_list a; va_start(a, flags); m = (mode_t)va_arg(a, int); va_end(a); }
  int r = NEXT(open)(path, flags, m);
  int saved = errno;
  emit_open_result(path, r < 0, saved, (flags & O_ACCMODE) == O_WRONLY);
  errno = saved;
  return r;
}

int open64(const char *path, int flags, ...) {
  mode_t m = 0;
  if (flags & O_CREAT) { va_list a; va_start(a, flags); m = (mode_t)va_arg(a, int); va_end(a); }
  int r = NEXT(open64)(path, flags, m);
  int saved = errno;
  emit_open_result(path, r < 0, saved, (flags & O_ACCMODE) == O_WRONLY);
  errno = saved;
  return r;
}

int openat(int dirfd, const char *path, int flags, ...) {
  mode_t m = 0;
  if (flags & O_CREAT) { va_list a; va_start(a, flags); m = (mode_t)va_arg(a, int); va_end(a); }
  int r = NEXT(openat)(dirfd, path, flags, m);
  int saved = errno;
  emit_at_result(dirfd, path, r < 0, saved, (flags & O_ACCMODE) == O_WRONLY);
  errno = saved;
  return r;
}

int openat64(int dirfd, const char *path, int flags, ...) {
  mode_t m = 0;
  if (flags & O_CREAT) { va_list a; va_start(a, flags); m = (mode_t)va_arg(a, int); va_end(a); }
  int r = NEXT(openat64)(dirfd, path, flags, m);
  int saved = errno;
  emit_at_result(dirfd, path, r < 0, saved, (flags & O_ACCMODE) == O_WRONLY);
  errno = saved;
  return r;
}

FILE *fopen(const char *path, const char *mode) {
  FILE *r = NEXT(fopen)(path, mode);
  int saved = errno;
  emit_open_result(path, r == NULL, saved, fopen_mode_written_only(mode));
  errno = saved;
  return r;
}

FILE *fopen64(const char *path, const char *mode) {
  FILE *r = NEXT(fopen64)(path, mode);
  int saved = errno;
  emit_open_result(path, r == NULL, saved, fopen_mode_written_only(mode));
  errno = saved;
  return r;
}

int stat(const char *path, struct stat *st) {
  int r = NEXT(stat)(path, st);
  int saved = errno;
  emit_result(path, r != 0, saved);
  errno = saved;
  return r;
}

int lstat(const char *path, struct stat *st) {
  int r = NEXT(lstat)(path, st);
  int saved = errno;
  emit_result(path, r != 0, saved);
  errno = saved;
  return r;
}

int access(const char *path, int mode) {
  int r = NEXT(access)(path, mode);
  int saved = errno;
  emit_result(path, r != 0, saved);
  errno = saved;
  return r;
}

int faccessat(int dirfd, const char *path, int mode, int flags) {
  int r = NEXT(faccessat)(dirfd, path, mode, flags);
  int saved = errno;
  emit_at_result(dirfd, path, r != 0, saved, 0);
  errno = saved;
  return r;
}

int fstatat(int dirfd, const char *path, struct stat *st, int flags) {
  int r = NEXT(fstatat)(dirfd, path, st, flags);
  int saved = errno;
  emit_at_result(dirfd, path, r != 0, saved, 0);
  errno = saved;
  return r;
}

DIR *opendir(const char *path) {
  DIR *r = NEXT(opendir)(path);
  int saved = errno;
  if (r != NULL) emit('D', path); else emit_result(path, 1, saved);
  errno = saved;
  return r;
}

ssize_t readlink(const char *path, char *buf, size_t n) {
  ssize_t r = NEXT(readlink)(path, buf, n);
  int saved = errno;
  emit_result(path, r < 0, saved);
  errno = saved;
  return r;
}
