/* Stupid Zombies 3.4.5 - R3.59 managed result-screen probe. */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <ctype.h>

#include "so_util.h"
#include "util.h"

extern so_module il2cpp_mod;

typedef void *(*fn_domain_get)(void);
typedef const void **(*fn_domain_get_assemblies)(const void *domain, size_t *size);
typedef const void *(*fn_assembly_get_image)(const void *assembly);
typedef size_t (*fn_image_get_class_count)(const void *image);
typedef void *(*fn_image_get_class)(const void *image, size_t index);
typedef void *(*fn_class_get_methods)(void *klass, void **iter);
typedef const char *(*fn_method_get_name)(const void *method);
typedef uint32_t (*fn_method_get_param_count)(const void *method);
typedef const char *(*fn_class_get_name)(void *klass);
typedef const char *(*fn_class_get_namespace)(void *klass);

static fn_domain_get p_domain_get;
static fn_domain_get_assemblies p_domain_get_assemblies;
static fn_assembly_get_image p_assembly_get_image;
static fn_image_get_class_count p_image_get_class_count;
static fn_image_get_class p_image_get_class;
static fn_class_get_methods p_class_get_methods;
static fn_method_get_name p_method_get_name;
static fn_method_get_param_count p_method_get_param_count;
static fn_class_get_name p_class_get_name;
static fn_class_get_namespace p_class_get_namespace;

static int s_done = 0;
static int s_tries = 0;

/* R3.70.22: exact managed Level controls used by physical START.
 * Filled by the existing managed-method inventory, so no ASLR-sensitive
 * absolute addresses are hardcoded. */
static uintptr_t s_level_pause_ptr = 0;
static void *s_level_pause_mi = 0;
static uintptr_t s_level_resume_ptr = 0;
static void *s_level_resume_mi = 0;

static int has_icase(const char *s, const char *needle) {
    if (!s || !needle) return 0;
    size_t n = strlen(needle);
    if (!n) return 1;
    for (; *s; ++s) {
        size_t i = 0;
        while (i < n && s[i] &&
               tolower((unsigned char)s[i]) == tolower((unsigned char)needle[i]))
            ++i;
        if (i == n) return 1;
    }
    return 0;
}

/* R3.70.12: one-shot managed-method inventory for the intermittent
 * level-entry black/fade hang. Diagnostic only: nothing is hooked/called. */
static int transition_candidate(const char *ns, const char *cls, const char *name) {
    if (!cls || !name) return 0;

    const char *skip_ns[] = {
        "System", "Microsoft", "Mono", "UnityEngine", "UnityEditor",
        "Unity.Services", "Unity.Collections", "Unity.Jobs",
        "TMPro", "Newtonsoft", "Google", "Firebase"
    };
    for (unsigned i = 0; i < sizeof(skip_ns)/sizeof(skip_ns[0]); ++i)
        if (ns && has_icase(ns, skip_ns[i]))
            return 0;

    /* R3.70.17: dump EVERY method from the four classes around the
     * LEVEL XX intro. This is inventory only; no method is called or hooked. */
    if (!strcmp(cls, "LevelLoadBackground") ||
        !strcmp(cls, "LevelBackground") ||
        !strcmp(cls, "LevelSettings") ||
        !strcmp(cls, "Level")) {
        return 9; /* [inv717] exact suspect class */
    }

    const char *strong[] = {
        "fade", "fader", "transition", "curtain", "black",
        "intro", "loading", "loadlevel", "load_level",
        "loadscene", "load_scene", "scenechange", "scene_change",
        "levelstart", "level_start", "startlevel", "start_level",
        "levelintro", "level_intro", "showlevel", "show_level"
    };
    for (unsigned i = 0; i < sizeof(strong)/sizeof(strong[0]); ++i)
        if (has_icase(cls, strong[i]) || has_icase(name, strong[i]))
            return 2;

    if (has_icase(cls, "level")) {
        const char *level_actions[] = {
            "start", "begin", "enter", "init", "awake", "enable",
            "load", "show", "hide", "fade", "transition", "ready",
            "open", "close", "finish", "complete", "restart", "resume"
        };
        for (unsigned i = 0; i < sizeof(level_actions)/sizeof(level_actions[0]); ++i)
            if (has_icase(name, level_actions[i]))
                return 1;
    }

    return 0;
}

static int resolve_exports(void) {
#define RESOLVE(sym, type) do { \
    uintptr_t a = so_try_find_addr_rx(&il2cpp_mod, "il2cpp_" #sym); \
    if (!a) return 0; \
    p_##sym = (type)a; \
} while (0)

    RESOLVE(domain_get, fn_domain_get);
    RESOLVE(domain_get_assemblies, fn_domain_get_assemblies);
    RESOLVE(assembly_get_image, fn_assembly_get_image);
    RESOLVE(image_get_class_count, fn_image_get_class_count);
    RESOLVE(image_get_class, fn_image_get_class);
    RESOLVE(class_get_methods, fn_class_get_methods);
    RESOLVE(method_get_name, fn_method_get_name);
    RESOLVE(method_get_param_count, fn_method_get_param_count);
    RESOLVE(class_get_name, fn_class_get_name);
    RESOLVE(class_get_namespace, fn_class_get_namespace);
#undef RESOLVE
    return 1;
}


/* R3.70.13: diagnostic-only next-level transition trace.
 *
 * Four managed entries are intercepted with exact 16-byte prologue replay:
 *   Level.LoadNextLevel   +0x00fd66e8
 *   Level.DoLoadNextLevel +0x00fd6760
 *   Level.LevelComplete   +0x00fd67e8
 *   Level.LoadLevelScene  +0x00fd6a08
 *
 * Trampolines only update counters; debug logging is deferred to poll().
 */

#define T713_OFF_LOAD_NEXT      0x00fd66e8u
#define T713_OFF_DO_LOAD_NEXT   0x00fd6760u
#define T713_OFF_LEVEL_COMPLETE 0x00fd67e8u
#define T713_OFF_LOAD_SCENE     0x00fd6a08u

volatile uint32_t g_trace713_seq = 0;

volatile uint32_t g_trace713_loadnext_seq = 0;
volatile uint32_t g_trace713_doloadnext_seq = 0;
volatile uint32_t g_trace713_complete_seq = 0;
volatile uint32_t g_trace713_loadscene_seq = 0;

volatile uint32_t g_trace713_loadnext_count = 0;
volatile uint32_t g_trace713_doloadnext_count = 0;
volatile uint32_t g_trace713_complete_count = 0;
volatile uint32_t g_trace713_loadscene_count = 0;

volatile uintptr_t g_trace713_loadnext_self = 0;
volatile uintptr_t g_trace713_doloadnext_self = 0;
volatile uintptr_t g_trace713_complete_self = 0;
volatile uintptr_t g_trace713_loadscene_self = 0;

uintptr_t g_trace713_page_249c000 = 0;
uintptr_t g_trace713_page_2317000 = 0;
uintptr_t g_trace713_page_2315000 = 0;

