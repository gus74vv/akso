# Card reader mode — debugging (sesión 27/09/15:53, EN CURSO — actualización en vivo)

> Resumen de estado + plan para continuar. Contexto anterior: sesión pi
> `2026-09-27T16-53-41Z` (akso); transcripción extraída en
> `/tmp/akso_prev_full.txt` (puede no persistir).

## Síntoma

Card reader mode (mounter en SRAM, USB mass storage): **todos los LBAs
devuelven el mismo sector (el MBR / bloque 0)**, y **una escritura a un
"sector" cambia TODOS los sectores al mismo valor**.

**🔑 Deducción nueva (clave de esta sesión):** si una write a cualquier LBA
altera el MBR y todas las reads devuelven el MBR, entonces **reads Y
writes están siempre apuntando al bloque 0 de la SD**. El argumento de
bloque (block number) está "stuck en 0" en algún punto del camino
(LBA→SCSI→blkbuf→sdc_read/write→comando SDMMC), o el comando SDMMC se
emite con argumento 0/stale. Esto encaja con "el MBR real se repetía":
la card sí está devolviendo el dato que se le pide… siempre bloque 0.

## Flow de entrada a card reader mode (VERIFICADO en código)

`TargetMenu.java` (Enter card reader mode):
1. `conn.transmitStop()`
2. `TargetModel.uploadPatchToMemory(firmwareDirectory()/mounter/mounter_build/mounter.sram1.bin)`
3. `conn.transmitStart()`

→ El mounter **no es un boot independiente**: se carga como "patch" de
Axoloti vía protocolo AKSO **mientras el firmware principal sigue
corriendo**. El board NO se resetea. El firmware principal ya tiene la SD
conectada (el file manager usa `SDCD1` + FatFs — `axoloti/firmware/sdcard.c`,
`FATFS SDC_FS`, `sdcDisconnect(&SDCD1)` en sdcard.c:52). El mounter hace
`sdcStart/sdcConnect` fresco **sobre el mismo periférico** → **dos
instancias de `SDCDriver` (memorias distintas: flash y SRAM) controlando
el mismo SDMMC**. Candidato fuerte a conflicto de estado.

## LLD compilado: SDMMCv2 (confirmado de nuevo)

- Port H7xx: `os/hal/ports/STM32/... platform.mk` incluye **sin
  condiciones** `LLD/SDMMCv2/driver.mk`. Ambos builds (firmware principal
  y mounter) compilan el mismo `SDMMCv2/hal_sdc_lld.c`.
- Leí el archivo local COMPLETO (867 líneas):
  `external/ChibiOS/os/hal/ports/STM32/LLD/SDMMCv2/hal_sdc_lld.c`
- ⚠️ **El upstream (master de ChibiOS/ChibiOS) tiene 928 líneas — el
  LLD local NO es idéntico al upstream.** Hay diferencias en la
  secuencia de read (arriba/abajo del arm de IDMA y envío de comando).
  Esto es sospechoso: puede ser una versión vieja, o una fix
  local. **Pendiente: diff local vs. upstream del read path.**
  (Copié el upstream a `/tmp/chibios_new_hal_sdc_lld.c` — puede no
  persistir; re-descargable:
  `curl -sL 'https://raw.githubusercontent.com/ChibiOS/ChibiOS/master/os/hal/ports/STM32/LLD/SDMMCv2/hal_sdc_lld.c'`)

## ChibiOS-Contrib (pregunta de la sesión: ¿actualizar?)

Comparado local vs. `github.com/ChibiOS/ChibiOS-Contrib` master:
- `hal_usb_msd.c`: **el local es MÁS nuevo/ completo que el master
  upstream** (incluye el BOT reset completo con stall/NAK y el
  `GET_MASS_STORAGE` que en upstream quedan comentados).
- `lib_scsi.c`: única diferencia = un cast en la línea 331
  (`(uint8_t)`).
- → **No hay nada que actualizar de ChibiOS-Contrib para nuestro H7.**
  (Nota: el git root real es `/Users/gustavo/Documents/dev/akso`;
  `external/ChibiOS-Contrib` es vendored, último commit nuestro:
  `e5646ab` fix ADC3.)

