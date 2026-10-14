# Objref attributes inside user-defined helper functions

**Fixes:** objects that declare their own helper member functions in
`code.declaration` and use an `objref` attribute inside them (community
object `gao/sample/beatslicer2`, its demo patches, and any object following
the same pattern) crashing the C++ compiler with:

```
error: 'parent' was not declared in this scope
    parent->instanceslicesbuf_i.array[si] = __SSAT(s,28)>>parent->instanceslicesbuf_i.GAIN;
```

Second blocker found while verifying the same patch: patches calling
`log()` failed at **link** time with `undefined reference to 'logf'`
(GCC 15 toolchain fallout, see §2).

---

## §1 — `'parent' was not declared in this scope`

### Symptom

Compiling `akso-community/patches/gao/Beatslicer_Demo2.axp` (uses
`gao/sample/beatslicer2` + `beatplayer2` with objrefs to the root-level
`slicesbuf`/`slicesind` tables):

```
/Users/gustavo/Library/Akso/build/xpatch.cpp: In member function
    'void rootc::instancebeatslicer2__1::insert_twoframe()':
xpatch.cpp:9416:17: error: 'parent' was not declared in this scope
xpatch.cpp:9424:17: error: 'parent' was not declared in this scope
xpatch.cpp: In member function 'void rootc::instancebeatslicer2__1::fadeout(uint32_t)':
xpatch.cpp:9443:22: error: 'parent' was not declared in this scope
xpatch.cpp:9449:15: error: 'parent' was not declared in this scope
```

Exactly 4 errors: 2 in `insert_twoframe()`, 2 in `fadeout()` — and **none**
in the same object's `dsp()`, although it also uses the same attributes.

### Root cause

`AttributeInstanceObjRef.CValue()` expands every `objref` attribute to
`parent->instance<i>_i…` (one `parent->` per `../` level plus one — this
behaviour is byte-identical to upstream Axoloti master; our fixes
`d592654`/`3b5dcef` only changed the `../`-inside-subpatcher path). The
identifier `parent` exists in the generated code **only as a function
parameter** of the standard member functions:

```cpp
public: void Init(rootc * parent, …)   // ← parent exists here
public: void dsp (rootc * parent, …)   // ← and here
void MidiInHandler(rootc *parent, …)   // ← and here
```

`beatslicer2` defines its **own** member functions in `code.declaration`:

```cpp
void insert_twoframe(void) { … attr_slicesbuffer.array[si] … }   // no parent param
void fadeout(uint32_t cuepoint) { … attr_slicesindex.LENGTH … }  // no parent param
```

There the expansion `parent->instanceslicesbuf_i` has no `parent` in scope →
compile error. The object was simply incompatible with the codegen (upstream
and fork alike); it is not a regression of the objref fix series.

### Fix

Generalise the back-pointer emitted since `3b5dcef` (which was conditional
on `AxoObjectPatcher`) to **every** object instance class in
`axoloti/src/main/java/axoloti/codegen/patch/object/AxoObjectInstanceCodegenView.java`:

```cpp
// generateClass(): every instance class now gets
<EnclosingClass> *parent;
// generateInitCodePlusPlus(): and in Init()
this->parent = parent;
```

- `Init`/`dsp`/`krate` still receive `parent` as a parameter (it shadows the
  member — legal C++, semantics unchanged).
- User helper functions now resolve `parent` through the member → the whole
  class of "helper functions + attr_" objects compiles.
- Subpatchers keep the exact `3b5dcef` behaviour (they are instances too).
- Polyphonic voices: instances inside the `voice` class get
  `voice *parent;` (`classname` is `"voice"` in both the member type and the
  `Init(voice * parent …)` signature), and the `d592654` chain
  `parent->common->parent->…` still resolves because the poly patcher
  instance keeps its own `parent` member (`3b5dcef`).

### Collision audit (why this is safe)

An object whose `code.declaration` already declares a member named `parent`
would now collide with the emitted one. Audited factory + community
libraries:

- `tiar/fx/nestVerb.axo` declares `parent` members — all **inside its
  hand-written embedded sub-classes** (`instancenested1/2/3 …`), never at the
  outer instance scope → no collision.
- `util/format`, `spectral/rfft 128`, `script/script2` use `attr_parent` only
  as a **type cast** (`((attr_parent *)arg)->…`), never as a member
  declaration → no collision.

## §2 — `undefined reference to 'logf'`

While verifying §1 by hand-patching `xpatch.cpp`, the link failed:

```
xpatch.o: in function 'rootc::dsp()':
  undefined reference to `logf'
  (logf): Unknown destination type (ARM/Thumb)
```

### Root cause

- `beatslicer2` computes `log(energy*15625000+1)*4.34294482f`.
- With `-O3 -fno-math-errno`, GCC **15** replaces the
  float→double→`log`→double→float round-trip with a direct `logf` call.
- The board firmware (`axoloti.elf`, CRC `3238BAE5`) exports `log` (double)
  but **not** `logf`, and patches link with
  `--just-symbols=<firmware>/build/axoloti.elf` — so `logf` resolves to
  nothing.
- The patch link rule never linked `libm`.

This is fallout of the arm64 patch-compiler migration
([`PATCH-COMPILER-ARM64.md`](PATCH-COMPILER-ARM64.md)), not of §1: any patch
whose `log/exp/pow`-style call gets rewritten to the float variant hits it.

### Fix

Added `-lm` to the patch link rule in `axoloti/firmware/Makefile.patch`
(applied to the working tree **and** to both bundles):

```make
%.elf: %.o
	@$(LD) $(LDFLAGS) "$<" -lm ${MODULE_LINKDIR} …
```

Only the needed symbols are pulled in (newlib `libm.a` ships with the GCC
15.3 toolchain).

---

## Verification

Hand-patched `xpatch.cpp` (member + init added to
`class instancebeatslicer2__1`, simulating the new codegen output) compiled
**and linked** with the bundle's arm64 toolchain — before the fix: 4
`parent` errors at compile; after §1: clean compile; after §1+§2:

```
Memory region         Used Size  Region Size  %age Used
            SRAM:       48632 B       384 KB     12.37%
           SDRAM:     1507360 B        32 MB      4.49%
done /Users/gustavo/Library/Akso/build/xpatch
```

In-app verification done (2026-10-13): `Beatslicer_Demo2.axp` compiles with
the new jar. Published as release **`v3.0.4.1-arm64`**
(`Akso-3.0.4.1-hotfix.zip`: rebuilt `Axoloti.jar` + `firmware/Makefile.patch`
+ install instructions; applies over 3.0.2–3.0.4). The jar is deployed to
both bundles (`Axoloti.jar.bak-preparentmember` kept in each).

## Files changed

| File | Change |
|---|---|
| `axoloti/src/main/java/axoloti/codegen/patch/object/AxoObjectInstanceCodegenView.java` | Emit `<EnclosingClass> *parent;` + `this->parent = parent;` for **every** instance (was: patchers only) |
| `axoloti/firmware/Makefile.patch` | `-lm` in the `%.elf` link rule (also in both bundles) |

Both are app-side: **no firmware reflash required** (the board keeps
`3238BAE5`; `-lm` only adds symbols to the patch image).
