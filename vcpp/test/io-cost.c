/* SPDX-License-Identifier: GPL-3.0-or-later

   What the file-system costs in a vcpp run are actually worth, measured
   directly rather than through strace's inflation (which reports them ~4x too
   expensive because every syscall takes a pair of stops).

     gcc -O2 -o io-cost test/io-cost.c && ./io-cost some/big/file.c

   Run it twice: the first run pays for cold negative dentries, the second is
   what a repeat build actually sees, and that is the number that matters here.

   A vcpp run on git's http.c does 430 read()s totalling 3.77 MB, and 1417
   openat()s of which 988 fail -- the include-path search trying each directory
   in turn. Against a ~100 ms run, warm:

     988 failed open()    1.45 ms   1.4%
     3.77 MB via read()   0.55 ms   0.5%
     3.77 MB via mmap()   0.85 ms   0.8%

   So mapping the input instead of reading it would be *slower*, by about
   0.3 ms. Faulting in 920 pages costs more than one memcpy out of a warm page
   cache, which is the same way round as the arena allocator in bump-alloc.c:
   page faults beat copying only when the copy is large and cold.

   libcpp could not take a mapping as-is anyway. read_file_guts allocates
   st_size + 16 because the SSE4 lexer reads aligned 16-byte chunks past the
   end of the content and stops on a '\n' sentinel that libcpp writes into the
   buffer; _cpp_clean_line then rewrites the buffer in place for backslash-
   newline splicing. A file mapping would need a writable private mapping plus
   a guaranteed writable tail page -- and would SIGSEGV on a file whose size is
   an exact multiple of the page size. gcc removed mmap from libcpp years ago;
   there is no trace of it left in the 13.3.0 sources.

   The failed opens are the larger of the two, and still only 1.4%. Removing
   them means indexing the include directories rather than probing them, which
   is a semantic change (a directory listing is a snapshot) for about a
   percent.  */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/stat.h>

static double now (void)
{ struct timespec t; clock_gettime (CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }

int main (int argc, char **argv)
{
  const char *big = argv[1];
  int N = 988;
  double t0 = now ();
  for (int i = 0; i < N; i++)
    { char p[256]; sprintf (p, "/usr/include/no_such_header_%d.h", i);
      int fd = open (p, O_RDONLY); if (fd >= 0) close (fd); }
  double t_open = now () - t0;

  struct stat st; stat (big, &st);
  // 3.77 MB is what a vcpp run on http.c reads; loop to reach it.
  int reps = (int) (3.77 * 1048576 / (st.st_size ? st.st_size : 1)) + 1;
  char *buf = malloc (st.st_size + 16);

  t0 = now ();
  for (int r = 0; r < reps; r++)
    { int fd = open (big, O_RDONLY); ssize_t n, tot = 0;
      while ((n = read (fd, buf + tot, st.st_size - tot)) > 0) tot += n;
      close (fd); }
  double t_read = now () - t0;

  t0 = now ();
  volatile char sink = 0;
  for (int r = 0; r < reps; r++)
    { int fd = open (big, O_RDONLY);
      char *m = mmap (NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
      for (off_t o = 0; o < st.st_size; o += 4096) sink ^= m[o];
      munmap (m, st.st_size); close (fd); }
  double t_mmap = now () - t0;

  printf ("988 failed open()   : %6.2f ms\n", t_open * 1e3);
  printf ("3.77 MB via read()  : %6.2f ms\n", t_read * 1e3);
  printf ("3.77 MB via mmap()  : %6.2f ms  (touching every page)\n", t_mmap * 1e3);
  return 0;
}
