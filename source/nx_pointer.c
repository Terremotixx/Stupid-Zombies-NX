/* nx_pointer.c -- see nx_pointer.h.
 *
 * Everything pointer-related for a Switch port of an Android game, in one file:
 * touchscreen, USB mouse, stick cursor, and the GL overlay that draws it.
 */
#include <switch.h>
#include <GLES2/gl2.h>
#include <stdio.h>
#include <stdarg.h>   /* va_list / va_start / va_end in logf_() */
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <setjmp.h>  /* png_jmpbuf / setjmp in the libpng error path */
#include <png.h>

#include "nx_pointer.h"

extern volatile unsigned long g_sz_last_frame_draws;

/* ------------------------------------------------------------------ config */

static NxpConfig s_cfg;
static int       s_ready = 0;

static void logf_(const char *fmt, ...) {
  if (!s_cfg.log) return;
  char buf[256];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  s_cfg.log(buf);
}

/* ------------------------------------------------------------------- state */

static PadState s_pad;
static int   s_rotation    = 0;   /* 0 none, 1 ROT90 CW, 2 ROT270 CCW */
static int   s_handle_touch = 1;

/* touch (handheld) */
static int   s_touch_active[16];
static float s_touch_x[16], s_touch_y[16];
/* R3.27: one real frame of zero-delta touch before UP. */
static unsigned char s_touch_release_wait[16];

/* cursor */
static float s_cx, s_cy;
static int   s_visible   = -1;      /* -1 = not yet decided */
static int   s_was_docked = -1;
static int   s_tap_prev  = 0;

/* R3.70.8 START fire quarantine.
 * nx_pointer owns a separate PadState from android_native_unity.  When START
 * opens Pause, abort only the controller-generated A/ZR/ZL tap state here so a
 * later containment/overlay transition cannot synthesize an NXP_UP (shot). */
static int   s_r3708_start_fire_quarantine = 0;

/* R3.62: actual visible result overlay (You Failed / Level Cleared). */
static volatile int s_r362_result_visual = 0;
static int s_r362_result_score = 0;

/* R3.69: Settings close confirmation.  Read-only public view of the
 * framebuffer detector; no change to R3.62 detection itself. */
int nxp_r362_result_visual_is_on(void) {
  return s_r362_result_visual ? 1 : 0;
}

/* R3.70.23: a newly-created Level can never already be in our START pause
 * state. Clear the temporal framebuffer detector too, otherwise a Pause/result
 * visual from the previous Level can survive long enough to reject START. */
void nxp_r362_visual_reset(void) {
  s_r362_result_score = 0;
  s_r362_result_visual = 0;
}

/* R3.45: a press begun on static UI may not leak to the next screen. */
static int   s_static_ui_press = 0;
static int   s_last_ui_class = -1;       /* A or mouse-left held last frame */
static int   s_cursor_release_wait = 0;
static float s_cursor_release_x = 0.0f, s_cursor_release_y = 0.0f;
static int   s_cursor_unlocked = 1; /* false until startup frame delay ends */
static int   s_cursor_delay_count = 0;

/* R3.27: hidden one-finger drag for analog-stick edge panning.
 * Real fingers use 0..7 and the visible cursor uses 8; UI_MAX_POINTERS is 10,
 * so pointer id 9 is the last valid slot. */
#define EDGE_PAN_ID 9
static int   s_edge_pan_active = 0;
static int   s_edge_pan_dir = 0;
static int   s_edge_pan_frames = 0;
static float s_edge_pan_x = 0.0f;
static float s_edge_pan_y = 0.0f;

/* tunables, adjusted live */
static float s_stick_speed;         /* px/frame at full stick deflection */
static float s_mouse_sens;          /* multiplier on mouse deltas */
static float s_gyro_sens;           /* multiplier on gyro angular velocity */

/* gyro (motion pointing). Handles differ per controller style, so we fetch one
 * for each and pick at read time based on what is actually connected. */
static HidSixAxisSensorHandle s_six[4];
static int s_gyro_ready = 0;        /* sensors started */
static int s_gyro_on    = 0;        /* toggled by '-' */
static int s_gyro_logged = 0;

/* A USB mouse takes priority: gyro is switched off while one is connected. */
static int s_mouse_connected = 0;

/* R3.35: mouse overlay auto-hide. */
#define MOUSE_HIDE_FRAMES 120
static int s_mouse_activity_this_frame = 0;
static int s_mouse_idle_frames = 0;
static int s_mouse_auto_hidden = 0;
static int s_pointer_owner_mouse = 0;

/* Raw angular velocity -> px/frame. The gyro reports a RATE, so this is the
 * per-frame gain; s_gyro_sens scales it and D-pad U/D tunes that live.
 * Measured on hardware: a normal turn gives |angular_velocity| ~0.14, so at the
 * original gain of 40 that was only ~6 px/frame -- far too slow. Tripled. */
#define GYRO_GAIN 120.0f

/* d-pad auto-repeat for the sensitivity adjustment */
static int   s_dpad_hold = 0;

/* Settings persistence. A save is queued whenever a sensitivity changes and
 * committed 3s after the LAST change, so holding the D-pad through twenty steps
 * writes the file once rather than twenty times. */
#define SETTINGS_DEBOUNCE_NS  3000000000ULL      /* 3 seconds */
static int s_settings_dirty = 0;
/* R3.64: one-shot Settings back click requested by physical B. */
static volatile int s_r364_settings_back_pending = 0;
static u64 s_settings_tick  = 0;

/* events for this frame */
static NxpEvent s_ev[24];
static int      s_nev;

#define STICK_MIN   2.0f
#define STICK_MAX  60.0f
#define SENS_MIN    0.25f
#define SENS_MAX    8.0f
#define GYRO_MIN    0.10f
#define GYRO_MAX    8.0f

static int is_docked(void) {
  return appletGetOperationMode() == AppletOperationMode_Console;
}

/* ------------------------------------------------------- settings file ---
 * IMPORTANT: use the port's locked fopen/fclose when it supplies them. The
 * engine's worker threads are doing file I/O constantly, and devkitPro's newlib
 * handle table is not thread-safe -- an unlocked open/close from here would race
 * it and corrupt the fd table (which shows up as a Data Abort deep inside
 * newlib, nowhere near this code). */
static FILE *cfg_fopen(const char *path, const char *mode) {
  return s_cfg.fopen_fn ? s_cfg.fopen_fn(path, mode) : fopen(path, mode);
}
static int cfg_fclose(FILE *f) {
  return s_cfg.fclose_fn ? s_cfg.fclose_fn(f) : fclose(f);
}

static void clamp_settings(void) {
  if (s_stick_speed < STICK_MIN) s_stick_speed = STICK_MIN;
  if (s_stick_speed > STICK_MAX) s_stick_speed = STICK_MAX;
  if (s_mouse_sens  < SENS_MIN)  s_mouse_sens  = SENS_MIN;
  if (s_mouse_sens  > SENS_MAX)  s_mouse_sens  = SENS_MAX;
  if (s_gyro_sens   < GYRO_MIN)  s_gyro_sens   = GYRO_MIN;
  if (s_gyro_sens   > GYRO_MAX)  s_gyro_sens   = GYRO_MAX;
}

static void settings_path(char *out, size_t n) {
  snprintf(out, n, "%s/pointer_r335.cfg", s_cfg.data_dir ? s_cfg.data_dir : ".");
}

static void settings_load(void) {
  if (!s_cfg.data_dir) return;
  char path[512];
  settings_path(path, sizeof path);

  FILE *f = cfg_fopen(path, "r");
  if (!f) { logf_("nxp: no pointer.cfg -- using defaults\n"); return; }

  char line[128];
  while (fgets(line, sizeof line, f)) {
    float v;
    if      (sscanf(line, "stick=%f", &v) == 1) s_stick_speed = v;
    else if (sscanf(line, "mouse=%f", &v) == 1) s_mouse_sens  = v;
    else if (sscanf(line, "gyro=%f",  &v) == 1) s_gyro_sens   = v;
  }
  cfg_fclose(f);
  clamp_settings();                      /* the file may have been hand-edited */
  logf_("nxp: settings loaded  stick=%.1f mouse=%.2f gyro=%.2f\n",
        s_stick_speed, s_mouse_sens, s_gyro_sens);
}

void nxp_save_settings(void) {
  if (!s_ready || !s_cfg.data_dir) return;
  char path[512];
  settings_path(path, sizeof path);

  FILE *f = cfg_fopen(path, "w");
  if (!f) { logf_("nxp: could not write %s\n", path); s_settings_dirty = 0; return; }

  fprintf(f, "# nx_pointer settings -- auto-saved, safe to edit\n");
  fprintf(f, "stick=%.2f\n", s_stick_speed);
  fprintf(f, "mouse=%.2f\n", s_mouse_sens);
  fprintf(f, "gyro=%.2f\n",  s_gyro_sens);
  cfg_fclose(f);

  s_settings_dirty = 0;
  logf_("nxp: settings saved  stick=%.1f mouse=%.2f gyro=%.2f\n",
        s_stick_speed, s_mouse_sens, s_gyro_sens);
}

