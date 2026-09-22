# photofinish — orientation for another LLM (or a newcomer)

**What it is:** an FFGL 2.1 effect for Resolume Arena/Avenue that photographs a
clip the way a photo-finish camera photographs a race — one column of sensor,
with the film moving past it. C++17 + GLSL 4.10, CMake, universal macOS
`.bundle` and a Windows `.dll`. MIT. Intended home
`github.com/stoatworks-labs/photofinish`.

`CLAUDE.md` is the command reference — build, render, verify. This file is the
*why*: read it before touching the ring, the column schedule, or anything that
mentions time.

---

## The one idea

**A photo-finish camera has no two-dimensional frame. It has one column of
sensor, and the film moves past it. So the horizontal axis of the picture is
TIME, NOT SPACE.**

Everything the effect is known for is a consequence of that single
substitution, and none of it is coded anywhere:

- anything **static** becomes a horizontal streak, because it is the same
  column written over and over;
- anything **moving** is stretched or squashed by 1/its speed, because the
  faster it crosses the slit the fewer columns it occupies;
- an object crossing at exactly the film speed comes out with its **true
  proportions** — which is why the winner looks normal and the crowd behind is
  smeared;
- an object crossing the *other way* comes out **reversed**, the famous
  backwards runner.

There is exactly one relation underneath all four. An object `b` source pixels
wide, crossing the slit at `v` source pixels per frame, photographed at `c`
columns per frame, occupies

    b · c / v   columns

and the four bullets are that expression at `v = 0`, at general `v`, at `v = c`
and at `v < 0`.

### The discipline

Nothing is warped, blurred or time-displaced on purpose. One column is sampled
per tick and pushed onto a ring, and everything above falls out. **If a term
ever appears whose job is to produce one of those artefacts, the mechanism has
been lost and the term is the evidence.**

Two places where that pressure was real and the answer was to leave it alone:

- **`Slit Width` looks like a blur control and is not.** A slit wider than one
  pixel averages the picture *across the direction of travel*, and because the
  scan axis is time, that average is over the time an object takes to cross the
  slit. It is motion blur in the time axis, arrived at rather than added. It
  does nothing at all to the other axis, which is the giveaway that it is not a
  blur: a real blur control would soften both.
- **A magnified strip is blocks, not a gradient.** At a `Sweep Length` shorter
  than the picture's axis, one ring column covers several output pixels. The
  obvious thing is to interpolate between adjacent columns, and it is wrong:
  two adjacent columns are two *instants*, and a blend of them is a picture of
  a moment nobody sampled. The strip pass uses `texelFetch` and the ring is
  `GL_NEAREST`. A side effect is that `pftest --static` can be a bit-for-bit
  claim at every Sweep Length rather than only at 1:1 — which is how you can
  tell the decision was right rather than merely defensible.

---

## The traps

Ordered by how much time they will cost you.

**The clock overflows a float, and this plugin is a clock with a picture
attached.** Resolume's `SetTime` has been measured at 499,217,238 ms into a
session. A 32-bit float's spacing there is about 0.03 s — larger than a frame —
so any phase computed from absolute time stops resolving anything and the
plugin quietly stops producing columns at an even rate some hours into a show.
Nothing about that shows up in a test that starts at t = 0, which is every test
anybody writes. So: the host's clock is used for the DIFFERENCE between two
frames and for nothing else, every phase is a `double` in 0..1 by construction,
and the only time-like number that reaches a shader is `Blend`, which is also
in 0..1. `pftest --clock` renders the same take from t = 0 and from
499,217,238 ms and requires the two strips to be identical bit for bit. Keep
that check. It is the cheapest insurance in the repo.

**`floor` on an exact-looking number drops a column, once, for ever.** The
harness drives `SetTime( frame / fps )`, so `dt` is the difference of two exact
quotients: `3/60 - 2/60` comes out one ulp *below* `1/60`. With a column period
of exactly one frame — the single most likely setting an operator picks — the
accumulated phase is 0.9999999999999998, `floor` says no column, the next frame
recovers it, and the strip is permanently one column behind where it should be.
It is invisible in a picture. It broke `--ring` on the first run. The fix is a
snap: `n = floor( x + kColumnSnap )`.

