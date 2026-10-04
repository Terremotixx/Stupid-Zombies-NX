/* android_native_unity.c -- the 27 NDK symbols libunity.so imports, for the
 * ZOOKEEPER DX Switch port. Unity is NOT a NativeActivity, so unlike cr3_nx's
 * android_native.c there is no ANativeActivity glue / android_main / AInputQueue
 * here: the engine is driven by the JNI-registered natives (see main.c). We only
 * provide the raw NDK functions libunity calls directly:
 *
 *   ANativeWindow_acquire/_release/_fromSurface/_setBuffersGeometry/
 *                _getWidth/_getHeight/_getFormat      -> libnx NWindow
 *   ALooper_prepare/_acquire/_release/_pollOnce/_wake/_forThread
 *                                                     -> condvar wait/wake
 *   ASensorManager_ , ASensorEventQueue_ , ASensor_   -> "no sensors"
 *
 * IMPORTANT context-ownership note: the engine creates its OWN EGL context from
 * the ANativeWindow (cr3_nx's main.c creates none). The host must NOT create an
 * SDL_GL / EGL context. Use SDL for audio + HID only. Delete the
 * SDL_GL_SetAttribute/SDL_GL_CreateContext/SDL_GL_SwapWindow calls from the
 * earlier main_skeleton.c; the engine calls eglSwapBuffers itself.
 *
 * Needs devkitA64 + libnx (switch.h) + switch-mesa. Not host-compilable.
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <switch.h>
#include <GLES3/gl3.h> /* docked cursor overlay */
#include "util.h"   /* debugPrintf */
#include "config.h" /* screen_width / screen_height */

#ifndef AWINDOW_FORMAT_RGBA_8888
#define AWINDOW_FORMAT_RGBA_8888 1
#endif

/* opaque NDK types -> concrete libnx instances */
typedef struct ANativeWindow ANativeWindow;     /* == NWindow* at runtime */
typedef struct ALooper       ALooper;

/* ==========================================================================
 * dock-aware screen state (also read by unity_jni.c's Display getters)
 * ========================================================================== */
static u32 g_w = 1280, g_h = 720;   /* PvZ Fusion: LANDSCAPE, matches Switch panel (was Zookeeper TATE 720x1280) */

void android_native_update_mode(void){
  /* Latched: read the mode ONCE at launch and hold it for the session. The
   * render size feeds the graphics surface and the engine builds its render
   * target against it, so it cannot change underneath either of them --
   * which is also why config.txt says to dock BEFORE launching. */
  static int latched = 0;
  if (latched) return;
  latched = 1;

  const int docked = (appletGetOperationMode() == AppletOperationMode_Console);
  int h = docked ? config.docked_res : config.handheld_res;
  if (h != 720 && h != 1080) h = docked ? 1080 : 720;   /* belt and braces */
  if (h == 1080) { g_w = 1920; g_h = 1080; }
  else           { g_w = 1280; g_h = 720;  }
  debugPrintf("[boot] render %ux%u (%s, config %d)\n",
              (unsigned)g_w, (unsigned)g_h, docked ? "docked" : "handheld", h);
  /* Landscape throughout: keep the engine-reported surface size equal to the
   * real window so the render target matches what we present (no mismatch). */
  screen_width = (int)g_w; screen_height = (int)g_h;
}
u32 android_native_width(void)  { return g_w; }
u32 android_native_height(void) { return g_h; }

/* ==========================================================================
 * ANativeWindow  ->  libnx NWindow
 * ========================================================================== */
/* fbstub45: pin the displayed region to exactly the dimensions Unity renders
 * into. nwindowSetDimensions may allocate a width-aligned (e.g. 720 -> 768)
 * swapchain buffer; without a matching crop the compositor can scan the extra
 * uninitialized columns, which shows up as the image being "cut off" / garbage
 * on the right edge. Cropping to (0,0,bw,bh) guarantees only the rendered
 * content is presented (stretched to the panel, no cutoff). */
/* PvZ Fusion is landscape, always: the game ships no portrait mode, and the
 * Switch panel is landscape too, so the render maps straight onto it. The
 * inherited TATE path -- a compositor rotation picked by config.portrait, from
 * the VLN reference port -- is gone along with the option that drove it.
 * Nothing rotates anywhere in this port now, so clear the buffer transform
 * explicitly rather than inheriting whatever a previous producer left set. */
static void nx_window_set_geom(NWindow *w, u32 bw, u32 bh) {
  Result rc = nwindowSetDimensions(w, bw, bh);
  nwindowSetCrop(w, 0, 0, bw, bh);
  nwindowSetTransform(w, 0);                 /* landscape: no rotation */
  u32 aw = 0, ah = 0;
  nwindowGetDimensions(w, &aw, &ah);
  debugPrintf("[gfx] window geom: requested %ux%u (rc=0x%x), nwindow reports %ux%u, crop 0,0,%u,%u (landscape, no rotation)\n",
              bw, bh, rc, aw, ah, bw, bh);
}

