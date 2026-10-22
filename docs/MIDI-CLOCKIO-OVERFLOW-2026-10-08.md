# `phi/midi/clock io` → flood de `midi output overflow` (2026-10-08)

**Estado: NO arreglado.** Es comportamiento **pre-existente del firmware**,
expuesto por un patch. Decisión del 08/10/2026: **no tocar firmware por ahora**;
workaround = no usar ese objeto. Si aparecen más patches con el mismo problema,
reconsiderar los "próximos pasos" de abajo.

---

## Síntoma (observado)

- Dar Live a un patch con `phi/midi/clock io` → la consola del patcher floodea
  `midi output overflow` (18+ líneas, sostenido).
- **Reproducido con un patch mínimo que contiene SÓLO ese objeto**, y con el
  **controlador MIDI desconectado** (puerto host vacío).
- Después de que ocurre, **cualquier** patch con objetos MIDI sigue mostrando el
  mensaje con cada mensaje MIDI enviado o recibido, hasta **power cycle** del
  board. Tras el power cycle, esos mismos patches se comportan normal.
- No depende de `sss/patch/autoSource` ni de nuestra edición de ese `.axo`.

## Verificado leyendo el código (evidencia)

1. **Origen del mensaje**: `exceptions.c:295` (`LogTextMessage("midi output overflow")`),
   para el tipo `usbh_midi_ringbuffer_overflow`, levantado por
   `report_usbh_midi_ringbuffer_overflow()`. Se llama desde **dos** lugares:
   `midi_buffer.c:43` (`midi_input_buffer_put`, ring de **entrada**) y
   `midi_buffer.c:83` (`midi_output_buffer_put`, ring de **salida**).
   El report es de un disparo por evento (guard `magicnumber`,
   `exceptions.c:398-403`); `exception_checkandreport()` limpia el flag y loguea,
   y se llama **en cada ACK del app** (`pconnection.c:500`).
   ⇒ Un *flood* de líneas = el ring **queda lleno y sigue recibiendo puts**.

2. **Productor**: el subpatch `phi/midi/clock io`
   (`~/Library/Akso/akso-community/objects/phi/midi/clock io.axs`) contiene
   `midi/out/clock` con `Run=1`, `midi In/Out=1` y param `clock=19.0`.
   Su `code.krate` emite `MIDI_TIMING_CLOCK` cada vez que `_posfrac` desborda el
   bit 31. **Caudal**: el XML `value="19.0"` se convierte con
   `ValueFrac32.getFrac() = v * 2^21` → fracción `19/1024` →
   `3000 Hz × 19/1024 ≈ **56 msgs/s**` (~139 BPM). Es un caudal trivial.

3. **Destino real** (sorpresa): `MidiSend1(dev, port, b0)` (`midi.c:120-126`)
   **ignora `dev`**; `Midi_calcPH(port, b0)` (`midi.c:112`) arma el nibble PH como
   **`(port - 1)`**. El objeto pasa `port = 1` (viene del `CEntries`
   `"MIDI_DEVICE_DIN, 1"`) → **vport 0** → y los mapas default rutean vport 0 a
   **DIN** (`serial_midi.c:63-69`, `bmvports[0]=0b1`) y **USB device**
   (`midi_usb.c:43-47`, `bmvports[0]=0b1`).
   ⇒ El clock se escribe en `midi_output_din` **y** `midi_output_usbd`.
   La selección `device="din"` del `.axs` es efectivamente **cosmética** en este
   fork: quien decide es el vport + el mapa de routing.
   (Bug latente relacionado: como `dev` se ignora, el combo `device` no hace lo
   que dice.)

4. **AKSO no tiene DIN**: el consumidor de `midi_output_din` es `ThreadMidiOut`
   (`serial_midi.c:238-253`) → `serial_MidiSend` → `sdWrite(&SDMIDI, …)`;
   `#define SDMIDI SD6` (`axoloti_board.h:48`), 31250 baud (`serial_midi.c:222`),
   TX en GPIOG14 open-drain **sin pull-up externo** (no hay receptor en AKSO).
   `serial_midi_init()` corre sin condicionales (`midi.c:154`), crea los dos
   hilos y hace `objinit` del ring.

