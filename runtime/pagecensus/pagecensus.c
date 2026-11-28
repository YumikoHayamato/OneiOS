// pagecensus: what holds the phone's 2 MiB blocks (docs/native.md "The pool's fill, step by step").
// gh_hugepage_reserve takes a 2 MiB window only when every page in it is free or can be moved (its gate, gh_a_candidate_range:
// a page in use that is neither on the LRU nor movable refuses the whole window). This reads /proc/kpageflags (root) over
// the System RAM ranges of /proc/iomem and says, per kind of page that refuses, how many windows it holds and how much free
// memory lies inside them. Read only. vm-boot runs it after the pool's fill (--pagecensus FILE), into run.txt.
//   pagecensus [--kpageflags FILE --ranges START-END[,START-END...]]   (the second form is for the tests: pfns, hex)
#define _GNU_SOURCE
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define WIN 512u
enum { K_FREE, K_LRU, K_SLAB, K_PGTABLE, K_OTHER, K_RESERVED, K_HOLE, K_N };
#define BIT(n) (1ull << (n))
#define KPF_LRU BIT(5)
#define KPF_SLAB BIT(7)
#define KPF_BUDDY BIT(10)
#define KPF_MMAP BIT(11)
#define KPF_ANON BIT(12)
#define KPF_SWAPBACKED BIT(14)
#define KPF_HEAD BIT(15)
#define KPF_TAIL BIT(16)
#define KPF_NOPAGE BIT(20)
#define KPF_THP BIT(22)
#define KPF_PGTABLE BIT(26)
#define KPF_RESERVED BIT(32)

static int kind(uint64_t f)
{
  if (f & KPF_NOPAGE) return K_HOLE;
  if (f & KPF_RESERVED) return K_RESERVED;
  if (f & KPF_BUDDY) return K_FREE;
  if (f & KPF_SLAB) return K_SLAB;
  if (f & KPF_PGTABLE) return K_PGTABLE;
  if (f & (KPF_LRU | KPF_THP | KPF_MMAP | KPF_ANON | KPF_SWAPBACKED)) return K_LRU;
  return K_OTHER;
}

struct range { uint64_t first, end; };  // pfns, end not included

static int iomem_ranges(struct range *r, int most)
{
  FILE *f = fopen("/proc/iomem", "r");
  char line[256];
  int n = 0;
  if (!f) return 0;
  while (fgets(line, sizeof line, f) && n < most) {
    uint64_t a, b;
    if (line[0] == ' ' || !strstr(line, ": System RAM") || sscanf(line, "%" SCNx64 "-%" SCNx64, &a, &b) != 2 || !b) continue;
    r[n].first = a >> 12;
    r[n++].end = (b + 1) >> 12;
  }
  fclose(f);
  return n;
}

