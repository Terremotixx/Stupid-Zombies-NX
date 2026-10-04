/*
 * Stupid Zombies 3.4.5 -- UnityPlayer JNI entry points
 * Game: com.gameresort.stupidzombies
 * Unity: 2022.3.19f1 (244b723c30a6), ARM64, IL2CPP
 *
 * GENERATED FROM THIS EXACT libunity.so. Do not reuse these offsets with any
 * other game/version. Runtime address = unity_mod.load_virtbase + offset.
 */
#ifndef UNITY_ENTRYPOINTS_H
#define UNITY_ENTRYPOINTS_H

#include <stdint.h>
#include "so_util.h"

#define OFF_JNI_OnLoad                         0x6018fc
#define OFF_initJni                            0x600aac
#define OFF_nativeDone                         0x600ab8
#define OFF_nativePause                        0x600b48
#define OFF_nativeResume                       0x600bac
#define OFF_nativeLowMemory                    0x600bf4
#define OFF_nativeApplicationUnload            0x600c3c
#define OFF_nativeFocusChanged                 0x600c8c
#define OFF_nativeRecreateGfxState             0x600ce0
#define OFF_nativeSendSurfaceChangedEvent      0x600d48
#define OFF_nativeRender                       0x600da0
#define OFF_nativeInjectEvent                  0x600e00
#define OFF_nativeSetInputArea                 0x6010f8
#define OFF_nativeSetKeyboardIsVisible         0x601178
#define OFF_nativeSetInputString               0x6011d0
#define OFF_nativeSetInputSelection            0x601270
#define OFF_nativeSoftInputCanceled            0x6012d8
#define OFF_nativeSoftInputLostFocus           0x601328
#define OFF_nativeReportKeyboardConfigChanged  0x601378
#define OFF_nativeSoftInputClosed              0x6013c0
#define OFF_nativeUnitySendMessage             0x601410
#define OFF_nativeIsAutorotationOn             0x6015c0
#define OFF_nativeMuteMasterAudio              0x601620
#define OFF_nativeRestartActivityIndicator     0x60167c
#define OFF_nativeSetLaunchURL                 0x6016c4
#define OFF_nativeHidePreservedContent         0x6017fc
#define OFF_nativeOrientationChanged           0x601844
#define OFF_nativeGetNoWindowMode              0x6018a4

/* Registered by the engine as well. The current wrapper does not need to call
 * these directly for first boot, but keeping exact RVAs here makes later
 * Choreographer/Swappy work reproducible. */
#define OFF_nOnChoreographer                   0xb57780
#define OFF_nSetSupportedRefreshPeriods        0xb598a0
#define OFF_nOnRefreshPeriodChanged            0xb59a80

/* Compatibility name used by the abreloaded loader core. */
#define OFF_nativeSendSurfaceChanged OFF_nativeSendSurfaceChangedEvent

/* JNI native signatures expected by the current loader core. AArch64 permits
 * the loader's historical extra deviceId argument to nativeInjectEvent; this
 * game's registered JNI signature itself is (InputEvent)Z. */
typedef void     (*fn_initJni)(void*,void*,void*);
typedef void     (*fn_gfxstate)(void*,void*,int32_t,void*);
typedef void     (*fn_v)(void*,void*);
typedef uint8_t  (*fn_z)(void*,void*);
typedef void     (*fn_vz)(void*,void*,int32_t);
typedef uint8_t  (*fn_inject)(void*,void*,void*,int32_t);
typedef void     (*fn_orient)(void*,void*,int32_t,int32_t);

#define UNITY_RESOLVE(mod, off) ((void*)((uintptr_t)(mod).load_virtbase + (off)))

#endif /* UNITY_ENTRYPOINTS_H */
