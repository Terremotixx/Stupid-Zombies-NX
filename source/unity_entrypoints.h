/* unity_entrypoints.h -- UnityPlayer native methods recovered from
 * libunity.so  (ANGRY BIRDS RELOADED, com.rovio.reloadedport 2.2.16218,
 * Unity 2022.3.7f1 build b16b3b16c7a0, arm64 / IL2CPP).
 *
 * Regenerate with:  python3 tools/extract_entrypoints.py libunity.so
 *
 * IMPORTANT: these offsets are LINK-TIME addresses for THIS EXACT libunity.so.
 * They are NOT portable between games or even between Unity patch releases,
 * and getting that wrong does not fail politely -- the loader calls
 * load_virtbase + offset, so a stale offset calls into the MIDDLE OF AN
 * UNRELATED FUNCTION.
 *
 * That is exactly what happened on this port's first boot. The tree was forked
 * from the Fruit Ninja Classic+ port (Unity 2022.3.0f1), whose JNI_OnLoad sits
 * at 0x60b2e0. In THIS build JNI_OnLoad is at 0x9ca390 -- so
 * "calling JNI_OnLoad(fake_vm)" jumped into unrelated code, which ran on until
 * it called an as-yet-unbound il2cpp function pointer and branched to address
 * 0 (esr=0x82000005: instruction abort, pc=0). The lesson is in the crash
 * shape: an instruction abort at pc=0 during a hand-resolved entry point means
 * a WRONG OFFSET, not a missing shim.
 *
 * HOW THESE WERE RECOVERED
 *   Unity registers these natives with RegisterNatives from JNINativeMethod[]
 *   tables -- {const char *name, const char *sig, void *fnPtr} triples. In a
 *   PIC shared object all three fields are filled in at load time by
 *   R_AARCH64_RELATIVE relocations, so the tables are readable straight out of
 *   .rela.dyn without disassembling or running anything. In this build they
 *   live in .data (not .data.rel.ro).
 *
 *   The scan finds seven such tables; the 26-entry one at 0x179be78 is the
 *   UnityPlayer drive surface reproduced below. The other six are ARCore (3),
 *   audio-volume/orientation-lock (2), HFP status (3), Camera2 (4), managed
 *   proxy helpers (3) and FMOD (3) -- none of which this port invokes.
 *
 *   Cross-checks: JNI_OnLoad is an EXPORTED symbol here, so its offset comes
 *   from .dynsym rather than the table scan and agrees independently; and all
 *   26 natives land in 0x9c956c..0x9ca338, contiguous with JNI_OnLoad at
 *   0x9ca390, which is the layout you expect when they are all emitted from
 *   the same translation unit.
 *
 *   Every signature was checked against the typedefs at the bottom of this
 *   file: initJni (Landroid/content/Context;)V and nativeRecreateGfxState
 *   (ILandroid/view/Surface;)V match fn_initJni / fn_gfxstate, nativeRender
 *   ()Z matches fn_z, nativeInjectEvent (Landroid/view/InputEvent;)Z matches
 *   fn_inject, and so on -- so the calling convention is unchanged from the
 *   base port even though every address moved.
 *
 * Runtime address = unity_mod.load_virtbase + offset (the .so links at base 0).
 */
#ifndef UNITY_ENTRYPOINTS_H
#define UNITY_ENTRYPOINTS_H

#include <stdint.h>
#include "so_util.h"

/* ---- UnityPlayer native method offsets (link-time vaddr) ---------------- */
/* JNI_OnLoad */
#define OFF_JNI_OnLoad                    0x9ca390 /* (JavaVM*,reserved)->jint  caches VM, registers natives */
/* drive-critical */
#define OFF_initJni                       0x9c956c /* (env,thiz,Context)                  */
#define OFF_nativeRecreateGfxState        0x9c97bc /* (env,thiz,int,Surface)  set surface */
#define OFF_nativeSendSurfaceChangedEvent 0x9c9824 /* (env,thiz)                          */
#define OFF_nativeRender                  0x9c987c /* (env,thiz)->Z   per-frame; false=stop */
#define OFF_nativeInjectEvent             0x9c98dc /* (env,thiz,InputEvent,int)->Z input  */
#define OFF_nativePause                   0x9c9608 /* (env,thiz)->Z                       */
#define OFF_nativeResume                  0x9c966c /* (env,thiz)                          */
#define OFF_nativeFocusChanged            0x9c974c /* (env,thiz,Z)                        */
#define OFF_nativeDone                    0x9c9578 /* (env,thiz)->Z   shutdown            */
#define OFF_nativeApplicationUnload       0x9c96fc /* (env,thiz)                          */
#define OFF_nativeLowMemory               0x9c96b4 /* (env,thiz)                          */
#define OFF_nativeOrientationChanged      0x9ca2d8 /* (env,thiz,int,int)                  */
/* secondary / usually unused for a port */
#define OFF_nativeUnitySendMessage        0x9c9eec /* (env,thiz,String,String,byte[])     */
#define OFF_nativeMuteMasterAudio         0x9ca0fc /* (env,thiz,Z)                        */
#define OFF_nativeGetNoWindowMode         0x9ca338 /* (env,thiz)->Z                       */
#define OFF_nativeIsAutorotationOn        0x9ca09c /* (env,thiz)->Z                       */
#define OFF_nativeSetLaunchURL            0x9ca1a0 /* (env,thiz,String)                   */
#define OFF_nativeRestartActivityIndicator 0x9ca158 /* (env,thiz)   -- present in this build */
/* soft keyboard (route via SoftInputProvider stub; not needed for first boot) */
#define OFF_nativeSetInputArea            0x9c9bd4
#define OFF_nativeSetKeyboardIsVisible    0x9c9c54
#define OFF_nativeSetInputString          0x9c9cac
#define OFF_nativeSetInputSelection       0x9c9d4c
#define OFF_nativeSoftInputClosed         0x9c9e9c
#define OFF_nativeSoftInputCanceled       0x9c9db4
#define OFF_nativeSoftInputLostFocus      0x9c9e04
#define OFF_nativeReportKeyboardConfigChanged 0x9c9e54
/* This engine has nativeSendSurfaceChangedEvent but not the later
 * nativeSendSurfaceChanged. The core only ever calls the *Event form; the
 * alias keeps any stray reference compiling. */