/* Queue a save; it lands SETTINGS_DEBOUNCE_NS after the last change. */
static void settings_touch(void) {
  s_settings_dirty = 1;
  s_settings_tick  = armGetSystemTick();
}

/* Called by android_native_unity when B is pressed while Settings is open.
 * The actual click is emitted by the next nxp_update(), so it follows the
 * ordinary nx_pointer -> android_native event path. */
void nxp_request_settings_back(void) {
  s_r364_settings_back_pending = 1;
}


/* Called once per frame from nxp_update(). */
static void settings_tick(void) {
  if (!s_settings_dirty) return;
  if (armTicksToNs(armGetSystemTick() - s_settings_tick) >= SETTINGS_DEBOUNCE_NS)
    nxp_save_settings();
}

/* --------------------------------------------------- custom cursor (PNG) */

#define CURSOR_MAX_DIM 1024
#define CURSOR_POINTER 0
#define CURSOR_GRAB    1
#define CURSOR_IMAGES  2

/* Raw file bytes, slurped at init (single-threaded) so the render thread never
 * has to touch the filesystem. Decoded + uploaded lazily in nxp_draw(). */
static uint8_t *s_png_bytes[CURSOR_IMAGES];
static size_t   s_png_len[CURSOR_IMAGES];

/* decoded */
static GLuint s_cursor_tex[CURSOR_IMAGES];
static int    s_cursor_w[CURSOR_IMAGES], s_cursor_h[CURSOR_IMAGES];
static float  s_cursor_visible_hot_x[CURSOR_IMAGES];
static float  s_cursor_visible_hot_y[CURSOR_IMAGES];
static int    s_cursor_visible_hot_valid[CURSOR_IMAGES];
static int    s_png_tried = 0;      /* decode attempted (success or not) */

static const char *cursor_filename(int which) {
  return which == CURSOR_GRAB ? "cursor_grab.png" : "cursor_pointer.png";
}

static void slurp_cursor_png(int which) {
  if (!s_cfg.data_dir) return;
  char path[512];
  const char *name = cursor_filename(which);
  snprintf(path, sizeof path, "%s/%s", s_cfg.data_dir, name);

  FILE *f = cfg_fopen(path, "rb");     /* locked path: engine threads are live */
  if (!f) { logf_("nxp: no %s%s\n", name,
                  which == CURSOR_POINTER ? " (using built-in arrow)" : ""); return; }

  fseek(f, 0, SEEK_END);
  long len = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (len <= 0 || len > 4 * 1024 * 1024) { cfg_fclose(f); return; }

  s_png_bytes[which] = malloc((size_t)len);
  if (!s_png_bytes[which]) { cfg_fclose(f); return; }
  s_png_len[which] = fread(s_png_bytes[which], 1, (size_t)len, f);
  cfg_fclose(f);

  if (s_png_len[which] != (size_t)len) {
    free(s_png_bytes[which]); s_png_bytes[which] = NULL; s_png_len[which] = 0; return;
  }
  logf_("nxp: %s loaded (%zu bytes), decoding after startup\n",
        name, s_png_len[which]);
}

/* libpng reader over the in-memory buffer */
typedef struct { const uint8_t *p; size_t len, off; } PngSrc;

static void png_read_mem(png_structp png, png_bytep out, png_size_t n) {
  PngSrc *s = (PngSrc *)png_get_io_ptr(png);
  if (s->off + n > s->len) { png_error(png, "short read"); return; }
  memcpy(out, s->p + s->off, n);
  s->off += n;
}

/* Decode to RGBA8 and upload. Returns 1 on success. Render thread only. */
static int cursor_upload_png(int which) {
  if (!s_png_bytes[which]) return 0;
  const char *name = cursor_filename(which);

  png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
  if (!png) return 0;
  png_infop info = png_create_info_struct(png);
  if (!info) { png_destroy_read_struct(&png, NULL, NULL); return 0; }

  uint8_t   *pixels = NULL;
  png_bytep *rows   = NULL;
  if (setjmp(png_jmpbuf(png))) {          /* libpng error path */
    free(pixels); free(rows);
    png_destroy_read_struct(&png, &info, NULL);
    logf_("nxp: %s decode failed%s\n", name,
          which == CURSOR_POINTER ? " -- using built-in arrow" : "");
    return 0;
  }

  PngSrc src = { s_png_bytes[which], s_png_len[which], 0 };
  png_set_read_fn(png, &src, png_read_mem);
  png_read_info(png, info);

  const png_uint_32 w = png_get_image_width(png, info);
  const png_uint_32 h = png_get_image_height(png, info);
  if (w == 0 || h == 0 || w > CURSOR_MAX_DIM || h > CURSOR_MAX_DIM) {
    logf_("nxp: %s is %ux%u -- max is %dx%d%s\n", name,
          (unsigned)w, (unsigned)h, CURSOR_MAX_DIM, CURSOR_MAX_DIM,
          which == CURSOR_POINTER ? ", using built-in arrow" : "");
    png_destroy_read_struct(&png, &info, NULL);
    return 0;
  }

  /* normalise anything to 8-bit RGBA so transparency always works */
  const int ct = png_get_color_type(png, info);
  const int bd = png_get_bit_depth(png, info);
  if (bd == 16)                       png_set_strip_16(png);
  if (ct == PNG_COLOR_TYPE_PALETTE)   png_set_palette_to_rgb(png);
  if (ct == PNG_COLOR_TYPE_GRAY && bd < 8) png_set_expand_gray_1_2_4_to_8(png);
  if (png_get_valid(png, info, PNG_INFO_tRNS)) png_set_tRNS_to_alpha(png);
  if (ct == PNG_COLOR_TYPE_GRAY || ct == PNG_COLOR_TYPE_GRAY_ALPHA)
    png_set_gray_to_rgb(png);
  /* ensure an alpha channel exists even for opaque RGB */
  png_set_filler(png, 0xFF, PNG_FILLER_AFTER);
  png_read_update_info(png, info);

  const size_t stride = (size_t)w * 4;
  pixels = malloc(stride * h);
  rows   = malloc(sizeof(png_bytep) * h);
  if (!pixels || !rows) { png_error(png, "oom"); }
  for (png_uint_32 y = 0; y < h; y++) rows[y] = pixels + y * stride;
  png_read_image(png, rows);
  png_read_end(png, NULL);

  /* R3.37: visible-pixel centre for the pressed/aim cursor. */
  if (which == CURSOR_GRAB) {
    int min_x = (int)w, min_y = (int)h, max_x = -1, max_y = -1;
    for (png_uint_32 yy = 0; yy < h; yy++) {
      const uint8_t *row = pixels + (size_t)yy * stride;
      for (png_uint_32 xx = 0; xx < w; xx++) {
        if (row[(size_t)xx * 4 + 3] > 8) {
          if ((int)xx < min_x) min_x = (int)xx;
          if ((int)xx > max_x) max_x = (int)xx;
          if ((int)yy < min_y) min_y = (int)yy;
          if ((int)yy > max_y) max_y = (int)yy;
        }
      }
    }
    if (max_x >= min_x && max_y >= min_y) {
      s_cursor_visible_hot_x[which] = ((float)min_x + (float)max_x + 1.0f) * 0.5f;
      s_cursor_visible_hot_y[which] = ((float)min_y + (float)max_y + 1.0f) * 0.5f;
      s_cursor_visible_hot_valid[which] = 1;
      logf_("nxp: grab visible hotspot=(%.1f,%.1f) alpha-box=%d,%d..%d,%d\n",
            s_cursor_visible_hot_x[which], s_cursor_visible_hot_y[which],
            min_x, min_y, max_x, max_y);
    }
  }
  png_destroy_read_struct(&png, &info, NULL);
  free(rows);

  /* Save the texture binding we are about to disturb -- this runs inside the
   * engine's context, and leaving its texture unbound would corrupt its frame. */
  GLint prev_active = 0, prev_tex = 0;
  glGetIntegerv(GL_ACTIVE_TEXTURE, &prev_active);
  glActiveTexture(GL_TEXTURE0);
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex);

  glGenTextures(1, &s_cursor_tex[which]);
  glBindTexture(GL_TEXTURE_2D, s_cursor_tex[which]);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  GLint prev_align = 4;
  glGetIntegerv(GL_UNPACK_ALIGNMENT, &prev_align);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei)w, (GLsizei)h, 0,
               GL_RGBA, GL_UNSIGNED_BYTE, pixels);
  glPixelStorei(GL_UNPACK_ALIGNMENT, prev_align);   /* engine uploads assume 4 */
  free(pixels);

  glBindTexture(GL_TEXTURE_2D, (GLuint)prev_tex);   /* put the engine's back */
  glActiveTexture((GLenum)prev_active);

  s_cursor_w[which] = (int)w;
  s_cursor_h[which] = (int)h;
  logf_("nxp: %s %dx%d ready (tex=%u)\n", name,
        s_cursor_w[which], s_cursor_h[which], s_cursor_tex[which]);
  return 1;
}

/* ------------------------------------------------------------------- init */

