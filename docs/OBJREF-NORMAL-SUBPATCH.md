# Objref resolution across normal subpatcher boundaries

**Fixes:** any object with an `objref` attribute (e.g. `table/read`,
`table/play`, `tabrecord2~`, `table/load`) inside a **normal**
(not polyphonic) subpatcher that references an object in the enclosing
patch via `../tablename`, crashing the C++ compiler with
`has no member named 'parent'`.

This is the non-polyphonic sibling of
[`OBJREF-POLYPHONIC-VOICE.md`](OBJREF-POLYPHONIC-VOICE.md). A third,
smaller fix (`d592654`) covered `patcher/voice`; this one covers plain
`patcher` subpatchers.

---

## Symptom

Compiling a patch where an object inside a normal subpatcher references
a table (or another object) in the enclosing patch via `../data`:

```
/Users/…/build/xpatch.cpp:925:20: error: 'class rootc::instancepatcher__1'
  has no member named 'parent'
  925 | outlet_v0= parent->parent->instancedata_i.array[
      |                    ^~~~~~
```

The error repeats for every usage of `instancedata_i` inside the
subpatcher (8 lines per `table/read`, 3 reads → 48 errors in the
reported patch).

---

## Root cause

`AttributeInstanceObjRef.CValue()` turns each `../` level of an object
reference into a `parent->` step:

```java
String o2 = "parent->";
while (o.length() > 3 && o.substring(0, 3).equals("../")) {
    o2 += "parent->";
    o = o.substring(3);
}
```

So `../data` inside a subpatcher compiles to
`parent->parent->instancedata_i`:

- the **first** `parent` is the `dsp`/`Init` parameter, i.e. the
  subpatcher instance itself (`instancepatcher__1*`);
- the **second** `parent` is meant to reach the patcher that *contains*
  `instancepatcher__1` (the root patcher, `rootc`).

The problem: the generated class for a **normal** subpatcher never
stored that enclosing-patcher pointer. Its `Init(rootc *parent)` method
received it and threw it away:

```cpp
class instancepatcher__1 {
  // … PExch, nested object classes/instances …
  public: void Init(rootc * parent) {
    // no "this->parent = parent;" here
  }
  public: void dsp (rootc * parent, …) { … }
};
```

Hence `instancetableread__1::dsp(instancepatcher__1 * parent, …)` could
not climb to `rootc` and the C++ compiler rejected `parent->parent`.

### Why `d592654` did not cover it

`d592654` handled only the **polyphonic** case: there the object lives
inside a `voice` class that has no `parent` at all — it has `common`
pointing to the polyphonic patcher instance — so the first `../` must be
rewritten to `parent->common->`. As a stop-gap it also hardcoded a
`rootc *parent` member (and its init) into `generatePolyCode()`.

A normal subpatcher has a different `parent` pointer *name* (`parent`,
not `common`) but still needed the missing member. Same bug, different
subpatch mode.

---

## Fix

Generalised the enclosing-patcher back-pointer in the base object
codegen (`AxoObjectInstanceCodegenView`), for **every** `AxoObjectPatcher`
instance (normal, polyphonic, polychannel, polyexpression, at any
nesting depth):

1. **Declaration** (`generateClass()`):

   ```cpp
   <EnclosingClass> *parent;
   ```

2. **Initialisation** (`generateInitCodePlusPlus()`):

   ```cpp
   this->parent = parent;
   ```

The member type is the real enclosing class name passed to
`generateClass()` (e.g. `rootc`, or `instancepatcher__2` for a deeper
nesting) instead of the hardcoded `rootc`.

The now-redundant hardcoded `rootc *parent` field and
`this->parent = parent;` init were removed from
`PatchViewCodegen.generatePolyCode()`.

```cpp
// generated for a normal subpatcher at the root
class instancepatcher__1 {
  // … nested classes/instances …
  rootc *parent;
  public: void Init(rootc * parent) {
    this->parent = parent;
    // …parameter init…
  }
};
```

Net effect: `parent->parent->instancedata_i` inside
`instancetableread__1::dsp` now resolves, and multi-level `../../…`
references keep working because every level stores its own correctly
typed pointer.

---

## Files changed

| File | Change |
|---|---|
| `axoloti/src/main/java/axoloti/codegen/patch/object/AxoObjectInstanceCodegenView.java` | Emit `<EnclosingClass> *parent;` and `this->parent = parent;` for every `AxoObjectPatcher` instance |
| `axoloti/src/main/java/axoloti/codegen/patch/PatchViewCodegen.java` | Drop the hardcoded `rootc *parent` field + init from `generatePolyCode()` (now generic) |

Both are **Java** — only `Axoloti.jar` needs to be rebuilt (no firmware
flash required). Commit: `3b5dcef`.

---

## Verification

The failing `xpatch.cpp` was hand-patched (`rootc *parent;` +
`this->parent = parent;` on `instancepatcher__1`) and compiled and linked
cleanly with the arm64 patch toolchain:

```
Memory region         Used Size  Region Size  %age Used
            SRAM:       26732 B       384 KB      6.80%
           SDRAM:      426112 B        32 MB      1.27%
done /Users/…/build/xpatch
```

The packaged fix was then verified in the same way after rebuilding and
deploying `Axoloti.jar`.

| Before | After |
|---|---|
| `parent->parent->instancedata_i` → compile error | `parent->parent->instancedata_i` → compiles and works |
