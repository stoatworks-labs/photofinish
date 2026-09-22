#pragma once

/**
	Host parameters are 0..1; these are what they mean.

	Every ranged parameter this plugin declares is a plain FF_TYPE_STANDARD
	float in 0..1, including the ones that stand for a number of seconds or a
	number of columns. That is not a style preference.
	`CFFGLPluginManager::SetParamInfo` clamps a standard default into 0..1
	*before* returning, and `SetParamRange` can only be called afterwards
	because it finds the parameter by ID -- so a parameter declared in seconds
	cannot declare a default in seconds, and 0.004 would survive by luck while
	64 would silently become 1. The conversions live here instead, in one file
	the plugin and the harness both call, so there is only ever one answer to
	what a slider position means.

	**The harness calls these rather than re-deriving them.** A test that
	worked out for itself what 0.307 means would agree with itself perfectly
	and prove nothing about the plugin.

	Where a mapping is geometric it is because the interesting range is at one
	end: the difference between 1 ms and 2 ms per column is a different effect
	and the difference between 400 ms and 500 ms is not.
*/
namespace photofinish
{
namespace controls
{

/// Where the slit sits on the axis it cuts ACROSS, 0..1. Identity, and here
/// only so that nothing outside this file has to know that.
///
/// For a horizontal sweep the slit is a vertical line and this is its x; for
/// a vertical sweep the slit is a horizontal line and this is its y. 0.5 is
/// the middle of the picture in both cases.
float SlitPosition( float value );

/// The slit's width across the picture, in SOURCE PIXELS: 1 to 64,
/// geometrically, rounded to a whole number of taps.
///
/// One pixel is the default and the honest default. A wider slit averages the
/// picture across the direction of travel, and because the output's scan axis
/// is time, that average is over the time an object takes to cross the slit --
/// it is motion blur in the time axis, arrived at rather than added. It is not
/// a blur control and it does not soften the other axis at all.
int SlitWidthPixels( float value );

/// The slit's lean, as a SLOPE: cross-axis displacement per unit of slit
/// length, in normalised picture coordinates. Bipolar about 0.5, range -1..+1.
///
/// A slope rather than an angle in degrees, because a slope needs no
/// trigonometry and has no pole: `tan` at 90 degrees is the one value an
/// operator dragging to the end of a slider would find. +-1 is 45 degrees in
/// normalised space, which is 45 degrees on screen only on a square picture --
/// see AGENTS.md, this is stated rather than corrected, because correcting it
/// would make the control mean a different number of pixels on every clip.
float SlitLean( float value );

/// Seconds of source time per column: 0.5 ms to 500 ms, geometrically.
///
/// This is the film speed and it is the only rate in the plugin. Everything
/// else about the picture -- how wide a moving object comes out, which way
/// round it is, whether it streaks -- is a consequence of it and of how fast
/// the subject moves.
///
/// The bottom of the range is deliberately far below a frame period: at 0.5 ms
/// a 60 fps host produces 33 columns per frame and every one of them but the
/// last is interpolated between two frames the host actually delivered. That
/// is the regime the whole Interpolate control exists for, and it has an
/// honest limit -- see AGENTS.md.
double TimePerColumnSeconds( float value );

/// How many columns the strip holds, given the length of the axis it sweeps
/// along in pixels. 8 columns to the full axis, geometrically.
///
/// Quantised to 48 geometric steps on purpose. The ring's indexing is modulo
/// its own length, so changing the length does not shuffle its contents, it
/// REINTERPRETS them: every column would still hold a real instant and every
/// one of them would be filed under the wrong time. Clearing is the only
/// answer that is not wrong, and an unquantised control would clear on every
/// pixel of a drag. Forty-eight steps means a nudge usually changes nothing
/// and a real move clears once.
///
/// Below the axis length the strip is magnified to fill the picture, and
/// magnified with GL_NEAREST: one column becomes a block of identical pixels
/// rather than a gradient between two instants that were never adjacent.
int SweepColumns( float value, int axisPixels );

} // namespace controls

/// What Axis stores. Option VALUES, not list positions.
enum AxisMode
{
	kAxisHorizontal = 0,///< the slit is a vertical line; time runs across
	kAxisVertical   = 1,///< the slit is a horizontal line; time runs down
	kAxisCount
};

/// What Direction stores. Which end of the scan axis is the newest column.
enum DirectionMode
{
	kDirectionForward = 0,///< newest at the right (horizontal) or bottom (vertical)
	kDirectionReverse = 1,
	kDirectionCount
};

/// What Sync stores.
enum SyncMode
{
	kSyncFree = 0,///< Time Per Column is the rate
	kSyncBeat = 1,///< one whole sweep per beat; Time Per Column is ignored
	kSyncBar  = 2,///< one whole sweep per bar; Time Per Column is ignored
	kSyncCount
};

/// What Interpolate stores.
enum InterpolateMode
{
	kInterpolateNearest = 0,
	kInterpolateLinear  = 1,
	kInterpolateCount
};

/// What Fill stores -- what happens when the strip is full.
enum FillMode
{
	/// The head marches across and wraps, writing in place. The picture builds
	/// up from empty the first time and then keeps sweeping, with a moving
	/// seam where the newest column meets the oldest. This is what a strip
	/// printer looks like.
	kFillBuild = 0,

	/// The newest column is always at the same edge and the whole picture
	/// slides. Same ring, read through an offset.
	kFillScroll = 1,

	/// Fill once and stop. Nothing is written after the strip is full, so the
	/// picture holds until something resets it.
	kFillOnce = 2,

	kFillCount
};

/// What Background stores -- what an unwritten column shows.
enum BackgroundMode
{
	kBackgroundBlack       = 0,
	kBackgroundSource      = 1,///< the live clip, so the strip grows over it
	kBackgroundTransparent = 2,
	kBackgroundCount
};

} // namespace photofinish