uintptr_t g_trace713_loadnext_resume = 0;
uintptr_t g_trace713_doloadnext_resume = 0;
uintptr_t g_trace713_complete_resume = 0;
uintptr_t g_trace713_loadscene_resume = 0;

static int g_trace713_installed = 0;
static uint32_t g_trace713_last_printed_seq = 0;

__asm__(
".text\n"
".align 2\n"

".global sz_trace713_loadnext_trampoline\n"
".type sz_trace713_loadnext_trampoline,%function\n"
"sz_trace713_loadnext_trampoline:\n"
"  .word 0xa9bf4ffe\n"
"  .word 0xb9802808\n"
"  .word 0x928eeec9\n"
"  .word 0xf2b11109\n"
"  adrp x16, g_trace713_loadnext_self\n"
"  add  x16, x16, :lo12:g_trace713_loadnext_self\n"
"  str  x0, [x16]\n"
"  adrp x16, g_trace713_seq\n"
"  add  x16, x16, :lo12:g_trace713_seq\n"
"  ldr  w17, [x16]\n"
"  add  w17, w17, #1\n"
"  str  w17, [x16]\n"
"  adrp x16, g_trace713_loadnext_seq\n"
"  add  x16, x16, :lo12:g_trace713_loadnext_seq\n"
"  str  w17, [x16]\n"
"  adrp x16, g_trace713_loadnext_count\n"
"  add  x16, x16, :lo12:g_trace713_loadnext_count\n"
"  ldr  w17, [x16]\n"
"  add  w17, w17, #1\n"
"  str  w17, [x16]\n"
"  adrp x16, g_trace713_loadnext_resume\n"
"  add  x16, x16, :lo12:g_trace713_loadnext_resume\n"
"  ldr  x16, [x16]\n"
"  br   x16\n"

".global sz_trace713_doloadnext_trampoline\n"
".type sz_trace713_doloadnext_trampoline,%function\n"
"sz_trace713_doloadnext_trampoline:\n"
"  .word 0xa9be57fe\n"
"  .word 0xa9014ff4\n"
"  adrp x20, g_trace713_page_249c000\n"
"  add  x20, x20, :lo12:g_trace713_page_249c000\n"
"  ldr  x20, [x20]\n"
"  adrp x21, g_trace713_page_2317000\n"
"  add  x21, x21, :lo12:g_trace713_page_2317000\n"
"  ldr  x21, [x21]\n"
"  adrp x16, g_trace713_doloadnext_self\n"
"  add  x16, x16, :lo12:g_trace713_doloadnext_self\n"
"  str  x0, [x16]\n"
"  adrp x16, g_trace713_seq\n"
"  add  x16, x16, :lo12:g_trace713_seq\n"
"  ldr  w17, [x16]\n"
"  add  w17, w17, #1\n"
"  str  w17, [x16]\n"
"  adrp x16, g_trace713_doloadnext_seq\n"
"  add  x16, x16, :lo12:g_trace713_doloadnext_seq\n"
"  str  w17, [x16]\n"
"  adrp x16, g_trace713_doloadnext_count\n"
"  add  x16, x16, :lo12:g_trace713_doloadnext_count\n"
"  ldr  w17, [x16]\n"
"  add  w17, w17, #1\n"
"  str  w17, [x16]\n"
"  adrp x16, g_trace713_doloadnext_resume\n"
"  add  x16, x16, :lo12:g_trace713_doloadnext_resume\n"
"  ldr  x16, [x16]\n"
"  br   x16\n"

".global sz_trace713_complete_trampoline\n"
".type sz_trace713_complete_trampoline,%function\n"
"sz_trace713_complete_trampoline:\n"
"  .word 0xf81e0ffe\n"
"  .word 0xa9014ff4\n"
"  adrp x20, g_trace713_page_249c000\n"
"  add  x20, x20, :lo12:g_trace713_page_249c000\n"
"  ldr  x20, [x20]\n"
"  ldrb w8, [x20, #790]\n"
"  adrp x16, g_trace713_complete_self\n"
"  add  x16, x16, :lo12:g_trace713_complete_self\n"
"  str  x0, [x16]\n"
"  adrp x16, g_trace713_seq\n"
"  add  x16, x16, :lo12:g_trace713_seq\n"
"  ldr  w17, [x16]\n"
"  add  w17, w17, #1\n"
"  str  w17, [x16]\n"
"  adrp x16, g_trace713_complete_seq\n"
"  add  x16, x16, :lo12:g_trace713_complete_seq\n"
"  str  w17, [x16]\n"
"  adrp x16, g_trace713_complete_count\n"
"  add  x16, x16, :lo12:g_trace713_complete_count\n"
"  ldr  w17, [x16]\n"
"  add  w17, w17, #1\n"
"  str  w17, [x16]\n"
"  adrp x16, g_trace713_complete_resume\n"
"  add  x16, x16, :lo12:g_trace713_complete_resume\n"
"  ldr  x16, [x16]\n"
"  br   x16\n"

".global sz_trace713_loadscene_trampoline\n"
".type sz_trace713_loadscene_trampoline,%function\n"
"sz_trace713_loadscene_trampoline:\n"
"  .word 0xf81e0ffe\n"
"  .word 0xa9014ff4\n"
"  adrp x20, g_trace713_page_249c000\n"
"  add  x20, x20, :lo12:g_trace713_page_249c000\n"
"  ldr  x20, [x20]\n"
"  adrp x19, g_trace713_page_2315000\n"
"  add  x19, x19, :lo12:g_trace713_page_2315000\n"
"  ldr  x19, [x19]\n"
"  adrp x16, g_trace713_loadscene_self\n"
"  add  x16, x16, :lo12:g_trace713_loadscene_self\n"
"  str  x0, [x16]\n"
"  adrp x16, g_trace713_seq\n"
"  add  x16, x16, :lo12:g_trace713_seq\n"
"  ldr  w17, [x16]\n"
"  add  w17, w17, #1\n"
"  str  w17, [x16]\n"
"  adrp x16, g_trace713_loadscene_seq\n"
"  add  x16, x16, :lo12:g_trace713_loadscene_seq\n"
"  str  w17, [x16]\n"
"  adrp x16, g_trace713_loadscene_count\n"
"  add  x16, x16, :lo12:g_trace713_loadscene_count\n"
"  ldr  w17, [x16]\n"
"  add  w17, w17, #1\n"
"  str  w17, [x16]\n"
"  adrp x16, g_trace713_loadscene_resume\n"
"  add  x16, x16, :lo12:g_trace713_loadscene_resume\n"
"  ldr  x16, [x16]\n"
"  br   x16\n"
);

extern void sz_trace713_loadnext_trampoline(void);
extern void sz_trace713_doloadnext_trampoline(void);
extern void sz_trace713_complete_trampoline(void);
extern void sz_trace713_loadscene_trampoline(void);

static int trace713_sig4(uintptr_t ib, uint32_t off,
                         uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    volatile uint32_t *p = (volatile uint32_t *)(ib + off);
    return p[0] == a && p[1] == b && p[2] == c && p[3] == d;
}

