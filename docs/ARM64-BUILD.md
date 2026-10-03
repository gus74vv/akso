# Akso (zrna) — Native Apple Silicon (arm64) port

> Session of 2026-09-27. Full conversion of `Akso.app` from x86_64 (Rosetta) to
> **native arm64**, including the USB connection fix and the board firmware.
>
> **Status: COMPLETE and fully functional.** The launcher, the bundled JRE, the
> usb4java native library and (since 2026-09-30) the patch compiler all run
> natively. See `PATCH-COMPILER-ARM64.md` for the compiler and the top-level
> `README.md` for the current state.

---

## 1. Background and initial diagnosis

### What the original app is

`/Applications/Akso.app` (build 2020-09-07) is a **Packr 2.7.0** bundle:

| Component | Original architecture | Notes |
|---|---|---|
| `Contents/MacOS/Akso` (C++ "PackrLauncher") | x86_64 | Reads `config.json`, creates the JVM |
| `Contents/Resources/jre/` (OpenJDK/Corretto 11.0.8) | x86_64 | Embedded, minimized with `jlink` |
| `Contents/Resources/Axoloti.jar` (Java app, main `axoloti.Axoloti`) | architecture independent | Pure Java |
| `Contents/Resources/external/gcc-arm/mac` (`arm-none-eabi-gcc` 8-2019) | x86_64 | Toolchain for the STM32 firmware |
| `Contents/Resources/external/jdks` (Corretto 11.0.8 x64) | x86_64 | Build-only, not kept in the final bundle |

Everything ran under **Rosetta 2** on Apple Silicon.

### The `xcrun` error

When compiling a patch, the (x86_64) app spawns a shell that invokes `xcrun`.
Under Rosetta the universal `/usr/bin/xcrun` runs as x86_64 and needs to load
`/Library/Developer/CommandLineTools/usr/lib/libxcrun.dylib`, which on this
system **only had arm64/arm64e slices**:

```
xcrun: error: unable to load libxcrun (dlopen(...): fat file, but missing
compatible architecture (have 'arm64,arm64e', need 'x86_64'))
```

A hybrid Command Line Tools state (modern macOS 26/27 SDKs, but an arm64-only
`libxcrun`). The real fix is a native arm64 app: its child processes run arm64
and use the slice that exists.

### Official build pipeline found in the repo

Repo: <https://zrna.org/akso> (version 3.0.1). The `Makefile` and
`packr-mac-x64.json` reveal the official build:

```
make firmware   → cd axoloti/firmware && ./compile_firmware_linux.sh
make gui        → cd axoloti && ant              (produces axoloti/dist/Axoloti.jar)
package-mac     → java -jar packr-all-2.7.0.json packr-mac-x64.json
                → cleanup: rm external/jdks, chmod jspawnhelper, ...
```

- The embedded JRE **ships in the repo**:
  `external/jdks/mac_x64/OpenJDK11U-jdk_x64_mac_hotspot_11.0.8_10.tar.gz`
- `minimizejre: "hard"` → Packr runs `jlink` on that JDK.
- **Key finding:** the `packr-mac` launcher of Packr 2.7.0 is x86_64-only, and
  so are 3.0.3 / 4.0.0 (2021 jars). A native launcher requires **compiling the
  PackrLauncher C++ for arm64** (open source, ~700 lines).

---

## 2. Conversion to arm64 (steps performed)

### 2.1 Tools

```sh
brew install ant            # ant 1.10.18
```

### 2.2 JDK 11 arm64

Downloaded **Azul Zulu 11.0.32.1 aarch64** (JDK.app layout, same shape as the
Corretto in the repo) and kept it in the repo so the build is self-contained:

```
external/jdks/mac_arm64/zulu11-macosx_aarch64.tar.gz
external/jdks/mac_arm64/zulu11.90.205-ca-jdk11.0.32.1-macosx_aarch64/Contents/Home
```

Verified: `bin/java` → `Mach-O 64-bit executable arm64`.

This JDK is used for three things: building `Axoloti.jar` (ant), building the
bundle (Packr runs `jlink` with it → arm64 JRE) and as the local build JDK.

### 2.3 Firmware

Built with the embedded x86_64 toolchain (under Rosetta, no problems):

```sh
export PATH="$HOME/akso/external/gcc-arm/mac/gcc-arm-none-eabi-8-2019-q3-update/bin:$PATH"
cd axoloti/firmware && make
```

Result: main firmware + flasher.

### 2.4 `Axoloti.jar`

```sh
export JAVA_HOME=external/jdks/mac_arm64/zulu11.90.205-ca-jdk11.0.32.1-macosx_aarch64/Contents/Home
cd axoloti && ant            # BUILD SUCCESSFUL
```

`dist/Axoloti.jar` has 3911 entries; the jar inside the installed app has 3912
(the only difference is one spurious `/` entry from the 2020 build).
**Same app.**

### 2.5 PackrLauncher for arm64