5. **No hay recuperación**: `midi_output_buffer_reset()` y
   `midi_output_buffer_deinit()` **no se llaman en ningún lado** del firmware →
   una vez lleno (y con el consumidor trabado) el ring queda lleno para siempre;
   cada put posterior reporta. Sólo el power cycle reinicializa los índices
   (`.bss`).

6. **El warning de GCC es benigno**: `_posfrac &= (1<<31)-1` (`-Woverflow`,
   arm64 GCC 15.3). `1<<31` es UB técnico pero GCC emite el patrón esperado
   (`0x80000000`); la máscara da `0x7FFFFFFF`. No es la causa.

## Descartado

- **NO es la edición del `.axo` `sss/patch/autoSource`** (aplicada y luego
  revertida el 08/10): un patch mínimo con **sólo** el clock lo reproduce. La
  edición sólo hizo que `ProbquencerSeed.axp` / `21.axp` / `shuphil1.axp`
  compilaran (expuso el comportamiento de runtime del patch); no toca ningún
  camino de MIDI.
- **NO es el puerto host / el controlador**: se reproduce **sin dispositivo** en
  el puerto host. (Sin dispositivo, `midi_outputmap_usbh1/2` tienen `nports=0` →
  `MidiSendVirtual` ni siquiera escribe en los rings del host.)
- **NO es ancho de banda**: ~56 msgs/s contra un ring de 128 y un UART de
  31250 baud (~3125 B/s) → 3 órdenes de magnitud de margen. Lo que está roto es
  el **drenaje**, no el caudal.

## NO determinado (requiere evidencia en runtime)

Causas candidatas del drenaje trabado de `midi_output_din`:

- **(a)** `ThreadMidiOut` bloqueado para siempre dentro de `sdWrite` (la cola de
  salida del driver nunca se drena).
- **(b)** El IRQ de TX de UART6 no drena en AKSO (bloquearía la cola igual, y
  encaja con "hace falta un stream sostenido": unos pocos mensajes entran en la
  cola del driver, un stream de 56 msgs/s termina bloqueando).
- **(c)** `ThreadMidiOut` muerto por stack overflow (256 B, `serial_midi.c:238`)
  en el camino bloqueante.
- **(d)** El ring de **entrada**: el mismo texto de reporte lo usa
  `midi_input_buffer_put` (`midi_buffer.c:43`) — p. ej. una línea RX flotante
  (`ThreadMidiIn` hace `sdGet(&SDMIDI)`, `serial_midi.c:225-234`) llenando
  `midi_input_buffer`.

## Próximos pasos (si se retoma)

1. **Build de diagnóstico** (patrón de `docs/USBH-DIAG.md`): por segundo,
   `puts/drops/gets` de `midi_output_din` + estado y stack libre de los hilos
   `midi_din_in`/`midi_din_out`. Distingue (a)-(d).
2. **Stub del path DIN en AKSO**: no crear el consumidor de DIN (o
   drenar-y-descartar) cuando `BOARD_AKSO` — fix legítimo para un board sin DIN.
   Re-testear con el patch del clock.
3. **Workaround patch/librería**: no usar `phi/midi/clock io` (o silenciar su
   clock con `midi In/Out = 0`).

## Workaround (hoy)

No usar el objeto. Cualquier patch que lo contenga wedged el ring de salida MIDI
hasta el próximo power cycle.

## Receta de reproducción

1. Power cycle del board.
2. Patch mínimo: un solo `phi/midi/clock io` con `midi In/Out=1`, `Run=1`.
3. Live → a los ~2-3 s (128 slots a ~56 msgs/s) aparece el flood.
4. Parar el patch y cargar otro con objetos MIDI → el mensaje sigue apareciendo
   por cada mensaje. Power cycle → se limpia.
