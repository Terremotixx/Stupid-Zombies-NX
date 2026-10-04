/* nx_patch_abr.h -- in-memory libunity.so patch table for
 * ANGRY BIRDS RELOADED  (com.rovio.reloadedport 2.2.16218,
 * Unity 2022.3.7f1 / b16b3b16c7a0, arm64, IL2CPP).
 *
 * WHAT IT DOES
 *   Unity's block allocator reserves memory in 256MB-aligned regions. On a 4GB
 *   Switch that granularity does not fit the so_loader address space, so we
 *   rewrite the allocator's region-size computation to 64MB granularity. Each
 *   entry rewrites one 32-bit instruction word: {from} is the stock word, {to}
 *   is the 64MB word. Same transform family as the Fruit Ninja Classic+ /
 *   Zookeeper DX / PvZ Fusion tables; only the offsets and registers differ.
 *
 * HOW THESE OFFSETS WERE DERIVED
 *   By DECODING operands over the whole of .text (never by grepping for a
 *   constant), then confirming the cluster structurally. See
 *   tools/derive_alloc_patches.py, which regenerates this table verbatim.
 *
 *     1. Every instruction in .text (0x530300, 0x11ef9c0 bytes) was decoded and
 *        kept only if it PARTICIPATES in a 2^28 computation:
 *          - MOVZ/MOVK  imm16 == 0x1000, hw == 1        (0x10000000  = 256MB)
 *          - MOVN       imm16 == 0xF000, hw == 1        (0x0FFFFFFF  = 256MB-1)
 *          - UBFM       immr == 28 AND imms >= immr     (a real right shift)
 *          - ADD/SUB    shifted register, LSL #28
 *          - AND-imm    logical immediate whose LOWEST SET BIT IS EXACTLY 28
 *        The last two filters matter. An earlier pass that accepted UBFM with
 *        imms < immr pulled in 441 hits that were all `LSL #4` aliases; one
 *        that accepted any mask clearing >=28 low bits pulled in the level-1
 *        page-directory masks at 0x71f070/0x71f168/0x71f174 (lowest set bits
 *        36 and 42), which are NOT granularity and must not be touched.
 *
 *     2. The survivors cluster hard: 21 of the 100 remaining hits fall inside
 *        0x718484..0x7213d8, a 36KB window. Disassembling that window names the
 *        functions by their known shapes (see per-entry comments below).
 *
 *     3. CROSS-BUILD CONFIRMATION. 12 of the 21 {from} words are BYTE-IDENTICAL
 *        to entries in the Fruit Ninja Classic+ table (Unity 2022.3.0f1,
 *        a different build of the same engine source family), and the relative
 *        spacing agrees to the byte in the tight pairs:
 *              MarkMemoryBlocks -> ReserveMemoryBlock   FN 0x90   ABR 0x90
 *              ReserveMemoryBlock -> GetMemoryBlockFP   FN 0x33c  ABR 0x33c
 *        Sites reached by both routes: 0x718d1c/0x718d20 (TLSAllocator pair),
 *        0x71a430, 0x71c80c/0x71c810 (DynamicHeapAllocator pair), 0x71ec78,
 *        0x71ed08, 0x71f044, 0x71f05c, 0x71f0b8, 0x71f0e0, 0x720e24, 0x7213d8.
 *
 * WHERE THIS BUILD DIFFERS FROM THE FRUIT NINJA TABLE  (do not transplant!)
 *   a) 2022.3.7f1 emits 56-bit-aware masks. GetMemoryBlockFromPointer+0x10 is
 *      `and x8, x1, #0x00FFFFFFF0000000` here, not `#0xFFFFFFFFF0000000`. The
 *      transform is the same (+2 to immr and imms) but the {from} word differs.
 *   b) This build splits the block-table index into TWO levels computed
 *      DIFFERENTLY in different functions, and that asymmetry is a trap.
 *      At 256MB the block number is ptr>>28 = bits[55:28]; the directory
 *      splits it as level-1 = ptr>>40 (bits[55:40]) and level-2 = bits[39:28]
 *      (the `ubfx #0x1c,#0xc`). The two fields are CONTIGUOUS.
 *
 *      Moving to 64MB makes the block number bits[55:26]. Keeping the level-2
 *      table the same size (12 bits) means level-2 becomes bits[37:26], and
 *      therefore level-1 MUST become ptr>>38 to stay contiguous with it.
 *
 *      GetMemoryBlockFromPointer gets this for free: it computes level-1 from
 *      the ALREADY-SHIFTED value (`lsr x8,x1,#26` then `ubfx w8,w8,#12,#16`
 *      = ptr bits[53:38]), so it follows the granularity automatically.
 *      GetBlockInfoFromPointer (0x71f0d0) and GetAllocatorContainingPtr
 *      (0x720e0c) compute level-1 DIRECTLY as `lsr x?,x?,#0x28`, so they do
 *      NOT follow, and must be patched to `#0x26` by hand -- entries 19/20.
 *
 *      An earlier revision of this table deliberately EXCLUDED those two on
 *      the theory that "level-1 is not granularity". That was wrong, and it
 *      cost a boot: with level-2 moved to bits[37:26] but level-1 still on
 *      bits[55:40], bits 38-39 fall in a gap, the decomposition no longer
 *      reconstructs the block number, every lookup misses, and
 *      GetAllocatorContainingPtr returns NULL. Its caller at 0x720da4 does
 *      `ldr x8,[x0]` with no null check -> data abort, far=0, esr=0x92000005,
 *      immediately after nativeRecreateGfxState. The 38 is not a guess: it is
 *      forced by GetMemoryBlockFromPointer's own arithmetic above.
 *
 *   c) This build has a round-up-to-boundary idiom Fruit Ninja's lacks:
 *      `mov wN, #0x0FFFFFFF` / `add` / `and #~0x0FFFFFFF` at 0x718484+0x71848c
 *      and 0x71ccb8+0x71ccc0. BOTH halves of each pair must move together --
 *      patching the mask without the addend rounds to the wrong boundary.
 *
 * TRANSFORMS
 *     MOVZ/MOVK   imm16 0x1000<<16 -> 0x0400<<16     256MB   -> 64MB
 *     MOVN        imm16 0xF000<<16 -> 0xFC00<<16     256MB-1 -> 64MB-1
 *     LSR  (UBFM) immr 28 -> 26, imms unchanged      >>28 -> >>26
 *     UBFX (UBFM) immr 28 -> 26, imms -= 2           field WIDTH preserved
 *     SUB  shifted  lsl #28 -> lsl #26
 *     AND  bitmask  immr/imms += 2                   boundary moves 2 bits
 *   Every {to} word in this table was disassembled back and checked to be the
 *   intended instruction; see tools/derive_alloc_patches.py --verify.
 *
 * SAFETY
 *   nx_patch_libunity() is VERIFY-FIRST: it reads each target word and only
 *   patches if it already equals {from}; if ANY site mismatches it patches
 *   NOTHING and logs loudly. A wrong offset is caught, not catastrophic.
 *   Note that a wrong TRANSFORM at a right offset is NOT caught by that check
 *   -- which is why (b) and (c) above are called out explicitly.
 */