**The snap has to be sized against the clock's ORIGIN, not against 1/60.** The
first value tried was 1e-9 and `--clock` failed with it. At 499,217,238 ms a
double's spacing is about 1.1e-10 s, so a frame difference taken there carries
a couple of ulps of error — roughly 1.3e-8 of a column period at 60 fps, which
is *ten times larger* than a 1e-9 snap. `kColumnSnap` is 1e-6 of a column: half
a nanosecond at the fastest column rate, twelve orders of magnitude below
anything the effect can represent, and four orders above the clock's own noise
floor at the far end of a long session. The remainder carried forward is
`x - floor( x + snap )` and is allowed to go very slightly negative, which is
what keeps the snap from throwing away a sliver of phase on every frame.

**`ScopedFBOBinding` does not restore the viewport.** It restores the
framebuffer binding and only that (SDK `b1afaf9`, `FFGLScopedFBOBinding.cpp`).
The slit pass sets a viewport ONE PIXEL WIDE, so without an explicit restore
the strip pass — which draws to the host's own framebuffer and has no buffer of
its own to size itself from — would render a single column of the frame and
leave the rest untouched. `ProcessOpenGL` captures the host viewport up front
and restores it before the strip pass.

**Every `ffglex::Scoped*` binding clears to 0 on scope exit — it does not
restore.** `FFGLFBO::Initialise` sizes its new colour texture under one of
those, so *allocating a buffer silently unbinds your input texture from the
active unit*. The symptom is the dangerous part: correct on every frame except
the one that allocates. Every `Ensure()` happens before anything binds a
texture.

**Reallocating a buffer CLEARS it, and one of ours is the previous frame.**
When the composition changes resolution, both frame copies are reallocated --
and a reallocated `PassBuffer` is cleared on purpose, because a buffer whose
contents are undefined is not "a bit of noise", it is whatever texture memory
the driver handed back. So on that one frame the copy that held the *previous*
frame is suddenly black, and every column taken in it is blended towards black:
at the fastest column rate, a third of the ring, once, reading as a dark band
nobody can account for. `EnsureBuffers` tracks the picture size itself and
clears `framesSeeded` when it moves, which makes the plugin copy the incoming
picture into BOTH buffers for that frame. `pftest --resize` is the guard, and
the audit is what found it -- nothing else would have.

**`ffglex::FFGLFBO::Release()` leaks the colour texture.** It deletes the
framebuffer and the depth renderbuffer, then tests `depthBufferID` a second
time where it plainly meant `colorTextureID`. `PassBuffer::Destroy()` deletes
it first. It matters here rather than being pedantry: the ring is a full
picture-sized RGBA8 buffer and it is reallocated every time the operator moves
`Sweep Length`.

**`FFGLScopedFBOBinding.h` is not in the umbrella header.** `FFGLSDK.h`
includes every other scoped binding and omits that one. Include it by hand; the
symptom is an unknown-type error on `ScopedFBOBinding` and nothing else.

**A float `mod` is not safe for a ring index.** GLSL defines `mod` as
`x - y * floor( x / y )`; where the subtraction lands on an exact multiple, the
division can round a hair below the integer, `floor` takes it down a whole
step, and the result comes back as `y` rather than zero. On a ring that is the
newest column appearing at the oldest end for one frame in a few thousand. The
strip pass uses integer `%` with both operands non-negative — which also keeps
clear of GLSL leaving `%` and `/` undefined on negative operands.

**A ranged parameter cannot have a ranged default.** `SetParamInfo` clamps an
`FF_TYPE_STANDARD` default into 0..1 *before* returning, and `SetParamRange`
can only be called afterwards. So every ranged parameter here is 0..1 and the
conversions live in `Controls.cpp`.

**Override `SetTextParameter` to return `FF_SUCCESS` for the About block.**
`instantiateGL` pushes every declared default back through the setters and
deletes the instance the moment one fails, and `CFFGLPlugin`'s stub is exactly
that failure. Omit it and the plugin cannot be created in any real host while
every in-repo check still passes.

**The plugin registers itself from a file-scope constructor.**
`CFFGLPluginInfo` is never referenced by name, so in a **STATIC** archive the
linker may drop the whole translation unit, giving a bundle that loads, exports
`plugMain`, and reports that it contains no plugins. The core is an **OBJECT**
library for that reason.