Source: `karlsabo/packr`, commit `1423bf3` (the 2.7.0 era launcher, the same
one the original app ships). Build:

```sh
JDK=.../zulu11.../Contents/Home
c++ -O2 -std=c++14 -arch arm64 \
    -I <packr-src>/PackrLauncher/src \
    -I <packr-src>/PackrLauncher/src/headers \
    -I $JDK/include -I $JDK/include/darwin \
    -DPACKR_VERSION_STRING=\"2.7.0\" \
    <packr-src>/PackrLauncher/src/main/packr.cpp \
    <packr-src>/PackrLauncher/src/main/macos/packr_macos.cpp \
    -framework CoreFoundation \
    -o Akso-launcher-arm64
```

Key facts from the code:

- It reads `config.json` (parser `sajson.h` + DrOpt for args).
- On macOS it locates `Contents/Resources/config.json` through
  `CFBundleCopyResourcesDirectoryURL` (that is why a standalone binary outside
  the bundle cannot find the config).
- It `dlopen`s `libjvm.dylib`, creates the JVM and runs `axoloti.Axoloti`.

### 2.6 The `Akso.app` bundle

New config `packr-mac-arm64.json` (copy of `packr-mac-x64.json` pointing at the
arm64 JDK):

```json
{
    "platform": "mac",
    "jdk": "external/jdks/mac_arm64/zulu11-macosx_aarch64.tar.gz",
    "executable": "Akso",
    "classpath": ["axoloti/dist/Axoloti.jar"],
    "removelibs": ["axoloti/dist/Axoloti.jar"],
    "mainclass": "axoloti.Axoloti",
    "vmargs": ["Xms1G", "Xmx1G"],
    "resources": ["external", "axoloti/firmware", "axoloti/platform_osx"],
    "minimizejre": "hard",
    "output": "package/mac/Akso.app",
    "verbose": true,
    "bundle": "org.zrna.akso",
    "icon": "axoloti/src/main/java/resources/axoloti_512x512.icns"
}
```

Run (Packr 3.0.3 from the `NimblyGames/Packr` releases; the jar embedded in the
repo is 2.7.0):

```sh
$JAVA_HOME/bin/java -jar packr-all-3.0.3.json packr-mac-arm64.json
# → "Unpacking JRE ... Minimizing JRE ... Done!"  (jlink arm64)
```

Post-processing (equivalent to the `package-mac` Makefile target):

```sh
A=package/mac/Akso.app/Contents/Resources
rm -rf $A/external/jdks $A/external/gcc-arm/linux $A/external/gcc-arm/win
chmod +x $A/jre/lib/jspawnhelper
chmod +x $A/external/STM32CubeProgrammer/mac/STM32CubeProgrammer.app/Contents/MacOs/bin/STM32_Programmer_CLI
```

Launcher swap (Packr 3.0.3 embeds its own x86_64 launcher; replace it with the
arm64 binary built in 2.5):

```sh
cp Akso-launcher-arm64 package/mac/Akso.app/Contents/MacOS/Akso
```

### 2.7 Bundle verification

```
$ file package/mac/Akso.app/Contents/MacOS/Akso
  → Mach-O 64-bit executable arm64
$ file package/mac/Akso.app/Contents/Resources/jre/bin/java
  → Mach-O 64-bit executable arm64
$ otool -L package/mac/Akso.app/Contents/MacOS/Akso
  → system frameworks only (CoreFoundation, libc++, libSystem)
$ sample <pid> 1
  → Code Type: ARM64
```

The bundle's `config.json` is identical to the original (`classPath`,
`mainClass`, `vmArgs -Xms1G -Xmx1G`).

---

## 3. USB connection fix (usb4java)

### Diagnosis

The app talks to the board through **usb4java 1.2.0** (JNI; `org.usb4java.*`,
used by `axoloti.Usb` / `axoloti.usb.LibUSBContext` / `USBBulkConnection`). The
jar only contained these native libraries:

```
org/usb4java/osx-x86/libusb4java.dylib      (2014)
org/usb4java/osx-x86_64/libusb4java.dylib   (2014)
```

usb4java 1.2.0's `org.usb4java.Loader` resolves the folder from `os.name` +
`os.arch`: on an arm64 JVM it looks for **`org/usb4java/osx-aarch64/`**, which
did not exist → the JVM could not load the library → "cannot connect to the
hardware", with no explicit error in the UI.

### Building `libusb4java.dylib` for arm64

- Wrapper sources: `https://github.com/usb4java/libusb4java.git`, commit
  `304a333`.
- libusb: **1.0.17** (GitHub tag, contemporary with the 2014 1.2.0 release),
  built statically for arm64:

  ```sh
  ./configure --disable-shared --enable-static --with-pic \
      CFLAGS="-arch arm64 -O2" LDFLAGS="-arch arm64"
  make    # → libusb/.libs/libusb-1.0.a
  ```

- A minimal `src/config.h` was created (normally generated by autotools; it is
  `#include`d by `usb4java.h`).