void nxp_init(const NxpConfig *cfg) {
  if (s_ready) return;
  memset(&s_cfg, 0, sizeof s_cfg);
  if (cfg) s_cfg = *cfg;

  if (s_cfg.panel_w <= 0)         s_cfg.panel_w = 1280;
  if (s_cfg.panel_h <= 0)         s_cfg.panel_h = 720;
  s_rotation     = s_cfg.rotation;
  s_handle_touch = s_cfg.handle_touch;
  if (s_cfg.cursor_id <= 0)       s_cfg.cursor_id = 8;
  if (s_cfg.max_touch_slots <= 0) s_cfg.max_touch_slots = 8;
  if (s_cfg.screen_w <= 0)        s_cfg.screen_w = 1920;
  if (s_cfg.screen_h <= 0)        s_cfg.screen_h = 1080;
  if (s_cfg.cursor_scale <= 0.0f) s_cfg.cursor_scale = 1.0f;
  if (s_cfg.cursor_margin_left < 0.0f) s_cfg.cursor_margin_left = 0.0f;
  if (s_cfg.cursor_margin < 0.0f)      s_cfg.cursor_margin = 0.0f;
  if (s_cfg.cursor_delay_frames < 0)   s_cfg.cursor_delay_frames = 0;
  s_cursor_unlocked = (s_cfg.cursor_delay_frames == 0);
  if (!s_cursor_unlocked) s_visible = 0;

  s_stick_speed = (s_cfg.stick_speed > 0.0f) ? s_cfg.stick_speed : 14.0f;
  s_mouse_sens  = (s_cfg.mouse_sens  > 0.0f) ? s_cfg.mouse_sens  : 1.0f;
  s_gyro_sens   = 1.0f;

  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  padInitializeDefault(&s_pad);
  hidInitializeTouchScreen();
  hidInitializeMouse();               /* USB / dock mouse */

  /* Six-axis (gyro): each controller style has its own handle, so grab them all
   * and choose the right one per frame from the active style. */
  Result rc0 = hidGetSixAxisSensorHandles(&s_six[0], 1, HidNpadIdType_Handheld, HidNpadStyleTag_NpadHandheld);
  Result rc1 = hidGetSixAxisSensorHandles(&s_six[1], 1, HidNpadIdType_No1,      HidNpadStyleTag_NpadFullKey);
  Result rc2 = hidGetSixAxisSensorHandles(&s_six[2], 2, HidNpadIdType_No1,      HidNpadStyleTag_NpadJoyDual);
  if (R_SUCCEEDED(rc0) && R_SUCCEEDED(rc1) && R_SUCCEEDED(rc2)) {
    for (int i = 0; i < 4; i++) hidStartSixAxisSensor(s_six[i]);
    s_gyro_ready = 1;
  } else {
    logf_("nxp: gyro unavailable (sensor handles failed)\n");
  }

  s_cx = s_cfg.screen_w * 0.5f;
  s_cy = s_cfg.screen_h * 0.5f;

  settings_load();                    /* restores stick/mouse/gyro sensitivities */
  slurp_cursor_png(CURSOR_POINTER);   /* read now, decode after startup on render */
  slurp_cursor_png(CURSOR_GRAB);

  s_ready = 1;
  logf_("nxp: init %dx%d (panel %dx%d), stick=%.1f mouse=%.2f\n",
        s_cfg.screen_w, s_cfg.screen_h, s_cfg.panel_w, s_cfg.panel_h,
        s_stick_speed, s_mouse_sens);
}

/* ---------------------------------------------------------------- helpers */

static void clamp_cursor(void) {
  float min_x = s_cfg.cursor_margin_left;
  float min_y = s_cfg.cursor_margin;
  float max_x = (float)(s_cfg.screen_w - 1) - s_cfg.cursor_margin;
  float max_y = (float)(s_cfg.screen_h - 1) - s_cfg.cursor_margin;
  if (min_x < s_cfg.cursor_margin) min_x = s_cfg.cursor_margin;
  if (max_x < min_x) min_x = max_x = s_cfg.screen_w * 0.5f;
  if (max_y < min_y) min_y = max_y = s_cfg.screen_h * 0.5f;
  if (s_cx < min_x) s_cx = min_x;
  if (s_cy < min_y) s_cy = min_y;
  if (s_cx > max_x) s_cx = max_x;
  if (s_cy > max_y) s_cy = max_y;
}

static void push(int id, float x, float y, int phase) {
  if (s_nev >= (int)(sizeof s_ev / sizeof s_ev[0])) return;
  NxpEvent *e = &s_ev[s_nev++];
  e->id = id; e->x = x; e->y = y; e->phase = phase;
}

static int any_real_touch_active(void) {
  const int n = s_cfg.max_touch_slots < 16 ? s_cfg.max_touch_slots : 16;
  for (int i = 0; i < n; i++)
    if (s_touch_active[i]) return 1;
  return 0;
}

static void edge_pan_stop(void) {
  if (!s_edge_pan_active) {
    s_edge_pan_dir = 0;
    s_edge_pan_frames = 0;
    return;
  }
  push(EDGE_PAN_ID, s_edge_pan_x, s_edge_pan_y, NXP_MOVE);
  push(EDGE_PAN_ID, s_edge_pan_x, s_edge_pan_y, NXP_UP);
  s_edge_pan_active = 0;
  s_edge_pan_dir = 0;
  s_edge_pan_frames = 0;
}

static void edge_pan_update(int dir, float analog_mag) {
  /* The only controller-generated drag allowed in UI:
   * side navigation on the actual 3-draw level list. */
  if (g_sz_last_frame_draws != 3) {
    edge_pan_stop();
    return;
  }

  if (analog_mag < 0.0f) analog_mag = -analog_mag;
  if (analog_mag > 1.0f) analog_mag = 1.0f;

  if (!s_edge_pan_active || s_edge_pan_dir != dir) {
    if (s_edge_pan_active) edge_pan_stop();
    s_edge_pan_active = 1;
    s_edge_pan_dir = dir;
    s_edge_pan_frames = 0;
    s_edge_pan_x = s_cfg.screen_w * 0.50f;
    s_edge_pan_y = s_cfg.screen_h * 0.52f;
    push(EDGE_PAN_ID, s_edge_pan_x, s_edge_pan_y, NXP_DOWN);
    return;
  }

  if (s_edge_pan_frames < 90) s_edge_pan_frames++;
  const float ramp = (float)s_edge_pan_frames / 90.0f;

  float strength = (analog_mag - 0.35f) / 0.65f;
  if (strength < 0.0f) strength = 0.0f;
  if (strength > 1.0f) strength = 1.0f;

  /* R3.47: slightly faster level-list edge scroll (+25% vs R3.46v2).
   * 5.0 -> 17.5 px/frame instead of 4.0 -> 14.0. */
  const float speed = (5.0f + 12.5f * ramp) * (0.65f + 0.35f * strength);
  s_edge_pan_x += (dir > 0) ? -speed : speed;
  push(EDGE_PAN_ID, s_edge_pan_x, s_edge_pan_y, NXP_MOVE);
}

/* Touch: the panel always reports in its own space (1280x720) regardless of the
 * resolution we render at, so scale into render space -- otherwise the right and
 * bottom edges (and the corners) are physically unreachable. */
/* Rotate a physical device delta (panel screen coords: +x right, +y down) into
 * render space, matching the compositor transform the host applied. Derived to
 * agree with the touch position mapping below and the port's proven touch remap:
 *   ROT90 CW  (1): (dgx,dgy) = ( dpy, -dpx)
 *   ROT270 CCW(2): (dgx,dgy) = (-dpy,  dpx)
 *   none      (0): unchanged
 * `rot` is passed per-device: a CONTROLLER that is physically rotated with the
 * screen (handheld/attached) needs the display rotation; a controller held
 * normally (detached Joy-Cons, docked) and a USB MOUSE (a desk device that never
 * turns with the console) must NOT be rotated -- pass 0 for those. */
static void nxp_rot_delta(int rot, float dpx, float dpy, float *dgx, float *dgy) {
  switch (rot) {
    case 1:  *dgx =  dpy; *dgy = -dpx; break;
    case 2:  *dgx = -dpy; *dgy =  dpx; break;
    default: *dgx =  dpx; *dgy =  dpy; break;
  }
}

/* Rotation to apply to STICK/GYRO input: only when the display is rotated AND the
 * controller is the attached handheld one (which turns with the screen). Detached
 * Joy-Cons and docked controllers are held upright, so they map straight through.
 * padIsHandheld() reports whether this frame's input came from the console's
 * built-in (attached) controller. */
static int nxp_ctrl_rot(void) {
  return (s_rotation && padIsHandheld(&s_pad)) ? s_rotation : 0;
}

