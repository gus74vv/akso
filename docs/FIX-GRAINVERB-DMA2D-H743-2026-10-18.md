# Fix: `jt/granular/grainverb` — STM32F427-only DMA2D code on the AKSO (STM32H743)

- **Date:** 2026-10-18
- **Status:** FIXED and **VALIDATED ON HARDWARE for all three objects by the
  user (2026-10-18)**: `grainverb`, `graintable` and `graindelay` compile and
  work (option "B", CPU-copy path). The `-Waggressive-loop-optimizations`
  warning (signed overflow of the phase accumulator) was also fixed by making
  the accumulator unsigned.
- **Scope decision (same day, by Gustavo):** this time the community-library
  object **is** being fixed, explicitly (contrast with the reverted
  `autoSource` fix of 2026-10-08, `FIX-AUTOSOURCE-MODSOURCES-2026-10-08.md`).

## Symptom

Going Live with a patch that contains `jt/granular/grainverb` fails to compile
with four errors in `~/Library/Akso/build/xpatch.cpp`:

```
xpatch.cpp:101: error: 'DMA2D_IFSR_CTEIF' was not declared; did you mean 'DMA2D_IFCR_CTEIF'?
xpatch.cpp:102: error: 'DMA2D_IFSR_CTCIF' was not declared; did you mean 'DMA2D_IFCR_CTCIF'?
xpatch.cpp:212: error: 'RCC_AHB1ENR_DMA2DEN' was not declared; did you mean 'RCC_AHB1ENR_DMA2EN'?
xpatch.cpp:213: error: 'RCC_AHB1RSTR_DMA2DRST' was not declared; did you mean 'RCC_AHB1RSTR_DMA2RST'?
make: *** [.../xpatch.o] Error 1
```

The patch is compiled into `xpatch.cpp` by the app (java) using
`axoloti/firmware/Makefile.patch` against the STM32H743 headers
(`external/ChibiOS/os/common/ext/ST/STM32H7xx/stm32h743xx.h`).

## Root cause

`grainverb.axo` embeds C code written for the **STM32F427** (original Axoloti
hardware). The AKSO is an **STM32H743**, so three classes of wrongness stack up:

1. **Typos (wrong on any STM32):** the DMA2D flag-**clear** register is `IFCR`;
   there is no `IFSR` register anywhere. `DMA2D_IFSR_CTEIF/CTCIF` →
   `DMA2D_IFCR_CTEIF/CTCIF`.
2. **Wrong chip **register names:** on the F427 the DMA2D clock/reset live in
   RCC AHB1; on the H743 the DMA2D is clocked/reset via **RCC AHB3**
   (`RCC_AHB3ENR_DMA2DEN`, `RCC_AHB3RSTR_DMA2DRST`), which *do* exist in the
   H743 headers (stm32h743xx.h). The rest of the DMA2D register map the object
   uses (`CR`, `FGMAR`, `OMAR`, `NLR`, `FGPFCCR`, `OPFCCR`, …) is identical on
   both families, and the `DMA2D` peripheral pointer exists on the H7 too.
3. **Would still be broken on the H743 even with the names fixed** (memory map
   + cache):
   - The k-rate ping-pong buffers `dmabuf1/2` live in `section(".sram2")`.
     `ramlink.ld` (the linker script for patches, `Makefile.patch` LDFLAGS
     `-T ../firmware/ramlink.ld`) maps `.sram2` to **0x2001C000** — the F427's
     SRAM2. On the H743 that range is **DTCM** (`D1_DTCMRAM_BASE` = 0x20000000,
     128K), and the TCM is reachable **only by the Cortex-M7 core** — bus
     masters such as DMA2D cannot access it.
   - The "enable SDRam read burst" hack reads/writes `0xA0000140`, which is
     `FMC_SDCR1` **of the F427** (`FMC_R_BASE` = 0xA0000000 on all STM32F4).
     On the H7 the FMC registers are at 0x52000000 (`FMC_Bank5_6_R_BASE` =
     0x52000140); 0xA0000140 is not the FMC on the H743. It is also redundant:
     the AKSO firmware already enables read burst in `configSDRAM()`
     (axoloti/firmware/sdram.c, `sdram_cfg.sdcr` includes `FMC_Read_Burst_Enable`).
   - The H743 boots with the **D-cache enabled** (`crt1.c:` `__core_init()` →
     `SCB_EnableDCache()`) and AXI masters (DMA2D included) **do not snoop the
     cache** — the same class of issue as the SDMMC card-reader fix. The CPU
     writes the `.sdram` array and DMA2D would read it behind the cache's back
     (dirty lines), and vice-versa for the dmabufs. Any future re-enable of the
     DMA2D path needs explicit `SCB_CleanDCache`/`SCB_InvalidateDCache` around
     the transfers.

