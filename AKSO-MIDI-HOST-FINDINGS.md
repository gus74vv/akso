# AKSO — USB host MIDI out / hot-plug: hallazgos

Sesión de depuración (30/09/2026). Objetivo inicial: manejar los LEDs del
**KORG nanoKONTROL2** desde un patch de AKSO (objeto `midi/out/cc` / `cc thin`,
device = "usb host port 1").

**Regla de lectura:** separar lo **VERIFICADO** (con evidencia) de lo
**NO PROBADO / DESCARTADO**. No repetir tests ya hechos.

---

## 0. Estado

- **MIDI out → LEDs del nanoKONTROL2: RESUELTO** (verificado en hardware).
  Fix de 1 línea, commit `02799f9` (§5).
- **Hot-plug del controlador: NO resuelto**, pero con **workaround** (§9).
- **Deploy de firmware**: hay que copiar el **set completo**, no sólo el `.bin`
  (§1) — causa real de un crash de go-live.
- Baseline de referencia: **`012F09F4`** (reproducible bit-a-bit, §2).
- La instrumentación usada está en `/tmp/session_instrumentation.patch` (no
  commiteada). Receta en §6.

---

## 1. VERIFICADO — Deploy: hay que copiar el `.elf`, no sólo el `.bin`

**Síntoma**: al flashear un firmware nuevo (sólo copiando `axoloti.bin` al
bundle), el patch **crashea al go-live**:
`Control transfer failed: -9` / `receive error: LIBUSB_ERROR_PIPE` /
`Disconnected from device.`

**Causa (verificada en código)**: el patch se linkea contra

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

---

## 2. VERIFICADO — Baseline reproducible y cómo se calcula el fwid

- `cd axoloti/firmware && make` desde source **prístino** →
  **`axoloti.bin` de 1210976 B, CRC32 (zlib) = `012F09F4`** (bit-a-bit).
- El fwid que muestra el app = **CRC32 (java.util.zip.CRC32) de `axoloti.bin`**
  (`axoloti/src/main/java/axoloti/utils/FirmwareID.java`).

**Toolchain**: `external/gcc-arm/mac/gcc-arm-none-eabi-8-2019-q3-update/bin`
al `PATH` antes de `make`.

---

## 3. VERIFICADO — El USB host MIDI out del AKSO genera bien

Con instrumentación (contadores + `LogTextMessage`, §6), con el patch corriendo
y el controlador en el puerto host:

| Observación | Significado |
|---|---|
| `q == s` (1,2,3,…) | **todos** los mensajes encolados se transmiten |
| `st=2` (SEND_DATA), `r=0` | state machine sano |
| `urb=1` (URB DONE) | el bulk-OUT completa: el device ACK-ea |
| `in` crece al apretar botones | el host **recibe** del device |
| `last=0B B0 42 7F` / `0B B0 42 00` | USB-MIDI correcto: cable 0, CIN `0xB`, canal 1, CC 66, 127/0 |
| `last=09 90 5E xx` / `08 80 5E xx` | Note On/Off bien formados |

**Nota "siempre manda 127"**: con **LFO en `trig` y el mismo LFO en `v`**, el
`cc` manda `v` en el flanco de subida — que coincide con el LFO en 1 → siempre
127. No es bug: hay que **desacoplar** (`v` = dial/toggle) o usar `cc thin`.

---

## 4. VERIFICADO — Descriptor real del nanoKONTROL2 y parse del AKSO

Dump crudo del config descriptor (`CfgDesc_Raw`):

```
Config: 09 02 53 00 01 01 00 80 32
        wTotalLength=0x0053 bNumInterfaces=01 bmAttributes=0x80 bMaxPower=0x32
Ifc 0:  09 04 00 00 02 01 03 00 00
        if=00 alt=00 bNumEndpoints=02 class=01(AUDIO) sub=03(MIDIStreaming)
  CS:   07 24 01 ...  (MS header) + jacks 0x10/0x30/0x20/0x40
  EP 02: 09 05 02 02 40 00 00 00 00  + 05 25 01 01 10   (BULK OUT, wMaxPacket 64, jack 0x10)
  EP 81: 09 05 81 02 40 00 00 00 00  + 05 25 01 01 30   (BULK IN,  wMaxPacket 64, jack 0x30)
```

- El device tiene **1 sola interface** (AUDIO/MIDIStreaming) — es así, no es un
  error de parse.
- Endpoints: OUT `0x02`, IN `0x81`.
- El AKSO reporta `if0/1 e02810000` y elige `ep=02/81` → **coincide**. La
  enumeración/parse del AKSO es **correcta**.