static void do_touch(void) {
  if (!s_handle_touch) return;
  HidTouchScreenState ts = {0};
  hidGetTouchScreenStates(&ts, 1);

  const int slots = s_cfg.max_touch_slots;
  int now[16] = {0};
  int count = ts.count > slots ? slots : ts.count;

  /* panel(px,py) landscape -> render(x,y). With rotation the render axes swap;
   * scale each render axis by the panel axis that feeds it. */
  const float sw = (float)s_cfg.screen_w, sh = (float)s_cfg.screen_h;
  const float pw = (float)s_cfg.panel_w,  ph = (float)s_cfg.panel_h;

  for (int i = 0; i < count; i++) {
    float px = (float)ts.touches[i].x, py = (float)ts.touches[i].y;
    float x, y;
    if      (s_rotation == 1) { x =        py * (sw/ph); y = (pw-px) * (sh/pw); }
    else if (s_rotation == 2) { x = (ph-py) * (sw/ph); y =     px  * (sh/pw); }
    else                      { x =   px * (sw/pw);     y =     py  * (sh/ph); }
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x > s_cfg.screen_w - 1) x = (float)(s_cfg.screen_w - 1);
    if (y > s_cfg.screen_h - 1) y = (float)(s_cfg.screen_h - 1);
    now[i] = 1;
    push(i, x, y, s_touch_active[i] ? NXP_MOVE : NXP_DOWN);
    s_touch_x[i] = x; s_touch_y[i] = y;
  }
  for (int i = 0; i < slots; i++) {
    if (now[i]) {
      s_touch_release_wait[i] = 0;
      s_touch_active[i] = 1;
      continue;
    }

    if (s_touch_active[i]) {
      if (!s_touch_release_wait[i]) {
        /* First frame after physical release: keep the logical Android pointer
         * down for one extra 60-Hz frame at exactly the same coordinates. */
        push(i, s_touch_x[i], s_touch_y[i], NXP_MOVE);
        s_touch_release_wait[i] = 1;
        s_touch_active[i] = 1;
      } else {
        /* Second absent frame: stationary sample, then final release. */
        push(i, s_touch_x[i], s_touch_y[i], NXP_MOVE);
        push(i, s_touch_x[i], s_touch_y[i], NXP_UP);
        s_touch_release_wait[i] = 0;
        s_touch_active[i] = 0;
      }
    } else {
      s_touch_release_wait[i] = 0;
    }
  }
}

/* D-pad UP/DOWN adjusts sensitivity. That is the ONLY sensitivity control --
 * left/right used to also tune the stick, which just meant two buttons doing
 * overlapping jobs and no clear answer to "which one am I changing?".
 *
 * Up/Down applies to whichever device is ACTUALLY driving the cursor right now:
 *   a mouse is plugged in  -> mouse sensitivity
 *   else gyro is on        -> gyro sensitivity
 *   else                   -> stick speed
 * so it is never a no-op, and there is only ever one thing it can mean.
 * Auto-repeats while held. Any change queues a debounced save. */
static void do_dpad(u64 held, u64 pressed) {
  const u64 any = HidNpadButton_Up | HidNpadButton_Down;

  int repeat = 0;
  if (held & any) {
    s_dpad_hold++;
    if (s_dpad_hold > 24 && (s_dpad_hold % 3) == 0) repeat = 1;   /* ~0.4s, then repeat */
  } else {
    s_dpad_hold = 0;
  }

  int step = 0;
  if (pressed & HidNpadButton_Up)   step = +1;
  if (pressed & HidNpadButton_Down) step = -1;
  if (repeat && (held & HidNpadButton_Up))   step = +1;
  if (repeat && (held & HidNpadButton_Down)) step = -1;
  if (!step) return;

  const float f = (step > 0) ? 1.15f : (1.0f / 1.15f);

  if (s_mouse_connected) {
    s_mouse_sens *= f;
    clamp_settings();
    logf_("nxp: mouse sensitivity = %.2f\n", s_mouse_sens);
  } else if (s_gyro_on) {
    s_gyro_sens *= f;
    clamp_settings();
    logf_("nxp: gyro sensitivity = %.2f\n", s_gyro_sens);
  } else {
    s_stick_speed *= f;
    clamp_settings();
    logf_("nxp: stick speed = %.1f px/frame\n", s_stick_speed);
  }

  settings_touch();                 /* save 3s after the last change */
}

/* Pick the six-axis handle that matches whatever is actually connected, then
 * read its latest sample. Handheld, Pro Controller and dual Joy-Con all report
 * through different handles. */
static int read_gyro(HidSixAxisSensorState *out, float *sign_x, float *sign_y) {
  if (!s_gyro_ready) return 0;
  const u64 style = padGetStyleSet(&s_pad);

  /* AXIS SIGNS.
   * The Pro Controller's IMU frame gives the correct pointing direction with
   * both axes negated. A Joy-Con's IMU sits rotated 180 degrees about its
   * pointing axis relative to that, so BOTH axes come out mirrored -- which is
   * exactly what shows up in practice (Pro correct, Joy-Con reversed on both).
   * Keep the signs in one place so a single controller can be re-flipped
   * without touching the rest of the maths. */
  *sign_x = -1.0f;
  *sign_y = -1.0f;                                  /* Pro Controller */

  if (style & HidNpadStyleTag_NpadFullKey)
    return hidGetSixAxisSensorStates(s_six[1], out, 1) > 0;

  if (style & HidNpadStyleTag_NpadHandheld) {
    *sign_x = +1.0f; *sign_y = +1.0f;               /* Joy-Con frame: mirrored */
    return hidGetSixAxisSensorStates(s_six[0], out, 1) > 0;
  }

  if (style & HidNpadStyleTag_NpadJoyDual) {
    *sign_x = +1.0f; *sign_y = +1.0f;               /* Joy-Con frame: mirrored */
    const u64 attr = padGetAttributes(&s_pad);
    if (attr & HidNpadAttribute_IsRightConnected)
      return hidGetSixAxisSensorStates(s_six[3], out, 1) > 0;
    if (attr & HidNpadAttribute_IsLeftConnected)
      return hidGetSixAxisSensorStates(s_six[2], out, 1) > 0;
  }
  return 0;
}

/* Motion pointing: tilt/turn the controller to move the cursor.
 *
 * The gyro reports an angular VELOCITY, so we treat it as a per-frame rate:
 *   yaw   (rotation about the controller's up axis)    -> cursor X
 *   pitch (rotation about the controller's right axis) -> cursor Y
 * Signs are negated so the cursor follows where you point the controller.
 * A connected mouse wins, so gyro is skipped entirely while one is plugged in. */
static void do_gyro(void) {
  if (!s_gyro_on || s_mouse_connected) return;

  HidSixAxisSensorState st = {0};
  float sx = -1.0f, sy = -1.0f;
  if (!read_gyro(&st, &sx, &sy)) return;

  if (s_gyro_logged < 5) {
    s_gyro_logged++;
    logf_("nxp: gyro av=(%.3f, %.3f, %.3f) signs=(%+.0f,%+.0f)\n",
          st.angular_velocity.x, st.angular_velocity.y, st.angular_velocity.z, sx, sy);
  }

  const float g = GYRO_GAIN * s_gyro_sens;
  const float dx = sx * st.angular_velocity.y * g;   /* yaw   -> horizontal */
  const float dy = sy * st.angular_velocity.x * g;   /* pitch -> vertical   */

  /* small deadzone so a resting controller does not creep */
  if (fabsf(dx) > 0.05f || fabsf(dy) > 0.05f) {
    float dgx, dgy; nxp_rot_delta(nxp_ctrl_rot(), dx, dy, &dgx, &dgy);
    s_cx += dgx;
    s_cy += dgy;
    clamp_cursor();
  }
}

/* USB mouse: relative motion drives the cursor; the wheel tunes sensitivity.
 *
 * IMPORTANT: read the whole sample buffer, not just the newest state. HID samples
 * faster than we render, and a wheel tick shows up in exactly ONE sample -- if we
 * only look at the latest one each frame, most ticks land in samples we never see
 * and the wheel appears dead. Accumulate every sample we have not processed yet
 * (tracked by sampling_number). */
static u64 s_mouse_seen = 0;
static int s_mouse_logged = 0;

