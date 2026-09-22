#include "Photofinish.h"

#include "Diag.h"
#include "Shaders.h"
#include "Strip.h"

//FFGLSDK.h includes every other scoped binding and omits this one (SDK
//b1afaf9), so it has to be asked for by name. The symptom without it is an
//unknown-type error on ScopedFBOBinding and nothing else.
#include <ffglex/FFGLScopedFBOBinding.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <string>
#include <vector>

using namespace ffglex;
using namespace photofinish;

static CFFGLPluginInfo PluginInfo(
	PluginFactory< Photofinish >,// Create method
	"PF01",                      // Plugin unique ID of maximum length 4.
	"SW Photofinish",            // Plugin name
	2,                           // API major version number
	1,                           // API minor version number
	0,                           // Plugin major version number
	1,                           // Plugin minor version number
	FF_EFFECT,                   // Plugin type
	"A strip camera. There is no frame: there is one column of sensor, and the film moves past it, so the horizontal axis of the picture is TIME rather than space.\n\nEverything it does follows from that one substitution. Anything standing still becomes a horizontal streak, because it is the same column written over and over. Anything moving is stretched or squashed by one over its speed. Something crossing at exactly the film speed comes out with its true proportions, which is why the winner looks normal and the crowd behind is smeared. Something crossing the other way comes out backwards.\n\nNothing is warped or blurred on purpose. Start with Slit Position in the middle and move Time Per Column until things you care about are the right shape.",// Plugin description
	"Photofinish FFGL effect"    // About
);

namespace
{
/// glGetString returns nullptr when there is no current context, and feeding
/// that to std::string is undefined behaviour. A logging call must never be
/// the thing that brings the host down.
std::string glStringOrUnknown( GLenum name )
{
	const GLubyte* value = glGetString( name );
	return value ? reinterpret_cast< const char* >( value ) : "unknown";
}

const char* const kAxisNames[]       = { "Horizontal", "Vertical" };
const char* const kDirectionNames[]  = { "Forward", "Reverse" };
const char* const kSyncNames[]       = { "Free", "Beat", "Bar" };
const char* const kInterpolateNames[] = { "Nearest", "Linear" };
const char* const kFillNames[]       = { "Build", "Scroll", "Once" };
const char* const kBackgroundNames[] = { "Black", "Source", "Transparent" };

/// Frames that must agree before the host's clock unit is settled.
constexpr int kClockVotes = 4;

/// Wall clock, for hosts that never call SetTime. Steady rather than system,
/// so nothing here moves when the machine's clock is corrected.
double wallSeconds()
{
	using namespace std::chrono;
	static const steady_clock::time_point start = steady_clock::now();
	return duration_cast< duration< double > >( steady_clock::now() - start ).count();
}

/// Case-insensitive name order, so a list sorts the way a reader expects it to
/// rather than the way a byte comparison does.
bool nameLess( const char* a, const char* b )
{
	for( ; *a && *b; ++a, ++b )
	{
		const int ca = std::tolower( static_cast< unsigned char >( *a ) );
		const int cb = std::tolower( static_cast< unsigned char >( *b ) );
		if( ca != cb )
			return ca < cb;
	}
	return *b != '\0';
}

int optionValue( float stored, int count )
{
	return std::clamp( static_cast< int >( std::lround( stored ) ), 0, count - 1 );
}
} // namespace

