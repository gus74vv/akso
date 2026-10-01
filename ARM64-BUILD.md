# Akso (zrna) — Conversión a Apple Silicon arm64 nativo

> Sesion del 27/09/2026. Conversión completa de `Akso.app` de x86_64 (Rosetta) a
> **arm64 nativo**, incluida la fix de conexión USB al hardware y el firmware
> de la placa.

> **Estado: COMPLETO y 100% funcional** (placa conectada, firmware A504289E
> flasheado, app instalada en `/Applications/Akso.app`). Siguientes pasos
> (MIDI, mejoras) en `AGENTS.md`.

---

## 1. Contexto y diagnóstico inicial

### ¿Qué es el app original?

`/Applications/Akso.app` (build 07/09/2020) es un bundle **Packr 2.7.0**:

| Componente | Arquitectura original | Notas |
|---|---|---|
| `Contents/MacOS/Akso` (launcher C++ "PackrLauncher") | x86_64 | Lee `config.json`, crea la JVM |
| `Contents/Resources/jre/` (OpenJDK/Corretto 11.0.8) | x86_64 | Embebido, minimizado |
| `Contents/Resources/Axoloti.jar` (app Java, main `axoloti.Axoloti`) | independiente | Java puro |
| `Contents/Resources/external/gcc-arm/mac` (arm-none-eabi-gcc 8-2019) | x86_64 | Toolchain para firmware STM32 |
| `Contents/Resources/external/jdks` (Corretto 11.0.8 x64) | x86_64 | Solo para build, no queda en el app |

Todo el app corría bajo **Rosetta 2** en Apple Silicon.

### El error de `xcrun`

Al compilar un patch, el app (x86_64) lanza un shell que invoca `xcrun`. El
`/usr/bin/xcrun` universal, bajo Rosetta, corre como x86_64 y necesita cargar
`/Library/Developer/CommandLineTools/usr/lib/libxcrun.dylib` — que en el
sistema **solo tenía slices arm64/arm64e (sin x86_64)**:

```
xcrun: error: unable to load libxcrun (dlopen(...): fat file, but missing
compatible architecture (have 'arm64,arm64e', need 'x86_64'))
```

Es decir: estado híbrido de las Command Line Tools (SDKs modernos macOS 26/27
pero libxcrun arm64-only). Solución de fondo: app nativo arm64 (sus hijos
corren arm64 y usan la slice que sí existe).

### Pipeline oficial descubierto en el repo

Repo: <https://zrna.org/akso> → clonado en `/Users/gustavo/Documents/dev/akso`
(version 3.0.1). El `Makefile` + `packr-mac-x64.json` revelan el build oficial:

```
make firmware   → cd axoloti/firmware && ./compile_firmware_linux.sh
make gui        → cd axoloti && ant              (genera axoloti/dist/Axoloti.jar)
package-mac     → java -jar packr-all-2.7.0.json packr-mac-x64.json
                → limpieza: rm external/jdks, chmod jspawnhelper, etc.
```

- El JRE embebido viene **en el repo**: `external/jdks/mac_x64/OpenJDK11U-jdk_x64_mac_hotspot_11.0.8_10.tar.gz`
- `minimizejre: "hard"` → Packr corre `jlink` sobre ese JDK.
- **Hallazgo clave**: el launcher `packr-mac` del Packr 2.7.0 es x86_64-only, y
  también en Packr 3.0.3 / 4.0.0 (jar de 2021). Para tener launcher nativo hay
  que **compilar el PackrLauncher C++ para arm64** (es open source, ~700 líneas).

---

## 2. Conversión a arm64 (pasos ejecutados)

### 2.1 Herramientas

```sh
brew install ant            # ant 1.10.18 (host ya tenía Zulu JDK 22)
```

### 2.2 JDK 11 arm64

Descarga de **Azul Zulu 11.0.32.1 aarch64** (formato JDK.app, el mismo que el
Corretto del repo) y lo dejé en el repo para que el build sea autocontenido:

```
external/jdks/mac_arm64/zulu11-macosx_aarch64.tar.gz
external/jdks/mac_arm64/zulu11.90.205-ca-jdk11.0.32.1-macosx_aarch64/Contents/Home
```

Verificado: `bin/java` → `Mach-O 64-bit executable arm64`.

Este JDK se usó para las 3 cosas: build de `Axoloti.jar` (ant), build del
bundle (Packr corre `jlink` con él → JRE arm64) y como JDK de build local.

### 2.3 Firmware

Compilado con el toolchain embebido (x86_64 bajo Rosetta, sin problemas):

```sh
export PATH=/Users/gustavo/Documents/dev/akso/external/gcc-arm/mac/gcc-arm-none-eabi-8-2019-q3-update/bin:$PATH
cd axoloti/firmware && make
```

OK: firmware principal + flasher.

### 2.4 `Axoloti.jar`

```sh
export JAVA_HOME=external/jdks/mac_arm64/zulu11.90.205-ca-jdk11.0.32.1-macosx_aarch64/Contents/Home
cd axoloti && ant            # BUILD SUCCESSFUL
```

`dist/Axoloti.jar` = 3911 entries; el jar del app instalado = 3912 (la única
diferencia es un entry spurious `/` del build 2020). **Mismo app.**

### 2.5 Launcher PackrLauncher para arm64

Source: `karlsabo/packr` (clonado en `/tmp/packr-src`), commit `1423bf3`
(era 2.7.0, el mismo launcher que trae el app original). Compilación:

```sh
JDK=.../zulu11.../Contents/Home
c++ -O2 -std=c++14 -arch arm64 \
    -I /tmp/packr-src/PackrLauncher/src \
    -I /tmp/packr-src/PackrLauncher/src/headers \
    -I $JDK/include -I $JDK/include/darwin \
    -DPACKR_VERSION_STRING=\"2.7.0\" \
    /tmp/packr-src/PackrLauncher/src/main/packr.cpp \
    /tmp/packr-src/PackrLauncher/src/main/macos/packr_macos.cpp \
    -framework CoreFoundation \
    -o /tmp/Akso-launcher-arm64
```

Resultados clave del código:

- Lee `config.json` (parser `sajson.h` + DrOpt para args).
- En macOS usa `CFBundleCopyResourcesDirectoryURL` para localizar
  `Contents/Resources/config.json` (por eso el binario suelto, fuera del
  bundle, no encuentra el config).
- `dlopen` de `libjvm.dylib`, crea la JVM, corre `axoloti.Axoloti`.

### 2.6 Bundle `Akso.app`

Config nueva `packr-mac-arm64.json` (copia de `packr-mac-x64.json` con el JDK
arm64):

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

Ejecución (Packr 3.0.3, descargado de los releases de `NimblyGames/Packr`;
el jar embebido del repo es 2.7.0):

```sh
$JAVA_HOME/bin/java -jar /tmp/packr-all-3.0.3.json packr-mac-arm64.json
# → "Unpacking JRE ... Minimizing JRE ... Done!"  (jlink arm64)
```

Post-procesado (equivalente al target `package-mac` del Makefile):

```sh
A=package/mac/Akso.app/Contents/Resources
rm -rf $A/external/jdks $A/external/gcc-arm/linux $A/external/gcc-arm/win
chmod +x $A/jre/lib/jspawnhelper
chmod +x $A/external/STM32CubeProgrammer/mac/STM32CubeProgrammer.app/Contents/MacOs/bin/STM32_Programmer_CLI
```

Intercambio del launcher (Packr 3.0.3 embebe su launcher x86_64; se reemplaza
por el arm64 compilado en 2.5):

```sh
cp /tmp/Akso-launcher-arm64 package/mac/Akso.app/Contents/MacOS/Akso
```

### 2.7 Verificación del bundle