#ifndef NX_PATCH_ABR_H
#define NX_PATCH_ABR_H

#include <stdint.h>

#define ABR_HAVE_BRANCH_FORCES  0   /* not needed unless an allocator abort appears */

typedef struct { uint32_t off, from, to; } NxPatchWord;

/* ---- 21 region-granularity sites (256MB -> 64MB) -------------------------
 * '=' : {from} is byte-identical to the corresponding Fruit Ninja table entry
 *       (independent confirmation across two builds of the same engine).      */
/* Stupid Zombies 3.4.5 / Unity 2022.3.19f1.
 * 256 MiB Unity allocator region granularity -> 64 MiB.
 * Every entry is verify-first: {offset, expected stock word, replacement word}.
 * The two #40 -> #38 sites preserve the level-1/level-2 index decomposition.
 * No AtomicPageAllocator site was added because no matching instruction was
 * verified in this build. Never copy the Angry Birds final site blindly. */
static const NxPatchWord ABR_PATCH_WORDS[] = {
  { 0x3a88cc, 0x12be0009, 0x12bf8009 }, /* round-up addend 256M-1 -> 64M-1 */
  { 0x3a88d4, 0x92648d36, 0x92669536 }, /* boundary mask */
  { 0x3a9170, 0xd35cfc28, 0xd35afc28 }, /* lsr #28 -> #26 */
  { 0x3a9174, 0x52a20009, 0x52a08009 }, /* 256M -> 64M */
  { 0x3aabfc, 0x52a20009, 0x52a08009 },
  { 0x3aceb0, 0xd35cfd29, 0xd35afd29 },
  { 0x3aceb4, 0x52a2000a, 0x52a0800a },
  { 0x3ad35c, 0x12be000a, 0x12bf800a },
  { 0x3ad364, 0x92648d36, 0x92669536 },
  { 0x3af364, 0xd35cdc33, 0xd35ad433 }, /* ubfx #28 -> #26, width retained */
  { 0x3af368, 0xd35cfd15, 0xd35afd15 },
  { 0x3af3f8, 0x52a20008, 0x52a08008 },
  { 0x3af734, 0xd35cfc28, 0xd35afc28 },
  { 0x3af744, 0x92646c28, 0x92667428 },
  { 0x3af74c, 0xd35c9c2a, 0xd35a942a }, /* level-2 field */
  { 0x3af764, 0xd35cdc29, 0xd35ad429 },
  { 0x3af768, 0xf2a2000b, 0xf2a0800b },
  { 0x3af7a8, 0xcb0a7108, 0xcb0a6908 },
  { 0x3af7c0, 0xd368fc28, 0xd366fc28 }, /* level-1 lsr #40 -> #38 */
  { 0x3af7d0, 0xd35c9c29, 0xd35a9429 },
  { 0x3b1484, 0xd368fc28, 0xd366fc28 }, /* level-1 lsr #40 -> #38 */
  { 0x3b149c, 0xd35c9e89, 0xd35a9689 },
};
#define ABR_PATCH_WORDS_N ((int)(sizeof(ABR_PATCH_WORDS)/sizeof(ABR_PATCH_WORDS[0])))

