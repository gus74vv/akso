# Akso — native Apple Silicon build + firmware fixes

An **unofficial fork** of [Akso](https://zrna.org/akso) — a modular audio DSP
platform with a visual patcher that targets standalone embedded hardware (the
AKSO board, STM32H753), derived from [Axoloti](https://github.com/axoloti/axoloti).

Akso's upstream repository (`zrna-research/akso`) has been inactive while the
hardware still has open firmware bugs. This fork carries the fixes needed to
actually use the board, plus a **native arm64 build for macOS on Apple Silicon**
(no Rosetta anywhere in the music-making path).

Everything here was verified on real hardware.

---

## Status

| | |
|---|---|
| Board firmware | **CRC32 `05394BAC`** (chip flashed and validated) |
| macOS app | **Native arm64**: launcher, bundled JRE, usb4java, patch compiler |
| MIDI | USB host in **and out** working (tested with a KORG nanoKONTROL2) |
| Card reader | USB mass storage (SD) readable and writable, `fsck`-clean |
| Patch compile | ~2.7 s for a full "Live" cycle (was ~42 s under Rosetta) |

Target hardware: AKSO (STM32H753, Cortex-M7). The patcher is Java/Swing; the
firmware is ChibiOS/RT + ST's USB host library.

---

## Fixes in this fork

| # | Area | Problem | Fix | Details |
|---|---|---|---|---|
| 1 | macOS Apple Silicon | The app was x86_64-only and ran under Rosetta; child processes crashed with `xcrun: unable to load libxcrun … need 'x86_64'`, breaking patch compilation | Native arm64 launcher (custom-built PackrLauncher), arm64 JRE, arm64 `libusb4java.dylib` | [`docs/ARM64-BUILD.md`](docs/ARM64-BUILD.md) |
| 2 | Patch compiler | ~40 s per compile (x86 GCC 8 under Rosetta) | Native arm64 GCC 15.3, pruned to 280 MB; selected automatically by `Makefile.patch` (falls back to the old toolchain) | [`docs/PATCH-COMPILER-ARM64.md`](docs/PATCH-COMPILER-ARM64.md) |
| 3 | Card reader (SD mass storage) | Every LBA returned block 0 / all writes went to the wrong sector → corrupt FAT, "unformatted" card | D-cache maintenance around the SDMMC IDMA transfers (`cacheBufferInvalidate` / `cacheBufferFlush`) | [`docs/CARD-READER-MASS-STORAGE.md`](docs/CARD-READER-MASS-STORAGE.md) |
| 4 | Card reader + MIDI controller | Entering card reader mode with a controller on the USB host port **hung the board** (no handler for the host IRQ left enabled in the NVIC by the main firmware) | Disable/clear IRQ 77 and hold the OTG-HS peripheral in reset in `patch_init()` of the mounter **and** the flasher | same doc |
| 5 | USB host MIDI out | Sending the first CC **crashed the board** (hard fault → USB disconnect): the host MIDI ring-buffer `notify` callback was never initialized | Initialize both host output ring buffers; NULL-guard in `midi_output_buffer_put()` | [`docs/MIDI-USB-HOST-FINDINGS.md`](docs/MIDI-USB-HOST-FINDINGS.md) §0-3 |
| 6 | USB host MIDI out | Messages were transmitted but the device received garbage: the DMA read stale RAM (cacheable `.bss` buffer) | Stage outgoing messages in non-cacheable `.ram3` | same doc §5 |
| 7 | USB host (kernel) | HCD ISR callbacks used `osMessagePut` (mapped to non-ISR-safe `chEvtSignal`), corrupting the ChibiOS kernel lock | Use the ISR-safe `osMessagePutI` | same doc §9 |
| 8 | Patcher | Compiling a patch with an unassigned `objref` (e.g. `table/load` without a target) produced 7 cryptic `SEVERAL incomplete object reference attribute` lines plus a C++ error | The log now names the object, its type and the attribute | commit `Improve empty object-reference attribute error` |
| 9 | UI | Knobs/sliders were hyper-sensitive to click+drag | Configurable **File → Preferences → Drag sensitivity** (0.05–1.0) | — |

### Known open issues

A full, prioritised list with links and evidence lives in [`TODO.md`](TODO.md).
Summary:

- **Controller hot-plug can hang the board — in one specific configuration.**
  It hangs if a USB-C (male) → USB-A (female) adapter is left plugged into the
  **board's** host port (a dangling female socket) and the controller is then
  plugged into that adapter. If the adapter travels with the **controller's**
  own cable and the whole assembly is plugged in, hot-plugging works fine.
  Workaround: keep the adapter on the controller side (or connect everything
  before powering the board on).
  [`docs/MIDI-USB-HOST-FINDINGS.md`](docs/MIDI-USB-HOST-FINDINGS.md) §9
- **Deleting objects/nets while a patch is live is not blocked** — diagnosis and
  a ready (unimplemented) one-file fix:
  [`docs/PATCHER-LIVE-LOCK.md`](docs/PATCHER-LIVE-LOCK.md)
- **Rescue flashing still needs Rosetta** — the bundled STM32CubeProgrammer CLI
  is 2.4.0/2.5.0 x86_64, and `dfu-util` was evaluated and **disqualified for
  writing**: [`docs/DFU-WRITE-INCIDENT.md`](docs/DFU-WRITE-INCIDENT.md)
- **The firmware toolchain is still x86** (Rosetta); the patch compiler is
  already native arm64
- **No way back from card reader mode after ejecting** — same behaviour as the
  original Axoloti, inherited, low priority

**Not reproducing:** the "SD card mounts intermittently at boot" issue — the
card now mounts normally (kept as history in
[`docs/MICROSD-DIAGNOSTICS.md`](docs/MICROSD-DIAGNOSTICS.md)).

**Out of scope:** code signing / notarization (needs a paid Apple Developer
account). Use the `xattr` command below.

---

## Install on macOS (Apple Silicon)

1. Download the release archive and move `Akso.app` to `/Applications`.
2. The app is **not notarized** (the release is free and ad-free by design —
   notarization would need a paid Apple Developer account). Remove the quarantine
   flag once, or right-click → *Open*:

   ```sh
   xattr -dr com.apple.quarantine /Applications/Akso.app
   ```

3. Connect the board over USB. If the app asks to update the firmware, accept —
   it uses the DFU bootloader through the bundled STM32CubeProgrammer CLI.
4. If the board ever looks dead, it can always be recovered: hold **switch 1**
   while plugging it in (DFU mode) and use **Board → Flash (rescue)**.

You need an AKSO-compatible board. Patches and objects (`axoloti/`, `~/Library/Akso/akso-factory`)
are compatible with the original Axoloti ecosystem.

---

## Building from source

### 0. Restore the cross-platform toolchains (once)

The ARM toolchains and the per-platform JDKs are part of this repository's
history, but they were **pruned from the working tree** to keep it small
(`external/gcc-arm/*`, `external/jdks/*`, ~1.5 GB). They can be restored from
the last upstream commit, which is an ancestor of this branch:

```sh
# ARM toolchains: pick the ones you need (mac = x86, for the firmware)
git checkout 2937859 -- external/gcc-arm/mac external/gcc-arm/linux external/gcc-arm/win
# Per-platform JDK 11 tarballs used by packr (stored via Git LFS)
git checkout 2937859 -- external/jdks/mac_x64 external/jdks/linux_x64 external/jdks/win_x64
```

Requires `git lfs` installed (`brew install git-lfs && git lfs install`) for the
`.tar.gz` JDKs. On Apple Silicon the **arm64** patch compiler is used instead of
`external/gcc-arm/mac`; it is not tracked (280 MB) — see
[`docs/PATCH-COMPILER-ARM64.md`](docs/PATCH-COMPILER-ARM64.md) for how to obtain/prune it.

### 1. Build the Java GUI (`Axoloti.jar`)

```sh
# JDK 11 is required (the 2020 code base does not build with newer JDKs)
export JAVA_HOME=/path/to/jdk-11            # e.g. the bundled Zulu 11 aarch64
cd axoloti && ant                           # → axoloti/dist/Axoloti.jar
```

> **Apple Silicon:** `ant` regenerates `Axoloti.jar` **without** the arm64
> `libusb4java.dylib`, so USB will not work until you re-inject it:
> `zip Axoloti.jar org/usb4java/osx-aarch64/libusb4java.dylib`
> (a copy is inside the released app's jar).

### 2. Build the firmware (the board image)

```sh
export PATH="$PWD/external/gcc-arm/mac/gcc-arm-none-eabi-8-2019-q3-update/bin:$PATH"   # (or linux/win)
cd axoloti/firmware && make
```

Produces `axoloti.bin` + `axoloti.elf` + `.hex/.map/.dmp/.list`, and the
`flasher` and `mounter` images (`axoloti/firmware/{flasher,mounter} && make`).

> **Deploying firmware to the app:** copy the **full set**
> (`axoloti.bin axoloti.elf axoloti.hex axoloti.map axoloti.dmp axoloti.list`)
> into `<Akso.app>/Contents/Resources/firmware/build/`, then restart the app.
> Copying only the `.bin` breaks patch compilation: patches are linked with
> `--just-symbols` against that `.elf`, so a mismatched pair makes patches call
> the wrong addresses (go-live crashes with `LIBUSB_ERROR_PIPE`). See
> [`docs/MIDI-USB-HOST-FINDINGS.md`](docs/MIDI-USB-HOST-FINDINGS.md) §1.

### 3. Package the app (packr)

```sh
make package-mac      # macOS x86_64 bundle (packr-mac-x64.json)
make package-linux    # Linux x64 bundle     (packr-linux-x64.json)
make package-win      # Windows x64 bundle   (packr-win-x64.json)
# Apple Silicon: packr-mac-arm64.json + the custom arm64 launcher:
#   see docs/ARM64-BUILD.md §2.5-2.6
```

Notes:

- `external/packr-all-2.7.0.jar` embeds **x86_64 launchers only**. On Apple
  Silicon the launcher must be replaced by an arm64 build of PackrLauncher
  2.7.0 (`docs/ARM64-BUILD.md` §2.5), and JRE minimization runs through `jlink`
  with the arm64 JDK.
- Output goes to `package/` (git-ignored) and to
  `package/akso-{mac,linux,win}-3.0.1.{zip,tar.gz}`.

### Porting the fixes to other platforms

- Fixes **1–2** (arm64 launcher/JRE/dylib, arm64 patch compiler) are
  macOS-arm64 specific; other platforms keep their existing x86 tools.
- Fixes **3–7** (card reader D-cache, mounter IRQ, USB host MIDI, ISR-safe
  message posting) are in **C firmware** and apply to every platform that runs
  this firmware.
- Fix **8** (patcher objref error) and **9** (drag sensitivity) are **Java** and
  are platform independent.

Step-by-step recipes for **macOS Intel, Windows and Linux** are in
[`docs/BUILD-OTHER-PLATFORMS.md`](docs/BUILD-OTHER-PLATFORMS.md) — including a
cherry-pick list if you only want the fixes in an existing tree. That document
is explicitly **marked as unvalidated**: only macOS Apple Silicon was verified
end to end here. Validating it is tracked in [`TODO.md`](TODO.md) as a task open
to contributors — reports are welcome.

---

## Repository layout

```
axoloti/          Java patcher (src/, lib/, doc/) + STM32 firmware + mounter/flasher
external/         vendored ChibiOS, ChibiOS-Contrib, ST USB host library, FatFs,
                  packr jar, per-platform JDKs and ARM toolchains (partially pruned)
package/          build output (git-ignored; released separately)
docs/             technical notes for every fix (see the table above)
NOTA.txt          internal development notes (Spanish)
```

`docs/` is the most useful part of this fork: each document separates what was
**verified** from what was **ruled out**, with root causes, measurements and
reproduction steps.

---

## License and credits

Licensed under the **GNU GPL v3** (see `COPYING`) — the same license as
Axoloti/Akso. Redistributing the built app or firmware is allowed as long as the
corresponding source is made available, which is what this repository does
(keep the license and notices intact).

- [Akso](https://zrna.org/akso) by zrna.org (hardware and adapted firmware)
- [Axoloti](https://github.com/axoloti/axoloti) — the original platform this
  project derives from
- [ChibiOS](https://www.chibios.org/), STM32 HAL, FatFs, usb4java, Packr (NimblyGames)
- The fixes and the Apple Silicon port in this fork: Gustavo ([@gus74vv](https://github.com/gus74vv))
