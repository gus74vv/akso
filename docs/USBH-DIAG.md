# USBH-DIAG — build de diagnóstico del USB host (carga de DSP inflada)

**Fecha:** 07/10/2026
**Síntoma que investiga:** con un controlador MIDI enchufado al puerto USB host, la
carga de DSP que reporta el patcher sube (~60% relativo) **aunque el patch no use
ningún objeto MIDI**.

---

## 1. Hipótesis (por qué sube sin que el patch cambie)

1. **La métrica es tiempo de reloj de pared, no ciclos del patch.**
   `patch.c:243-259`:

   ```c
   tStart = port_rt_get_counter_value();
   ... dsp_process + drenaje MIDI in + adc_convert() ...
   DspTime = (port_rt_get_counter_value() - tStart);
   dspLoadPct = (DspTime) / (STM32_SYS_CK / 300000);
   ```

   y `port_rt_get_counter_value()` es `DWT->CYCCNT`
   (`external/ChibiOS/os/common/ports/ARMCMx/chcore_v7m.h:781`): **sigue contando
   mientras al hilo DSP lo interrumpen**. O sea: `dsp%` = fracción del período de
   audio (~333 µs = 160.000 ciclos @480 MHz) que ocupa la ráfaga del hilo DSP,
   **incluyendo todo lo que lo desaloja**.

2. **Con un device conectado, el stack USB host del firmware agrega:**

   - IRQ del OTG HS a **prioridad NVIC 6** (`usbh_conf.c:150`), o sea por encima
     de todos los hilos de ChibiOS. Handler: `Vector174` con
     `chSysLockFromISR` + `HAL_HCD_IRQHandler` + `chSysUnlockFromISR`
     (`usbh_conf.c:576+`).
   - **SOF habilitado** (`Sof_enable = 1` → `GINTMSK_SOFM`,
     `external/STM32H7xx_HAL_Driver/Src/stm32h7xx_ll_usb.c:414`) y su callback
     encadena `HAL_HCD_SOF_Callback → USBH_LL_IncTimer → USBH_HandleSof →
     USBH_MIDI_SOFProcess` (`usbh_conf.c:194`, `usbh_core.c:1206`,
     `usbh_midi_core.c:455`), que **postea un evento al hilo USB en cada SOF**.
     En high speed el SOF es cada microframe (125 µs); no hay filtrado por
     número de frame.
   - La clase MIDI **polea el bulk-IN en forma continua**
     (`usbh_midi_core.c:290`). Un device sin datos responde **NAK**, y el core
     tiene `HCINTMSK_NAKM` habilitado (`stm32h7xx_ll_usb.c:1557-1584`) → IRQ por
     cada intento; cada una llama `USBH_LL_NotifyURBChange` →
     `osMessagePutI(USBH_PORT_EVENT)` (`usbh_conf.c:224`, `usbh_core.c:1366`).
   - Hilo USB host creado con **`HIGHPRIO`, igual que el hilo DSP** (macro
     `osThreadDef` propio en `usbh_conf.h`), durmiendo solo 1 ms por vuelta
     (`usbh_core.c:1334-1340`).
   - Contención real de memoria: la DMA del host escribe en `.ram3` (SRAM no
     cacheable, `STM32_NOCACHE_SRAM3 TRUE` en `cfg/mcuconf.h`) y el ISR/DMA
     expulsa líneas de D-cache → los mismos ciclos de patch cuestan más ciclos
     de reloj.

3. Todo eso es **carga de sistema, no del patch** → explica que suba igual con o
   sin objetos MIDI. Si el número pasa 95/98% además se saltea el VU y se cuentan
   `underruns` (`patch.c:258-275`).

## 2. Qué agrega este build (mínimo)

Build **separado** (`build-diag/`), activado con `-DUSBH_DIAG=1`. Con `USBH_DIAG`
sin definir **no cambia nada** (ver §5, verificación).

- Mide en `Vector174` los ciclos (`DWT->CYCCNT`) que se pasan dentro del ISR del
  USB y los promedia en ventanas de 1 s → `usbh_diag_isr_pct` (+ `usbh_diag_isr_hz`).