static int do_mouse(void) {
  s_mouse_activity_this_frame = 0;
  HidMouseState st[16];
  int n = (int)hidGetMouseStates(st, 16);
  if (n <= 0) return 0;

  s32 dx = 0, dy = 0, wheel_y = 0, wheel_x = 0;
  u32 buttons = 0;
  u64 newest = s_mouse_seen, btn_sn = 0;

  for (int i = 0; i < n; i++) {
    const HidMouseState *m = &st[i];
    if (m->sampling_number > btn_sn) { btn_sn = m->sampling_number; buttons = m->buttons; }
    if (m->sampling_number <= s_mouse_seen) continue;      /* already handled */
    dx      += m->delta_x;
    dy      += m->delta_y;
    wheel_y += m->wheel_delta_y;
    wheel_x += m->wheel_delta_x;      /* some drivers put vertical scroll here */
    if (m->sampling_number > newest) newest = m->sampling_number;
  }

  /* Use whichever axis actually reports. On this hardware wheel_delta_y stayed 0
   * while the mouse otherwise worked perfectly, so don't assume either one. */
  s32 wheel = wheel_y ? wheel_y : wheel_x;

  /* Presence: trust the IsConnected attribute, but treat any real activity as
   * proof of a mouse too (in case a device never sets the flag). */
  u32 attrs = 0;
  for (int i = 0; i < n; i++)
    if (st[i].sampling_number == btn_sn) { attrs = st[i].attributes; break; }
  int conn = (attrs & HidMouseAttribute_IsConnected) ? 1 : 0;
  if (!conn && (dx || dy || wheel_x || wheel_y || buttons)) conn = 1;

  if (conn != s_mouse_connected) {
    s_mouse_connected = conn;
    logf_("nxp: mouse %s\n", conn ? "connected" : "disconnected");
    if (conn && s_gyro_on) {
      s_gyro_on = 0;                 /* a mouse takes priority over motion */
      logf_("nxp: gyro OFF (mouse connected)\n");
    }
  }

  if (s_mouse_seen == 0) {           /* first sight of the mouse: sync, don't jump */
    s_mouse_seen = newest;
    logf_("nxp: mouse detected (%d samples buffered)\n", n);
    return (buttons & HidMouseButton_Left) ? 1 : 0;
  }
  s_mouse_seen = newest;

  /* Log every event that carries a wheel value, plus a few movement samples, so
   * a dead wheel is immediately visible in the log rather than a mystery. */
  if ((wheel_x || wheel_y) || s_mouse_logged < 3) {
    if (s_mouse_logged < 40) {
      s_mouse_logged++;
      logf_("nxp: mouse dx=%d dy=%d wheel_y=%d wheel_x=%d buttons=0x%x\n",
            (int)dx, (int)dy, (int)wheel_y, (int)wheel_x, (unsigned)buttons);
    }
  }

  if (wheel != 0) {
    /* one notch = one step; several notches in a frame compound */
    for (int i = 0; i < (wheel > 0 ? wheel : -wheel) && i < 8; i++)
      s_mouse_sens *= (wheel > 0) ? 1.15f : (1.0f / 1.15f);
    if (s_mouse_sens < SENS_MIN) s_mouse_sens = SENS_MIN;
    if (s_mouse_sens > SENS_MAX) s_mouse_sens = SENS_MAX;
    logf_("nxp: mouse sensitivity = %.2f\n", s_mouse_sens);
  }

  if (dx || dy || wheel_x || wheel_y || buttons) {
    s_mouse_activity_this_frame = 1;
    s_pointer_owner_mouse = 1;
    s_mouse_idle_frames = 0;
    s_mouse_auto_hidden = 0;
  }

  if (dx || dy) {
    float dgx, dgy;
    nxp_rot_delta(0, (float)dx * s_mouse_sens, (float)dy * s_mouse_sens, &dgx, &dgy);  /* mouse: no rotation */
    s_cx += dgx; s_cy += dgy;
    clamp_cursor();
    if (s_cursor_unlocked && s_visible <= 0)
      s_visible = 1;                       /* moving the mouse brings it up */
  }

  return (buttons & HidMouseButton_Left) ? 1 : 0;
}

/* ----------------------------------------------------------------- update */

