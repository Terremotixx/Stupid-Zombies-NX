# Angry Birds Reloaded — Nintendo Switch port (Unity 2022.3 / IL2CPP wrapper)

This is a native wrapper / loader that runs the original ARM64 Android build of
*Angry Birds Reloaded* on Switch homebrew. It contains **no game code and no
game assets** — it loads the game's own libraries and recreates, natively, the
thin Android/JNI layer the Unity engine expects.

> **Status: boots and gameplay is working on Switch hardware.** Audio, assets,
> cutscenes and offline post-level results are functional; the remaining work
> is performance/polish and the separately stubbed tutorial-MP4 backend.

## Install & run

You need files from your own copy of Angry Birds Reloaded (`com.rovio.reloadedport`,
2.2.16218 — unzip the APK).

Put the `.nro` in **any folder** under `sdmc:/switch/` and place your game files
next to it — the loader finds its folder at runtime, so the name is up to you:

```
sdmc:/switch/abreloaded
├── abreloaded_nx.nro
├── libmain.so  libunity.so  libil2cpp.so   <- from your APK: lib/arm64-v8a/
├── cursor_pointer.png                      <- pointing hand (included)
├── cursor_grab.png                         <- held/grabbing hand (included)
└── assets/                                 <- from your APK: the whole assets/ folder
```

Launch via **title override** (hold **R** while starting an installed game) or a
forwarder.

The included **`cursor_pointer.png`** and **`cursor_grab.png`** are separate,
lossless crops of the supplied hand image. They appear only after 120 rendered
frames, keeping the cursor off Unity's splash/loading screens. The left edge is
inset for reliable slingshot dragging, with smaller bounds on the other edges.
The hand is scaled to roughly 38 pixels tall at 720p (57 pixels at 1080p),
matching the relative size of the PC cursor. Both states share a vertical
canvas anchor so pressing a click button does not make the hand jump upward.

## Controls

Angry Birds is a one-finger game — aiming the slingshot is a single
press-drag-release — so this port uses a **single** cursor, unlike the Fruit
Ninja port it derives from, whose two cursors exist because that game wants two
fingers. Handheld uses the touchscreen directly, exactly like Android.

| Input | Action |
| --- | --- |
| **Touchscreen** | Direct touch — the native fit for this game |
| **Left stick** | Move the cursor |
| **A** / **ZR** / **ZL** | Tap / drag (ZL and ZR let you play one-handed) |
| **+** | Toggle the on-screen cursor |
| **–** | Toggle gyro pointing (tilt/turn the controller to aim) |
| **L** / **R** | Recenter the cursor (helps gyro aiming) |
| **B** | Android Back |
| **D-pad up / down** | Adjust sensitivity of whatever is driving the cursor |

A USB mouse works in both handheld and docked: move to control the cursor,
left-click to tap, scroll wheel to change sensitivity. Stick, mouse and gyro
sensitivities are remembered in `pointer.cfg` after in-game adjustment.

## Settings

`config.txt` is written next to the `.nro` on first launch:

```
handheld_res 720     # 720 or 1080
docked_res   1080    # 720 or 1080
```

## Porting status

This tree is a fork of the **Fruit Ninja Classic+** port, chosen because that
game is the same engine generation (Unity 2022.3 / IL2CPP / arm64) and, as it
turns out, an almost exact superset of this one's native surface:

> **498** distinct undefined symbols across `libmain.so` + `libunity.so` +
> `libil2cpp.so` → **493** already covered by that loader core → **5** remain,
> all in `source/imports_abr_extra.c`.

What that fork does *not* transfer is anything expressed as an offset into
Fruit Ninja's binaries. Those are re-derived per game, and here is where each
stands.

| Piece | State | Notes |
| --- | --- | --- |
| Import surface (5 symbols) | **Done** | `nearbyintf`, `writev`, `perror`, `__android_log_buf_write` real; `execl` → `ENOSYS` |
| libunity allocator table (256MB→64MB) | **Done, 21 sites** | Derived + verified for this binary — `source/nx_patch_abr.h` |
| Single-cursor input | **Done** | `nx_pointer.c` from the Papers, Please port |
| Boehm GC stop-the-world bridge | **Done, 4 offsets** | Derived + cross-confirmed for this binary; four-offset Papers, Please model |
| Time.get_* hooks, splash bypass, il2cpp guards | **NOT DERIVED, gated off** | Fruit-Ninja-specific; each is a separate task |
| FMOD→OpenSL audio | **Done** | Native OpenSL output forced and mixed through SDL |
| Cutscene/scene-load A/V sync | **Done** | OpenSL queue position freezes only while a Unity visual frame is blocked |
| Video playback (26 tutorial MP4s) | **Stubbed** | `AMediaCodec` fails fast, as in both reference ports |
| Adjust / GameCenter / Purchasing / Analytics | **Not stubbed yet** | Managed-side; not reached through `dlsym` |

