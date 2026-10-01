# Patcher — se pueden borrar objetos/conexiones con un patch LIVE (sesión 29/09)

> Estado: **diagnóstico + solución propuesta, NO implementado.**
> Pendiente de implementar en una sesión futura (foco: mejoras del patcher).

## Síntoma

Mientras un patch está live (corriendo en el board), el editor no impide
borrar objetos ni conexiones. El comportamiento **no es intencional**: el
app ya tiene un mecanismo de "lock" pensado para congelar el editor en
ese estado, pero los paths de borrado lo ignoran.

## Diagnóstico

### El mecanismo de lock (existe, funciona a medias)

- `PatchViewLive.goLive()` setea `controller.setLocked(true)` al terminar
  exitosamente compilación + upload + start
  (`axoloti/src/main/java/axoloti/live/patch/PatchViewLive.java:297`), y
  `setLocked(false)` en el path de error.
- Se desloquea también en `PatchFrame.showDisconnect()` / al parar el
  patch (`transmitStop`) y en `PatchView.showCompileFail()`.
- El flag vive en `PatchModel.locked` (`PatchModel.java:116`), expuesto por
  `PatchController.isLocked()` / `setLocked()` (`PatchController.java:665-671`).

### Paths de edición que SÍ respetan el lock

| Acción | Dónde se chequea |
|---|---|
| Mover objetos (flechas) | `PatchView.moveSelectedAxoObjInstances()` — loguea `can't move: locked` |
| Agregar objeto (selector de clases) | `PatchView.showClassSelector()` — early return |
| Paste | `PatchViewSwing` `importData()` — `if (!isLocked())` |

### Paths de edición que NO lo respetan (el bug)

Todos van directo al `PatchController` sin chequear `isLocked()`:

| Acción | Entry point | Llega a |
|---|---|---|
| Tecla Delete/Backspace | `PatchViewSwing.java:~224`, `PatchViewPiccolo.java:~155` | `PatchController.delete(IAxoObjectInstance)` (`:291`) |
| Menú Edit → Delete | `PatchFrame.java:~1150` (`jMenuItemDeleteActionPerformed`) | idem |
| Cut (arrastrar y mover fuera) | `PatchViewSwing.java:~355` (TransferHandler `MOVE`) | idem |
| Context menu iolet "Disconnect" | `IoletInstancePopupMenu.java:~64` | `PatchController.disconnect(InletInstance)` (`:787`) |
| Context menu iolet "Delete net" | `IoletInstancePopupMenu.java:~80` | `PatchController.delete(Net)` (`:820`) |
| Arrastrar cable y soltar fuera | `IoletInstanceView.java:~109` | `disconnect(Inlet/OutletInstance)` |

Los 3 métodos del controller afectados **no tienen guard**:

- `PatchController.java:291` — `delete(IAxoObjectInstance o)`
- `PatchController.java:820` — `delete(Net n)`
- `PatchController.java:787` / `:811` — `disconnect(InletInstance)` / `disconnect(OutletInstance)`

**Evidencia de la intención original:** en `addObjectInstance`
(`PatchController.java:311-313`) el guard quedó **comentado a medias**:

```java
if (true) {
    /*!isLocked()) {*/
```

con su branch de else logueando `can't add connection: locked!`. Es decir,
el lock sí existía como guard de edición; la mitad de los paths nunca se
le aplicó (y otra se desactivó y quedó así).

### Consecuencias prácticas (por qué "anda" pero es un gap)

- El board sigue corriendo el binario viejo: el auto-recompile de
  `PatchViewLive` está **desactivado** (`enableAutoRecompile()` comentado en
  el constructor; `modelPropertyChange()` early-return si
  `!auto_recompile`). El borrado solo surte efecto al **próximo go-live**
  (recompila desde el modelo).
- Mientras tanto `PatchViewLive` queda con referencias viejas:
  `parameterInstanceViews` de parámetros de objetos ya borrados y
  `disp_addr`/`disp_length` del display vector antiguo. En general
  inofensivo, pero caldo de cultura: p. ej. param changes reporteados por el
  board contra índices fuera de rango (`USBBulkConnection_v2`
  `RPacketParamChange` loguea `index out of range`).
- Riesgo de confusión de estado: el patch live y el modelo divergen sin que
  el UI avise de nada.

## Solución propuesta (quirúrgica, 1 archivo)