- **La barra de "dsp" del patcher muestra ese porcentaje** (el ack reporta
  `usbh_diag_isr_pct` en el campo `dspload`). Sin controlador debería dar ~0-1%.
- **No arranca el patch guardado** (`/start.bin` de la SD, ni el del flash
  interno): flashear otro firmware corre los símbolos (ver §4b) y arrancarlos da
  reset loop. El patch se hace go-live desde el patcher como siempre.
- No toca el SOF, ni el hilo USB, ni el MIDI, ni los LEDs.

> Una primera versión con 4 modos en runtime (SOF a 1 kHz / SOF apagado / CC de
> cambio / feedback por LED) se descartó después del incidente de §4b, para no
> meter más variables mientras el board estaba en loop.

Archivos:

| Archivo | Qué |
|---|---|
| `axoloti/firmware/usbh_diag.h/.c` | nuevo: contadores + tick de 1 s |
| `axoloti/firmware/usbh_conf.c` | medición DWT en `Vector174`; `usbh_diag_init()` |
| `axoloti/firmware/sysmon.c` | llama `usbh_diag_tick()` en su loop de 100 ms |
| `axoloti/firmware/pconnection.c` | reporta el ISR% en `dspload` |
| `axoloti/firmware/patch.c` | no arranca el patch guardado |
| `axoloti/firmware/Makefile` | `ifdef USBH_DIAG` → agrega `usbh_diag.c` + `-DUSBH_DIAG=1` |

## 2b. Fix aplicado: enmascarar la IRQ de NAK (07/10/2026)

**Medición 07/10/2026** (build A, con la barra = carga del ISR): con el controlador
conectado el ISR del USB se comía **20% del CPU** (0% desenchufado). Eso es el
"interrupt storm" del host ST: el core reintenta solo cuando el device responde
NAK, pero **cada NAK levanta una IRQ** (`HCINTMSK_NAKM` viene habilitado para los
canales bulk, `external/STM32H7xx_HAL_Driver/Src/stm32h7xx_ll_usb.c:1557-1584`), y
la clase MIDI polea el bulk-IN en forma continua (`usbh_midi_core.c:290`).

Fix: limpiar ese bit en los dos canales del MIDI después de abrirlos
(`usbh_midi_core.c`, guardado por `USBH_FIX_NAK`). El reintento sigue (lo hace el
hardware) y de la transferencia nos enteramos por XFRC.

| Build | Flags | Dir | bin |
|---|---|---|---|
| A (mide + fix) | `USBH_DIAG=1` | `build-diag/` | `995577e9…` (reconstruir si se vuelve a usar) |
| **firmware con el fix (normal)** | *(ninguno)* | `build-fix/` y cualquier build limpio | `3a41af77…` — crc32 `0x3238BAE5` |
| anterior, sin fix (rollback) | — | `build/` | `8cfa4f18…` — crc32 `0x7072DEBF` |

El `#if USBH_FIX_NAK` se sacó: **el fix está en el build normal**. Verificado: un
build limpio da exactamente el mismo `.bin` (`3a41af77…`) que se validó en
hardware, y `build/` (sin fix) quedó intacto como rollback.

Deploy: `./fw-deploy.sh fix` (con fix), `./fw-deploy.sh diag` (instrumentado),
`./fw-deploy.sh current` (rollback sin fix).

### Resultado en hardware (07/10/2026)

Con el controlador (nanoKONTROL2) enchufado y el build A, medido en la app:

| | antes del fix | con el fix (A) |
|---|---|---|
| ISR del USB (barra) | **20%** | **0%** |
| MIDI in/out del host | OK | **OK** |
| patches go-live | OK | OK |

→ El storm eran los NAK del bulk: enmascarar esa IRQ baja el costo del ISR del USB
a ~0 sin romper nada. Falta el cierre con el build B (dsp% real con/sin controlador)
y el re-test de hot-plug (ver §8).

## 3. Build (no toca `build/`)