static void trace713_patch_abs(uintptr_t ib, uint32_t off, void *target) {
    volatile uint32_t *p = (volatile uint32_t *)(ib + off);
    const uintptr_t t = (uintptr_t)target;
    const uint32_t stub[4] = {
        0x58000050u, 0xd61f0200u,
        (uint32_t)(t & 0xffffffffu), (uint32_t)(t >> 32),
    };
    so_patch_code((void *)p, stub, sizeof(stub));
}


/* R3.70.15: diagnostic-only trace for the LEVEL XX intro/fade.
 *
 * Exact methods from this Stupid Zombies 3.4.5 libil2cpp:
 *   iTween.CameraFadeFrom(float,float) +0x00f76aec
 *   iTween.CameraFadeTo(float,float)   +0x00f77aac
 *   iTween.FadeUpdate(...)            +0x00f8d0e0
 *
 * These three entries have relocation-safe first 16 bytes (no ADRP/branch),
 * so we can replay their original prologues exactly and continue at +16.
 * No game state is changed.
 */
#define T715_OFF_CAM_FADE_FROM2 0x00f76aecu
#define T715_OFF_CAM_FADE_TO2   0x00f77aacu
#define T715_OFF_FADE_UPDATE    0x00f8d0e0u

volatile uint32_t g_trace715_seq = 0;
volatile uint32_t g_trace715_from_seq = 0;
volatile uint32_t g_trace715_to_seq = 0;
volatile uint32_t g_trace715_update_seq = 0;

volatile uint32_t g_trace715_from_count = 0;
volatile uint32_t g_trace715_to_count = 0;
volatile uint32_t g_trace715_update_count = 0;

volatile uintptr_t g_trace715_from_x0 = 0;
volatile uintptr_t g_trace715_to_x0 = 0;
volatile uintptr_t g_trace715_update_x0 = 0;

uintptr_t g_trace715_from_resume = 0;
uintptr_t g_trace715_to_resume = 0;
uintptr_t g_trace715_update_resume = 0;

static int g_trace715_installed = 0;
static uint32_t g_trace715_seen_from = 0;
static uint32_t g_trace715_seen_to = 0;
static uint32_t g_trace715_seen_update = 0;
static uint32_t g_trace715_fade_generation = 0;
static uint32_t g_trace715_updates_at_command = 0;
static uint32_t g_trace715_quiet_ticks = 0;
static int g_trace715_command_active = 0;

__asm__(
".text\n"
".align 2\n"

".global sz_trace715_from_trampoline\n"
".type sz_trace715_from_trampoline,%function\n"
"sz_trace715_from_trampoline:\n"
"  .word 0x6dbc23e9\n" /* stp d9,d8,[sp,#-64]! */
"  .word 0xf9000bfe\n" /* str x30,[sp,#16] */
"  .word 0xa90257f6\n" /* stp x22,x21,[sp,#32] */
"  .word 0xa9034ff4\n" /* stp x20,x19,[sp,#48] */
"  adrp x16, g_trace715_from_x0\n"
"  add  x16, x16, :lo12:g_trace715_from_x0\n"
"  str  x0, [x16]\n"
"  adrp x16, g_trace715_seq\n"
"  add  x16, x16, :lo12:g_trace715_seq\n"
"  ldr  w17, [x16]\n"
"  add  w17, w17, #1\n"
"  str  w17, [x16]\n"
"  adrp x16, g_trace715_from_seq\n"
"  add  x16, x16, :lo12:g_trace715_from_seq\n"
"  str  w17, [x16]\n"
"  adrp x16, g_trace715_from_count\n"
"  add  x16, x16, :lo12:g_trace715_from_count\n"
"  ldr  w17, [x16]\n"
"  add  w17, w17, #1\n"
"  str  w17, [x16]\n"
"  adrp x16, g_trace715_from_resume\n"
"  add  x16, x16, :lo12:g_trace715_from_resume\n"
"  ldr  x16, [x16]\n"
"  br   x16\n"

".global sz_trace715_to_trampoline\n"
".type sz_trace715_to_trampoline,%function\n"
"sz_trace715_to_trampoline:\n"
"  .word 0x6dbc23e9\n"
"  .word 0xf9000bfe\n"
"  .word 0xa90257f6\n"
"  .word 0xa9034ff4\n"
"  adrp x16, g_trace715_to_x0\n"
"  add  x16, x16, :lo12:g_trace715_to_x0\n"
"  str  x0, [x16]\n"
"  adrp x16, g_trace715_seq\n"
"  add  x16, x16, :lo12:g_trace715_seq\n"
"  ldr  w17, [x16]\n"
"  add  w17, w17, #1\n"
"  str  w17, [x16]\n"
"  adrp x16, g_trace715_to_seq\n"
"  add  x16, x16, :lo12:g_trace715_to_seq\n"
"  str  w17, [x16]\n"
"  adrp x16, g_trace715_to_count\n"
"  add  x16, x16, :lo12:g_trace715_to_count\n"
"  ldr  w17, [x16]\n"
"  add  w17, w17, #1\n"
"  str  w17, [x16]\n"
"  adrp x16, g_trace715_to_resume\n"
"  add  x16, x16, :lo12:g_trace715_to_resume\n"
"  ldr  x16, [x16]\n"
"  br   x16\n"

".global sz_trace715_update_trampoline\n"
".type sz_trace715_update_trampoline,%function\n"
"sz_trace715_update_trampoline:\n"
"  .word 0xd10143ff\n" /* sub sp,sp,#0x50 */
"  .word 0x6d0123e9\n" /* stp d9,d8,[sp,#16] */
"  .word 0xa9025ffe\n" /* stp x30,x23,[sp,#32] */
"  .word 0xa90357f6\n" /* stp x22,x21,[sp,#48] */
"  adrp x16, g_trace715_update_x0\n"
"  add  x16, x16, :lo12:g_trace715_update_x0\n"
"  str  x0, [x16]\n"
"  adrp x16, g_trace715_seq\n"
"  add  x16, x16, :lo12:g_trace715_seq\n"
"  ldr  w17, [x16]\n"
"  add  w17, w17, #1\n"
"  str  w17, [x16]\n"
"  adrp x16, g_trace715_update_seq\n"
"  add  x16, x16, :lo12:g_trace715_update_seq\n"
"  str  w17, [x16]\n"
"  adrp x16, g_trace715_update_count\n"
"  add  x16, x16, :lo12:g_trace715_update_count\n"
"  ldr  w17, [x16]\n"
"  add  w17, w17, #1\n"
"  str  w17, [x16]\n"
"  adrp x16, g_trace715_update_resume\n"
"  add  x16, x16, :lo12:g_trace715_update_resume\n"
"  ldr  x16, [x16]\n"
"  br   x16\n"
);

extern void sz_trace715_from_trampoline(void);
extern void sz_trace715_to_trampoline(void);
extern void sz_trace715_update_trampoline(void);

