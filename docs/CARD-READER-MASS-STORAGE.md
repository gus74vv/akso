# Card reader mode (USB mass storage) — broken SD I/O and its fix

> Investigation from 2026-09-27, root cause confirmed and fixed on 2026-09-28,
> plus a follow-up fixed on 2026-10-01.

## Symptom

Entering **card reader mode** (the `mounter` firmware running from SRAM and
exposing the SD card as USB mass storage) produced corrupt I/O on the Mac:

- **Every LBA returned the same sector** (block 0 / the MBR).
- **Writing to one "sector" changed every sector** to the same value.
- Disk Utility could not format the card; a format "completed" but left a 0 B
  volume that would not mount. Write throughput was ~120 KB/s instead of the
  expected 10-20 MB/s.
- `fsck_msdos -n` exited with 201 (invalid FAT), `diskutil verifyVolume`
  reported `-69845`.

A detail of the mass-storage path, evident from `diskutil info`: reads behaved
as if they returned the *last written block*, and small writes (the MBR, FAT
entries) never reached the card — i.e. **the requested LBA was effectively
ignored**.

## How card reader mode works (verified in code)

`TargetMenu.java` ("Enter card reader mode") does:

1. `conn.transmitStop()`
2. `TargetModel.uploadPatchToMemory(firmwareDirectory()/mounter/mounter_build/mounter.sram1.bin)`
3. `conn.transmitStart()`

So the **mounter is not an independent boot**: it is loaded as an Axoloti
"patch" over the AKSO protocol **while the main firmware keeps running**, and
the board is **not** reset. The main firmware already owns the SD card (its file
manager uses `SDCD1` + FatFs, see `axoloti/firmware/sdcard.c`), and the mounter
then does a fresh `sdcStart`/`sdcConnect` **on the same peripheral** — i.e. two
`SDCDriver` instances (one in flash, one in SRAM) driving the same SDMMC.

## What was verified and what was ruled out

Verified working or irrelevant:

| Piece | Verdict |
|---|---|
| `lib_scsi.c` | OK — the LBA is passed through correctly (`block * SDC_BLOCK_SIZE` to `sdcRead`), same as upstream |
| `hal_usb_msd.c` | OK — equal to or better than upstream; MSD read/write callbacks pass through |
| `hal_sdc.c` (ChibiOS) | OK — `sdcRead`/`sdcWrite` hand `block` to the LLD untouched |
| Physical card | Good — the main firmware's file manager reads it fine |
| `blkbuf` (mounter) | 512 B, 32-byte aligned, in `.ram0a` |
| Selected LLD | SDMMCv2 (`platform.mk` for the H7 includes `LLD/SDMMCv2/driver.mk` unconditionally; both firmware and mounter build the same `hal_sdc_lld.c`) |
| Local LLD vs upstream | The local file matches ChibiOS `ver20.3.1` (this fork is branch 3.0.1 / HAL 7.1.2) except whitespace and a debug `while(1);` trap — **not** an outdated file |
| ChibiOS-Contrib versions | Nothing to update: local `hal_usb_msd.c` is newer than upstream, `lib_scsi.c` differs only by a cast |

## Root cause (confirmed)

`external/ChibiOS/os/common/startup/ARMCMx/compilers/GCC/crt1.c:141`
(`__core_init`) calls **`SCB_EnableDCache()`** — the AKSO ChibiOS fork enables
the Cortex-M7 **D-cache at boot**. The SD driver (SDMMC + IDMA) performs DMA
**without any cache maintenance**, and the IDMA does not snoop the D-cache.

The mounter reuses **a single 512-byte `blkbuf`** for every sector
(`lib_scsi.c:data_read_write10`) and that buffer lives in `.ram0a` (AXI SRAM,
cacheable in the mounter). Result: the CPU always served (and wrote) its stale
cached copy → "every LBA is block 0" on reads, and writes never reached the
card.

The original Axoloti hardware (STM32F4, Cortex-M4, no cache) never hit this;
the AKSO (STM32H7, M7 with D-cache) does.

## Fix (commit `e7788fd`, 2 files)

- `external/ChibiOS-Contrib/os/various/lib_scsi.c` — in `data_read_write10`:
  `cacheBufferInvalidate(buf, bs)` before `blkRead`, and
  `cacheBufferFlush(buf, bs)` before `blkWrite`. (`blkbuf` is 32-byte aligned by
  the `msdStart` contract, which is what the cache helpers require.)
- `external/ChibiOS/os/hal/ports/STM32/LLD/SDMMCv2/hal_sdc_lld.c` — removed the
  debug `while(1);` from `sdc_lld_write_aligned`, which used to hang the board
  (a thread trap, not a reset) on a write failure.

**General implication:** any buffer handed to the SDMMC IDMA must be either
cache-maintained around the transfer or placed in non-cacheable memory.

## Build and deploy the mounter

```sh
# x86 toolchain under Rosetta (GCC 8-2019)
cd axoloti/firmware/mounter && make
# then copy the full set into <bundle>/Contents/Resources/firmware/mounter/mounter_build/
```

Copy the **whole set**, not only `mounter.sram1.bin`. Known-good hashes:

| Build | Hash (sram1) |
|---|---|
| First fix (`e7788fd`) | `d7d66975…` |
| With the host-IRQ fix below (2026-10-01) | `8e60ad24…` |

## Validation on hardware (2026-09-28)