//---------------------------------------------------------------------------
Photofinish::Photofinish()
{
	SetMinInputs( 1 );
	SetMaxInputs( 1 );

	//The host drives the clock where it can, so that a render of the same
	//stretch of timeline produces the same columns and an export matches the
	//preview.
	//
	//Note what it cannot drive: WHICH FRAMES ARE IN THE RING. That is a
	//consequence of which frames the host asked for, in the order it asked,
	//and nothing in FFGL lets a plugin ask for a frame it was not given. See
	//AGENTS.md -- it is the honest limit of the whole effect.
	SetTimeSupported( true );

	//---------------------------------------------------------------------
	// Defaults. SetParamInfof reads each one back out of GetFloatParameter,
	// so these assignments are what the host is told the defaults are.
	//
	// They add up to a middle slit, four columns per frame at 60 fps, a strip
	// as long as the picture is wide, scrolling. Drop it on a clip of anything
	// moving and the effect is immediately what it is. The null is Mix at
	// zero.
	//---------------------------------------------------------------------
	params[ PT_SLIT_POSITION ] = 0.50f;
	params[ PT_SLIT_WIDTH ]    = 0.0f;//one source pixel: a fetch, not an average
	params[ PT_SLIT_ANGLE ]    = 0.50f;//bipolar null
	params[ PT_AXIS ]          = static_cast< float >( kAxisHorizontal );
	params[ PT_DIRECTION ]     = static_cast< float >( kDirectionForward );

	//0.307 on the geometric map is 4.17 ms, which is four columns per frame at
	//60 fps and a whole sweep of a 1920-wide picture in eight seconds. One
	//column per frame is the "true" strip camera and it is far too slow to
	//show anybody what the effect does in the two seconds they will give it.
	params[ PT_TIME_PER_COLUMN ] = 0.307f;
	params[ PT_SWEEP_LENGTH ]    = 1.0f;//the full width of the picture
	params[ PT_SYNC ]            = static_cast< float >( kSyncFree );
	params[ PT_INTERPOLATE ]     = static_cast< float >( kInterpolateLinear );

	params[ PT_FILL ]       = static_cast< float >( kFillScroll );
	params[ PT_BACKGROUND ] = static_cast< float >( kBackgroundBlack );
	params[ PT_MIX ]        = 1.0f;
	params[ PT_FREEZE ]     = 0.0f;

	//---------------------------------------------------------------------
	// Declaration.
	//
	// Every ranged parameter is a plain 0..1 float even where it stands for a
	// number of seconds or a number of columns. SetParamInfo clamps an
	// FF_TYPE_STANDARD default into 0..1 *before* a range can be attached (SDK
	// b1afaf9), so a parameter declared in seconds cannot declare a default in
	// seconds. The conversions live in Controls.cpp.
	//
	// Option lists are declared in alphabetical order and every entry keeps
	// the value it has always had. Those are two separate things in FFGL:
	// SetParamElementInfo takes an element's display slot and its stored value
	// as different arguments, and the spec is explicit that picking an option
	// gives the parameter "a value equal to that of the option's value" -- the
	// slot is never stored. So a list can be sorted for whoever has to read it
	// without a saved composition or the harness changing meaning.
	//
	// Two lists are EXEMPT, because position is their meaning: Sync (Free ->
	// Beat -> Bar is a progression) and Interpolate (Nearest -> Linear is one
	// too, from no interpolation to some). Sorting either would file them by
	// initial letter and say nothing.
	//---------------------------------------------------------------------
	auto declareOptions = [ this ]( unsigned int paramID, int count, auto nameOf ) {
		std::vector< int > order( static_cast< size_t >( count ) );
		for( int i = 0; i < count; ++i )
			order[ i ] = i;

		std::stable_sort( order.begin(), order.end(),
		                  [ & ]( int a, int b ) { return nameLess( nameOf( a ), nameOf( b ) ); } );

		for( int slot = 0; slot < count; ++slot )
			SetParamElementInfo( paramID,
			                     static_cast< unsigned int >( slot ),
			                     nameOf( order[ slot ] ),
			                     static_cast< float >( order[ slot ] ) );
	};

	auto declareOptionsInOrder = [ this ]( unsigned int paramID, int count, auto nameOf ) {
		for( int i = 0; i < count; ++i )
			SetParamElementInfo( paramID, static_cast< unsigned int >( i ), nameOf( i ),
			                     static_cast< float >( i ) );
	};

	SetParamInfof( PT_SLIT_POSITION, "Slit Position", FF_TYPE_STANDARD );
	SetParamInfof( PT_SLIT_WIDTH, "Slit Width", FF_TYPE_STANDARD );
	SetParamInfof( PT_SLIT_ANGLE, "Slit Angle", FF_TYPE_STANDARD );

	SetOptionParamInfo( PT_AXIS, "Axis", kAxisCount, params[ PT_AXIS ] );
	declareOptions( PT_AXIS, kAxisCount, []( int v ) { return kAxisNames[ v ]; } );

	SetOptionParamInfo( PT_DIRECTION, "Direction", kDirectionCount, params[ PT_DIRECTION ] );
	declareOptions( PT_DIRECTION, kDirectionCount, []( int v ) { return kDirectionNames[ v ]; } );

	SetParamInfof( PT_TIME_PER_COLUMN, "Time Per Column", FF_TYPE_STANDARD );
	SetParamInfof( PT_SWEEP_LENGTH, "Sweep Length", FF_TYPE_STANDARD );

	SetOptionParamInfo( PT_SYNC, "Sync", kSyncCount, params[ PT_SYNC ] );
	declareOptionsInOrder( PT_SYNC, kSyncCount, []( int v ) { return kSyncNames[ v ]; } );

	SetOptionParamInfo( PT_INTERPOLATE, "Interpolate", kInterpolateCount, params[ PT_INTERPOLATE ] );
	declareOptionsInOrder( PT_INTERPOLATE, kInterpolateCount,
	                       []( int v ) { return kInterpolateNames[ v ]; } );

	SetOptionParamInfo( PT_FILL, "Fill", kFillCount, params[ PT_FILL ] );
	declareOptions( PT_FILL, kFillCount, []( int v ) { return kFillNames[ v ]; } );

	SetOptionParamInfo( PT_BACKGROUND, "Background", kBackgroundCount, params[ PT_BACKGROUND ] );
	declareOptions( PT_BACKGROUND, kBackgroundCount, []( int v ) { return kBackgroundNames[ v ]; } );

	SetParamInfof( PT_MIX, "Mix", FF_TYPE_STANDARD );

	//A real boolean rather than a slider with a threshold in it. The host
	//draws it as a switch, and a switch is what it is: the film either moves
	//or it does not.
	SetParamInfo( PT_FREEZE, "Freeze", FF_TYPE_BOOLEAN, false );

	for( FFUInt32 i = PT_SLIT_POSITION; i <= PT_DIRECTION; ++i )
		SetParamGroup( i, "Slit" );
	for( FFUInt32 i = PT_TIME_PER_COLUMN; i <= PT_INTERPOLATE; ++i )
		SetParamGroup( i, "Time" );
	for( FFUInt32 i = PT_FILL; i <= PT_FREEZE; ++i )
		SetParamGroup( i, "Output" );

	// The About block. Declared inline rather than through a helper, because
	// SetParamInfo is protected on CFFGLPlugin and nothing outside the class
	// can call it.
	SetParamInfo( PT_ABOUT_FIRST, "About", FF_TYPE_TEXT, stoatworks::about::defaultText() );
	{
		FFUInt32 aboutId = PT_ABOUT_FIRST + 1;
		for( const auto& b : stoatworks::about::buttons() )
			SetParamInfo( aboutId++, b.label, FF_TYPE_EVENT, false );
	}
	for( FFUInt32 i = PT_ABOUT_FIRST; i < PT_COUNT; ++i )
		SetParamGroup( i, "About" );

	FFGLLog::LogToHost( "Created Photofinish effect" );

	diag::init();
}