```sh
cd ~/Documents/dev/akso/axoloti/firmware
export PATH=~/Documents/dev/akso/external/gcc-arm/mac/gcc-arm-none-eabi-8-2019-q3-update/bin:$PATH
mkdir -p build-diag/obj build-diag/lst .dep-diag
make USBH_DIAG=1 BUILDDIR=build-diag DEPDIR=.dep-diag -j8
```

Sale `build-diag/axoloti.{bin,elf,hex,map,dmp,list}`. `build/` queda intacto.

Y para que la app lo pueda flashear hay que deployarlo al bundle (con backup
automático):

```sh
cd ~/Documents/dev/akso
./fw-deploy.sh diag
```

Estado actual de los artefactos:

| Artefacto | md5 |
|---|---|
| `build/axoloti.bin` (flasheado = `7072DEBF`, y en ambos bundles) | `8cfa4f1886d2cea8dbc897a945415978` |
| `build-diag/axoloti.bin` (diag) | `5dad6070eb58473f4b5156d1b7ee3934` |

## 4. Flash + deploy

**OJO (regla dura del proyecto):** el patch se linkea con
`--just-symbols=<bundle>/Contents/Resources/firmware/build/axoloti.elf`
(`Makefile.patch:112`). Si se flashea el `.bin` y el `.elf` del bundle no
coincide, el patch puede romper al go-live → **hay que copiar el SET COMPLETO**
(`bin`+`elf`+`hex`+`map`+`dmp`+`list`) al bundle, guardando antes el set viejo.

El flash lo dispara la app desde `<bundle>/Contents/Resources/firmware/build/axoloti.bin`
(`TargetMenu.java:250/272`, `MainFrame.java:626`) y "Flash (rescue)" vía
`platform_osx/upload_fw_dfu.sh`.

```sh
cd ~/Documents/dev/akso
./fw-deploy.sh diag     # backup del set previo + deploy del set diag en los 2 bundles
```

El script guarda el set que reemplaza en
`<bundle>/Contents/Resources/firmware/build/backup-diag-usbh/` (y no lo pisa si
corre dos veces). Después hay que **reiniciar la app**.

Estado ya aplicado (07/10/2026): el set diag (`axoloti.bin` = `5dad6070…`) está
deployado en `package/mac/Akso.app` y en `/Applications/Akso.app`. Falta sólo el
flash desde la app (paso 4).

Después: Board → Flash, y reconectar la app. El firmware reporta su propio CRC32
de flash (`pconnection.c:953`), así que el patcher toma el ID nuevo solo.

## 4b. Incidente 07/10/2026 — el patch de arranque, CONFIRMADO

**Síntoma (2 veces):** tras flashear un firmware modificado, el board entra en loop
de arranque (LED verde → destello verde rápido → alternancia verde/rojo,
repitiéndose) y el patcher no conecta o conecta, lee versión/CRC y se desconecta
(`LIBUSB_ERROR_PIPE`).

**Causa (confirmada):** cualquier firmware que **agregue o saque código** corre las
direcciones de los símbolos (medido en el primer diag: **1408 de 2540** símbolos
movidos; el `.bin` +400 B). El patch que arranca solo (`/start.bin` de la SD vía
`patch.c:501` `LoadPatchStartSD`, o el del flash interno vía `patch.c:512`) está
linkeado con `--just-symbols` contra el ELF del firmware **viejo**, así que salta a
direcciones equivocadas → hard fault → `NVIC_SystemReset()` (`exceptions.c:526`) →
reset loop. Volver al firmware bueno lo arregla (el patch vuelve a coincidir).

**Cómo evitarlo mientras se prueba un firmware nuevo:** el build diag **no arranca
el patch guardado** (`LoadPatchStartSD`/`LoadPatchStartFlash` con `#if USBH_DIAG`).
⚠️ Cuidado: ese guard cuelga de `USBH_DIAG`, así que un build con **sólo**
`USBH_FIX_NAK` **sí** arranca `/start.bin` → cae en este mismo loop (pasó el
07/10 con el build B, que además quedó en el bundle por un bug del script
`fw-deploy.sh`). Para cualquier build de prueba nuevo: o activar también el guard
(p. ej. `#if USBH_DIAG || USBH_FIX_NAK`), o sacar la SD, o —mejor— hacer el cambio
**neutro en tamaño** para no mover símbolos.