## Descartes acumulados (sesión anterior + esta)

| Pieza | Veredicto |
|---|---|
| `lib_scsi.c` | OK — LBA se pasa bien: `block * SDC_BLOCK_SIZE` a `sdcRead`. Igual que upstream. |
| `hal_usb_msd.c` | OK — igual/mejor que upstream. Callbacks de read/write del MSD son pasacables. |
| `hal_sdc.c` (ChibiOS) | OK — `sdcRead`/`sdcWrite` envían `block` al LLD tal cual. |
| Card física | Buena — el file manager (firmware principal) la lee bien. |
| blkbuf (mounter) | 512B, align 32, en `.ram0a` (SRAM) — verificado en main.c del mounter. |
| LLD seleccionado | SDMMCv2 (no SDIOv1 — la sesión anterior leía el equivocado). |

## Pendientes / próximos pasos (EN ORDEN)

1. **LEER `axoloti/firmware/mounter/main.c`** (callbacks `blkbuf_read`/
   `blkbuf_write`): verificar que el LBA llega intacto a `sdcRead`/
   `sdcWrite`, y qué hace con `SDCD1` (¿sdcStart fresco? ¿conflicto con
   la instancia del firmware principal?). ← **Siguiente paso concreto.**
2. **Diff del LLD local vs. upstream** (punto anterior): si el local
   difiere en la secuencia de read (arm de IDMA / comando), evaluar
   portar la secuencia upstream o entender por qué cambió.
3. Con la pista "bloque stuck en 0": instrumentar `sdc_lld_read` en el
   mounter para volcar el argumento de bloque real enviado al SDMMC
   (vía AKSO/LED) y localizar si el 0 nace en SCSI→blkbuf, en blkbuf→
   sdcRead, o en sdcRead→LLD.
4. Verificar qué hace el firmware principal antes de cargar el mounter:
   ¿desconecta la SD? ¿deja el SDMMC en estado que el sdcStart fresco
   del mounter no limpia (p.ej. estado DMA/IRQ stale, o la otra
   instancia de `SDCDriver` con `is_started`)?

## Archivos clave

- `axoloti/firmware/mounter/main.c` — mounter (blkbuf, sdcStart, patch_init)
- `axoloti/firmware/sdcard.c` — SD del firmware principal (referencia, funciona)
- `axoloti/src/main/java/axoloti/swingui/target/TargetMenu.java` — flow "Enter card reader mode" (uploadPatchToMemory)
- `external/ChibiOS/os/hal/ports/STM32/LLD/SDMMCv2/hal_sdc_lld.c` — LLD (867 líneas, difiere de upstream 928)
- `external/ChibiOS/os/hal/src/hal_sdc.c` — driver SDC (OK)
- `external/ChibiOS-Contrib/os/various/lib_scsi.c` — SCSI (OK)
- `external/ChibiOS-Contrib/os/hal/src/hal_usb_msd.c` — USB MSC (OK)
- `axoloti/firmware/mounter/mcuconf.h` — config SD del mounter (STM32_SDC_USE_SDMMC1, etc.)

## Nota de testing

La micro SD se quedó **sin MBR** (se borró en pruebas) → **reformatear
FAT32** antes de volver a probar. Test mínimo al reactivar:
`diskutil list`, `dd if=/dev/rdiskX bs=512 count=4 of=/tmp/t`, y
comparar sectores 0..3 (antes: todos idénticos al MBR).

---

# Actualización (tarde) — format `util/format`, hipótesis D-cache

## Cambios aplicados

