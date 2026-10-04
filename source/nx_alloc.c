/* nx_alloc.c -- the loader's ALLOCATOR and heap-debug machinery.
 *
 * (Renamed from imports_pvz_extra.c. The old name was actively misleading: the
 * import table was ~10 of its 483 lines, and everything else is memory
 * infrastructure that has nothing to do with imports. Deleting this file on the
 * assumption that it was "just PvZ's import table" is what produced several
 * hundred `undefined reference to __wrap_malloc` link errors.)
 *
 * WHAT LIVES HERE, and why it must not be dropped:
 *   __wrap_malloc / memalign / calloc / realloc / memmove / memcpy / free
 *       The Makefile passes -Wl,--wrap for all seven. The linker rewrites every
 *       reference -- including newlib's and libsysbase's own internal ones --
 *       to __wrap_X, so these MUST exist or nothing links.
 *   gpua_*      the dedicated GPU arena (PORTING sec 4 / PvZ sec 23-27)
 *   can_*       heap canaries + the sweeper thread
 *   quar_*      free quarantine (use-after-free detection)
 *
 * None of it is game-specific. Nothing outside this file references those
 * helpers directly -- they hook in purely through the --wrap indirection, which
 * is exactly why their loss showed up at link time rather than compile time.
 *
 * Original header follows.
 * ------------------------------------------------------------------------
 * The handful of imports PvZ Fusion 3.6.1 needed that the
 * Zookeeper base tables (imports.c + unity_imports.c + firebase) do NOT already
 * provide.
 *
 * The base wrapper already resolves ~600 symbols (all the libc/libm/EGL/NDK the
 * Unity engine normally uses). A symbol-table diff of THIS game's libunity.so +
 * libil2cpp.so against all three base tables leaves exactly FIVE unresolved
 * imports -- listed below. They differ from Zookeeper only because PvZ Fusion's
 * C# uses hyperbolic math and reads the process environment.
 *
 * All five are standard and provided by devkitA64 newlib, so we just point the
 * resolver at them. If the LINKER ever reports "undefined reference" to one,
 * replace that row with a tiny local impl (e.g. cosh -> (exp(x)+exp(-x))/2).
 *
 * Wire-up (in imports.c build_combined()):
 *   (that wire-up now lives in imports_fruitninja_extra.c, as
 *    ndk_extra_functions / ndk_extra_numfunctions)
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stddef.h>
#include <string.h>      /* memset (canary calloc) */
#include <malloc.h>      /* malloc_usable_size (free quarantine) */
#include <stdio.h>       /* snprintf (quarantine report) */
#include <math.h>
#include <switch.h>      /* Mutex/Thread/svcSleepThread (canary sweeper) */
#include "config.h"      /* ABR_POISON_FREE (round 135) */
#include "so_util.h"
#include "util.h"        /* debugPrintf */

extern char **environ;   /* newlib provides the real definition */

/* The 5-row resolver table that used to live here is gone: this port supplies
 * cosh/sinh/tanh/environ (and 70 more) from imports_fruitninja_extra.c.
 * nearbyintf is NOT carried over -- 2022.3.0f1's libunity does not import it,
 * where 2022.3.62's did. Everything BELOW this line is kept: it is the
 * allocator, the GPU arena and the heap-debug machinery, not import glue. */

/* ---- heap-corruption guard (save-load asset processing) --------------------
 * A crash on UnityMain inside _free_r showed free() handed 0x3d636c1c3d1fcf53 --
 * packed float RGBA data, not a heap pointer. An object's pointer field had been
 * overwritten by asset/texture float data (a buffer overrun during save load),
 * and freeing that garbage faulted. We can't yet localize the overrun, but we can
 * keep it from being fatal: with -Wl,--wrap,free in the Makefile, EVERY free()
 * call routes here first -- libunity/il2cpp frees (via the &free import table
 * entry, which --wrap redirects to __wrap_free) AND mesa's own internal frees.
 * A real malloc() result is 8-byte aligned and inside the 39-bit user address
 * space; a smashed pointer is neither, so we skip it (leaking the block) and log
 * instead of crashing in _free_r. Fast integer checks only -- no syscall on the
 * hot path, which matters because asset load frees heavily. If a subtler
 * corruption (aligned + in-range but non-heap) ever slips through, add an
 * svcQueryMemory readability tier here. */
extern void __real_free(void *p);

/* ---- heap overrun hunter (canaries) ----------------------------------------
 * The free-guard above proved a heap buffer overrun exists (pointer fields
 * smashed with float RGBA during asset processing); it has now graduated to
 * smashing mesa's GL buffer bookkeeping -> the recurring glBufferData faults
 * at round/garbage destinations. Catch the overrunner: every allocation gets
 * an 8-byte tail canary (size+8 alloc, XOR-keyed to the pointer) tracked in a
 * lossy side table; a sweeper thread checks live canaries every 2s and
 * __wrap_free checks at free. On a smash we log the block size and the first
 * 32 bytes written PAST the block -- the content names the writer.
 * The side table is alignment-neutral (no headers), so memalign semantics are
 * preserved; collisions just evict tracking (lossy is fine for hunting). */
extern void *__real_malloc(size_t sz);
extern void *__real_memalign(size_t al, size_t sz);
extern void *__real_calloc(size_t n, size_t s);
extern void *__real_realloc(void *p, size_t sz);

