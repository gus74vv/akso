# AKSO — USB host MIDI out / hot-plug: findings

Debugging session (2026-09-30). Initial goal: drive the LEDs of a
**KORG nanoKONTROL2** from an AKSO patch (objects `midi/out/cc` / `cc thin`,
device = "usb host port 1").

**How to read this document:** it separates what is **VERIFIED** (with evidence)
from what is **UNTESTED / RULED OUT**. Do not repeat tests that were already
done.

---

## 0. Status

- **MIDI out → nanoKONTROL2 LEDs: SOLVED** (verified on hardware). One-line fix,
  see §5.
- **Controller hot-plug: NOT solved**, but there is a **workaround** (§9). It only
  hangs in one configuration: the USB-C(male)→USB-A(female) adapter left inserted
  in the **board's** host port, with the controller plugged into it afterwards.
  With the adapter on the **controller's** cable, hot-plug works (refinement of
  2026-10-03).
- **Firmware deploy**: the **full set** must be copied, not just the `.bin`
  (§1) — this was the actual cause of a go-live crash.
- Reference baseline: **`012F09F4`** (reproducible bit-for-bit, §2).
- The instrumentation used lives in `/tmp/session_instrumentation.patch`
  (never committed); recipe in §6.

---

## 1. VERIFIED — Deploy: you must copy the `.elf`, not just the `.bin`

**Symptom:** after flashing a new firmware (copying only `axoloti.bin` into the
bundle), a patch **crashes at go-live**: `Control transfer failed: -9` /
`receive error: LIBUSB_ERROR_PIPE` / `Disconnected from device.`

**Cause (verified in code):** the patch is linked against

```
axoloti/firmware/Makefile.patch:108
  $(LD) $(LDFLAGS) "$<" ... -Wl,-Map=...,--cref,--just-symbols=${FIRMWARE}/build/axoloti.elf -o "$@"
```

that is, against **`<Akso.app>/Contents/Resources/firmware/build/axoloti.elf`**.
The app compiles the patch from `Axoloti.firmwareDirectory()`
(= `<Akso.app>/Contents/Resources/firmware`, see `CompilePatch.java` and
`Axoloti.java:firmwareDirectory()`), and `Makefile.patch` uses
`FIRMWARE = ../firmware`, which resolves to that **same** folder.

If only `axoloti.bin` is replaced, the patch links against the **old symbols**
from the baseline `.elf` while the board runs new firmware at shifted addresses
→ the patch calls the wrong addresses.

**Operational fix (VERIFIED):** when deploying firmware, copy the **full set**
into `<bundle>/Contents/Resources/firmware/build/`:

```
axoloti.bin  axoloti.elf  axoloti.hex  axoloti.map  axoloti.dmp  axoloti.list
```

and **restart the app** (so the patch `make` picks up the new `.elf`).

---

## 2. VERIFIED — Reproducible baseline and how the firmware ID is computed

- `cd axoloti/firmware && make` from a **pristine** tree →
  **`axoloti.bin` of 1210976 B, CRC32 (zlib) = `012F09F4`** (bit-for-bit).
- The firmware ID shown by the app is the **CRC32 (java.util.zip.CRC32) of
  `axoloti.bin`** (`axoloti/src/main/java/axoloti/utils/FirmwareID.java`).

**Toolchain:** `external/gcc-arm/mac/gcc-arm-none-eabi-8-2019-q3-update/bin` on
`PATH` before `make`.

---

## 3. VERIFIED — AKSO's USB host MIDI out does generate correctly

With instrumentation (counters + `LogTextMessage`, §6), patch running and the
controller on the host port:

| Observation | Meaning |
|---|---|
| `q == s` (1,2,3,…) | **all** queued messages are transmitted |
| `st=2` (SEND_DATA), `r=0` | state machine healthy |
| `urb=1` (URB DONE) | the bulk-OUT completes: the device ACKs |
| `in` grows when buttons are pressed | the host **receives** from the device |
| `last=0B B0 42 7F` / `0B B0 42 00` | valid USB-MIDI: cable 0, CIN `0xB`, channel 1, CC 66, 127/0 |
| `last=09 90 5E xx` / `08 80 5E xx` | well-formed Note On/Off |

**Note about "it always sends 127":** with an **LFO in `trig` and the same LFO
in `v`**, `cc` sends `v` on the rising edge — which coincides with the LFO being
at 1 → always 127. Not a bug: decouple the inputs (`v` driven by a dial/toggle)
or use `cc thin`.

---

## 4. VERIFIED — Real nanoKONTROL2 descriptor and AKSO's parser

Raw config descriptor dump (`CfgDesc_Raw`):

