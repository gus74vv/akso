# Compilador de patches: GCC 15.3 arm64 nativo (30/09/2026)

> Cierra el último binario x86 del flujo de trabajo. La app ya era arm64 nativa
> (launcher Packr + JRE + dylib usb4java); el que faltaba era el compilador de
> patches (`arm-none-eabi-g++`), que corría x86_64 bajo Rosetta.

## Qué cambió

| | Antes | Ahora |
|---|---|---|
| Compilador de patches | GCC 8-2019 (x86_64, **Rosetta**) | **GCC 15.3 (arm64 nativo)** |
| Ubicación | `external/gcc-arm/mac/gcc-arm-none-eabi-8-2019-q3-update/` | `external/gcc-arm/mac-arm64/` |
| Tamaño | 441 MB | **280 MB** (podado) |

**No se toca el firmware de la placa ni el `axoloti.elf` contra el que se linkea
el patch (`--just-symbols`) → no hace falta reflashear.** El firmware sigue
siendo el mismo (**CRC 05394BAC**, el que reporta el app como "Local firmware
CRC").

## Por qué: la compilación del patch ocurre en la Mac, no en el AKSO

El patch (`xpatch.cpp`, generado por el editor) se cross-compila en el host y se
sube a la placa ya como código ARM. El STM32H7 solo carga y ejecuta. Prueba
natural: un patch grabado como *startup* en el board arranca en milisegundos;
el mismo patch por *Live* tardaba ~30 s, porque ese tiempo es el compile.

### Medición (M1 Max, mismo `xpatch.cpp`, mismos flags)

| Toolchain | Ejecución | Compilar `xpatch.cpp` | Pipeline completo (`make`) |
|---|---|---|---|
| GCC 8.3.1 (antes) | x86_64 / Rosetta | 39,9–41,0 s | ~42 s |
| GCC 14.2.1 | arm64 nativo | 4,07–4,14 s | 4,51 s |
| **GCC 15.3.1** | **arm64 nativo** | **3,64–3,70 s** | **4,15 s** |

Rosetta aportaba ~1,2× (la mejora grande vino del salto de versión de GCC).
Extra: el código generado sale ~15–20 % más chico.

### Desglose real de un "Live" (GCC 15.3, patch de 12.304 líneas)

| Tramo | Tiempo | Quién |
|---|---|---|
| Compile | ~2,0 s | Mac |
| Upload sram1 (392 KB, USB FS) | ~0,5 s | USB |
| Upload sram3 | ~0,01 s | USB |
| Arranque DSP + readback | ~0,2 s | Placa |
| **TOTAL** | **~2,7 s** | |

## Cómo se selecciona el toolchain

`Makefile.patch` (repo + ambos bundles, idénticos):

```make
# Compilador de patches: arm64 nativo (GCC 15.3) por defecto.
# Fallback al GCC 8 x86 (Rosetta) si su directorio existe (rollback).
GCC_ARM64 ?= ${external}/gcc-arm/${platform}-arm64/bin
GCC_LEGACY ?= ${external}/gcc-arm/${platform}/gcc-arm-none-eabi-8-2019-q3-update/bin
gcc_bin ?= $(if $(wildcard $(GCC_ARM64)/arm-none-eabi-g++),$(GCC_ARM64),$(GCC_LEGACY))
```

- Si existe `mac-arm64/` → se usa (caso normal).
- Si no → cae al GCC 8 x86.
- La variable de entorno `gcc_bin` sigue teniendo prioridad (`?=`).

## ROLLBACK

Todo el rollback está en `~/Library/Akso/rollback-gcc8-20260930/`:

```
gcc-arm-none-eabi-8-2019-q3-update-mac-x86.tar.zst   # toolchain x86 podado (37 MB)
Makefile.patch.backup/                               # los 3 Makefile.patch originales
xpatch.h.gch + xpatch.sram*.bin + SHA256SUMS.txt     # estado compilado previo
```

### Opción A — volver a GCC 8 (x86/Rosetta)

```sh
G=~/Library/Akso/rollback-gcc8-20260930
B=/Applications/Akso.app/Contents/Resources/external/gcc-arm   # o el dev bundle
mkdir -p "$B/mac/gcc-arm-none-eabi-8-2019-q3-update"
tar --zstd -xf "$G/gcc-arm-none-eabi-8-2019-q3-update-mac-x86.tar.zst" \
    --strip-components=1 -C "$B/mac/gcc-arm-none-eabi-8-2019-q3-update"
mv "$B/mac-arm64" "$B/mac-arm64.off"     # así el Makefile cae al x86
rm -f ~/Library/Akso/build/xpatch.h.gch  # el PCH es por-versión de GCC
```

### Opción B — forzar x86 sin mover directorios

Lanzar la app con la variable de entorno (el `?=` le da prioridad):

```sh
gcc_bin="$B/mac/gcc-arm-none-eabi-8-2019-q3-update/bin" /Applications/Akso.app/Contents/MacOS/Akso
```

### Opción C — volver al `Makefile.patch` original

Copiar desde `$G/Makefile.patch.backup/` a los 3 lugares (repo + dev bundle +
bundle instalado; hash original `3dbb8d892c908c623c25750e4de2fb143f2e07e5e704045057dffc9982c2ea08`).

### Qué NO se puede romper de forma irremediable

- El firmware de la placa no cambió → no hay brick; el bootloader ROM + DFU de
  rescate siempre está.
- Los patches son solo archivos: si uno compilado con GCC 15 suena mal, se
  revierte el toolchain y se recompila.
- El repo está en git → `git revert` / `git checkout` para la parte trackeada.

## Notas

- El toolchain arm64 está **podado**: conserva solo el multilib
  `thumb/v7e-m+fp/hard` (cortex-m7 hard-float) + headers + binutils. **No sirve
  para compilar *firmware*** ni otras arquitecturas. El firmware sigue
  compilándose con el toolchain x86 (Rosetta) documentado en `ARM64-BUILD.md`.
- Se eliminaron de los bundles: `gcc-arm/mac` (x86), y del dev bundle además
  `gcc-arm/linux`, `gcc-arm/win` y `external/jdks` (build-only). ~2,4 GB menos.
- En el repo se eliminaron `external/gcc-arm/{linux,win,mac}` (el `mac` era el
  x86) y `external/jdks/{linux_x64,mac_x64,win_x64}` (~1,5 GB menos). El
  toolchain arm64 vive en `external/gcc-arm/mac-arm64/` **sin trackear**
  (`.gitignore`), igual que el JDK extraído: 280 MB de binarios no van a git.
  Ojo al reconstruir el bundle con packr: copia `external/` tal cual, así que
  `mac-arm64/` tiene que estar presente en el working tree.
- El warning nuevo del linker (`LOAD segment with RWX permissions`) es
  inofensivo: el patch se carga a mano en RAM, no como ELF ejecutable.