#define CAN_TAB_BITS 16
#define CAN_TAB_N    (1u << CAN_TAB_BITS)
#define CAN_KEY      0xC0DEC0DE5EED5EEDULL
typedef struct { uintptr_t p; size_t sz; } CanEnt;
static CanEnt g_can[CAN_TAB_N];
static Mutex  g_can_lk;
static int    g_can_sweeper = 0;
static volatile int g_can_stop = 0;
static int    g_can_thread_live = 0;
static Thread g_can_thread;
static unsigned g_can_evict = 0, g_can_live = 0;   /* coverage, see can_track */

static inline unsigned can_slot(uintptr_t a) {
  return (unsigned)((a >> 4) * 0x9E3779B97F4A7C15ULL >> (64 - CAN_TAB_BITS));
}
static inline void can_write(void *p, size_t sz) {
  *(uint64_t *)((uint8_t *)p + sz) = CAN_KEY ^ (uintptr_t)p;
}
static inline int can_ok(uintptr_t p, size_t sz) {
  return *(volatile uint64_t *)(p + sz) == (CAN_KEY ^ p);
}
static void can_report(uintptr_t p, size_t sz, const char *when) {
  const uint8_t *o = (const uint8_t *)(p + sz);
  debugPrintf("[canary] SMASH %s: block %p size %u; overrun bytes: "
              "%02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x "
              "%02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x\n",
              when, (void *)p, (unsigned)sz,
              o[0],o[1],o[2],o[3], o[4],o[5],o[6],o[7],
              o[8],o[9],o[10],o[11], o[12],o[13],o[14],o[15],
              o[16],o[17],o[18],o[19], o[20],o[21],o[22],o[23],
              o[24],o[25],o[26],o[27], o[28],o[29],o[30],o[31]);
  /* floats too -- the historic smash was float RGBA */
  const float *f = (const float *)(p + sz);
  debugPrintf("[canary]   as floats: %g %g %g %g\n", f[0], f[1], f[2], f[3]);
}
static void can_sweeper_thread(void *arg) {
  (void)arg;
  while (!g_can_stop) {
    svcSleepThread(2000000000ULL);   /* 2s */
    if (g_can_stop) break;
    mutexLock(&g_can_lk);
    for (unsigned i = 0; i < CAN_TAB_N; i++) {
      if (g_can[i].p && !can_ok(g_can[i].p, g_can[i].sz)) {
        can_report(g_can[i].p, g_can[i].sz, "LIVE-SWEEP");
        g_can[i].p = 0;              /* report once */
      }
    }
    mutexUnlock(&g_can_lk);
  }
}
static void can_track(void *p, size_t sz) {
  if (!p) return;
  can_write(p, sz);
  mutexLock(&g_can_lk);
  if (!g_can_sweeper) {
    g_can_sweeper = 1;
    g_can_stop = 0;
    Result _canrc = threadCreate(&g_can_thread, can_sweeper_thread, NULL, NULL, 0x4000, 0x3B, -2);
    if (R_SUCCEEDED(_canrc) && R_SUCCEEDED(threadStart(&g_can_thread)))
      g_can_thread_live = 1;
  }
  unsigned s = can_slot((uintptr_t)p);
  /* Round 147: measure the loss. "No SMASH in the log" has been quoted as
   * evidence that nothing overruns a block, but the table is 64Ki slots,
   * direct-indexed and silently overwritten on collision, against ~23k
   * allocations per heartbeat. Without these counters there is no way to tell
   * "nothing overran" from "we were not watching" -- the same silent-limit
   * trap as the JNI pools in r146. */
  if (g_can[s].p && g_can[s].p != (uintptr_t)p) g_can_evict++;
  else if (!g_can[s].p)                         g_can_live++;
  g_can[s].p = (uintptr_t)p; g_can[s].sz = sz;   /* lossy: eviction ok */
  mutexUnlock(&g_can_lk);
}
/* SZ_R313_CANARY_STOP */
void nx_canary_sweeper_stop(void) {
  if (!g_can_sweeper) return;
  g_can_stop = 1;
  if (g_can_thread_live) {
    threadWaitForExit(&g_can_thread);
    threadClose(&g_can_thread);
    g_can_thread_live = 0;
  }
  g_can_sweeper = 0;
  debugPrintf("[quitthr] canary sweeper stopped\n");
}

/* Coverage of the tail-canary detector. Reported on the heartbeat so that
 * "no SMASH" can be read as evidence rather than hope. */
void nx_canary_stats(unsigned *live, unsigned *evicted, unsigned *slots) {
  if (live)    *live    = g_can_live;
  if (evicted) *evicted = g_can_evict;
  if (slots)   *slots   = CAN_TAB_N;
}

static void can_untrack_check(uintptr_t a) {
  mutexLock(&g_can_lk);
  unsigned s = can_slot(a);
  if (g_can[s].p == a) {
    if (!can_ok(a, g_can[s].sz)) can_report(a, g_can[s].sz, "AT-FREE");
    g_can[s].p = 0;
    if (g_can_live) g_can_live--;
  }
  mutexUnlock(&g_can_lk);
}

/* ---- free quarantine (use-after-free hunter/neutralizer) --------------------
 * Canaries stayed silent while pointer slots keep getting hit with packed float
 * data (this run: dst hi32 = a float bit pattern; regs full of packed float
 * pairs; historic smash was float RGBA). Tail canaries can't see writes INSIDE
 * a block -- the signature of a STALE POINTER writing into memory that was
 * freed and reallocated (to mesa's GL bookkeeping, lately). Quarantine frees:
 * hold recently freed blocks (poisoned, unreusable) in a ring; verify the
 * poison when a block finally leaves quarantine and log any write-after-free
 * with offset + content. While armed, the stale writer scribbles into
 * quarantined memory instead of someone's live allocation -- so this is a
 * mitigation as well as a detector. Bounded: <=1MB blocks, <=4096 entries,
 * <=64MB held. */
