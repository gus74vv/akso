# Diagnóstico — microSD 8 GB en SoC "AKSO"

> Fecha: 28/09/2026 — Mac (macOS), dispositivo USB `disk4`

## Síntoma inicial

- MicroSD de 8 GB conectada a un SoC llamado **AKSO** (firmware ChibiOS/RT), expuesta por USB.
- La tarjeta aparecía "sin formato" y **Disk Utility no podía formatearla ni repararla**.

## Lo que se observó

| Hallazgo | Detalle |
|---|---|
| Dispositivo | `disk4`, 7.7 GB (7,744,782,336 bytes), USB, removable, no read-only |
| Nombre USB | `"ChibiOS/RT Mass Storage Device"` (el SoC actúa de puente USB → SD) |
| Estructura inicial | MBR (FDisk) con 1 partición FAT32, offset 1 MiB |
| `fsck_msdos -n` | **Exit code 201** — FAT no válida / no encontrada |
| `diskutil verifyVolume` | Error `-69845` (File system verify or repair failed) |
| `diskutil eraseDisk FAT32 SD MBR` | Completaba el formato pero el volumen quedaba en **0 B** y **no montaba** |
| Velocidad de escritura | ~**120 KB/s** (normal: 10–20 MB/s) |

## Tests concluyentes (escritura/lectura por LBA)

| Test | Resultado |
|---|---|
| Escribir 64 KB de `0xAB` en LBA 100000 | Se reflejó en **todos los LBAs leídos** (0, 100000, 200000) |
| Escribir 512 bytes en LBA 200000 y 500000 | **Se perdieron** — no quedaron en ningún sector |
| Escribir 64 KB de `0x55` en LBA 300000 | Vuelve a aparecer en **todos los LBAs** (0, 100000, 300000) |
| MBR después de formatear | Sin firma `55AA`, tabla de particiones vacía o basura |

**Comportamiento del dispositivo:** cualquier lectura devuelve el *último bloque de 64 KB escrito*, y las escrituras pequeñas (< 64 KB, p. ej. el MBR o las entradas FAT) no llegan a la tarjeta. El LBA solicitado **se ignora**.

## Conclusión

> **El problema no es la microSD, es el puente USB del SoC.**
>
> La implementación de mass storage de la firmware **ChibiOS/RT** del AKSO está rota:
> no mapea correctamente los sectores (todas las lecturas/devoluciones apuntan al mismo buffer de 64 KB) y descarta escrituras pequeñas. Por eso el formateo "completa" en la Mac pero la FAT final queda corrupta e inmontable, y Disk Utility no puede repararlo.

## Recomendaciones

1. **Formatear la microSD sin pasar por el SoC:** sacarla de la placa y usar un lector USB de microSD directo a la Mac. Ahí el formateo (MBR + FAT32) toma segundos y la tarjeta queda limpia.
2. Volver a insertar la tarjeta en el SoC y flashearla con la imagen/firmware correspondiente.
3. Si se necesita que el propio SoC exponga la SD por USB (p. ej. para flashear sin PC): revisar/actualizar la firmware ChibiOS/RT del board — el bug está ahí, no en la tarjeta.

## Comandos usados (referencia)

```bash
diskutil list
diskutil info /dev/disk4 /dev/disk4s1
diskutil verifyVolume /dev/disk4s1          # fsck_msdos -n → exit 201
sudo diskutil eraseDisk FAT32 SD MBR /dev/disk4
sudo dd if=/dev/zero of=/dev/disk4 bs=1M count=4 oflag=sync   # ~120 KB/s
# Test de aliasing por LBA (python3, escritura de patrones 0xAB / 0x55 / 0x77)
```

---

## RESOLUCIÓN (28/09/2026) — commit `e7788fd`

**Confirmado: el problema era el puente USB del SoC, y era la D-cache.**

- La D-cache del Cortex-M7 está activa desde `crt1.c:__core_init()`, y el IDMA
  del SDMMC no snoopea la cache. El mounter usa un único `blkbuf` de 512 B
  (`.ram0a`, AXI SRAM cacheable) → la CPU leía/escribía la copia cacheada stale:
  todas las lecturas devolvían el bloque 0 y las escrituras mandaban datos
  viejos. Exactamente el síntoma observado.
- **Fix:** mantenimiento de cache en `lib_scsi.c` (`data_read_write10`):
  `cacheBufferInvalidate` antes de `blkRead`, `cacheBufferFlush` antes de
  `blkWrite`. Más la remoción del `while(1)` de debug del LLD SDMMC.
- **Validado en hardware** (card reader): MBR `55AA`, LBAs distintos,
  `fsck_msdos -n` exit 0, y test de escritura con fd abierto (4 patrones / 4
  LBAs, sin contaminación, restauración OK).

La microSD fue formateada fuera del SoC (MBR + FAT32, offset 1 MiB) y sus
archivos se restauraron por lector USB en la Mac (`/shared/808/*.raw` desde
`~/Library/Akso/akso-factory/objects/wave/`).

**Pendiente relacionado (no parte de este fix):** el firmware principal sigue
desmontando la SD al arrancar (`SDCSW`/GPIOD13 lee 1 en Akso). Se monta recién
al arrancar el primer patch. Ver `AGENTS.md` §"Pendiente: SD no monta al
arrancar".
