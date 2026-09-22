# photofinish

A strip camera as an FFGL **effect** for Resolume Arena/Avenue: one column of
sensor, with the film moving past it, so the picture's scan axis is **time**.
C++/GLSL, CMake MODULE → universal `.bundle` (macOS) + Windows `.dll`. MIT.

Read `AGENTS.md` before changing the ring, the column schedule, or anything
that mentions time.

## Commands (CMake)
- Configure: `cmake -B build -DCMAKE_BUILD_TYPE=Release`
- Fast dev build: add `-DCMAKE_OSX_ARCHITECTURES=arm64`
- Build: `cmake --build build --parallel`
- Install into Arena: `cmake --install build`
- Render a frame offline: `./build/pftest --out /tmp/f.png --size 1920x1080 --frames 340`
- Set anything by name: `--set "Time Per Column=0.2" --set "Axis=1"`
- List parameters, with their kinds and real ranges: `./build/pftest --list`
- The demo card on its own: `./build/pftest --card /tmp/card.png`

## Verify
- Everything: `tools/verify.sh` (fresh **universal** Release build + every
  check + the sweep + plist, codesign and oxbow, ~15 s)
- Every check in one process: `./build/pftest --schedule --static --ring --clock
  --interp --width --matched --reverse --sync --negative`
- Or through ctest, one per check: `ctest --test-dir build --output-on-failure`
- No dead controls: `python3 tools/sweep.py` (`--size WxH`, `--frames N`)
- The cost: `./build/pftest --bench --frames 200`, and at the fastest column
  rate: `--set "Time Per Column=0"`

Each check, one line each:

| | what it proves |
|---|---|
| `--schedule` | when a column is taken. **No GL at all** — the one check a machine with no context can still run |
| `--static` | a still picture is exact horizontal streaks. **Bitwise** |
| `--ring` | the strip holds the last min(N, L) columns, in order. **Bitwise** |
| `--clock` | the same take from t=0 and from 499,217,238 ms. **Bitwise** |
| `--interp` | a column between two frames is their blend, to one code value |
| `--width` | a moving bar's rendered width against `b·c/v`, to one column |
| `--matched` | at the film speed, the object's own proportions, in absolute pixels |
| `--reverse` | the other way round comes out mirrored, measured as skewness |
| `--sync` | one whole sweep per bar, and per beat |
| `--negative` | every check above, perturbed, asserted to **fail** |

## Notes
- **Nothing is warped, blurred or time-displaced on purpose.** One column is
  sampled per tick and pushed onto a ring; the streaks, the stretch by 1/speed
  and the backwards runner all fall out of that. A term whose job is to produce
  one of those artefacts means the mechanism has been lost.
- **Time is frame-relative, always.** Resolume's clock overflows a float
  (measured at 499,217,238 ms, where a float resolves 0.03 s). Only differences
  of the host's clock are used, every phase is a `double` in 0..1, and the only
  time-like number that reaches a shader is `Blend`. `--clock` is the guard.
- **`n = floor( x + kColumnSnap )`, and the snap is 1e-6 of a column.** Sized
  against the clock's *origin*, not against a frame period. 1e-9 fails
  `--clock`. See `Strip.h`.
- **The ring is `GL_NEAREST` and the strip pass uses `texelFetch`.** A column
  is one instant; a filtered read between two of them is a picture of a moment
  nobody sampled.
- **`ScopedFBOBinding` does not restore the viewport**, and the slit pass's
  viewport is ONE PIXEL WIDE. `ProcessOpenGL` captures and restores the host's.
- **Every `Ensure()` happens before anything binds a texture** — `FFGLFBO`
  allocation unbinds the active texture unit and only on the frame that
  allocates.
- **GLSL `%` and `/` are undefined on negative operands**, and float `mod` on a
  ring index returns the modulus instead of zero where the division rounds low.
  Both operands in the strip pass are non-negative integers.
- **Reserved GLSL words**: `patch sample input output filter common active half
  layout flat`. A shader that will not compile is `InitGL FAILED`, a black clip
  in the host, and one line in the diagnostics log.
- **Parameter names must be unique** — `--set` and the sweep find them by name.
- **A dropdown stores its element VALUE, not its display slot.** Lists are
  declared alphabetically and every entry keeps the value it shipped with;
  `Sync` and `Interpolate` are exempt because position is their meaning.
- `SetParamInfo` clamps a STANDARD default into 0..1 before a range can be
  attached, so every ranged control is 0..1 and `Controls.cpp` converts.
- Override `SetTextParameter` to return `FF_SUCCESS` for the About block, or no
  host can instantiate the plugin at all.
- `photofinish_core` is an OBJECT library, not STATIC — the plugin registers
  itself from a file-scope constructor nothing references by name.
- macOS build must be universal. Verify with `lipo`, never the build log.
- FFGL id is `PF01`, type `effect`, name `SW Photofinish`.
- Test hooks: `SetClockScaleForTest( 1.0 )` declares seconds;
  `SetColumnPeriodForTest( s )` sets an exact column period, which a geometric
  slider cannot land on.

## Not done yet
- Not yet loaded into Resolume, and not installed anywhere.
- No release tag, no GitHub repo, not registered on the website.
- `StoatworksAbout*.h` and `ATTRIBUTIONS.md` are provisional hand copies, with
  `guide=""` because no user guide exists.
- No factory presets, no bar-line lock, no OpenFX port, no browser demo.
- Never run on Windows, on Intel, or on a GPU-less rasteriser.

## Diagnostics

`source/Diag.{h,cpp}` — log file only, no crash handler (this runs inside
Resolume).

    ~/Library/Logs/photofinish/photofinish.YYYY-MM-DD.log       (macOS)
    %LOCALAPPDATA%\photofinish\logs\photofinish.YYYY-MM-DD.log  (Windows)

It records the build, the GL driver, which shader failed to compile if one
did, what unit the host's clock turned out to be in, and the ring's shape every
time it is rebuilt. The last two are invisible from the picture and both change
what every column means.
