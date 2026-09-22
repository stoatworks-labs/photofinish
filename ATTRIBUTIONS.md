# Attributions

Photofinish is built on other people's work. This file lists what that work is,
who did it, and what it is doing here.

> **Provisional.** Across the fleet this file is generated from master lists in
> `stoatworks-backend` by `scripts/sync-attributions.py`. Photofinish is not
> registered there yet, so this copy is hand-written. Register it before release
> — and note that the script's `--only` flag truncates the file rather than
> filtering it.

## Third-party code this project uses

### Resolume FFGL SDK

<https://github.com/resolume/ffgl>
Licence: BSD-3-Clause
Copyright: FreeFrame

Vendored as a git submodule at `external/ffgl`, pinned to `b1afaf9`.

The plugin ABI itself. An FFGL effect is defined by this SDK's headers — there is
no other way to be loadable by Resolume Arena and Avenue.

### GLEW — the OpenGL Extension Wrangler Library

<https://github.com/nigels-com/glew>
Licence: BSD-3-Clause (with Mesa 3-D and Khronos components)
Copyright: Milan Ikits, Marcelo E. Magallon and Lev Povalahev

Windows only, from vcpkg, statically linked. The SDK's headers pull it in for the
OpenGL function pointers; macOS uses the system OpenGL framework instead.

### zlib

<https://zlib.net>
Licence: zlib
Copyright: Jean-loup Gailly and Mark Adler

Linked from the system, by the offline harness only, so that `pftest --out` can
write a PNG in fifty lines instead of vendoring an image library. It is not in
the plugin.

## Work from elsewhere in the fleet

### tinsel

<https://github.com/stoatworks-labs/tinsel>
Licence: MIT
Copyright: Stoatworks Labs

`source/PassBuffer.{h,cpp}` and `source/Diag.{h,cpp}` are copied from tinsel with
only their header comments changed. They work around two defects in the FFGL SDK
— `FFGLFBO::Release()` leaking the colour texture, and every `ffglex::Scoped*`
binding clearing to 0 on scope exit rather than restoring — and those defects are
the same everywhere, so a second implementation would be a second thing to get
wrong.

### The About block

`source/StoatworksAbout*.h` come from `stoatworks-backend/about`. They are
vendored into every plugin in the fleet by `scripts/sync-about.py`; this copy is
**hand-written and provisional**, because Photofinish is not registered in the
backend yet.

## Prior art, not code

The effect is a **strip camera** — a photo-finish camera, a slit-scan camera, a
rollout camera. Nothing was ported from anywhere; the idea is a hundred years old
and the implementation of it here is one ring buffer.