#define QUAR_N        4096
#define QUAR_MAX_SZ   (1u << 20)
#define QUAR_CAP      (64u << 20)
#define QUAR_POISON   0xDE
typedef struct { void *p; uint32_t sz; } QuarEnt;
static QuarEnt g_quar[QUAR_N];
static unsigned g_quar_head, g_quar_tail;   /* ring: head=oldest, tail=next-in */
static size_t   g_quar_bytes;
__attribute__((unused)) static void quar_check_free(QuarEnt e) {   /* no locks held */
  const uint8_t *b = (const uint8_t *)e.p;
  for (uint32_t i = 0; i < e.sz; i++) {
    if (b[i] != QUAR_POISON) {
      uint32_t end = i + 32 > e.sz ? e.sz : i + 32;
      char line[3 * 36 + 4]; int n = 0;
      for (uint32_t k = i; k < end && n < (int)sizeof(line) - 4; k++)
        n += snprintf(line + n, sizeof(line) - n, " %02x", b[k]);
      debugPrintf("[quar] WRITE-AFTER-FREE: block %p size %u, first hit at +0x%x:%s\n",
                  e.p, e.sz, i, line);
      const float *f = (const float *)(b + (i & ~3u));
      debugPrintf("[quar]   as floats: %g %g %g %g\n", f[0], f[1], f[2], f[3]);
      break;   /* report once per block */
    }
  }
  __real_free(e.p);
}
__attribute__((unused)) static void quar_add(void *p, size_t sz) {
  memset(p, QUAR_POISON, sz);
  QuarEnt out[8]; int no = 0; int direct = 0;
  mutexLock(&g_can_lk);
  while (g_quar_bytes + sz > QUAR_CAP ||
         (g_quar_tail + 1) % QUAR_N == g_quar_head) {
    if (no >= 8) { direct = 1; break; }        /* couldn't make room: free p directly */
    QuarEnt e = g_quar[g_quar_head];
    g_quar[g_quar_head].p = NULL;
    g_quar_head = (g_quar_head + 1) % QUAR_N;
    if (e.p) { g_quar_bytes -= e.sz; out[no++] = e; }
  }
  if (!direct) {
    g_quar[g_quar_tail].p = p; g_quar[g_quar_tail].sz = (uint32_t)sz;
    g_quar_tail = (g_quar_tail + 1) % QUAR_N;
    g_quar_bytes += sz;
  }
  mutexUnlock(&g_can_lk);
  for (int i = 0; i < no; i++) quar_check_free(out[i]);   /* outside the lock */
  if (direct) __real_free(p);
}

/* CANARIES DISABLED. They were built to hunt the heap corruption that turned out
 * to be mesa's nouveau_mm_allocate bug (PORTING 7), which is fixed properly now.
 * Keeping them is actively risky here: libdrm_nouveau's nouveau_bo_new allocates
 * every GPU buffer with memalign(0x1000, size), so the +8 byte tail and the side
 * table sat directly in the GPU allocation path. Pass straight through; set
 * CANARY_ENABLE to 1 to re-arm for a future corruption hunt. */
#define CANARY_ENABLE 0
/* How many threads are currently inside one of our allocator wrappers.
 *
 * The GC stop-the-world check needs this. It already proves g_mmap_lock and the
 * log lock are free before it trusts a pause, but NOT newlib's malloc lock --
 * and a thread paused inside malloc holds that lock forever, so the collector
 * blocks the moment it allocates. That is what produced the 846 ms stop-world in
 * the round-107 log, and the forced resume that cut the collection short.
 *
 * A plain global is enough: the collector reaches gc_stop_world() from inside
 * GC_suspend_all, never from inside these wrappers, so a non-zero count always
 * means somebody ELSE is in here. No TLS, and two relaxed atomics per malloc. */
volatile int g_nx_alloc_busy;
int nx_alloc_busy_count(void) {
  return __atomic_load_n(&g_nx_alloc_busy, __ATOMIC_ACQUIRE);
}
#define NX_ALLOC_ENTER() __atomic_fetch_add(&g_nx_alloc_busy, 1, __ATOMIC_ACQ_REL)
#define NX_ALLOC_LEAVE() __atomic_fetch_sub(&g_nx_alloc_busy, 1, __ATOMIC_ACQ_REL)

static void *nx_malloc_inner(size_t sz) {
#if CANARY_ENABLE
  void *p = __real_malloc(sz + 8); can_track(p, sz); return p;
#else
  return __real_malloc(sz);
#endif
}

/* Thin wrapper: marks this thread as inside the allocator so the GC
 * stop-the-world check can refuse to pause while somebody holds newlib's
 * malloc lock. Body moved to nx_malloc_inner unchanged. */
void *__wrap_malloc(size_t sz) {
  NX_ALLOC_ENTER();
  void *r = nx_malloc_inner(sz);
  NX_ALLOC_LEAVE();
  return r;
}
/* GPU-allocation accounting. libdrm_nouveau's nouveau_bo_new() backs EVERY GPU
 * buffer with memalign(0x1000, size) before nvMapCreate/nvAddressSpaceMap, so
 * page-aligned memaligns are a faithful proxy for GPU memory demand. texStorage is
 * failing with GL_OUT_OF_MEMORY while newlib has GBs free, so we need to know which
 * of nouveau_bo_new's three steps fails. If memalign itself returns NULL, it is CPU
 * memory after all; if it always succeeds, the failure is downstream in
 * nvMapCreate (handle limit) or nvAddressSpaceMap (GPU address space), neither of
 * which we can fix from here -- but knowing the live total tells us the budget. */