- MBR ends in `55AA`; distinct LBAs return distinct sectors.
- `fsck_msdos -n` exits 0.
- Raw write test with an open file descriptor (so macOS does not remount between
  steps): 4 patterns at 4 LBAs, no cross-contamination, original content
  restored.

The microSD itself was reformatted outside the SoC (MBR + FAT32, 1 MiB offset)
and its files restored from a Mac card reader.

---

## Follow-up 2026-10-01 — card reader hangs when a MIDI controller is attached

**Symptom:** entering card reader mode with a MIDI controller plugged into the
USB host port left the board hung (LEDs off) and the SD card never mounted on
the Mac. Without a controller it mounted fine.

Note that the patcher logs `LIBUSB_ERROR_PIPE` / `TimeoutException` /
`Disconnected from device` in **both** cases — that is **expected**: the device
re-enumerates as mass storage. The log from
`TargetMenu.jMenuItemMountActionPerformed` is informational, not a failure.

**Root cause (verified in the vector tables):**

- The main firmware enables the USB host IRQ in the NVIC:
  `HAL_NVIC_EnableIRQ(OTG_HS_IRQn)` (`usbh_conf.c:153`). `USB_OTG_HS` is
  `USB1_OTG_HS`, **IRQ 77**, whose handler is `CH_IRQ_HANDLER(Vector174)` in
  `usbh_conf.c` (the USB device is OTG_FS/USB2, IRQ 101 → a different vector).
- The mounter takes over by calling `_crt0_entry()` from `patch_init()`. **That
  is not a MCU reset**: `NVIC->ISER` (enable) and any pending bit survive.
  ChibiOS only sets priorities in `port_init`, and `rccResetAHB1(~0)` in
  `halInit()` resets the *peripheral*, not the NVIC.
- In the mounter image there is **no handler for the host**:
  `Vector174 = 0x24000fc2 = _unhandled_exception` (a `b .stay` infinite loop),
  while `Vector1D4` (IRQ 101 = OTG_FS device) is defined by `hal_usb_lld.o`.
  With a controller attached the host raises IRQs (SOF / port connect), so on
  the first `cpsie i` (inside `chSysInit`) the CPU jumps into the loop → kernel
  stuck, LEDs off, SD never enumerated. Same family as the hot-plug hang in
  `MIDI-USB-HOST-FINDINGS.md` §9.

**Fix applied** in `patch_init()` of `mounter/main.c` **and** `flasher/main.c`
(the flasher had the same vulnerable pattern), before `_crt0_entry()`:

```c
NVIC_DisableIRQ(OTG_HS_IRQn);
NVIC_ClearPendingIRQ(OTG_HS_IRQn);
RCC->AHB1RSTR |= RCC_AHB1RSTR_USB1OTGHSRST;
```

**Build:** `make` in `axoloti/firmware/mounter` and `.../flasher` (x86 GCC 8
under Rosetta). New hashes: mounter `8e60ad24…` (sram1), flasher `fddc1e1a…`
(sram1).

**Deploy:** the full mounter + flasher set into
`Contents/Resources/firmware/{mounter,flasher}/*_build/` of both the dev bundle
and the installed app. **No board reflashing required** — the main firmware did
not change.

**Validated on hardware (2026-10-01):** with the MIDI controller on the host
port, card reader mode mounts the SD card and the board no longer hangs.

## Known pending issue — there is no way back from card reader mode *(inherited from upstream, deferred)*

> **Status (2026-10-03):** the original Axoloti/Akso behaves the same way, so this
> is **not a regression of this fork** and it is **not a priority** to fix. It is
> documented here for whoever picks it up (and because the patcher message is
> misleading).

**Symptom:** ejecting the SD card from macOS leaves the mounter running (LED
blinking) and the patcher never reconnects
(`No available USB device found with matching PID/VID`). A **power cycle** (or
DFU reset) is required.

**Cause:** the mounter takes over the MCU (`_crt0_entry → main()`) and ends in
an infinite loop, so `msdStop()` is unreachable. Ejecting only unmounts the
volume; the USB device stays enumerated as ST mass storage
(`0x0483/0x5740`), while the patcher looks for `0x16C0/0x0442`
(`IConnection.java`). The patcher message ("eject ... to enable editor
connection again") is misleading. Upstream had this noted as a TODO in that loop
(*"do a system reset when card is unmounted by host…"*) and never implemented
it.

**Options when this is picked up:**

- **A (minimal):** in the mounter, handle the SCSI `START_STOP_UNIT` command
  (0x1B) with `LoEj=1` by calling `NVIC_SystemReset()` (returns to the main
  firmware and the patcher reconnects). `lib_scsi.c` is compiled **only** into
  the mounter. Depends on macOS actually sending that command on eject (needs
  hardware validation).
- **B (robust, touches the app):** when the patcher cannot find
  `0x16C0/0x0442` but does see the ST mass storage device `0x0483/0x5740`, call
  `libusb_reset_device()`; the mounter resets upon receiving `USB_EVENT_RESET`
  while already configured.

## Related pending issue — intermittent SD mount at boot

The main firmware mounts the SD card **intermittently** at boot: `sysmon` reads
`SDCSW` (GPIOD13), which reads 1 on many AKSO power-ups, interprets that as
"card removed" and calls `sdcard_unmount()`. When it happens, starting a patch
remounts the card. This is unrelated to mass storage — see
`MICROSD-DIAGNOSTICS.md` and the project notes.
