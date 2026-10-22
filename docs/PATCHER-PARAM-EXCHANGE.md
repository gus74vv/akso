# Patcher ↔ board parameter exchange — open issues

Debugging session (2026-10-03). Starting point: the patch
`Nanokontrol2FMdrumseq.axp` (8-step, 3 voices) went out of sync as soon as the
**master clock knob (nanoKONTROL2 knob 8 = CC 23)** was moved, and the desync
survived a firmware rollback.

**Status:** the patch itself is fixed (data, see §1). **The patcher-side
fragility (§2) and the ordering fragility (§3) are NOT fixed** — this document
is the note that keeps them tracked.

---

## 0. Summary of what was ruled out

| Hypothesis | Verdict |
|---|---|
| Introduced by `ea2cb20` ("USB host MIDI out: drain 16 events, ring 128") | **Ruled out.** Reproduced with `05394BAC` (drain 4, ring 32) and with `7072DEBF` (drain 16, ring 128). Board flashed back to `7072DEBF`. |
| Board MIDI input path / MIDI CC → parameter on the MCU | **Ruled out.** With the board standalone (patch uploaded as `/start.bin`, patcher disconnected, USB to the Mac unplugged) the patch **stays in sync** while moving CC 23. |
| SD card holding the old (good) compiled patch | No: the SD does not have the old version (it was overwritten). |
| MIDI CC reach / generated CC handler | Verified fine: all three `lfo/square` instances get the same value in the same k-cycle (`instancesquare__N_i.MidiHandler` is dispatched for all of them; `(data2!=127)?(data2-64)<<21:0x07FFFFFF`). |

So: the three clock rates diverge **only while the patcher is connected** —
i.e. the editor is writing parameter values back to the board.

---

## 1. What the patch file had (fixed)

- The three `lfo/square` "Master Clock (knob 8)" params were saved at
  **-6.0 / -4.0 / -2.0** — three different rates for the three voices. Each LFO
  starts at phase 0 with its own rate, so the voices can never line up; moving
  CC 23 equalises them in rate but keeps the accumulated phase → permanent
  desync.
- The **saved toggle state was asymmetric**: in voice 3's band three toggles
  were stored at 1 (`toggle_47` = play, CC 41; `toggle_41` and `toggle_49`,
  CC 45) while voices 1-2 had everything at 0. `logic/flipflop` is
  edge-triggered and starts with `p_set=0`, so a toggle stored at 1 counts as a
  rising edge at boot → voice 3 started playing alone, and its step counter
  started on a different step (the per-step multipliers 1..8 then make each
  voice sound at a "different clock").

Both were reset (three clocks at -4.0, all toggles 0, patch boots stopped and
symmetric). Backups of the patch: `Nanokontrol2FMdrumseq.axp.bak-*`,
`.bak2-*`, `.bak3-*` next to the `.axp`.

**Root cause of the diverge-while-connected:** the editor's parameter exchange
(§2). Values arriving from the board can be dropped and the editor's own values
pushed back; when several parameters are driven by the same control (here: CC 23
→ 3 params), they can end up with different values, and a save then persists
them into the `.axp`.

---

## 2. Patcher ↔ board parameter exchange — the bug to fix

Protocol: the board reports `(patchID, value, index)` (`tx_pckt_paramchange`,
`axoloti/firmware/pconnection.c:458`), one packet per changed parameter, with
`index` = position in `patchMeta.params`. The editor sends `AxoP` with
`(patchID, value, uint16 index)`.

Editor side, three interacting pieces:

1. `axoloti/src/main/java/axoloti/connection/USBBulkConnection_v2.java`
   `RPacketParamChange()` (~lines 1060-1095):
   ```java
   if (!pi.getNeedsTransmit()) {
       pi.getDModel().setValue(pi.getDModel().int32ToVal(value));
   }
   ```
   **If the editor has a pending local change for that parameter, the board's
   value is silently dropped** — and the editor's own (stale) value will be
   sent back.

2. `axoloti/src/main/java/axoloti/live/patch/parameter/ParameterInstanceLiveView.java`
   (~line 58):
   ```java
   if (ParameterInstance.VALUE.is(evt)) {
       needsTransmit = true;
   }
   ```
   **Any** value change marks the parameter "needs transmit" — *including the
   change caused by applying the board's own report* → the editor echoes the
   value back to the board.

