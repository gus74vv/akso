# DFU write tooling — incidente y estado (2026-10-03)

> Sesión: prueba de `dfu-util` como reemplazo arm64 del `STM32CubeProgrammer` (pendiente #7
> de `ARM64-BUILD.md`), antes de que macOS 28 corte Rosetta.
> **Resultado: dfu-util queda DESCALIFICADO para el write path.** El rescue sigue siendo el
> ST CLI (hoy v2.5.0 x86 bajo Rosetta), y la migración arm64 va por la versión arm64 de ST.
> La placa quedó recuperada y verificada byte a byte.

## 1. Setup de la prueba

| Item | Valor |
|---|---|
| Host | macOS 27.0 (build 26A428), Apple Silicon |
| Tool | `dfu-util 0.11` arm64 nativo (Homebrew), linkeado a `libusb 1.0.30` |
| Device | ROM bootloader ST `0483:df11`, `alt=0 @Internal Flash /0x08000000/16*128Kg` (2 MB), `alt=1 @Option Bytes` |
| Target | `axoloti.bin` del bundle, **1210976 B**, sha256 `9b038e1efcfa8a7c599880d4d5dd927960c343ecee9629f37eab45b0976e7ec0`, crc32 `05394BAC` |
| Script | `~/Library/Akso/dfu-test/dfu-safe-test.sh` (fases 0/1 read-only, fase 2 con `--write`) |

La placa se puso en DFU con **switch 1 apretado al encender** (bootloader ROM puro, sin firmware
de por medio).

## 2. Lo que SÍ funciona en dfu-util 0.11 arm64 (macOS 27)

- `dfu-util -l` enumera los dos alt settings. **No** aparece el bug histórico de macOS
  `Cannot set alternate interface` (`Setting Alternate Interface #0 … DFU state(2) = dfuIDLE`).
- `dfu-util -d 0483:df11 -a 0 -s 0x08000000:0x200000 -U <file>` → **readback de los 2 MB OK**
  (transfer size 1024). El ROM bootloader de este H7 **sí** permite upload.
- El flash de la placa era **byte-idéntico** al `.bin` del bundle (sha256 igual), y
  0x120000–0x200000 estaba todo `0xFF` (limpio). Dump completo guardado:
  `~/Library/Akso/dfu-test/board_flash.bin` (estado bueno pre-incidente).

## 3. El incidente (write)

Comando ejecutado (con la placa en DFU y el `.bin` idéntico al contenido previo):

```sh
dfu-util -d 0483:df11 -a 0 -s 0x08000000:leave -D axoloti.bin
```

Síntoma: **colgado**, sin terminar (lo corté a los 240 s). No hubo output útil porque pipeé la
salida en vivo (error de instrumentación: no repetir).

Estado tras el corte:

```
DFU state(4) = dfuDNBUSY, status(0) = No error condition is present
dfu-util: dfuse_download: libusb_control_transfer returned -9 (LIBUSB_ERROR_PIPE)
dfu-util: Error during special command "SET_ADDRESS" download
```

Forense del flash (readback tras power-cycle, que limpia el `DNBUSY`):

| Medición | Resultado |
|---|---|
| bytes que coinciden con el `.bin` | 39 712 / 1 210 976 |
| primer offset distinto | `0x000000` |
| último offset distinto | `0x11FFFF` (fin del sector 8) |
| 0x120000–0x127B40 | intacto (coincide con el `.bin`) |
| no-`0xFF` en la región del app | 31 019 (restos de erase parcial) |
| boot normal | no arranca (app borrada) |

Interpretación: dfu-util **mandó el `ERASE`** sobre los sectores 0–9 (destruyó 0–8 y dejó el 9
sin tocar) y **se murió en la transición erase → `SET_ADDRESS`/`DNLOAD`**: no escribió **ni un
byte de datos** (por eso el primer diff es el offset 0, no un prefijo válido). Es el patrón del
ticket `dfu-util #168` (`Error during special command "SET_ADDRESS" download` / `LIBUSB_ERROR_PIPE`
en ROM DFU de STM32).

Nota: el `:leave` **no** fue la causa (nunca se llegó a la etapa de descarga).

## 4. Recuperación (camino probado, sin tocar el app)