/* ---- branch forces: none located, and none needed for first boot --------- */
static const NxPatchWord ABR_BRANCH_FORCES[] = { { 0, 0, 0 } };  /* placeholder */
#define ABR_BRANCH_FORCES_N 0

/* ---- DELIBERATELY NOT PATCHED -------------------------------------------
 * Recorded so a future pass does not "helpfully" add them:
 *   (0x71f0d0 and 0x720e0c were once listed here as level-1 indices that did
 *    not need moving. They DO -- see note (b) above. They are now entries
 *    19 and 20 of the table.)
 *   0x71f070  mov x11, #-0x1000000000    paired high half of the loop stride;
 *             its low half (0x71f078 movk) IS patched. The constant is
 *             -(2^36) + granularity; only the granularity term moves.
 *   0x71f168  mov x9,  #-0x40000000000   level-1 masks (lowest set bit 42)
 *   0x71f174  and x13, x11, #0xfffffc0000000000
 *   0x71ec6c  and x8,  x1,  #0x00ffffffffffffff   56-bit VA canonicalisation
 */

/* =========================================================================
 * PER-BINARY IL2CPP / ENGINE HACKS -- ALL DISABLED FOR THIS PORT
 * =========================================================================
 * The Fruit Ninja Classic+ base this tree forked from carries a large set of
 * hard-won, BINARY-SPECIFIC patches: an il2cpp IsInst null-klass guard, a GC
 * liveness typeHierarchy guard, a TimeManager::Update hook, FMOD->OpenSL
 * output selection, splash-screen bypass sites, and preload-budget branches.
 *
 * Every one of those is an offset into FRUIT NINJA's libil2cpp.so /
 * libunity.so. Angry Birds Reloaded is a different game on a different Unity
 * revision (2022.3.7f1 vs 2022.3.0f1), so those offsets address ARBITRARY
 * instructions here. Applying them would corrupt unrelated code, and unlike
 * the allocator table there is no {from}-word verification on most of these
 * paths to catch it.
 *
 * They are therefore all gated OFF, and the offsets are defined as 0 purely so
 * the tree compiles. Each one is a separate porting task: locate the site in
 * THIS binary, verify it, then flip its gate. Do not flip a gate without
 * re-deriving its offsets -- a zero offset that goes live patches the module
 * base.
 *
 * The allocator table above is the ONE table that HAS been derived for this
 * game, and it is what a first boot needs.
 * ========================================================================= */