**The harness needs a synthetic clock, and this plugin needs it more than most.**
Left to the wall clock the harness renders a hundred frames in a few
milliseconds, so almost no time passes and almost no columns are produced —
the strip comes out empty and every control reads as dead. `oxbow selftest`
shows exactly this from the outside: it renders 120 frames as fast as it can
and does not drive `SetTime`, so its "lit pixels: 0.3%" is the plugin being a
clock, not the plugin being broken.

**The harness declares the clock's UNIT rather than letting the plugin infer
it.** `SetClockScaleForTest( 1.0 )` says "seconds" outright. An absolute time
handed over in a single frame is genuinely ambiguous, and an implicit unit is
what let the milliseconds-or-seconds bug through elsewhere in the fleet. The
inference path (votes over several frames against a steady clock) still exists
for real hosts and is what a host that sends milliseconds needs.

**`SetColumnPeriodForTest` exists because a geometric slider cannot land on a
frame period.** Every closed-form check is written at an exact ratio of columns
to frames, and `Time Per Column`'s mapping is `0.5 ms · 1000^t`. The real
control is still what `tools/sweep.py` exercises.

---

## Shape of the code

    source/Controls.*    0..1 host parameters to physical units, and the enums
    source/Strip.*       the column schedule, and the whole of the clock
    source/Shaders.cpp   the three passes, as GLSL
    source/PassBuffer.*  FFGLFBO with the leak fixed (copied from tinsel)
    source/Photofinish.* the plugin: parameters, buffers, the passes
    source/Diag.*        a log file, for the shader that will not compile
    tools/pftest/        the offline harness
    tools/sweep.py       no control is silently dead
    tools/verify.sh      all of it

Three passes:

1. **copy** — picture size. The host's texture into one of two frame buffers,
   resolving `MaxUV` and the half-texel inset once so no later pass has to
   think about either. Two buffers, not one, because a column is generally
   taken at an instant *between* two frames the host delivered.
2. **slit** — ONE COLUMN wide, run once per column the schedule asks for. The
   viewport is `( writePos, 0, 1, rows )`, which is why the slit shader's
   `uv.x` is 0.5 at every fragment and carries nothing.
3. **strip** — output size. Reads the ring with `texelFetch`, oldest to newest
   along the scan axis, with the background under the columns that have not
   been written yet.

**Nothing is mirrored between GLSL and C++.** There is no shared library string
and no probe shader, because the only arithmetic a test needs to assert on is
the column schedule, and that is pure C++ in `Strip.cpp` that never reaches a
shader. The ring's *read* mapping does live only in GLSL, and it is proved
through rendered pixels by `pftest --ring` rather than by a second copy of it
in the harness.

---

## Decisions taken without asking

The brief said decide and write it down. These are the ones a reader will
otherwise wonder about.

**Sub-frame columns are interpolated between the two neighbouring frames, and
`Interpolate` chooses how.** This is the question the spec said not to answer
silently. When the column period is shorter than the frame period, the
intervening columns fall at instants the host never delivered. `Linear` blends
the two frames around each one; `Nearest` takes whichever it is closer to.
Linear is the default because a strip built from nearest-neighbour columns has
visible stair-steps wherever an edge crosses, at exactly the rate the operator
just turned up.

**The honest limit of that, stated plainly:** a linear blend of two frames is
not a picture of the moment between them. If an object crosses the slit in less
than one host frame, the plugin never saw it over the slit and no amount of
interpolation recovers it; if it crosses in a few frames, the blend is a ghost
of two positions rather than one intermediate position. That is not a defect in
the model, it is the consequence of a plugin being handed frames and nothing
else — FFGL has no way to ask for a frame it was not given. `pftest --width` is
therefore written with `Nearest`, where the arithmetic is exact, and
`pftest --interp` checks the blend on flat fields, where "a picture of the
moment between" and "a blend of two frames" are the same thing. The regime
where they differ is not checked because there is no right answer to check
against.

**When the column period is LONGER than a frame, columns simply do not
advance.** The spec says "columns repeat"; what that means here is that the
strip stands still for a frame or two and then moves on, rather than the same
column being written twice. The visible result is the same and the ring stays
honest about how many distinct instants it holds.