static void trace715_install(void) {
    if (g_trace715_installed) return;

    const uintptr_t ib = (uintptr_t)il2cpp_mod.load_virtbase;
    if (!ib) {
        debugPrintf("[fade715] libil2cpp base unavailable; trace NOT installed\n");
        return;
    }

    const int ok_from =
        trace713_sig4(ib, T715_OFF_CAM_FADE_FROM2,
                      0x6dbc23e9u, 0xf9000bfeu, 0xa90257f6u, 0xa9034ff4u);
    const int ok_to =
        trace713_sig4(ib, T715_OFF_CAM_FADE_TO2,
                      0x6dbc23e9u, 0xf9000bfeu, 0xa90257f6u, 0xa9034ff4u);
    const int ok_update =
        trace713_sig4(ib, T715_OFF_FADE_UPDATE,
                      0xd10143ffu, 0x6d0123e9u, 0xa9025ffeu, 0xa90357f6u);

    if (!ok_from || !ok_to || !ok_update) {
        debugPrintf("[fade715] signature mismatch FROM=%d TO=%d UPDATE=%d; NOTHING patched\n",
                    ok_from, ok_to, ok_update);
        return;
    }

    g_trace715_from_resume   = ib + T715_OFF_CAM_FADE_FROM2 + 16u;
    g_trace715_to_resume     = ib + T715_OFF_CAM_FADE_TO2 + 16u;
    g_trace715_update_resume = ib + T715_OFF_FADE_UPDATE + 16u;

    trace713_patch_abs(ib, T715_OFF_CAM_FADE_FROM2,
                       (void *)&sz_trace715_from_trampoline);
    trace713_patch_abs(ib, T715_OFF_CAM_FADE_TO2,
                       (void *)&sz_trace715_to_trampoline);
    trace713_patch_abs(ib, T715_OFF_FADE_UPDATE,
                       (void *)&sz_trace715_update_trampoline);

    g_trace715_installed = 1;
    debugPrintf("[fade715] LEVEL-intro fade trace installed FROM=+0x%x TO=+0x%x UPDATE=+0x%x\n",
                T715_OFF_CAM_FADE_FROM2, T715_OFF_CAM_FADE_TO2,
                T715_OFF_FADE_UPDATE);
}

extern volatile unsigned long g_sz_last_frame_draws;

static void trace715_poll(void) {
    if (!g_trace715_installed) return;

    const uint32_t fc = g_trace715_from_count;
    const uint32_t tc = g_trace715_to_count;
    const uint32_t uc = g_trace715_update_count;

    if (fc != g_trace715_seen_from) {
        g_trace715_seen_from = fc;
        g_trace715_fade_generation++;
        g_trace715_updates_at_command = uc;
        g_trace715_quiet_ticks = 0;
        g_trace715_command_active = 1;
        debugPrintf("[fade715] gen=%u CameraFadeFrom2 count=%u seq=%u x0=%p draws=%lu\n",
                    g_trace715_fade_generation, fc, g_trace715_from_seq,
                    (void *)g_trace715_from_x0, g_sz_last_frame_draws);
    }

    if (tc != g_trace715_seen_to) {
        g_trace715_seen_to = tc;
        g_trace715_fade_generation++;
        g_trace715_updates_at_command = uc;
        g_trace715_quiet_ticks = 0;
        g_trace715_command_active = 1;
        debugPrintf("[fade715] gen=%u CameraFadeTo2 count=%u seq=%u x0=%p draws=%lu\n",
                    g_trace715_fade_generation, tc, g_trace715_to_seq,
                    (void *)g_trace715_to_x0, g_sz_last_frame_draws);
    }

    if (uc != g_trace715_seen_update) {
        const uint32_t delta = uc - g_trace715_seen_update;
        g_trace715_seen_update = uc;
        g_trace715_quiet_ticks = 0;

        if (g_trace715_command_active) {
            const uint32_t since = uc - g_trace715_updates_at_command;
            if (since <= 3u || (since % 30u) < delta) {
                debugPrintf("[fade715] gen=%u FadeUpdate total=%u since-command=%u seq=%u "
                            "x0=%p draws=%lu\n",
                            g_trace715_fade_generation, uc, since,
                            g_trace715_update_seq, (void *)g_trace715_update_x0,
                            g_sz_last_frame_draws);
            }
        }
    } else if (g_trace715_command_active) {
        if (g_trace715_quiet_ticks < 0xffffffffu)
            g_trace715_quiet_ticks++;
        if (g_trace715_quiet_ticks == 30u) {
            debugPrintf("[fade715] gen=%u FadeUpdate QUIET updates=%u draws=%lu\n",
                        g_trace715_fade_generation,
                        uc - g_trace715_updates_at_command,
                        g_sz_last_frame_draws);
            g_trace715_command_active = 0;
        }
    }
}


/* R3.70.16: diagnostic-only trace for level initialization after LoadLevelScene.
 *
 * Exact entries for this Stupid Zombies 3.4.5 libil2cpp:
 *   Level.Awake                 +0x00fd4db0
 *   Level.LoadLevelFromJSON     +0x00fd52e8
 *   Level.Start                 +0x00fd5c60
 *   LevelBackground.Awake       +0x00fd9cc8
 *   LevelLoadBackground.Awake   +0x00fdae98
 *   LevelSettings.Awake         +0x00fdb504
 *
 * No managed call is injected and no game state is changed. Trampolines only
 * increment counters and resume the verified original prologue.
 */

#define T716_OFF_LEVEL_AWAKE       0x00fd4db0u
#define T716_OFF_LOAD_JSON         0x00fd52e8u
#define T716_OFF_LEVEL_START       0x00fd5c60u
#define T716_OFF_BG_AWAKE          0x00fd9cc8u
#define T716_OFF_LOADBG_AWAKE      0x00fdae98u
#define T716_OFF_SETTINGS_AWAKE    0x00fdb504u

volatile uint32_t g_trace716_seq = 0;

#define T716_DECL(name) \
    volatile uint32_t g_trace716_##name##_seq = 0; \
    volatile uint32_t g_trace716_##name##_count = 0; \
    volatile uintptr_t g_trace716_##name##_self = 0; \
    uintptr_t g_trace716_##name##_resume = 0

T716_DECL(level_awake);
T716_DECL(load_json);
T716_DECL(level_start);
T716_DECL(bg_awake);
T716_DECL(loadbg_awake);
T716_DECL(settings_awake);

#undef T716_DECL

/* R3.70.22: call the game's own pause controls on the current Level instance.
 * IL2CPP instance methods receive (__this, MethodInfo*) for zero C# args. */
typedef void (*sz_level_action_fn)(void *self, const void *method);

int sz_level_pause_direct(void) {
    const uintptr_t self = g_trace716_level_start_self;
    if (!self || !s_level_pause_ptr || !s_level_pause_mi) {
        debugPrintf("[start722] PauseMenu unavailable self=%p ptr=%p mi=%p\n",
                    (void *)self, (void *)s_level_pause_ptr, s_level_pause_mi);
        return 0;
    }

    ((sz_level_action_fn)s_level_pause_ptr)((void *)self, s_level_pause_mi);
    debugPrintf("[start722] direct Level.PauseMenu self=%p\n", (void *)self);
    return 1;
}