ANativeWindow *android_native_window(void){
  NWindow *w = nwindowGetDefault();
  nx_window_set_geom(w, g_w, g_h);
  return (ANativeWindow *)w;
}
void     ANativeWindow_acquire(ANativeWindow *w){ (void)w; }                 /* singleton: refcount no-op */
void     ANativeWindow_release(ANativeWindow *w){ (void)w; }
ANativeWindow *ANativeWindow_fromSurface(void *env, void *surface){
  (void)env; (void)surface; return android_native_window();               /* one surface == our window */
}
int32_t  ANativeWindow_getWidth (ANativeWindow *w){ (void)w; return (int32_t)g_w; }
int32_t  ANativeWindow_getHeight(ANativeWindow *w){ (void)w; return (int32_t)g_h; }
int32_t  ANativeWindow_getFormat(ANativeWindow *w){ (void)w; return AWINDOW_FORMAT_RGBA_8888; }
int32_t  ANativeWindow_setBuffersGeometry(ANativeWindow *w, int32_t width, int32_t height, int32_t format){
  (void)format;
  /* The NX window is a FIXED-SIZE display. Resizing the real window to a
   * non-native size (e.g. the game's saved 640x1137 low-res, applied EARLY at
   * startup when Unity knows it from PlayerPrefs) makes mesa build a 640x1137
   * swapchain whose buffers the NX display path never consumes -> the first
   * eglSwapBuffers blocks forever (the boot-2 hang). Android devices without
   * hardware resolution scaling behave exactly like this fix: the resize
   * "succeeds" (returns 0) but readback (getWidth/getHeight, eglQuerySurface)
   * still shows the native size, which is precisely how Unity detects
   * "Hardware resolution scaling not supported" and falls back to its software
   * blit -- the same path that already works when SetResolution happens late
   * at frame 2. So: accept only the native geometry; report success for the
   * rest so Unity's own fallback engages. */
  if (width > 0 && height > 0) {
    if ((u32)width != g_w || (u32)height != g_h) {
      debugPrintf("[gfx] setBuffersGeometry %dx%d REJECTED (fixed-size window stays %ux%u; engine will blit-scale)\n",
                  width, height, g_w, g_h);
      return 0;
    }
    nx_window_set_geom((NWindow *)w, (u32)width, (u32)height);
  }
  return 0;
}

/* ==========================================================================
 * ALooper -- Unity uses it as a per-thread wait/wake primitive (not real fd
 * polling), so a condvar-backed looper is sufficient. If the engine turns out
 * to register real fds, port cr3_nx's fake-fd PollItem layer in here.
 * ========================================================================== */
#define ALOOPER_POLL_WAKE     (-1)
#define ALOOPER_POLL_TIMEOUT  (-3)
#define MAX_LOOPERS 16

struct ALooper { Mutex m; CondVar cv; int signaled; int refs; u32 owner; int used; };
static struct ALooper g_loopers[MAX_LOOPERS];
static Mutex g_loopers_lock;
static int   g_loopers_init = 0;

static void loopers_once(void){ if(!g_loopers_init){ mutexInit(&g_loopers_lock); g_loopers_init=1; } }

static struct ALooper *looper_for(u32 tid, int create){
  loopers_once();
  mutexLock(&g_loopers_lock);
  for (int i=0;i<MAX_LOOPERS;i++) if (g_loopers[i].used && g_loopers[i].owner==tid){
    struct ALooper *l=&g_loopers[i]; mutexUnlock(&g_loopers_lock); return l; }
  if (create) for (int i=0;i<MAX_LOOPERS;i++) if (!g_loopers[i].used){
    struct ALooper *l=&g_loopers[i];
    l->used=1; l->owner=tid; l->signaled=0; l->refs=1;
    mutexInit(&l->m); condvarInit(&l->cv);
    mutexUnlock(&g_loopers_lock); return l; }
  mutexUnlock(&g_loopers_lock);
  return NULL;
}
static u32 cur_tid(void){ return (u32)(uintptr_t)threadGetCurHandle(); }

ALooper *ALooper_prepare(int opts){ (void)opts; return (ALooper *)looper_for(cur_tid(), 1); }
ALooper *ALooper_forThread(void){  return (ALooper *)looper_for(cur_tid(), 0); }
void     ALooper_acquire(ALooper *l){ struct ALooper *L=(void*)l; if(L){ mutexLock(&L->m); L->refs++; mutexUnlock(&L->m);} }
void     ALooper_release(ALooper *l){ struct ALooper *L=(void*)l; if(L){ mutexLock(&L->m); if(--L->refs<=0) L->used=0; mutexUnlock(&L->m);} }

void ALooper_wake(ALooper *l){
  struct ALooper *L=(void*)l; if(!L) return;
  mutexLock(&L->m); L->signaled=1; condvarWakeAll(&L->cv); mutexUnlock(&L->m);
}
int ALooper_pollOnce(int timeoutMillis, int *outFd, int *outEvents, void **outData){
  struct ALooper *L = (void*)looper_for(cur_tid(), 1);
  if (outFd) *outFd=0;
  if (outEvents) *outEvents=0;
  if (outData) *outData=NULL;
  mutexLock(&L->m);
  if (!L->signaled){
    if (timeoutMillis==0){ mutexUnlock(&L->m); return ALOOPER_POLL_TIMEOUT; }
    if (timeoutMillis<0)  condvarWait(&L->cv,&L->m);
    else condvarWaitTimeout(&L->cv,&L->m,(u64)timeoutMillis*1000000ull);
  }
  int was = L->signaled; L->signaled=0;
  mutexUnlock(&L->m);
  return was ? ALOOPER_POLL_WAKE : ALOOPER_POLL_TIMEOUT;
}
/* Unity rarely uses these two, but provide them for completeness. */
int ALooper_addFd(ALooper *l,int fd,int ident,int events,void *cb,void *data){
  (void)l;(void)fd;(void)ident;(void)events;(void)cb;(void)data; return 1; }
int ALooper_removeFd(ALooper *l,int fd){ (void)l;(void)fd; return 1; }

/* ==========================================================================
 * Sensors -- report none. (CR3 imported no ASensorManager; Unity does, so these
 * must exist and return a clean empty state rather than be missing symbols.)
 * ========================================================================== */