void nxp_update(void) {
  if (!s_ready) return;
  padUpdate(&s_pad);

  /* Do not draw over Unity's splash and multi-stage startup preload. The main
   * loop cannot advance this counter while nativeRender is blocked, so reaching
   * the configured frame means startup has returned and produced real frames. */
  if (!s_cursor_unlocked && ++s_cursor_delay_count >= s_cfg.cursor_delay_frames) {
    s_cursor_unlocked = 1;
    s_visible = 1;
    clamp_cursor();
    logf_("nxp: startup complete -- cursor enabled after %d frames\n",
          s_cursor_delay_count);
  }

  const int docked = is_docked();
  if (docked != s_was_docked) {
    /* docked has no touchscreen, so make sure there is always a way to point */
    if (!s_cursor_unlocked) s_visible = 0;
    else if (s_visible < 0 || docked) s_visible = docked ? 1 : 0;
    s_was_docked = docked;
  }

  const u64 held    = padGetButtons(&s_pad);
  const u64 pressed = padGetButtonsDown(&s_pad);

  /* R3.70.8: START and nx_pointer use independent PadState instances.
   * Kill the LOCAL controller-shot state before Pause changes the UI class.
   * Do NOT emit UP here: android_native_unity's R3.70.7 path sends
   * ACTION_CANCEL for the live Android pointer before issuing Back. */
  {
    const u64 fire_mask =
      HidNpadButton_A | HidNpadButton_ZR | HidNpadButton_ZL;

    if (pressed & HidNpadButton_Plus) {
      logf_("[start708] PLUS quarantine tap_prev=%d wait=%d fireheld=0x%llx\n",
            s_tap_prev, s_cursor_release_wait,
            (unsigned long long)(held & fire_mask));

      s_tap_prev = 0;
      s_cursor_release_wait = 0;
      s_static_ui_press = 0;
      s_r3708_start_fire_quarantine = 1;
    }

    /* Keep A/ZR/ZL suppressed only while START itself, or a fire button that
     * overlapped START, remains physically held. Touch/mouse are untouched. */
    if (s_r3708_start_fire_quarantine &&
        !(held & (fire_mask | HidNpadButton_Plus))) {
      s_r3708_start_fire_quarantine = 0;
      logf_("[start708] PLUS quarantine RELEASED\n");
    }
  }

  /* R3.35: PLUS/START is handled as Android Back/Pause. */
  /* R3.63: SELECT/MINUS toggles the virtual mouse/cursor.
   * This reuses the wrapper's proven cursor visibility mechanism. */
  if (s_cursor_unlocked && (pressed & HidNpadButton_Minus)) {
    s_visible = (s_visible > 0) ? 0 : 1;
    s_mouse_auto_hidden = 0;
    s_mouse_idle_frames = 0;
    logf_("nxp: cursor %s (SELECT)\n", s_visible > 0 ? "ON" : "OFF");
  }

  /* R3.63: R3/right-stick click owns the existing gyro toggle. */
  if (s_cursor_unlocked && (pressed & HidNpadButton_StickR)) {
    if (s_mouse_connected) {
      logf_("nxp: gyro stays OFF while a mouse is connected\n");
    } else if (!s_gyro_ready) {
      logf_("nxp: gyro unavailable on this controller\n");
    } else {
      s_gyro_on = !s_gyro_on;
      logf_("nxp: gyro %s (R3)\n", s_gyro_on ? "ON" : "OFF");
      if (s_gyro_on && s_visible <= 0) s_visible = 1;   /* pointing needs a cursor */
    }
  }

  /* L / R recenter the cursor to the middle of the screen. Handy with gyro:
   * point the controller where it's comfortable, recenter, and aim from there
   * (like recentering a VR view). Works in every mode. */
  if (s_cursor_unlocked && (pressed & (HidNpadButton_L | HidNpadButton_R))) {
    s_cx = s_cfg.screen_w * 0.5f;
    s_cy = s_cfg.screen_h * 0.5f;
    if (s_visible <= 0) s_visible = 1;    /* show it so you can see where it went */
    logf_("nxp: cursor recentered\n");
  }

  do_dpad(held, pressed);

  s_nev = 0;

  if (s_r364_settings_back_pending) {
    const float bx = (float)s_cfg.panel_w * 0.070f;
    const float by = (float)s_cfg.panel_h * 0.130f;

    /* Same logical top-left arrow area that works by real touch, but now the
     * event traverses the normal R3.61 Settings state machine. */
    push(s_cfg.cursor_id, bx, by, NXP_DOWN);
    push(s_cfg.cursor_id, bx, by, NXP_UP);

    s_r364_settings_back_pending = 0;
    logf_("nxp: R3.64 queued Settings back click %.1f,%.1f\n",
          (double)bx, (double)by);
  }

  if (!docked) do_touch();                 /* touchscreen: handheld only */

  /* Mouse first: it sets s_mouse_connected, which gates the gyro below. */
  const int mouse_tap = do_mouse();

  if (s_mouse_connected && s_pointer_owner_mouse) {
    if (s_mouse_activity_this_frame) {
      s_mouse_idle_frames = 0;
      s_mouse_auto_hidden = 0;
    } else {
      if (s_mouse_idle_frames < MOUSE_HIDE_FRAMES + 1) s_mouse_idle_frames++;
      if (s_mouse_idle_frames >= MOUSE_HIDE_FRAMES && !s_tap_prev)
        s_mouse_auto_hidden = 1;
    }
  } else {
    s_mouse_idle_frames = 0;
    s_mouse_auto_hidden = 0;
  }

  do_gyro();                               /* no-op if off, or if a mouse is in */

  if (s_visible > 0) {
    HidAnalogStickState ls = padGetStickPos(&s_pad, 0);
    /* R3.34: know whether we are aiming before moving the cursor. */
    /* R3.56: controller drag is valid only in active gameplay, not the level-entry
     * High Score/fade nor completed/failed result overlays. */
    static int s_r356_prev_ui_class = -1;
    static int s_r356_intro_guard = 0;
    static int s_r356_play_band_frames = 0;
    static int s_r356_gameplay_armed = 0;
    static int s_r356_modal_candidate_frames = 0;
    static int s_r356_modal_clear_frames = 0;
    static int s_r356_modal_lock = 0;

    const u64 controller_mask =
      HidNpadButton_A | HidNpadButton_ZR | HidNpadButton_ZL;
    const int controller_held =
      (!s_r3708_start_fire_quarantine && (held & controller_mask)) ? 1 : 0;
    const int controller_pressed =
      (!s_r3708_start_fire_quarantine && (pressed & controller_mask)) ? 1 : 0;

    /* R3.46v2:
     * 2 draws = main menu / STAGES
     * 3 draws = level list
     * ONLY controller buttons are made non-draggable on <=3-draw UI.
     * Touchscreen and mouse keep their original behaviour.
     */
    const unsigned long frame_draws = g_sz_last_frame_draws;
    const int button_static_ui = (frame_draws > 0 && frame_draws <= 3) ? 1 : 0;
    const int ui_class = (frame_draws <= 2) ? 0 : (frame_draws == 3 ? 1 : 2);

    /* R3.67: a held controller button must never continue across screens.
     * Pointer reset closes the OLD gesture; quarantine prevents the same
     * physical hold from creating input on the destination screen. */
    static int s_r367_prev_ui_class = -1;
    static int s_r367_hold_quarantine = 0;

    if (ui_class != s_r367_prev_ui_class) {
      if (s_r367_prev_ui_class >= 0 && controller_held) {
        s_r367_hold_quarantine = 1;

        if (s_tap_prev) {
          push(s_cfg.cursor_id, s_cx, s_cy, NXP_MOVE);
          push(s_cfg.cursor_id, s_cx, s_cy, NXP_MOVE);
          push(s_cfg.cursor_id, s_cx, s_cy, NXP_UP);
        }

        s_tap_prev = 0;
        s_cursor_release_wait = 0;
        s_static_ui_press = 1;

        logf_("nxp: R3.67 transition quarantine %d->%d held=1\n",
              s_r367_prev_ui_class, ui_class);
      }

      s_r367_prev_ui_class = ui_class;
    }

    if (s_r367_hold_quarantine && !controller_held) {
      s_r367_hold_quarantine = 0;
      s_static_ui_press = 0;
      s_tap_prev = 0;
      s_cursor_release_wait = 0;
      logf_("nxp: R3.67 transition quarantine RELEASED\n");
    }

    /* LEVELS -> DYNAMIC starts with the High Score / black-fade presentation. */
    if (ui_class != s_r356_prev_ui_class) {
      if (s_r356_prev_ui_class == 1 && ui_class == 2) {
        s_r356_intro_guard = 60;
        s_r356_play_band_frames = 0;
        s_r356_gameplay_armed = 0;
        s_r356_modal_candidate_frames = 0;
        s_r356_modal_clear_frames = 0;
        s_r356_modal_lock = 0;
        logf_("nxp: R3.56 level-entry drag guard ON\n");
      } else if (ui_class != 2) {
        s_r356_intro_guard = 0;
        s_r356_play_band_frames = 0;
        s_r356_gameplay_armed = 0;
        s_r356_modal_candidate_frames = 0;
        s_r356_modal_clear_frames = 0;
        s_r356_modal_lock = 0;
      }
      s_r356_prev_ui_class = ui_class;
    }

    if (ui_class == 2) {
      if (s_r356_intro_guard > 0)
        s_r356_intro_guard--;

      if (frame_draws > 0 && frame_draws <= 12 && !s_r356_gameplay_armed)
        s_r356_intro_guard = 20;

      if (!s_r356_gameplay_armed &&
          s_r356_intro_guard == 0 &&
          frame_draws >= 14 && frame_draws <= 19) {
        if (++s_r356_play_band_frames >= 30) {
          s_r356_gameplay_armed = 1;
          s_r356_play_band_frames = 30;
          logf_("nxp: R3.56 gameplay drag ARMED\n");
        }
      }

      if (s_r356_gameplay_armed) {
        const int modal_draw_pattern =
          (frame_draws > 0 && (frame_draws <= 13 || frame_draws >= 20)) ? 1 : 0;

        /* R3.57: weighted modal detector.
         * A Level Cleared / You Failed overlay can oscillate 19/20/21 draws,
         * so consecutive-frame counting misses it.  Suspicious frames add 2;
         * ordinary gameplay frames remove only 1. */
        if (!s_r356_modal_lock) {
          if (modal_draw_pattern) {
            if (s_r356_modal_candidate_frames < 20)
              s_r356_modal_candidate_frames += 2;
          } else if (s_r356_modal_candidate_frames > 0) {
            s_r356_modal_candidate_frames--;
          }

          if (s_r356_modal_candidate_frames >= 4) {
            s_r356_modal_lock = 1;
            s_r356_modal_clear_frames = 0;
            logf_("nxp: R3.57 result-overlay drag guard ON draws=%lu score=%d\n",
                  frame_draws, s_r356_modal_candidate_frames);
          }
        } else {
          /* Stay click-only until 30 clearly normal gameplay-looking frames.
           * Result screens therefore cannot inject any held drag/inertia. */
          if (frame_draws >= 14 && frame_draws <= 19) {
            if (++s_r356_modal_clear_frames >= 30) {
              s_r356_modal_lock = 0;
              s_r356_modal_clear_frames = 0;
              s_r356_modal_candidate_frames = 0;
              logf_("nxp: R3.57 result-overlay drag guard OFF\n");
            }
          } else {
            s_r356_modal_clear_frames = 0;
          }
        }
      }
    }

    if (ui_class != s_last_ui_class) {
      /* R3.49 containment reset:
       * When the rendered screen class changes, kill ONLY controller-generated
       * pointer state before the destination screen can inherit it.
       *
       * This does not touch the real touchscreen path. It also deliberately
       * quarantines A/ZR/ZL until physically released, so holding a shot/click
       * through a loading transition cannot immediately become a drag on the
       * next screen.
       */
      edge_pan_stop();

      if (controller_held || s_tap_prev) {
        /* Stationary brake samples + UP. This mirrors the proven controller
         * release path and gives Unity a zero-velocity end sample. */
        push(s_cfg.cursor_id, s_cx, s_cy, NXP_MOVE);
        push(s_cfg.cursor_id, s_cx, s_cy, NXP_MOVE);
        push(s_cfg.cursor_id, s_cx, s_cy, NXP_UP);
      }

      s_tap_prev = 0;
      s_cursor_release_wait = 0;
      s_static_ui_press = controller_held ? 1 : 0;

      logf_("nxp: UI transition %d->%d reset controller pointer (draws=%lu held=%d)\n",
            s_last_ui_class, ui_class, frame_draws, controller_held);
      logf_("nxp: UI class=%s draws=%lu\n",
            ui_class == 0 ? "STATIC" : (ui_class == 1 ? "LEVELS" : "DYNAMIC"),
            frame_draws);
      s_last_ui_class = ui_class;
    }

    extern int sz_settings_is_open(void);

    /* R3.59: outcome detection no longer uses draw-count guesses. */
    const int r356_click_only_ui =
      button_static_ui || sz_settings_is_open() ||
      (s_r356_intro_guard > 0) ||
      s_r362_result_visual;

    static int s_r356_prev_click_only = 0;
    if (r356_click_only_ui && !s_r356_prev_click_only &&
        (controller_held || s_tap_prev)) {
      push(s_cfg.cursor_id, s_cx, s_cy, NXP_MOVE);
      push(s_cfg.cursor_id, s_cx, s_cy, NXP_MOVE);
      push(s_cfg.cursor_id, s_cx, s_cy, NXP_UP);
      s_tap_prev = 0;
      s_cursor_release_wait = 0;
      s_static_ui_press = controller_held ? 1 : 0;
    }
    s_r356_prev_click_only = r356_click_only_ui;

    if (!controller_held)
      s_static_ui_press = 0;

    if (!s_r367_hold_quarantine &&
        r356_click_only_ui && controller_pressed && !s_static_ui_press) {
      /* UI overlays: one complete click only; NEVER a held drag. */
      push(s_cfg.cursor_id, s_cx, s_cy, NXP_DOWN);
      push(s_cfg.cursor_id, s_cx, s_cy, NXP_UP);
      s_static_ui_press = 1;
    }

    const int controller_drag =
      (!s_r367_hold_quarantine &&
       !r356_click_only_ui &&
       !s_static_ui_press &&
       controller_held) ? 1 : 0;

    const int tap = controller_drag | mouse_tap;
    const float aim_speed_scale =
      (!s_r367_hold_quarantine &&
       !r356_click_only_ui &&
       controller_held) ? 0.42f : 1.0f;

    if (ls.x || ls.y) {
      s_pointer_owner_mouse = 0;
      s_mouse_auto_hidden = 0;
      s_mouse_idle_frames = 0;
      const float cursor_speed = s_stick_speed * aim_speed_scale;
      float dgx, dgy;
      nxp_rot_delta(nxp_ctrl_rot(), (ls.x / 32767.0f) * cursor_speed,
                    -(ls.y / 32767.0f) * cursor_speed, &dgx, &dgy);
      s_cx += dgx; s_cy += dgy;
      clamp_cursor();
    }

    /* A, ZR and ZL all confirm/tap (ZL/ZR let you play one-handed), as does the
     * mouse's left button. */

    /* Analog-only edge pan: when the cursor is within the left/right 30%,
     * pushing the LEFT stick farther toward that side drags the level strip.
     * Touch and mouse movement never trigger this. */
    {
      const float sx = (float)ls.x / 32767.0f;
      const float edge = (float)s_cfg.screen_w * 0.30f;
      int pan_dir = 0;

      if (!tap && !any_real_touch_active() && fabsf(sx) >= 0.42f) {
        if (sx < 0.0f && s_cx <= edge)
          pan_dir = -1;
        else if (sx > 0.0f && s_cx >= (float)s_cfg.screen_w - edge)
          pan_dir = +1;
      }

      if (pan_dir) edge_pan_update(pan_dir, fabsf(sx));
      else         edge_pan_stop();
    }
    if (tap) {
      s_cursor_release_wait = 0;
      if (!s_tap_prev) {
        push(s_cfg.cursor_id, s_cx, s_cy, NXP_DOWN);
        s_tap_prev = 1;
      } else {
        push(s_cfg.cursor_id, s_cx, s_cy, NXP_MOVE);
      }
    } else if (s_tap_prev) {
      if (s_cursor_release_wait == 0) {
        s_cursor_release_x = s_cx;
        s_cursor_release_y = s_cy;
      }
      /* Keep visual and Android pointer on one identical fixed point. */
      s_cx = s_cursor_release_x;
      s_cy = s_cursor_release_y;
      if (s_cursor_release_wait < 3) {
        push(s_cfg.cursor_id, s_cursor_release_x, s_cursor_release_y, NXP_MOVE);
        s_cursor_release_wait++;
      } else {
        push(s_cfg.cursor_id, s_cursor_release_x, s_cursor_release_y, NXP_MOVE);
        push(s_cfg.cursor_id, s_cursor_release_x, s_cursor_release_y, NXP_UP);
        s_cursor_release_wait = 0;
        s_tap_prev = 0;
      }
    } else {
      s_cursor_release_wait = 0;
    }
  } else {
    edge_pan_stop();
    s_tap_prev = 0;
    s_cursor_release_wait = 0;
  }

  settings_tick();          /* commits a queued save 3s after the last change */
}