**`Sync` derives a RATE from the tempo; it does not lock to the bar LINE.**
`SetBeatInfo` gives a plugin a bar *phase* as well as a tempo, and locking the
write head to it would align the sweep to the bar line exactly. It also makes
the head JUMP on a tempo change, on a scrub, and on any host that does not send
the phase (the SDK defaults it to 0) — and a head that jumps tears the strip in
a way that reads as a broken plugin. So `Sync` sets the column period to one
bar (or beat) divided by the ring's length, one sweep takes one bar, and
nothing ever jumps. **The sweep is not aligned to the bar line.** That is a real
limitation and it is the obvious v0.2.

**`Sweep Length` is the ring's LENGTH IN COLUMNS, not a duration.** Two
controls in the spec's Time group — `Time Per Column` and `Sweep Length` —
would be redundant if both were times. `Time Per Column` is the film speed;
`Sweep Length` is how much film you can see at once, from 8 columns to the full
axis. One sweep takes `Sweep Length × Time Per Column` seconds. Below the axis
length the strip is magnified to fill the picture, in blocks.

**`Sweep Length` is quantised to 48 geometric steps.** The ring's indexing is
modulo its own length, so changing the length does not shuffle its contents, it
*reinterprets* them: every column would still hold a real instant and every one
would be filed under the wrong time. Clearing is the only answer that is not
wrong, and an unquantised control would clear on every pixel of a drag. Forty-
eight steps means a nudge usually changes nothing and a real move clears once.

**`Slit Angle` is a SLOPE in normalised picture space, not degrees.** A slope
needs no trigonometry and has no pole — `tan` at 90° is the one value an
operator dragging to the end of a slider would find. ±1 is 45° in normalised
space, which is 45° *on screen* only on a square picture. That is stated rather
than corrected, because correcting it would make the control mean a different
number of pixels on every clip.