int sz_level_resume_direct(void) {
    const uintptr_t self = g_trace716_level_start_self;
    if (!self || !s_level_resume_ptr || !s_level_resume_mi) {
        debugPrintf("[start722] ResumeGameFromPause unavailable self=%p ptr=%p mi=%p\n",
                    (void *)self, (void *)s_level_resume_ptr, s_level_resume_mi);
        return 0;
    }

    ((sz_level_action_fn)s_level_resume_ptr)((void *)self, s_level_resume_mi);
    debugPrintf("[start722] direct Level.ResumeGameFromPause self=%p\n",
                (void *)self);
    return 1;
}

uintptr_t g_trace716_page_249c000 = 0;
uintptr_t g_trace716_page_2317000 = 0;
uintptr_t g_trace716_page_2315000 = 0;

static int g_trace716_installed = 0;
static uint32_t g_trace716_last_printed_seq = 0;

#define T716_EVENT_BODY(name) \
"  adrp x16, g_trace716_" #name "_self\n" \
"  add  x16, x16, :lo12:g_trace716_" #name "_self\n" \
"  str  x0, [x16]\n" \
"  adrp x16, g_trace716_seq\n" \
"  add  x16, x16, :lo12:g_trace716_seq\n" \
"  ldr  w17, [x16]\n" \
"  add  w17, w17, #1\n" \
"  str  w17, [x16]\n" \
"  adrp x16, g_trace716_" #name "_seq\n" \
"  add  x16, x16, :lo12:g_trace716_" #name "_seq\n" \
"  str  w17, [x16]\n" \
"  adrp x16, g_trace716_" #name "_count\n" \
"  add  x16, x16, :lo12:g_trace716_" #name "_count\n" \
"  ldr  w17, [x16]\n" \
"  add  w17, w17, #1\n" \
"  str  w17, [x16]\n"

__asm__(
".text\n"
".align 2\n"

".global sz_trace716_level_awake_trampoline\n"
".type sz_trace716_level_awake_trampoline,%function\n"
"sz_trace716_level_awake_trampoline:\n"
"  .word 0xfc1a0fe8\n"
"  .word 0xf90007fe\n"
"  .word 0xa9016ffc\n"
"  .word 0xa90267fa\n"
T716_EVENT_BODY(level_awake)
"  adrp x16, g_trace716_level_awake_resume\n"
"  add  x16, x16, :lo12:g_trace716_level_awake_resume\n"
"  ldr  x16, [x16]\n"
"  br   x16\n"

".global sz_trace716_load_json_trampoline\n"
".type sz_trace716_load_json_trampoline,%function\n"
"sz_trace716_load_json_trampoline:\n"
"  .word 0xa9ba7bfd\n"
"  .word 0xa9016ffc\n"
"  .word 0xa90267fa\n"
"  .word 0xa9035ff8\n"
T716_EVENT_BODY(load_json)
"  adrp x16, g_trace716_load_json_resume\n"
"  add  x16, x16, :lo12:g_trace716_load_json_resume\n"
"  ldr  x16, [x16]\n"
"  br   x16\n"

".global sz_trace716_level_start_trampoline\n"
".type sz_trace716_level_start_trampoline,%function\n"
"sz_trace716_level_start_trampoline:\n"
"  .word 0xa9be57fe\n"
"  .word 0xa9014ff4\n"
"  adrp x20, g_trace716_page_249c000\n"
"  add  x20, x20, :lo12:g_trace716_page_249c000\n"
"  ldr  x20, [x20]\n"
"  adrp x21, g_trace716_page_2317000\n"
"  add  x21, x21, :lo12:g_trace716_page_2317000\n"
"  ldr  x21, [x21]\n"
T716_EVENT_BODY(level_start)
"  adrp x16, g_trace716_level_start_resume\n"
"  add  x16, x16, :lo12:g_trace716_level_start_resume\n"
"  ldr  x16, [x16]\n"
"  br   x16\n"

".global sz_trace716_bg_awake_trampoline\n"
".type sz_trace716_bg_awake_trampoline,%function\n"
"sz_trace716_bg_awake_trampoline:\n"
"  .word 0xf81d0ffe\n"
"  .word 0xa90157f6\n"
"  .word 0xa9024ff4\n"
"  adrp x22, g_trace716_page_249c000\n"
"  add  x22, x22, :lo12:g_trace716_page_249c000\n"
"  ldr  x22, [x22]\n"
T716_EVENT_BODY(bg_awake)
"  adrp x16, g_trace716_bg_awake_resume\n"
"  add  x16, x16, :lo12:g_trace716_bg_awake_resume\n"
"  ldr  x16, [x16]\n"
"  br   x16\n"

".global sz_trace716_loadbg_awake_trampoline\n"
".type sz_trace716_loadbg_awake_trampoline,%function\n"
"sz_trace716_loadbg_awake_trampoline:\n"
"  .word 0xa9bd5ffe\n"
"  .word 0xa90157f6\n"
"  .word 0xa9024ff4\n"
"  adrp x22, g_trace716_page_249c000\n"
"  add  x22, x22, :lo12:g_trace716_page_249c000\n"
"  ldr  x22, [x22]\n"
T716_EVENT_BODY(loadbg_awake)
"  adrp x16, g_trace716_loadbg_awake_resume\n"
"  add  x16, x16, :lo12:g_trace716_loadbg_awake_resume\n"
"  ldr  x16, [x16]\n"
"  br   x16\n"

".global sz_trace716_settings_awake_trampoline\n"
".type sz_trace716_settings_awake_trampoline,%function\n"
"sz_trace716_settings_awake_trampoline:\n"
"  .word 0xa9be57fe\n"
"  .word 0xa9014ff4\n"
"  adrp x20, g_trace716_page_249c000\n"
"  add  x20, x20, :lo12:g_trace716_page_249c000\n"
"  ldr  x20, [x20]\n"
"  adrp x21, g_trace716_page_2315000\n"
"  add  x21, x21, :lo12:g_trace716_page_2315000\n"
"  ldr  x21, [x21]\n"
T716_EVENT_BODY(settings_awake)
"  adrp x16, g_trace716_settings_awake_resume\n"
"  add  x16, x16, :lo12:g_trace716_settings_awake_resume\n"
"  ldr  x16, [x16]\n"
"  br   x16\n"
);

#undef T716_EVENT_BODY

extern void sz_trace716_level_awake_trampoline(void);
extern void sz_trace716_load_json_trampoline(void);
extern void sz_trace716_level_start_trampoline(void);
extern void sz_trace716_bg_awake_trampoline(void);
extern void sz_trace716_loadbg_awake_trampoline(void);
extern void sz_trace716_settings_awake_trampoline(void);