## Fix applied ("option B": use the object's own no-DMA path)

All edits are confined to the library object
`~/Library/Akso/akso-community/objects/jt/granular/grainverb.axo`
(**outside** this repo; the app library is `~/Library/Akso`):
nothing in this repo, the firmware, or `ramlink.ld` was touched.

- Removed `setupDMACopy()`, `initiateDMACopy()`, `waitDMACopyComplete()` and the
  DMA2D clock/reset init (replaced by explanatory comments).
- Removed the `.sram2` ping-pong buffers, the FMC read-burst hack (init +
  dispose + the `save_FMC_SDCR1` save/restore).
- In the k-rate code, the DMA branch was disabled (`#if 1 // use DMA` →
  `#if 0`, left in place as dead reference with a "do not re-enable" comment),
  so the object's own **CPU-copy branch** (`#else // implementation without DMA
  copy`, reads grains directly from SDRAM) is what now runs.

Rationale: the CPU path removes every H7-specific hazard above (no bus-master
access to TCM, no cache coherency, no FMC address assumptions), needs no linker
changes, and the extra CPU cost is negligible (8 words per grain on a Cortex-M7
with the SDRAM read burst already enabled by the firmware).

**Backup (original preserved):**
`~/Library/Akso/akso-community/objects/jt/granular/grainverb.axo.bak-preNODMAfix-20261018`
(the loader ignores it — it filters with `endsWith(".axo")`,
`AxoObjects.java:257`).

**To revert:** `cp` that `.bak-…` over `grainverb.axo` and restart the app.
(`akso-community` is its own git repo; the change there is left uncommitted on
purpose, same convention as the autoSource incident.)

**To validate:** open a patch with the object, go Live, confirm it compiles and
the reverb behaves (expect CPU-copy audio; no board session was run as part of
this change).

## Follow-up (DONE 2026-10-18): the same fix extended to the whole `jt/granular` family

`jt/granular/graintable.axo` and `jt/granular/graindelay.axo` contained the
**identical** STM32F427-only scaffold (verified by grep over
`~/Library/Akso/objects` + `akso-community/objects` — nothing outside
`jt/granular` uses `DMA2D`, `DMA2D_IFSR_*`, `RCC_AHB1*_DMA2D*` or `0xA0000140`):

- `DMA2D->IFCR |= DMA2D_IFSR_CTEIF/CTCIF`,
- `RCC->AHB1ENR |= RCC_AHB1ENR_DMA2DEN; RCC->AHB1RSTR …DMA2DRST`,
- `save_FMC_SDCR1 = *((uint32_t *)0xA0000140); …`,
- `dmabuf1/2` in `section(".sram2")`.

They would have failed to compile with exactly the same four errors. The
same option-B treatment (remove the DMA2D helpers, the FMC hack and the
`.sram2` buffers; `#if 1 // use DMA` → `#if 0` so the object's own CPU-copy
branch runs) was **applied to both on 2026-10-18**, with backups:
`graintable.axo.bak-preNODMAfix-20261018` and
`graindelay.axo.bak-preNODMAfix-20261018`. Both validated on hardware by the
user the same day.

**Extra fix (all three objects):** the CPU-copy branch triggered
`warning: iteration 7 invokes undefined behavior [-Waggressive-loop-optimizations]`
on `phl += incr1` — the per-grain phase accumulator is *meant* to wrap mod
2<sup>32</sup>, but as a signed `int` that is signed-overflow UB. Changing the
local to `uint32_t phl = phase;` makes the wrap defined and silences the
warning without changing behavior.
