# Building for macOS (Intel), Windows and Linux — UNVALIDATED guide

> ## ⚠️ Status: NOT VALIDATED
> Everything in this fork was **verified on macOS Apple Silicon (arm64) only**.
> The recipes below are assembled from the repository's own build system
> (`Makefile`, the `packr-*.json` configs, the in-tree toolchains) and from the
> fact that the firmware fixes are plain C. **Nobody has run them end to end on
> macOS Intel, Windows or Linux.**
>
> Treat this as a starting point and expect to fix small things (paths, shell
> scripts, missing packages). If you try it, please open an issue with what
> worked and what broke — that is how this document becomes trustworthy.

Nothing here is needed to *use* the app on Apple Silicon: that is the release
bundle. This is for people who want to build the fork themselves or port the
fixes to another platform.

---

## 0. What you need — and what you do not

| Thing | Do you need to build it? |
|---|---|
| **Mounter** binaries (card reader) | **No.** They are tracked in the repo *and already contain our fix* (`mounter.sram1.bin`, sha256 `8e60ad24…`). ARM machine code is host-independent, so they work as-is on any platform. |
| **Main firmware** (`axoloti.bin`/`.elf`) | **Yes** — it is not tracked (git-ignored). Needs the ARM toolchain for your platform. |
| **Flasher** (`flasher.sram1.bin`) | **Yes** — not tracked either (it has the same host-IRQ fix as the mounter, so rebuild it from source). |
| Java patcher (`Axoloti.jar`) | Yes — `ant` with a JDK 11. The Java fixes work on every platform. |
| The arm64 work (custom launcher, arm64 JRE, `osx-aarch64` dylib, native arm64 patch compiler) | **No** — it is macOS-on-Apple-Silicon specific. On other platforms keep the existing x86 tools; nothing to do. |

So: on Intel/Windows/Linux you keep the original packaging path and only pick up
the **source fixes**.

---

## 1. Route A — take only the fixes into an existing upstream-based tree

This is the least invasive option if you already build Akso/Axoloti for your
platform (e.g. an old checkout that works except for the bugs we fixed).

```sh
git remote add gus74vv https://github.com/gus74vv/akso
git fetch gus74vv

# our branch is a direct descendant of upstream's 2937859, so you can see exactly
# what changed:  git log --oneline 2937859..gus74vv/master
```

Net-effect commits worth cherry-picking (current hashes):

| Fix | Commit | Files touched |
|---|---|---|
| USB host MIDI out crashed the board on the first CC (uninitialized ring-buffer `notify`) | `af86cbe` (revert/reapply pair `22573f5`/`1ac212d` — pick `af86cbe` and skip the reverts) | `axoloti/firmware/midi_usbh.c`, `usbh.h`, `usbh_conf.c`, `midi_buffer.c` |
| USB host MIDI out sent garbage (DMA read stale cacheable RAM) | `75e42b8` | `axoloti/firmware/usbh_midi_core.c` |
| HCD ISR used the non-ISR-safe `osMessagePut` (kernel lock corruption) | `3b17fbe` | `external/STM32_USB_Host_Library/Core/Src/usbh_core.c` |
| Card reader hung when a MIDI controller was attached | `b146df6` | `axoloti/firmware/mounter/main.c`, `axoloti/firmware/flasher/main.c` |
| Card reader returned block 0 / corrupted the FAT (D-cache vs SDMMC IDMA) | `ea42606` | `external/ChibiOS-Contrib/os/various/lib_scsi.c`, `external/ChibiOS/os/hal/ports/STM32/LLD/SDMMCv2/hal_sdc_lld.c` |
| Mounter would not build (missing ADC3 config) | `58f1abd` | `axoloti/firmware/mounter/mcuconf.h` |
| Case-colliding duplicate source file (breaks case-insensitive filesystems) | `485f51d` | deletes `external/CMSIS/DSP_Lib/Source/TransformFunctions/arm_bitreversal2.S` (the Makefile uses the lowercase `.s`, so case-sensitive systems are unaffected) |
| Patcher: clear error for an unassigned `objref` | `a453508` | `axoloti/src/main/java/axoloti/patch/object/attribute/AttributeInstanceObjRef.java` |
| Patcher UI: configurable drag sensitivity | `cb13592` | 5 files under `axoloti/src/main/java/axoloti/swingui/` + `preferences/` |
| macOS arm64 only (launcher/JRE/dylib) | `b534e61`, `367005d` | `Makefile.patch`, `packr-mac-arm64.json`, `external/jdks/mac_arm64/` |

```sh
# example: only the firmware-side fixes
git cherry-pick af86cbe 75e42b8 3b17fbe ea42606 b146df6 58f1abd 485f51d
# and for the patcher
git cherry-pick a453508 cb13592
```

> **Critical pairing rule:** patches are compiled and linked against
> `axoloti.elf` (`--just-symbols`). If you flash a firmware built from a
> different tree, the `.bin` and the `.elf` the app uses must match, or patches
> call the wrong addresses and go-live crashes with `LIBUSB_ERROR_PIPE`
> ([`MIDI-USB-HOST-FINDINGS.md`](MIDI-USB-HOST-FINDINGS.md) §1). Always deploy
> the **full set**: `axoloti.bin axoloti.elf axoloti.hex axoloti.map axoloti.dmp
> axoloti.list`.

Route B — build the whole fork — is below.