#define OFF_nativeSendSurfaceChanged      OFF_nativeSendSurfaceChangedEvent

/* nativeHidePreservedContent is not in this build's table either (the scan
 * finds 26 natives and it is not among them). The loader core
 * declares it but never calls it. Resolving it to 0 makes an accidental call
 * fail loudly instead of jumping into the middle of an unrelated function. */
#define OFF_nativeHidePreservedContent    0x0      /* ABSENT in 2022.3.7f1 -- do not call */
#define ABR_HAVE_HIDE_PRESERVED_CONTENT    0

/* ---- JNI native signatures: ret (*)(JNIEnv*, jobject thiz, args...) ----- */
typedef void     (*fn_initJni)(void*,void*,void*);
typedef void     (*fn_gfxstate)(void*,void*,int32_t,void*);
typedef void     (*fn_v)(void*,void*);
typedef uint8_t  (*fn_z)(void*,void*);
typedef void     (*fn_vz)(void*,void*,int32_t);
typedef uint8_t  (*fn_inject)(void*,void*,void*,int32_t);
typedef void     (*fn_orient)(void*,void*,int32_t,int32_t);

#define UNITY_RESOLVE(mod, off) ((void*)((uintptr_t)(mod).load_virtbase + (off)))

/* ===========================================================================
 * Drive sequence (what the Java UnityPlayer does; you do it in main.c):
 *
 *   initJni(env, thiz, fake_context);                  // early init
 *   nativeRecreateGfxState(env, thiz, 0, fake_surface);// give it the surface
 *   nativeSendSurfaceChangedEvent(env, thiz);          // engine builds GL state
 *   for (;;) {
 *       // input: nativeInjectEvent(env,thiz, motionEvent, deviceId);
 *       if (!nativeRender(env, thiz)) break;           // false == engine wants out
 *   }
 *   nativeApplicationUnload(env, thiz);  nativeDone(env, thiz);
 *
 * NOTE on input: nativeInjectEvent takes a Java InputEvent/MotionEvent jobject,
 * which the engine then queries back via JNI (getActionMasked/getX/getY/
 * getPointerId/getPointerCount...). Fruit Ninja is a swipe game driven ENTIRELY
 * by this path -- it is the single most load-bearing subsystem in the port.
 * Sub-frame swipe sampling matters here in a way it did not for PvZ: the blade
 * trail is built from the MotionEvent history, so feeding one point per frame
 * gives coarse, "steppy" slices. See PORTING_FRUITNINJA.md sec 8.
 * =========================================================================== */

/* ---- Non-UnityPlayer native tables also present in this build (FYI) -------
 * We do NOT register/drive these; listed only so nobody re-hunts them.
 *   choreographer   nOnChoreographer                     @0xb458ec
 *   swappy          nOnRefreshPeriodChanged              @0xb47bec
 *                   nSetSupportedRefreshPeriods          @0xb47a0c
 *   ARCore          initializeARCore/pause/resume        @0x5e34e4/0x5e3548/0x5e359c
 *   Camera2         initCamera2Jni/deinit                @0x6035fc/0x603648
 *                   nativeFrameReady/SurfaceTextureReady @0x607b38/0x6079d0
 *   HFP audio       initHFPStatusJni/deinit              @0x5e61b0/0x5e61fc
 *   audio volume    onAudioVolumeChanged                 @0x5ec484
 *   query status    nativeStatusQueryResult              @0x5e53dc
 *   orient lock     nativeUpdateOrientationLockState     @0x5ec5c0
 *
 * (This build does NOT register nativeGetSoftInputType, which the 2022.3.62
 *  builds do. Nothing in the loader calls it.)
 * -------------------------------------------------------------------------- */

#endif /* UNITY_ENTRYPOINTS_H */