void *ASensorManager_getInstance(void){ static int x; return &x; }
void *ASensorManager_getInstanceForPackage(const char *p){ (void)p; return ASensorManager_getInstance(); }
int   ASensorManager_getSensorList(void *m, void **list){ (void)m; if(list)*list=NULL; return 0; }
void *ASensorManager_getDefaultSensor(void *m, int type){ (void)m;(void)type; return NULL; }
void *ASensorManager_createEventQueue(void *m, void *looper, int ident, void *cb, void *data){
  (void)m;(void)looper;(void)ident;(void)cb;(void)data; static int q; return &q; }
int   ASensorManager_destroyEventQueue(void *m, void *q){ (void)m;(void)q; return 0; }

int   ASensorEventQueue_enableSensor (void *q, const void *s){ (void)q;(void)s; return -1; }
int   ASensorEventQueue_disableSensor(void *q, const void *s){ (void)q;(void)s; return 0; }
int   ASensorEventQueue_setEventRate (void *q, const void *s, int32_t us){ (void)q;(void)s;(void)us; return 0; }
int   ASensorEventQueue_getEvents    (void *q, void *ev, size_t n){ (void)q;(void)ev;(void)n; return 0; }
int   ASensorEventQueue_hasEvents    (void *q){ (void)q; return 0; }

const char *ASensor_getName      (const void *s){ (void)s; return ""; }
const char *ASensor_getVendor    (const void *s){ (void)s; return ""; }
int         ASensor_getType      (const void *s){ (void)s; return 0; }
float       ASensor_getResolution(const void *s){ (void)s; return 0.0f; }
int         ASensor_getMinDelay  (const void *s){ (void)s; return 0; }

/* cr3 dead-handler stub: no orientation sensor -> report level. */
void android_get_orientation(float *x, float *y, float *z){
  if (x) *x = 0.0f;
  if (y) *y = 0.0f;
  if (z) *z = 0.0f;
}

/* ==========================================================================
 * HID -> Unity input, via nx_pointer.
 *
 * nx_pointer owns every pointing device on the console -- touchscreen,
 * stick, USB mouse and gyro -- plus the on-screen cursor and its settings
 * file. Angry Birds Reloaded is a ONE-FINGER game: aiming the slingshot is a
 * single press-drag-release, so the Fruit Ninja base's TWO-cursor module was
 * swapped for the single-cursor one from the Papers, Please port. A second
 * cursor would have no slingshot to pull.
 * It hands back device-independent NxpEvents (id, x, y, phase); everything
 * below is the translation from those into the fake Android MotionEvents that
 * nativeInjectEvent expects (unity_input.c), and the B -> Back key mapping,
 * which is a game binding rather than a pointer concern.
 *
 * This replaces the port's original stick-cursor/dot-overlay implementation.
 * ========================================================================== */
#include <stdio.h>
#include "unity_input.h"
#include "nx_pointer.h"

/* Locked stdio from libc_shim.c. nx_pointer writes pointer.cfg from the render
 * thread while the engine's workers are opening bundle files on their own
 * threads; both sides go through these so they never touch newlib's FILE table
 * at the same time. See the note above fopen_fn in nx_pointer.h. */
FILE *nx_fopen_locked(const char *path, const char *mode);
int   nx_fclose_locked(FILE *f);

/* Our own pad, used ONLY for B -> Back. nx_pointer keeps a separate PadState of
 * its own; that is fine, because padUpdate() snapshots HID shared memory into
 * whichever struct you hand it, so each PadState tracks its own press/release
 * edges independently. */
static PadState g_pad;

/* R3.70.22: shared state/direct managed pause helpers. */
extern volatile unsigned long g_sz_last_frame_draws;
extern int sz_level_pause_direct(void);
extern int sz_level_resume_direct(void);
extern volatile uintptr_t g_trace716_level_start_self;
extern void nxp_r362_visual_reset(void);

static uintptr_t g_start_level_self_seen = 0;

/* R3.38: START controls the game's pause UI.
 * R3.70.7: entering Pause uses Android Back instead of a gameplay touch. */
static int g_start_pause_toggle = 0;
static int g_start_pause_back_down = 0;
static int g_start_pause_pending = 0;

/* R3.70.9: if the first Android-Back pause request is ignored during a
 * transient gameplay/modal frame, replay one clean Back pulse automatically. */
static int g_start_pause_pending_frames = 0;
static int g_start_pause_retry_down = 0;
static int g_start_pause_retry_done = 0;

extern int nxp_r362_result_visual_is_on(void);


static void nxp_log_line(const char *msg){ debugPrintf("%s", msg); }

void android_native_input_init(void){
  NxpConfig c;
  memset(&c, 0, sizeof c);

  /* Render size is fixed for the session (chosen at launch from config.txt),
   * so the module never needs to be told about a mid-session change. */
  c.screen_w        = (int)g_w;
  c.screen_h        = (int)g_h;
  c.panel_w         = 1280;          /* the touch panel reports in its own     */
  c.panel_h         = 720;           /* 1280x720 space at every resolution     */
  c.data_dir        = GAME_HOME;     /* the .nro's own folder                  */

  /* The pointer id must be SMALL. Unity maps an Android pointer id into a
   * fixed-size touch pool, so the module's old 100 default was silently
   * dropped there -- the cursor drew and moved but its taps never arrived.
   * Real fingers take 0..7, the cursor takes 8: 9 pointers total, inside
   * UI_MAX_POINTERS, so unity_motionevent never has to clamp. */
  c.max_touch_slots = 8;
  c.cursor_id       = 8;

  c.stick_speed     = 0.0f;          /* 0 => library default, then pointer.cfg */
  c.mouse_sens      = 0.0f;          /* 0 => library default, then pointer.cfg */
  c.cursor_scale    = 0.58f;         /* SZ R3.22: ~1.62x larger, easier to see on Switch */
  c.cursor_margin_left = (float)g_h / 15.0f; /* 48px at 720p: slingshot guard */
  c.cursor_margin   = (float)g_h / 30.0f;    /* keep cursor point on screen */
  c.cursor_delay_frames = 1;         /* R3.57: visibility waits for actual menu frame */
  c.rotation        = 0;             /* Angry Birds is landscape; no TATE      */
  c.handle_touch    = 1;             /* module owns touch, as nx_pointer did   */
  c.log             = nxp_log_line;
  c.fopen_fn        = nx_fopen_locked;
  c.fclose_fn       = nx_fclose_locked;

  nxp_init(&c);

  /* Pad for the Back key. nxp_init has already called padConfigureInput. */
  padInitializeDefault(&g_pad);

  /* input_log_fn intentionally left unset: the MotionEvent-getter trace is a
   * debug aid (set it to debugPrintf to re-enable). Leaving it off keeps the
   * touch path log-silent so taps don't stutter on slow SD writes. */
}

