# object-fixes — patched factory/community objects

This folder ships **object (`.axo`) fixes** that are applied on the maintainer's machine but
**do not reach users automatically**. The people who build the `gus74vv/akso` fork need these
objects patched to match this fork's firmware/board.

## Why this exists

On first run the patcher clones its object libraries from the **upstream** repos, not from
this one (`axoloti/src/main/java/axoloti/preferences/Preferences.java`, `resetLibraries()`):

| library | remote the app clones | local dir |
|---|---|---|
| factory | `https://github.com/zrna-research/akso-factory.git` | `~/Library/Akso/akso-factory/` |
| community | `https://github.com/zrna-research/akso-community.git` | `~/Library/Akso/akso-community/` |

We never push to those upstream repos, and this fork does not ship its own object repos, so
these fixes would otherwise be stranded on one machine. Keep them here and apply them after
the app has cloned the libraries.

The directory layout below mirrors the library paths (relative to
`~/Library/Akso/<library>/`), so applying is a straight copy.

## Contents

| object | library | change | kind |
|---|---|---|---|
| `objects/script/script.axo` | akso-factory | Removed the erroneous `static` from the inner thread function `ThreadX2` (`ThreadX` stays `static`). Without this, any script that writes the object outlets `out1`/`out2` from its thread fails to compile: `invalid use of member ... in static worker function`. | fix (replace) |
| `objects/script/script2.axo` | akso-factory | Same `static` fix on `ThreadX2`. It compiled by accident before (user code runs in non-static `setup()`/`loop()`), but now it is aligned with upstream. | fix (replace) |
| `objects/midi/out/cc thin slow.axo` | akso-factory | Rate-limited MIDI CC output for the USB host (part of the host MIDI-out throughput fix). Does not exist upstream. | **new (copy in)** |
| `objects/jt/granular/graindelay.axo` | akso-community | Removed direct STM32 `DMA2D` register access used for the grain-buffer copy (not valid on the Akso board). | fix (replace) |
| `objects/jt/granular/graintable.axo` | akso-community | Same `DMA2D` removal. | fix (replace) |
| `objects/jt/granular/grainverb.axo` | akso-community | Same `DMA2D` removal. | fix (replace) |

Backups (`*.bak-*`) are intentionally **not** tracked here; only the patched `.axo`.
Background on the `script` fix: `docs/FIX-SCRIPT-OBJECT-STATIC-2026-10-21.md`.

## How to apply (macOS)

From the root of this repo, after the app has cloned the libraries on first run:

```sh
# factory objects
rsync -av object-fixes/akso-factory/objects/ "$HOME/Library/Akso/akso-factory/objects/"

# community objects
rsync -av object-fixes/akso-community/objects/ "$HOME/Library/Akso/akso-community/objects/"
```

`rsync` overwrites only the files present in this folder. Alternative, per object:

```sh
cp "$HOME/dev/akso/object-fixes/akso-factory/objects/script/script.axo" \
   "$HOME/Library/Akso/akso-factory/objects/script/script.axo"      # adjust your repo path
```

Then **restart the patcher** (or reload the library) so the new `.axo` is re-read.

## ⚠️ A library Sync/Upgrade can revert these

The patcher's library sync (`AxoGitLibrary.sync()` → `checkout()`) **force-resets a dirty
working tree when no credentials are configured** ("unauthorised changes, resetting"). If a
user (or you) runs **Sync/Upgrade** on the factory/community library, these patched files
can be overwritten by the pristine upstream versions. After any library sync, re-apply the
copies above.

The libraries are on branch `3.0.1`.