int nxp_poll(NxpEvent *out, int max) {
  int n = s_nev < max ? s_nev : max;
  if (n > 0) memcpy(out, s_ev, (size_t)n * sizeof(NxpEvent));
  return n;
}

int   nxp_cursor_visible(void)          { return s_visible > 0; }
void  nxp_cursor_pos(float *x, float *y){ if (x) *x = s_cx; if (y) *y = s_cy; }
float nxp_stick_speed(void)             { return s_stick_speed; }
float nxp_mouse_sens(void)              { return s_mouse_sens; }
float nxp_gyro_sens(void)               { return s_gyro_sens; }
int   nxp_gyro_enabled(void)            { return s_gyro_on && !s_mouse_connected; }
int   nxp_mouse_connected(void)         { return s_mouse_connected; }


/* ======================= GL overlay ====================================== */

static GLuint s_prog = 0;
static GLint  s_u_screen, s_u_origin, s_u_scale, s_u_colour, s_u_tex, s_u_use_tex;
static int    s_gl_failed = 0;

/* Built-in arrow: tip at (0,0), y down, drawn as a fan from the tip. */
static const GLfloat s_arrow[] = {
   0.0f,  0.0f,   0.0f, 16.0f,   4.0f, 12.0f,   7.0f, 18.0f,
  10.0f, 16.5f,   7.0f, 10.5f,  12.0f, 10.0f,
};
#define ARROW_VERTS 7

static GLuint mkshader(GLenum t, const char *src) {
  GLuint s = glCreateShader(t);
  glShaderSource(s, 1, &src, NULL);
  glCompileShader(s);
  GLint ok = 0;
  glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok) { glDeleteShader(s); return 0; }
  return s;
}

static int gl_init(void) {
  if (s_prog) return 1;
  if (s_gl_failed) return 0;

  static const char *vs =
    "attribute vec2 aPos;\n"
    "attribute vec2 aUV;\n"
    "varying vec2 vUV;\n"
    "uniform vec2 uScreen;\n"
    "uniform vec2 uOrigin;\n"
    "uniform float uScale;\n"
    "void main() {\n"
    "  vUV = aUV;\n"
    "  vec2 p = uOrigin + aPos * uScale;\n"
    "  gl_Position = vec4((p.x/uScreen.x)*2.0-1.0, 1.0-(p.y/uScreen.y)*2.0, 0.0, 1.0);\n"
    "}\n";
  static const char *fs =
    "precision mediump float;\n"
    "varying vec2 vUV;\n"
    "uniform vec4 uColour;\n"
    "uniform sampler2D uTex;\n"
    "uniform float uUseTex;\n"
    "void main() {\n"
    /* Branch on a uniform (uniform control flow -- always safe in GLES2) so the
     * built-in-arrow path never samples a texture. That means we don't have to
     * bind or disturb ANY texture state unless a custom cursor.png is in use. */
    "  if (uUseTex > 0.5) gl_FragColor = texture2D(uTex, vUV);\n"
    "  else               gl_FragColor = uColour;\n"
    "}\n";

  GLuint v = mkshader(GL_VERTEX_SHADER, vs), f = mkshader(GL_FRAGMENT_SHADER, fs);
  if (!v || !f) { s_gl_failed = 1; logf_("nxp: cursor shader compile failed\n"); return 0; }

  GLuint p = glCreateProgram();
  glAttachShader(p, v);
  glAttachShader(p, f);
  glBindAttribLocation(p, 0, "aPos");
  glBindAttribLocation(p, 1, "aUV");
  glLinkProgram(p);
  glDeleteShader(v);
  glDeleteShader(f);

  GLint ok = 0;
  glGetProgramiv(p, GL_LINK_STATUS, &ok);
  if (!ok) { glDeleteProgram(p); s_gl_failed = 1; logf_("nxp: cursor link failed\n"); return 0; }

  s_prog      = p;
  s_u_screen  = glGetUniformLocation(p, "uScreen");
  s_u_origin  = glGetUniformLocation(p, "uOrigin");
  s_u_scale   = glGetUniformLocation(p, "uScale");
  s_u_colour  = glGetUniformLocation(p, "uColour");
  s_u_tex     = glGetUniformLocation(p, "uTex");
  s_u_use_tex = glGetUniformLocation(p, "uUseTex");
  return 1;
}

/* A vertex attribute's FULL state. Saving only the enabled flag (as we did at
 * first) is not enough: glVertexAttribPointer also records size/type/stride/
 * pointer AND which ARRAY_BUFFER was bound at the time. The engine sets its
 * attribute pointers once and reuses them across frames, so overwriting slot 0
 * with our arrow array left every subsequent engine draw reading garbage
 * geometry -- 25 draw calls a frame, and a black screen. */
typedef struct {
  GLint enabled, size, type, norm, stride, buf;
  void *ptr;
} AttribState;

static void attrib_save(GLuint i, AttribState *a) {
  glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &a->enabled);
  glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_SIZE, &a->size);
  glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_TYPE, &a->type);
  glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED, &a->norm);
  glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &a->stride);
  glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &a->buf);
  glGetVertexAttribPointerv(i, GL_VERTEX_ATTRIB_ARRAY_POINTER, &a->ptr);
}

static void attrib_restore(GLuint i, const AttribState *a) {
  /* glVertexAttribPointer captures whatever ARRAY_BUFFER is bound right now, so
   * re-bind the attribute's original buffer before restoring its pointer. */
  glBindBuffer(GL_ARRAY_BUFFER, (GLuint)a->buf);
  if (a->size > 0)
    glVertexAttribPointer(i, a->size, (GLenum)a->type,
                          (GLboolean)(a->norm ? GL_TRUE : GL_FALSE),
                          a->stride, a->ptr);
  if (a->enabled) glEnableVertexAttribArray(i);
  else            glDisableVertexAttribArray(i);
}


/* R3.62: framebuffer-based result-screen recognition.
 *
 * Detect the actual result UI instead of guessing from draw-call count.
 * You Failed and Level Cleared share:
 *   - cyan/teal QUIT button at top-left
 *   - bright music-circle control at top-right
 *
 * We require both corner features and a short temporal confirmation.
 */
static void r362_detect_result_visual(void) {
  GLint vp[4] = {0,0,0,0};
  glGetIntegerv(GL_VIEWPORT, vp);

  const int vw = vp[2];
  const int vh = vp[3];
  if (vw < 320 || vh < 180) {
    s_r362_result_score = 0;
    s_r362_result_visual = 0;
    return;
  }

  int rw = vw / 10;
  int rh = vh / 8;
  if (rw < 48) rw = 48;
  if (rh < 32) rh = 32;
  if (rw > 128) rw = 128;
  if (rh > 96) rh = 96;

  const int pad_x = vw / 50;
  const int pad_y = vh / 40;
  const int y  = vp[1] + vh - rh - pad_y;
  const int lx = vp[0] + pad_x;
  const int rx = vp[0] + vw - rw - pad_x;

  static unsigned char left_px[128 * 96 * 4];
  static unsigned char right_px[128 * 96 * 4];

  glReadPixels(lx, y, rw, rh, GL_RGBA, GL_UNSIGNED_BYTE, left_px);
  glReadPixels(rx, y, rw, rh, GL_RGBA, GL_UNSIGNED_BYTE, right_px);

  const int n = rw * rh;
  int teal = 0;
  int white = 0;

  for (int i = 0; i < n; ++i) {
    const unsigned char lr = left_px[i*4+0];
    const unsigned char lg = left_px[i*4+1];
    const unsigned char lb = left_px[i*4+2];

    if (lg >= 115 && lb >= 75 &&
        lg >= (int)lr + 25 &&
        (int)lg + (int)lb >= 230)
      teal++;

    const unsigned char rr = right_px[i*4+0];
    const unsigned char rg = right_px[i*4+1];
    const unsigned char rb = right_px[i*4+2];

    const int rmax =
      rr > rg ? (rr > rb ? rr : rb) : (rg > rb ? rg : rb);
    const int rmin =
      rr < rg ? (rr < rb ? rr : rb) : (rg < rb ? rg : rb);

    if (rr >= 175 && rg >= 175 && rb >= 175 && (rmax - rmin) <= 55)
      white++;
  }

  const int teal_hit  = (teal  * 100 >= n * 4);
  const int white_hit = (white * 100 >= n * 1);
  const int hit = teal_hit && white_hit;

  if (hit) {
    if (s_r362_result_score < 8)
      s_r362_result_score += 2;
  } else {
    if (s_r362_result_score > 0)
      s_r362_result_score -= 1;
  }

  const int old = s_r362_result_visual;

  if (!old && s_r362_result_score >= 4)
    s_r362_result_visual = 1;
  else if (old && s_r362_result_score <= 1)
    s_r362_result_visual = 0;

  if (old != s_r362_result_visual) {
    logf_("nxp: R3.62 result visual %s teal=%d white=%d n=%d\n",
          s_r362_result_visual ? "ON" : "OFF",
          teal, white, n);
  }
}

