# Stupid Zombies NX

**Stupid Zombies 3.4.5 port for Nintendo Switch**

Stupid Zombies NX is a native Nintendo Switch wrapper/port of the Android ARM64 version of **Stupid Zombies 3.4.5**.

It loads the original game's ARM64 Unity libraries and runs them on Nintendo Switch through a lightweight Android/JNI compatibility layer.

Current port version: **R3.70.23**

Target game:

- Stupid Zombies 3.4.5
- Package: `com.gameresort.stupidzombies`
- Android ARM64 (`arm64-v8a`)
- Unity 2022.3.19f1
- IL2CPP

## Installation

You need a **legally obtained copy of Stupid Zombies 3.4.5 for Android**.

The original game files are **not included** in this repository or in the releases.

### Automatic setup

Use the included preparation script:

```bash
python3 tools/prepare_sd.py \
  /path/to/stupid-zombies-3-4-5.apk \
  /path/to/SD \
  --nro /path/to/stupidzombies_nx.nro
```

The resulting installation should look like this:

```text
sd:/switch/stupidzombies/
├── stupidzombies_nx.nro
├── libmain.so
├── libunity.so
├── libil2cpp.so
├── cursor_pointer.png
├── cursor_grab.png
└── assets/
    └── bin/
        └── Data/
            └── ...
```

The following files come from your original Stupid Zombies APK:

```text
libmain.so
libunity.so
libil2cpp.so
assets/bin/Data/
```

They are not distributed by this project.

### Verify your APK

The included checker can verify that the APK matches the supported version:

```bash
python3 tools/check_apk.py /path/to/stupid-zombies-3-4-5.apk
```

## Launching

Stupid Zombies NX requires the full Nintendo Switch application memory pool.

Do **not** launch it in Homebrew Menu applet/album mode.

Use title override by holding **R** while launching a game, then start Stupid Zombies NX from the Homebrew Menu.

A forwarder with full application memory can also be used.

## Controls

| Control | Action |
|---|---|
| Left Stick | Move cursor / aim |
| R3 | Enable / disable gyroscope aiming |
| Gyroscope | Fine aiming while enabled |
| A | Touch / fire |
| ZR / ZL | Touch / fire |
| Minus (-) | Show / hide cursor |
| Plus (+) | Pause / resume |

The game remains based on its original touch interface, with Nintendo Switch controls and gyroscope input translated by the wrapper.

## Building

You need devkitPro/devkitA64 and the required Nintendo Switch homebrew libraries.

Required packages include:

- libnx
- switch-mesa
- switch-libdrm_nouveau
- switch-sdl2
- zlib
- libpng

Build with:

```bash
make clean
make -j
```

The resulting executable is:

```text
stupidzombies_nx.nro
```

## Debugging

A debug log may be generated at:

```text
sd:/switch/stupidzombies/debug.log
```

It can be inspected using:

```bash
python3 tools/analyze_debug_log.py debug.log
```

## Known issues

The port is considered playable and stable.

- Minor input/cursor edge cases may still occur with unusual combinations of inputs.
- In some situations, the background may move slightly due to the way the original touch movement is translated to controller input. This is only a minor visual issue and does not affect gameplay.

If you encounter a reproducible problem, please include the relevant `debug.log` when opening an issue.

## Credits

Stupid Zombies NX was developed from and heavily adapted from the open-source Nintendo Switch Android `.so` loader ecosystem.

Special thanks to:

- [aks796/abreloaded_nx](https://github.com/aks796/abreloaded_nx) for the wrapper base used during development
- Andy Nguyen
- fgsfds
- TheOfficialFloW
- Zookeeper DX port contributors
- Very Little Nightmares port contributors
- PvZ Fusion NX port contributors
- devkitPro and libnx contributors

## Legal

This project is not affiliated with or endorsed by GameResort.

**Stupid Zombies**, its program code, graphics, audio, assets and other original game content are property of their respective copyright holders.

No original Stupid Zombies game code or assets are distributed in this repository.

Users must provide the required files from their own legally obtained copy of Stupid Zombies 3.4.5.

The source code specific to this project and the inherited open-source wrapper code are provided under the terms of the accompanying MIT License.