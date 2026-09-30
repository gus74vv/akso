# AKSO — USB host MIDI out / hot-plug: hallazgos de sesión

Sesión de depuración (30/09/2026). Objetivo: manejar los LEDs del **KORG
nanoKONTROL2** desde un patch de AKSO (objeto `midi/out/cc` / `cc thin`,
device = "usb host port 1").

**Regla de lectura:** separar lo **VERIFICADO** (con evidencia) de lo
**NO PROBADO / DESCARTADO**. No repetir tests ya hechos.

---

## 0. Estado final de esta sesión

- **MIDI out → LEDs del nanoKONTROL2: RESUELTO** (verificado en hardware).
  Fix de 1 línea en `axoloti/firmware/usbh_midi_core.c` (§5).
- **Hot-plug del controlador: NO resuelto** — sigue reseteando el board. Es lo
  próximo a fixear (§10).
- Baseline de referencia: **`012F09F4`** (reproducible bit-a-bit, §2).
- La instrumentación usada está en `/tmp/session_instrumentation.patch` (224
  líneas, **no** commiteada). Receta en §6.

---

## 1. VERIFICADO — Deploy: hay que copiar el `.elf`, no sólo el `.bin`

**Síntoma**: al flashear un firmware nuevo (sólo copiando `axoloti.bin` al
bundle), el patch **crashea al go-live**:
`Control transfer failed: -9` / `receive error: LIBUSB_ERROR_PIPE` /
`Disconnected from device.`

**Causa (verificada en código)**: el patch se linkea con

```
axoloti/firmware/Makefile.patch:108
  $(LD) $(LDFLAGS) "$<" ... -Wl,-Map=...,--cref,--just-symbols=${FIRMWARE}/build/axoloti.elf -o "$@"
```

o sea contra **`<Akso.app>/Contents/Resources/firmware/build/axoloti.elf`**.
El app compila el patch desde `Axoloti.firmwareDirectory()`
(= `<Akso.app>/Contents/Resources/firmware`, ver `CompilePatch.java` y
`Axoloti.java:firmwareDirectory()`), y `Makefile.patch` usa
`FIRMWARE = ../firmware`, que resuelve a la **misma** carpeta.

Si se reemplaza sólo `axoloti.bin`, el patch se linkea con los **símbolos
viejos** del `.elf` del baseline mientras el board corre un firmware nuevo con
direcciones corridas → el patch llama a direcciones equivocadas.

**Fix operativo (VERIFICADO)**: al deployar un firmware, copiar **el set
completo** a `<bundle>/Contents/Resources/firmware/build/`:

```
axoloti.bin  axoloti.elf  axoloti.hex  axoloti.map  axoloti.dmp  axoloti.list
```

y **reiniciar el app** (para que el `make` del patch vea el `.elf` nuevo).

Con eso, un firmware con cambios dejó de crashear al go-live — el crash era el
deploy, no el código.

---

## 2. VERIFICADO — Baseline reproducible y cómo se calcula el fwid

- `cd axoloti/firmware && make` desde source **prístino** →
  **`axoloti.bin` de 1210976 B, CRC32 (zlib) = `012F09F4`** (bit-a-bit).
- El fwid que muestra el app = **CRC32 (java.util.zip.CRC32) de `axoloti.bin`**
  (`axoloti/src/main/java/axoloti/utils/FirmwareID.java`).
- El board también reporta su fwid (`GetFirmwareID()`, `pconnection.c`).

**Toolchain**: `external/gcc-arm/mac/gcc-arm-none-eabi-8-2019-q3-update/bin`
agregarlo al `PATH` antes de `make`.

---

## 3. VERIFICADO — El USB host MIDI out del AKSO genera bien

Instrumentación (contadores + `LogTextMessage`, ver §6) probó, con el patch
corriendo y el controlador en el puerto host:

| Observación | Significado |
|---|---|
| `q == s` (1,2,3,…) | **todos** los mensajes encolados se transmiten |
| `st=2` (SEND_DATA), `r=0` | state machine sano, sin trabas |
| `urb=1` (URB DONE) | el bulk-OUT **completa**: el device ACK-ea la transferencia |
| `in` crece al apretar botones del controlador | el host **recibe** del device |
| `last=0B B0 42 7F` / `0B B0 42 00` | USB-MIDI event correcto: cable 0, CIN `0xB`, status `0xB0` (canal 1), CC `0x42` (66), valor 127/0 |
| `last=09 90 5E xx` / `08 80 5E xx` | Note On/Off bien formados |

**Conclusión**: framing, canal, CC, valores y transferencia son **correctos**.

### Nota sobre "siempre manda 127"

Con **LFO en `trig` y el mismo LFO en `v`**, el `cc` manda `v` en el flanco de
subida — que coincide con el LFO en 1 → **siempre 127**. No es bug. Para ver
0 hay que **desacoplar** (`v` = dial/toggle, `trig` = clock), o usar `cc thin`
(sin `trig`, manda al cambiar `v`).

---

## 4. VERIFICADO — Descriptor real del nanoKONTROL2 y parse del AKSO