//---------------------------------------------------------------------------
FFResult Photofinish::InitGL( const FFGLViewportStruct* vp )
{
	//The GL strings first, and unconditionally: when a shader will not compile
	//it is almost always the driver or the GL version, and knowing which
	//machine reported what is most of the diagnosis.
	diag::info( std::string( "GL vendor=" ) + glStringOrUnknown( GL_VENDOR )
	            + " renderer=" + glStringOrUnknown( GL_RENDERER )
	            + " version=" + glStringOrUnknown( GL_VERSION ) );

	struct
	{
		FFGLShader* shader;
		const char* fragment;
		const char* name;
	} const stages[] = {
		{ &copyShader, kCopyShader, "copy" },
		{ &slitShader, kSlitShader, "slit" },
		{ &stripShader, kStripShader, "strip" },
	};

	for( const auto& stage : stages )
	{
		if( stage.shader->Compile( kVertexShader, stage.fragment ) )
			continue;

		//Returning FF_FAIL here is invisible to the operator: the effect
		//simply does nothing in Resolume, with no message anywhere. These two
		//lines are the only record of which pass it was.
		diag::error( std::string( "the " ) + stage.name
		             + " shader failed to compile - the effect will do nothing" );
		FFGLLog::LogToHost( "Photofinish: shader failed to compile" );
		DeInitGL();
		return FF_FAIL;
	}

	if( !quad.Initialise() )
	{
		diag::error( "quad geometry failed to initialise" );
		FFGLLog::LogToHost( "Photofinish: quad geometry failed to initialise" );
		DeInitGL();
		return FF_FAIL;
	}

	writePos     = 0;
	filled       = 0;
	columnPhase  = 0.0;
	framesSeeded = false;

	//Use base-class init as the success result so it retains the viewport.
	return CFFGLPlugin::InitGL( vp );
}

