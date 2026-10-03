# Patcher — objects and connections can be deleted while a patch is LIVE

> **Status: diagnosis + proposed fix — NOT IMPLEMENTED.** Kept for a future
> session. Nobody has patched this yet, so the bug is still present in this
> build.

## Symptom

While a patch is live (running on the board), the editor does not prevent
deleting objects or connections. This is **not intentional**: the app already
has a "lock" mechanism meant to freeze the editor in that state, but the delete
paths ignore it.

## Diagnosis

### The lock mechanism (it exists, it is half-wired)

- `PatchViewLive.goLive()` calls `controller.setLocked(true)` after a successful
  compile + upload + start (`axoloti/src/main/java/axoloti/live/patch/PatchViewLive.java:297`),
  and `setLocked(false)` on the error path.
- It is also unlocked in `PatchFrame.showDisconnect()` / when the patch is
  stopped (`transmitStop`) and in `PatchView.showCompileFail()`.
- The flag lives in `PatchModel.locked` (`PatchModel.java:116`), exposed through
  `PatchController.isLocked()` / `setLocked()` (`PatchController.java:665-671`).

### Editing paths that DO honour the lock

| Action | Where it is checked |
|---|---|
| Moving objects (arrow keys) | `PatchView.moveSelectedAxoObjInstances()` — logs `can't move: locked` |
| Adding an object (class selector) | `PatchView.showClassSelector()` — early return |
| Paste | `PatchViewSwing.importData()` — `if (!isLocked())` |

### Editing paths that DO NOT honour it (the bug)

All of them go straight to `PatchController` without checking `isLocked()`:

| Action | Entry point | Reaches |
|---|---|---|
| Delete/Backspace key | `PatchViewSwing.java:~224`, `PatchViewPiccolo.java:~155` | `PatchController.delete(IAxoObjectInstance)` (`:291`) |
| Edit → Delete menu | `PatchFrame.java:~1150` (`jMenuItemDeleteActionPerformed`) | same |
| Cut (drag out) | `PatchViewSwing.java:~355` (TransferHandler `MOVE`) | same |
| Iolet context menu "Disconnect" | `IoletInstancePopupMenu.java:~64` | `PatchController.disconnect(InletInstance)` (`:787`) |
| Iolet context menu "Delete net" | `IoletInstancePopupMenu.java:~80` | `PatchController.delete(Net)` (`:820`) |
| Dragging a cable and dropping it outside | `IoletInstanceView.java:~109` | `disconnect(Inlet/OutletInstance)` |

The three affected controller methods have **no guard**:

- `PatchController.java:291` — `delete(IAxoObjectInstance o)`
- `PatchController.java:820` — `delete(Net n)`
- `PatchController.java:787` / `:811` — `disconnect(InletInstance)` / `disconnect(OutletInstance)`

**Evidence of the original intent:** in `addObjectInstance`
(`PatchController.java:311-313`) the guard is **half commented out**:

```java
if (true) {
    /*!isLocked()) {*/
```

with its else branch still logging `can't add connection: locked!`. So the lock
did exist as an editing guard; half of the paths never got it (and one was
disabled and left that way).

### Practical consequences (why it "works" but is still a gap)

- The board keeps running the old binary: `PatchViewLive`'s auto-recompile is
  **disabled** (`enableAutoRecompile()` commented out in the constructor;
  `modelPropertyChange()` early-returns when `!auto_recompile`). Deletion only
  takes effect at the **next go-live** (which recompiles from the model).
- Meanwhile `PatchViewLive` keeps stale references: `parameterInstanceViews` of
  deleted objects and the old display vector's `disp_addr`/`disp_length`. Mostly
  harmless, but it is fertile ground: e.g. parameter changes reported by the
  board against out-of-range indices (`USBBulkConnection_v2` `RPacketParamChange`
  logs `index out of range`).
- State confusion risk: the live patch and the model diverge with no UI
  feedback.