```sh
CLI=$HOME/Documents/dev/akso/external/STM32CubeProgrammer/mac/STM32CubeProgrammer.app/Contents/MacOs/bin/STM32_Programmer_CLI
"$CLI" -c port=USB1 -d $HOME/Documents/dev/akso/package/mac/Akso.app/Contents/Resources/firmware/build/axoloti.bin 0x08000000
```

Resultado: `Erasing internal memory sectors [0 9]` → `File download complete`,
`Time elapsed during download operation: 00:00:21.795`, `exit=0`.
Después: readback + `cmp` de los primeros 1210976 B contra el `.bin` → **idénticos**
(sha256 `9b038e1e…`).

Dos datos a tener en cuenta:
- El banner del CLI dice **STM32CubeProgrammer v2.5.0** (el `AppInfo.plist` del bundle declara 2.4.0).
- El CLI **no saca la placa de DFU**: hay que **power-cycle sin switch 1** para que arranque el firmware.

## 5. Conclusiones

1. **dfu-util no va para el write path** en este H7/ROM DFU (libusb 1.0.30 + macOS 27), al menos con
   los parámetros por defecto. Sirve para leer/enumerar (útil como herramienta de diagnóstico).
2. La migración arm64 va por **STM32CubeProgrammer 2.20+** (primera versión con Apple silicon nativo;
   2.23 documenta `x86_64` y `ARM-aarch64`, misma ruta `STM32CubeProgrammer.app/Contents/MacOs/bin/STM32_Programmer_CLI`
   y misma CLI `-c port=USB1 -d <file> <addr>`).
3. El SCP x86 **sigue funcionando** en macOS 27 bajo Rosetta → el deadline real es **macOS 28**.
4. Disciplina de flash (obligatoria de acá en adelante):
   - **dump completo** del flash antes de cualquier experimento (`-U`, 0x200000);
   - **readback + `cmp`** contra el `.bin` a flashear antes de escribir;
   - watchdog + salida a archivo (**nunca** pipear dfu-util en vivo);
   - tener el ST CLI intacto como fallback, y probar escrituras nuevas **fuera de la región del app**.

## 6. Plan para la próxima sesión (pruebas de escritura seguras)

Región desechable: **sector 10, `0x08140000`** (el app termina en `0x08127B40`; 0x08140000–0x08200000
está todo `0xFF`). Si el write se cuelga, el app no se toca y un power-cycle normal recupera.

1. Patrón de 4 KB → `dfu-util -d 0483:df11 -a 0 -s 0x08140000 -D pattern.bin` (**sin `:leave`**),
   luego readback `0x08140000:0x1000` + `cmp`.
   - Si se cuelga igual → confirmado que el problema es el write path de dfu-util/DfuSe, no la región.
   - Si funciona → aislar la variable (¿`:leave`? ¿offset? ¿tamaño?) antes de pensar en usarlo.
2. Mismo patrón con el **ST CLI** (`-d pattern.bin 0x08140000`) → si anda, la diferencia es la
   implementación de dfu-util, no el host.
3. Bajar **STM32CubeProgrammer 2.23 ARM-aarch64** y probar el rescue real con esa versión
   (es el objetivo de la migración): ojo con quarantena/Gatekeeper (`xattr -dr com.apple.quarantine`)
   y con el paso de instalación (ahora viene un `Setup…app`).
4. Recién con eso validado: `platform_osx/upload_fw_dfu.sh` (+ `Makefile:40` y ambos bundles),
   dejando el CLI x86 como fallback hasta macOS 28.
5. Aparte (no es recovery): toolchain de firmware `arm-none-eabi` x86 → migrar a ARM GNU Toolchain
   darwin-arm64 (GCC 12.3+/13+) o compilar firmware en Linux/CI. Es porteo, no drop-in.

## 7. Artefactos

| Archivo | Qué es |
|---|---|
| `~/Library/Akso/dfu-test/board_flash.bin` | dump 2 MB del estado **bueno** pre-incidente |
| `~/Library/Akso/dfu-test/after2.bin` | dump del estado roto (erase parcial) |
| `~/Library/Akso/dfu-test/recovered.bin` | dump post-rescue (== `.bin`) |
| `~/Library/Akso/dfu-test/stcli.log` | log del ST CLI exitoso |
| `~/Library/Akso/dfu-test/dfu-safe-test.sh` | script de prueba (read-only por defecto; `--write` para el app) |