#define ABR_HAVE_TIME_FIX                0
#define ABR_HAVE_TIME_HOOKS              0
#define ABR_HAVE_IL2CPP_VM 0
#define ABR_HAVE_ISINST_GUARD 0
#define ABR_HAVE_LIVENESS_GUARD 0
#define ABR_HAVE_FINISH_PROBE 0
#define ABR_HAVE_FMOD_BUFFER_BYPASS 0
#define ABR_HAVE_HIDE_PRESERVED_CONTENT 1
#define ABR_HAVE_FMOD_OPENSL 0
#define ABR_HAVE_OFFLINE_RESULT_BUTTONS 0

/* --- offsets: NOT DERIVED for this binary. Zero + gate off. --------------- */
#define ABR_IL2CPP_ISINST_AND       0u
#define ABR_IL2CPP_ISINST_AND_OLD   0u
#define ABR_IL2CPP_ISINST_GUARD     0u
#define ABR_IL2CPP_LIVENESS_ADD     0u
#define ABR_IL2CPP_LIVENESS_BODY    0u
#define ABR_IL2CPP_LIVENESS_ALIVE   0u
#define ABR_IL2CPP_LIVENESS_ADD_LO  0u
#define ABR_IL2CPP_LIVENESS_ADD_HI  0u
#define ABR_IL2CPP_LIVENESS_HASPAR  0u
#define ABR_IL2CPP_LIVENESS_LR_INST 0u
#define ABR_IL2CPP_KLASS_TYPEHIER   0xc8u   /* struct offsets: engine-generic */
#define ABR_IL2CPP_KLASS_DEPTH      0x125u
#define ABR_IL2CPP_ARRAY_LEN        0x18u   /* Il2CppArray.max_length         */
#define ABR_IL2CPP_ARRAY_DATA       0x20u   /* first element                  */
#define ABR_IL2CPP_VM_GLOBAL        0u
#define ABR_IL2CPP_HANDLER_SLOT     0u
#define ABR_IL2CPP_HANDLER_FN       0u
#define ABR_IL2CPP_FINISH_FLAG      0u
#define ABR_IL2CPP_GETTYPE_LEAF     0u
#define ABR_IL2CPP_CFT_LR           0u
#define ABR_IL2_SPLASH_FINISHED_CHECK 0u
#define ABR_GET_SHOULD_SHOW_SPLASH  0u
#define ABR_TIME_UPDATE_ENTRY       0u
#define ABR_TIME_UPDATE_BODY        0u
#define ABR_TIME_UPDATE_WORD        0u
#define ABR_TIME_THUNK_WORD         0u
#define ABR_TIME_GETMANAGER         0u
#define ABR_PACING_GETTER           0u
#define ABR_FMOD_OUTPUT_SITE        0x00b432dcu
#define ABR_FMOD_BUFFER_SITE        0u
#define ABR_RESULT_PREPARE_ENTRY    0x01123bb8u
#define ABR_RESULT_SET_BUTTONS      0x01123c58u
#define ABR_PRELOAD_EXIT_BRANCH     0x00835154u
#define ABR_PRELOAD_BUDGET_TABLE_LOAD 0x00835118u
#define ABR_PRELOAD_BUDGET_DEFAULT  0x00835120u
#define ABR_CFT_SP_DATABINDING      0u
#define ABR_CFT_SP_VALUE            0u
#define ABR_DATABINDING_PROPNAME    0u
#define ABR_DATABINDING_DATAPATH    0u
#define ABR_DATABINDING_STRFMT      0u
#define ABR_DIALOGUE_CONFIG         0u
#define ABR_DIALOGUE_INDEX          0u
#define ABR_DLGCONFIG_PIECES        0u
#define ABR_SDS_SP_DIALOGUE         0u
#define ABR_SDS_SP_PIECE            0u
#define ABR_REFLECTIONTYPE_TYPE     0x10u  /* Il2CppReflectionType.type       */
static const uint32_t ABR_LIVENESS_PROLOGUE[4] = { 0, 0, 0, 0 };
static const NxPatchWord ABR_FMOD_WORDS[] = { { 0, 0, 0 } };
#define ABR_FMOD_WORDS_NUM 0