Dump crudo del config descriptor (instrumentación, `CfgDesc_Raw`):

```
Config: 09 02 53 00 01 01 00 80 32
        bLength=09 type=02 wTotalLength=0x0053 bNumInterfaces=01
        bConfigurationValue=01 iConfiguration=00 bmAttributes=0x80 bMaxPower=0x32
Ifc 0:  09 04 00 00 02 01 03 00 00
        if=00 alt=00 bNumEndpoints=02 class=01(AUDIO) sub=03(MIDIStreaming)
  CS:   07 24 01 00 01 25 00        (MS header, wTotalLength=0x25)
        06 24 02 01 10 03           (MIDI IN jack, embedded, id 0x10)
        09 24 03 02 40 01 10 01 04  (MIDI OUT jack, external, id 0x40 <- 0x10)
        09 24 03 01 30 01 20 01 05  (MIDI OUT jack, embedded, id 0x30 <- 0x20)
        06 24 02 02 20 04           (MIDI IN jack, external, id 0x20)
  EP 02: 09 05 02 02 40 00 00 00 00  + 05 25 01 01 10
        addr=0x02 BULK wMaxPacket=64 ; CS endpoint: bNumEmbMIDIJack=1, jack 0x10
  EP 81: 09 05 81 02 40 00 00 00 00  + 05 25 01 01 30
        addr=0x81 BULK wMaxPacket=64 ; CS endpoint: bNumEmbMIDIJack=1, jack 0x30
```

- El device tiene **1 sola interface** (AUDIO/MIDIStreaming) — **no** es un
  error de parse, ni falta la AudioControl interface.
- Endpoints: OUT `0x02`, IN `0x81`.
- El AKSO reporta `if0/1 e02810000` y elige `ep=02/81` → **coincide con el
  descriptor**. La enumeración/parse del AKSO es **correcta**.

---

## 5. RESUELTO — El LED del nanoKONTROL2 no reaccionaba (D-cache / DMA)

**Síntoma**: el firmware mandaba el CC correcto (`0B B0 42 00/7F`), la
transferencia completaba (`urb=1`)… y el LED no cambiaba. El mismo controlador
en la **Mac** (Max `ctlout`) sí funcionaba.

**Causa raíz (VERIFICADA en hardware)**: incoherencia de **D-cache** con el
**DMA del USB host**.

- El HCD tiene DMA habilitado (`axoloti/firmware/usbh_conf.c`, `USBH_LL_Init`:
  `hhcd.Init.dma_enable = 1`) y el HAL HCD **no hace mantenimiento de cache**.
- `SCB_EnableDCache()` está activo (`crt1.c`).
- Memoria **no-cacheable**: sólo SRAM3 (`0x30040000`) vía ChibiOS
  (`cfg/mcuconf.h: STM32_NOCACHE_SRAM3 = TRUE` → región MPU en
  `hal_lld_init()`) y `ram0a` (`main.c` MPU_REGION_7).
- **Input** (device→AKSO): el buffer vive en `.ram3` (heap `USBH_malloc` →
  `mem[256]` en `.ram3`) → **no-cacheable** → funcionaba.
- **Output** (AKSO→device): se armaba en `outbuf`, un `static` en
  `USBH_MIDI_ProcessOutput` → **`.bss` = ram1 (`0x30000000`), cacheable** →
  las escrituras del CPU quedaban en la D-cache y **el DMA leía RAM stale** →
  el device recibía basura y la ignoraba.

Es la **misma familia** del bug de SD/IDMA ya documentado (fix con
`cacheBufferFlush`/`cacheBufferInvalidate`).

**Fix (1 línea)** — `axoloti/firmware/usbh_midi_core.c`,
`USBH_MIDI_ProcessOutput`, case `MIDI_SEND_DATA`:

```c
static midi_message_t outbuf[4] __attribute__((section(".ram3"))) __attribute__((aligned(32)));
```

(de `.bss` cacheable a `.ram3` no-cacheable). Verificado: con esto **el LED
sigue al toggle** (`cc thin` + toggle, cc 66).

**Alternativa equivalente**: `cacheBufferFlush(outbuf, s*4)` antes del
`USBH_BulkSendData` (con `outbuf` alineado a 32 B; `cacheBufferFlush` está en
`external/ChibiOS/os/hal/ports/common/ARMCMx/cache.h`, `n` = bytes).

**Implicancia general**: cualquier buffer que se le pase al **DMA del USB host**
debe estar en memoria no-cacheable (o flushearse). Aplica a futuros buffers de
salida del host.

---

## 6. VERIFICADO — Instrumentación usada (para no re-descubrir)

Cambios en `/tmp/session_instrumentation.patch` (aplicar sobre el baseline):

- `axoloti/firmware/midi_usbh.c/.h`: contadores globales
  (`usbh_out_queued/sent/retry/state/urb/in_events/last_ph/b0/b1/b2`,
  `usbh_out_ep/in_ep`, `usbh_niface/sel_iface`, `usbh_ep_addr[8]`,
  `usbh_cfg_len`, `usbh_cfg_raw[512]`). `usbh_in_events += len` en
  `usbhmidi_cb`.
