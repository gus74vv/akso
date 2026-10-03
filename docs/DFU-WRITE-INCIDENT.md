# DFU write tooling — incident report and current state (2026-10-03)

> Session goal: evaluate `dfu-util` as an arm64 replacement for
> `STM32CubeProgrammer` in the rescue path, before macOS 28 drops Rosetta.
> **Result: `dfu-util` is DISQUALIFIED for the write path.** Rescue stays on the
> ST CLI (currently v2.5.0 x86 under Rosetta) and the arm64 migration goes
> through ST's own arm64 build. The board was fully recovered and verified
> byte-for-byte.
>
> Status: closed — the incident is understood, the board is recovered, and the
> only pending work is the arm64 migration of the ST CLI (see the plan at the
> end).

## 1. Test setup

| Item | Value |
|---|---|
| Host | macOS 27.0 (build 26A428), Apple Silicon |
| Tool | `dfu-util 0.11`, native arm64 (Homebrew), linked against `libusb 1.0.30` |
| Device | ST ROM bootloader `0483:df11`, `alt=0 @Internal Flash /0x08000000/16*128Kg` (2 MB), `alt=1 @Option Bytes` |
| Target | the bundle's `axoloti.bin`, **1210976 B**, sha256 `9b038e1efcfa8a7c599880d4d5dd927960c343ecee9629f37eab45b0976e7ec0`, crc32 `05394BAC` |
| Script | `~/Library/Akso/dfu-test/dfu-safe-test.sh` (phases 0/1 read-only, phase 2 uses `--write`) |

The board was put in DFU by holding **switch 1** while powering it on (pure ROM
bootloader, no firmware involved).

## 2. What DOES work with dfu-util 0.11 arm64 (macOS 27)

- `dfu-util -l` enumerates both alt settings. The historic macOS bug
  (`Cannot set alternate interface`) does **not** appear
  (`Setting Alternate Interface #0 … DFU state(2) = dfuIDLE`).
- `dfu-util -d 0483:df11 -a 0 -s 0x08000000:0x200000 -U <file>` → **2 MB
  readback OK** (transfer size 1024). This H7's ROM bootloader **does** allow
  upload.
- The board's flash was **byte-identical** to the bundle `.bin` (same sha256),
  and 0x120000–0x200000 was all `0xFF` (clean). Full dump saved as
  `~/Library/Akso/dfu-test/board_flash.bin` (the good pre-incident state).

## 3. The incident (write)

Command executed (board in DFU, `.bin` identical to the previous content):

```sh
dfu-util -d 0483:df11 -a 0 -s 0x08000000:leave -D axoloti.bin
```

Symptom: it **hung** and never finished (killed after 240 s). There was no
useful output because the output was piped live (instrumentation mistake — do
not repeat).

State after the kill:

```
DFU state(4) = dfuDNBUSY, status(0) = No error condition is present
dfu-util: dfuse_download: libusb_control_transfer returned -9 (LIBUSB_ERROR_PIPE)
dfu-util: Error during special command "SET_ADDRESS" download
```

Flash forensics (readback after a power cycle, which clears `DNBUSY`):

| Measurement | Result |
|---|---|
| bytes matching the `.bin` | 39 712 / 1 210 976 |
| first differing offset | `0x000000` |
| last differing offset | `0x11FFFF` (end of sector 8) |
| 0x120000–0x127B40 | intact (matches the `.bin`) |
| non-`0xFF` in the app region | 31 019 (partial erase leftovers) |
| normal boot | does not start (app erased) |

Interpretation: `dfu-util` **sent the `ERASE`** for sectors 0–9 (destroying 0–8
and leaving 9 untouched) and **died in the erase → `SET_ADDRESS`/`DNLOAD`
transition**: it never wrote **a single data byte** (hence the first diff is
offset 0, not a valid prefix). This matches ticket `dfu-util #168`
(`Error during special command "SET_ADDRESS" download` / `LIBUSB_ERROR_PIPE` on
STM32 ROM DFU).

Note: `:leave` was **not** the cause (the download stage was never reached).