- **Descriptor = 83 B** (< 512) → el overflow de `CfgDesc_Raw[512]` **no aplica**
  a este device.

---

## 5. RESUELTO — El LED no reaccionaba: D-cache / DMA del USB host

**Síntoma**: el firmware mandaba el CC correcto, la transferencia completaba
(`urb=1`)… y el LED no cambiaba. El mismo controlador en la **Mac** (Max
`ctlout`) sí funcionaba.

**Causa raíz (VERIFICADA en hardware)**: incoherencia de **D-cache** con el
**DMA del USB host**.

- El HCD tiene DMA habilitado (`usbh_conf.c`, `USBH_LL_Init`:
  `hhcd.Init.dma_enable = 1`) y el HAL HCD **no hace mantenimiento de cache**.
- `SCB_EnableDCache()` está activo (`crt1.c`).
- Memoria **no-cacheable**: SRAM3 (`0x30040000`) vía ChibiOS
  (`cfg/mcuconf.h`: `STM32_NOCACHE_SRAM3 = TRUE` → región MPU en
  `hal_lld_init()`) y `ram0a` (`main.c` MPU_REGION_7).
- **Input**: el buffer vive en `.ram3` (heap `USBH_malloc` → `mem[256]`) →
  no-cacheable → **funcionaba**.
- **Output**: se armaba en `outbuf`, un `static` de `USBH_MIDI_ProcessOutput`
  → **`.bss` = ram1 (`0x30000000`), cacheable** → las escrituras del CPU
  quedaban en la D-cache y **el DMA leía RAM stale** → el device recibía basura.

Es la **misma familia** del bug de SD/IDMA (fix con `cacheBufferFlush`).

**Fix (1 línea, commit `02799f9`)** — `axoloti/firmware/usbh_midi_core.c`:

```c
static midi_message_t outbuf[4] __attribute__((section(".ram3"))) __attribute__((aligned(32)));
```

**Implicancia general**: cualquier buffer que se le pase al **DMA del USB host**
debe estar en memoria no-cacheable (o flushearse).

---

## 6. VERIFICADO — Instrumentación usada (para no re-descubrir)

Cambios en `/tmp/session_instrumentation.patch` (sobre el baseline):

- `midi_usbh.c/.h`: contadores globales (`usbh_out_queued/sent/retry/state/urb/
  in_events/last_ph/b0/b1/b2`, `usbh_out_ep/in_ep`, `usbh_niface/sel_iface`,
  `usbh_ep_addr[8]`, `usbh_cfg_len`, `usbh_cfg_raw[512]`).
- `midi.c` → `MidiSendVirtual`: `usbh_out_queued++` al encolar.
- `usbh_midi_core.c`: captura de interface/endpoints/`CfgDesc_Raw` en
  `USBH_MIDI_InterfaceInit`; `usbh_out_state/urb/ep/in_ep` y `usbh_out_last_*`
  en `USBH_MIDI_ProcessOutput`.
- `sysmon.c`: `LogTextMessage` cada ~1 s con una línea **corta**.

**Canal**: `LogTextMessage` (firmware) → header `AxoT` por el bulk → el app lo
muestra en su **consola Java** (`USBBulkConnection_v2`, case `tx_hdr_log`,
`Level.WARNING`).

**Ojo**: `LogTextMessage` es **bloqueante** (`chBSemWait`) → usar con moderación,
desde el thread `sysmon`, no desde el k-rate ni un ISR. Y las líneas **largas se
truncan** en la consola.

---

## 7. DESCARTADO (no volver a probar)

| Hipótesis | Por qué se descartó |
|---|---|
| "El host manda un solo mensaje y se traba" | `q == s` — transmite todos |
| "El AKSO nunca manda 0" | Sí manda, si el patch lo produce |
| "El deploy/code-size rompe el board" | Era el `.elf` faltante (§1) |
| "El config descriptor overflow causa el hot-plug" | El descriptor es 83 B (< 512) |
| "El host elige mal el endpoint" | `ep=02/81` coincide con el descriptor (§4) |
| "El watchdog resetea el board" | `WATCHDOG_ENABLED = 0` |
| "`HAL_Delay` cuelga (uwTick no avanza)" | AKSO lo overridea a `chThdSleepMilliseconds` |
| "Se agotan los pipes" | `DeInitStateMachine` limpia `Pipes[]` |
| "Falta un objeto `change`/flipflop" | El `cc` lee `v` directo; el problema era el trigger |

---

## 8. Warts del fork AKSO (contexto)

- `sysmon.c`: monitor de SD sobre `SDCSW` (GPIOD13) — pin **no confiable** en
  AKSO; en flanco 0→1 llama `StopPatch()` + `sdcard_unmount()`.