**There are no factory presets.** Every other effect in the fleet has them, and
they bring the whole host-echo mechanism with them (`hostValues[]`,
`seedHostValues`, the three-way "who is writing this parameter" test — see
tinsel's AGENTS.md). Thirteen controls of which four are the whole effect did
not justify it for 0.1.0, and the round this plugin was built in was about the
verification pass rather than the surface. It is the obvious v0.2 alongside the
bar-line lock.

**No OpenFX port and no browser demo.** Not required for 0.1.0. The OFX port is
genuinely interesting here because OFX *can* ask for other frames — a strip
camera in Resolve could be exact rather than interpolated — and that is worth a
note rather than a rush.

---

## Every numeric check, audited

The brief for this round asked for an explicit pass over every numeric check,
asking of each one: **would this still hold on a different rasteriser, and at a
different raster?** Last round four of six plugins in this fleet shipped checks
calibrated to this Mac's GPU that failed on a GPU-less CI runner, and in all
four cases the test was wrong, not the plugin.

Two shapes were deliberately avoided throughout, on the evidence of the genlock
build, which found two of its own checks wrong this same pass:

- **An edge position found by interpolating to a 50% crossing is not
  translation-invariant.** Its bias comes from the curvature of the transition
  and moves with the very thing being measured. Every width here is an
  *integral over a window containing the whole transition* — a linear
  functional, exact under translation.
- **A thresholded centroid quantises to whole columns**, so the same object can
  read half a column further left at one raster than at another for no reason
  but where the threshold fell. Every centroid here is coverage-weighted.

And the positive discipline: prefer a tolerance derived from a lattice — one
column, one row, one source frame, one 8-bit code — over one fitted to the
number this Mac printed first, and show that the analytic bound sits
comfortably inside it.

| Check | Assertion | Tolerance, and where it comes from | Rasteriser-independent? | Raster-independent? |
|---|---|---|---|---|
| `--schedule` | columns produced over 600 frames `== floor( intervals · c )` | **None — exact equality.** A column is taken every time the running total crosses a whole number, so after `N` columns' worth of time exactly `floor( N )` have been taken | Yes — **no GL at all**. This is the one check a runner with no context can still run | Yes — no raster |
| `--static` | every sample along the time axis equals the first, **bitwise** | **Zero.** Two columns of a still subject are not close, they are the same arithmetic on the same numbers; `texelFetch` keeps the sampler out of it even when magnifying | Yes. No filtered fetch is involved on the path under test | **Run at 320×180, 640×360 and 1280×720**, at 1:1 and magnified, on both axes |
| `--ring` | column `i` holds frame `N − L + i`, **bitwise** | **Zero.** Flat fields carrying their own index: 8 bits in, 8 bits stored, 8 bits out, and the slit's position and the source's filtering are out of the question | Yes | **Run at 320×180, 640×360 and 1024×576**, including a magnifying Sweep Length where one ring column covers several output pixels — the raster-sensitive part of the whole plugin |
| `--clock` | the same take from t=0 and from 499,217,238 ms, **bitwise** | **Zero** | Yes | Run at one raster; the quantity under test is a `double`, not a pixel |
| `--interp` | a column at blend `k/4` equals `lerp( was, now, k/4 )` | **One 8-bit code value**, from the ring's storage format. The two fields are 40 and 200 so every quarter blend is an exact code and the expectation is an integer | Yes. Flat fields, so no interpolation of the *source* happens anywhere | Run at 320×180 and 1280×720 |
| `--width` | integrated coverage `== b · c / v` | **One column**, which is the finest thing a strip can represent. The analytic quantisation bound is printed and asserted to be under a quarter of it; worst observed **0.110** | Yes. The measurement is a sum of code values; the bar's ramps are one column period wide, which makes the sum a partition of unity and so exact for every sampling phase | **Run at 320×180 and 1280×720**, at four column rates, including a non-integer speed so nothing can be passing by landing on whole pixels |
| `--matched` | rendered width `== b`, in absolute pixels | **One column**, as above; analytic bound **0.220** at the fastest rate | Yes | **Run at 320×180 and 1280×720 with the SAME absolute pixel sizes** — a 64-pixel object must come out 64 pixels wide at both |
| `--reverse` | skewness of the strip `== ∓` skewness of the object | **0.03**, stated. The *derived* part — the worst that sampling the object's profile on the strip's own column lattice can move its skewness, maximised over 64 phases — is computed each run and asserted to be inside it; it comes out **0.0001** | Yes. Skewness is dimensionless and invariant under the stretch, translation and scaling the strip applies, so the expected number is a property of the object alone. The expected value is obtained by integrating *the same function the card is drawn from*, never a constant typed in | Run at 320×180 and 1280×720 |
| `--sync` | one whole sweep per bar, and per beat | **Exact**: full one frame after a bar, not full one frame before | Yes | **Run at two ring lengths** (320 and 1024), so the derived column period is a different number each time |
| `--slit` | the slit reads the column it says it does: on the band 255, one band-width off 0, **bitwise**, on both axes; a leaned slit crosses the band at the row the geometry names; `N` taps over a one-pixel line give exactly `1/N` of full scale | **Zero** on position (a band is 255 and everything else is 0, so there is no tolerance to have); **one row** on the lean, from the lattice — the strip cannot locate something along the slit to better than the sample it is made of; **one 8-bit code value** on the width | Yes. The position readings are exact fetches; the lean is a **coverage-weighted** centroid, never a thresholded one | The position and width readings run at 320×180 on both axes; **the lean runs at 320×180 and 1280×720**, because the predicted row scales with the picture's height and a formula quietly fitted to 180 rows would show up at 720. It does not: 0.031 and 0.000 of a row |
| `--resize` | no column is darkened when the composition changes resolution mid-take | **Exact**: white in, white out, so any value below 255 is the defect | Yes | The check IS a raster change — 320×180 in, 400×200 in, with the output left at 320×180 |
| `--negative` | nine perturbations, asserted to FAIL | n/a | Yes | Run at 320×180 |
| `--bench` | — | **Not pass/fail.** There is no threshold worth asserting on somebody else's GPU | — | — |

### The negative controls

A check that cannot fail is not a check. `pftest --negative` perturbs the model
or the input by an amount a real defect would produce and asserts the relevant
assertion rejects it:

1. a column rate **15% out** — `--width` must reject it (it is 24 columns out)
2. a column rate **1% out** — `--width` must reject that too, which is the
   interesting boundary: a one-column tolerance on a 160-column measurement is
   about 0.6%, so if 1% got through the tolerance would be too loose for the
   size of the thing being measured
3. an **off-by-one in the ring's read index** — `--ring` must reject it (319 of
   319 columns mismatch)
