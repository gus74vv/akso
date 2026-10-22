# FIX — `script/script` fails to compile on Akso (`invalid use of member ... in static member function`)

- **Date:** 2026-10-21
- **Status:** **FIXED** (applied to the local object library; verified by compiling the patch)
- **Not a board / firmware issue.** It is a codegen regression introduced when the Axoloti
  factory library was ported to Akso.
- **Scope of change:** data under `~/Library/Akso/akso-factory/objects/` (the app library),
  **not** the repo. Same situation as the `autoSource` fix of 2026-10-08.

## Symptom

`akso-factory/patches/demos/sequencing/scripted_midi_drama.axp` fails to compile:

```
xpatch.cpp:188: error: invalid use of member 'rootc::instancescript1::out1'
                 in static member function
```

The patch's `script1` writes `out1 = (i+1)<<21; out2 = (i)<<21;` **from inside its thread**.

## Root cause

The factory object `objects/script/script.axo` declared the inner thread body as **static**:

```cpp
int32_t out1, out2;
static THD_FUNCTION(ThreadX2, arg) {   // <-- the bug: this is static
       attr_script                        // the user script is injected HERE
}
static THD_FUNCTION(ThreadX, arg) {
       ((attr_parent *)arg)->ThreadX2(arg);
}
```

`THD_FUNCTION(t, a)` expands to `PORT_THD_FUNCTION(t, a)`, and for ARMCMx/GCC that is
`void t(void *a)` — **no `static`**. The `static` in the generated code comes from the
literal text written in the `.axo`. A static member function cannot access non-static
members; `out1`/`out2` are members of `instancescript1` → the exact error.

Only `ThreadX` needs to be `static`: it is the C function pointer handed to
`chThdCreateStatic`. `ThreadX2` is **never** used as a C function pointer (it is called
through the instance), so it can — and for `script`, must — be non-static.

## Why it is NOT the board (evidence it regressed in the port)

Fetched the upstream objects and diffed them:

- Original **Axoloti** `axoloti-factory` (HEAD `78cb74b`, 2018) `objects/script/script.axo`:

  ```cpp
  msg_t ThreadX2(){                    // NON-static in original Axoloti
      attr_script
  }
  static msg_t ThreadX(void *arg) { ((attr_parent *)arg)->ThreadX2(); }
  WORKING_AREA(waThreadX, 1024); Thread *Thd;
  ```

- **Akso** `akso-factory` ships `static THD_FUNCTION(ThreadX2, arg)` +
  `THD_WORKING_AREA` + `thread_t`.

So the object **did compile on original Axoloti**: the thread body was non-static there.
During the port someone mechanically modernized the ChibiOS thread API
(`WORKING_AREA` / `Thread *` / `msg_t` → `THD_FUNCTION` / `THD_WORKING_AREA` / `thread_t`)
and added `static` to **both** `ThreadX` and `ThreadX2`. `ThreadX` static is correct;
`ThreadX2` static is the bug.

This is a **pure C++ front-end (semantic) error**, target-independent: GCC rejects it
identically for `arm-none-eabi` (Axoloti) and `arm64` (Akso), native or under Rosetta.
The codegen has no Axoloti/Akso branch here: `attr_parent` → the instance class name via
`getCInstanceName()`, `<code.declaration>` is inserted verbatim, and `TargetModel.java`
carries no thread/`static` logic. Verified.

## Fix applied

Removed `static` from `ThreadX2` in both script objects (`ThreadX` stays `static`):

- `akso-factory/objects/script/script.axo`
- `akso-factory/objects/script/script2.axo`

`script2` had the same `static` but was **latent**: it wraps the user code in **non-static**
`setup()`/`loop()` methods, so it compiled by accident. Now aligned with upstream.

Backups (rollback = `cp` the `.bak-*` back; the library loader filters by `.axo`, so a
`.bak-*` file is ignored):

- `akso-factory/objects/script/script.axo.bak-preTHDfix-20261021`
- `akso-factory/objects/script/script2.axo.bak-preTHDfix-20261021`

## Verification

After relaunching the app and refreshing the library, `scripted_midi_drama.axp` compiles
("Compiling patch." → success). Because the fix reproduces the upstream semantics
(non-static `ThreadX2`), this compile **is** the proof that the original object worked.

## Library scope — how much of `akso-factory` was ported

Full `diff -rq` of `axoloti-factory` (2018) vs `akso-factory/objects/`:

- **499 / 519 objects identical** (byte-for-byte), **20 differ**, **0 removed**.
- All **20** differing objects carry a real content change (none is a bare `sha` re-seal),
  falling in 3 buckets:
  1. **Hardware: STM32F4 (Axoloti) → STM32H7 (Akso)** — `gpio/i2c/config` (I2C `TIMINGR`
     per RM0433 instead of `OPMODE_I2C`/`400000`/`FAST_DUTY_CYCLE_2`), `gpio/spi/config`,
     `gpio/in|out/analog` (ADC channel remap), `audio/in|outconfig` (dropped the
     Stereo / Mono / Balanced codec modes).
  2. **Patcher API renames** — `ctrl/hook` (`PExParameterChange` → `ParameterChange`),
     `harmony/note scale`.
  3. **ChibiOS RTOS thread API modernized** — the 5 objects with threads:
     `script/script`, `script/script2`, `spectral/rfft 128`, `wave/play fn`,
     `wave/play fn stereo`.
- So the port was **surgical**: ~96% of the library is a verbatim copy (portable C audio
  /DSP), and the conversion touched only what genuinely differs between the two
  boards / RTOS / patcher API. `script` fell into bucket **3** (RTOS), not bucket **1**
  (hardware).

### Which thread objects can break

Only objects that inject **untrusted user code** inside a `static` `ThreadX2` break —
that is `script` (and the latent `script2`, now fixed). `spectral/rfft 128` keeps
`ThreadX2` static but is fine because every member access is written via
`((attr_parent *)arg)->member` (author-written code, not user-injected), so it is
correctly `static`-safe. `wave/play fn*` were not audited in detail but do not inject
user code into the thread.

## Related

- `docs/FIX-AUTOSOURCE-MODSOURCES-2026-10-08.md` (precedent: library-object change → fix
  parked on the object, applied out-of-repo in `~/Library/Akso`).
- `ARM64-BUILD.md`, `docs/MIDI-USB-HOST-FINDINGS.md` (the real arm64 / target work —
  unrelated to this bug).