static void trace716_install(void) {
    if (g_trace716_installed) return;

    const uintptr_t ib = (uintptr_t)il2cpp_mod.load_virtbase;
    if (!ib) {
        debugPrintf("[init716] libil2cpp base unavailable; trace NOT installed\n");
        return;
    }

    const int a = trace713_sig4(ib, T716_OFF_LEVEL_AWAKE,
                                0xfc1a0fe8u, 0xf90007feu, 0xa9016ffcu, 0xa90267fau);
    const int j = trace713_sig4(ib, T716_OFF_LOAD_JSON,
                                0xa9ba7bfdu, 0xa9016ffcu, 0xa90267fau, 0xa9035ff8u);
    const int st = trace713_sig4(ib, T716_OFF_LEVEL_START,
                                 0xa9be57feu, 0xa9014ff4u, 0xf000a634u, 0xd0009a15u);
    const int b = trace713_sig4(ib, T716_OFF_BG_AWAKE,
                                0xf81d0ffeu, 0xa90157f6u, 0xa9024ff4u, 0xf000a616u);
    const int l = trace713_sig4(ib, T716_OFF_LOADBG_AWAKE,
                                0xa9bd5ffeu, 0xa90157f6u, 0xa9024ff4u, 0xd000a616u);
    const int q = trace713_sig4(ib, T716_OFF_SETTINGS_AWAKE,
                                0xa9be57feu, 0xa9014ff4u, 0xb000a614u, 0xd00099d5u);

    if (!a || !j || !st || !b || !l || !q) {
        debugPrintf("[init716] signature mismatch Awake=%d JSON=%d Start=%d "
                    "BgAwake=%d LoadBgAwake=%d SettingsAwake=%d; NOTHING patched\n",
                    a, j, st, b, l, q);
        return;
    }

    g_trace716_page_249c000 = ib + 0x0249c000u;
    g_trace716_page_2317000 = ib + 0x02317000u;
    g_trace716_page_2315000 = ib + 0x02315000u;

    g_trace716_level_awake_resume    = ib + T716_OFF_LEVEL_AWAKE + 16u;
    g_trace716_load_json_resume      = ib + T716_OFF_LOAD_JSON + 16u;
    g_trace716_level_start_resume    = ib + T716_OFF_LEVEL_START + 16u;
    g_trace716_bg_awake_resume       = ib + T716_OFF_BG_AWAKE + 16u;
    g_trace716_loadbg_awake_resume   = ib + T716_OFF_LOADBG_AWAKE + 16u;
    g_trace716_settings_awake_resume = ib + T716_OFF_SETTINGS_AWAKE + 16u;

    trace713_patch_abs(ib, T716_OFF_LEVEL_AWAKE,
                       (void *)&sz_trace716_level_awake_trampoline);
    trace713_patch_abs(ib, T716_OFF_LOAD_JSON,
                       (void *)&sz_trace716_load_json_trampoline);
    trace713_patch_abs(ib, T716_OFF_LEVEL_START,
                       (void *)&sz_trace716_level_start_trampoline);
    trace713_patch_abs(ib, T716_OFF_BG_AWAKE,
                       (void *)&sz_trace716_bg_awake_trampoline);
    trace713_patch_abs(ib, T716_OFF_LOADBG_AWAKE,
                       (void *)&sz_trace716_loadbg_awake_trampoline);
    trace713_patch_abs(ib, T716_OFF_SETTINGS_AWAKE,
                       (void *)&sz_trace716_settings_awake_trampoline);

    g_trace716_installed = 1;
    debugPrintf("[init716] level-init trace installed Awake=+0x%x JSON=+0x%x "
                "Start=+0x%x Bg=+0x%x LoadBg=+0x%x Settings=+0x%x\n",
                T716_OFF_LEVEL_AWAKE, T716_OFF_LOAD_JSON, T716_OFF_LEVEL_START,
                T716_OFF_BG_AWAKE, T716_OFF_LOADBG_AWAKE, T716_OFF_SETTINGS_AWAKE);
}

static void trace716_poll(void) {
    if (!g_trace716_installed) return;

    for (;;) {
        uint32_t best = 0xffffffffu;
        const char *name = 0;
        uint32_t count = 0;
        uintptr_t self = 0;

#define T716_CONSIDER(ev, label) do { \
        uint32_t q = g_trace716_##ev##_seq; \
        if (q > g_trace716_last_printed_seq && q < best) { \
            best = q; name = label; count = g_trace716_##ev##_count; \
            self = g_trace716_##ev##_self; \
        } \
    } while (0)

        T716_CONSIDER(level_awake,    "Level.Awake");
        T716_CONSIDER(load_json,      "Level.LoadLevelFromJSON");
        T716_CONSIDER(level_start,    "Level.Start");
        T716_CONSIDER(bg_awake,       "LevelBackground.Awake");
        T716_CONSIDER(loadbg_awake,   "LevelLoadBackground.Awake");
        T716_CONSIDER(settings_awake, "LevelSettings.Awake");

#undef T716_CONSIDER

        if (!name || best == 0xffffffffu)
            break;

        debugPrintf("[init716] seq=%u %s count=%u self=%p loadscene_count=%u draws=%lu\n",
                    best, name, count, (void *)self,
                    g_trace713_loadscene_count, g_sz_last_frame_draws);
        g_trace716_last_printed_seq = best;
    }
}


/* R3.70.18: exact NullReferenceException caller trace.
 *
 * eac564 is the tiny IL2CPP null-reference wrapper:
 *   eac564  str x30,[sp,#-16]!
 *   eac568  bl  eee4b8
 *
 * Therefore, when eee4b8 is entered from that wrapper:
 *   x30  == libil2cpp + 0xeac56c
 *   [sp] == return address of the original managed callsite
 *
 * We hook eee4b8 (whose first 16 bytes are verified and relocation-safe),
 * capture [sp] ONLY when x30 proves we arrived through eac564, then replay
 * the original prologue and continue at eee4c8. No exception behavior changes.
 */

#define T718_OFF_NRE_HELPER       0x00eee4b8u
#define T718_OFF_NRE_WRAPPER_RET  0x00eac56cu

volatile uint32_t  g_trace718_count = 0;
volatile uintptr_t g_trace718_caller_ret = 0;
uintptr_t          g_trace718_wrapper_ret_abs = 0;
uintptr_t          g_trace718_resume = 0;

static int g_trace718_installed = 0;
static uint32_t g_trace718_seen_count = 0;

__asm__(
".text\n"
".align 2\n"
".global sz_trace718_nre_trampoline\n"
".type sz_trace718_nre_trampoline,%function\n"
"sz_trace718_nre_trampoline:\n"

    /* Only [sp] has the original managed LR when this helper came from eac564. */
"  adrp x16, g_trace718_wrapper_ret_abs\n"
"  add  x16, x16, :lo12:g_trace718_wrapper_ret_abs\n"
"  ldr  x16, [x16]\n"
"  cmp  x30, x16\n"
"  b.ne 1f\n"

"  ldr  x17, [sp]\n"
"  adrp x16, g_trace718_caller_ret\n"
"  add  x16, x16, :lo12:g_trace718_caller_ret\n"
"  str  x17, [x16]\n"

"  adrp x16, g_trace718_count\n"
"  add  x16, x16, :lo12:g_trace718_count\n"
"  ldr  w17, [x16]\n"
"  add  w17, w17, #1\n"
"  str  w17, [x16]\n"

"1:\n"
    /* Original eee4b8 .. eee4c4. */
"  .word 0xd10083ff\n" /* sub sp,sp,#0x20 */
"  .word 0xa900fbff\n" /* stp xzr,x30,[sp,#8] */
"  .word 0x910003e0\n" /* mov x0,sp */
"  .word 0xf90003ff\n" /* str xzr,[sp] */

"  adrp x16, g_trace718_resume\n"
"  add  x16, x16, :lo12:g_trace718_resume\n"
"  ldr  x16, [x16]\n"
"  br   x16\n"
);