- `patch.c:157` `StartPatch1()`: limpia CCM RAM (`0x20000000..0x2001d000`) y
  llama `sdcard_attemptMountIfUnmounted()`.
- El stack de USB host es **la librería vieja de ST**
  (`external/STM32_USB_Host_Library`) + el **HCD del H7**
  (`external/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_hcd.c`) + el glue
  `axoloti/firmware/usbh_conf.c`. Hay además una copia `_LEGACY` y un stack
  USBH de ChibiOS-Contrib (ninguno usado).
- El app (arm64) compila el patch con el toolchain embebido
  (`external/gcc-arm/mac/...`); el toolchain de **firmware** es x86 (Rosetta).

---

## 9. ABIERTO — hot-plug del controlador: el board se CUELGA (no resetea)

**Síntoma (VERIFICADO)**: al hot-pluggear el nanoKONTROL2, el board **se cuelga**:
los LEDs se apagan del todo y **no revive solo** (hace falta re-enumerar el USB
o cortar la energía). El app muestra `Control transfer failed: -5` /
`receive error: LIBUSB_ERROR_PIPE` / `Disconnected from device.`

**Aprendido con marcadores en BKPSRAM (VERIFICADO)**:
- El **CPU sigue vivo** (al re-enumerar el USB, el app lo encuentra) pero el
  **kernel queda trabado** (los threads no corren → LEDs apagados).
- El último paso alcanzado es la **salida del ISR del host** (marcador `1501`,
  justo después de `HAL_HCD_IRQHandler`); el state machine (`USBH_Process`)
  **nunca vuelve a correr** → algo deja el kernel bloqueado dentro/al salir del
  ISR.

**TRIGGER (VERIFICADO) — es la conexión "sucia", no el hot-plug limpio**:
- Adaptador USB-C→USB-A **puesto** en el AKSO + insertar el cable USB-A del
  controlador en el adaptador → **cuelga**.
- Ensamblar todo (controlador + adaptador) y **después** enchufar al AKSO →
  **NO cuelga**.

**Workaround (decisión 30/09/2026)**: ensamblar controlador + adaptador y recién
ahí conectarlo al AKSO (y conectar antes de encender el board).

**Fix aplicado — `osMessagePutI` (correcto, pero NO suficiente)**:
en `external/STM32_USB_Host_Library/Core/Src/usbh_core.c`,
`USBH_LL_PortEnabled`, `USBH_LL_Connect`, `USBH_LL_Disconnect` y
`USBH_LL_NotifyURBChange` usaban `osMessagePut`, que en este port mapea a
**`chEvtSignal`** (`usbh_conf.h`) — **no ISR-safe** — y **se llaman desde el ISR
del HCD** (`HAL_HCD_*_Callback`). Se cambiaron a `osMessagePutI`
(`chEvtSignalI`). **El cuelgue sucio persiste con el fix** → hay otra causa.

**Probado y NO sirvió**:
- Clampear la lectura del config descriptor a `USBH_MAX_SIZE_CONFIGURATION` en
  `USBH_GetCfgDesc()` (`usbh_ctlreq.c`) — el reset/cuelgue persiste (y no aplica:
  descriptor de 83 B).
- Marcadores entre la salida del ISR y el cuelgue: el state machine nunca corre,
  así que no hay nada que marcar ahí.

**Pistas para retomar** (NO probadas):
- Buscar **otras** APIs no-ISR-safe llamadas desde el ISR del HCD (revisar los
  callbacks `HAL_HCD_*_Callback` y todo lo que toquen). En `usbh_core.c` quedan
  `USBH_Process`/`USBH_HandleEnum`/`USBH_ReEnumerate` usando `osMessagePut`,
  pero esos corren en el thread del host (OK).
- Manejo de **connect/disconnect/connect rápido** (bounce) en el host: ¿el
  de-init libera/reusa todo bien? (`Pipes[]`, `fakefree`/`fakemalloc` del
  `mem[256]`, `USBH_MIDI_InterfaceDeInit` vs `InterfaceInit`).
- Diagnóstico: marcadores con **slots separados** (callbacks vs. state machine)
  — hoy el `1501` tapa el callback que corrió justo antes.
- `exceptions.c`: en AKSO el dump va a `.bss` (no sobrevive al reset). Se probó
  moverlo a `.ram3` (NOINIT) + habilitar el reporte de registros — sirvió como
  diagnóstico y no se dejó. (Un `.noinit`/BKPRAM con `while` sin timeout **cuelga
  el boot**; usar timeout acotado.)
