#!/usr/bin/env python3
"""Every control must actually change the picture.

A GLSL uniform whose name does not match the C++ is ignored without a word:
glGetUniformLocation returns -1 and glUniform on -1 is a documented no-op. So a
control can be wired to nothing while the plugin compiles, links, loads and
renders perfectly, and nothing in a build says anything. This is the only check
in the repo that stands between a typo and a shipped slider that does nothing.

    python3 tools/sweep.py [--size WxH] [--frames N] [--binary build/pftest]

Exit code 1 means something is dead.

------------------------------------------------------------------ the traps

**The card has to MOVE.** On a still picture this plugin provably renders flat
horizontal streaks -- that is the mechanism working, and it would make almost
every control here read as dead. `pftest`'s demo card moves, and everything in
it that moves travels at a known speed in source pixels per frame.

**The ring must not be full.** Fill and Background only differ where there are
columns that have not been written yet: Build puts the written ones at the left
and Scroll at the right, and Background decides what the rest shows. At the
default column rate the frame count below fills about a quarter of the strip,
which is what leaves the difference visible.

**A dropdown holds its element VALUE, not its display slot.** FFGL keeps the
two apart and only the value is ever stored, so `pftest --list` prints each
option's real range and this file sweeps every element rather than 0.0/0.5/1.0
-- otherwise the third entry of a three-entry list would never be tried.

**Never sweep the About block.** Those are buttons that open a web browser, and
sweeping them opens one tab per press. `pftest --list` marks them `about`.
"""

import argparse
import pathlib
import re
import subprocess
import sys
import tempfile
import zlib

ROOT = pathlib.Path(__file__).resolve().parent.parent

WIDTH, HEIGHT = 640, 360

# Long enough for the strip to be recognisably a strip and short enough that it
# is nowhere near full: at the default 4 columns a frame, 40 frames is 160 of
# the 640 columns a 640-wide picture holds.
FRAMES = 40

# What else has to be true for a control to mean anything. Empty on purpose --
# and that is a claim, not an omission: every control on this plugin reads on
# the default settings, because the default settings are a slit in the middle
# of a moving picture and that is the whole effect. A control added later that
# needs a context and does not get one will be reported dead here.
CONTEXT: dict[str, dict[str, float]] = {}

SKIP: dict[str, str] = {}


def parameters(binary):
    """id, name, kind, low, high, straight from the harness's own declaration."""
    out = subprocess.run([binary, "--list"], capture_output=True, text=True)
    if out.returncode != 0:
        print("could not list parameters:", out.stdout, out.stderr)
        sys.exit(1)

    found = []
    for line in out.stdout.splitlines():
        m = re.match(
            r"\s*(\d+)\s+(.+?)\s{2,}(\S+)\s+([\d.eE+-]+)\s+\[\s*([\d.eE+-]+)"
            r"\s*\.\.\s*([\d.eE+-]+)\s*\]",
            line,
        )
        if m:
            found.append((int(m.group(1)), m.group(2).strip(), m.group(3),
                          float(m.group(5)), float(m.group(6))))
    return found


def positions(kind, low, high):
    """Where to sample this control.

    An option is swept at every element value it has. Anything else gets both
    ends and the middle -- three rather than two, because a control can be a
    no-op at both ends and not in between, and Slit Angle is exactly that
    shape: its null is at 0.5 and its two halves are mirror images.
    """
    if kind == "option":
        return [float(v) for v in range(int(round(high)) + 1)]
    if kind == "boolean":
        return [low, high]
    return [low, (low + high) / 2.0, high]


def render(binary, path, size, frames, overrides):
    args = [binary, "--out", str(path), "--size", size, "--frames", str(frames)]
    for name, value in overrides.items():
        args += ["--set", f"{name}={value}"]
    result = subprocess.run(args, capture_output=True, text=True)
    if result.returncode != 0:
        print("render failed:", " ".join(args), result.stdout, result.stderr)
        sys.exit(1)
    return pathlib.Path(path).read_bytes()


def pixels(png):
    """Raw RGBA out of the harness's own PNG (filter 0 rows), so nothing else
    is a dependency."""
    i = 8
    idat = b""
    width = height = 0
    while i < len(png):
        length = int.from_bytes(png[i:i + 4], "big")
        kind = png[i + 4:i + 8]
        data = png[i + 8:i + 8 + length]
        if kind == b"IHDR":
            width = int.from_bytes(data[0:4], "big")
            height = int.from_bytes(data[4:8], "big")
        elif kind == b"IDAT":
            idat += data
        i += 12 + length
    raw = zlib.decompress(idat)
    stride = width * 4
    out = bytearray()
    for row in range(height):
        out += raw[row * (stride + 1) + 1:(row + 1) * (stride + 1)]
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--binary", default=str(ROOT / "build" / "pftest"))
    ap.add_argument("--size", default=f"{WIDTH}x{HEIGHT}")
    ap.add_argument("--frames", type=int, default=FRAMES)
    args = ap.parse_args()

    binary = args.binary
    if not pathlib.Path(binary).exists():
        print(f"{binary} is not built")
        return 2

    dead = []
    swept = 0

    with tempfile.TemporaryDirectory(prefix="pfsweep") as scratch:
        for pid, name, kind, low, high in parameters(binary):
            if kind == "about":
                continue
            if name in SKIP:
                print(f"  skip  {name}  ({SKIP[name]})")
                continue

            context = dict(CONTEXT.get(name, {}))
            digests = set()
            for i, value in enumerate(positions(kind, low, high)):
                overrides = dict(context)
                overrides[name] = value
                png = render(binary, f"{scratch}/{pid}_{i}.png", args.size,
                             args.frames, overrides)
                digests.add(bytes(pixels(png)))

            swept += 1
            alive = len(digests) > 1
            if not alive:
                dead.append(name)
            print(f"  {'ok' if alive else 'DEAD':4}  {name}")

    print()
    if dead:
        print(f"{len(dead)} control(s) changed nothing: {', '.join(dead)}")
        print("either the uniform name does not match the shader, or the sweep")
        print("needs a CONTEXT entry saying what else has to be true.")
        return 1

    print(f"all {swept} swept controls measurably change the picture")
    return 0


if __name__ == "__main__":
    sys.exit(main())