//---------------------------------------------------------------------------
bool Photofinish::EnsureBuffers( int pictureWidth, int pictureHeight, int wantLength, int wantRows )
{
	//The two frame copies. GL_RGBA8 and not a float format: what arrives from
	//the host is eight bits per channel, and storing it wider would only
	//spread the same values out. Keeping it at eight is also what makes
	//`--static` and `--ring` bit-for-bit claims rather than tolerances.
	for( PassBuffer& buffer : frames )
	{
		//Linear, because a tilted or wide slit lands between texels. That is
		//sampling SPACE, which the slit is entitled to do; the ring is a
		//different question -- see below.
		if( !buffer.Ensure( pictureWidth, pictureHeight, GL_RGBA8, PassBuffer::Sampling::Linear ) )
			return false;
	}

	if( pictureWidth != pictureWidthWas || pictureHeight != pictureHeightWas )
	{
		//See the declaration: the buffers have just been cleared, so neither
		//of them holds a previous frame any more.
		framesSeeded     = false;
		pictureWidthWas  = pictureWidth;
		pictureHeightWas = pictureHeight;
	}

	const bool shapeMoved = wantLength != ringLength || wantRows != ringRows;

	//Nearest on the ring, always. A column is one instant, and a filtered read
	//between two columns would return a picture of a moment that was never
	//sampled. The strip pass uses texelFetch anyway, which ignores the filter
	//entirely; this is the belt to that pair of braces, and it is what the
	//buffer would need if anything ever read it with `texture`.
	if( !ring.Ensure( wantLength, wantRows, GL_RGBA8, PassBuffer::Sampling::Nearest ) )
		return false;

	if( shapeMoved )
	{
		//The ring's indexing is modulo its own length, so changing the length
		//does not shuffle its contents -- it REINTERPRETS them. Every column
		//would still hold a real instant and every one of them would be filed
		//under the wrong time, which shows as the strip jumping to a different
		//arrangement of the same moments. Starting empty costs a sweep and is
		//the only answer that is not wrong. Controls::SweepColumns quantises
		//the slider so that a drag clears a handful of times rather than on
		//every pixel.
		ring.Clear();
		ringLength  = wantLength;
		ringRows    = wantRows;
		writePos    = 0;
		filled      = 0;
		columnPhase = 0.0;

		diag::info( "ring rebuilt: " + std::to_string( wantLength ) + " columns x "
		            + std::to_string( wantRows ) + " rows" );
	}

	return true;
}