```
$ file package/mac/Akso.app/Contents/MacOS/Akso
  → Mach-O 64-bit executable arm64
$ file package/mac/Akso.app/Contents/Resources/jre/bin/java
  → Mach-O 64-bit executable arm64
$ otool -L package/mac/Akso.app/Contents/MacOS/Akso
  → solo frameworks del sistema (CoreFoundation, libc++, libSystem)
$ sample <pid> 1
  → Code Type: ARM64
```

`config.json` del bundle idéntico al original (`classPath`, `mainClass`,
`vmArgs -Xms1G -Xmx1G`).

**App final: `/Users/gustavo/Documents/dev/akso/package/mac/Akso.app`**

---

## 3. Fix de conexión USB al hardware (usb4java)

### Diagnóstico

El app se conecta a la placa vía **usb4java 1.2.0** (JNI; clases
`org.usb4java.*`, usado por `axoloti.Usb` / `axoloti.usb.LibUSBContext` /
`USBBulkConnection`). En el jar solo existían los dylibs:

```
org/usb4java/osx-x86/libusb4java.dylib      (2014)
org/usb4java/osx-x86_64/libusb4java.dylib   (2014)
```

El `org.usb4java.Loader` de 1.2.0 resuelve el folder por `os.name` +
`os.arch`: en una JVM arm64 → **`org/usb4java/osx-aarch64/`**, que no existe
→ la JVM no carga la librería → "no se conecta al hardware" (sin error
explícito visible en la UI).

### Build de `libusb4java.dylib` arm64

- Source del wrapper: `https://github.com/usb4java/libusb4java.git` (clonado
  en `/tmp/libusb4java`, commit `304a333`).
- libusb: **1.0.17** (tag de GitHub, coetáneo a la release 1.2.0 de 2014),
  compilado estáticamente para arm64:

  ```sh
  cd /tmp/libusb-1.0.17
  ./configure --disable-shared --enable-static --with-pic \
      CFLAGS="-arch arm64 -O2" LDFLAGS="-arch arm64"
  make    # → libusb/.libs/libusb-1.0.a
  ```

- Se creó un `src/config.h` mínimo (el autotools original lo genera; el
  `usb4java.h` lo include).
- Compilación de los `src/*.c` del wrapper (22 objetos) y link:

  ```sh
  JDK=.../zulu11.../Contents/Home
  for f in src/*.c; do
      cc -O2 -fPIC -arch arm64 -I src -I "$JDK/include" -I "$JDK/include/darwin" \
          -I /tmp/libusb-1.0.17/libusb -c "$f" -o "obj/$(basename "$f" .c).o"
  done
  cc -dynamiclib -arch arm64 -o libusb4java.dylib obj/*.o \
      /tmp/libusb-1.0.17/libusb/.libs/libusb-1.0.a \
      -framework Foundation -framework IOKit
  ```

- Resultado: `libusb4java.dylib` arm64, autocontenido (libusb estática; solo
  depende de frameworks del sistema). 168 KB.

### Prueba de humo (con las clases 1.2.0 reales del jar)

Test Java mínimo (`LibUsb.init` + enumeración + open + claim) con el dylib en
el classpath:

```
init = 0                        ← SUCCESS
1 device: vid=0x16C0 pid=0x0442 ← placa Akso detectada
open = 0, claimInterface = 0    ← comunicación OK
```

### Integración

```sh
# con el dylib en <dir>/org/usb4java/osx-aarch64/libusb4java.dylib:
cd <dir> && zip Axoloti.jar org/usb4java/osx-aarch64/libusb4java.dylib
```

Aplicado a `axoloti/dist/Axoloti.jar` (source) y al jar dentro del bundle
`package/mac/Akso.app/Contents/Resources/Axoloti.jar`.

### Verificación en la app real

Al relanzar el app: **"Connected to device."**, Firmware ID `A504289E`,
"SDCard mounted". Proceso: `Code Type: ARM64`.

---

## 4. Firmware de la placa