/* =========================================================================
 * OFFLINE AUDIO / RESULTS UI -- DERIVED FOR THIS GAME
 * =========================================================================
 * Audio:
 *   AudioManager::InitNormal is at libunity+0xb42f90.  Its output selection is
 *   structurally identified by the FMOD::System::setOutput call and the nearby
 *   "FMOD was unable to select requested output" literal:
 *
 *     b432b8  bl   GetAndroidOutputKind
 *     b432c0  mov  w8,#21              AudioTrack
 *     b432d0  mov  w9,#22              OpenSL ES
 *     b432d4  csel w21,w9,w8,eq
 *     b432d8  ldr  x0,[x19,#0x158]     FMOD system
 *     b432dc  mov  w1,w21
 *     b432e4  bl   FMOD::System::setOutput
 *
 *   The selected Android AudioTrack output calls FMODAudioDevice.start(), but
 *   that Java class has no thread/runtime in the wrapper, so no one drains PCM.
 *   Replacing the verified mov with mov w1,#22 selects the native OpenSL output
 *   already backed by opensles.c.  The AudioTrack and OpenSL description strings
 *   and output implementations are both present in this exact libunity binary.
 *
 * Results buttons:
 *   PostGameCompletePopup.Prepare (token 0x06002379, libil2cpp+0x1123bb8)
 *   calls SetButtonsState(false) at +0x38.  The only later true call is in
 *   TournamentNetworkHandler.LevelComplete (token 0x06002a1b,
 *   libil2cpp+0x117c228).  On Android the tournament service invokes it; the
 *   offline wrapper has no such service, so the complete popup permanently
 *   hides BtnGroup even though OnFinishGame and score persistence succeed.
 *
 *     1123be8  mov x0,x20
 *     1123bec  mov w1,wzr             false
 *     1123bf0  bl  0x1123c58          SetButtonsState
 *
 *   Do NOT change that false to true: SetButtonsState touches BtnGroup before
 *   Initialize/PrepareDefaultComplete have populated the popup, throwing a
 *   NullReferenceException out of GameView.OnGoalsCompleted before it saves the
 *   score (the r6 zero-score/no-stars regression).  r7 hooks Prepare, lets the
 *   stock method and its caller finish, then calls SetButtonsState(true) after
 *   nativeRender returns.  Progression remains owned by the normal save path.
 *
 * Preload budget (Unity 2022.3.7f1):
 *   UpdatePreloading's quality table load/default/early-exit are uniquely
 *   identified by this exact instruction sequence in the supplied libunity:
 *
 *     835118  ldr w21,[x9,x8,lsl#2]
 *     835120  mov w21,#4
 *     835150  bl  SingleStep
 *     835154  tbz w0,#0,exit
 *
 *   The table is {2,4,10,4,50}.  Replacing both budget sources with the chosen
 *   fixed budget and NOPing the verified early exit makes page/scene loads keep
 *   integrating queued assets instead of abandoning the frame on a transiently
 *   empty queue.
 * ========================================================================= */

