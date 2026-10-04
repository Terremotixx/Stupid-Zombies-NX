/* Stupid Zombies 3.4.5 - R3.50 level-store escape.
 *
 * Scope is deliberately tiny:
 *   - observe Level.ShowStore(self)
 *   - remember that Level instance while the store is open
 *   - let controller B call the game's own Level.CloseStorePanel(self)
 *
 * Billing/IAP is NOT emulated here. A failed/unavailable purchase stays failed
 * and grants nothing. This file only prevents the in-level store from trapping
 * the player.
 */
#include <stdint.h>
#include <switch.h>

#include "so_util.h"
#include "util.h"

extern so_module il2cpp_mod;

/* Verified against Stupid Zombies 3.4.5 ARM64 / Unity 2022.3.19f1. */
#define SZ_OFF_LEVEL_SHOW_STORE          0x00fd70f4u
#define SZ_OFF_LEVEL_SHOW_STORE_HELPER   0x00fd6130u
#define SZ_OFF_LEVEL_SHOW_STORE_RESUME   0x00fd7104u
#define SZ_OFF_LEVEL_CLOSE_STORE_PANEL   0x00fd74d8u

/* Original first 4 instructions of Level.ShowStore:
 *   stp x30,x19,[sp,#-0x10]!
 *   mov w1,#1
 *   mov x19,x0
 *   bl  0xfd6130
 */
#define SZ_SHOW_I0 0xa9bf4ffeu
#define SZ_SHOW_I1 0x52800021u
#define SZ_SHOW_I2 0xaa0003f3u
#define SZ_SHOW_I3 0x97fffc0cu

volatile void *sz_store_level_self = 0;
volatile uint32_t sz_store_open = 0;
/* R3.55: menu Stores do not use Level.ShowStore. */
static volatile uint32_t sz_menu_store_open = 0;
static volatile uint32_t sz_menu_store_close_pending = 0;
/* R3.57: Settings is separate from Store; never apply Store lockdown to it. */
static volatile uint32_t sz_settings_open = 0;
static volatile uint32_t sz_settings_close_pending = 0;
void *sz_store_show_helper = 0;
void *sz_store_show_resume = 0;

static void (*sz_close_store_panel)(void *self, void *method) = 0;
static int sz_store_hook_installed = 0;

__asm__(
".text\n"
".align 2\n"
".global sz_level_show_store_trampoline\n"
".type sz_level_show_store_trampoline,%function\n"
"sz_level_show_store_trampoline:\n"
"  adrp x16, sz_store_level_self\n"
"  add  x16, x16, :lo12:sz_store_level_self\n"
"  str  x0, [x16]\n"
"  adrp x16, sz_store_open\n"
"  add  x16, x16, :lo12:sz_store_open\n"
"  mov  w17, #1\n"
"  str  w17, [x16]\n"
"\n"
"  stp  x30, x19, [sp, #-0x10]!\n"
"  mov  w1, #1\n"
"  mov  x19, x0\n"
"  adrp x16, sz_store_show_helper\n"
"  add  x16, x16, :lo12:sz_store_show_helper\n"
"  ldr  x16, [x16]\n"
"  blr  x16\n"
"  adrp x16, sz_store_show_resume\n"
"  add  x16, x16, :lo12:sz_store_show_resume\n"
"  ldr  x16, [x16]\n"
"  br   x16\n"
);
extern void sz_level_show_store_trampoline(void);

void sz_store_fix_install(void) {
    if (sz_store_hook_installed) return;

    uintptr_t ib = (uintptr_t)il2cpp_mod.load_virtbase;
    if (!ib) {
        debugPrintf("[storefix] libil2cpp base unavailable; hook skipped\n");
        return;
    }

    volatile uint32_t *p =
        (volatile uint32_t *)(ib + SZ_OFF_LEVEL_SHOW_STORE);

    if (p[0] != SZ_SHOW_I0 || p[1] != SZ_SHOW_I1 ||
        p[2] != SZ_SHOW_I2 || p[3] != SZ_SHOW_I3) {
        debugPrintf("[storefix] Level.ShowStore signature mismatch: "
                    "%08x %08x %08x %08x; NOT patching\n",
                    p[0], p[1], p[2], p[3]);
        return;
    }

    sz_store_show_helper =
        (void *)(ib + SZ_OFF_LEVEL_SHOW_STORE_HELPER);
    sz_store_show_resume =
        (void *)(ib + SZ_OFF_LEVEL_SHOW_STORE_RESUME);
    sz_close_store_panel =
        (void (*)(void *, void *))(ib + SZ_OFF_LEVEL_CLOSE_STORE_PANEL);

    const uint32_t stub[4] = {
        0x58000050u, /* ldr x16, #8 */
        0xd61f0200u, /* br  x16     */
        (uint32_t)((uintptr_t)&sz_level_show_store_trampoline & 0xffffffffu),
        (uint32_t)((uintptr_t)&sz_level_show_store_trampoline >> 32),
    };
    so_patch_code((void *)p, stub, sizeof(stub));

    sz_store_hook_installed = 1;
    debugPrintf("[storefix] R3.50 installed: Level.ShowStore=%p CloseStorePanel=%p\n",
                (void *)(ib + SZ_OFF_LEVEL_SHOW_STORE),
                (void *)(ib + SZ_OFF_LEVEL_CLOSE_STORE_PANEL));
}

/* Called from the existing B-button path on the Unity/main thread.
 * Return 1 only when B was consumed by closing the in-level store.
 */

/* R3.51: safe pointer policy for the in-level Store. */
int sz_store_try_close(void);