//---------------------------------------------------------------------------
double Photofinish::ColumnPeriod() const
{
	if( columnPeriodOverride > 0.0 )
		return columnPeriodOverride;

	const int sync = optionValue( params[ PT_SYNC ], kSyncCount );

	if( sync == kSyncFree || ringLength <= 0 )
		return controls::TimePerColumnSeconds( params[ PT_TIME_PER_COLUMN ] );

	//The tempo the host is running at. It always sends something -- Resolume
	//calls SetBeatInfo unconditionally and the SDK defaults to 120/0 -- but a
	//host that never does would leave bpm at zero and make a bar infinitely
	//long, so it is guarded.
	const double tempo      = hostBpm > 1.0f ? static_cast< double >( hostBpm ) : 120.0;
	const double barSeconds = 240.0 / tempo;//four beats to the bar
	const double sweep      = sync == kSyncBar ? barSeconds : barSeconds / 4.0;

	//A RATE, not a phase lock. The host's bar phase is the only phase FFGL
	//exposes, and locking the write head to it would make the head JUMP on a
	//tempo change, on a scrub, and on any host that does not send it -- and a
	//head that jumps tears the strip in a way that reads as a broken plugin.
	//Deriving the rate means one sweep takes one bar and nothing ever jumps.
	//See AGENTS.md: the sweep is not aligned to the bar LINE, and that is
	//stated rather than hidden.
	return sweep / static_cast< double >( ringLength );
}

//---------------------------------------------------------------------------
double Photofinish::NormalisedNow()
{
	const double wallNow = wallSeconds();
	if( wallStart < 0.0 )
		wallStart = wallNow;

	const double raw = hostTime;

	//---------------------------------------------------------------------
	// Host clock units.
	//
	// The FFGL header never says what unit SetTime is in, and hosts disagree:
	// Resolume hands over MILLISECONDS (measured live, and the SDK's own
	// Particles sample divides by 1000), while the offline harness -- and any
	// host following the header's silence -- sends seconds. steady_clock says
	// how much real time passed, the host says how much host time passed, and
	// the ratio names the unit outright: 1 for seconds, 1000 for milliseconds,
	// and nothing plausible in between.
	//
	// It matters more here than anywhere else in the fleet. Get it wrong by a
	// factor of a thousand and every frame asks for a thousand times too many
	// columns, which is not a subtly wrong picture -- it is a whole ring
	// rewritten from two frames, every frame.
	//---------------------------------------------------------------------
	if( clockScale == 0.0 && raw >= 0.0 && lastRawTime >= 0.0 && lastWallTime >= 0.0 )
	{
		const double hostDelta = raw - lastRawTime;
		const double wallDelta = wallNow - lastWallTime;

		//A paused host, a looping clip or a stalled frame tells us nothing.
		if( hostDelta > 0.0 && wallDelta >= 0.0005 )
		{
			const double ratio = hostDelta / wallDelta;
			if( ratio > 0.1 && ratio < 10.0 )
				++secondsVotes;
			else if( ratio > 100.0 && ratio < 10000.0 )
				++millisVotes;

			//Several frames rather than one, so a single odd frame cannot
			//decide it alone.
			if( secondsVotes >= kClockVotes || millisVotes >= kClockVotes )
				clockScale = millisVotes > secondsVotes ? 0.001 : 1.0;
		}
	}

	if( raw >= 0.0 )
		lastRawTime = raw;
	lastWallTime = wallNow;

	//Until the unit is settled -- and for a host that never calls SetTime --
	//run on the real clock: wrong in origin but right in rate, where assuming
	//seconds would be a thousand times fast on Resolume. Only differences of
	//this are ever used, so a wrong origin costs nothing at all.
	return ( raw >= 0.0 && clockScale != 0.0 ) ? raw * clockScale : wallNow - wallStart;
}