### 1. `akso-factory/objects/util/format.axo` (código de diagnóstico, se compila al patch)
Reescrito completo. Estado actual:
- **Displays** (todos `int32.label`): `busy`, `succeeded`, `failed`, `b0`..`b4`.
- **Params**: `confirm1` (toggle), `confirm2` (momentary).
- **`code.declaration`**:
  - Miembros: `fs_error`, `progress` (1=start, 2=cache off + estado leído, 3=f_mkfs hecho),
    `d_cap`, `d_state`, `d_cid`, `pconfirm`, `mkfs_work[4096]` (align 32).
  - `ThreadX2()`: **Opción A** — `SCB_DisableDCache(); SCB_CleanInvalidateDCache();`
    ANTES de `f_mkfs("0:", 0, mkfs_work, sizeof(mkfs_work))`.
    Antes de mkfs captura `SDCD1.capacity`, `SDCD1.state`, `SDCD1.cid[0]`
    (diagnóstico de detección de tarjeta).
  - Thread: patrón probado del objeto `script` (compiló y corrió en builds previos):
    `static msg_t ThreadX(void *arg)` + cast a `instanceformat__1`,
    `WORKING_AREA(waThreadX, 8192)` (8 KB, ampliado de 1024 por posible overflow),
    `chThdCreateStatic(... NORMALPRIO ...)`.
- **`code.krate`**: crea el thread si `confirm1 && confirm2 && !pconfirm`;
  mapea displays: `b0`=FRESULT, `b1`=progress, `b2`=capacity, `b3`=state, `b4`=cid[0].
- **`code.dispose`**: `chThdTerminate` + `chThdWait` si `pThd` (corrige
  "patch stopped but did not terminate its thread(s)").
- Ya NO llama a `report_fatfs_error` (ese path paraba el patch y generaba el
  error de thread no terminada; el FRESULT ahora se ve directo en `b0`).

### 2. Cambios previos de esta sesión (ya documentados)
- `util/format` compilable: quitar `static` de `mkfs_work` (semántica de
  class-scope del codegen). Commits previos en el repo.
- Cadena de I/O verificada pass-through: FatFs → `fatfs_diskio.c`
  (pasa el buffer de FatFs DIRECTO a `sdcRead`/`sdcWrite`, sin memcpy)
  → `SDCD1` → DMA SDMMC.

## Hipótesis D-cache (por confirmar con la Opción A)

- AKSO H7 tiene D-cache; el F4 original del diseño no.
- `hal_sdc_lld.c` (ChibiOS) usa DMA del SDMMC contra buffers en RAM cacheable
  sin `SCB_InvalidateDCache`/`SCB_CleanDCache` alrededor → incoherencia.
- Explica AMBOS bugs: (a) card reader "stuck at LBA 0" (mounter usa
  `blkbuf` 512B align 32 como buffer de MSD), (b) `f_mkfs` → `FR_DISK_ERR`.
- Evidencia de que NO es hardware: el file manager (mismo driver SD + FatFs)
  funcionaba antes de las pruebas de escritura.

## Plan

- **Opción A (AHORA)**: build de `format.axo` con `SCB_DisableDCache()` antes
  de `f_mkfs`. Test: confirm1 ON + confirm2 → leer displays.
  - Si `succeeded=1` → hipótesis CONFIRMADA.
  - Si sigue fallando → mirar `b0` (FRESULT), `b1` (¿llegó a 3?),
    `b2/b3/b4` (¿tarjeta detectada?).
- **Opción B (fallback/solución de fondo, sin dar vueltas)**:
  cache-maintenance por operación en `hal_sdc_lld.c` (`blkRead`/`blkWrite`):
  `SCB_InvalidateDCache()` tras DMA de read; `SCB_CleanDCache()` antes de
  DMA de write; y/o marcar `blkbuf` del mounter como no-cacheable
  (`.ram_nc` / `SCB_SetMemAttr`). Rebuild firmware + mounter, re-test
  LBA reads/writes en card reader mode.

## Notas de build/flash
- `format.axo` se compila con el build normal del firmware (codegen a
  `Library/Akso/build/xpatch.cpp`). Verificar que el build regenere
  `xpatch.cpp` (el que está en disco es de un build anterior).
- Si el board se desconecta de USB al correr f_mkfs → es el mismo síntoma
  que con D-cache ON; con stack 8 KB ya descartado overflow, apuntaría a
  hang en la I/O SD (ver `b1` al re-conectar).

---

# HANDOFF FINAL (cierre de sesión)

## Hallazgo clave de esta sesión: D-cache ON al arrancar

`external/ChibiOS/os/common/startup/ARMCMx/compilers/GCC/crt1.c:141`
(`__core_init`): **`SCB_EnableDCache();`** — el fork de ChibiOS de AKSO
activa la D-cache del Cortex-M7 **al arrancar**. El driver SD (SDMMC/IDMA)
hace DMA **sin** mantenimiento de cache. El Axoloti original (STM32F4, M4,
sin cache) funciona; el AKSO (STM32H7, M7, D-cache) falla.

