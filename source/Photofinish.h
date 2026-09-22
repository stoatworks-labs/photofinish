#pragma once

#include "Controls.h"
#include "PassBuffer.h"
#include "StoatworksAboutParams.h"

#include <FFGLSDK.h>

#include <string>

/**
	Photofinish -- a strip camera, as an FFGL effect.

	------------------------------------------------------------- the one idea

	**A photo-finish camera has no two-dimensional frame. It has one column of
	sensor, and the film moves past it. So the horizontal axis of the picture
	is TIME, NOT SPACE.**

	That single substitution produces every artefact the effect is known for,
	and none of them is coded anywhere:

	- anything **static** becomes a horizontal streak, because it is the same
	  column written over and over;
	- anything **moving** is stretched or squashed by 1/its speed, because the
	  faster it crosses the slit the fewer columns it occupies;
	- an object moving at exactly the film speed comes out with its **true
	  proportions** -- which is why the winner looks normal and the crowd
	  behind is smeared;
	- an object moving the *other* way comes out **reversed**, the famous
	  backwards runner.

	Nothing is warped, blurred or time-displaced on purpose. One column is
	sampled per tick and pushed onto a ring, and all of the above falls out. If
	a term ever appears in this file whose job is to produce one of those
	effects, the mechanism has been lost and the term is the evidence.

	------------------------------------------------------------- the pipeline

	    copy   the host's texture into one of two frame buffers, alternating
	    slit   one column of the ring, once per column the schedule asks for
	    strip  the ring, oldest to newest along the scan axis

	Two frame copies and not one, because a column is generally taken at an
	instant BETWEEN two frames the host delivered. `Interpolate` says whether
	it is built from both or taken from the nearer.

	`Strip.h` holds the schedule -- how many columns a frame owes and at what
	instant each of them falls -- and it is the one piece of arithmetic in the
	plugin that a test can assert on without rendering anything.

	------------------------------------------------------------ the clock trap

	☠️ This plugin is a clock with a picture attached, so it is the fleet's
	most exposed to the one measured at 499,217,238 ms. Read the header of
	`Strip.h` before touching anything time-like here. In short: the host's
	clock is used for the DIFFERENCE between two frames and nothing else, every
	phase is a double in 0..1, and the only time-like number that reaches a
	shader is `Blend`, which is also in 0..1.
*/
class Photofinish : public CFFGLPlugin
{
public:
	Photofinish();

	//CFFGLPlugin
	FFResult InitGL( const FFGLViewportStruct* vp ) override;
	FFResult ProcessOpenGL( ProcessOpenGLStruct* pGL ) override;
	FFResult DeInitGL() override;

	FFResult SetFloatParameter( unsigned int index, float value ) override;
	float GetFloatParameter( unsigned int index ) override;

	FFResult SetTime( double time ) override;
	void SetBeatInfo( float bpm, float barPhase ) override;

	char* GetTextParameter( unsigned int index ) override;

	/// Declared only so the About line can accept its own default.
	/// `instantiateGL` pushes every declared default back through the setters
	/// and deletes the whole instance if one fails, and CFFGLPlugin's
	/// SetTextParameter is a stub that returns exactly that failure. Omit this
	/// and the plugin cannot be created in any real host while every in-repo
	/// check still passes.
	FFResult SetTextParameter( unsigned int index, const char* value ) override;

	//-----------------------------------------------------------------------
	// Test hooks.
	//
	// Both of these exist because the harness DECLARES what it is doing rather
	// than leaving the plugin to infer it. An implicit unit is what let the
	// milliseconds-or-seconds bug through elsewhere in the fleet, and a column
	// period arrived at through a geometric slider mapping cannot be made
	// exactly equal to a frame period -- which is the single most interesting
	// setting there is, and the one every closed-form check is written at.
	//-----------------------------------------------------------------------

	/// Say outright what unit SetTime is in: 1.0 for seconds, 0.001 for
	/// milliseconds. Skips the calibration.
	void SetClockScaleForTest( double scale );

	/// Override the column period, in seconds, exactly. A non-positive value
	/// hands control back to the Time Per Column parameter.
	void SetColumnPeriodForTest( double seconds );

	/// The ring's shape and where its head is. Configuration facts the harness
	/// prints and sizes its expectations from -- not the thing under test,
	/// which is what comes out of the strip pass.
	int RingLengthForTest() const
	{
		return ringLength;
	}
	int RingRowsForTest() const
	{
		return ringRows;
	}
	int WritePosForTest() const
	{
		return writePos;
	}
	int FilledForTest() const
	{
		return filled;
	}