- Compiled the wrapper `src/*.c` (22 objects) and linked:

  ```sh
  cc -dynamiclib -arch arm64 -o libusb4java.dylib obj/*.o \
      <libusb-1.0.17>/libusb/.libs/libusb-1.0.a \
      -framework Foundation -framework IOKit
  ```

- Result: arm64 `libusb4java.dylib`, self-contained (libusb is static; it only
  links system frameworks), 168 KB.

### Smoke test (with the real 1.2.0 classes from the jar)

A minimal Java test (`LibUsb.init` + enumerate + open + claim) with the new
dylib on the classpath:

```
init = 0                        ← SUCCESS
1 device: vid=0x16C0 pid=0x0442 ← Akso board detected
open = 0, claimInterface = 0    ← communication OK
```

### Integration

```sh
# with the dylib placed at <dir>/org/usb4java/osx-aarch64/libusb4java.dylib:
cd <dir> && zip Axoloti.jar org/usb4java/osx-aarch64/libusb4java.dylib
```

Applied to `axoloti/dist/Axoloti.jar` (build output) and to the jar inside
`package/mac/Akso.app/Contents/Resources/Axoloti.jar`.

### Verification in the real app

After relaunching: **"Connected to device."**, Firmware ID reported, "SDCard
mounted". Process: `Code Type: ARM64`.

---

## 4. Board firmware

- The board shipped with the 2020 firmware (**CRC 0x9922E990**).
- The new app embeds the firmware compiled from the current tree
  (**CRC 0xA504289E** at that time; the current build is **7072DEBF**).
- The app offered the standard firmware update: the board entered DFU mode
  (STM32 ROM bootloader over USB; `dfu-util` x86_64 under Rosetta works fine),
  flashed and rebooted with the normal LED sequence ending in solid green.

---

## 5. Final state

| Item | State |
|---|---|
| New app | `package/mac/Akso.app` — 100% arm64 |
| Original app | `/Applications/Akso.app` — untouched (x86_64, kept as reference/backup) |
| Launcher | arm64 (custom-built PackrLauncher 2.7.0) |
| JRE | arm64 (Zulu 11.0.32.1, minimized with `jlink`) |
| `Axoloti.jar` | Java (arch independent) + **aarch64** `libusb4java.dylib` added |
| Patch compiler | **arm64 native (GCC 15.3)** since 2026-09-30 — see `PATCH-COMPILER-ARM64.md` |
| Firmware toolchain | x86_64 (runs under Rosetta as a child process) |
| Board | New firmware flashed |

### What is tracked in the repo

Following the existing `external/jdks/*` convention for the other platforms:

- **Tracked:** `external/jdks/mac_arm64/zulu11-macosx_aarch64.tar.gz`
  (Zulu JDK 11 aarch64, ~190 MB, stored through **Git LFS** — same as the
  `linux_x64` / `mac_x64` / `win_x64` JDKs of the repo) and
  `packr-mac-arm64.json`. On another Mac: unpack the tarball into
  `external/jdks/mac_arm64/`.
- **Ignored (`.gitignore`):** the extracted JDK
  `external/jdks/mac_arm64/zulu11.90.205-ca-jdk11.0.32.1-macosx_aarch64/`
  (~300 MB) and the arm64 toolchain `external/gcc-arm/mac-arm64/` (280 MB).
- **Regenerated after each `ant` run:** `axoloti/dist/Axoloti.jar` loses the
  aarch64 `libusb4java.dylib`; re-inject it with
  `zip Axoloti.jar org/usb4java/osx-aarch64/libusb4java.dylib` before packaging
  (see the top-level `README.md`).

---

## 6. Open items / optional improvements

1. **Sign / notarize the bundle.** For use outside this machine, an ad-hoc
   signature (`codesign --force --deep -s - Akso.app`) or Developer ID signing
   plus notarization is required, otherwise Gatekeeper quarantines the app.
2. **Universal build** (one bundle for both Intel and Apple Silicon): universal
   launcher (compile the C++ with both `-arch` flags) + universal JRE
   (multi-arch JDK and `jlink --target-image x86_64 --target-image aarch64`) +
   usb4java dylibs for both architectures.
3. **Native arm64 firmware toolchain.** The patch compiler is already native
   (`PATCH-COMPILER-ARM64.md`); the *firmware* still builds with the x86_64
   GCC 8-2019 toolchain under Rosetta. Upstream's toolchain is not in this
   tree; see `README.md` for how to restore it.
4. **STM32CubeProgrammer arm64.** The CLI bundled for the "Board → Flash
   (Rescue)" path is 2.4.0 x86_64 (runs under Rosetta; Rosetta 2 ends with
   macOS 27).
5. **Cross-platform fixes.** The USB host MIDI and card-reader fixes are not
   Mac-specific — see `MIDI-USB-HOST-FINDINGS.md` and
   `CARD-READER-MASS-STORAGE.md` for how to port them.