4. **no interpolation**, judged against the interpolated expectation — 80 code
   values out
5. **a moving picture**, judged as a still one — 242 code values out
6. **two takes that went the same way**, judged as two directions — the
   opposite-signs assertion must fail
7. a column period **15% out** in the schedule itself, with no GL — 599 columns
   against 688
8. **a genuinely black previous frame**, judged by `--resize`'s own
   measurement. The reseed cannot be switched off from the harness, so the
   situation it prevents is reproduced instead — one black frame followed by
   white ones is exactly what a reallocated frame copy looks like from the
   ring's side

9. **a slit one pixel out of position** — the tightest spatial discrimination
   here: on a four-pixel band, the last texel centre inside reads 255 and the
   first outside reads 0

All nine reject. If one of them ever stops rejecting, the check it belongs to
has gone soft and the number it prints means nothing.

### What the audit changed

- `--schedule` was originally "within one column of `c × intervals`". It is now
  exact equality with `floor( c × intervals )`. The old form would have passed a
  plugin that systematically dropped one column in every run.
- `kColumnSnap` was 1e-9 and `--clock` failed. Sizing it against the clock's
  origin rather than against a frame period is the fix, and the reasoning is in
  `Strip.h` so the next person does not shrink it back.
- The `--width` bar was going to have hard edges, which leaves a genuine half-
  column of sampling-phase noise in every reading — exactly the kind of number
  that gets absorbed into a tolerance and then fails somewhere else. Ramps one
  column period wide make the integral a partition of unity and exact for every
  phase. The observed error dropped to 0.016 columns.
- `--reverse` was going to compare against the closed-form skewness of a
  triangle (−0.5657). It compares against the numerically integrated skewness
  of the actual profile instead, so softening the object's edges to stop it
  aliasing cannot silently invalidate the expected value.
- `--slit` did not exist either, and the gap it fills is the worst kind: every
  other check places the slit at a texel centre and then measures TIME, and the
  width of a bar's crossing does not depend on where the slit is. **A slit
  reading the wrong column of the source would have passed every single one of
  them.** It is now the only check here that measures space.
- `--resize` did not exist until the audit asked what happens when the picture
  changes size. The answer was a real defect: both frame copies are
  reallocated, a reallocated buffer is cleared, so whichever held the previous
  frame goes black and every column taken in that one frame is blended towards
  black — at the fastest column rate, a third of the ring, once, reading as a
  dark band nobody can account for. The fix is three lines and a tracked
  picture size; the check is what found it.
- The first version of `--resize` failed on a correct plugin, because the
  resize also rebuilds the RING, so the output's 320 pixels map onto 400 slots
  and everything past the written ones is legitimately the black background.
  It reads only the written region now. Worth recording as the shape of an
  easy mistake: a check whose first run fails is not automatically a bug found.
- `--width` and `--matched` were wrapping the ring on the faster cases, which
  means the integral was of two halves of two different crossings — a number
  with no meaning that was nonetheless close enough to look like a pass on the
  slower ones. They now plan the take so the ring cannot wrap, and say so
  loudly if it does.

### What this audit does NOT cover

- **Nothing here has run on a GPU-less rasteriser.** The CI workflow is written
  and will do exactly that, but there is no repo for it to run in yet. The
  argument above is that none of these checks *can* depend on the rasteriser;
  it is an argument, not a measurement, and the first CI run is what turns it
  into one. `--schedule` is the exception and it needs no GL at all.
- **Nothing here has run on Intel.** The build is universal and `lipo` says so,
  but only the arm64 slice has ever executed.
- The `--reverse` tolerance of 0.03 is the one number in the table that is
  stated rather than derived. The derived bound inside it (0.0001) is computed
  every run, so if the sampling ever gets coarse enough to matter the check will
  say so before it fails.

---

## What is genuinely verified, and what is assumed

**Verified, by measurement, on this machine (Apple Silicon, macOS 26.4,
`4.1 Metal - 90.5`):**

- **95 assertions**, all passing, across twelve check suites. The headline numbers
  are in the table above and `tools/verify.sh` prints them on every run.
- **A still picture renders as bitwise-constant streaks** — 0 code values of
  difference over 2.3 million samples, at three rasters, at 1:1 and magnified,
  on both axes, and with a wide and leaned slit.