/* R3.70.21: keep pause/result visual recognition alive even when the
 * virtual cursor itself is hidden. This must be called with Unity's GL
 * context current, immediately before nxp_draw(). */
void nxp_r362_visual_poll(void) {
  r362_detect_result_visual();
}

void nxp_draw(void) {
  static int s_r357_menu_seen = 0;
  if (!s_r357_menu_seen &&
      (g_sz_last_frame_draws == 2 || g_sz_last_frame_draws == 3)) {
    s_r357_menu_seen = 1;
    logf_("nxp: R3.57 menu reached -> cursor visible with menu/music\n");
  }
  if (!s_r357_menu_seen || !s_ready || s_visible <= 0 || s_mouse_auto_hidden) return;
  if (!gl_init()) return;

  /* Decode + upload both standalone cursor states on first post-startup draw
   * (needs a live GL context). No PNG work is done during Unity's long load. */
  if (!s_png_tried) {
    s_png_tried = 1;
    for (int i = 0; i < CURSOR_IMAGES; i++) {
      if (s_png_bytes[i]) {
        if (!cursor_upload_png(i)) s_cursor_tex[i] = 0;
        free(s_png_bytes[i]);
        s_png_bytes[i] = NULL;
        s_png_len[i] = 0;
      }
    }
  }

  /* ---- save every bit of state we touch ---- */
  AttribState a0, a1;
  attrib_save(0, &a0);
  attrib_save(1, &a1);

  GLint prev_prog = 0, prev_buf = 0;
  GLint bs_rgb = 0, bd_rgb = 0, bs_a = 0, bd_a = 0;
  glGetIntegerv(GL_CURRENT_PROGRAM, &prev_prog);
  glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prev_buf);
  glGetIntegerv(GL_BLEND_SRC_RGB, &bs_rgb);
  glGetIntegerv(GL_BLEND_DST_RGB, &bd_rgb);
  glGetIntegerv(GL_BLEND_SRC_ALPHA, &bs_a);
  glGetIntegerv(GL_BLEND_DST_ALPHA, &bd_a);
  const GLboolean was_blend   = glIsEnabled(GL_BLEND);
  const GLboolean was_depth   = glIsEnabled(GL_DEPTH_TEST);
  const GLboolean was_cull    = glIsEnabled(GL_CULL_FACE);
  const GLboolean was_scissor = glIsEnabled(GL_SCISSOR_TEST);

  glDisable(GL_DEPTH_TEST);
  glDisable(GL_CULL_FACE);
  glDisable(GL_SCISSOR_TEST);
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);   /* honours PNG alpha */

  glUseProgram(s_prog);
  /* CRITICAL: the cursor shader positions via NDC (gl_Position), which maps to
   * the CURRENT GL viewport. The game leaves a 640x1137 viewport set at swap
   * time (its downscaled render pass), so without this the cursor gets squished
   * into that sub-region while its TAP stays in full 720x1280 space -> a
   * position-dependent offset (zero at one corner, growing away). Draw the
   * cursor in the full framebuffer (== screen_w x screen_h == the tap space),
   * then restore the game's viewport below. */
  GLint prev_vp[4] = {0,0,0,0};
  glGetIntegerv(GL_VIEWPORT, prev_vp);
  glViewport(0, 0, s_cfg.screen_w, s_cfg.screen_h);
  glUniform2f(s_u_screen, (GLfloat)s_cfg.screen_w, (GLfloat)s_cfg.screen_h);
  glUniform2f(s_u_origin, s_cx, s_cy);

  const int cursor_kind = (s_tap_prev && s_cursor_tex[CURSOR_GRAB])
                            ? CURSOR_GRAB : CURSOR_POINTER;
  const GLuint cursor_tex = s_cursor_tex[cursor_kind];
  if (cursor_tex) {
    /* Standalone pointer/grab images. Both were cropped onto same-size square
     * canvases, so use one vertical canvas anchor to prevent the grab state
     * jumping upward when A/ZL/ZR is pressed. */
    const GLfloat w = (GLfloat)s_cursor_w[cursor_kind];
    const GLfloat h = (GLfloat)s_cursor_h[cursor_kind];
    const GLfloat hot_x =
      (cursor_kind == CURSOR_GRAB && s_cursor_visible_hot_valid[CURSOR_GRAB])
        ? (GLfloat)s_cursor_visible_hot_x[CURSOR_GRAB]
        : ((cursor_kind == CURSOR_GRAB) ? (w * 0.50f) : (w * 0.36f));
    const GLfloat hot_y =
      (cursor_kind == CURSOR_GRAB && s_cursor_visible_hot_valid[CURSOR_GRAB])
        ? (GLfloat)s_cursor_visible_hot_y[CURSOR_GRAB]
        : ((cursor_kind == CURSOR_GRAB) ? (h * 0.50f) : (h * 0.10f));
    const GLfloat quad[] = {
      -hot_x,    -hot_y,  w-hot_x,    -hot_y,
      -hot_x, h-hot_y,    w-hot_x, h-hot_y
    };
    const GLfloat uv[]   = { 0,0,  1,0,  0,1,  1,1 };

    /* Texture units: read the ACTIVE unit first, then switch to unit 0 and read
     * ITS binding. (Reading GL_TEXTURE_BINDING_2D before switching gives you the
     * active unit's binding, and restoring that onto unit 0 corrupts unit 0.) */
    GLint prev_active = 0, tex0 = 0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &prev_active);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex0);

    glBindBuffer(GL_ARRAY_BUFFER, 0);                  /* client-side arrays */
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, quad);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, uv);

    glBindTexture(GL_TEXTURE_2D, cursor_tex);
    glUniform1i(s_u_tex, 0);
    glUniform1f(s_u_use_tex, 1.0f);
    glUniform1f(s_u_scale,
                ((GLfloat)s_cfg.screen_h / 1080.0f) * s_cfg.cursor_scale);
    glUniform4f(s_u_colour, 1, 1, 1, 1);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    glBindTexture(GL_TEXTURE_2D, (GLuint)tex0);        /* restore unit 0 */
    glActiveTexture((GLenum)prev_active);              /* then the active unit */
  } else {
    /* ---- built-in arrow: solid colour, no texture touched at all ---- */
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, s_arrow);
    glUniform1f(s_u_use_tex, 0.0f);                    /* shader will not sample */

    const GLfloat sc = 2.0f * ((GLfloat)s_cfg.screen_h / 1080.0f);
    glUniform1f(s_u_scale, sc * 1.22f);
    glUniform4f(s_u_colour, 0.0f, 0.0f, 0.0f, 0.85f);  /* outline */
    glDrawArrays(GL_TRIANGLE_FAN, 0, ARROW_VERTS);

    glUniform1f(s_u_scale, sc);
    glUniform4f(s_u_colour, 1.0f, 1.0f, 1.0f, 1.0f);   /* fill */
    glDrawArrays(GL_TRIANGLE_FAN, 0, ARROW_VERTS);
  }

  /* ---- restore ----
   * Attributes first (each re-binds its own source buffer), then put the global
   * ARRAY_BUFFER binding back, then the program and the fixed-function bits. */
  glViewport(prev_vp[0], prev_vp[1], prev_vp[2], prev_vp[3]);   /* game's viewport back */
  attrib_restore(0, &a0);
  attrib_restore(1, &a1);
  glBindBuffer(GL_ARRAY_BUFFER, (GLuint)prev_buf);
  glUseProgram((GLuint)prev_prog);
  glBlendFuncSeparate((GLenum)bs_rgb, (GLenum)bd_rgb, (GLenum)bs_a, (GLenum)bd_a);
  if (!was_blend) glDisable(GL_BLEND); else glEnable(GL_BLEND);
  if (was_depth)   glEnable(GL_DEPTH_TEST);
  if (was_cull)    glEnable(GL_CULL_FACE);
  if (was_scissor) glEnable(GL_SCISSOR_TEST);
}