```
Config: 09 02 53 00 01 01 00 80 32
        wTotalLength=0x0053 bNumInterfaces=01 bmAttributes=0x80 bMaxPower=0x32
Ifc 0:  09 04 00 00 02 01 03 00 00
        if=00 alt=00 bNumEndpoints=02 class=01(AUDIO) sub=03(MIDIStreaming)
  CS:   07 24 01 ...  (MS header) + jacks 0x10/0x30/0x20/0x40
  EP 02: 09 05 02 02 40 00 00 00 00  + 05 25 01 01 10   (BULK OUT, wMaxPacket 64, jack 0x10)
  EP 81: 09 05 81 02 40 00 00 00 00  + 05 25 01 01 30   (BULK IN,  wMaxPacket 64, jack 0x30)
```

- The device has **one single interface** (AUDIO/MIDIStreaming) — that is how it
  is, not a parsing error.
- Endpoints: OUT `0x02`, IN `0x81`.
- AKSO reports `if0/1 e02810000` and picks `ep=02/81` → **matches**. AKSO's
  enumeration/parsing is **correct**.
- **Descriptor = 83 B** (< 512) → the `CfgDesc_Raw[512]` overflow does **not**
  apply to this device.

---

## 5. SOLVED — The LED did not react: D-cache vs USB host DMA

**Symptom:** the firmware sent the correct CC, the transfer completed
(`urb=1`)… and the LED did not change. The same controller worked on the
**Mac** (`ctlout`).

**Root cause (VERIFIED on hardware):** **D-cache** incoherence with the
**USB host DMA**.

- The HCD has DMA enabled (`usbh_conf.c`, `USBH_LL_Init`:
  `hhcd.Init.dma_enable = 1`) and the HCD HAL does **no cache maintenance**.
- `SCB_EnableDCache()` is active (`crt1.c`).
- Non-cacheable memory: SRAM3 (`0x30040000`) through ChibiOS
  (`cfg/mcuconf.h`: `STM32_NOCACHE_SRAM3 = TRUE` → MPU region in
  `hal_lld_init()`) and `ram0a` (`main.c`, MPU_REGION_7).
- **Input**: the buffer lives in `.ram3` (`USBH_malloc` → `mem[256]`) →
  non-cacheable → **worked**.
- **Output**: it was built in `outbuf`, a `static` inside
  `USBH_MIDI_ProcessOutput` → **`.bss` = ram1 (`0x30000000`), cacheable** →
  CPU writes stayed in the D-cache and **the DMA read stale RAM** → the device
  received garbage.

This is the **same family** as the SD/IDMA bug (fixed with `cacheBufferFlush`).

**Fix (1 line)** — `axoloti/firmware/usbh_midi_core.c`:

```c
static midi_message_t outbuf[4] __attribute__((section(".ram3"))) __attribute__((aligned(32)));
```

**General implication:** any buffer handed to the **USB host DMA** must live in
non-cacheable memory (or be flushed explicitly).

---

## 6. VERIFIED — Instrumentation used (so it does not need rediscovering)

Changes in `/tmp/session_instrumentation.patch` (on top of the baseline):

- `midi_usbh.c/.h`: global counters (`usbh_out_queued/sent/retry/state/urb/
  in_events/last_ph/b0/b1/b2`, `usbh_out_ep/in_ep`, `usbh_niface/sel_iface`,
  `usbh_ep_addr[8]`, `usbh_cfg_len`, `usbh_cfg_raw[512]`).
- `midi.c` → `MidiSendVirtual`: `usbh_out_queued++` on enqueue.
- `usbh_midi_core.c`: capture interface/endpoints/`CfgDesc_Raw` in
  `USBH_MIDI_InterfaceInit`; `usbh_out_state/urb/ep/in_ep` and
  `usbh_out_last_*` in `USBH_MIDI_ProcessOutput`.
- `sysmon.c`: one **short** `LogTextMessage` line every ~1 s.

**Channel:** `LogTextMessage` (firmware) → `AxoT` header over bulk → the app
prints it in its **Java console** (`USBBulkConnection_v2`, case `tx_hdr_log`,
`Level.WARNING`).

**Careful:** `LogTextMessage` is **blocking** (`chBSemWait`) → use it sparingly,
from the `sysmon` thread, never from the k-rate code or an ISR. And **long lines
are truncated** in the console.

---

## 7. RULED OUT (do not retest)

| Hypothesis | Why it was ruled out |
|---|---|
| "The host sends only one message and stalls" | `q == s` — it transmits all of them |
| "AKSO never sends 0" | It does, if the patch produces it |
| "The deploy / code size breaks the board" | It was the missing `.elf` (§1) |
| "The config descriptor overflow causes the hot-plug hang" | The descriptor is 83 B (< 512) |
| "The host picks the wrong endpoint" | `ep=02/81` matches the descriptor (§4) |
| "The watchdog resets the board" | `WATCHDOG_ENABLED = 0` |
| "`HAL_Delay` hangs (uwTick not advancing)" | AKSO overrides it to `chThdSleepMilliseconds` |
| "Pipes are exhausted" | `DeInitStateMachine` clears `Pipes[]` |
| "A `change`/flipflop object is missing" | `cc` reads `v` directly; the issue was the trigger |