- `axoloti/firmware/midi.c` → `MidiSendVirtual`: `usbh_out_queued++` al encolar.
- `axoloti/firmware/usbh_midi_core.c`:
  - captura de `iface`/endpoints/`CfgDesc_Raw` en `USBH_MIDI_InterfaceInit`;
  - `usbh_out_state/urb/ep/in_ep` y `usbh_out_last_*` en `USBH_MIDI_ProcessOutput`.
- `axoloti/firmware/sysmon.c`: en el thread sysmon, `LogTextMessage` **cada ~1 s**
  con una línea **corta** (`mo q.. s.. u.. l..`), más `moEp` cada ~5 s y un dump
  del config descriptor en bloques de 8 bytes (`cdXX ..`).

**Canal de salida**: `LogTextMessage` (firmware) → header `AxoT` por el bulk →
el app lo muestra en su **consola Java** (`USBBulkConnection_v2`, case
`tx_hdr_log`, `Level.WARNING`).

**Común a todo log**: `LogTextMessage` es **bloqueante** (`chBSemWait`) → usar
con moderación, desde el thread `sysmon`, no desde el k-rate.
**IMPORTANTE**: líneas **largas se truncan** en la consola → mantenerlas cortas
(≤ ~50 chars).

---

## 7. DESCARTADO (no volver a probar)

| Hipótesis | Por qué se descartó |
|---|---|
| "El host manda un solo mensaje y se traba" | `q == s` — transmite todos |
| "El AKSO nunca manda 0" | Sí manda (`last=0B B0 42 00`), si el patch lo produce |
| "El deploy/code-size rompe el board" | Era el `.elf` faltante (§1) |
| "El config descriptor overflow causa el hot-plug" | El descriptor es 83 B (< 512) → no aplica |
| "El host elige mal el endpoint" | `ep=02/81` coincide con el descriptor (§4) |
| "Falta un objeto `change`/flipflop" | El `cc` lee `v` directo; el problema era el trigger |

---

## 8. Warts del fork AKSO (contexto)

- `axoloti/firmware/sysmon.c`: monitor de SD sobre `SDCSW` (GPIOD13) — pin
  **no confiable** en AKSO; en flanco 0→1 llama `StopPatch()` + `sdcard_unmount()`.
- `axoloti/firmware/patch.c:157` `StartPatch1()`: limpia CCM RAM
  (`0x20000000..0x2001d000`) y llama `sdcard_attemptMountIfUnmounted()`.
- El stack de USB host es **la librería vieja de ST** (`external/STM32_USB_Host_Library`)
  emparejada con el **HCD del H7** (`external/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_hcd.c`)
  y el glue `axoloti/firmware/usbh_conf.c`. Hay además una copia
  `STM32_USB_Host_Library_LEGACY` (no usada) y un stack USBH de ChibiOS-Contrib
  (no usado).
- La app (arm64) compila el patch con el toolchain embebido
  (`external/gcc-arm/mac/...`); el toolchain de **firmware** es x86 y corre bajo
  Rosetta.

---

## 9. PRÓXIMO A FIXEAR — hot-plug del controlador resetea el board

**Síntoma (VERIFICADO)**: al hot-pluggear el nanoKONTROL2 (con el board
conectado al patcher), el board **se resetea** y se desconecta del app:
`Control transfer failed: -9` / `receive error: LIBUSB_ERROR_PIPE` /
`Disconnected from device.` (mismo patrón que el crash de go-live de §1, pero
este **no** es el deploy).

**Estado**: NO resuelto. Ocurre también con el fix del §5 aplicado.

**Lo que se probó y NO sirvió**:
- Clampear el largo de la lectura del config descriptor a
  `USBH_MAX_SIZE_CONFIGURATION` en `USBH_Get_CfgDesc()`
  (`external/STM32_USB_Host_Library/Core/Src/usbh_ctlreq.c`). El reset persiste.
- Nota: el descriptor del nanoKONTROL2 es **83 bytes** (< 512), así que el
  overflow de `CfgDesc_Raw[512]` **no aplica** a este device.

**Workaround operativo**: conectar el controlador **antes** de encender el board.

**Pistas a explorar** (NO probadas):
- El rol de la **D-cache/DMA** en la **enumeración** (control transfers). El
  buffer de control vive en `hUSBHost` (`.ram3`, no-cacheable), pero hay que
  revisar **todos** los buffers que toca el HCD durante la enumeración
  (`CfgDesc_Raw`, `device.CfgDesc`, los buffers de string desc, etc.).
- `USBH_AllocPipe`/`USBH_OpenPipe`/toggle al re-enumerar en caliente.
- Interacción con `sysmon`/`SDCSW` (`StopPatch()` en flanco 0→1) y con
  `sdcard_unmount()` durante el hot-plug.
- El handler de excepciones (`exceptions.c`) hace `NVIC_SystemReset()`; el dump
  no sobrevive al reset en AKSO. Para capturar la PC haría falta el experimento
  `.noinit` (ver historial del repo) **en pasos chicos**.