---

## 2. Common prerequisites (all platforms)

```sh
# 1) git + Git LFS: the per-platform JDK tarballs are stored via LFS
git lfs install

# 2) the ARM toolchain for YOUR platform and the per-platform JDK 11
#    (both were pruned from the working tree; they live in history)
git checkout 2937859 -- external/gcc-arm/linux        # or: win | mac (mac = x86)
git checkout 2937859 -- external/jdks/linux_x64       # or: win_x64 | mac_x64
git lfs pull                                          # fetch the big tarballs

# 3) JDK 11 and ant (the 2020 code base does not build with newer JDKs)
#    Debian/Ubuntu:  sudo apt install ant openjdk-11-jdk
#    macOS:          brew install ant openjdk@11
#    Windows:        MSYS2 + JDK 11 + ant, or build inside WSL2
export JAVA_HOME=/path/to/jdk-11
```

`external/packr-all-2.7.0.jar` (the bundler) already ships in the repo, and the
three packr configs are in the repo root:

| Platform | Config | Output | Bundled JDK |
|---|---|---|---|
| macOS Intel | `packr-mac-x64.json` | `package/mac/Akso.app` | `external/jdks/mac_x64` |
| Windows x64 | `packr-win-x64.json` | `package/win/akso-3.0.1` | `external/jdks/win_x64` |
| Linux x64 | `packr-linux-x64.json` | `package/linux/akso-3.0.1` | `external/jdks/linux_x64` |

---

## 3. Build steps (all platforms, with the platform-specific notes inline)

```sh
# --- firmware (main + flasher), with the ARM toolchain on PATH -------------
export PATH="$PWD/external/gcc-arm/linux/gcc-arm-none-eabi-8-2019-q3-update/bin:$PATH"
cd axoloti/firmware && make                       # main firmware
cd flasher && make                                # flasher image
cd ../mounter && make                             # mounter (optional: tracked bins exist)
# on macOS: use external/gcc-arm/mac/... ; on Windows: external/gcc-arm/win/...
# (the tracked mounter bins already contain our fix, so building it is optional)

# --- Java patcher ----------------------------------------------------------
cd ../../axoloti && ant                          # → axoloti/dist/Axoloti.jar

# --- bundle -----------------------------------------------------------------
cd .. && make package-linux                      # or: package-mac / package-win
# (make package-* also builds the firmware + the jar first)
```

Notes per platform:

- **macOS Intel** — no extra steps: the `osx-x86_64` `libusb4java.dylib` (2014)
  is already inside `Axoloti.jar`, and the x86 PackrLauncher embedded in
  `packr-all-2.7.0.jar` is the right architecture. Gatekeeper will quarantine
  the unsigned app (`xattr -dr com.apple.quarantine …`). Flashing uses
  `external/STM32CubeProgrammer/mac/`.
- **Windows x64** — the in-tree build system assumes a POSIX shell (`make`,
  `sh`, `rm`), so build from **MSYS2/MinGW-w64 or WSL2** (the repo ships
  `external/make/win` and `external/gcc-arm/win`). Ant + JDK 11 for
  `Axoloti.jar`. Flashing uses `external/STM32CubeProgrammer/win/`. The patcher
  spawns `make.exe`/`arm-none-eabi-g++.exe`, so the paths inside the bundle must
  resolve as they do upstream — expect to debug this; it is the least tested
  platform.
- **Linux x64** — install `ant`, JDK 11 and the usual build tools
  (`build-essential`, `unzip`, `zip`, `libusb-1.0-0-dev`); the `linux-x86_64`
  usb4java `.so` is already in the jar. Flashing uses
  `external/STM32CubeProgrammer/linux/`; add a udev rule for the STM32 ROM
  bootloader (`0483:df11`) so the CLI can open it without `sudo`.
- **Apple Silicon** — if you want the arm64 bundle, the extra work (custom arm64
  launcher, arm64 JRE, `osx-aarch64` dylib, arm64 patch compiler) is documented
  in [`ARM64-BUILD.md`](ARM64-BUILD.md) and
  [`PATCH-COMPILER-ARM64.md`](PATCH-COMPILER-ARM64.md).

---

## 4. What to watch out for

1. **Patch compiler selection.** `Makefile.patch` prefers
   `external/gcc-arm/<platform>-arm64/`. Only macOS has it in this repo; on
   Linux/Windows the `wildcard` fails and it falls back to the legacy x86 GCC 8
   toolchain — that is expected and fine (just slower).
2. **`external/jdks` restore requires Git LFS.** Without `git lfs pull` you get
   130-byte pointer files and packr will fail with a confusing unpack error.
3. **Deploy the firmware as a full set** (see the pairing rule in §1).
4. **Case-sensitive filesystems** are unaffected by `485f51d` (the Makefile
   references `arm_bitreversal2.s`, lowercase).
5. **Do not commit built bundles**: `package/` is git-ignored on purpose (the
   arm64 bundle is ~1.2 GB).
6. **Licensing**: GPLv3. If you redistribute your build, publish the
   corresponding source (this repository, or your own fork of it).

## 5. If you try it, report back

Open an issue on <https://github.com/gus74vv/akso/issues> with: OS + version,
architecture, the exact commands, the full output of the failing step, and the
commit you built. Instructions that turn out to be wrong will be corrected here
and the platform will be marked as validated (or dropped).