## Proposed fix (surgical, single file)

**File:** `axoloti/src/main/java/axoloti/patch/PatchController.java`.
All deletion funnels into 3 methods, so guards there cover every UI path
(keyboard, menu, cut, context menu, drag):

1. `delete(IAxoObjectInstance o)` (`:291`) — at the top:
   ```java
   if (isLocked()) {
       Logger.getLogger(PatchController.class.getName()).log(Level.INFO, "can't delete object: locked");
       return deletionSucceeded;
   }
   ```
2. `delete(Net n)` (`:820`) — at the top:
   ```java
   if (isLocked()) {
       Logger.getLogger(PatchController.class.getName()).log(Level.INFO, "can't delete net: locked");
       return;
   }
   ```
3. `disconnect(InletInstance)` (`:787`) and `disconnect(OutletInstance)`
   (`:811`) — at the top of each:
   ```java
   if (isLocked()) {
       Logger.getLogger(PatchController.class.getName()).log(Level.INFO, "can't disconnect: locked");
       return false;
   }
   ```

Design notes:

- Style matches the existing messages (`can't move: locked`,
  `can't add connection: locked!`).
- There are **no non-UI internal callers** of these 3 methods (verified:
  `PatchViewSwing`, `PatchViewPiccolo`, `PatchFrame`, `IoletInstancePopupMenu`,
  `IoletInstanceView` — all UI), so the go-live/stop paths are unaffected.
- `delete(o)` calls `disconnect(IAxoObjectInstance)` (the other overload,
  `:586`), which iterates iolets and calls the now-guarded inlet/outlet
  overloads — correct order: `delete` checks the lock **before** disconnecting.
- **Known minor quirk:** UI callers run `addMetaUndo("delete objects")`
  **before** iterating `delete(o)`. If the guard blocks, an empty meta-undo is
  left on the stack (an Undo entry that does nothing). Acceptable; to polish it,
  check `isLocked()` in the caller before `addMetaUndo` (4 places).

### Optional scope (NOT included by default)

To freeze the **whole** editor while live (not only deletion), add guards to:

- `addConnection(InletInstance, OutletInstance)` (`:696`) — today it allows
  **creating** connections while live.
- `addObjectInstance(IAxoObject, Point)` (`:311`) — restore the commented guard:
  replace `if (true) { /*!isLocked()) {*/` with `if (!isLocked()) {` and drop the
  `if (true)` (its else log says "add connection", it should say "add object").

### Side observation (out of scope)

The Edit → Lock/Unlock menu item is a **no-op**:
`jMenuItemLockActionPerformed` / `jMenuItemUnlockActionPerformed`
(`PatchFrame.java:995-1001`) have empty bodies with
`//getPatchView().Lock();` commented out. The lock is only managed
automatically by go-live/stop.

## Verification checklist (when implementing it)

1. Rebuild the jar: `cd axoloti && JAVA_HOME=../external/jdks/mac_arm64/*/ ant`,
   re-inject the aarch64 usb4java dylib
   (`zip Axoloti.jar org/usb4java/osx-aarch64/libusb4java.dylib`) and replace the
   jar in both bundles (back up the previous one first).
2. Manual test with the board:
   - [ ] Go live with a patch; try Delete/Backspace on a selected object → not deleted.
   - [ ] Edit → Delete → not deleted.
   - [ ] Iolet context menu on a connected iolet: "Disconnect" / "Delete net" → nothing happens.
   - [ ] Drag a cable and drop it outside → not disconnected.
   - [ ] Drag an object out (cut) → not deleted.
   - [ ] The patch keeps playing throughout (no resets/new underflows).
   - [ ] Stop the patch (or disconnect): all deletions work again.
   - [ ] Adding/moving objects while live behaves as before (move blocked, add
         blocked by `showClassSelector`) — nothing breaks.
3. Sanity: the INFO log `can't ...: locked` shows in the app console for each
   blocked operation.