/* Flush a pending sensitivity change on the way out. Normally nx_pointer
 * saves by itself 3s after the last adjustment; this catches the case where
 * the player quits inside that window. */
void android_native_input_shutdown(void){
  nxp_save_settings();
}

/* inject signature == recovered nativeInjectEvent: (env,thiz,InputEvent,int)->Z */
typedef uint8_t (*inject_fn)(void*,void*,void*,int);

/* ---- live pointer set ----------------------------------------------------
 * Android hands the engine the FULL set of pointers that are currently down on
 * every event; the action word says what happened, and for the multi-pointer
 * variants its high byte says which INDEX in that array it happened to. So we
 * have to keep the set ourselves rather than forwarding events one at a time.
 * Fingers and the cursor coexist here, which is what makes "touch the screen
 * while the cursor is up" behave like real multitouch instead of a fight. */
#define NXG_MAX UI_MAX_POINTERS

static int   g_live_id[NXG_MAX];
static float g_live_x [NXG_MAX];
static float g_live_y [NXG_MAX];
static unsigned char g_live_age[NXG_MAX];
static int   g_live_n = 0;

static int live_find(int id){
  for (int i = 0; i < g_live_n; i++)
    if (g_live_id[i] == id) return i;
  return -1;
}

static void emit(inject_fn inject, void *env, void *thiz, int action){
  if (g_live_n <= 0) return;
  inject(env, thiz,
         unity_motionevent(action, g_live_n, g_live_id, g_live_x, g_live_y), 0);
}

static void live_release_all(inject_fn inject, void *env, void *thiz, const char *why){
  if (g_live_n <= 0) return;
  debugPrintf("[input] release-all n=%d (%s)\n", g_live_n, why ? why : "cleanup");

  while (g_live_n > 0){
    const int idx = g_live_n - 1;
    emit(inject, env, thiz,
         (g_live_n == 1)
           ? AMOTION_ACTION_UP
           : (AMOTION_ACTION_POINTER_UP | (idx << AMOTION_ACTION_PTR_IDX_SHIFT)));
    g_live_n--;
  }
}

/* R3.35: pausing must not release an aim as a normal UP. */
static void live_cancel_all(inject_fn inject, void *env, void *thiz, const char *why){
  if (g_live_n <= 0) return;
  debugPrintf("[input] cancel-all n=%d (%s)\n", g_live_n, why ? why : "cancel");
  emit(inject, env, thiz, AMOTION_ACTION_CANCEL);
  g_live_n = 0;
}

static void sz_ui_tap(inject_fn inject, void *env, void *thiz, float x, float y) {
  live_cancel_all(inject, env, thiz, "START UI tap");
  g_live_n = 1;
  g_live_id[0] = 8;
  g_live_x[0] = x;
  g_live_y[0] = y;
  g_live_age[0] = 0;
  emit(inject, env, thiz, AMOTION_ACTION_DOWN);
  emit(inject, env, thiz, AMOTION_ACTION_UP);
  g_live_n = 0;
}

/* R3.59: distinguish the actual root menu from STATIC submenus.
 * Starts true on boot. Store/Settings use their own existing state. */
static int g_sz_mainmenu_root = 1;

/* R3.68: blocked pointer sequences are tracked PER pointer id.
 *
 * R3.66 stored only one blocked id. Real multitouch can have several live ids
 * at once, so a later blocked DOWN could overwrite an earlier sequence before
 * its MOVE/UP arrived and leak that tail into Unity. */
#define SZ_BLOCKED_POINTER_SLOTS 64
static unsigned char g_sz_swallow_pointer[SZ_BLOCKED_POINTER_SLOTS];

static int sz_swallow_pointer_valid_id(int id) {
  return id >= 0 && id < SZ_BLOCKED_POINTER_SLOTS;
}

static void sz_swallow_pointer_begin(int id, const char *why) {
  if (sz_swallow_pointer_valid_id(id)) {
    g_sz_swallow_pointer[id] = 1;
    debugPrintf("[input68] block sequence id=%d (%s)\n",
                id, why ? why : "?");
  } else {
    debugPrintf("[input68] blocked DOWN out-of-range id=%d (%s)\n",
                id, why ? why : "?");
  }
}


/* R3.70.4 residual-drag cleanup:
 * Forced/abnormal gesture termination uses ACTION_CANCEL.
 * Normal NXP_UP remains untouched. */