**Fichero único:** `axoloti/src/main/java/axoloti/patch/PatchController.java`.
Todo el borrado funnelea a 3 métodos; guards ahí cubren todos los paths de
UI (tecla, menú, cut, context menu, drag):

1. `delete(IAxoObjectInstance o)` (`:291`) — al inicio:
   ```java
   if (isLocked()) {
       Logger.getLogger(PatchController.class.getName()).log(Level.INFO, "can't delete object: locked");
       return deletionSucceeded;
   }
   ```
2. `delete(Net n)` (`:820`) — al inicio:
   ```java
   if (isLocked()) {
       Logger.getLogger(PatchController.class.getName()).log(Level.INFO, "can't delete net: locked");
       return;
   }
   ```
3. `disconnect(InletInstance)` (`:787`) y `disconnect(OutletInstance)`
   (`:811`) — al inicio de cada uno:
   ```java
   if (isLocked()) {
       Logger.getLogger(PatchController.class.getName()).log(Level.INFO, "can't disconnect: locked");
       return false;
   }
   ```

Notas de diseño:

- Estilo consistente con el existente (`can't move: locked`, `can't add
  connection: locked!`).
- **No hay callers internos no-UI** de estos 3 métodos (verificado:
  `PatchViewSwing`, `PatchViewPiccolo`, `PatchFrame`,
  `IoletInstancePopupMenu`, `IoletInstanceView` — todo UI), así que el
  path de go-live/stop no se pisa.
- `delete(o)` llama a `disconnect(IAxoObjectInstance)` (otra sobrecarga,
  `:586`), que itera los iolets y llama a las sobrecargas de inlet/outlet
  que quedan guardadas — orden correcto: `delete` chequea el lock **antes**
  de disconnectar.
- **Quirk menor conocido:** los callers de UI hacen
  `addMetaUndo("delete objects")` **antes** de iterar los
  `delete(o)`. Si el guard bloquea, queda una meta-undo vacía en la pila
  (un Undo extra que no hace nada). Aceptable; si se quiere pulir, checar
  `isLocked()` en el caller antes del `addMetaUndo` (4 lugares).

### Scope opcional (NO incluido por default)

Si se quiere congelar **todo** el editor con patch live (no solo borrado),
agregar guards también en:

- `addConnection(InletInstance, OutletInstance)` (`:696`) — hoy permite
  **crear** conexiones con patch live.
- `addObjectInstance(IAxoObject, Point)` (`:311`) — restaurar el guard
  comentado: reemplazar `if (true) { /*!isLocked()) {*/` por
  `if (!isLocked()) {` y quitar el `if (true)` (ojo: el log del else dice
  "add connection", debería decir "add object").

### Observación colateral (fuera de scope)

El menú Edit → Lock/Unlock es un **no-op**:
`jMenuItemLockActionPerformed` / `jMenuItemUnlockActionPerformed`
(`PatchFrame.java:995-1001`) tienen body vacío con
`//getPatchView().Lock();` comentado. El lock se maneja solo de forma
automática por go-live/stop.

## Verificación (al implementar)

1. Build del jar: `cd axoloti && JAVA_HOME=../external/jdks/mac_arm64/*/ ant`
   + reinyección de la dylib usb4java aarch64 (`zip Axoloti.jar
   org/usb4java/osx-aarch64/libusb4java.dylib`) + reemplazo en ambos
   bundles (dev `package/mac/Akso.app` e instalado), backup del jar
   anterior (ver AGENTS.md §build).
2. Testeo manual con la placa:
   - [ ] Go-live con un patch; intentar Delete/Backspace sobre objeto
         seleccionado → no se borra.
   - [ ] Edit → Delete → no se borra.
   - [ ] Context menu de un iolet con cable: "Disconnect" / "Delete net" →
         no pasa nada.
   - [ ] Arrastrar un cable hasta soltarlo fuera → no se desconecta.
   - [ ] Arrastrar un objeto (cut) → no se borra.
   - [ ] El patch sigue sonando todo el tiempo (sin reset/underflows
         nuevos).
   - [ ] Parar el patch (o desconectar): todos los borrados vuelven a
         funcionar.
   - [ ] Agregar/mover objetos con patch live: mismo comportamiento de
         antes (mover bloqueado, agregar bloqueado por
         `showClassSelector`) — no se rompe nada.
3. Sanity: log INFO `can't ...: locked` visible en la consola del app al
   intentar cada operación bloqueada.