- La placa traía el firmware de la build 2020: **crc 0x9922E990**.
- El app nuevo embebe el firmware compilado del repo actual: **crc 0xA504289E**.
- El app pidió el update de firmware (flujo estándar): la placa entró en modo
  flash DFU (STM32 bootloader por USB; el `dfu-util` x86_64 embebido corre bajo
  Rosetta, sin problemas), flasheó y reinició con la secuencia de LEDs normal
  terminando en verde sólido.
- Conectado de nuevo: **OK — "wow, funciona"**.

---

## 5. Estado final

| Item | Estado |
|---|---|
| App nuevo | `/Users/gustavo/Documents/dev/akso/package/mac/Akso.app` — 100% arm64 |
| App original | `/Applications/Akso.app` — intacto (x86_64, sirvió de referencia/backup) |
| Launcher | arm64 (PackrLauncher 2.7.0 compilado a medida) |
| JRE | arm64 (Zulu 11.0.32.1, minimizado con jlink) |
| `Axoloti.jar` | Java (independiente) + `libusb4java.dylib` **aarch64** añadido |
| Toolchain firmware | x86_64 (corre bajo Rosetta como hijo; se puede hacer arm64 opcional) |
| Placa | Firmware nuevo (A504289E) flasheado |
| Error `xcrun` | Resuelto por diseño: el app y sus hijos corren arm64 nativo |

### Cambios en el repo (decisión 27/09/2026, coherente con la convención
existente de `external/jdks/*` de las otras plataformas)

- **Trackeado:** `external/jdks/mac_arm64/zulu11-macosx_aarch64.tar.gz`
  (Zulu JDK 11 aarch64, ~190 MB — igual que los JDKs de linux_x64/mac_x64/
  win_x64 del repo) y `packr-mac-arm64.json` (config del bundle arm64;
  ver §2.6). En otro Mac: descomprimir el tarball en `external/jdks/mac_arm64/`
  (ver §2.2).
- **Ignorado (`.gitignore`):** el JDK extraído
  `external/jdks/mac_arm64/zulu11.90.205-ca-jdk11.0.32.1-macosx_aarch64/`
  (~300 MB).
- `Axoloti.jar` (dist + bundle) — con `org/usb4java/osx-aarch64/libusb4java.dylib`
- (Opcional) el dylib aarch64 + script de build en el repo para reproducibilidad

---

## 6. Pendientes / mejoras opcionales

1. **Sustituir `/Applications/Akso.app`** por el nuevo (el original quedó como
   backup) y/o distribuir el nuevo a otros Macs.
2. **Firmar el bundle** (`codesign --force --deep -s -` ad-hoc) si se va a
   compartir fuera de la máquina (notarización solo si se publica).
3. ~~**Toolchain arm64**: reemplazar `external/gcc-arm/mac` por
   `arm-none-eabi-gcc` arm64 (p. ej. Homebrew) para compilar firmware 100%
   nativo.~~ **HECHO (30/09/2026) para el COMPILADOR DE PATCHES**: GCC 15.3 arm64
   nativo en `<bundle>/external/gcc-arm/mac-arm64/` (podado, 280 MB), por
   defecto vía `Makefile.patch`. El *firmware* sigue en el toolchain x86
   (Rosetta). Mediciones y ROLLBACK en **`PATCH-COMPILER-ARM64.md`**.
4. **Build universal** (para que el mismo bundle sirva en Intel y Apple
   Silicon): launcher universal (compilar el C++ con ambos `-arch`) + JRE
   universal (JDK multi-arch + `jlink --target-image x86_64 --target-image
   aarch64`) + dylib usb4java de ambas arquitecturas.
5. **Compilar un patch** de prueba para re-verificar en vivo la desaparición
   del error de `xcrun` (el camino arm64 ya está cubierto por diseño).
6. **Commit de los cambios** en el repo (dylib, script de build del launcher y
   del dylib usb4java). **Decisión 27/09/2026:** tarball del JDK y
   `packr-mac-arm64.json` trackeados (convención `external/jdks/*`); solo el
   JDK extraído queda en `.gitignore` (ver §5).
