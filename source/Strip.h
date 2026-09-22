#pragma once

namespace photofinish
{
/**
	The column schedule: when a column is taken, and out of which two frames.

	--------------------------------------------------------- the one idea

	A strip camera has no frames. The film moves continuously past a slit, so a
	column is taken every `tpc` seconds whatever the projector happens to be
	doing. A plugin, however, is handed **frames**, at the host's rate and no
	other. Reconciling those two clocks is the whole of this file, and it is
	one line of arithmetic plus the two decisions that hang off it.

	Columns are counted, not timed. `phase` is how far past the last column the
	clock has got, **in columns**, and it lives in 0..1 (see the snap below
	for the one hair's breadth it is allowed outside that). Each frame advances it
	by `dt / tpc` and every whole number crossed is a column:

	    x = phase + dt / tpc
	    n = floor( x )          columns to take this frame
	    phase = x - n

	`n` may be zero (a column period longer than a frame -- columns repeat, in
	the sense that the picture simply does not advance), one, or several dozen
	(a column period far shorter than a frame).

	------------------------------------------------- why nothing here is absolute

	☠️ **No absolute time appears in this file, or anywhere downstream of it.**

	Resolume's `SetTime` has been measured at 499,217,238 ms into a session. A
	32-bit float's spacing at that magnitude is about 0.03 -- so a phase
	computed from absolute time would stop resolving anything finer than thirty
	milliseconds, and a plugin whose entire output is a clock would quietly
	stop producing columns at an even rate some hours into a show. Nothing
	about that shows up in a test that starts at t = 0, which is every test
	anybody writes.

	So the host's clock is used for exactly one thing: the DIFFERENCE between
	this frame and the last. That difference is a few tens of milliseconds
	whatever the origin, `phase` is a double in 0..1 by construction, and the
	only time-like number that ever reaches a shader is `blend`, which is also
	in 0..1. `pftest --clock` renders the same take from t = 0 and from
	t = 499,217,238 ms and requires the two strips to be identical bit for bit.

	------------------------------------------------------- the snap, and why

	`floor( x )` is exact arithmetic on an inexact number, and the case it gets
	wrong is the most likely setting there is: a column period equal to the
	frame period.

	The harness drives `SetTime( frame / fps )`, so `dt` is the difference of
	two exact quotients and is NOT exactly `1/fps` -- `3/60 - 2/60` comes out
	one ulp below `1/60`. With `tpc` exactly `1/60`, `x` is 0.9999999999999998
	and `floor` says no column. The next frame recovers one column, and the one
	after that, so the rate is right for ever afterwards but the strip is
	permanently one column behind where it should be. It is invisible in a
	picture and it broke `--ring` on the first run.

	A column boundary landing exactly on a frame boundary is genuinely
	ambiguous, so it is resolved rather than left to the last bit: `n` is
	`floor( x + kColumnSnap )`.

	**The snap has to be sized against the clock's origin, not against 1/60.**
	At 499,217,238 ms a double's spacing is about 1.1e-10 s, so a frame
	difference taken there carries an error of a couple of ulps -- roughly
	1.3e-8 of a column period at 60 fps. A snap of 1e-9 would sit *below* that
	and `--clock` would fail: the same take rendered at t = 0 and at
	t = 499,217,238 ms would come out one column apart. 1e-6 of a column is
	half a nanosecond at the fastest column rate this plugin offers, which is
	twelve orders of magnitude below anything the effect can represent, and it
	is four orders of magnitude above the clock's own noise floor at the far
	end of a long session.

	The remainder carried forward is `x - floor( x + snap )` and is allowed to
	go very slightly NEGATIVE. Clamping it at zero instead would throw away up
	to one snap of phase every time the snap fired, which on the
	one-column-per-frame setting is every frame -- a fifth of a column an hour.
	Harmless, but a drift that need not exist.
*/
namespace strip
{

/// How close to a column boundary counts as being on it, in columns.
/// See the header -- it is sized against the clock's origin, not against a
/// frame period.
inline constexpr double kColumnSnap = 1e-6;

/// The most host time one frame may advance the strip by, in seconds.
///
/// The host's clock is not ours. It jumps when the composition is scrubbed,
/// when a clip is retriggered, and by however long the machine was asleep, and
/// a 40-minute jump at the fastest column rate is five million columns. Capped
/// at a quarter of a second, which is fifteen frames -- longer than any real
/// frame and shorter than any real scrub.
inline constexpr double kMaxFrameDelta = 0.25;

/// What one frame owes the strip.
struct Schedule
{
	/// Columns to take this frame. Never negative; capped by the caller at the
	/// ring's length, because writing more than a whole ring in one frame
	/// writes over work already done in the same frame.
	int columns = 0;

	/// The phase to carry into the next frame, in -kColumnSnap..1. The tiny
	/// negative end is deliberate; see the header.
	double phase = 0.0;
};

/// Advance the phase by one frame.
///
/// `dt` is the frame's own length in seconds, already clamped by the caller;
/// `tpc` is seconds per column. A non-positive `tpc` or `dt` yields no columns
/// and leaves the phase alone.
Schedule advance( double phase, double dt, double tpc );

/// Where within the frame column `index` is taken, as a fraction of the frame:
/// 0 is the previous frame's instant, 1 is this frame's.
///
/// Column `index` (0-based within this frame) falls `index + 1 - phase` column
/// periods after the frame started, so
///
///     blend = ( index + 1 - phase ) * tpc / dt
///
/// where `phase` is the value BEFORE `advance` was called. It is clamped into
/// 0..1: the only way out of range is a frame whose column count the caller
/// capped, and a column that would have been taken after the end of the frame
/// is better pinned to the end of it than extrapolated past it.
double blendAt( int index, double phase, double dt, double tpc );

} // namespace strip
} // namespace photofinish
