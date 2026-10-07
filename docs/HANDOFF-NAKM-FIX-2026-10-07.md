# HANDOFF — Fix de "dsp load" con controlador USB conectado (07/10/2026)

Para el próximo agente/sesión. **El fix está aplicado, validado en hardware y
commiteado + pusheado a `fork` el 07/10/2026** (ver `git log`: *"usb host:
enmascarar NAKM en los canales MIDI corta el interrupt storm"* + el andamiaje de
diagnóstico), y con la release **`v3.0.4-arm64`** publicada (assets: bundle con el
firmware fix + `axoloti-firmware-3238BAE5.zip`). El **hot-plug del controlador** se
aborda en la próxima sesión; no se tocó el 07/10.

---

## 1. TL;DR

- **Síntoma:** con el nanoKONTROL2 enchufado al puerto host USB, la "dsp load" del
  patcher subía (~+60% relativo) **aunque el patch no usara ningún objeto MIDI**.
- **Causa (medida, no teorizada):** el ISR del USB host se comía **20% del CPU**.
  `dspLoadPct` se mide con `DWT->CYCCNT` (reloj de pared, `patch.c:243-259`), así
  que cuenta el tiempo robado al hilo DSP dentro de su ventana. El robo venía de un
  **storm de IRQs por NAK**: el core host reintenta solo los NAK del bulk, pero
  **cada NAK levanta una IRQ** (`HCINTMSK_NAKM` viene en el mask bulk/ctrl,
  `external/STM32H7xx_HAL_Driver/Src/stm32h7xx_ll_usb.c:1557-1562`) y la clase MIDI
  polea el bulk-IN en forma continua (`usbh_midi_core.c:290`).
- **Fix:** limpiar `NAKM` en el mask de los **dos canales del MIDI** después de
  `USBH_OpenPipe` (`usbh_midi_core.c`, sin flag: está en el build normal).
- **Resultado en hardware:** ISR **20% → ~0%**; dsp real **igual con y sin
  controlador**; MIDI in/out sin pérdidas; patches go-live OK.
- **Firmware funcional actual (grabado en el board y en las dos apps del patcher):
  `crc32 0x3238BAE5`** — bin md5 `3a41af77…`, dir `axoloti/firmware/build-fix/`.
  Rollback (sin fix): `crc32 0x7072DEBF` — bin md5 `8cfa4f18…`, dir
  `axoloti/firmware/build/` (intacto a propósito).

## 2. Cambios en el working tree (sin commitear)

| Archivo | Qué |
|---|---|
| `axoloti/firmware/usbh_midi_core.c` | **el fix**: `USBH_HC(i)->HCINTMSK &= ~USB_OTG_HCINTMSK_NAKM` en los canales Out/In + macro `USBH_HC` (el tipo global del OTG no expone el array de canales) |
| `axoloti/firmware/usbh_diag.c` / `.h` | nuevo: medidor del ISR del USB (inerte sin `USBH_DIAG`) |
| `axoloti/firmware/usbh_conf.c` | mide `Vector174` con DWT + `usbh_diag_init()` |
| `axoloti/firmware/sysmon.c` | llama `usbh_diag_tick()` (ventana de 1 s) |
| `axoloti/firmware/pconnection.c` | en diag: barra `dspload` = ISR%, `underruns` = IRQs en kHz |
| `axoloti/firmware/patch.c` | en diag: **no arranca** `/start.bin` ni el patch del flash (evita el boot loop) |
| `axoloti/firmware/Makefile` | `ifdef USBH_DIAG` → agrega `usbh_diag.c` + `-DUSBH_DIAG=1` |
| `axoloti/.gitignore` | ignora `build-diag/`, `.dep-diag/`, `build-fix/`, `.dep-fix/`, `build-backup-*` |
| `docs/USBH-DIAG.md` | doc del build de diagnóstico + incidente del boot loop (§4b) |
| `docs/MIDI-USB-HOST-FINDINGS.md` | **§11 nuevo** (el fix) + §0 Status |
| `fw-deploy.sh` | tool: deploy/rollback del set de firmware a los 2 bundles |

Verificación de que el fix es el que se validó: un `make` limpio (sin flags) da el
mismo `.bin` (`3a41af77…`) que se probó en hardware.

## 3. Commit (ya hecho el 07/10/2026: la receta que se usó)

