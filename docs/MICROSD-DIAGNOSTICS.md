# Diagnosis — 8 GB microSD through the AKSO SoC (2026-09-28)

> Mac (macOS) side diagnosis, before the root cause was found. Kept because the
> methodology (LBA-level tests) is reusable when a card or the mass-storage
> bridge misbehaves. **Resolution:** it was the SoC firmware, not the card — the
> SDMMC IDMA reads/writes buffers without D-cache maintenance, so the CPU served
> stale cached data. See `CARD-READER-MASS-STORAGE.md` for the fix
> (commit `e7788fd`).

## Initial symptom

- An 8 GB microSD connected to a SoC called **AKSO** (ChibiOS/RT firmware),
  exposed over USB as a mass storage device.
- The card showed as "unformatted" and **Disk Utility could neither format nor
  repair it**.

## Observations

| Finding | Detail |
|---|---|
| Device | `disk4`, 7.7 GB (7,744,782,336 bytes), USB, removable, not read-only |
| USB name | `"ChibiOS/RT Mass Storage Device"` (the SoC is a USB → SD bridge) |
| Initial layout | MBR (FDisk) with 1 FAT32 partition at offset 1 MiB |
| `fsck_msdos -n` | **Exit code 201** — invalid FAT |
| `diskutil verifyVolume` | Error `-69845` (file system verify or repair failed) |
| `diskutil eraseDisk FAT32 SD MBR` | Completed, but the volume ended up **0 B** and would not mount |
| Write speed | ~**120 KB/s** (normal: 10–20 MB/s) |

## Conclusive tests (write/read per LBA)

| Test | Result |
|---|---|
| Write 64 KB of `0xAB` at LBA 100000 | Appeared at **every LBA read** (0, 100000, 200000) |
| Write 512 bytes at LBA 200000 and 500000 | **Lost** — did not land in any sector |
| Write 64 KB of `0x55` at LBA 300000 | Again showed up at **every LBA** (0, 100000, 300000) |
| MBR after formatting | No `55AA` signature, empty/garbage partition table |

**Device behaviour:** any read returned the *last 64 KB block written*, and small
writes (the MBR, FAT entries) never reached the card. The requested LBA was
**ignored**.

## Conclusion at the time

> **The problem is not the microSD, it is the SoC's USB bridge.**
>
> The mass-storage implementation in the AKSO **ChibiOS/RT** firmware is broken:
> it does not map sectors correctly (all reads/writes hit the same 64 KB buffer)
> and drops small writes. That is why formatting "completes" on the Mac but the
> resulting FAT is corrupt and unmountable, and why Disk Utility cannot repair
> it.

## Recommendations (still valid)

1. **Format the microSD without going through the SoC:** take it out and use a
   microSD reader directly on the Mac (MBR + FAT32 takes seconds).
2. Re-insert it into the SoC and flash the relevant image/firmware.
3. If the SoC must expose the SD over USB (e.g. to flash without a PC), fix the
   board firmware — the bug is there, not in the card.

## Commands used (reference)

```bash
diskutil list
diskutil info /dev/disk4 /dev/disk4s1
diskutil verifyVolume /dev/disk4s1          # fsck_msdos -n → exit 201
sudo diskutil eraseDisk FAT32 SD MBR /dev/disk4
sudo dd if=/dev/zero of=/dev/disk4 bs=1M count=4 oflag=sync   # ~120 KB/s
# LBA aliasing test (python3, writing 0xAB / 0x55 / 0x77 patterns)
```

---

## Resolution (2026-09-28, commit `e7788fd`)

**Confirmed: the SoC's USB bridge was at fault, and the cause was the D-cache.**

- The Cortex-M7 D-cache is enabled from `crt1.c:__core_init()`, and the SDMMC
  IDMA does not snoop the cache. The mounter uses a single 512-byte `blkbuf`
  (`.ram0a`, AXI SRAM, cacheable) → the CPU read and wrote its stale cached
  copy: every read returned block 0 and writes carried old data. Exactly the
  observed symptom.
- **Fix:** cache maintenance in `lib_scsi.c` (`data_read_write10`):
  `cacheBufferInvalidate` before `blkRead`, `cacheBufferFlush` before
  `blkWrite`; plus removal of the debug `while(1)` in the SDMMC LLD.
- **Validated on hardware** (card reader): MBR `55AA`, distinct LBAs,
  `fsck_msdos -n` exit 0, and a raw write test with an open fd (4 patterns over
  4 LBAs, no contamination, restored correctly).

## Related open issue — SD mounts intermittently at boot

The main firmware mounts the SD card **intermittently** at boot: `sysmon` reads
`SDCSW` (GPIOD13), which reads 1 on many AKSO power-ups, interprets it as
"removed" and unmounts the card. When that happens, starting a patch remounts it
(`StartPatch1` → `sdcard_attemptMountIfUnmounted`). Known failed attempts:

- Seeding `sdcsw_prev` with the real pin level prevents the spurious unmount and
  leaves the card mounted… but **breaks patch boot/audio**, and the USB drops.
  Reverted.
- Enabling the H7 BKPRAM clock (`RCC_AHB4ENR_BKPRAMEN`) with a
  `while (PWR_CR2_BRRDY)` **hangs the boot** (recoverable only through DFU).

The bootloader ROM is always present, so an apparently bricked board is
recoverable: hold **switch 1** while connecting (DFU) and use
"Board → Flash (rescue)".