→ **Confirmado como root cause candidato #1** de ambos bugs:
- (a) card reader "stuck at LBA 0" (mounter),
- (b) `f_mkfs` fallando/crasheando (format object).

**Corrección a la doc anterior:** el LLD local NO es "867 vs 928 líneas"
contra upstream master: es **idéntico a ChibiOS `ver20.3.1`** excepto
whitespace + una trampa `while(1);` debug (trap al fallar → hang de
thread, no reset). El fork local es rama "3.0.1" (HAL 7.1.2).

## Estado de `format.axo` (dejarlo así)

`~/Library/Akso/akso-factory/objects/util/format.axo` = **original
`f7a3ef7` byte a byte** (git commit `1a7ac38` "restore original for
handoff"). Backups trackeados en el mismo repo:
`format.axo.orig-f7a3ef7`, `format.axo.orig-20260927`.

**Los fixes de esta sesión quedaron en la git history** (repo
`~/Library/Akso/akso-factory`, repo git):
- `fcd807d` — f_mkfs 4-arg (API R0.14+),
- `f071a53` — buffer de trabajo,
- `b76e5aa` — `mkfs_work` miembro de clase plano (no static data member).

**OJO:** el original **NO compila** contra el FatFs actual
(R0.14: `f_mkfs(const char*, const MKFS_PARM*, void*, uint32_t)` — 4
args; el original llama a `f_mkfs(0,0,0)`). El próximo build necesita
aplicar esos commits (o equivalente mínimo):
1. `static byte mkfs_work[4096];` como **miembro de clase** (el codegen
   genera class scope; `static` a nivel de clase rompe),
2. `f_mkfs("0:", 0, mkfs_work, sizeof(mkfs_work))`,
3. stack `WORKING_AREA(waThreadX, 8192)` (1024 era corto para f_mkfs),
4. (test D-cache) `SCB_DisableDCache()` antes del I/O y
   `SCB_EnableDCache()` al terminar el thread.

## Lo que se probó y crashó

Build con (1)+(2)+(3)+(4): **compiló** pero al presionar confirm1+2 el
board se **desconecta de USB** (reset: exception →
`NVIC_SystemReset`). El krate solo crea el thread al presionar los
botones → el crash está **dentro del thread**. Puntos sospechosos, en
orden: `SCB_DisableDCache()`/walk de cache, `f_mkfs` → I/O SD → trampa
`while(1)` del LLD (la card está corrupta: todos LBAs → bloque 0),
o el remount. La trampa del LLD da **hang** (no reset), así el reset
apunta a HardFault (ptr nula, desborde, o acceso a memoria mal
alineada por la cache).

**Prueba bisect recomendada al reanudar:**
- **Build B (sin D-cache, sin f_mkfs):** thread solo hace
  `SCB_DisableDCache()` + leer `SDCD1.cid[]`/`SDCD1.capacity` a
  displays + terminar. Si crasha → el problema es
  `SCB_DisableDCache`/thread en sí (no f_mkfs).
- **Build C (D-cache ON, f_mkfs):** si también crasha → el crash no
  depende de la cache (posible trampa LLD por card corrupta).
- **Build A (el de arriba, D-cache OFF + f_mkfs):** el definitivo para
  la hipótesis.

**Si la D-cache se confirma** → fix de fondo (Opción B de la doc
anterior): cache maintenance por operación en `hal_sdc_lld.c`
(clean antes de write, invalidate después de read) para el firmware
principal Y el mounter. Alternativa: buffer SDC en zona non-cacheable.

## Pendiente: la card

La microSD sigue **corrupta** (LBA 0 repetido, sin MBR usable). El
format via editor es la vía elegida (macOS/diskutil falló: writes
stuck en LBA 0 — mismo bug). Si el board se queda "connecting…" /
desconectado tras un crash: resetear el board (DFU o power cycle) y/o
cargar un patch mínimo.

## API ChibiOS verificada (fork 3.0.1)

- `chThdTerminate(thread_t*)`, `chThdWait(thread_t*)` — un solo puntero.
- Displays válidos: `bool32` (LED), `int32.label` (número), etc.
  (`AxoObject.java`). El codegen reescribe `attr_parent` →
  `instance<id>` (p.ej. `instanceformat__1`).
- `report_fatfs_error(int, const char*)` (`exceptions.c:328`) solo llena
  el exceptiondump (BKPSRAM, sobrevive al reset), no para el patch.
- `sysmon_disable_blinker()`/`sysmon_enable_blinker()` +
  `palSetPad`/`palClearPad` — disponibles si se quiere diagnóstico por
  LED física (patrón por etapa; sobrevive al reset visualmente).

## Paths

| Qué | Dónde |
|---|---|
| format object | `~/Library/Akso/akso-factory/objects/util/format.axo` (git, commits arriba) |
| crt1 con D-cache ON | `~/Documents/dev/akso/external/ChibiOS/os/common/startup/ARMCMx/compilers/GCC/crt1.c:141` |
| LLD SD (con trampa while(1)) | `~/Documents/dev/akso/external/ChibiOS/os/hal/ports/STM32/STM32H7/sdc_lld.c` (verificar nombre exacto; rama 3.0.1) |
| mounter | `~/Documents/dev/akso/axoloti/firmware/mounter/` |
| exception dump / reset | `~/Documents/dev/akso/axoloti/firmware/exceptions.c` |
| FatFs R0.14 | `~/Documents/dev/akso/external/ChibiOS/ext/fatfs/source/` |
| build generado | `~/Library/Akso/build/xpatch.cpp` |

---

# RESOLUCIÓN FINAL (28/09/2026) — commit `e7788fd`

**Root cause confirmado: D-cache + DMA incoherente (la hipótesis ya anotada
en esta sesión).**

- `crt1.c:__core_init()` enciende la D-cache del M7. El IDMA del SDMMC no
  snoopea la cache.
- El mounter reusa **un solo `blkbuf` de 512 B** para todos los sectores
  (`lib_scsi.c:data_read_write10`), y está en `.ram0a` (AXI SRAM cacheable en
  el mounter). Por eso "todo LBA devolvía bloque 0" y las escrituras no
  llegaban: la CPU servía/escribía la copia cacheada stale.

**Fix aplicado (2 archivos):**
- `external/ChibiOS-Contrib/os/various/lib_scsi.c`: en `data_read_write10`,
  `cacheBufferInvalidate(buf, bs)` antes del `blkRead` y
  `cacheBufferFlush(buf, bs)` antes del `blkWrite`.
- `external/ChibiOS/os/hal/ports/STM32/LLD/SDMMCv2/hal_sdc_lld.c`: se removió
  el `while(1);` de debug de `sdc_lld_write_aligned`.

**Descartado/desestimado:** el LLD local es correcto; `lib_scsi.c` y
`hal_usb_msd.c` pasan el LBA bien; la card es buena. El bug estaba puramente en
coherencia de cache.

**Validación en hardware:** MBR `55AA`, LBAs distintos, `fsck_msdos -n` exit 0,
y test de escritura raw con fd abierto (4 patrones en 4 LBAs, sin
contaminación entre sectores, y restauración verificada).

**Build/deploy:** el mounter (`axoloti/firmware/mounter`, toolchain x86 bajo
Rosetta) se rebuildea con `make`; copiar `mounter.{sram1,sram3,sdram}.bin` a
`<bundle>/Contents/Resources/firmware/mounter/mounter_build/`. Hash corregido
`d7d66975…`.

**Ojo:** en paralelo apareció otro bug pre-existente — el firmware principal
monta la SD de forma **intermitente** al arrancar (`SDCSW`=GPIOD13 lee 1 en
muchos arranques → `sysmon` la desmonta; cuando pasa, arrancar un patch la
remonta). Intentar dejarla montada
vía `sysmon` (seed de `sdcsw_prev`) **rompe el arranque de patches**; y
habilitar BKPRAM con el `while(BRRDY)` cuelga el boot. Documentado en
`AGENTS.md`; recuperable por DFU. No es parte de mass storage.