## 4. Recovery (proven path, without touching the app)

```sh
CLI=$HOME/akso/external/STM32CubeProgrammer/mac/STM32CubeProgrammer.app/Contents/MacOs/bin/STM32_Programmer_CLI
"$CLI" -c port=USB1 -d $HOME/akso/package/mac/Akso.app/Contents/Resources/firmware/build/axoloti.bin 0x08000000
```

Result: `Erasing internal memory sectors [0 9]` → `File download complete`,
`Time elapsed during download operation: 00:00:21.795`, `exit=0`.
Then readback + `cmp` of the first 1210976 B against the `.bin` → **identical**
(sha256 `9b038e1e…`).

Two things to remember:

- The CLI banner says **STM32CubeProgrammer v2.5.0** (the bundle's
  `AppInfo.plist` declares 2.4.0).
- The CLI does **not** take the board out of DFU: a **power cycle without
  switch 1** is needed for the firmware to boot.

## 5. Conclusions

1. **`dfu-util` is not viable for the write path** on this H7/ROM DFU
   (libusb 1.0.30 + macOS 27), at least with default parameters. It is fine for
   reading/enumerating (useful as a diagnostic tool).
2. The arm64 migration must go through **STM32CubeProgrammer 2.20+** (first
   version with native Apple silicon; 2.23 documents `x86_64` and `ARM-aarch64`,
   same path `STM32CubeProgrammer.app/Contents/MacOs/bin/STM32_Programmer_CLI`
   and same CLI `-c port=USB1 -d <file> <addr>`).
3. The x86 SCP **still works** on macOS 27 under Rosetta → the real deadline is
   **macOS 28**.
4. Flash discipline (mandatory from now on):
   - **full flash dump** before any experiment (`-U`, 0x200000);
   - **readback + `cmp`** against the `.bin` to be written, before writing;
   - watchdog + output to a file (**never** pipe `dfu-util` live);
   - keep the ST CLI intact as a fallback, and try new writes **outside the app
     region**.

## 6. Plan for the next session (safe write tests)

Disposable region: **sector 10, `0x08140000`** (the app ends at `0x08127B40`;
0x08140000–0x08200000 is all `0xFF`). If the write hangs, the app is untouched
and a normal power cycle recovers.

1. A 4 KB pattern → `dfu-util -d 0483:df11 -a 0 -s 0x08140000 -D pattern.bin`
   (**without `:leave`**), then readback `0x08140000:0x1000` + `cmp`.
   - If it hangs the same way → the problem is dfu-util/DfuSe's write path, not
     the region.
   - If it works → isolate the variable (`:leave`? offset? size?) before
     considering it.
2. Same pattern with the **ST CLI** (`-d pattern.bin 0x08140000`) → if that
   works, the difference is dfu-util's implementation, not the host.
3. Download **STM32CubeProgrammer 2.23 ARM-aarch64** and test the real rescue
   with it (that is the migration goal): watch out for quarantine/Gatekeeper
   (`xattr -dr com.apple.quarantine`) and the new installer step
   (`Setup….app`).
4. Only then: update `platform_osx/upload_fw_dfu.sh` (+ `Makefile:40` and both
   bundles), leaving the x86 CLI as a fallback until macOS 28.
5. Separately (not recovery): firmware toolchain, `arm-none-eabi` x86 → migrate
   to ARM GNU Toolchain darwin-arm64 (GCC 12.3+/13+) or build firmware on
   Linux/CI. That is a port, not a drop-in.

## 7. Artifacts

| File | What it is |
|---|---|
| `~/Library/Akso/dfu-test/board_flash.bin` | 2 MB dump of the **good** pre-incident state |
| `~/Library/Akso/dfu-test/after2.bin` | dump of the broken state (partial erase) |
| `~/Library/Akso/dfu-test/recovered.bin` | post-rescue dump (== the `.bin`) |
| `~/Library/Akso/dfu-test/stcli.log` | successful ST CLI log |
| `~/Library/Akso/dfu-test/dfu-safe-test.sh` | test script (read-only by default; `--write` for the app) |
