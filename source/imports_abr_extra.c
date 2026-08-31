/* imports_abr_extra.c -- the import surface ANGRY BIRDS RELOADED needs on top
 * of the Fruit Ninja Classic+ loader core.
 *
 * Derived by diffing the undefined symbols of libmain.so + libunity.so +
 * libil2cpp.so (arm64-v8a, com.rovio.reloadedport 2.2.16218) against every
 * resolver table in the base (imports.c, imports_fruitninja_extra.c,
 * libc_shim.c, unity_imports.c, imports_audio.inc).
 *
 *   498 distinct undefined symbols across the three libraries
 *   493 already covered by the core
 *   ---
 *     5 remain, and they are all in this file.
 *
 * That the delta is only five is the headline compatibility result: Angry Birds
 * Reloaded is Unity 2022.3.7f1 / IL2CPP, the same engine generation as Fruit
 * Ninja Classic+ (2022.3.0f1), and its native surface is a near-subset. In
 * particular the whole MediaNDK / AImageReader / ASensor surface that
 * imports_fruitninja_extra.c already stubs is reused unchanged.
 *
 * Wire this in exactly like imports_fruitninja_extra.c:
 *   extern DynLibFunction abr_extra_functions[];
 *   extern size_t         abr_extra_numfunctions;
 * and append it to the combined table in imports.c. The *_functions /
 * *_numfunctions naming is the loader core's convention, not a choice.
 */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <math.h>
#include <sys/socket.h>   /* libnx: pulls sys/_iovec.h -- newlib has no sys/uio.h */
#include "so_util.h"   /* DynLibFunction */
#include "util.h"      /* debugPrintf */

#define NOTSUP(sym) do {                                              \
    static int _warned = 0;                                           \
    if (!_warned) { _warned = 1;                                      \
      debugPrintf("[stub] %s called -- unsupported on Switch\n", sym); }\
  } while (0)

/* ---- 1. nearbyintf -- REAL ------------------------------------------------
 * newlib has nearbyint(double) but the loader core never registered the float
 * form. Round-to-nearest-even in the current rounding mode; on aarch64 the
 * default mode IS round-to-nearest-even, and unlike rintf it must not raise
 * FE_INEXACT. `frintn` is exactly that instruction. */
static float nearbyintf_fake(float x) {
  float r;
  __asm__ volatile("frintn %s0, %s1" : "=w"(r) : "w"(x));
  return r;
}

/* ---- 2. writev -- REAL ----------------------------------------------------
 * Reached through libil2cpp's stdio when a managed Console/stderr write is
 * gathered. devkitA64's newlib has write() but no writev(); loop over the
 * iovec and let write() do the work. Returns bytes written, or -1/errno.
 * A short write terminates the loop, as POSIX requires. */
static ssize_t writev_fake(int fd, const struct iovec *iov, int iovcnt) {
  if (iovcnt < 0 || (iovcnt > 0 && !iov)) { errno = EINVAL; return -1; }
  ssize_t total = 0;
  for (int i = 0; i < iovcnt; i++) {
    if (iov[i].iov_len == 0) continue;
    ssize_t n = write(fd, iov[i].iov_base, iov[i].iov_len);
    if (n < 0) return total ? total : -1;
    total += n;
    if ((size_t)n < iov[i].iov_len) break;   /* short write: stop */
  }
  return total;
}

/* ---- 3. perror -- REAL ----------------------------------------------------
 * Routed to debug.log rather than a stderr nobody reads on Switch. */
static void perror_fake(const char *s) {
  const char *e = strerror(errno);
  if (s && *s) debugPrintf("[perror] %s: %s\n", s, e ? e : "?");
  else         debugPrintf("[perror] %s\n", e ? e : "?");
}

/* ---- 4. __android_log_buf_write -- REAL (redirected) ----------------------
 * Bionic's buffer-selecting logger. Same shape as __android_log_write with a
 * leading bufID; the loader core already funnels __android_log_print into
 * debugPrintf, so do the same here and ignore the buffer selector.
 * Returns >0 on success, as bionic does. */
static int __android_log_buf_write_fake(int bufID, int prio, const char *tag,
                                        const char *text) {
  (void)bufID; (void)prio;
  debugPrintf("[alog] %s: %s\n", tag ? tag : "?", text ? text : "");
  return text ? (int)strlen(text) : 0;
}

/* ---- 5. execl -- STUB (correctly fails) -----------------------------------
 * There is no process creation on Switch. Referenced from libil2cpp's
 * System.Diagnostics.Process glue, which the game does not use; if it is ever
 * reached, failing with ENOSYS is the honest answer and is what the managed
 * layer is written to handle. execl never returns on success, so returning -1
 * is unambiguous. */
static int execl_fake(const char *path, const char *arg0, ...) {
  (void)path; (void)arg0;
  NOTSUP("execl");
  errno = ENOSYS;
  return -1;
}

DynLibFunction abr_extra_functions[] = {
  { "__android_log_buf_write", (uintptr_t)&__android_log_buf_write_fake },
  { "execl",                   (uintptr_t)&execl_fake },
  { "nearbyintf",              (uintptr_t)&nearbyintf_fake },
  { "perror",                  (uintptr_t)&perror_fake },
  { "writev",                  (uintptr_t)&writev_fake },
};
size_t abr_extra_numfunctions =
    sizeof(abr_extra_functions) / sizeof(abr_extra_functions[0]);