extern void sz_trace718_nre_trampoline(void);

static void trace718_install(void) {
    if (g_trace718_installed) return;

    const uintptr_t ib = (uintptr_t)il2cpp_mod.load_virtbase;
    if (!ib) {
        debugPrintf("[nre718] libil2cpp base unavailable; trace NOT installed\n");
        return;
    }

    const int ok_helper =
        trace713_sig4(ib, T718_OFF_NRE_HELPER,
                      0xd10083ffu, 0xa900fbffu, 0x910003e0u, 0xf90003ffu);

    /* Also verify the wrapper shape used to recover the original LR. */
    volatile uint32_t *w = (volatile uint32_t *)(ib + 0x00eac564u);
    const int ok_wrapper =
        (w[0] == 0xf81f0ffeu) && /* str x30,[sp,#-16]! */
        (w[2] == 0xf81f0ffeu);   /* next helper begins at eac56c */

    if (!ok_helper || !ok_wrapper) {
        debugPrintf("[nre718] signature mismatch helper=%d wrapper=%d; NOTHING patched\n",
                    ok_helper, ok_wrapper);
        return;
    }

    g_trace718_wrapper_ret_abs = ib + T718_OFF_NRE_WRAPPER_RET;
    g_trace718_resume = ib + T718_OFF_NRE_HELPER + 16u;

    trace713_patch_abs(ib, T718_OFF_NRE_HELPER,
                       (void *)&sz_trace718_nre_trampoline);

    g_trace718_installed = 1;
    debugPrintf("[nre718] NRE caller trace installed helper=+0x%x wrapper-ret=+0x%x\n",
                T718_OFF_NRE_HELPER, T718_OFF_NRE_WRAPPER_RET);
}

static void trace718_poll(void) {
    if (!g_trace718_installed) return;

    const uint32_t c = g_trace718_count;
    if (c == g_trace718_seen_count) return;

    const uintptr_t ib = (uintptr_t)il2cpp_mod.load_virtbase;
    const uintptr_t ret = g_trace718_caller_ret;
    uintptr_t callsite = 0;
    uintptr_t off = 0;

    if (ret >= 4u)
        callsite = ret - 4u; /* address of BL eac564 */
    if (ib && callsite >= ib)
        off = callsite - ib;

    if (c > g_trace718_seen_count + 1u) {
        debugPrintf("[nre718] coalesced %u NRE(s); showing latest\n",
                    c - g_trace718_seen_count);
    }

    debugPrintf("[nre718] #%u callsite=%p libil2cpp+0x%lx ret=%p "
                "loadscene_count=%u draws=%lu\n",
                c, (void *)callsite, (unsigned long)off, (void *)ret,
                g_trace713_loadscene_count, g_sz_last_frame_draws);

    g_trace718_seen_count = c;
}


/* R3.70.20: Switch-safe interstitial readiness.
 *
 * R3.70.18 + R3.70.19 proved:
 *   +0xfa230c = AndroidAgent::getBridge()
 *   +0xfa4830 = AndroidAgent::isInterstitialReady()
 *   fatal NRE callsite +0xfa48fc is inside isInterstitialReady(), reached when
 *   getBridge() returns NULL.
 *
 * Android/IronSource cannot provide that bridge in this Switch port, so the
 * correct platform-safe answer is simply "interstitial not ready".
 *
 * This avoids touching scene loading, LEVEL XX, input, Store/IAP, or rewards.
 */

#define T720_OFF_IS_INTERSTITIAL_READY 0x00fa4830u

static int g_fix720_installed = 0;

static void fix720_install(void) {
    if (g_fix720_installed) return;

    const uintptr_t ib = (uintptr_t)il2cpp_mod.load_virtbase;
    if (!ib) {
        debugPrintf("[fix720] libil2cpp base unavailable; fix NOT installed\n");
        return;
    }

    /* Exact Stupid Zombies 3.4.5 prologue:
     * fa4830: str x30,[sp,#-32]!
     * fa4834: stp x20,x19,[sp,#16]
     * fa4838: adrp x20,249c000
     * fa483c: adrp x19,2316000
     */
    const int ok =
        trace713_sig4(ib, T720_OFF_IS_INTERSTITIAL_READY,
                      0xf81e0ffeu, 0xa9014ff4u, 0x9000a7d4u, 0xd0009b93u);

    if (!ok) {
        debugPrintf("[fix720] signature mismatch at AndroidAgent.isInterstitialReady; "
                    "NOTHING patched\n");
        return;
    }

    /* bool false; return */
    const uint32_t code[2] = {
        0x52800000u, /* mov w0,#0 */
        0xd65f03c0u, /* ret */
    };

    so_patch_code((void *)(ib + T720_OFF_IS_INTERSTITIAL_READY),
                  code, sizeof(code));

    g_fix720_installed = 1;
    debugPrintf("[fix720] AndroidAgent.isInterstitialReady -> false "
                "(bridge unavailable on Switch) @+0x%x\n",
                T720_OFF_IS_INTERSTITIAL_READY);
}