void android_native_feed_hid(inject_fn inject, void *env, void *thiz){
  /* R3.70.23: Level.Start gives us a fresh instance for every gameplay level.
   * Never inherit Pause bookkeeping or framebuffer hysteresis from the
   * previous Level. */
  {
    const uintptr_t level_self_now = g_trace716_level_start_self;

    if (level_self_now && level_self_now != g_start_level_self_seen) {
      if (g_start_pause_back_down) {
        inject(env, thiz, unity_keyevent(AKEY_ACTION_UP, AKEYCODE_BACK), 0);
        g_start_pause_back_down = 0;
      }

      if (g_start_pause_retry_down) {
        inject(env, thiz, unity_keyevent(AKEY_ACTION_UP, AKEYCODE_BACK), 0);
        g_start_pause_retry_down = 0;
      }

      g_start_level_self_seen = level_self_now;

      g_start_pause_toggle = 0;
      g_start_pause_pending = 0;
      g_start_pause_pending_frames = 0;
      g_start_pause_retry_done = 0;

      nxp_r362_visual_reset();

      debugPrintf("[start723] NEW Level self=%p -> pause state RESET\n",
                  (void *)level_self_now);
    }
  }

  /* R3.70.10: deterministic one-frame Back pulse for the PRIMARY pause request.
   * R3.70.7 tied Back-UP to physical START release; that makes the Android key
   * duration depend on the user's press.  Release it here on the next HID frame
   * instead, before evaluating pause confirmation/retry. */
  if (g_start_pause_back_down) {
    inject(env, thiz, unity_keyevent(AKEY_ACTION_UP, AKEYCODE_BACK), 0);
    g_start_pause_back_down = 0;
    debugPrintf("[start710] primary Back pulse UP\n");
  }

  /* R3.70.9: confirm the requested Pause; if the first Back was ignored,
   * retry exactly once after a few frames.  The log from R3.70.8 shows this
   * exact failure mode: START request with no visual confirmation, followed by
   * a second physical START that succeeds. */
  if (g_start_pause_pending) {
    if (nxp_r362_result_visual_is_on()) {
      if (g_start_pause_retry_down) {
        inject(env, thiz, unity_keyevent(AKEY_ACTION_UP, AKEYCODE_BACK), 0);
        g_start_pause_retry_down = 0;
      }
      g_start_pause_pending = 0;
      g_start_pause_pending_frames = 0;
      g_start_pause_retry_done = 0;
      g_start_pause_toggle = 1;
      debugPrintf("[start709] PAUSE confirmed by visual detector\n");
    } else {
      if (g_start_pause_pending_frames < 120)
        g_start_pause_pending_frames++;

      /* Wait long enough for a normal request to settle.  Retry only after
       * the physical START's original Back key has been released. */
      if (!g_start_pause_retry_done &&
          g_start_pause_pending_frames >= 6) {
        inject(env, thiz, unity_keyevent(AKEY_ACTION_DOWN, AKEYCODE_BACK), 0);
        g_start_pause_retry_down = 1;
        g_start_pause_retry_done = 1;
        debugPrintf("[start709] first Pause request unconfirmed -> automatic Back retry DOWN frame=%d\n",
                    g_start_pause_pending_frames);
      } else if (g_start_pause_retry_down &&
                 g_start_pause_pending_frames >= 7) {
        inject(env, thiz, unity_keyevent(AKEY_ACTION_UP, AKEYCODE_BACK), 0);
        g_start_pause_retry_down = 0;
        debugPrintf("[start709] automatic Back retry UP\n");
      }

      /* Do not leave START permanently latched if even the retry is ignored. */
      if (g_start_pause_retry_done &&
          !g_start_pause_retry_down &&
          g_start_pause_pending_frames >= 24) {
        g_start_pause_pending = 0;
        g_start_pause_pending_frames = 0;
        g_start_pause_retry_done = 0;
        g_start_pause_toggle = 0;
        debugPrintf("[start709] Pause retry timeout; START re-armed\n");
      }
    }
  }

  extern void sz_result_probe_poll(void);
  sz_result_probe_poll();

  /* No set-screen call: the render size is chosen once at launch from
   * config.txt (handheld_res / docked_res) and held for the session, so g_w/g_h
   * cannot change under the module. */
  nxp_update();

  NxpEvent ev[24];
  const int n = nxp_poll(ev, (int)(sizeof ev / sizeof ev[0]));

  for (int i = 0; i < g_live_n; i++)
    if (g_live_age[i] < 255) g_live_age[i]++;

  /* MOVEs are batched: several pointers can move in one frame, and Android
   * expresses that as ONE action with every pointer's new position, not one
   * event each. A DOWN or UP closes the batch. */
  int move_pending = 0;

  for (int i = 0; i < n; i++){
    const int   id = ev[i].id;
    const float x  = ev[i].x, y = ev[i].y;
    int idx = live_find(id);

    if (ev[i].phase == NXP_MOVE){
      if (idx < 0) continue;                    /* never saw its DOWN */
      g_live_x[idx] = x; g_live_y[idx] = y;
      g_live_age[idx] = 0;
      move_pending = 1;
      continue;
    }

    if (move_pending){
      emit(inject, env, thiz, AMOTION_ACTION_MOVE);
      move_pending = 0;
    }

    /* R3.56: block main-menu privacy/legal link strip.
     * Keep top-right Store/settings untouched. */
    if (ev[i].phase == NXP_DOWN){
          /* R3.59: privacy handling moved AFTER real Store/Settings state detection. */
    }

    /* R3.68: finish the blocked sequence for THIS pointer id only.
     * Other fingers / controller pointers stay tracked independently. */
    if (sz_swallow_pointer_valid_id(id) && g_sz_swallow_pointer[id]) {
      if (ev[i].phase == NXP_MOVE) {
        continue;
      }

      if (ev[i].phase == NXP_UP) {
        debugPrintf("[input68] swallowed blocked UP id=%d\n", id);
        g_sz_swallow_pointer[id] = 0;
        continue;
      }

      if (ev[i].phase == NXP_DOWN) {
        /* If an old UP was lost, recover only this id and accept the fresh
         * sequence. Never clear the other simultaneously blocked ids. */
        debugPrintf("[input68] stale blocked sequence recovered on new DOWN id=%d\n",
                    id);
        g_sz_swallow_pointer[id] = 0;
      }
    }

    /* R3.61: screen state is sampled BEFORE Store/Settings process this DOWN.
     *
     * Important: both Store and Settings clear their state on the top-left
     * Back arrow before returning 0 so Unity can receive that same arrow tap.
     * R3.60 then saw "not in Store/Settings anymore" and incorrectly applied
     * the root Do-not-sell mask to the very same event.
     *
     * Keep both BEFORE and AFTER state. If either says Store/Settings, this
     * event belongs to that screen and privacy filtering is bypassed entirely.
     */
    if (ev[i].phase == NXP_DOWN){
      extern int sz_store_handle_pointer_down(float x, float y);
      extern int sz_store_is_open(void);
      extern int sz_settings_is_open(void);
      extern volatile unsigned long g_sz_last_frame_draws;

      const int was_store = sz_store_is_open();
      const int was_settings = sz_settings_is_open();

      if (sz_store_handle_pointer_down(x, y)){
        sz_swallow_pointer_begin(id, "Store blocked DOWN");
        if (g_live_n > 0)
          debugPrintf("[input704] CANCEL live gesture before blocked Store DOWN\n");
        live_cancel_all(inject, env, thiz, "R3.70.4 Store blocked sequence");
        continue;
      }

      const int now_store = sz_store_is_open();
      const int now_settings = sz_settings_is_open();

      const int belongs_to_store_or_settings =
        was_store || was_settings || now_store || now_settings;

      if (belongs_to_store_or_settings) {
        /* Store/Settings own this DOWN completely.
         * In particular, their left-side Back arrow must never fall through
         * into the main-menu privacy blocker. */
        debugPrintf("[screen61] bypass privacy wasS=%d wasSet=%d nowS=%d nowSet=%d x=%.1f y=%.1f\n",
                    was_store, was_settings, now_store, now_settings,
                    (double)x, (double)y);
      } else if (g_sz_last_frame_draws == 2) {
        const int in_legal_band =
          (y >= 0.0f && y <= (float)g_h * 0.20f &&
           x >= 0.0f && x <= (float)g_w * 0.92f);

        const int top_left =
          (x >= 0.0f && x <= 190.0f &&
           y >= 0.0f && y <= 170.0f);

        if (g_sz_mainmenu_root) {
          /* ONLY the actual root menu gets the privacy/legal dead-zone. */
          if (in_legal_band) {
            debugPrintf("[privacy61] ROOT ONLY blocked x=%.1f y=%.1f\n",
                        (double)x, (double)y);
            sz_swallow_pointer_begin(id, "root privacy blocked DOWN");
            if (g_live_n > 0)
              debugPrintf("[input704] CANCEL live gesture before blocked privacy DOWN\n");
            live_cancel_all(inject, env, thiz,
                            "R3.70.4 root privacy blocked sequence");
            continue;
          }

          const int root_play =
            (x >= (float)g_w * 0.42f && x <= (float)g_w * 0.58f &&
             y >= (float)g_h * 0.38f && y <= (float)g_h * 0.62f);

          if (root_play) {
            g_sz_mainmenu_root = 0;
            debugPrintf("[menu61] PLAY -> submenu/navigation x=%.1f y=%.1f\n",
                        (double)x, (double)y);
          }
        } else if (top_left) {
          /* STATIC submenu: same left zone is a real Back arrow. */
          g_sz_mainmenu_root = 1;
          debugPrintf("[menu61] STATIC submenu Back -> root\n");
        }
      }
    }

    if (ev[i].phase == NXP_DOWN){
      if (idx >= 0){                            /* already down -- treat as move */
        g_live_x[idx] = x; g_live_y[idx] = y;
        g_live_age[idx] = 0;
        move_pending = 1;
        continue;
      }

      if (g_live_n > 0) {
        debugPrintf("[input704] CANCEL old gesture on source switch -> id=%d\n", id);
        live_cancel_all(inject, env, thiz, "R3.70.4 source switch");
      }

      if (g_live_n >= NXG_MAX) continue;        /* out of slots */
      idx = g_live_n++;
      g_live_id[idx] = id; g_live_x[idx] = x; g_live_y[idx] = y;
      g_live_age[idx] = 0;
      emit(inject, env, thiz,
           (g_live_n == 1)
             ? AMOTION_ACTION_DOWN
             : (AMOTION_ACTION_POINTER_DOWN | (idx << AMOTION_ACTION_PTR_IDX_SHIFT)));
    }
    else if (ev[i].phase == NXP_UP){
      if (idx < 0) continue;
      /* R3.70.6 level release brake:
       * The stress log shows the inertia survives without any R3.70.4
       * abnormal cleanup firing. That means Unity is receiving a normal UP
       * with ScrollRect release velocity. On the actual LEVELS list only
       * (draws==3), feed two stationary MOVE samples immediately before UP.
       * This is the same zero-velocity brake technique already used by the
       * proven R3.49 transition containment, but without any neutral gate. */
      extern volatile unsigned long g_sz_last_frame_draws;
      if (g_sz_last_frame_draws == 3 && g_live_n == 1) {
        g_live_x[idx] = x;
        g_live_y[idx] = y;
        emit(inject, env, thiz, AMOTION_ACTION_MOVE);
        emit(inject, env, thiz, AMOTION_ACTION_MOVE);
        debugPrintf("[input706] LEVELS release brake id=%d x=%.1f y=%.1f\n",
                    id, (double)x, (double)y);
      }
      g_live_x[idx] = x; g_live_y[idx] = y;
      /* The lifting pointer is still IN the array for its own UP -- that is how
       * getActionIndex() identifies which one left. Remove it afterwards. */
      emit(inject, env, thiz,
           (g_live_n == 1)
             ? AMOTION_ACTION_UP
             : (AMOTION_ACTION_POINTER_UP | (idx << AMOTION_ACTION_PTR_IDX_SHIFT)));
      for (int k = idx + 1; k < g_live_n; k++){
        g_live_id [k-1] = g_live_id [k];
        g_live_x  [k-1] = g_live_x  [k];
        g_live_y  [k-1] = g_live_y  [k];
        g_live_age[k-1] = g_live_age[k];
      }
      g_live_n--;
    }
  }

  if (move_pending) emit(inject, env, thiz, AMOTION_ACTION_MOVE);

  for (int i = 0; i < g_live_n; i++){
    if (g_live_age[i] >= 4){
      debugPrintf("[input704] CANCEL stale live pointer id=%d age=%u\n",
                  g_live_id[i], (unsigned)g_live_age[i]);
      live_cancel_all(inject, env, thiz, "R3.70.4 stale pointer watchdog");
      break;
    }
  }

  /* ---- B -> Android Back, edge-triggered ---- */
  padUpdate(&g_pad);
  const u64 bdown = padGetButtonsDown(&g_pad);
  const u64 bup   = padGetButtonsUp(&g_pad);
  extern int sz_store_is_open(void);
  extern volatile unsigned long g_sz_last_frame_draws;
  if (bdown & HidNpadButton_Plus){
    /* R3.70.11: START belongs only to the in-level flow.
     *
     * <=2 draws = main/static UI, 3 draws = level list.
     * >3 draws  = dynamic in-level screens.
     *
     * R3.62 visual ON with no pause state means a result/modal screen, so do
     * not turn START into Back there either.  A confirmed Pause remains
     * eligible because g_start_pause_toggle==1, allowing START to Resume. */
    const int start_level_context =
      g_start_pause_toggle ||
      g_start_pause_pending ||
      ((g_sz_last_frame_draws > 3) &&
       !nxp_r362_result_visual_is_on());

    if (!start_level_context) {
      /* Outside a level START is a complete no-op.  Also clear any stale
       * pause bookkeeping so two presses can never behave like Android Back. */
      if (g_start_pause_back_down) {
        inject(env, thiz, unity_keyevent(AKEY_ACTION_UP, AKEYCODE_BACK), 0);
        g_start_pause_back_down = 0;
      }
      if (g_start_pause_retry_down) {
        inject(env, thiz, unity_keyevent(AKEY_ACTION_UP, AKEYCODE_BACK), 0);
        g_start_pause_retry_down = 0;
      }
      g_start_pause_toggle = 0;
      g_start_pause_pending = 0;
      g_start_pause_pending_frames = 0;
      g_start_pause_retry_done = 0;

      debugPrintf("[start711] START ignored outside active level draws=%lu visual=%d\n",
                  g_sz_last_frame_draws,
                  nxp_r362_result_visual_is_on());
    } else if (sz_store_is_open()) {
      g_start_pause_toggle = 0;
      g_start_pause_pending = 0;
      g_start_pause_pending_frames = 0;
      g_start_pause_retry_done = 0;
      if (g_start_pause_retry_down) {
        inject(env, thiz, unity_keyevent(AKEY_ACTION_UP, AKEYCODE_BACK), 0);
        g_start_pause_retry_down = 0;
      }
      if (g_live_n > 0)
        debugPrintf("[input704] CANCEL live gesture on ignored Store START\n");
      live_cancel_all(inject, env, thiz, "R3.70.4 Store lockdown");
      debugPrintf("[storelock] START ignored; only B/back-arrow enabled\n");
    } else if (g_start_pause_pending) {
      debugPrintf("[start709] physical START ignored: Pause request already pending\n");
    } else if (!g_start_pause_toggle) {
      /* R3.70.22: START calls the game's real PauseMenu directly.
       * Cursor visibility is now completely irrelevant. */
      live_cancel_all(inject, env, thiz, "R3.70.22 direct START/Pause");

      if (g_start_pause_retry_down) {
        inject(env, thiz, unity_keyevent(AKEY_ACTION_UP, AKEYCODE_BACK), 0);
        g_start_pause_retry_down = 0;
      }

      g_start_pause_back_down = 0;
      g_start_pause_pending_frames = 0;
      g_start_pause_toggle = 0;

      if (sz_level_pause_direct()) {
        /* R3.70.23: PauseMenu is the actual game method. Once it returns,
         * START state becomes PAUSED immediately; visual detection is no
         * longer the source of truth for the toggle. */
        g_start_pause_toggle = 1;
        g_start_pause_pending = 0;
        g_start_pause_pending_frames = 0;
        g_start_pause_retry_done = 0;
        debugPrintf("[start723] START -> PAUSE direct; toggle=1\n");
      } else {
        /* Safety fallback if method discovery failed. */
        inject(env, thiz, unity_keyevent(AKEY_ACTION_DOWN, AKEYCODE_BACK), 0);
        g_start_pause_back_down = 1;
        g_start_pause_pending = 1;
        g_start_pause_retry_done = 0;
        debugPrintf("[start722] direct PauseMenu unavailable -> Back fallback DOWN\n");
      }
    } else {
      /* R3.70.22: same for Resume -- no pointer/cursor/UI tap required. */
      if (!sz_level_resume_direct()) {
        /* Keep the old working UI tap only as an emergency fallback. */
        sz_ui_tap(inject, env, thiz,
                  (float)g_w * 0.500f, (float)g_h * 0.355f);
        debugPrintf("[start722] direct Resume unavailable -> pause UI fallback\n");
      } else {
        debugPrintf("[start722] START -> RESUME direct managed call\n");
      }

      g_start_pause_toggle = 0;
      g_start_pause_pending = 0;
      g_start_pause_pending_frames = 0;
      g_start_pause_retry_done = 0;

      /* Do not let the Pause visual survive into the following gameplay
       * input frame and make START look like a result/modal screen. */
      nxp_r362_visual_reset();

      if (g_start_pause_retry_down) {
        inject(env, thiz, unity_keyevent(AKEY_ACTION_UP, AKEYCODE_BACK), 0);
        g_start_pause_retry_down = 0;
      }
    }
  }

  /* R3.70.10: physical START release is NOT the normal Back-UP anymore.
   * This is only an emergency fallback if the next HID frame somehow did not
   * perform the fixed one-frame pulse release above. */
  if ((bup & HidNpadButton_Plus) && g_start_pause_back_down) {
    inject(env, thiz, unity_keyevent(AKEY_ACTION_UP, AKEYCODE_BACK), 0);
    g_start_pause_back_down = 0;
    debugPrintf("[start710] emergency physical-release Back UP\n");
  }

  /* R3.50: when the in-level Store is open, B uses the game's own
   * Level.CloseStorePanel(). Purchases stay unavailable and grant nothing.
   * Everywhere else B keeps the existing Android Back behaviour.
   */
  static int sz_store_b_consumed = 0;
  extern int sz_store_try_close(void);
  if (bdown & HidNpadButton_B){
    {
      extern volatile unsigned long g_sz_last_frame_draws;
      extern int sz_store_is_open(void);
      extern int sz_settings_is_open(void);

      if (g_sz_last_frame_draws == 2 &&
          !sz_store_is_open() &&
          !sz_settings_is_open())
        g_sz_mainmenu_root = 1;
    }
    g_start_pause_toggle = 0;
    g_start_pause_pending = 0;
    g_start_pause_pending_frames = 0;
    g_start_pause_retry_done = 0;
    if (g_start_pause_back_down) {
      inject(env, thiz, unity_keyevent(AKEY_ACTION_UP, AKEYCODE_BACK), 0);
      g_start_pause_back_down = 0;
      debugPrintf("[start709] B resync released pending START Back key\n");
    }
    if (g_start_pause_retry_down) {
      inject(env, thiz, unity_keyevent(AKEY_ACTION_UP, AKEYCODE_BACK), 0);
      g_start_pause_retry_down = 0;
      debugPrintf("[start709] B resync released automatic retry Back key\n");
    }
    if (g_live_n > 0)
      debugPrintf("[input704] CANCEL live gesture before Back navigation\n");
    live_cancel_all(inject, env, thiz, "R3.70.4 Back navigation");
    sz_store_b_consumed = sz_store_try_close();
    if (!sz_store_b_consumed) {
      extern int sz_settings_is_open(void);
      if (sz_settings_is_open()) {
        extern void nxp_request_settings_back(void);
        /* R3.64: queue the arrow through nx_pointer. Do NOT call sz_ui_tap()
         * here: direct injection from the B handler poisoned Settings input
         * state on hardware. */
        nxp_request_settings_back();
        sz_store_b_consumed = 1;
        debugPrintf("[settings64] B -> queued normal Settings back-arrow click\n");
      } else {
      extern int sz_store_menu_take_b(void);
      if (sz_store_menu_take_b()) {
        /* Same top-left arrow used successfully by touch. */
        sz_ui_tap(inject, env, thiz,
                  (float)g_w * 0.070f, (float)g_h * 0.130f);
        sz_store_b_consumed = 1;
      } else {
        inject(env, thiz, unity_keyevent(AKEY_ACTION_DOWN, AKEYCODE_BACK), 0);
      }
      }
    }
  }
  if (bup & HidNpadButton_B){
    if (!sz_store_b_consumed)
      inject(env, thiz, unity_keyevent(AKEY_ACTION_UP, AKEYCODE_BACK), 0);
    sz_store_b_consumed = 0;
  }
}