Every disabled item is gated by a flag in `source/config.h` or
`source/nx_patch_abr.h`, and each carries, inline, the reasoning and the route
to deriving it. **Do not flip a gate without re-deriving its offsets** — a zero
offset that goes live patches the module base.

### The GC bridge

POSIX signals are never delivered on Switch, so Boehm's stop-the-world — which
`pthread_kill`s every thread and waits on the acks its signal handlers would
post — hangs forever on the first collection. The fix is to post those acks
from `pthread_kill` itself, which needs four `libil2cpp` globals.

`libil2cpp.so` keeps a 2.3MB `.text` (the runtime, including Boehm) separate
from its 18MB `il2cpp` codegen section. Scanning `.text` for calls into the PLT
gives the Boehm fingerprint in one shot — 2 `pthread_kill`, 2 `sem_post`,
1 `sem_wait`, 1 `sem_init`, 1 `sigsuspend`, all inside a 1.7KB window that is
`pthread_stop_world.c`. Every value is confirmed at least twice:

| Global | Offset | Confirmed by |
| --- | --- | --- |
| `GC_sig_suspend` | `0x24a650c` | `pthread_kill`'s `w1` in `GC_suspend_all`, **and** `GC_thr_init` defaulting it to 30 |
| `GC_sig_thr_restart` | `0x24a6510` | `pthread_kill`'s `w1` in `GC_start_world`, **and** `GC_thr_init` defaulting it to 24 |
| 2nd-ack gate | `0x24a6508` | Gates the handler's second `sem_post`, **and** the restart path in `GC_start_world` |
| `GC_suspend_ack_sem` | `0x26c47a8` | `sem_post` ×2, `sem_wait` in `GC_stop_world`, `sem_init(&s,0,0)` in `GC_thr_init` |

The three ints land in `.data` and the semaphore in `.bss` — which is also why
leaving them zero was *inert* rather than dangerous: before GC init the signal
globals read 0 and never match a real signal. As a structural cross-check,
Papers, Please's three ints sit at `+0/+4/+8` of each other in the same order
(start-ack, suspend, restart); ours are `0x508/0x50c/0x510`.

### Why the allocator table could not just be copied

Twelve of its 21 `from` words *are* byte-identical to Fruit Ninja's, which is
what makes the cluster identifiable at all. But 2022.3.7f1 differs from
2022.3.0f1 in three ways, two of which are traps:

1. It emits **56-bit-aware masks** (`and x8,x1,#0x00FFFFFFF0000000`), so those
   `from` words differ.
2. It indexes the level-1 page directory with **`lsr #0x28`** at `0x71f0d0` and
   `0x720e0c`, where Fruit Ninja patches a `lsr #28`. Patching this game's two
   would corrupt the directory index. They are deliberately excluded.
3. It has a round-up idiom Fruit Ninja lacks, where **both halves of each pair
   must move together**.

The loader is verify-first: every `{from}` word must match before any write, so
a wrong *offset* is caught. A wrong *transform* at a right offset is **not**,
which is why each entry documents its own reasoning.

## Building

Requires devkitPro with the `switch-dev` group plus these portlibs:

```sh
pacman -S switch-dev
pacman -S switch-mesa switch-libdrm_nouveau switch-sdl2 switch-libpng switch-zlib
```

```sh
export DEVKITPRO=/opt/devkitpro
make                        # -> abreloaded_nx.nro
```

To regenerate the allocator table against your own copy of the game — do this
if the game ever updates, because a stale offset patches the wrong instruction
and fails somewhere unrelated:

```sh
python3 tools/derive_alloc_patches.py       # needs `pip install capstone`
```

Set `DEBUG_LOG` to `1` in `source/config.h` to get a `debug.log` next to the
`.nro`; it is the first place to look if something fails.

## Credits

The loader/shim infrastructure (`so_util`, `libc_shim`, `jni_fake`, `unity_jni`,
`opensles`, `nx_pointer`, diagnostics) derives from the open-source Switch
`.so`-loader lineage — Andy Nguyen, fgsfds and ChanseyIsTheBest, building on
TheOfficialFloW's Vita/Switch loader tradition — reaching this project through
the **Fruit Ninja Classic+** port (engine generation, loader core, NDK stubs)
and the **Papers, Please** port (`nx_pointer`, the four-offset GC bridge model).
All MIT-licensed. Thanks to everyone in that lineage for making this approach
possible.

## Legal

No affiliation with Rovio Entertainment or SEGA. "Angry Birds" is the property
of its owner. **This repository contains no assets or program code from the
game, and none may be distributed with builds.** You must supply your own game
files from a copy you own. Wrapper source is MIT (see `LICENSE`).