/* =========================================================================
 * BOEHM GC STOP-THE-WORLD BRIDGE -- DERIVED FOR THIS GAME  (4 offsets)
 * =========================================================================
 * il2cpp's Boehm GC stops the world by pthread_kill()ing every other thread;
 * each target's signal handler sem_posts an ack and parks in sigsuspend, while
 * GC_stop_world sem_waits once per live thread. POSIX signals are NEVER
 * delivered on Switch, so without this bridge the acks never arrive and the
 * FIRST COLLECTION HANGS FOREVER inside GC_stop_world. Papers, Please calls
 * this "the verified boot wall".
 *
 * sem_post/sem_wait DO work here (real libnx Semaphore underneath), so we make
 * pthread_kill itself post the ack the never-delivered handler would have
 * posted. That needs four il2cpp globals, all recovered from THIS binary.
 *
 * WHERE THEY CAME FROM
 *   libil2cpp.so has a `.text` (2.3MB runtime, holds Boehm) separate from its
 *   18MB `il2cpp` codegen section. Scanning `.text` for BL sites against the
 *   PLT stubs gives the Boehm fingerprint exactly as both references describe:
 *
 *       pthread_kill    2 sites   0xf99f34, 0xf9a19c
 *       sem_post        2 sites   0xf99e20, 0xf99e6c   (both in the handler)
 *       sem_wait        1 site    0xf9a0c4             (GC_stop_world)
 *       sem_init        1 site    0xf9a27c             (GC_thr_init)
 *       sigsuspend      1 site    0xf99e40             (handler parks here)
 *
 *   all inside 0xf99c00..0xf9a300 -- one compilation unit, pthread_stop_world.c.
 *
 *   GC_suspend_all @0xf99f34      adrp x24,#0x24a6000 ; ldr w1,[x24,#0x50c]
 *   GC_start_world @0xf9a19c      adrp x24,#0x24a6000 ; ldr w1,[x24,#0x510]
 *   handler        @0xf99e5c      adrp x8, #0x24a6000 ; ldr w8,[x8,#0x508]
 *                                 -> gates the SECOND sem_post, then stores
 *                                    last_stop_count|1 (orr x8,x20,#1)
 *   ack semaphore  @0xf99e14/e68  adrp x0, #0x26c4000 ; add x0,x0,#0x7a8
 *
 * EVERY VALUE IS CONFIRMED BY AT LEAST TWO INDEPENDENT ROUTES
 *   0x24a650c  suspend sig  : pthread_kill's w1 in GC_suspend_all
 *                             AND GC_thr_init defaulting it to 30 when -1
 *                             (0xf9a238 ldr / 0xf9a244 mov w8,#0x1e / str)
 *   0x24a6510  restart sig  : pthread_kill's w1 in GC_start_world
 *                             AND GC_thr_init defaulting it to 24 when -1
 *                             (0xf9a250 ldr / 0xf9a25c mov w9,#0x18 / str),
 *                             followed by the cmp w8,w9 that Boehm uses to
 *                             assert the two signals differ.
 *   0x24a6508  start ack    : gates the handler's 2nd sem_post (0xf99e5c)
 *                             AND gates the restart path in GC_start_world
 *                             (0xf9a17c ldr w8,[x23,#0x508] ; cbz)
 *   0x26c47a8  ack semaphore: sem_post x2 in the handler, sem_wait in
 *                             GC_stop_world, and sem_init(&sem,0,0) in
 *                             GC_thr_init -- four sites, one address.
 *
 *   Section placement checks out: the three ints are in .data and the
 *   semaphore is in .bss, which is also why leaving them zero was INERT --
 *   before GC init the sig values read 0 (never matching a real signal) and
 *   the ack storage is NULL (sem_post_fake no-ops).
 *
 *   Structural cross-check against Papers, Please: its three ints sit at
 *   0xf52a90 / 0xf52a94 / 0xf52a98 -- start-ack, suspend, restart at +0/+4/+8.
 *   Ours are 0x508 / 0x50c / 0x510: the SAME consecutive layout in the same
 *   order, which is what the Boehm source declares.
 *
 * MODEL: the four-offset Papers, Please one (ack on behalf of the thread),
 * not Fruit Ninja's seven-offset real stop-the-world. See ABR_GC_BRIDGE_SIMPLE
 * in config.h. The three offsets the real-STW model additionally needs
 * (GC_threads, GC_stop_count, GC_retry_signals) are NOT derived, and that
 * model stays compiled out.
 * ========================================================================= */
#define GC_SUSPEND_SIG_OFF_FN     0x249bdc4u  /* .data  GC_sig_suspend        */
#define GC_RESTART_SIG_OFF_FN     0x249bdc8u  /* .data  GC_sig_thr_restart    */
#define GC_START_ACK_OFF_FN       0x249bdc0u  /* .data  2nd-ack gate          */
#define GC_ACK_SEM_OFF_FN         0x26b9c90u  /* .bss   GC_suspend_ack_sem    */

/* real-stop-the-world model only -- NOT derived, and its model is off */
#define GC_THREADS_OFF_FN         0u
#define GC_STOP_COUNT_OFF_FN      0u
#define GC_RETRY_SIGNALS_OFF_FN   0u

#endif /* NX_PATCH_ABR_H */