	/// The order the host shows them in: where the slit is, how fast the film
	/// runs, and what comes out.
	enum ParamID : FFUInt32
	{
		//Slit
		PT_SLIT_POSITION,
		PT_SLIT_WIDTH,
		PT_SLIT_ANGLE,
		PT_AXIS,
		PT_DIRECTION,

		//Time
		PT_TIME_PER_COLUMN,
		PT_SWEEP_LENGTH,
		PT_SYNC,
		PT_INTERPOLATE,

		//Output
		PT_FILL,
		PT_BACKGROUND,
		PT_MIX,
		PT_FREEZE,

		//About. FFGL has no window and cannot make one, so the name, the
		//version, the maker and the links are parameters the host draws with
		//everything else. Last in the enum, so no saved composition's
		//parameter ids shift when more arrive. See StoatworksAboutParams.h.
		PT_ABOUT_FIRST,
		PT_COUNT = PT_ABOUT_FIRST + stoatworks::about::kParamCount
	};

private:
	/// Bring the two frame copies and the ring to this shape, reallocating
	/// only what has changed. False means the driver would not give us the
	/// memory.
	bool EnsureBuffers( int pictureWidth, int pictureHeight, int wantLength, int wantRows );

	/// The column period this frame, in seconds: Time Per Column, or the
	/// tempo divided by the ring's length when Sync is Beat or Bar.
	double ColumnPeriod() const;

	/// Normalise the host's clock to seconds, deciding its unit the first time
	/// there is enough evidence.
	double NormalisedNow();

	ffglex::FFGLShader copyShader;
	ffglex::FFGLShader slitShader;
	ffglex::FFGLShader stripShader;
	ffglex::FFGLScreenQuad quad;

	//-----------------------------------------------------------------------
	// The two frame copies.
	//
	// A fixed array and not a std::vector: ffglex::FFGLFBO has a user-declared
	// destructor and raw GL ids, so its implicit copy constructor duplicates
	// the ids without duplicating the objects, and a vector reallocation would
	// hand two PassBuffers the same framebuffer and delete it twice.
	//-----------------------------------------------------------------------
	photofinish::PassBuffer frames[ 2 ];
	int frameIndex   = 0;    ///< which of the two holds THIS frame
	bool framesSeeded = false;///< false until both hold a real picture

	//-----------------------------------------------------------------------
	// The ring. `writePos` is the slot the next column goes into; `filled` is
	// how many slots have ever been written, capped at the length.
	//
	// A ring whose contents are undefined is not "a bit of noise on the first
	// frame", it is whatever texture memory the driver handed back -- so the
	// strip pass asks `filled` whether a slot means anything rather than
	// trusting the buffer, and PassBuffer clears on allocation as well.
	//-----------------------------------------------------------------------
	photofinish::PassBuffer ring;
	int ringLength = 0;
	int ringRows   = 0;
	int writePos   = 0;
	int filled     = 0;

	//-----------------------------------------------------------------------
	// Time. See Strip.h -- everything here is a difference or a fraction.
	//-----------------------------------------------------------------------
	double hostTime     = -1.0;///< the host's raw clock, in the host's own unit
	double lastNow      = -1.0;///< the previous frame's normalised time, seconds
	double columnPhase  = 0.0; ///< how far past the last column we are, in columns

	double clockScale   = 0.0; ///< 0 until decided; then 1.0 or 0.001
	double lastWallTime = -1.0;
	double wallStart    = -1.0;
	double lastRawTime  = -1.0;
	int secondsVotes    = 0;
	int millisVotes     = 0;
	int clockFrames     = 0;

	double columnPeriodOverride = -1.0;///< the test hook; negative means off

	/// What the host says the tempo is. Resolume calls SetBeatInfo
	/// unconditionally and the SDK defaults to 120/0, but a host that never
	/// does would leave these at the defaults -- which is why Sync derives a
	/// RATE from the tempo rather than locking to the phase. See AGENTS.md.
	float hostBpm      = 120.0f;
	float hostBarPhase = 0.0f;

	float params[ PT_COUNT ] = {};

	/// GetTextParameter hands the host a bare pointer, so the string has to
	/// outlive the call.
	std::string aboutText;
};