---

## 8. AKSO firmware quirks (context)

- `sysmon.c`: SD card monitor on `SDCSW` (GPIOD13) — an **unreliable** pin on
  AKSO; on a 0→1 edge it calls `StopPatch()` + `sdcard_unmount()`.
- `patch.c:157` `StartPatch1()`: clears CCM RAM (`0x20000000..0x2001d000`) and
  calls `sdcard_attemptMountIfUnmounted()`.
- The USB host stack is **ST's legacy library**
  (`external/STM32_USB_Host_Library`) + the **H7 HCD**
  (`external/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_hcd.c`) + the glue in
  `axoloti/firmware/usbh_conf.c`. There is also a `_LEGACY` copy and a
  ChibiOS-Contrib USBH stack (neither is used).

---

## 9. OPEN — controller hot-plug hangs the board (no reset)

**Symptom (VERIFIED):** hot-plugging the nanoKONTROL2 makes the board **hang**:
the LEDs go dark completely and it **does not recover on its own** (the USB has
to be re-enumerated or power cycled). The app shows
`Control transfer failed: -5` / `receive error: LIBUSB_ERROR_PIPE` /
`Disconnected from device.`

**What BKPSRAM markers showed (VERIFIED):**
- The **CPU is still alive** (re-enumerating the USB makes the app find it) but
  the **kernel is stuck** (threads stop running → LEDs off).
- The last step reached is the **exit of the host ISR** (marker `1501`, right
  after `HAL_HCD_IRQHandler`); the state machine (`USBH_Process`) **never runs
  again** → something leaves the kernel blocked inside/while exiting the ISR.

**TRIGGER (VERIFIED) — what matters is where the adapter sits, not hot-plugging per se:**
- Adapter (USB-C male → USB-A female) **left inserted in the board's host port**
  (a dangling female socket), then the controller's USB-A cable plugged into that
  adapter → **hangs**.
- Adapter **attached to the controller's cable**, then the whole assembly plugged
  into the board → **fine**, no hang (even with the board already running).

**Refinement (2026-10-03):** hot-plugging the controller is safe as long as the
adapter travels with the controller's cable. What hangs the board is connecting
into an adapter that is **already sitting in the board's port**, i.e. the board
gets a connect event on a port whose mechanical/electrical state changed earlier
(when the adapter was inserted). So the relevant variable is the board-side
dangling socket, not the act of hot-plugging a device.

**Workaround (decision of 2026-09-30):** keep the adapter on the controller side
(adapter attached to the controller's cable, then plug the assembly into AKSO),
or connect everything before powering the board on.

**Fix applied — `osMessagePutI` (correct, but NOT sufficient):**
in `external/STM32_USB_Host_Library/Core/Src/usbh_core.c`,
`USBH_LL_PortEnabled`, `USBH_LL_Connect`, `USBH_LL_Disconnect` and
`USBH_LL_NotifyURBChange` used `osMessagePut`, which in this port maps to
**`chEvtSignal`** (`usbh_conf.h`) — **not ISR-safe** — and they **are called from
the HCD ISR** (`HAL_HCD_*_Callback`). They now use `osMessagePutI`
(`chEvtSignalI`). **The dirty-connection hang persists with the fix** → there is
another cause.

**Tried and did not help:**
- Clamping the config descriptor read to `USBH_MAX_SIZE_CONFIGURATION` in
  `USBH_GetCfgDesc()` (`usbh_ctlreq.c`) — the reset/hang persists (and does not
  apply: 83 B descriptor).
- Markers between ISR exit and the hang: the state machine never runs, so there
  is nothing to mark there.

**Leads to pick this up again** (NOT tested):
- Look for **other** non-ISR-safe APIs called from the HCD ISR (review the
  `HAL_HCD_*_Callback` callbacks and everything they touch). In `usbh_core.c`,
  `USBH_Process`/`USBH_HandleEnum`/`USBH_ReEnumerate` still use `osMessagePut`,
  but those run in the host thread (fine).
- **connect/disconnect/fast-reconnect (bounce)** handling in the host: does
  de-init release and reuse everything correctly? (`Pipes[]`,
  `fakefree`/`fakemalloc` on `mem[256]`, `USBH_MIDI_InterfaceDeInit` vs
  `InterfaceInit`).
- Diagnostics: markers in **separate slots** (callbacks vs state machine) —
  today `1501` overwrites the callback that ran immediately before.
- `exceptions.c`: on AKSO the dump goes to `.bss` (it does not survive a reset).
  Moving it to `.ram3` (NOINIT) and enabling register reporting was tried: it
  worked as a diagnostic and was not kept. (A `.noinit`/BKPRAM region with an
  unbounded `while` **hangs the boot**; use a bounded timeout.)