int sz_settings_is_open(void) {
    if (sz_settings_open && sz_settings_close_pending) {
        extern int nxp_r362_result_visual_is_on(void);

        if (nxp_r362_result_visual_is_on()) {
            sz_settings_open = 0;
            sz_settings_close_pending = 0;
            debugPrintf("[settings69] Settings close CONFIRMED by root visual\n");
        }
    }

    return sz_settings_open ? 1 : 0;
}

/* R3.63: B owns this close, so clear Settings state before the synthetic
 * top-left-arrow tap reaches Unity. */
int sz_settings_take_b(void) {
    if (!sz_settings_open)
        return 0;

    sz_settings_close_pending = 1;
    debugPrintf("[settings69] legacy Settings B helper -> close REQUESTED\n");
    return 1;
}

int sz_store_is_open(void) {
    if (sz_menu_store_open && sz_menu_store_close_pending) {
        extern int nxp_r362_result_visual_is_on(void);

        if (nxp_r362_result_visual_is_on()) {
            sz_menu_store_open = 0;
            sz_menu_store_close_pending = 0;
            debugPrintf("[store70] menu Store close CONFIRMED by destination visual\n");
        }
    }

    return ((sz_store_hook_installed && sz_store_open) || sz_menu_store_open) ? 1 : 0;
}

int sz_store_menu_take_b(void) {
    if (!sz_menu_store_open)
        return 0;

    sz_menu_store_close_pending = 1;
    debugPrintf("[store70] B -> menu Store close REQUESTED; lockdown stays armed\n");
    return 1;
}

int sz_store_handle_pointer_down(float x, float y) {
    extern volatile unsigned long g_sz_last_frame_draws;

    /* R3.70.5 Store entry Y guard.
     *
     * Stress testing exposed a false Store arm at (1039,159): the old Store
     * detector accepted the entire y=0..160 strip even though real Store-entry
     * samples cluster substantially higher on screen. Once falsely armed,
     * lockdown correctly blocked the rest of the root menu and looked like a
     * frozen main menu.
     *
     * Keep Settings' proven y=0..160 range unchanged. Tighten ONLY Store to
     * y<=125. X range is deliberately unchanged to avoid breaking valid Store
     * taps observed across different menu/LEVELS states. */
    if (!sz_menu_store_open &&
        !(sz_store_hook_installed && sz_store_open) &&
        (g_sz_last_frame_draws == 2 || g_sz_last_frame_draws == 3) &&
        y >= 0.0f && y <= 160.0f) {
        if (x >= 860.0f && x <= 1125.0f && y <= 125.0f) {
            sz_menu_store_open = 1;
            sz_menu_store_close_pending = 0;
            sz_settings_open = 0;
            sz_settings_close_pending = 0;
            debugPrintf("[storelock] menu Store armed by entry %.1f,%.1f draws=%lu\n",
                        (double)x, (double)y, g_sz_last_frame_draws);
            return 0;
        }

        if (x >= 860.0f && x <= 1125.0f && y > 125.0f) {
            debugPrintf("[store705] ignored false Store-zone candidate %.1f,%.1f draws=%lu\n",
                        (double)x, (double)y, g_sz_last_frame_draws);
        }

        if ((g_sz_last_frame_draws == 2 || g_sz_last_frame_draws == 3) && x > 1125.0f && x <= 1280.0f) {
            sz_settings_open = 1;
            sz_settings_close_pending = 0;
            debugPrintf("[settings57] Settings armed by entry %.1f,%.1f\n",
                        (double)x, (double)y);
            return 0;
        }
    }

    if (sz_settings_open) {
        /* Settings is NEVER locked.  Every option remains clickable.
         * Tapping the visible top-left arrow simply clears our state and
         * lets the game's own arrow receive the click. */
        if (x >= 0.0f && x <= 190.0f && y >= 0.0f && y <= 170.0f) {
            sz_settings_close_pending = 1;
            debugPrintf("[settings69] top-left arrow allowed -> close REQUESTED; "
                        "keep Settings armed until visual confirmation\n");
        }
        return 0;
    }

    if (sz_menu_store_open) {
        if (x >= 0.0f && x <= 180.0f && y >= 0.0f && y <= 160.0f) {
            sz_menu_store_close_pending = 1;
            debugPrintf("[store70] menu arrow %.1f,%.1f allowed -> close REQUESTED; "
                        "lockdown stays armed\n",
                        (double)x, (double)y);
            return 0;
        }

        debugPrintf("[storelock] menu Store blocked pointer %.1f,%.1f\n",
                    (double)x, (double)y);
        return 1;
    }

    if (!sz_store_hook_installed || !sz_store_open)
        return 0;

    /* R3.54 hard lockdown: only the real top-left back-arrow remains active.
     * Everything else in the Store is swallowed before Unity sees it. */
    if (x >= 0.0f && x <= 180.0f && y >= 0.0f && y <= 160.0f) {
        debugPrintf("[storelock] arrow %.1f,%.1f -> CloseStorePanel\n",
                    (double)x, (double)y);
        (void)sz_store_try_close();
        return 1;
    }

    debugPrintf("[storelock] blocked pointer %.1f,%.1f\n",
                (double)x, (double)y);
    return 1;
}

int sz_store_try_close(void) {
    void *self = (void *)sz_store_level_self;

    if (!sz_store_hook_installed || !sz_store_open ||
        !self || !sz_close_store_panel)
        return 0;

    /* Clear first: if CloseStorePanel itself causes another UI/input event,
     * that event must see the store as already closing. */
    sz_store_open = 0;

    debugPrintf("[storefix] B -> Level.CloseStorePanel(self=%p)\n", self);
    sz_close_store_panel(self, 0);
    return 1;
}