static size_t g_gpu_live, g_gpu_peak; static unsigned long g_gpu_n, g_gpu_freed;

/* ---- dedicated GPU arena --------------------------------------------------
 * libdrm_nouveau backs every GPU buffer with memalign(0x1000, size). Those are
 * large (up to ~22 MB), page-aligned, and churned constantly, so serving them
 * from newlib's general heap shreds it: once fragmented, a 21 MB CONTIGUOUS
 * page-aligned run cannot be placed even with a gigabyte free in aggregate, and
 * memalign returns NULL -> nouveau_bo_new fails -> texStorage GL_OUT_OF_MEMORY ->
 * incomplete FBO -> black screen. Reserve one big contiguous region ONCE at first
 * use (before the heap is churned) and hand GPU buffers out of it with a simple
 * page bitmap, so they can never fragment newlib and newlib can never fragment
 * them. Falls back to plain memalign if the arena is full or unavailable. */
#define GPUA_PAGE   0x1000u
#define GPUA_BYTES  ((size_t)1280 * 1024 * 1024)   /* contiguous GPU region.
   1152 MB came from FRUIT NINJA ("observed 927 MB live and still failing at
   1024 MB"). That number has never been measured for Angry Birds Reloaded, and
   holding 1152 MB out of a 2048 MB newlib heap is what starved the OC commit
   pool into the hard OOM on this game's first scene load (see OC_POOL_BYTES).
   MEASURED for this game now, and 896 was NOT enough:
       [gpua] high-water 835 MB of 896 MB arena
       [gpu]  memalign FAILED size=65536 KB   (x3)
   The arena filled to 835/896, after which 64 MB buffers fell back to plain
   memalign and FAILED -- which is exactly the documented black-texture /
   missing-asset failure. So this game wants roughly what Fruit Ninja did.
   Round 7 uses 1280. Logs show the scene needs about 1241 MB when the three
   failed requests (64 + 64 + 16 MB) are included, so placement alone cannot
   make 1152 sufficient. The extra 128 MB is funded by trimming the OC pool by
   128 MB versus the original 768 MB setting; the recorded OC allocation peak
   is 611 MB, below the new 640 MB budget. gpua_init's 64 MB shrink ladder remains the safety valve if
   a particular boot cannot reserve the full target contiguously.
   A previous 1344 MB experiment was funded by squeezing MMAP_ARENA_RESERVE to
   512 and hung on 3 of 5 boots before gpua_init. This round keeps that arena at
   its known-good 640 MB instead.
   Observed arena high-water was 1094 MB with the old shared-region policy, and
   the requests that failed were 64 MB ones refused with 107 MB free -- i.e.
   fragmentation, now addressed by the two-ended placement below.
   Note gpua_alloc still falls back to memalign and the init ladder still
   shrinks in 64 MB steps, so this is a target, not a hard requirement. */
#define GPUA_MIN    (64u * 1024)                   /* below this, use newlib */
/* ...and above this too. THE GPU ARENA IS NOT A GENERAL PAGE-ALIGNED ALLOCATOR.
 *
 * The routing test is `al >= 0x1000 && sz >= GPUA_MIN`, which is meant to spot
 * nouveau_bo_new. It also catches ANY other big page-aligned allocation, and
 * main.c allocates the OC commit pool as exactly that:
 *
 *     pool = memalign(0x1000, pool_sz);          // main.c, 768 MB
 *
 * So the commit pool was being served OUT OF THE GPU ARENA. The evidence is
 * unambiguous: the arena's first high-water step was 768 MB at n=2 -- equal to
 * OC_POOL_BYTES to the megabyte -- and it stayed 768 MB when the arena was
 * resized 896 -> 1152, because it was never a function of the arena at all.
 * Two thirds of a 1152 MB GPU arena was holding the commit pool, leaving ~384 MB
 * for actual GPU buffers, which is why textures kept failing however much the
 * arena was grown. Growing it just grew the part the pool did not use.
 *
 * Real nouveau buffer objects in this game top out at 64 MB (the repeated
 * "65536 KB" requests in the log). 128 MB is therefore a ceiling with 2x
 * headroom that no genuine BO can reach, while excluding the pool and anything
 * else of that scale. Oversized requests fall through to plain memalign, which
 * is where they always should have gone. */
#define GPUA_MAX    ((size_t)128 * 1024 * 1024)
/* Keep the recurring 64 MiB BO class at the top of the arena and everything
 * smaller at the bottom. There is deliberately no fixed partition: either
 * class may consume all currently free space. r6 used an 8 MiB threshold, but
 * hardware still failed 64 MiB requests with 105 MiB total free / 54 MiB as
 * the largest run: 16/32 MiB churn was puncturing the 64 MiB region. The real
 * workload has a clean size gap (32 MiB -> 64 MiB), so split at 48 MiB. */
#define GPUA_LARGE  ((size_t)48 * 1024 * 1024)
static uint8_t  *gpua_base;
static size_t    gpua_pages;
static uint8_t  *gpua_used;      /* 1 byte/page: in use */
static uint32_t *gpua_runlen;    /* run length recorded at the first page */
static Mutex     gpua_lock;
static int       gpua_state;     /* 0=untried 1=ready -1=disabled */
static size_t    gpua_live_pages, gpua_peak_pages;
static size_t    gpua_hist_n[8], gpua_hist_b[8];   /* <64K,128K,256K,1M,4M,8M,16M,+ */

/* NOTE: never call debugPrintf while holding gpua_lock. Logging writes to the SD
 * card and can allocate internally, which re-enters __wrap_memalign -> gpua_alloc
 * -> mutexLock(gpua_lock); libnx mutexes are NOT recursive, so that self-deadlocks
 * the calling thread (observed as "clock thread driving Update (main stalled)").
 * Record the outcome under the lock, report it after releasing. */
static void gpua_init(void) {
  if (gpua_state) return;
  size_t got_mb = 0; int report = 0;
  mutexLock(&gpua_lock);
  if (!gpua_state) {
    size_t want = GPUA_BYTES;
    uint8_t *b = NULL;
    while (want >= (size_t)128 * 1024 * 1024) {     /* shrink until it fits */
      b = (uint8_t *)__real_memalign(GPUA_PAGE, want);
      if (b) break;
      want -= (size_t)64 * 1024 * 1024;
    }
    if (b) {
      gpua_pages  = want / GPUA_PAGE;
      gpua_used   = (uint8_t *)__real_calloc(gpua_pages, 1);
      gpua_runlen = (uint32_t *)__real_calloc(gpua_pages, sizeof(uint32_t));
      if (gpua_used && gpua_runlen) {
        gpua_base = b; gpua_state = 1;
        got_mb = want >> 20; report = 1;
      } else {
        __real_free(gpua_used); __real_free(gpua_runlen); __real_free(b);
        gpua_used = NULL; gpua_runlen = NULL;
        gpua_state = -1; report = 2;
      }
    } else {
      gpua_state = -1; report = 3;
    }
  }
  mutexUnlock(&gpua_lock);
  if (report == 1) debugPrintf("[gpua] GPU arena reserved: %u MB contiguous\n", (unsigned)got_mb);
  else if (report == 2) debugPrintf("[gpua] GPU arena DISABLED (bitmap alloc failed)\n");
  else if (report == 3) debugPrintf("[gpua] GPU arena DISABLED (no contiguous region)\n");
}

/* Claim [start, start+need). Caller holds gpua_lock. */
static void *gpua_claim(size_t start, size_t need, size_t sz) {
  for (size_t k = 0; k < need; k++) gpua_used[start + k] = 1;
  gpua_runlen[start] = (uint32_t)need;
  gpua_live_pages += need;
  if (gpua_live_pages > gpua_peak_pages) gpua_peak_pages = gpua_live_pages;
  { unsigned b = 0; size_t t = sz >> 16;            /* bucket by size */
    while (t && b < 7) { t >>= 1; b++; }
    gpua_hist_n[b]++; gpua_hist_b[b] += sz; }
  return gpua_base + start * GPUA_PAGE;
}

/* PLACEMENT: two-ended first fit, with no fixed partition.
 *
 * The old rolling cursor mixed smaller BOs into holes left by 64 MiB BOs.
 * At the observed failure it had 117 MiB free in a 1152 MiB arena, yet no
 * contiguous 64 MiB run, so nouveau_bo_new failed and Unity's texture/FBO
 * creation went black.  A previously tested fixed partition avoided some
 * mixing but stranded 178 MiB on the small side.
 *
 * Scanning sub-48-MiB allocations upward and the 64-MiB class downward gives the
 * useful part of segregation without reserving a byte for either class. Both
 * scans cover the entire arena, so unused space on one side is always usable
 * by the other. Starting at the corresponding edge (instead of a rolling
 * cursor) also reuses freed holes before opening new ones, keeping the live
 * set packed. */
static void *gpua_alloc(size_t sz) {
  gpua_init();
  if (gpua_state != 1) return NULL;
  size_t need = (sz + GPUA_PAGE - 1) / GPUA_PAGE;
  if (!need || need > gpua_pages) return NULL;
  void *out = NULL;
  mutexLock(&gpua_lock);
  if (sz >= GPUA_LARGE) {
    /* Highest-address fit. `run` is the free span [i, i+run) while walking
     * backward, so reaching `need` means i is the exact claim start. */
    size_t run = 0;
    for (size_t end = gpua_pages; end > 0; ) {
      size_t i = --end;
      if (!gpua_used[i]) {
        if (++run == need) { out = gpua_claim(i, need, sz); break; }
      } else {
        run = 0;
      }
    }
  } else {
    /* Lowest-address fit. Reusing the first suitable hole keeps small churn
     * concentrated at the bottom instead of fragmenting the untouched tail. */
    size_t i = 0;
    while (i + need <= gpua_pages) {
      while (i < gpua_pages && gpua_used[i]) i++;
      if (i + need > gpua_pages) break;
      size_t run = 0;
      while (run < need && !gpua_used[i + run]) run++;
      if (run == need) { out = gpua_claim(i, need, sz); break; }
      i += run + 1;
    }
  }
  mutexUnlock(&gpua_lock);
  return out;
}

static int gpua_owns(const void *p) {
  return gpua_state == 1 && (const uint8_t *)p >= gpua_base &&
         (const uint8_t *)p < gpua_base + gpua_pages * GPUA_PAGE;
}

/* Snapshot fragmentation without logging under gpua_lock. Used only on the
 * bounded allocation-failure path, so the full bitmap walk is not hot. */
static void gpua_space_stats(size_t *free_pages, size_t *largest_run) {
  size_t free_n = 0, run = 0, largest = 0;
  mutexLock(&gpua_lock);
  if (gpua_state == 1) {
    for (size_t i = 0; i < gpua_pages; i++) {
      if (!gpua_used[i]) {
        free_n++;
        if (++run > largest) largest = run;
      } else {
        run = 0;
      }
    }
  }
  mutexUnlock(&gpua_lock);
  if (free_pages) *free_pages = free_n;
  if (largest_run) *largest_run = largest;
}

static void gpua_free(void *p) {
  size_t i = (size_t)(((uint8_t *)p - gpua_base) / GPUA_PAGE);
  mutexLock(&gpua_lock);
  uint32_t n = (i < gpua_pages) ? gpua_runlen[i] : 0;
  if (n) {
    for (uint32_t k = 0; k < n; k++) gpua_used[i + k] = 0;
    gpua_runlen[i] = 0;
    if (gpua_live_pages >= n) gpua_live_pages -= n; else gpua_live_pages = 0;
  }
  mutexUnlock(&gpua_lock);
}
static void *nx_memalign_inner(size_t al, size_t sz) {
#if CANARY_ENABLE
  void *p = __real_memalign(al ? al : 8, sz + 8); can_track(p, sz); return p;
#else
  void *p = NULL;
  int gpua_missed = 0, gpua_eligible = 0;
  if (al >= 0x1000 && sz >= GPUA_MIN && sz <= GPUA_MAX) {
    gpua_eligible = 1;
    p = gpua_alloc(sz);                                     /* dedicated GPU arena */
    if (!p) gpua_missed = 1;   /* arena full (or disabled) -> newlib fallback */
  }
  if (!p) p = __real_memalign(al ? al : 8, sz);
  if (gpua_missed) {
    /* The arena being full is the leading indicator of the black-texture
     * failure: the fallback below can still succeed for a while, then starts
     * returning NULL. Log the first few so the NEXT run says "arena too small"
     * before it says "memalign FAILED". */
    static unsigned nm;
    if (nm < 8) { nm++;
      size_t free_pages = 0, largest_run = 0;
      gpua_space_stats(&free_pages, &largest_run);
      debugPrintf("[gpua] arena FULL -- %u KB fell back to newlib "
                  "(arena %u/%u MB, free=%u MB largest=%u MB, allocs=%lu frees=%lu)\n",
                  (unsigned)(sz >> 10),
                  (unsigned)((gpua_live_pages * GPUA_PAGE) >> 20),
                  (unsigned)((gpua_pages * GPUA_PAGE) >> 20),
                  (unsigned)((free_pages * GPUA_PAGE) >> 20),
                  (unsigned)((largest_run * GPUA_PAGE) >> 20),
                  g_gpu_n, g_gpu_freed);
      if (nm == 1) {   /* once, on the first fill: what shape are the requests? */
        static const char *lbl[8] = {"<64K","<128K","<256K","<512K","<1M","<2M","<4M",">=4M"};
        for (unsigned b = 0; b < 8; b++)
          if (gpua_hist_n[b])
            debugPrintf("[gpua]   hist %-6s n=%u total=%u MB\n", lbl[b],
                        (unsigned)gpua_hist_n[b], (unsigned)(gpua_hist_b[b] >> 20));
      } }
  }
  if (al >= 0x1000) {                       /* nouveau_bo_new signature */
    if (!p) {
      static unsigned nf;
      if (nf < 3) { nf++;
        debugPrintf("[gpu] memalign FAILED size=%u KB (live=%u MB peak=%u MB allocs=%lu frees=%lu)\n",
                    (unsigned)(sz >> 10), (unsigned)(g_gpu_live >> 20),
                    (unsigned)(g_gpu_peak >> 20), g_gpu_n, g_gpu_freed);
      }
    } else {
      g_gpu_live += sz; g_gpu_n++;
      if (g_gpu_live > g_gpu_peak) g_gpu_peak = g_gpu_live;
      /* ABR round 3: the arena reached 768 MB in its FIRST TWO allocations and
       * 1.9 GB overall, which is not a plausible working set for this game at
       * 720p -- so before sizing anything else, find out WHAT is being asked
       * for. Log every large page-aligned request with its size, who served it
       * and the running totals. Bounded so it cannot flood the SD card. */
      { static unsigned nbig;
        if (sz >= (4u << 20) && nbig < 40) { nbig++;
          debugPrintf("[gpubo] #%-3lu %6u KB %-6s | arena %u/%u MB | live %u MB freed=%lu\n",
                      g_gpu_n, (unsigned)(sz >> 10),
                      (!gpua_eligible || gpua_missed) ? "newlib" : "arena",
                      (unsigned)((gpua_live_pages * GPUA_PAGE) >> 20),
                      (unsigned)((gpua_pages * GPUA_PAGE) >> 20),
                      (unsigned)(g_gpu_live >> 20), g_gpu_freed); } }
      /* ABR: report only when the arena crosses a new 64 MB high-water mark.
       * Per-alloc logging would be thousands of SD writes; this is a handful,
       * and it is the measurement that tells us what GPUA_BYTES should be for
       * THIS game instead of inheriting Fruit Ninja's 1152 MB. */
      { static size_t hw_mb;
        size_t live_mb = (gpua_live_pages * GPUA_PAGE) >> 20;
        if (live_mb >= hw_mb + 64) { hw_mb = live_mb & ~63u;
          debugPrintf("[gpua] high-water %u MB of %u MB arena (bo live=%u MB peak=%u MB n=%lu)\n",
                      (unsigned)live_mb, (unsigned)((gpua_pages * GPUA_PAGE) >> 20),
                      (unsigned)(g_gpu_live >> 20), (unsigned)(g_gpu_peak >> 20), g_gpu_n); } }
      if (0)
        debugPrintf("[gpu] bo allocs=%lu freed=%lu LIVE=%u MB | arena %u/%u MB peak %u MB\n",
                    g_gpu_n, g_gpu_freed, (unsigned)(g_gpu_live >> 20),
                    (unsigned)((gpua_live_pages * GPUA_PAGE) >> 20),
                    (unsigned)((gpua_pages * GPUA_PAGE) >> 20),
                    (unsigned)((gpua_peak_pages * GPUA_PAGE) >> 20));
      { static unsigned hist_shown; if (0) {
        static const char *lbl[8] = {"<64K","<128K","<256K","<512K","<1M","<2M","<4M",">=4M"};
        for (unsigned b = 0; b < 8; b++)
          if (gpua_hist_n[b])
            debugPrintf("[gpu]   %-6s n=%u total=%u MB\n", lbl[b],
                        (unsigned)gpua_hist_n[b], (unsigned)(gpua_hist_b[b] >> 20)); } }
    }
  }
  return p;
#endif
}

/* Thin wrapper: marks this thread as inside the allocator so the GC
 * stop-the-world check can refuse to pause while somebody holds newlib's
 * malloc lock. Body moved to nx_memalign_inner unchanged. */
void *__wrap_memalign(size_t al, size_t sz) {
  NX_ALLOC_ENTER();
  void *r = nx_memalign_inner(al, sz);
  NX_ALLOC_LEAVE();
  return r;
}
static void *nx_calloc_inner(size_t n, size_t s) {
#if CANARY_ENABLE
  if (s && n > (SIZE_MAX - 8) / s) return NULL;
  size_t sz = n * s;
  void *p = __real_malloc(sz + 8);
  if (p) { memset(p, 0, sz); can_track(p, sz); }
  return p;
#else
  return __real_calloc(n, s);
#endif
}

/* Thin wrapper: marks this thread as inside the allocator so the GC
 * stop-the-world check can refuse to pause while somebody holds newlib's
 * malloc lock. Body moved to nx_calloc_inner unchanged. */
void *__wrap_calloc(size_t n, size_t s) {
  NX_ALLOC_ENTER();
  void *r = nx_calloc_inner(n, s);
  NX_ALLOC_LEAVE();
  return r;
}
static void *nx_realloc_inner(void *p, size_t sz) {
#if CANARY_ENABLE
  if (p) can_untrack_check((uintptr_t)p);
  void *q = __real_realloc(p, sz + 8); can_track(q, sz); return q;
#else
  /* An arena block is NOT a newlib block: handing it to realloc would corrupt the
   * heap. Copy out to a real allocation and release the arena slot. */
  if (p && gpua_owns(p)) {
    size_t old_pages = 0;
    { size_t i = (size_t)(((uint8_t *)p - gpua_base) / GPUA_PAGE);
      if (i < gpua_pages) old_pages = gpua_runlen[i]; }
    size_t oldsz = old_pages * GPUA_PAGE;
    void *q = __real_malloc(sz);
    if (q) memcpy(q, p, sz < oldsz ? sz : oldsz);
    gpua_free(p);
    return q;
  }
  return __real_realloc(p, sz);
#endif
}

/* Thin wrapper: marks this thread as inside the allocator so the GC
 * stop-the-world check can refuse to pause while somebody holds newlib's
 * malloc lock. Body moved to nx_realloc_inner unchanged. */
void *__wrap_realloc(void *p, size_t sz) {
  NX_ALLOC_ENTER();
  void *r = nx_realloc_inner(p, sz);
  NX_ALLOC_LEAVE();
  return r;
}

/* ---- copy-destination guard (the glBufferData killer, final line of defense) --
 * Every crash in this arc is a memmove/memcpy into a poisoned destination with
 * the same shape: low 32 bits ~0, high bits junk, or beyond the 39-bit user AS.
 * The producer (mesa transfer-map path; one reachable uninitialized-tx return
 * exists when staging mm_allocate fails) is still being pinned down. Until
 * then: validate copy destinations. An insane dst is logged WITH THE CALLER
 * ADDRESS (names the exact site) and the copy is SKIPPED -- a stale/garbage GL
 * buffer renders wrong at worst, instead of killing the process. Sane copies
 * pass straight through to newlib (tail-called, near-zero overhead). */
extern void *__real_memmove(void *dst, const void *src, size_t n);
extern void *__real_memcpy(void *dst, const void *src, size_t n);
static int copy_dst_insane(uintptr_t d, size_t n) {
  /* DISABLED now that mesa's nouveau_mm_allocate bug is fixed. This was a
   * crash-era net that SKIPS a memcpy it judges insane -- and one of its
   * rules (low-32 == 0) matches legitimate 4GB-aligned addresses such as our
   * own mmap arena base (0x7200000000). A silently skipped copy = missing
   * geometry//texture data, i.e. exactly a black screen. Keep the plumbing,
   * fail-open. */
  return 0;
  if (!n) return 0;
  if (d == 0) return 1;
  if (d >= 0x8000000000ULL) return 1;                 /* past 39-bit user AS */
  if ((d & 0xFFFFFFFFULL) == 0 && d != 0) return 1;   /* {lo32=0} poison shape */
  if ((d >> 32) == 0xdedededeULL) return 1;           /* our 0xDE free-poison in the ptr */
  if ((d & 0xFFFFFFFF00000000ULL) &&
      (d & 0xFFFFFFFFULL) == 0x1181a8d8ULL) return 1; /* observed mangled-high family */
  /* any 0xDE byte in the top half is quarantine poison bleed */
  if ((((d >> 32) & 0xFF) == 0xDE) && (((d >> 40) & 0xFF) == 0xDE)) return 1;
  return 0;
}
static void copy_report(const char *fn, void *dst, const void *src, size_t n) {
  static unsigned cnt = 0;
  if (cnt < 32 || (cnt & 0xFF) == 0)
    debugPrintf("[copyguard] SKIP %s(dst=%p, src=%p, n=%u) caller=%p\n",
                fn, dst, src, (unsigned)n, __builtin_return_address(0));
  cnt++;
}
void *__wrap_memmove(void *dst, const void *src, size_t n) {
  if (copy_dst_insane((uintptr_t)dst, n)) { copy_report("memmove", dst, src, n); return dst; }
  return __real_memmove(dst, src, n);
}
void *__wrap_memcpy(void *dst, const void *src, size_t n) {
  if (copy_dst_insane((uintptr_t)dst, n)) { copy_report("memcpy", dst, src, n); return dst; }
  return __real_memcpy(dst, src, n);
}

static void nx_free_inner(void *p) {
  uintptr_t a = (uintptr_t)p;
  if (a == 0) return;                         /* free(NULL): no-op */
  if ((a & 7) != 0 || a >= 0x8000000000ULL) { /* misaligned or past user AS */
    static unsigned n = 0;
    if (n++ < 128)
      debugPrintf("[heap] SKIP free(%p): corrupted pointer (asset-load smash) -- leaking\n", p);
    return;
  }
#if CANARY_ENABLE
  can_untrack_check(a);
#endif
  if (gpua_owns(p)) {
    /* BUGFIX (ABR): this path used to return without touching g_gpu_live, so
     * every arena-served buffer counted as allocated FOREVER. g_gpu_live then
     * grew monotonically and the "[gpu] memalign FAILED ... live=1784 MB"
     * diagnostic was reporting a number ~2x the truth, which is actively
     * misleading when sizing GPUA_BYTES. Decrement it here using the same
     * run-length the arena recorded. (gpua_live_pages was always correct --
     * that is why the [gpua] high-water line could be trusted and this one
     * could not.) */
    size_t idx = (size_t)(((uint8_t *)p - gpua_base) / GPUA_PAGE);
    size_t n   = (idx < gpua_pages) ? gpua_runlen[idx] : 0;
    size_t us  = n * (size_t)GPUA_PAGE;
    if (us <= g_gpu_live) g_gpu_live -= us; else g_gpu_live = 0;
    gpua_free(p); g_gpu_freed++; return;
  }
  if ((a & 0xFFF) == 0) {          /* page-aligned => almost certainly a nouveau bo */
    size_t us = malloc_usable_size(p);
    if (us <= g_gpu_live) g_gpu_live -= us; else g_gpu_live = 0;
    g_gpu_freed++;
  }
  /* Free quarantine DISABLED: it held up to 64MB of freed blocks hostage, which
   * starves mesa's GART slab allocations (nouveau bo memory comes from newlib).
   * It existed to prove the use-after-free, which is now fixed properly in
   * mesa (nouveau_mm_allocate slab-failure handling). Set QUAR_ENABLE to 1 to
   * re-arm it for future memory-corruption hunts. */
#define QUAR_ENABLE 0
#if QUAR_ENABLE
  size_t us = malloc_usable_size(p);
  if (us && us <= QUAR_MAX_SZ) { quar_add(p, us); return; }   /* hold + poison */
#endif
#if ABR_POISON_FREE
  /* Round 135: poison WITHOUT quarantine -- the useful half of the above at
   * none of its cost. Nothing is retained, so mesa's GART slabs are unaffected;
   * the block goes straight back to newlib. What it buys is determinism.
   *
   * Every boot randomises the whole layout (arena, heap region, module bases,
   * even the overcommit window SIZE: 1216/1472/1280 MB across the three boots in
   * the r134 log). So a stale or uninitialised pointer takes a different value
   * every run, and whether it lands on mapped memory is a coin flip. That is
   * why the same bug has produced a NULL klass, a bogus-but-mapped klass, an
   * unmapped klass and a raw 1 -- and why each guard held only until the shape
   * changed.
   *
   * With this on, memory that has been freed reads back as 0xDEDEDEDE...
   * regardless of layout: the same value every boot, and one the dumper can
   * name on sight. It converts "random garbage" into a signal that says
   * use-after-free, or -- if the garbage is still random -- rules reuse out. */
  { const size_t us = malloc_usable_size(p);
    if (us) memset(p, QUAR_POISON, us); }
#endif
  __real_free(p);
}

/* Thin wrapper: marks this thread as inside the allocator so the GC
 * stop-the-world check can refuse to pause while somebody holds newlib's
 * malloc lock. Body moved to nx_free_inner unchanged. */
void __wrap_free(void *p) {
  NX_ALLOC_ENTER();
  nx_free_inner(p);
  NX_ALLOC_LEAVE();
}
