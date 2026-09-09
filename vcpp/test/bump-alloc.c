/* SPDX-License-Identifier: GPL-3.0-or-later

   A linear allocator with a no-op free, preloaded over glibc's, to answer
   "would a faster allocator help?" with a measurement instead of a profile.

   It would not. The answer is kept here because it is not the obvious one.

     gcc -O2 -fPIC -shared -o bump.so test/bump-alloc.c
     LD_PRELOAD=./bump.so ./bin/vcpp ... file.c -o /dev/null

   On git 2.55.0's http.c, against glibc's allocator:

     glibc   1.28 G instructions   5463 faults    23 MB peak RSS
     bump    1.88 G instructions   1397 faults   573 MB peak RSS

   Slower, by a lot, and the RSS says why: libcpp's peak live set is 23 MB but
   it allocates 573 MB over the run. It churns -- macro expansion buffers are
   allocated and released constantly -- so glibc spends its time recycling 23 MB
   while this spends its time faulting in and zeroing 573 MB of fresh pages.
   Kernel time went from ~4% to 18.7%. Huge pages cut the fault *count* fourfold
   and did not save it, because the cost is zeroing, not faulting.

   Two other things had to be fixed before it was even this close, both worth
   knowing if anyone tries again:

   - The naive version is 4x slower than glibc, not 1.5x, because libcpp grows
     buffers with realloc, glibc extends the last block in place, and a bump
     allocator that always copies makes repeated growth O(n^2). Hence the
     capacity field, the in-place extend for the most recent block, and the
     geometric growth below.
   - calloc must not memset arena memory. It comes from a fresh anonymous
     mmap and is already zero; re-zeroing it cost another 1.0 G instructions.

   The real conclusion is that allocation was never the bottleneck: it is about
   6% of the profile, because libcpp already pools through its own _cpp_buff
   rather than going to malloc for every token.  */
#define _GNU_SOURCE
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>

#define ARENA (4ull << 30)
#define HDR   16
static char *base, *cur, *end;
static char boot[1 << 20];
static size_t boot_used;

struct hdr { size_t size, cap; };

static void arena_init (void)
{
  base = mmap (NULL, ARENA, PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  if (base == MAP_FAILED) { base = cur = end = 0; return; }
#ifdef MADV_HUGEPAGE
  madvise (base, ARENA, MADV_HUGEPAGE);
#endif
  cur = base; end = base + ARENA;
}

static void *bump_cap (size_t n, size_t cap)
{
  size_t need = (cap + sizeof (struct hdr) + 15) & ~(size_t) 15;
  if (!base) arena_init ();
  char *p;
  if (base && cur + need <= end) { p = cur; cur += need; }
  else if (boot_used + need <= sizeof boot) { p = boot + boot_used; boot_used += need; }
  else return 0;
  ((struct hdr *) p)->size = n;
  ((struct hdr *) p)->cap  = need - sizeof (struct hdr);
  return p + sizeof (struct hdr);
}
static void *bump (size_t n) { return bump_cap (n, n); }

void *malloc (size_t n) { return bump (n); }
void free (void *p) { (void) p; }
void *calloc (size_t a, size_t b)
{
  // Arena bytes are never handed out twice and come straight from an anonymous
  // mmap, so they are already zero -- memset here would be re-zeroing fresh
  // pages, which is exactly what glibc's calloc is careful not to do. Only the
  // bootstrap buffer needs clearing.
  size_t n = a * b;
  char *p = bump (n);
  if (p && (p < base || p >= end)) memset (p, 0, n);
  return p;
}
void *realloc (void *p, size_t n)
{
  if (!p) return bump (n);
  struct hdr *h = (struct hdr *) ((char *) p - sizeof (struct hdr));
  if (n <= h->cap) { h->size = n; return p; }

  // The block is the arena's most recent allocation: just move the frontier.
  char *blk_end = (char *) p + h->cap;
  if (base && blk_end == cur)
    {
      size_t want = (n + 15) & ~(size_t) 15;
      if (cur + (want - h->cap) <= end)
        { cur += want - h->cap; h->cap = want; h->size = n; return p; }
    }

  size_t grow = h->cap * 2 > n ? h->cap * 2 : n;
  void *q = bump_cap (n, grow);
  if (q) memcpy (q, p, h->size);
  return q;
}
int posix_memalign (void **r, size_t a, size_t n)
{ (void) a; *r = bump (n); return *r ? 0 : 12; }
void *memalign (size_t a, size_t n) { (void) a; return bump (n); }
void *aligned_alloc (size_t a, size_t n) { (void) a; return bump (n); }
char *strdup (const char *s)
{ size_t n = strlen (s) + 1; char *p = bump (n); if (p) memcpy (p, s, n); return p; }
