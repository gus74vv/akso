# Objref resolution across polyphonic voice boundaries

**Fixes:** `play` and `tabrecord2~` (any object with an `objref`
attribute) inside a `patcher/voice` subpatcher crashing the C++ compiler
with `has no member named 'parent'`.

---

## Symptom

Compiling a patch where an object inside a polyphonic (`patcher/voice`)
subpatcher references a table (or another object) in an enclosing scope
via `../../tablename`:

```
/Users/…/build/xpatch.cpp:587:31: error: 'class rootc::instancepatcher__1::voice'
  has no member named 'parent'
  587 |  pos = asat>>(27-parent->parent->instancet_i.LENGTHPOW);
      |                         ^~~~~~
```

The error repeats for every usage of `instancet_i` inside the `voice`
class (up to 8 redundant lines per object).

---

## Root cause

Two independent problems, both in the C++ code generator
(`axoloti/src/main/java/axoloti/codegen/…`):

### 1. Wrong parent pointer name inside the voice class

When an object reference uses `../` to refer to an object in an enclosing
patcher, `AttributeInstanceObjRef.CValue()` prepends `parent->` for each
level of `../`. For a normal subpatcher this is correct: the subpatcher
inherits from a struct that includes the parent's members, and `parent`
points to the enclosing patcher instance.

But a **polyphonic** subpatcher generates a `voice` class that does
**not** have a `parent` member. Instead, the `voice` class has a `common`
pointer that points to its containing polyphonic patcher instance.

The original code had a **commented-out** FIXME block that recognised this:

```java
/* FIXME: object references to object in parent patch...
if (o.substring(0,3).equals("../") && subpatchmode == polyphonic) {
  o2 = o2 + "common->";
}
*/
```

It was never activated.

| Scope | Parent access |
|---|---|
| Normal subpatcher | `parent->` |
| Polyphonic `voice` | `parent->common->` |

### 2. No back-link from the polyphonic patcher to the root

Even after fixing problem #1, the generated expression
`parent->common->parent->instancet_i` failed because the polyphonic
patcher class (`instancepatcher__1`) itself has no `parent` member.

The polyphonic patcher is a nested class of the root patcher (`rootc`),
but it never stored a reference back to the root. When a voice object
navigated two levels up (`../../table`), the chain broke:
- `parent` = `voice*`
- `parent->common` = `instancepatcher__1*` (the polyphonic patcher)
- `parent->common->parent` = **does not exist**

---

## Fix

### `AttributeInstanceObjRef.CValue()`

Uncommented and corrected the FIXME logic at
`axoloti/src/main/java/axoloti/patch/object/attribute/AttributeInstanceObjRef.java`.

When the object is inside a polyphonic / polychannel / polyexpression
subpatcher, the first `../` now resolves to `parent->common->` instead
of `parent->parent->`.

```java
// Before (broken for polyphonic):
if (o.equals("../"))  o2 = "parent->";
while (/* more ../ */) o2 += "parent->";

// After:
if (o.equals("../") && polyphonic)  o2 = "parent->common->";
while (/* more ../ */) o2 += "parent->";
```

### `PatchViewCodegen.generatePolyCode()`

Added two changes at
`axoloti/src/main/java/axoloti/codegen/patch/PatchViewCodegen.java`:

1. **Field declaration** — a `rootc *parent` member is added to the
   polyphonic patcher class (`instancepatcher__1`), just before the
   `voice` class definition.

2. **Initialisation** — the `Init(rootc *parent)` method of the polyphonic
   patcher now saves the received pointer: `this->parent = parent;`

This makes `parent->common->parent->instancet_i` resolve correctly:
- `parent` = `voice*`
- `parent->common` = `instancepatcher__1*`
- `parent->common->parent` = `rootc*` (the root patcher)
- `parent->common->parent->instancet_i` = the table in the root

---

## Files changed

| File | Change |
|---|---|
| `axoloti/src/main/java/axoloti/patch/object/attribute/AttributeInstanceObjRef.java` | Activate the polyphonic FIXME in `CValue()` |
| `axoloti/src/main/java/axoloti/codegen/patch/PatchViewCodegen.java` | Add `rootc *parent` field + init assignment in `generatePolyCode()` |

Both are **Java** — only `Axoloti.jar` needs to be rebuilt (no firmware
flash required).

---

## Verification

The example patch `granular_verb.axp` (shipped with the patcher)
compiles and runs without error. Any patch using `play` or `tabrecord2~`
(or any object with an `objref` attribute) inside a `patcher/voice` that
references a table in an enclosing scope is fixed.

| Before | After |
|---|---|
| `parent->parent->instancet_i` → compile error | `parent->common->parent->instancet_i` → compiles and works |