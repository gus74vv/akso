#!/bin/bash
#
# fw-deploy.sh — copia el SET COMPLETO de firmware al bundle de la app
# (dev: package/mac/Akso.app  +  instalado: /Applications/Akso.app).
#
#   ./fw-deploy.sh            # firmware de DIAGNÓSTICO USB host (build-diag)
#  ./fw-deploy.sh fix        # fix del host (sin instrumentación, build-fix)
#   ./fw-deploy.sh current    # firmware actual/bueno (build/, 7072DEBF)
#   ./fw-deploy.sh restore    # igual que current, desde el backup en el repo
#
# El set completo (bin+elf+hex+map+dmp+list) es obligatorio: el patch se
# linkea con --just-symbols=<bundle>/.../firmware/build/axoloti.elf, así que
# el .elf del bundle tiene que coincidir con el .bin flasheado.
#
# Guarda el set que reemplaza en <bundle>/.../firmware/build/backup-<tag>/
# (sólo si ese backup no existe todavía). Después de correrlo: cerrar la app,
# abrirla de nuevo y Board → Flash.
#
# Detalle: docs/USBH-DIAG.md
set -euo pipefail

REPO="$(cd "$(dirname "$0")" && pwd)"
BUNDLES=("$REPO/package/mac/Akso.app" "/Applications/Akso.app")
FILES=(axoloti.bin axoloti.elf axoloti.hex axoloti.map axoloti.dmp axoloti.list)

case "${1:-diag}" in
    diag)    SRC="$REPO/axoloti/firmware/build-diag";              TAG="diag-usbh" ;;
    fix)     SRC="$REPO/axoloti/firmware/build-fix";               TAG="usbh-nakfix" ;;
    current) SRC="$REPO/axoloti/firmware/build";                   TAG="7072DEBF" ;;
    restore) SRC="$REPO/axoloti/firmware/build-backup-7072DEBF";   TAG="7072DEBF" ;;
    *) echo "uso: $0 [diag|fix|current|restore]" >&2; exit 1 ;;
esac

if [ ! -f "$SRC/axoloti.bin" ]; then
    echo "ERROR: falta $SRC/axoloti.bin" >&2
    exit 1
fi

echo "== origen: $SRC"
md5 "$SRC/axoloti.bin"

for B in "${BUNDLES[@]}"; do
    D="$B/Contents/Resources/firmware/build"
    if [ ! -d "$D" ]; then
        echo "== salteo $B (no existe $D)"
        continue
    fi
    BK="$D/backup-$TAG"
    if [ -d "$BK" ]; then
        echo "== backup ya existente (no lo toco): $BK"
    else
        mkdir -p "$BK"
        for f in "${FILES[@]}"; do
            if [ -f "$D/$f" ]; then cp "$D/$f" "$BK/"; fi
        done
        echo "== backup del set previo en: $BK"
    fi
    for f in "${FILES[@]}"; do
        if [ -f "$SRC/$f" ]; then cp "$SRC/$f" "$D/"; else echo "AVISO: no existe $SRC/$f"; fi
    done
    echo "== deploy listo en $D"
    md5 "$D/axoloti.bin" "$D/axoloti.elf"
done

echo
echo "Ahora: cerrá la app, abrila de nuevo y hacé Board → Flash."
echo "Para volver atrás: ./fw-deploy.sh current  (y Board → Flash de nuevo)"