**Flash (Rescue) no salva si el diag sigue en el bundle**: ese camino también toma
`<bundle>/Contents/Resources/firmware/build/axoloti.bin`. Hay que hacer
`./fw-deploy.sh current` **antes** de flashear. Y si el patcher no conecta (loop
muy rápido), `Board → Flash` no sirve: hay que ir por **DFU** (switch 1 al
conectar).

**CRC32 de referencia** (la app muestra el CRC del firmware conectado; el del
board sale de `CalcCRC32` de su propia flash, `pconnection.c:953`):

| Build | crc32 | len |
|---|---|---|
| `build/` (bueno, `7072DEBF`) | `0x7072DEBF` | 1211744 |
| diag+fix (A) | `0xC4CD8B35` | 1211944 |
| fix solo (B) | `0x3238BAE5` | 1211776 |

## 5. Verificación hecha en este build

- `make` normal (sin `USBH_DIAG`) reconstruido en `build-check/` → `.bin`
  **bit a bit idéntico** a `build/axoloti.bin` (`8cfa4f18…`): los cambios son
  inertes sin el define. (`build-check/` ya borrado.)
- `build-diag` compila limpio (exit 0); símbolos `usbh_diag_*` presentes en
  `build-diag/axoloti.map`; `usbh_diag.o` en `0x8029800`.

## 6. Protocolo de medición

Con este firmware, la barra de "dsp" del patcher **es** el % de CPU dentro del ISR
del USB host.

| # | estado | barra |
|---|---|---|
| 1 | patch live, controlador desenchufado | (esperado ~0-1%) |
| 2 | mismo patch, controlador enchufado | **?** ← este es el número |

Comparar con el salto medido con el firmware bueno (mismo patch): sin controlador
= A, con controlador = B (tu observación: B ≃ 1.6·A).

Interpretación:

- Si (2) ≈ (B − A) en puntos → el aumento es **tiempo robado por el ISR** del USB,
  y el fix apunta al ritmo de IRQs (SOF a 8 kHz + NAKs del poleo bulk-IN).
- Si (2) es mucho menor que (B − A) → lo que infla la métrica es la **contención de
  memoria/cache** del DMA del host (u otra cosa), no el ISR.

## 7. Rollback

```sh
cd ~/Documents/dev/akso
./fw-deploy.sh current     # restaura el set bueno (build/, 7072DEBF) en los 2 bundles
# después: reiniciar la app y Board → Flash
```

- `./fw-deploy.sh restore` hace lo mismo pero tomando el set del backup del repo
  (`axoloti/firmware/build-backup-7072DEBF/`), por si `build/` se toca.
- Copias del set bueno que quedan: `axoloti/firmware/build/` (md5 `8cfa4f18…`),
  `axoloti/firmware/build-backup-7072DEBF/`, y en cada bundle
  `.../firmware/build/backup-diag-usbh/` + `.../backup-7072DEBF/`.
- Si al flashear el diag el board quedara sin responder: `Board → Flash (Rescue)`
  (switch 1 apretado al encender, DFU) funciona igual, y después el
  `./fw-deploy.sh current` de arriba.

## 8. Pendiente / siguiente paso

Candidatos, en orden de evidencia:

1. **CERRADO (07/10/2026):** enmascarar `NAKM` en los canales del MIDI, ya en el
   build normal (sin flag). Verificado en hardware: ISR del USB 20% → ~0%, dsp
   real igual con y sin controlador, MIDI in/out sin pérdidas, patches OK.
   Detalle en `MIDI-USB-HOST-FINDINGS.md` §11.
   Pendiente **aparte** (no abordado todavía): el hot-plug del controlador
   (`MIDI-USB-HOST-FINDINGS.md` §9) sigue igual — se traba el kernel, no es el
   storm de NAKs.
2. Sin usar todavía: no correr el `SOFProcess` de la clase en cada SOF (muestrear a
   1 ms) y no postear evento si ya hay URB pendiente.
3. Sin usar todavía: bajar el hilo USB host a `NORMALPRIO` (hoy es `HIGHPRIO`,
   igual que el DSP).