3. `axoloti/src/main/java/axoloti/live/patch/PatchViewLive.java`
   `pollParameters()` (~lines 170-190): walks the parameters and sends **the
   first one** with `needsTransmit`, then `break`s (one parameter per poll
   cycle); `TXData()` clears the flag *before* transmitting.

Firmware side, for reference: a parameter change coming *from* the editor is
applied with signals mask `0xFFFFFFEE`
(`pconnection.c:~817`, `ParameterChange(&params[index], value, 0xFFFFFFEE)`),
i.e. **without** the USB-report bit — so the current flow does not loop
infinitely, but (1)+(2) is exactly a "pending change wins over the board"
policy, one parameter at a time.

### Proposed fix (not implemented)

- (a) Apply the board's value always, or track the *origin* of a value change
  (a flag set only for user/UI edits, not for values applied from the board) so
  a board report never leaves the editor in "dirty" state.
- (b) Send all pending parameters in one pass (drop the `break` in
  `pollParameters()`), or batch them, so the editor cannot lag behind while a
  knob streams values.
- (c) Optional: coalesce per parameter (keep only the newest value).
- Validation: LED-heavy patch (27 `midi/out/cc`) and a fast knob turn (CC flood)
  with the patcher connected; check the app console for
  `Rx paramchange index out of range` (would indicate an index mismatch, which
  is a separate hypothesis and was never observed in this session's logs).

Maps to `midi/out/cc`-style patches directly: the same mechanism explains why
parameter values "come back" with the wrong value while a controller streams.

### Building and deploying the fix (jar)

Everything is Java on the editor side; the running app loads
`<bundle>/Contents/Resources/Axoloti.jar` from **both** bundles
(`/Applications/Akso.app` and `package/mac/Akso.app`):

```sh
cd axoloti
JAVA_HOME=../external/jdks/mac_arm64/<jdk> ant        # -> dist/Axoloti.jar
```

Deploy (per bundle): back up the current jar, copy the new one, and **re-inject
the arm64 usb4java dylib** — `ant` regenerates the jar without it:

```sh
cp Axoloti.jar Axoloti.jar.bak-<tag>
cp <repo>/axoloti/dist/Axoloti.jar Axoloti.jar
zip Axoloti.jar org/usb4java/osx-aarch64/libusb4java.dylib   # from any .bak jar
```

Then restart the app. Full context: `docs/ARM64-BUILD.md` and the "Tras el build
del jar" note in `AGENTS.md`.

### Reproducing (to verify the fix)

1. Patch: `~/axoloti patches/axoloti patches/Nanokontrol2FMdrumseq.axp` — the
   three `lfo/square` "Master Clock (knob 8)" params all carry `MidiCC="23"`;
   nanoKONTROL2 plugged into the board's USB host port (adapter on the
   controller's cable, see `MIDI-USB-HOST-FINDINGS.md` §9).
2. Go Live, move knob 8 (CC 23). In the editor the three dials end up with
   **different** values (observed 5.15 / 1.62 / 1.45 Hz) and the sequencer's
   three voices run at different clocks.
3. Control (already done once): upload the same patch as `/start.bin`, unplug
   the Mac's USB, boot the board standalone, move CC 23 → stays in sync. This
   isolates the editor as the writer.
4. Instrumentation: a temporary `Logger` in `RPacketParamChange` printing
   `index`, `value` and the parameter name, plus one in `pollParameters()` when
   a `TXData()` is transmitted. The fight shows up as values bouncing between
   the editor's stored value and the board's, one parameter at a time.

### Success criteria

- With the editor connected, a CC mapped to several parameters leaves **all of
  them equal** (editor and board) and the patch stays in sync.
- A value reported by the board is never silently ignored; saving (live or not)
  cannot persist a diverged set.
- No echo storm: with the LED-heavy patch (27 `midi/out/cc`) and a fast knob
  turn, the link stays alive and the app console shows no repeated traffic
  (the board applies editor-sent values with mask `0xFFFFFFEE`, i.e. without
  reporting them back — that assumption must hold after the change).

### Untested hypothesis — index mismatch

`RPacketParamChange` resolves the parameter **by index** in
`patch.getParameterInstances()`, and the board sends the index inside the
*compiled* patch. Both are built from the same object order (`sortByPosition()`
when `writeCode()` runs), but the editor's live view is created **once** at
go-live while `save()` re-sorts the model by position as well — so touching the
layout *after* going live can leave the editor's live indices stale until the
next go-live. Symptoms would be parameters changing that were never touched, or
`Rx paramchange index out of range` in the app console. Not observed in this
session's logs, but worth instrumenting while fixing the exchange.

---

## 3. Related fragility found — execution order = canvas position

This explains "the stored compiled patch works, a fresh compile does not" with
the *same* `.axp` content:

- `axoloti/src/main/java/axoloti/patch/PatchModel.java:86`
  `static final boolean USE_EXECUTION_ORDER = false;`
- `PatchController.writeCode()` (~line 160): with that flag false it calls
  **`sortByPosition()`**; `sortByExecution()`'s body is commented out (no-op).
- `PatchModel.save()` (~line 303) also calls `sortByPosition()` — **the `.axp`
  is always written in canvas order**.
- `AxoObjectInstanceAbstract.compareTo()` sorts by `(y, x)`.
- `axoloti/src/main/java/axoloti/patch/net/Net.java:172` `needsLatch()` returns
  true when the destination object's index is `<=` the source's index — i.e.
  **the destination executes before the source** — and the codegen then reads
  that net through a `...Latch` copy: **a 1 k-rate-cycle (~333 µs) delay**.

Measured in this patch's generated `xpatch.cpp`: **124 of 244 nets are
latched**. Consequences:

- Moving an object on the canvas can flip latches → the patch's *timing*
  changes without a single wire changing.
- A patch compiled from the same file after a layout/save-order change can
  sound different from a previously stored binary. In an 8-step sequencer with
  per-step clock multipliers, one flipped latch moves *when* the step counter
  advances and *where* the trig lands.

Possible fix: implement `sortByExecution()` for real (topological order, ties
by position) and set `USE_EXECUTION_ORDER = true`, so a latch appears only
where the dataflow actually has a cycle. This must be validated by ear with the
sequencer patch, because it flips many latches at once.

---

## 4. Tried and reverted — single master dial wired to the LFO pitch inlets

Workaround attempted: remove `MidiCC="23"` from the three `lfo/square` params,
add one `ctrl/dial b` (name `dial_9`, `MidiCC="23"`, value 0.0, placed first in
canvas order so its net is not latched) and wire `dial_9.out` to the three
`lfo/square` `pitch` **inlets** (`MFC: param_pitch + inlet_pitch`). Rationale:
one parameter in the exchange instead of three, unison guaranteed by the graph.

**Result: misbehaved → reverted** (patch restored to the previous state, dial
removed). Hypotheses not yet checked:

- the dial's `frac32.s.map` range is ±64 (≈ ±5 octaves), so CC 23 0..127 covers
  an extreme sweep around the base value;
- adding the object changed the execution order/latch pattern (§3), which in
  this patch is enough to change timing;
- the three `pitch` inlets were previously unconnected and uninitialised paths
  may be involved.

---

## 5. Current state and workaround

- **Firmware:** board and both bundles on `7072DEBF` (drain 16 / ring 128).
  Backups: `~/Library/Akso/backup-fw-7072DEBF-20261003-173143` and
  `~/Library/Akso/backup-fw-05394BAC-20261003-152533`.
- **Patch:** `Nanokontrol2FMdrumseq.axp` is back to the state that boots in sync
  (three clock params at -4.0, all toggles 0) — i.e. **still three parameters
  driven by CC 23**, so it stays exposed to the bug in §2. Backups next to it:
  `.bak-20261003-175115` (original, clocks -6/-4/-2, voice 3 toggles at 1),
  `.bak2-20261003-181052`, `.bak3-20261003-182944` (= current state, taken
  before the dial experiment). No copy of the `dial_9` version is kept.
- **Workaround until §2 is fixed:** move the master clock with the **editor
  disconnected** (standalone patch). Do not save the patch right after moving
  CC 23 with the editor connected — saving persists the diverged values into the
  `.axp` (that is exactly how -6.0 / -4.0 / -2.0 got there).

## 6. Suggested order for the next session

1. Fix §2 — small, self-contained Java change: never let a pending local value
   suppress the board's value (or track the origin of the change), and drain all
   pending parameters per poll instead of one. Build/deploy per §2 and validate
   against the success criteria.
2. Then decide about §3 (execution order / latches): implementing
   `sortByExecution()` for real flips many latches at once and needs a by-ear
   check with the sequencer patch.
3. If the single-master-dial wiring is retried (§4): fix the range first (the
   dial spans ±64 semitones against the CC's 0..127) and keep the new object's
   canvas position such that the latch pattern of the existing nets does not
   change.