int main(int argc, char **argv)
{
  const char *file = "/proc/kpageflags";
  struct range r[128];
  int nr = 0;
  for (int i = 1; i + 1 < argc; i += 2) {
    if (!strcmp(argv[i], "--kpageflags")) file = argv[i + 1];
    else if (!strcmp(argv[i], "--ranges")) {
      char *s = argv[i + 1];
      while (*s && nr < 128) {
        char *e;
        r[nr].first = strtoull(s, &e, 16);
        if (*e != '-') break;
        r[nr++].end = strtoull(e + 1, &e, 16);
        s = *e == ',' ? e + 1 : e;
      }
    }
  }
  if (!nr) nr = iomem_ranges(r, 128);
  int fd = open(file, O_RDONLY);
  if (fd < 0 || !nr) {
    printf("pagecensus: %s\n", fd < 0 ? "cannot open the page flags (root, CONFIG_PROC_PAGE_MONITOR)" : "no System RAM range in /proc/iomem (root shows the addresses)");
    return 1;
  }
  // a window's verdict: whole (one 2 MiB allocation: the pool's pages look so), free (all free), movable (free and LRU
  // pages only: the module's gate lets it through), or refused by the kinds in `mask` (bits of K_SLAB.. K_HOLE)
  enum { MASKS = 1 << K_N };
  static uint64_t wins[MASKS], free_in[MASKS], blockers[MASKS];
  uint64_t whole = 0, all_free = 0, movable = 0, movable_lru = 0, total = 0, pages[K_N] = { 0 }, few[3] = { 0 }, few_free[3] = { 0 }, unread = 0;
  for (int i = 0; i < nr; i++) {
    for (uint64_t w = (r[i].first + WIN - 1) / WIN * WIN; w + WIN <= r[i].end; w += WIN) {
      uint64_t f[WIN];
      if (pread(fd, f, sizeof f, (off_t)(w * 8)) != (ssize_t)sizeof f) { unread++; continue; }
      unsigned n[K_N] = { 0 }, tails = 0;
      for (unsigned p = 0; p < WIN; p++) {
        n[kind(f[p])]++;
        if (p && (f[p] & KPF_TAIL) && !(f[p] & (KPF_THP | KPF_SLAB | KPF_LRU))) tails++;
      }
      total++;
      for (int k = 0; k < K_N; k++) pages[k] += n[k];
      if ((f[0] & KPF_HEAD) && !(f[0] & (KPF_THP | KPF_SLAB | KPF_LRU)) && tails == WIN - 1) { whole++; continue; }
      unsigned mask = 0, stuck = 0;
      for (int k = K_SLAB; k < K_N; k++)
        if (n[k]) { mask |= 1u << k; stuck += n[k]; }
      if (!mask) {
        if (n[K_FREE] == WIN) all_free++;
        else { movable++; movable_lru += n[K_LRU]; }
        continue;
      }
      wins[mask]++;
      free_in[mask] += n[K_FREE];
      blockers[mask] += stuck;
      int b = stuck <= 4 ? 0 : stuck <= 64 ? 1 : 2;
      few[b]++;
      few_free[b] += n[K_FREE];
    }
  }
  close(fd);
  static const char *const name[K_N] = { "free", "lru", "slab", "pagetable", "other-kernel", "reserved", "hole" };
#define MIB(p) ((unsigned long long)((p) >> 8))
  printf("pagecensus: %llu windows of 2 MiB in %d RAM ranges (%llu not read)\n", (unsigned long long)total, nr, (unsigned long long)unread);
  printf("pagecensus: pages MiB:");
  for (int k = 0; k < K_N; k++) printf(" %s %llu", name[k], MIB(pages[k]));
  printf("\npagecensus: whole 2 MiB allocations (the pool's pages and the like): %llu windows\n", (unsigned long long)whole);
  printf("pagecensus: takeable: %llu windows all free, %llu with only free and movable pages (%llu MiB to move); CMA blocks are in these counts and the module never takes them\n",
         (unsigned long long)all_free, (unsigned long long)movable, MIB(movable_lru));
  uint64_t refused = 0, refused_free = 0;
  for (unsigned m = 1; m < MASKS; m++) { refused += wins[m]; refused_free += free_in[m]; }
  printf("pagecensus: refused: %llu windows hold a page nothing moves, with %llu MiB free inside them\n", (unsigned long long)refused, MIB(refused_free));
  for (unsigned m = 1; m < MASKS; m++) {
    if (!wins[m]) continue;
    printf("pagecensus:   by");
    for (int k = K_SLAB; k < K_N; k++)
      if (m & (1u << k)) printf(" %s", name[k]);
    printf(": %llu windows, %llu MiB free inside, %llu KiB of such pages\n", (unsigned long long)wins[m], MIB(free_in[m]), (unsigned long long)(blockers[m] * 4));
  }
  printf("pagecensus: refused windows by how many pages refuse: 1-4 pages %llu windows (%llu MiB free inside), 5-64 %llu (%llu MiB), more %llu (%llu MiB)\n",
         (unsigned long long)few[0], MIB(few_free[0]), (unsigned long long)few[1], MIB(few_free[1]), (unsigned long long)few[2], MIB(few_free[2]));
  printf("pagecensus: other-kernel = in use with no LRU, slab or page-table flag: vmalloc and kernel stacks, drivers' and GPU pages, zram's, pages free on a CPU's own list\n");
  return 0;
}