//---------------------------------------------------------------------------
FFResult Photofinish::ProcessOpenGL( ProcessOpenGLStruct* pGL )
{
	if( pGL->numInputTextures < 1 || pGL->inputTextures[ 0 ] == nullptr )
		return FF_FAIL;

	const FFGLTextureStruct& picture = *pGL->inputTextures[ 0 ];
	if( picture.Width == 0 || picture.Height == 0 )
		return FF_FAIL;

	const int pictureWidth  = static_cast< int >( picture.Width );
	const int pictureHeight = static_cast< int >( picture.Height );

	//The host's viewport, read before anything of ours changes it.
	//
	//`ScopedFBOBinding` restores the framebuffer binding and *only* the
	//framebuffer binding -- it does not touch the viewport (SDK b1afaf9,
	//FFGLScopedFBOBinding.cpp). So every pass's viewport leaks out into the
	//pass after it, and the strip pass, which draws to the host's own
	//framebuffer and so has no buffer of its own to size itself from, would
	//inherit whatever the last pass left. Here that would be the slit pass's
	//ONE-COLUMN viewport, and the effect would render a single column of the
	//frame and leave the rest untouched.
	GLint hostViewport[ 4 ] = { 0, 0, 0, 0 };
	glGetIntegerv( GL_VIEWPORT, hostViewport );

	//---------------------------------------------------------------------
	// Time. Differences only -- see Strip.h.
	//---------------------------------------------------------------------
	const double now = NormalisedNow();
	const double dt  = lastNow >= 0.0 ? std::clamp( now - lastNow, 0.0, strip::kMaxFrameDelta )
	                                  : 0.0;

	if( ++clockFrames == 60 )
	{
		//Once, at frame 60. The clock's unit and the host's transport are both
		//invisible from the picture and both change what every column means --
		//and the bar phase is here rather than merely being accepted because
		//whether a host sends one at all is the whole question behind a v0.2
		//bar-line lock. See AGENTS.md.
		diag::info( "host clock at frame 60: raw=" + std::to_string( hostTime )
		            + " scale=" + std::to_string( clockScale )
		            + " seconds=" + std::to_string( now )
		            + " bpm=" + std::to_string( hostBpm )
		            + " barPhase=" + std::to_string( hostBarPhase ) );
	}

	lastNow = now;

	//---------------------------------------------------------------------
	// What the controls say.
	//---------------------------------------------------------------------
	const int axis       = optionValue( params[ PT_AXIS ], kAxisCount );
	const int direction  = optionValue( params[ PT_DIRECTION ], kDirectionCount );
	const int fill       = optionValue( params[ PT_FILL ], kFillCount );
	const int background = optionValue( params[ PT_BACKGROUND ], kBackgroundCount );
	const int interp     = optionValue( params[ PT_INTERPOLATE ], kInterpolateCount );
	const bool frozen    = params[ PT_FREEZE ] >= 0.5f;

	const bool vertical  = axis == kAxisVertical;
	const int scanPixels = vertical ? pictureHeight : pictureWidth;
	const int slitPixels = vertical ? pictureWidth : pictureHeight;

	const int wantLength = controls::SweepColumns( params[ PT_SWEEP_LENGTH ], scanPixels );
	const int wantRows   = std::max( 1, slitPixels );

	//---------------------------------------------------------------------
	// Buffers.
	//
	// Every Ensure() happens here, before anything binds a texture. That is
	// not tidiness: ffglex::FFGLFBO::Initialise sizes its new colour texture
	// under a ScopedTextureBinding, and every ffglex Scoped* binding *clears*
	// to 0 on scope exit rather than restoring what was there. Allocating a
	// buffer therefore unbinds the input texture from the active unit, and the
	// symptom is the dangerous part -- correct on every frame except the one
	// that allocates.
	//---------------------------------------------------------------------
	if( !EnsureBuffers( pictureWidth, pictureHeight, wantLength, wantRows ) )
	{
		diag::error( "could not allocate the ring: " + std::to_string( wantLength ) + " x "
		             + std::to_string( wantRows ) + " - try a shorter Sweep Length" );
		return FF_FAIL;
	}

	const double columnPeriod = ColumnPeriod();

	//---------------------------------------------------------------------
	// 1. The picture, into one of the two frame copies.
	//---------------------------------------------------------------------
	frameIndex          = 1 - frameIndex;
	PassBuffer& frameNow = frames[ frameIndex ];
	PassBuffer& frameWas = frames[ 1 - frameIndex ];

	const FFGLTexCoords maxCoords = GetMaxGLTexCoords( picture );
	auto copyInto                 = [ & ]( PassBuffer& target ) {
        ScopedFBOBinding fbo( target.GetGLID(), ScopedFBOBinding::RB_REVERT );
        target.ResizeViewPort();
        ScopedShaderBinding shader( copyShader.GetGLID() );
        ScopedSamplerActivation sampler( 0 );
        Scoped2DTextureBinding texture( picture.Handle );

        copyShader.Set( "SourceTexture", 0 );
        copyShader.Set( "MaxUV", maxCoords.s, maxCoords.t );
        copyShader.Set( "HalfTexel",
                        0.5f / static_cast< float >( pictureWidth ),
                        0.5f / static_cast< float >( pictureHeight ) );
        quad.Draw();
	};

	copyInto( frameNow );

	if( !framesSeeded )
	{
		//The first frame has no previous frame, and an unseeded one is not
		//"black for a frame" -- it is whatever the buffer was cleared to,
		//blended into every column taken in that frame. At the fastest column
		//rate that is a third of the ring.
		copyInto( frameWas );
		framesSeeded = true;
	}

	//---------------------------------------------------------------------
	// 2. The columns this frame owes.
	//---------------------------------------------------------------------
	const double phaseBefore     = columnPhase;
	const strip::Schedule sched  = frozen ? strip::Schedule { 0, columnPhase }
	                                      : strip::advance( columnPhase, dt, columnPeriod );
	columnPhase                  = sched.phase;

	//Never more than a whole ring in one frame: past that the later columns
	//write over work done earlier in the same frame, so the extra draws cost
	//time and change nothing.
	int columns = std::min( sched.columns, ringLength );

	//Once is the mode that stops. Build and Scroll wrap for ever.
	if( fill == kFillOnce && filled + columns > ringLength )
		columns = std::max( 0, ringLength - filled );

	if( columns > 0 )
	{
		ScopedFBOBinding fbo( ring.GetGLID(), ScopedFBOBinding::RB_REVERT );
		ScopedShaderBinding shader( slitShader.GetGLID() );

		ScopedSamplerActivation sampler0( 0 );
		Scoped2DTextureBinding nowTexture( frameNow.TextureID() );
		ScopedSamplerActivation sampler1( 1 );
		Scoped2DTextureBinding wasTexture( frameWas.TextureID() );

		const int taps = controls::SlitWidthPixels( params[ PT_SLIT_WIDTH ] );

		//The slit's width and lean are in normalised units of the axis the
		//slit cuts ACROSS, which is the picture's width for a horizontal sweep
		//and its height for a vertical one. So a 16-pixel slit is 16 pixels on
		//either axis rather than 16 units of whichever happens to be longer.
		const float acrossPixels = static_cast< float >( vertical ? pictureHeight : pictureWidth );

		slitShader.Set( "FrameNow", 0 );
		slitShader.Set( "FrameWas", 1 );
		slitShader.Set( "TimeIsLinear", interp == kInterpolateLinear ? 1.0f : 0.0f );
		slitShader.Set( "SlitCentre", controls::SlitPosition( params[ PT_SLIT_POSITION ] ) );
		slitShader.Set( "SlitSpan", static_cast< float >( taps ) / acrossPixels );
		slitShader.Set( "SlitSlope", controls::SlitLean( params[ PT_SLIT_ANGLE ] ) );
		slitShader.Set( "AxisVertical", vertical ? 1.0f : 0.0f );
		slitShader.Set( "TapCount", taps );
		slitShader.Set( "HalfTexel",
		                0.5f / static_cast< float >( pictureWidth ),
		                0.5f / static_cast< float >( pictureHeight ) );

		for( int i = 0; i < columns; ++i )
		{
			const double blend = strip::blendAt( i, phaseBefore, dt, columnPeriod );
			slitShader.Set( "Blend", static_cast< float >( blend ) );

			//ONE COLUMN of the ring. The quad covers the whole of NDC, so a
			//viewport one pixel wide rasterises exactly this column and
			//nothing else -- which is also why the slit pass's uv.x is 0.5 at
			//every fragment and carries no information.
			glViewport( writePos, 0, 1, ringRows );
			quad.Draw();

			writePos = ( writePos + 1 ) % ringLength;
			if( filled < ringLength )
				++filled;
		}
	}

	//---------------------------------------------------------------------
	// 3. The strip, straight to the host's framebuffer.
	//---------------------------------------------------------------------
	{
		//Back to the host's viewport. See the note where it was captured --
		//without this the strip pass inherits the slit pass's one-column
		//viewport.
		glViewport( hostViewport[ 0 ], hostViewport[ 1 ], hostViewport[ 2 ], hostViewport[ 3 ] );

		ScopedShaderBinding shader( stripShader.GetGLID() );

		ScopedSamplerActivation sampler0( 0 );
		Scoped2DTextureBinding ringTexture( ring.TextureID() );
		ScopedSamplerActivation sampler1( 1 );
		Scoped2DTextureBinding sourceTexture( frameNow.TextureID() );

		stripShader.Set( "RingTexture", 0 );
		stripShader.Set( "SourceTexture", 1 );
		stripShader.Set( "RingLength", ringLength );
		stripShader.Set( "RingRows", ringRows );
		stripShader.Set( "WritePos", writePos );
		stripShader.Set( "FilledCount", filled );
		stripShader.Set( "FillMode", fill );
		stripShader.Set( "BackgroundMode", background );
		stripShader.Set( "AxisVertical", vertical ? 1.0f : 0.0f );
		stripShader.Set( "Reversed", direction == kDirectionReverse ? 1.0f : 0.0f );
		stripShader.Set( "MixAmount", std::clamp( params[ PT_MIX ], 0.0f, 1.0f ) );
		stripShader.Set( "HalfTexel",
		                 0.5f / static_cast< float >( pictureWidth ),
		                 0.5f / static_cast< float >( pictureHeight ) );
		quad.Draw();
	}

	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
FFResult Photofinish::DeInitGL()
{
	copyShader.FreeGLResources();
	slitShader.FreeGLResources();
	stripShader.FreeGLResources();
	quad.Release();

	frames[ 0 ].Destroy();
	frames[ 1 ].Destroy();
	ring.Destroy();

	ringLength       = 0;
	ringRows         = 0;
	writePos         = 0;
	filled           = 0;
	columnPhase      = 0.0;
	framesSeeded     = false;
	pictureWidthWas  = 0;
	pictureHeightWas = 0;

	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
FFResult Photofinish::SetFloatParameter( unsigned int index, float value )
{
	if( index >= PT_COUNT )
		return FF_FAIL;

	// The About buttons open a browser and store nothing, so they are handled
	// before the params[] write below -- there is no value to keep.
	if( index >= PT_ABOUT_FIRST )
		return stoatworks::about::handleParam( index - PT_ABOUT_FIRST, value ) ? FF_SUCCESS : FF_FAIL;

	params[ index ] = value;
	return FF_SUCCESS;
}

float Photofinish::GetFloatParameter( unsigned int index )
{
	if( index >= PT_COUNT )
		return 0.0f;

	return params[ index ];
}

//---------------------------------------------------------------------------
char* Photofinish::GetTextParameter( unsigned int index )
{
	if( index == PT_ABOUT_FIRST )
	{
		aboutText = stoatworks::about::textParam( 0 );
		return const_cast< char* >( aboutText.c_str() );
	}

	return CFFGLPlugin::GetTextParameter( index );
}

FFResult Photofinish::SetTextParameter( unsigned int index, const char* value )
{
	// See the declaration: the base class fails, and a failed default deletes
	// the instance. The About line is display-only, so there is genuinely
	// nothing to store -- but it has to say so successfully.
	if( index == PT_ABOUT_FIRST )
		return FF_SUCCESS;

	return CFFGLPlugin::SetTextParameter( index, value );
}

FFResult Photofinish::SetTime( double time )
{
	hostTime = time;
	return FF_SUCCESS;
}

void Photofinish::SetBeatInfo( float bpm, float barPhase )
{
	hostBpm      = bpm;
	hostBarPhase = barPhase;
}

void Photofinish::SetClockScaleForTest( double scale )
{
	clockScale = scale;
}

void Photofinish::SetColumnPeriodForTest( double seconds )
{
	columnPeriodOverride = seconds;
}