static void trace713_install(void) {
    if (g_trace713_installed) return;

    const uintptr_t ib = (uintptr_t)il2cpp_mod.load_virtbase;
    if (!ib) {
        debugPrintf("[fade713] libil2cpp base unavailable; trace NOT installed\n");
        return;
    }

    const int ok_loadnext =
        trace713_sig4(ib, T713_OFF_LOAD_NEXT,
                      0xa9bf4ffeu, 0xb9802808u, 0x928eeec9u, 0xf2b11109u);
    const int ok_doloadnext =
        trace713_sig4(ib, T713_OFF_DO_LOAD_NEXT,
                      0xa9be57feu, 0xa9014ff4u, 0xd000a634u, 0xb0009a15u);
    const int ok_complete =
        trace713_sig4(ib, T713_OFF_LEVEL_COMPLETE,
                      0xf81e0ffeu, 0xa9014ff4u, 0xd000a634u, 0x394c5a88u);
    const int ok_loadscene =
        trace713_sig4(ib, T713_OFF_LOAD_SCENE,
                      0xf81e0ffeu, 0xa9014ff4u, 0xd000a634u, 0xf00099f3u);

    if (!ok_loadnext || !ok_doloadnext || !ok_complete || !ok_loadscene) {
        debugPrintf("[fade713] signature mismatch LN=%d DLN=%d LC=%d LS=%d; NOTHING patched\n",
                    ok_loadnext, ok_doloadnext, ok_complete, ok_loadscene);
        return;
    }

    g_trace713_page_249c000 = ib + 0x0249c000u;
    g_trace713_page_2317000 = ib + 0x02317000u;
    g_trace713_page_2315000 = ib + 0x02315000u;

    g_trace713_loadnext_resume   = ib + T713_OFF_LOAD_NEXT + 16u;
    g_trace713_doloadnext_resume = ib + T713_OFF_DO_LOAD_NEXT + 16u;
    g_trace713_complete_resume   = ib + T713_OFF_LEVEL_COMPLETE + 16u;
    g_trace713_loadscene_resume  = ib + T713_OFF_LOAD_SCENE + 16u;

    trace713_patch_abs(ib, T713_OFF_LOAD_NEXT, (void *)&sz_trace713_loadnext_trampoline);
    trace713_patch_abs(ib, T713_OFF_DO_LOAD_NEXT, (void *)&sz_trace713_doloadnext_trampoline);
    trace713_patch_abs(ib, T713_OFF_LEVEL_COMPLETE, (void *)&sz_trace713_complete_trampoline);
    trace713_patch_abs(ib, T713_OFF_LOAD_SCENE, (void *)&sz_trace713_loadscene_trampoline);

    g_trace713_installed = 1;
    debugPrintf("[fade713] trace installed LN=+0x%x DLN=+0x%x LC=+0x%x LS=+0x%x\n",
                T713_OFF_LOAD_NEXT, T713_OFF_DO_LOAD_NEXT,
                T713_OFF_LEVEL_COMPLETE, T713_OFF_LOAD_SCENE);
}

static void trace713_poll(void) {
    if (!g_trace713_installed) return;

    const uint32_t cur = g_trace713_seq;
    if (cur == g_trace713_last_printed_seq) return;

    if (g_trace713_complete_seq > g_trace713_last_printed_seq)
        debugPrintf("[fade713] seq=%u LevelComplete count=%u self=%p\n",
                    g_trace713_complete_seq, g_trace713_complete_count,
                    (void *)g_trace713_complete_self);

    if (g_trace713_loadnext_seq > g_trace713_last_printed_seq)
        debugPrintf("[fade713] seq=%u LoadNextLevel count=%u self=%p\n",
                    g_trace713_loadnext_seq, g_trace713_loadnext_count,
                    (void *)g_trace713_loadnext_self);

    if (g_trace713_doloadnext_seq > g_trace713_last_printed_seq)
        debugPrintf("[fade713] seq=%u DoLoadNextLevel count=%u self=%p\n",
                    g_trace713_doloadnext_seq, g_trace713_doloadnext_count,
                    (void *)g_trace713_doloadnext_self);

    if (g_trace713_loadscene_seq > g_trace713_last_printed_seq)
        debugPrintf("[fade713] seq=%u LoadLevelScene count=%u self=%p\n",
                    g_trace713_loadscene_seq, g_trace713_loadscene_count,
                    (void *)g_trace713_loadscene_self);

    g_trace713_last_printed_seq = cur;
}

void sz_result_probe_poll(void) {
    trace713_poll();
    trace715_poll();
    trace716_poll();
    trace718_poll();
    if (s_done) return;
    if (++s_tries < 120) return;
    if ((s_tries % 120) != 0) return;

    if (!resolve_exports()) {
        if (s_tries <= 600)
            debugPrintf("[result59] IL2CPP exports not ready yet\n");
        return;
    }

    void *domain = p_domain_get();
    if (!domain) return;

    size_t na = 0;
    const void **asms = p_domain_get_assemblies(domain, &na);
    if (!asms || !na) return;

    int hits = 0;
    debugPrintf("[fade712] scanning managed methods for level-entry/fade candidates\n");

    for (size_t ai = 0; ai < na; ++ai) {
        const void *img = p_assembly_get_image(asms[ai]);
        if (!img) continue;

        size_t nc = p_image_get_class_count(img);
        for (size_t ci = 0; ci < nc; ++ci) {
            void *klass = p_image_get_class(img, ci);
            if (!klass) continue;

            const char *cn = p_class_get_name(klass);
            const char *ns = p_class_get_namespace(klass);

            void *it = 0;
            void *mi;
            while ((mi = p_class_get_methods(klass, &it)) != 0) {
                const char *mn = p_method_get_name(mi);
                const int why = transition_candidate(ns, cn, mn);
                uintptr_t mp = mi ? *(const uintptr_t *)mi : 0;

                /* R3.70.19: exact address-map inventory around the two NRE
                 * callsites and the singleton getter. Diagnostic only. */
                const uintptr_t ib719 = (uintptr_t)il2cpp_mod.load_virtbase;
                const uintptr_t off719 =
                    (ib719 && mp >= ib719) ? (mp - ib719) : 0;
                const int addr719 =
                    off719 >= 0x00fa2000u && off719 < 0x00fa4b00u;

                if (!why && !addr719) continue;

                uint32_t pc = p_method_get_param_count(mi);

                if (addr719)
                    debugPrintf("[addr719] off=+0x%lx %s.%s::%s params=%u method=%p ptr=0x%lx\n",
                                (unsigned long)off719,
                                ns ? ns : "", cn ? cn : "?", mn ? mn : "?",
                                (unsigned)pc, mi, (unsigned long)mp);

                if (why == 9) {
                    /* R3.70.22: retain the real game's pause controls. */
                    if (cn && mn && !strcmp(cn, "Level") && pc == 0) {
                        if (!strcmp(mn, "PauseMenu")) {
                            s_level_pause_ptr = mp;
                            s_level_pause_mi = mi;
                            debugPrintf("[start722] captured Level.PauseMenu ptr=0x%lx mi=%p\n",
                                        (unsigned long)mp, mi);
                        } else if (!strcmp(mn, "ResumeGameFromPause")) {
                            s_level_resume_ptr = mp;
                            s_level_resume_mi = mi;
                            debugPrintf("[start722] captured Level.ResumeGameFromPause ptr=0x%lx mi=%p\n",
                                        (unsigned long)mp, mi);
                        }
                    }

                    debugPrintf("[inv717] %s.%s::%s params=%u method=%p ptr=0x%lx\n",
                                ns ? ns : "", cn ? cn : "?", mn ? mn : "?",
                                (unsigned)pc, mi, (unsigned long)mp);
                }

                if (why)
                    debugPrintf("[fade712] CAND kind=%d %s.%s::%s params=%u method=%p ptr=0x%lx\n",
                                why, ns ? ns : "", cn ? cn : "?", mn ? mn : "?",
                                (unsigned)pc, mi, (unsigned long)mp);

                hits++;
                if (hits >= 700) goto done;
            }
        }
    }

done:
    debugPrintf("[fade712] candidate scan complete hits=%d\n", hits);
    trace713_install();
    trace715_install();
    trace716_install();
    trace718_install();
    fix720_install();
    s_done = 1;
}
