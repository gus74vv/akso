# Fix: objeto `sss/patch/autoSource` — moduladores declarados vs. código C (08/10/2026)

> ## ⚠️ REVERTIDO el mismo día (08/10/2026)
>
> **Decisión: no tocar la librería de la comunidad por un patch.**
> El `.axo` volvió al original (con `<ModulationSources>`), así que
> `ProbquencerSeed.axp`, `21.axp` y `shuphil1.axp` **vuelven a fallar la
> compilación** con el error `'class rootc' has no member named
> 'MODULATOR_instanceautoSource__1'`.
>
> - Original restaurado: `akso-community/objects/sss/patch/autoSource.axo`
>   (+ copia en `~/Library/Akso/autoSource.axo.bak-modsourcesfix-20261010`)
> - Versión **con fix** guardada: `akso-community/objects/sss/patch/autoSource.axo.bak-nomodsourcesfix-20261008`
>   (el loader filtra con `endsWith(".axo")`, `AxoObjects.java:257` → un `.bak-…`
>   en ese dir **no** se carga)
> - **Re-aplicar** = `cp` de ese `.bak-…` sobre `autoSource.axo` (y reiniciar el app).
>
> El fix **funcionaba** (los 3 patches compilaban y el `reverb_1.amount` se
> modulaba de verdad). Se revirtió por decisión de alcance, no por un problema
> del fix: el `midi output overflow` que apareció en esa misma sesión **no tiene
> relación** (ver `MIDI-CLOCKIO-OVERFLOW-2026-10-08.md`: un patch mínimo con sólo
> `phi/midi/clock io` lo reproduce).

## Síntoma

Dar **Live** al patch `~/Library/Akso/akso-community/patches/phi/ProbquencerSeed.axp`
(también afecta a `21.axp` y `shuphil1.axp`, mismos objetos) falla con:

```
error: 'class rootc' has no member named 'MODULATOR_instanceautoSource__1';
did you mean 'MODULATOR_instanceautoSource__1_source1'?
  &parent->GetModulationTable()[parent->MODULATOR_instanceautoSource__1*NMODULATIONTARGETS],
```

## Causa raíz: inconsistencia interna del objeto `.axo`

El objeto `~/Library/Akso/akso-community/objects/sss/patch/autoSource.axo`
(autor Remco van der Most, librería sss) tenía:

1. **Definición**: `<ModulationSources>` con 3 fuentes nombradas
   (`source1`, `source2`, `source3`) → el codegen declara los índices con la
   convención nueva (`Modulator.getCName()`, `Modulator.java:55`):
   `MODULATOR_<instancia>_source1` / `_source2` / `_source3`.
2. **Código C** (`code.krate`): usa el placeholder **viejo**
   `parent->MODULATOR_attr_name`, que el codegen reemplaza por el nombre de
   instancia **sin sufijo** (`AxoObjectInstanceCodegenView.java:190`:
   `s.replace("attr_name", getCInstanceName())`) → referencia
   `MODULATOR_instanceautoSource__1`, que no existe → error de compilación.

El objeto factory `patch/modsource` (upstream) usa el mismo placeholder viejo
pero **no** declara `<ModulationSources>` → `AxoObject.getModulators()` devuelve
`[null]` (un modulador sin nombre) → CName `MODULATOR_<instancia>` sin sufijo →
coincide con su código. Consistente. El de la comunidad quedó entre dos
convenciones.

## Efecto colateral: modulación huérfana con `modName=""`

Los 3 patches guardan sus modulaciones como
`<modulation sourceName="autoSource_N" modName="" value="…"/>` (convención
vieja: fuente sin nombre). Con los moduladores renombrados a
`source1/2/3`, `Modulation.getModulator()` (`Modulation.java:88`) no encontraba
match → la modulación quedaba huérfana y **no entraba en la tabla de
modulación** (fila `{-1,0}`): aunque se arreglara el nombre del miembro C, la
modulación no habría hecho nada.

## Fix (1 archivo, lado librería — sin reflashear board ni rebuild del app)

`~/Library/Akso/akso-community/objects/sss/patch/autoSource.axo` → quitar el
bloque `<ModulationSources>` (queda solo `<providesModulationSource>true</providesModulationSource>`).
`source2`/`source3` eran declaraciones muertas: el objeto tiene 1 inlet y una
sola implementación de fuente.

Con la fix:
- 1 modulador sin nombre → CName `MODULATOR_<instancia>` → coincide con el
  código del objeto.
- La modulación con `modName=""` matchea el modulador sin nombre
  (`Modulation.java:96`: `m.getName()==null && modName.isEmpty()`) → se adjunta
  y la modificación funciona.

**Backup:** `~/Library/Akso/autoSource.axo.bak-modsourcesfix-20261010`.

**Importante:** el app cachea las definiciones de objetos al arrancar →
**reiniciar el app** (las dos apps) para que el `.axo` editado se tome.

## Validación

- XML bien formado (`xmllint --noout` exit 0).
- Cadena de codegen verificada por lectura (ver arriba).
- Pendiente: Live en el patcher (reiniciar app primero).

## Nota

El warning `(1<<31)-1` → `2147483647` (objeto `clock` del patch) es benigno:
`1<<31` en `int` con signo es UB técnico y GCC 15 avisa, pero el resultado es
la máscara esperada `0x7FFFFFFF`. No es la causa del fallo.
