# TODO — open work in this fork

Priority list of known open items. **Nothing here blocks normal use**: the app
and firmware shipped in the release are the verified, working state. Each entry
links to the document with the diagnosis, evidence and (where it exists) the
proposed fix.

---

## 1. Controller hot-plug can hang the board — *medium*

- **Trigger (refined):** reproducible **with a USB-C (male) → USB-A (female)
  adapter attached to the board's host port**. Without that adapter, hot-plug
  could not be reproduced.
- **Symptom:** LEDs go dark, the kernel stops scheduling (`USBH_Process` never
  runs again), the CPU stays alive and the host ISR exit is the last step
  reached. No self-recovery.
- **Workaround:** assemble controller + adapter *before* connecting it to the
  board, and connect before powering the board on.
- **Already applied:** ISR-safe `osMessagePutI` in the HCD callbacks (correct,
  but not sufficient).
- **Leads:** other non-ISR-safe calls from the HCD ISR, fast connect/disconnect
  (bounce) handling, separate marker slots for callbacks vs state machine.
- **Details:** [`docs/MIDI-USB-HOST-FINDINGS.md`](docs/MIDI-USB-HOST-FINDINGS.md) §9

## 2. Patcher live lock — deletions are not blocked — *low/medium*

- While a patch is live, objects/nets can still be deleted (keyboard, Edit menu,
  cut, iolet context menu, cable drag) because `PatchController.delete()` /
  `disconnect()` have no `isLocked()` guard.
- Diagnosis and a surgical one-file fix are ready but **not implemented**.
- **Details:** [`docs/PATCHER-LIVE-LOCK.md`](docs/PATCHER-LIVE-LOCK.md)

## 3. arm64 migration of the rescue flasher (STM32CubeProgrammer) — *medium, time-boxed*

- The bundle ships **STM32CubeProgrammer 2.4.0/2.5.0 x86_64**, which runs under
  Rosetta. Rosetta 2 ends with macOS 27, so "Board → Flash (rescue)" — the only
  recovery path for a bricked board — stops working on macOS 28.
- `dfu-util` was evaluated as an arm64 replacement and is **disqualified for
  writing** (it erases and then fails before the first data byte).
- **Plan:** embed ST's **2.23 ARM-aarch64** build (same relative path, same CLI
  invocation), then update `platform_osx/upload_fw_dfu.sh`, `Makefile` and both
  bundles, keeping the x86 CLI as a fallback.
- **Details:** [`docs/DFU-WRITE-INCIDENT.md`](docs/DFU-WRITE-INCIDENT.md) §5-6

## 4. arm64 migration of the firmware toolchain — *low (port, not drop-in)*

- The **patch** compiler is already native arm64 (GCC 15.3,
  [`docs/PATCH-COMPILER-ARM64.md`](docs/PATCH-COMPILER-ARM64.md)); the **firmware**
  still builds with the x86 GCC 8-2019 toolchain under Rosetta.
- Options: ARM GNU Toolchain `darwin-arm64` (GCC 12.3+/13+) or build the
  firmware on Linux/CI. Requires validating the produced `axoloti.bin`/`.elf`
  pair (a mismatch breaks patch linking, see
  [`docs/MIDI-USB-HOST-FINDINGS.md`](docs/MIDI-USB-HOST-FINDINGS.md) §1).

## 5. Card reader: no way back to the editor after ejecting — *low (inherited, deferred)*

- Ejecting the SD from the host leaves the mounter running; the patcher cannot
  reconnect (it looks for `0x16C0/0x0442` while the board stays as ST mass
  storage `0x0483/0x5740`). A power cycle is needed.
- **This behaves the same in the original Axoloti/Akso**, so it is not a
  regression of this fork and is **not a priority**.
- Two implementation options are written down (SCSI `START_STOP_UNIT` →
  `NVIC_SystemReset()`, or a `libusb_reset_device()` fallback in the patcher):
  [`docs/CARD-READER-MASS-STORAGE.md`](docs/CARD-READER-MASS-STORAGE.md).

## 6. Code signing / notarization — *out of scope for now*

- **Not planned**: requires a paid Apple Developer account (USD 99/year).
- Users must run `xattr -dr com.apple.quarantine /Applications/Akso.app` (or
  right-click → Open) the first time.
- If it ever matters, ad-hoc signing (`codesign --force --deep -s - Akso.app`)
  is free and removes part of the friction.

## 7. Validate the cross-platform build guide — *open to contributors*

- [`docs/BUILD-OTHER-PLATFORMS.md`](docs/BUILD-OTHER-PLATFORMS.md) documents how to
  build this fork on **macOS Intel, Windows and Linux**, but it is explicitly
  **UNVALIDATED**: only macOS Apple Silicon was verified end to end (there is no
  Mac Intel, Windows or Linux machine behind this fork, and no one has run the
  recipes yet).
- The fixes themselves are platform independent (C firmware + Java), so this is
  about *build/package* instructions, not about the fixes working.
- **What is needed:** someone on each platform to follow the guide and report
  back. **Definition of done** per platform: `make package-<platform>` produces a
  bundle that launches, connects to the board, compiles a patch and flashes a
  firmware update.
- **How to contribute:** open an issue at
  <https://github.com/gus74vv/akso/issues> with OS + version, architecture, the
  exact commands, the full output of any failing step and the commit built.
  Instructions that turn out to be wrong will be fixed, and the platform will be
  marked as validated (or dropped from the document).
- Nice-to-have while doing it: record the produced `axoloti.bin` CRC32 and note
  whether it matches the macOS build (`05394BAC`) — a different CRC is fine, but
  the `.bin`/`.elf` pair must always match within your own build.

---

## Closed / not reproducing

- **"SD card mounts intermittently at boot"** (recorded 2026-09-28): as of
  2026-10-03 the card **mounts normally**; the issue is not reproducing. Kept as
  history in [`docs/MICROSD-DIAGNOSTICS.md`](docs/MICROSD-DIAGNOSTICS.md).