/* ==========================================================================
 * Cursor overlay. nx_pointer draws the cursor (built-in arrow, or cursor.png
 * if one is on the SD card) and saves/restores the GL state it touches; the
 * wrapper here supplies the two things it cannot know from inside the library.
 * Called by the swap wrapper in imports.c, right before eglSwapBuffers.
 * ========================================================================== */
void android_native_draw_cursor(void){
  if (!nxp_cursor_visible()) return;

  /* 1. VAO. Unity leaves one of its own vertex-array objects bound, and under
   *    GLES3 a non-zero VAO forbids client-side vertex arrays -- which is
   *    exactly what the cursor draws with, so glVertexAttribPointer would raise
   *    INVALID_OPERATION and nothing would appear. Binding VAO 0 for the
   *    duration also means every attribute change lands in a scratch VAO
   *    instead of Unity's, so restoring the binding restores it exactly.
   * 2. Viewport. The cursor shader maps render-space pixels straight to NDC, so
   *    it needs the viewport to cover the whole window; the engine may well
   *    have left it set to some intermediate render target. */
  GLint prev_vao = 0, vp[4];
  glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prev_vao);
  glGetIntegerv(GL_VIEWPORT, vp);

  glBindVertexArray(0);
  glViewport(0, 0, (GLsizei)g_w, (GLsizei)g_h);

  /* R3.70.21: visual state must not depend on cursor visibility. */
  extern void nxp_r362_visual_poll(void);
  nxp_r362_visual_poll();

  nxp_draw();

  glViewport(vp[0], vp[1], vp[2], vp[3]);
  glBindVertexArray((GLuint)prev_vao);
}