```sh
cd ~/Documents/dev/akso
git add axoloti/firmware/usbh_midi_core.c axoloti/firmware/usbh_diag.c axoloti/firmware/usbh_diag.h \
        axoloti/firmware/Makefile axoloti/firmware/patch.c axoloti/firmware/pconnection.c \
        axoloti/firmware/sysmon.c axoloti/firmware/usbh_conf.c axoloti/.gitignore \
        docs/USBH-DIAG.md docs/MIDI-USB-HOST-FINDINGS.md docs/HANDOFF-NAKM-FIX-2026-10-07.md \
        fw-deploy.sh
# NO incluir NOTA.txt, TODO.md ni docs/PATCHER-PARAM-EXCHANGE.md (son cambios previos del dueño)
git commit -m "usb host: enmascarar NAKM en los canales MIDI corta el interrupt storm" \
  -m "Con un controlador conectado el ISR del USB comia 20% del CPU y la 'dsp load' del patcher subia (~+60% relativo) aunque el patch no usara MIDI: dspLoadPct se mide con DWT->CYCCNT (reloj de pared), asi que cuenta el tiempo robado al hilo DSP. El core host reintenta solo los NAK, pero cada NAK levantaba una IRQ y la clase MIDI polea el bulk-IN de forma continua." \
  -m "Fix: limpiar NAKM en los dos canales del MIDI tras USBH_OpenPipe. Verificado en hardware: ISR 20% -> ~0%, dsp real igual con/sin controlador, MIDI in/out sin perdidas. Firmware pre-fix 0x7072DEBF, con fix 0x3238BAE5." \
  -m "Incluye el build de diagnostico USBH_DIAG (inerte sin el flag) y fw-deploy.sh."
# push SOLO a nuestro repo (nunca a origin):
# git push fork master
```

## 4. Flashear / volver atrás

- `./fw-deploy.sh fix` (firmware con el fix) · `./fw-deploy.sh diag` (instrumentado)
  · `./fw-deploy.sh current` (rollback sin fix). Después: cerrar/abrir el patcher y
  **Board → Flash**.
- Si el patcher **no conecta** (p.ej. boot loop): **Board → Flash (Rescue)** con el
  **switch 1 apretado** al conectar el USB (camino DFU; no necesita conexión).
  Ojo: el rescue también flashea `<bundle>/…/firmware/build/axoloti.bin`, así que el
  bundle manda para los dos caminos.
- ⚠️ **Regla dura:** cualquier firmware que cambie el layout de símbolos
  **invalida el patch guardado** (`/start.bin` en la SD, o el del flash interno) →
  hard fault → **reset loop** (LEDs parpadeando, el patcher se cae con
  `LIBUSB_ERROR_PIPE`). Antes de flashear un build nuevo: **sacar la SD o borrar
  `/start.bin`**; después re-subirlo desde el patcher ("Upload to SDCard as
  startup"). Detalle y CRCs de referencia: `docs/USBH-DIAG.md` §4b.

## 5. Cómo se midió (para reusar)

```sh
cd ~/Documents/dev/akso/axoloti/firmware
export PATH=~/Documents/dev/akso/external/gcc-arm/mac/gcc-arm-none-eabi-8-2019-q3-update/bin:$PATH
make USBH_DIAG=1 BUILDDIR=build-diag DEPDIR=.dep-diag -j8   # no toca build/
```
Con ese build: la barra de "dsp" del patcher muestra el **% de CPU dentro del ISR
del USB** y el campo **"xrun"** (panel RT del MainFrame) los **IRQs del OTG en kHz**.
No arranca el patch guardado. Todo en `docs/USBH-DIAG.md`.

## 6. Próxima sesión: hot-plug del controlador (NO abordado)

- Estado, evidencia y leads: **`docs/MIDI-USB-HOST-FINDINGS.md` §9** (+ §0 Status).
- Trigger verificado: adaptador USB-C(male)→USB-A(female) **dejado en el puerto del
  board**, y después enchufar el controlador a ese adaptador → cuelga. Si el
  adaptador viaja pegado al cable del controlador, el hot-plug anda.
- No es un crash: el **CPU sigue vivo** pero el **kernel queda trabado** (LEDs
  apagados, la máquina de estados del host no vuelve a correr). El último marcador
  es la **salida del ISR del host**. No depende de que haya patch corriendo.
- Ya probado sin éxito: `osMessagePut` → `osMessagePutI` en los callbacks del HCD.
- Leads sin probar (en orden sugerido):
  1. **Balance lock/unlock del ISR**: `Vector174` mezcla `CH_IRQ_PROLOGUE/EPILOGUE`
     con `chSysLockFromISR()/chSysUnlockFromISR()`; si el contador de lock queda
     desbalanceado el kernel se traba exactamente así (comparar con la convención
     OSAL de ChibiOS: `OSAL_IRQ_PROLOGUE/EPILOGUE` + `osalSysLockFromISR`).
  2. Ciclo connect/disconnect rápido (bounce): liberación/reuso de `Pipes[]`,
     `fakemalloc`/`fakefree` del pool de 256 B, `USBH_MIDI_InterfaceInit` vs
     `InterfaceDeInit`.
  3. Marcadores en **slots separados** (callbacks vs state machine) y dump en
     `.ram3` (con timeout acotado; BKPRAM colgó el boot).
- Ventaja nueva: con el storm de NAKs eliminado, el ISR está mucho más limpio para
  instrumentar.

## 7. Estado para el dueño

- El fix está **funcionando en la app** que usa (dsp real igual con/sin
  controlador, MIDI sin pérdidas). El firmware grabado es `0x3238BAE5`.
- Commit, push a `fork` y release `v3.0.4-arm64`: hechos el 07/10/2026.
- El hot-plug queda para la próxima sesión.