- **The ring holds the right frames in the right order** — 0 wrong columns over
  8 configurations at 3 rasters, including a magnifying Sweep Length, both
  fill modes, reversed, and the partly-filled case.
- **The rendered width follows `b · c / v`** to 0.016 columns, against a
  tolerance of one column and an analytic quantisation bound of 0.110.
- **An object at the film speed keeps its own proportions** to 0.031 columns,
  and one at twice the film speed comes out at 0.5002 of its width.
- **The two directions are mirror images** — skewness matches the object's own
  to 0.0002, against a lattice bound of 0.0001.
- **The clock survives 5.8 days** — the same take from t = 0 and from
  499,217,238 ms is bit-identical, including at the fastest column rate.
- **The slit is where it says it is** — bitwise on the band and bitwise black
  one band-width off, on both axes; a leaned slit crosses the band 0.031 of a
  row from where the geometry puts it at 320×180 and 0.000 at 1280×720; and
  `N` taps over a one-pixel line come
  back at exactly `1/N` of full scale for N = 1, 3, 8, 18 and 64.
- **No dead controls.** All **13** measurably change the picture
  (`tools/sweep.py`), with no CONTEXT table at all — which is itself a claim:
  every control reads on the defaults, because the defaults are a slit in the
  middle of a moving picture and that is the whole effect.
- **The build is universal and exports `plugMain`** — `lipo` reports
  `x86_64 arm64`, `nm -gU` finds `_plugMain`.
- **It registers, instantiates and renders 120 frames in a real FFGL host** —
  `oxbow selftest`, which also confirms the name, the id and the type a host
  sees: `SW Photofinish`, `PF01`, `effect`.
- **The render cost** (`pftest --bench --frames 200`, after a 20-frame warm-up,
  `glFinish` on both sides):

  | | ms/frame, default rate | ms/frame, fastest rate | % of a 60fps frame |
  | --- | --- | --- | --- |
  | 1280×720 | 0.174 | 0.200 | 1.0–1.2% |
  | 1920×1080 | 0.265 | 0.289 | 1.6–1.7% |
  | 3840×2160 | 0.963 | 1.029 | 5.8–6.2% |

  The two columns are four columns a frame and thirty-three. The cost barely
  moves, which is worth knowing: a column draw is one pixel wide, so the
  per-column work is draw-call overhead rather than fill, and the frame is
  dominated by the copy and the strip pass.

**Assumed, or not yet done:**

- **Never loaded into Resolume.** Everything here was compiled, rendered and
  measured offline against the real plugin class in a headless CGL context,
  plus one load in the fleet's own `oxbow` host. Nothing has driven Arena. How
  the parameters *present* — whether three groups read sensibly in the
  inspector, whether `Sweep Length` clearing the strip mid-drag is acceptable
  in practice, whether `Time Per Column` wants a different curve once somebody
  has their hand on it — is untested.
- **Never run on Windows.** The CI job is written; there is no repo for it to
  run in.
- **Never run on a GPU-less rasteriser.** See the audit above.
- **The host's clock unit has only been asserted, not observed.** The
  milliseconds-or-seconds calibration is the fleet's, copied, and the harness
  declares seconds outright so the inference path is never exercised here. A
  host that sends milliseconds has not been watched doing it. This is the
  single most consequential untested path in the plugin: get the unit wrong by
  a factor of a thousand and every frame asks for a thousand times too many
  columns.
- **`Sync` has never seen a real `SetBeatInfo`.** The check runs at the SDK's
  default 120 bpm, which is what a host that never calls it leaves in place. A
  tempo change mid-take has not been tried.
- **No bar-line lock, no presets, no OpenFX, no browser demo.** See the
  decisions above.

---

## Siblings

`PassBuffer` and `Diag` are **tinsel**'s, copied rather than rewritten. The
`Ensure()`-before-binding discipline, the OBJECT-library registration, the
`sweep.py` shape and the `verify.sh` shape come from **tinsel**, **afterglow**
and **graticule**. The ring-of-recent-things idea is **afterglow**'s, one
dimension down. The `Sync` mapping is **regauss**'s, minus the phase lock.
Cross-cutting fleet knowledge lives in
[fleet-notes](https://github.com/stoatworks-labs/fleet-notes).
