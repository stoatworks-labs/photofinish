/**
	pftest -- render Photofinish offline, and measure what its strip is doing.

	What a strip camera does to a moving object is a fact with a closed form.
	The horizontal axis of the output is time, so an object of width `b` moving
	at `v` source pixels per frame, photographed at `c` columns per frame,
	occupies

	    b * c / v   columns

	and nothing about that depends on the machine it was rendered on. Every
	numeric check below is built from that one relation or from the ring's own
	integer bookkeeping, and every tolerance is derived from a lattice -- a
	column, a source frame, an 8-bit code value -- rather than fitted to what
	this Mac printed first. AGENTS.md has the audit, check by check.

		pftest --out /tmp/f.png    render the demo card through the plugin
		pftest --card /tmp/c.png   the demo card on its own
		pftest --list              every parameter, its kind and its range
		pftest --schedule          the column schedule, with no GL at all
		pftest --static            a still picture is exact horizontal streaks
		pftest --ring              the ring holds the last min(N, L) columns
		pftest --clock             the same take from t=0 and from t=499217238ms
		pftest --interp            a column between two frames is their blend
		pftest --width             a moving bar's rendered width, in closed form
		pftest --matched           at the film speed, true proportions
		pftest --reverse           the other way round comes out mirrored
		pftest --negative          the checks above can actually fail
		pftest --bench             the render cost, 720p through 4K

	**Every check that can be raster-sensitive runs at two rasters on
	purpose**, one of them small enough to resemble a runner with no GPU. Last
	round four of six plugins in this fleet shipped numbers calibrated to this
	machine's rasteriser; in all four cases the test was wrong, not the plugin.
*/

#include "Controls.h"
#include "Photofinish.h"
#include "Strip.h"

#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <zlib.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace photofinish;

namespace
{
//---------------------------------------------------------------------------
// Tolerances, all in one place, each with the lattice it comes from.
// AGENTS.md explains every one of them; the numbers live here.
//---------------------------------------------------------------------------

/// ONE COLUMN. The strip cannot represent anything finer than a column, so a
/// width measured off it cannot be asserted finer than one either. Used by
/// --width and --matched, and the analytic error budget each of them prints
/// has to come out comfortably inside it or the check is not measuring what it
/// says it is.
constexpr double kColumnTolerance = 1.0;

/// ONE 8-BIT CODE VALUE. The ring is GL_RGBA8 because what arrives from the
/// host is eight bits per channel, so a blend written into it lands on a code
/// and the only question is which way the last one rounded.
constexpr int kCodeTolerance = 1;

/// Skewness is dimensionless, so this one cannot come from a lattice in the
/// same way. It is a stated number and --reverse computes the worst-case
/// sampling-phase error at its own column spacing and asserts that the derived
/// figure sits inside it.
constexpr double kSkewTolerance = 0.03;

/// The clock origin the fleet has measured Resolume reach, in milliseconds.
/// A float's spacing here is about 0.03 s. See Strip.h.
constexpr double kFarClockMs = 499217238.0;

//---------------------------------------------------------------------------
// A PNG writer. zlib ships with the OS, so this is a few chunk headers and a
// CRC rather than a dependency.
//---------------------------------------------------------------------------
void putU32( std::vector< unsigned char >& out, uint32_t value )
{
	out.push_back( static_cast< unsigned char >( value >> 24 ) );
	out.push_back( static_cast< unsigned char >( value >> 16 ) );
	out.push_back( static_cast< unsigned char >( value >> 8 ) );
	out.push_back( static_cast< unsigned char >( value ) );
}

void putChunk( std::vector< unsigned char >& out, const char* type,
               const std::vector< unsigned char >& data )
{
	putU32( out, static_cast< uint32_t >( data.size() ) );
	const size_t start = out.size();
	out.insert( out.end(), type, type + 4 );
	out.insert( out.end(), data.begin(), data.end() );
	uLong crc = crc32( 0L, Z_NULL, 0 );
	crc       = crc32( crc, out.data() + start, static_cast< uInt >( 4 + data.size() ) );
	putU32( out, static_cast< uint32_t >( crc ) );
}

bool writePng( const std::string& path, int width, int height,
               const std::vector< unsigned char >& rgba )
{
	std::vector< unsigned char > raw;
	raw.reserve( static_cast< size_t >( height ) * ( 1 + static_cast< size_t >( width ) * 4 ) );
	for( int y = 0; y < height; ++y )
	{
		raw.push_back( 0 );//filter: none
		const unsigned char* row = rgba.data() + static_cast< size_t >( y ) * width * 4;
		raw.insert( raw.end(), row, row + static_cast< size_t >( width ) * 4 );
	}

	uLongf compressedSize = compressBound( static_cast< uLong >( raw.size() ) );
	std::vector< unsigned char > compressed( compressedSize );
	if( compress2( compressed.data(), &compressedSize, raw.data(),
	               static_cast< uLong >( raw.size() ), 6 ) != Z_OK )
		return false;
	compressed.resize( compressedSize );

	std::vector< unsigned char > png = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };

	std::vector< unsigned char > ihdr;
	putU32( ihdr, static_cast< uint32_t >( width ) );
	putU32( ihdr, static_cast< uint32_t >( height ) );
	ihdr.push_back( 8 );//bit depth
	ihdr.push_back( 6 );//truecolour with alpha
	ihdr.push_back( 0 );
	ihdr.push_back( 0 );
	ihdr.push_back( 0 );
	putChunk( png, "IHDR", ihdr );
	putChunk( png, "IDAT", compressed );
	putChunk( png, "IEND", {} );

	FILE* file = fopen( path.c_str(), "wb" );
	if( file == nullptr )
		return false;
	const size_t written = fwrite( png.data(), 1, png.size(), file );
	fclose( file );
	return written == png.size();
}

//---------------------------------------------------------------------------
// Source frames.
//
// EVERYTHING here is built with row 0 at the BOTTOM, which is GL's convention
// and the plugin's. Only the PNG writer flips, and it flips once. A harness
// that flipped in two places would have a check that passed for the wrong
// reason every time the two cancelled.
//---------------------------------------------------------------------------
using Frame = std::vector< unsigned char >;

Frame blackFrame( int width, int height )
{
	Frame f( static_cast< size_t >( width ) * height * 4, 0 );
	for( size_t i = 3; i < f.size(); i += 4 )
		f[ i ] = 255;
	return f;
}

/// A uniform colour. Used by every check that wants the slit's position and
/// the source's filtering to be irrelevant: on a flat field a fetch anywhere
/// returns the same number, so what is left under test is the SCHEDULE and the
/// RING and nothing else.
Frame flatFrame( int width, int height, int r, int g, int b )
{
	Frame f( static_cast< size_t >( width ) * height * 4 );
	for( size_t i = 0; i < f.size(); i += 4 )
	{
		f[ i + 0 ] = static_cast< unsigned char >( r );
		f[ i + 1 ] = static_cast< unsigned char >( g );
		f[ i + 2 ] = static_cast< unsigned char >( b );
		f[ i + 3 ] = 255;
	}
	return f;
}

/// A flat field carrying the frame's own index, sixteen bits of it, so a
/// column read back out of the strip says which frame it was taken from.
Frame stampedFrame( int width, int height, int index )
{
	return flatFrame( width, height, index & 0xFF, ( index >> 8 ) & 0xFF, 128 );
}

int stampOf( const unsigned char* pixel )
{
	return static_cast< int >( pixel[ 0 ] ) + 256 * static_cast< int >( pixel[ 1 ] );
}

//---------------------------------------------------------------------------
// The moving bar, and its profile.
//
// The profile is a box of width `b` convolved with a normalised box of width
// `R`: a trapezoid, flat on top, with ramps `R` wide, whose integral is
// EXACTLY `b` whatever `R` is.
//
// That shape is not decoration. Sampling it at a spacing that divides `R`
// gives a sum equal to its integral for EVERY sampling phase -- the ramps are
// a partition of unity -- so the measured width is translation invariant on
// the lattice rather than wobbling by half a column depending on where the bar
// happened to start. Sampling a hard-edged bar instead would leave a genuine
// half-column of phase noise in every reading, which is exactly the kind of
// number that gets absorbed into a tolerance and then fails somewhere else.
//---------------------------------------------------------------------------
double barProfile( double x, double centre, double b, double ramp )
{
	const double d = std::fabs( x - centre );
	return std::clamp( ( ( b + ramp ) * 0.5 - d ) / ramp, 0.0, 1.0 );
}

/// Full-height vertical bar on black, at a sub-pixel centre.
Frame barFrame( int width, int height, double centre, double b, double ramp )
{
	Frame f = blackFrame( width, height );
	for( int x = 0; x < width; ++x )
	{
		const double value = barProfile( static_cast< double >( x ) + 0.5, centre, b, ramp );
		const unsigned char code =
			static_cast< unsigned char >( std::lround( std::clamp( value, 0.0, 1.0 ) * 255.0 ) );
		for( int y = 0; y < height; ++y )
		{
			const size_t i = ( static_cast< size_t >( y ) * width + x ) * 4;
			f[ i + 0 ] = code;
			f[ i + 1 ] = code;
			f[ i + 2 ] = code;
			f[ i + 3 ] = 255;
		}
	}
	return f;
}

//---------------------------------------------------------------------------
// The wedge: an object that is not symmetric, so that reversing it is visible.
//
// It ramps linearly from nothing at its left edge to full at its right, then
// falls away over `ramp`. Most of its mass is at the right, so as a
// distribution it has a long tail to the LEFT and its skewness is negative.
// Reversed, positive. Nothing here needs to know the closed form -- the
// harness integrates this very function to get the number it expects.
//---------------------------------------------------------------------------
double wedgeProfile( double x, double left, double b, double ramp )
{
	const double rise = std::clamp( ( x - left ) / b, 0.0, 1.0 );
	const double fall = std::clamp( ( left + b + ramp - x ) / ramp, 0.0, 1.0 );
	return rise * fall;
}

Frame wedgeFrame( int width, int height, double left, double b, double ramp )
{
	Frame f = blackFrame( width, height );
	for( int x = 0; x < width; ++x )
	{
		const double value = wedgeProfile( static_cast< double >( x ) + 0.5, left, b, ramp );
		const unsigned char code =
			static_cast< unsigned char >( std::lround( std::clamp( value, 0.0, 1.0 ) * 255.0 ) );
		for( int y = 0; y < height; ++y )
		{
			const size_t i = ( static_cast< size_t >( y ) * width + x ) * 4;
			f[ i + 0 ] = code;
			f[ i + 1 ] = code;
			f[ i + 2 ] = code;
			f[ i + 3 ] = 255;
		}
	}
	return f;
}

//---------------------------------------------------------------------------
// The demo card, for --out and for the sweep.
//
// It MOVES, and for this plugin that is not a nicety -- it is the whole
// subject. A still card is a picture the effect turns into perfectly flat
// horizontal streaks, which is correct, proves the mechanism, and would make
// every control in tools/sweep.py read as dead.
//
// Everything that moves travels at a speed in SOURCE PIXELS PER FRAME, not in
// fractions of the picture, because the whole point is what each one's speed
// is relative to the film. At the defaults -- four columns per frame at 60 fps
// -- the card demonstrates all four claims at once:
//
//   a disc at 4 px/frame        matched: comes out round, its own proportions
//   an arrow at 4 px/frame      mirrored: its leading point crosses first, so
//                               it lands at the OLD end of the strip and the
//                               arrow comes out pointing the other way
//   the same arrow at -4        not mirrored: the famous backwards runner is
//                               the one going the other way, and this is it
//   a bar at 12 px/frame        three times the film speed, so a third as wide
//   colour bars, a ramp, fine
//   stripes and a diagonal      static: flat horizontal streaks
//
// The stripes are there for Slit Width, which averages ACROSS the direction of
// travel and so needs detail there to average; the diagonal is there for Slit
// Angle, which is invisible on anything that does not change along the slit.
//
// The moving things wrap, so a take long enough to fill the strip holds
// several crossings rather than one and then nothing.
//---------------------------------------------------------------------------

/// A right-pointing triangle: a vertical edge at `left`, tapering to a point
/// at `left + length`. `band` is 0 at the bottom of its lane and 1 at the top.
bool inArrow( double x, double band, double left, double length )
{
	if( band < 0.0 || band > 1.0 )
		return false;
	const double reach = length * ( 1.0 - std::fabs( 2.0 * band - 1.0 ) );
	return x >= left && x <= left + reach;
}

Frame demoCard( int width, int height, int frame )
{
	Frame card( static_cast< size_t >( width ) * height * 4 );

	const double w = static_cast< double >( width );
	const double h = static_cast< double >( height );
	const double t = static_cast< double >( frame );

	const double arrowLength = 0.12 * w;
	const double discRadius  = std::min( 0.10 * h, 0.06 * w );

	//Wrapped over the picture's width plus the object's own length, so an
	//object leaves on one side before it reappears on the other.
	auto wrapped = []( double x, double span ) {
		const double m = std::fmod( x, span );
		return m < 0.0 ? m + span : m;
	};

	const double arrowRightX = wrapped( 0.05 * w + 4.0 * t, w + arrowLength ) - arrowLength;
	const double arrowLeftX  = w - wrapped( 0.20 * w + 4.0 * t, w + arrowLength );
	const double discX       = wrapped( 0.86 * w + 4.0 * t, w + 2.0 * discRadius ) - discRadius;
	const double barX        = w - wrapped( 0.35 * w + 12.0 * t, w + 0.02 * w );
	const double barHalf     = 0.010 * w;

	for( int y = 0; y < height; ++y )
	{
		//v = 0 at the BOTTOM, matching GL and the plugin.
		const double v  = ( static_cast< double >( y ) + 0.5 ) / h;
		const double py = static_cast< double >( y ) + 0.5;

		for( int x = 0; x < width; ++x )
		{
			const double u  = ( static_cast< double >( x ) + 0.5 ) / w;
			const double px = static_cast< double >( x ) + 0.5;

			double r = 0.05;
			double g = 0.05;
			double b = 0.07;

			//Six saturated bars across the bottom tenth. Static: streaks.
			if( v < 0.10 )
			{
				static const double bars[ 6 ][ 3 ] = {
					{ 1.0, 0.1, 0.1 }, { 0.1, 1.0, 0.1 }, { 0.1, 0.1, 1.0 },
					{ 0.1, 1.0, 1.0 }, { 1.0, 0.1, 1.0 }, { 1.0, 1.0, 0.1 }
				};
				const int bar = std::min( 5, static_cast< int >( u * 6.0 ) );
				r = bars[ bar ][ 0 ];
				g = bars[ bar ][ 1 ];
				b = bars[ bar ][ 2 ];
			}
			//A smooth ramp above them.
			else if( v < 0.17 )
			{
				r = g = b = u;
			}
			//Fine vertical stripes, four source pixels apart: the only thing
			//in the card a wide slit has to average.
			else if( v > 0.88 )
			{
				const bool on = ( x / 4 ) % 2 == 0;
				r = g = b = on ? 0.85 : 0.08;
			}

			//A static diagonal. Slit Angle changes which x each row of the
			//slit reads, so it needs something that varies along the slit AND
			//across it; a diagonal is the smallest thing that does both.
			if( std::fabs( u - ( 0.22 + 0.30 * v ) ) < 0.005 )
			{
				r = 0.95;
				g = 0.80;
				b = 0.20;
			}

			//The arrow going left, in the lower lane.
			if( inArrow( px, ( py - 0.19 * h ) / ( 0.21 * h ), arrowLeftX, arrowLength ) )
			{
				r = 0.30;
				g = 0.85;
				b = 0.55;
			}

			//The arrow going right, in the upper lane. Same shape, so the two
			//of them side by side in the strip are the whole claim.
			if( inArrow( px, ( py - 0.42 * h ) / ( 0.21 * h ), arrowRightX, arrowLength ) )
			{
				r = 1.00;
				g = 0.55;
				b = 0.25;
			}

			//The disc, at exactly the default film speed, with a soft shoulder
			//so it does not alias as it travels.
			const double dx   = px - discX;
			const double dy   = py - 0.77 * h;
			const double dist = std::sqrt( dx * dx + dy * dy );
			if( dist < discRadius )
			{
				const double edge = std::min( 1.0, ( discRadius - dist ) / ( discRadius * 0.15 ) );
				r = r + ( 1.00 - r ) * edge;
				g = g + ( 0.90 - g ) * edge;
				b = b + ( 0.70 - b ) * edge;
			}

			//The fast bar, crossing the other way at three times the film
			//speed. Straight edges, so the squash is readable.
			if( std::fabs( px - barX ) < barHalf && v > 0.18 && v < 0.87 )
			{
				r = 0.35;
				g = 0.65;
				b = 1.00;
			}

			const size_t i = ( static_cast< size_t >( y ) * width + x ) * 4;
			card[ i + 0 ] = static_cast< unsigned char >( std::clamp( r, 0.0, 1.0 ) * 255.0 + 0.5 );
			card[ i + 1 ] = static_cast< unsigned char >( std::clamp( g, 0.0, 1.0 ) * 255.0 + 0.5 );
			card[ i + 2 ] = static_cast< unsigned char >( std::clamp( b, 0.0, 1.0 ) * 255.0 + 0.5 );
			card[ i + 3 ] = 255;
		}
	}

	return card;
}

//---------------------------------------------------------------------------
// GL plumbing.
//---------------------------------------------------------------------------
CGLContextObj createContext()
{
	//Accelerated first; fall back so the harness still runs somewhere without
	//a GPU, where it will at least prove the shaders compile and -- because
	//nothing here asserts on a filtered fetch -- should still pass every
	//numeric check.
	const CGLPixelFormatAttribute accelerated[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAAccelerated,
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};
	const CGLPixelFormatAttribute software[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};

	CGLPixelFormatObj format = nullptr;
	GLint formatCount        = 0;
	if( CGLChoosePixelFormat( accelerated, &format, &formatCount ) != kCGLNoError || format == nullptr )
	{
		if( CGLChoosePixelFormat( software, &format, &formatCount ) != kCGLNoError || format == nullptr )
			return nullptr;
	}

	CGLContextObj context = nullptr;
	const CGLError error  = CGLCreateContext( format, nullptr, &context );
	CGLDestroyPixelFormat( format );
	if( error != kCGLNoError )
		return nullptr;

	CGLSetCurrentContext( context );
	return context;
}

GLuint makeTexture( int width, int height, const unsigned char* pixels )
{
	GLuint texture = 0;
	glGenTextures( 1, &texture );
	glBindTexture( GL_TEXTURE_2D, texture );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glBindTexture( GL_TEXTURE_2D, 0 );
	return texture;
}

GLuint makeFramebuffer( GLuint texture )
{
	GLuint fbo = 0;
	glGenFramebuffers( 1, &fbo );
	glBindFramebuffer( GL_FRAMEBUFFER, fbo );
	glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0 );
	return fbo;
}

Frame flipRows( const Frame& image, int width, int height )
{
	Frame flipped( image.size() );
	const size_t stride = static_cast< size_t >( width ) * 4;
	for( int y = 0; y < height; ++y )
		std::memcpy( flipped.data() + static_cast< size_t >( y ) * stride,
		             image.data() + static_cast< size_t >( height - 1 - y ) * stride, stride );
	return flipped;
}

//---------------------------------------------------------------------------
// The rig: the real plugin class, a synthetic clock, and a texture to render
// into. Nothing here re-implements any part of the effect.
//---------------------------------------------------------------------------
struct NamedParameter
{
	std::string name;
	unsigned int index = 0;
	float value        = 0.0f;
};

std::vector< NamedParameter > listParameters( Photofinish& plugin )
{
	std::vector< NamedParameter > list;
	for( unsigned int i = 0; i < Photofinish::PT_COUNT; ++i )
	{
		const char* const name = plugin.GetParamName( i );
		list.push_back( NamedParameter { name ? name : "?", i, plugin.GetFloatParameter( i ) } );
	}
	return list;
}

struct Rig
{
	Photofinish plugin;
	int width  = 0;
	int height = 0;
	double fps = 60.0;

	/// Seconds added to every SetTime. The whole point of --clock.
	double clockOrigin = 0.0;

	GLuint sourceTexture = 0;
	GLuint outputTexture = 0;
	GLuint outputFBO     = 0;

	FFGLTextureStruct inputStruct {};
	FFGLTextureStruct* inputs[ 1 ] {};
	ProcessOpenGLStruct process {};
	bool started = false;

	bool begin( int w, int h )
	{
		width  = w;
		height = h;

		//The harness DECLARES its clock unit rather than leaving the plugin's
		//calibration to infer one. An absolute time handed over in a single
		//frame is genuinely ambiguous, and an implicit unit is what let the
		//milliseconds bug through elsewhere in the fleet.
		plugin.SetClockScaleForTest( 1.0 );

		FFGLViewportStruct viewport = {};
		viewport.width              = static_cast< FFUInt32 >( w );
		viewport.height             = static_cast< FFUInt32 >( h );
		if( plugin.InitGL( &viewport ) != FF_SUCCESS )
		{
			std::fprintf( stderr, "InitGL failed -- see the diagnostics log for which shader\n" );
			return false;
		}

		const Frame empty = blackFrame( w, h );
		sourceTexture     = makeTexture( w, h, empty.data() );
		outputTexture     = makeTexture( w, h, nullptr );
		outputFBO         = makeFramebuffer( outputTexture );

		inputStruct.Width = inputStruct.HardwareWidth = static_cast< FFUInt32 >( w );
		inputStruct.Height = inputStruct.HardwareHeight = static_cast< FFUInt32 >( h );
		inputStruct.Handle                              = sourceTexture;
		inputs[ 0 ]                                     = &inputStruct;

		process.numInputTextures = 1;
		process.inputTextures    = inputs;
		process.HostFBO          = outputFBO;

		started = true;
		return true;
	}

	bool set( const std::string& name, float value )
	{
		for( const NamedParameter& p : listParameters( plugin ) )
		{
			if( p.name != name )
				continue;
			plugin.SetFloatParameter( p.index, value );
			return true;
		}
		std::fprintf( stderr, "no parameter called '%s'\n", name.c_str() );
		return false;
	}

	/// Render one frame with this source. `index` drives the synthetic clock;
	/// left to the wall clock the harness would render a hundred frames in a
	/// few milliseconds, so no time would pass, no columns would be produced
	/// and the strip would provably stay empty.
	bool frame( int index, const Frame& source )
	{
		plugin.SetTime( clockOrigin + static_cast< double >( index ) / fps );

		glBindTexture( GL_TEXTURE_2D, sourceTexture );
		glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE,
		                 source.data() );
		glBindTexture( GL_TEXTURE_2D, 0 );

		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glViewport( 0, 0, width, height );
		glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
		glClear( GL_COLOR_BUFFER_BIT );
		return plugin.ProcessOpenGL( &process ) == FF_SUCCESS;
	}

	/// Row 0 is the BOTTOM, as everywhere else here.
	Frame read() const
	{
		Frame pixels( static_cast< size_t >( width ) * height * 4 );
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glPixelStorei( GL_PACK_ALIGNMENT, 1 );
		glReadPixels( 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data() );
		return pixels;
	}

	const unsigned char* at( const Frame& image, int x, int y ) const
	{
		return image.data() + ( static_cast< size_t >( y ) * width + x ) * 4;
	}

	~Rig()
	{
		if( !started )
			return;
		plugin.DeInitGL();
		glDeleteFramebuffers( 1, &outputFBO );
		glDeleteTextures( 1, &outputTexture );
		glDeleteTextures( 1, &sourceTexture );
	}
};

//---------------------------------------------------------------------------
// Measurement.
//---------------------------------------------------------------------------

/// Sum of a row's green channel, in units of full scale. A LINEAR FUNCTIONAL
/// of the picture, which is the entire reason the width checks use it: a
/// linear functional of a translated signal is exact under the translation,
/// where an edge found by interpolating to a 50% crossing is not -- its bias
/// comes from the curvature of the transition and moves with the very thing
/// being measured.
double integrateRow( const Frame& image, int width, int y, int& partialColumns )
{
	double total    = 0.0;
	partialColumns  = 0;
	for( int x = 0; x < width; ++x )
	{
		const int code = image[ ( static_cast< size_t >( y ) * width + x ) * 4 + 1 ];
		total += static_cast< double >( code ) / 255.0;
		if( code > 0 && code < 255 )
			++partialColumns;
	}
	return total;
}

struct Moments
{
	double mass  = 0.0;
	double mean  = 0.0;
	double sigma = 0.0;
	double skew  = 0.0;
};

/// Mass, centroid, width and skewness of a non-negative weight sequence at
/// positions `position[i]`.
///
/// A COVERAGE-WEIGHTED centroid, never a thresholded one. A thresholded
/// centroid quantises to whole columns, so the same object can read half a
/// column further left at one raster than at another for no reason but where
/// the threshold happened to fall.
Moments momentsOf( const std::vector< double >& weight, const std::vector< double >& position )
{
	Moments m;
	double s1 = 0.0;
	for( size_t i = 0; i < weight.size(); ++i )
	{
		m.mass += weight[ i ];
		s1 += position[ i ] * weight[ i ];
	}
	if( m.mass <= 0.0 )
		return m;

	m.mean = s1 / m.mass;

	double s2 = 0.0, s3 = 0.0;
	for( size_t i = 0; i < weight.size(); ++i )
	{
		const double d = position[ i ] - m.mean;
		s2 += d * d * weight[ i ];
		s3 += d * d * d * weight[ i ];
	}

	const double variance = s2 / m.mass;
	m.sigma               = std::sqrt( std::max( variance, 0.0 ) );
	m.skew                = m.sigma > 0.0 ? ( s3 / m.mass ) / ( m.sigma * m.sigma * m.sigma ) : 0.0;
	return m;
}

Moments rowMoments( const Frame& image, int width, int y )
{
	std::vector< double > weight( static_cast< size_t >( width ) );
	std::vector< double > position( static_cast< size_t >( width ) );
	for( int x = 0; x < width; ++x )
	{
		weight[ x ]   = static_cast< double >( image[ ( static_cast< size_t >( y ) * width + x ) * 4 + 1 ] )
		              / 255.0;
		position[ x ] = static_cast< double >( x );
	}
	return momentsOf( weight, position );
}

//---------------------------------------------------------------------------
// Reporting.
//
// Every check also files ONE headline number, and they are printed together at
// the end. That block is what goes in a report and in AGENTS.md: a run that
// only says "all checks passed" tells you nothing about how much margin there
// was, and margin is the only thing that says whether a tolerance is honest.
//---------------------------------------------------------------------------
int g_failures = 0;

struct Headline
{
	std::string check;
	std::string what;
	std::string value;
};
std::vector< Headline > g_headlines;

void headline( const char* check, const char* what, const std::string& value )
{
	g_headlines.push_back( Headline { check, what, value } );
}

std::string figure( const char* format, double value )
{
	char buffer[ 128 ] {};
	std::snprintf( buffer, sizeof( buffer ), format, value );
	return buffer;
}

void printSummary()
{
	if( g_headlines.empty() )
		return;

	std::printf( "\n== summary: the headline number from every check\n" );
	for( const Headline& h : g_headlines )
		std::printf( "   %-10s %-40s %s\n", h.check.c_str(), h.what.c_str(), h.value.c_str() );
}

void ok( const char* what )
{
	std::printf( "   ok    %s\n", what );
}

void bad( const std::string& what )
{
	std::printf( "   FAIL  %s\n", what.c_str() );
	++g_failures;
}

void check( bool condition, const std::string& what )
{
	if( condition )
		ok( what.c_str() );
	else
		bad( what );
}

//---------------------------------------------------------------------------
// --schedule. No GL: this is the arithmetic that decides when a column is
// taken, and it is the one part of the plugin a runner with no GPU can still
// prove.
//---------------------------------------------------------------------------
double g_scheduleWorst = 0.0;

int runSchedule()
{
	std::printf( "== schedule: when a column is taken\n" );

	const double fps    = 60.0;
	const int frames    = 600;
	const double period = 1.0 / fps;

	struct Case
	{
		const char* name;
		double columnsPerFrame;
	};
	const Case cases[] = {
		{ "one column per frame   ", 1.0 },
		{ "four columns per frame ", 4.0 },
		{ "one column per 3 frames", 1.0 / 3.0 },
		{ "pi columns per frame   ", 3.14159265358979323846 },
		{ "33 columns per frame   ", 33.0 },
	};

	for( const Case& c : cases )
	{
		const double tpc = period / c.columnsPerFrame;

		//Driven exactly as the rig drives it: absolute times that are exact
		//quotients, differenced. That is where the one-ulp shortfall comes
		//from, and a schedule test that fed it a clean 1/60 would never see
		//it.
		double phase  = 0.0;
		double lastNow = -1.0;
		long long total = 0;
		int perFrameMin = 1 << 30;
		int perFrameMax = 0;

		for( int k = 0; k < frames; ++k )
		{
			const double now = static_cast< double >( k ) / fps;
			const double dt  = lastNow >= 0.0 ? std::clamp( now - lastNow, 0.0, strip::kMaxFrameDelta )
			                                  : 0.0;
			lastNow = now;

			const strip::Schedule s = strip::advance( phase, dt, tpc );
			phase                   = s.phase;
			total += s.columns;
			if( k > 0 )
			{
				perFrameMin = std::min( perFrameMin, s.columns );
				perFrameMax = std::max( perFrameMax, s.columns );
			}
		}

		//Frame 0 has no previous frame, so 599 frame intervals elapse, and
		//the count is EXACT rather than within a tolerance: a column is taken
		//every time the running total crosses a whole number, so after
		//`intervals * c` columns' worth of time exactly floor( intervals * c )
		//of them have been taken. Asserting the floor rather than "within one
		//column of the product" is the difference between a check that would
		//catch a systematically dropped column and one that would not.
		const double exactly  = c.columnsPerFrame * ( frames - 1 );
		const long long want  = static_cast< long long >( std::floor( exactly + strip::kColumnSnap ) );
		const long long error = total - want;

		std::printf( "   %s  %6lld columns, want exactly %6lld (%8.2f of them elapsed), out by %lld"
		             "  (per frame %d..%d)\n",
		             c.name, total, want, exactly, error, perFrameMin, perFrameMax );

		check( error == 0, std::string( "rate holds exactly: " ) + c.name );
		g_scheduleWorst = std::max( g_scheduleWorst,
		                            static_cast< double >( std::llabs( error ) ) );
	}

	//The regression guard for the snap. With tpc exactly a frame period every
	//frame must produce exactly one column -- no frame producing zero and the
	//next producing two.
	{
		const double tpc = period;
		double phase     = 0.0;
		double lastNow   = -1.0;
		bool everyFrameOne = true;

		for( int k = 0; k < frames; ++k )
		{
			const double now = static_cast< double >( k ) / fps;
			const double dt  = lastNow >= 0.0 ? now - lastNow : 0.0;
			lastNow          = now;

			const strip::Schedule s = strip::advance( phase, dt, tpc );
			phase                   = s.phase;
			if( k > 0 && s.columns != 1 )
				everyFrameOne = false;
		}
		check( everyFrameOne,
		       "a column period equal to a frame period gives exactly one column every frame" );
	}

	//blendAt: the instants within a frame, in closed form.
	{
		const double dt  = period;
		const double tpc = period / 4.0;
		bool exact       = true;
		for( int i = 0; i < 4; ++i )
		{
			const double want = ( i + 1 ) * 0.25;
			exact = exact && std::fabs( strip::blendAt( i, 0.0, dt, tpc ) - want ) < 1e-12;
		}
		check( exact, "four columns in a frame fall at 0.25, 0.50, 0.75 and 1.00 of it" );
	}

	headline( "schedule", "column-count error over 600 frames",
	          figure( "%.0f", g_scheduleWorst ) + " columns (exact, no tolerance)" );
	return 0;
}

//---------------------------------------------------------------------------
// --static. A picture that does not move renders as exact horizontal streaks.
//
// BITWISE, and it is allowed to be: every column in the ring is produced by
// the same arithmetic on the same numbers, so two columns are not close, they
// are equal. The strip pass reads the ring with texelFetch, so no sampler is
// involved even at a magnifying Sweep Length.
//---------------------------------------------------------------------------
int g_staticWorst = 0;
int g_staticCases = 0;

bool staticAt( int width, int height, float sweepLength, float slitWidth, float slitAngle,
               bool vertical, const char* label )
{
	Rig rig;
	if( !rig.begin( width, height ) )
		return false;

	rig.set( "Sweep Length", sweepLength );
	rig.set( "Slit Width", slitWidth );
	rig.set( "Slit Angle", slitAngle );
	rig.set( "Axis", vertical ? 1.0f : 0.0f );
	rig.set( "Fill", static_cast< float >( kFillBuild ) );
	rig.set( "Mix", 1.0f );

	//Four columns a frame at 60 fps, so a ring as long as the axis fills in a
	//quarter of the frames it has columns.
	rig.plugin.SetColumnPeriodForTest( 1.0 / 240.0 );

	const Frame card = demoCard( width, height, 7 );

	const int axis   = vertical ? height : width;
	const int frames = axis / 4 + 8;
	for( int k = 0; k < frames; ++k )
	{
		if( !rig.frame( k, card ) )
		{
			std::fprintf( stderr, "ProcessOpenGL failed on frame %d\n", k );
			return false;
		}
	}

	if( rig.plugin.FilledForTest() != rig.plugin.RingLengthForTest() )
	{
		bad( std::string( label ) + ": the ring did not fill, so the check would be vacuous" );
		return true;
	}

	const Frame out = rig.read();

	long long compared = 0;
	int worst          = 0;
	int worstAt        = -1;

	//Along the scan axis, every sample must equal the first one.
	const int scanLength  = vertical ? height : width;
	const int sliceLength = vertical ? width : height;

	for( int s = 0; s < sliceLength; ++s )
	{
		const unsigned char* first = vertical ? rig.at( out, s, 0 ) : rig.at( out, 0, s );
		for( int t = 1; t < scanLength; ++t )
		{
			const unsigned char* here = vertical ? rig.at( out, s, t ) : rig.at( out, t, s );
			for( int c = 0; c < 4; ++c )
			{
				const int difference = std::abs( static_cast< int >( here[ c ] )
				                                 - static_cast< int >( first[ c ] ) );
				if( difference > worst )
				{
					worst   = difference;
					worstAt = t;
				}
			}
			++compared;
		}
	}

	std::printf( "   %s %lld samples, worst difference %d code value(s)%s\n",
	             label, compared, worst,
	             worstAt >= 0 && worst > 0 ? "" : "" );
	check( worst == 0, std::string( label ) + " is bitwise constant along the time axis" );
	g_staticWorst = std::max( g_staticWorst, worst );
	++g_staticCases;
	return true;
}

int runStatic()
{
	std::printf( "== static: a picture that does not move is exact horizontal streaks\n" );

	//Two rasters on purpose, one of them small enough to resemble a runner
	//with no GPU, and a magnifying Sweep Length at each -- that is where a
	//filtered read of the ring would show up as a gradient between two
	//instants rather than a streak.
	struct Case
	{
		int width, height;
		float sweep, slitWidth, slitAngle;
		bool vertical;
		const char* label;
	};
	const Case cases[] = {
		{ 320, 180, 1.0f, 0.0f, 0.5f, false, "320x180  1:1          " },
		{ 320, 180, 0.6f, 0.0f, 0.5f, false, "320x180  magnified    " },
		{ 320, 180, 1.0f, 0.7f, 0.7f, false, "320x180  wide + leaned" },
		{ 1280, 720, 1.0f, 0.0f, 0.5f, false, "1280x720 1:1          " },
		{ 1280, 720, 0.6f, 0.0f, 0.5f, false, "1280x720 magnified    " },
		{ 640, 360, 1.0f, 0.0f, 0.5f, true, "640x360  vertical axis" },
	};

	for( const Case& c : cases )
		if( !staticAt( c.width, c.height, c.sweep, c.slitWidth, c.slitAngle, c.vertical, c.label ) )
			return 1;

	headline( "static", "worst difference along the time axis",
	          std::to_string( g_staticWorst ) + " code values, over "
	              + std::to_string( g_staticCases ) + " configurations at 3 rasters" );
	return 0;
}

//---------------------------------------------------------------------------
// --ring. After N ticks the ring holds exactly the last min(N, L) columns, in
// order.
//
// Every source frame is a FLAT FIELD carrying its own index, so the slit's
// position, the source's filtering and the picture's raster are all out of the
// question and what is left under test is the ring's bookkeeping. Bitwise:
// eight bits in, eight bits stored, eight bits out.
//---------------------------------------------------------------------------

/// Read the strip's stamps, one per output pixel along the scan axis.
std::vector< int > stampsAlongScan( Rig& rig, const Frame& out )
{
	std::vector< int > stamps( static_cast< size_t >( rig.width ) );
	const int row = rig.height / 2;
	for( int x = 0; x < rig.width; ++x )
		stamps[ x ] = stampOf( rig.at( out, x, row ) );
	return stamps;
}

struct RingRun
{
	std::vector< int > stamps;
	int ringLength = 0;
	int columns    = 0;
};

bool ringRun( int width, int height, float sweepLength, int fill, bool reversed, int frames,
              RingRun& result, double clockOrigin = 0.0 )
{
	Rig rig;
	rig.clockOrigin = clockOrigin;
	if( !rig.begin( width, height ) )
		return false;

	rig.set( "Sweep Length", sweepLength );
	rig.set( "Fill", static_cast< float >( fill ) );
	rig.set( "Direction", reversed ? 1.0f : 0.0f );
	//Nearest, so a column IS a frame rather than a blend of two -- which is
	//what makes this a bit-for-bit claim about ordering rather than a
	//tolerance about colour.
	rig.set( "Interpolate", static_cast< float >( kInterpolateNearest ) );
	rig.set( "Background", static_cast< float >( kBackgroundBlack ) );
	rig.set( "Mix", 1.0f );

	//Exactly one column per frame. The slider's geometric map cannot land on a
	//frame period exactly, and this check is entirely about which frame went
	//into which column.
	rig.plugin.SetColumnPeriodForTest( 1.0 / rig.fps );

	for( int k = 0; k < frames; ++k )
		if( !rig.frame( k, stampedFrame( width, height, k ) ) )
			return false;

	result.ringLength = rig.plugin.RingLengthForTest();
	result.columns    = frames - 1;//frame 0 has no previous frame, so no delta
	result.stamps     = stampsAlongScan( rig, rig.read() );
	return true;
}

/// What the strip must hold, worked out from the ring's rules and nothing
/// else. `columns` is how many have been written; `L` the ring's length.
std::vector< int > expectedStamps( int width, int L, int columns, int fill, bool reversed )
{
	std::vector< int > want( static_cast< size_t >( width ), -1 );

	for( int x = 0; x < width; ++x )
	{
		//Display slot: the same block mapping the strip pass does, and the
		//reason a magnifying Sweep Length is checked at all.
		double scan = ( static_cast< double >( x ) + 0.5 ) / static_cast< double >( width );
		if( reversed )
			scan = 1.0 - scan;

		const int slot = std::clamp( static_cast< int >( std::floor( scan * L ) ), 0, L - 1 );

		int column = -1;
		if( fill == kFillScroll )
		{
			//The newest column is at the far end; ages run back from it.
			const int age = L - 1 - slot;
			if( age < columns )
				column = columns - 1 - age;
		}
		else
		{
			//Written in place. Slot `slot` last took a column at
			//slot, slot + L, slot + 2L ... -- the most recent of those.
			if( columns > slot )
			{
				const int laps = ( columns - 1 - slot ) / L;
				column         = slot + laps * L;
			}
		}

		//Column j was taken on frame j + 1.
		want[ x ] = column >= 0 ? column + 1 : -1;
	}

	return want;
}

int g_ringWrong = 0;
int g_ringCases = 0;

bool ringCase( int width, int height, float sweepLength, int fill, bool reversed, int frames,
               const char* label )
{
	RingRun run;
	if( !ringRun( width, height, sweepLength, fill, reversed, frames, run ) )
		return false;

	const std::vector< int > want =
		expectedStamps( width, run.ringLength, run.columns, fill, reversed );

	int wrong      = 0;
	int firstWrong = -1;
	for( int x = 0; x < width; ++x )
	{
		//An unwritten slot must be the background, which is opaque black, so
		//its stamp reads as zero -- and frame 0 never produces a column, so
		//zero is not a legitimate stamp.
		const int expect = want[ x ] < 0 ? 0 : want[ x ];
		if( run.stamps[ x ] != expect )
		{
			++wrong;
			if( firstWrong < 0 )
				firstWrong = x;
		}
	}

	std::printf( "   %s L=%-5d columns=%-5d wrong=%d", label, run.ringLength, run.columns, wrong );
	if( wrong > 0 )
		std::printf( "  first at x=%d: got frame %d, want %d", firstWrong, run.stamps[ firstWrong ],
		             want[ firstWrong ] < 0 ? 0 : want[ firstWrong ] );
	std::printf( "\n" );

	check( wrong == 0, std::string( label ) + " holds the right frames in the right order" );
	g_ringWrong = std::max( g_ringWrong, wrong );
	++g_ringCases;
	return true;
}

int runRing()
{
	std::printf( "== ring: the last min(N, L) columns, in order\n" );

	struct Case
	{
		int width, height;
		float sweep;
		int fill;
		bool reversed;
		int frames;
		const char* label;
	};

	const Case cases[] = {
		//Small raster, full ring, more frames than columns: the wrapped case.
		{ 320, 180, 1.0f, kFillScroll, false, 500, "320x180  scroll, wrapped  " },
		//Fewer frames than columns: the partly-filled case, where the
		//background has to be told apart from the picture.
		{ 320, 180, 1.0f, kFillScroll, false, 150, "320x180  scroll, half full" },
		{ 320, 180, 1.0f, kFillBuild, false, 500, "320x180  build,  wrapped  " },
		{ 320, 180, 1.0f, kFillBuild, false, 150, "320x180  build,  half full" },
		{ 320, 180, 1.0f, kFillScroll, true, 500, "320x180  scroll, reversed " },
		//A DIFFERENT RASTER, and a magnifying Sweep Length so that one ring
		//column covers several output pixels. That block mapping is the
		//raster-sensitive part of the whole plugin.
		{ 640, 360, 0.72f, kFillScroll, false, 400, "640x360  magnified        " },
		{ 640, 360, 0.72f, kFillBuild, true, 400, "640x360  magnified+reversed" },
		//A big raster at 1:1, which is a long ring and the slowest case here.
		{ 1024, 576, 1.0f, kFillScroll, false, 1200, "1024x576 scroll, wrapped  " },
	};

	for( const Case& c : cases )
		if( !ringCase( c.width, c.height, c.sweep, c.fill, c.reversed, c.frames, c.label ) )
			return 1;

	//Once: writing stops when the ring is full, so the strip is the FIRST L
	//columns however long the take runs.
	{
		RingRun shortRun, longRun;
		if( !ringRun( 320, 180, 1.0f, kFillOnce, false, 340, shortRun )
		    || !ringRun( 320, 180, 1.0f, kFillOnce, false, 900, longRun ) )
			return 1;

		check( shortRun.stamps == longRun.stamps,
		       "Once stops writing when the strip is full and holds it" );

		bool ascending = true;
		for( int x = 1; x < 320; ++x )
			ascending = ascending && longRun.stamps[ x ] == longRun.stamps[ x - 1 ] + 1;
		check( ascending, "Once holds frames 1..L in order, left to right" );
	}

	headline( "ring", "columns filed under the wrong frame",
	          std::to_string( g_ringWrong ) + " of the strip, over "
	              + std::to_string( g_ringCases ) + " configurations at 3 rasters" );
	return 0;
}

//---------------------------------------------------------------------------
// --clock. The same take, rendered from t = 0 and from 499,217,238 ms.
//
// This is the regression guard for the trap this plugin is most exposed to. It
// costs two runs and it is the only thing standing between a future narrowing
// of any time value to a float and a plugin that works perfectly for the first
// few minutes of every session.
//---------------------------------------------------------------------------
int runClock()
{
	std::printf( "== clock: the same strip from t=0 and from t=%.0f ms\n", kFarClockMs );

	RingRun fromZero, fromFar;
	if( !ringRun( 320, 180, 1.0f, kFillScroll, false, 500, fromZero, 0.0 ) )
		return 1;
	if( !ringRun( 320, 180, 1.0f, kFillScroll, false, 500, fromFar, kFarClockMs / 1000.0 ) )
		return 1;

	int wrong = 0;
	for( size_t i = 0; i < fromZero.stamps.size(); ++i )
		if( fromZero.stamps[ i ] != fromFar.stamps[ i ] )
			++wrong;

	std::printf( "   %zu columns compared, %d differ\n", fromZero.stamps.size(), wrong );
	check( wrong == 0, "a clock 5.8 days in gives the identical strip" );

	//And the same again at the fastest column rate the plugin offers, where
	//the phase advances 33 columns a frame and any precision loss in the delta
	//is multiplied by 33.
	{
		Rig a, b;
		if( !a.begin( 320, 180 ) )
			return 1;
		b.clockOrigin = kFarClockMs / 1000.0;
		if( !b.begin( 320, 180 ) )
			return 1;

		for( Rig* rig : { &a, &b } )
		{
			rig->set( "Interpolate", static_cast< float >( kInterpolateNearest ) );
			rig->plugin.SetColumnPeriodForTest( controls::TimePerColumnSeconds( 0.0f ) );
		}

		for( int k = 0; k < 40; ++k )
		{
			const Frame source = demoCard( 320, 180, k );
			if( !a.frame( k, source ) || !b.frame( k, source ) )
				return 1;
		}

		const Frame pa = a.read();
		const Frame pb = b.read();
		check( pa == pb, "the same holds at the fastest column rate, 33 columns a frame" );

		headline( "clock", "columns differing at t=499,217,238 ms",
		          std::to_string( wrong ) + " of " + std::to_string( fromZero.stamps.size() )
		              + ", and the fastest rate is bit-identical too" );
	}

	return 0;
}

//---------------------------------------------------------------------------
// --interp. A column taken between two frames is their blend.
//
// The two frames are FLAT FIELDS at 40 and 200, chosen so that every quarter
// blend of them -- 80, 120, 160 -- is an exact 8-bit code. So the expectation
// is an integer and the tolerance is one code value, which is the storage
// lattice and not a fudge.
//---------------------------------------------------------------------------
int runInterp()
{
	std::printf( "== interp: a column between two frames is their blend\n" );

	constexpr int kLow    = 40;
	constexpr int kHigh   = 200;
	constexpr int kPerFrame = 4;

	struct Case
	{
		int width, height;
		const char* label;
	};
	const Case rasters[] = {
		{ 320, 180, "320x180 " },
		{ 1280, 720, "1280x720" },
	};

	int worstOverall = 0;

	for( const Case& raster : rasters )
	{
		for( int linear = 1; linear >= 0; --linear )
		{
			Rig rig;
			if( !rig.begin( raster.width, raster.height ) )
				return 1;

			rig.set( "Interpolate", static_cast< float >( linear ? kInterpolateLinear
			                                                     : kInterpolateNearest ) );
			rig.set( "Fill", static_cast< float >( kFillBuild ) );
			rig.set( "Mix", 1.0f );
			rig.plugin.SetColumnPeriodForTest( 1.0 / ( rig.fps * kPerFrame ) );

			const int frames = 12;
			for( int k = 0; k < frames; ++k )
				if( !rig.frame( k, flatFrame( raster.width, raster.height,
				                              ( k % 2 ) ? kHigh : kLow,
				                              ( k % 2 ) ? kHigh : kLow,
				                              ( k % 2 ) ? kHigh : kLow ) ) )
					return 1;

			const Frame out       = rig.read();
			const int columns     = ( frames - 1 ) * kPerFrame;
			const int L           = rig.plugin.RingLengthForTest();
			const int row         = raster.height / 2;

			int worst   = 0;
			int worstAt = -1;

			for( int j = 0; j < columns && j < L; ++j )
			{
				//Column j was taken on frame f, at blend (j % 4 + 1) / 4.
				const int f        = j / kPerFrame + 1;
				const double blend = static_cast< double >( j % kPerFrame + 1 ) / kPerFrame;
				const double was   = ( ( f - 1 ) % 2 ) ? kHigh : kLow;
				const double now   = ( f % 2 ) ? kHigh : kLow;

				const double want = linear ? was + blend * ( now - was )
				                           : ( blend < 0.5 ? was : now );

				//Build writes in place, so display pixel j IS ring column j at
				//a 1:1 Sweep Length.
				const int got = rig.at( out, j, row )[ 1 ];
				const int d   = std::abs( got - static_cast< int >( std::lround( want ) ) );
				if( d > worst )
				{
					worst   = d;
					worstAt = j;
				}
			}

			std::printf( "   %s %-8s worst %d code value(s)%s\n", raster.label,
			             linear ? "linear" : "nearest", worst,
			             worstAt >= 0 && worst > 0 ? " (see column above)" : "" );
			check( worst <= kCodeTolerance,
			       std::string( raster.label ) + ( linear ? " linear" : " nearest" )
			           + " matches the closed form to one code value" );
			worstOverall = std::max( worstOverall, worst );
		}
	}

	headline( "interp", "worst error against the closed form",
	          std::to_string( worstOverall ) + " code values (tolerance "
	              + std::to_string( kCodeTolerance ) + ", from the 8-bit ring)" );
	return 0;
}

//---------------------------------------------------------------------------
// The width family. A bar of width b moving at v source pixels per frame,
// photographed at c columns per frame, occupies b * c / v columns.
//---------------------------------------------------------------------------
struct WidthCase
{
	int width, height;
	double columnsPerFrame;///< c
	double speed;          ///< v, source pixels per frame, signed
	double barWidth;       ///< b, source pixels
	const char* label;
};

struct WidthResult
{
	double measured  = 0.0;///< integrated coverage, in columns
	double predicted = 0.0;
	double budget    = 0.0;///< the analytic quantisation bound, in columns
	int partial      = 0;
	int ringLength   = 0;
	Moments moments;
};

/// How long a take has to be for the whole crossing to be in the strip, and
/// where the object has to start.
///
/// Two frames of margin at each end and not more. The strip must hold the
/// WHOLE event and must not wrap, because a wrapped ring has overwritten the
/// beginning of the very thing being integrated -- and the integral would then
/// be of two halves of two different crossings, which is a number with no
/// meaning that is nonetheless close enough to look like a pass.
struct Take
{
	double start = 0.0;///< the object's reference coordinate on frame 0
	int frames   = 0;
};

Take planTake( double slitX, double speed, double footprint, double offsetToReference )
{
	const double lead = footprint * 0.5 + 2.0 * std::fabs( speed );

	Take take;
	take.start  = slitX - ( speed > 0.0 ? lead : -lead ) + offsetToReference;
	take.frames = static_cast< int >( std::ceil( ( footprint + 4.0 * std::fabs( speed ) )
	                                             / std::fabs( speed ) ) )
	              + 1;
	return take;
}

/// Run a bar across the slit and integrate what came out.
///
/// The ramp is EIGHT TIMES the speed, so it spans eight frames and is an exact
/// multiple of every column spacing these cases use. That is what makes the
/// integral exact under translation rather than wobbling by half a column with
/// the bar's starting phase -- see barProfile.
bool measureWidth( const WidthCase& c, WidthResult& result, bool reversedDirection = false )
{
	Rig rig;
	if( !rig.begin( c.width, c.height ) )
		return false;

	const double ramp     = 8.0 * std::fabs( c.speed );
	const double footprint = c.barWidth + ramp;

	//The slit at an exact texel centre, so a single-tap fetch lands on a texel
	//and the source's own filtering never enters the measurement.
	const int slitTexel = c.width / 2;
	rig.set( "Slit Position",
	         static_cast< float >( ( static_cast< double >( slitTexel ) + 0.5 )
	                               / static_cast< double >( c.width ) ) );
	rig.set( "Slit Width", 0.0f );//one source pixel: a fetch, not an average
	rig.set( "Slit Angle", 0.5f );//no lean
	rig.set( "Sweep Length", 1.0f );
	rig.set( "Fill", static_cast< float >( kFillBuild ) );
	rig.set( "Direction", reversedDirection ? 1.0f : 0.0f );
	rig.set( "Interpolate", static_cast< float >( kInterpolateNearest ) );
	rig.set( "Background", static_cast< float >( kBackgroundBlack ) );
	rig.set( "Mix", 1.0f );

	rig.plugin.SetColumnPeriodForTest( 1.0 / ( rig.fps * c.columnsPerFrame ) );

	const Take take = planTake( static_cast< double >( slitTexel ) + 0.5, c.speed, footprint, 0.0 );

	for( int k = 0; k < take.frames; ++k )
	{
		const double centre = take.start + c.speed * static_cast< double >( k );
		if( !rig.frame( k, barFrame( c.width, c.height, centre, c.barWidth, ramp ) ) )
			return false;
	}

	result.ringLength = rig.plugin.RingLengthForTest();
	if( rig.plugin.FilledForTest() >= result.ringLength )
	{
		bad( std::string( c.label ) + ": the ring wrapped, so the event is no longer whole" );
		result.ringLength = 0;
		return true;
	}

	const Frame out = rig.read();
	const int row   = c.height / 2;

	result.measured  = integrateRow( out, c.width, row, result.partial );
	result.predicted = c.barWidth * c.columnsPerFrame / std::fabs( c.speed );
	result.moments   = rowMoments( out, c.width, row );

	//The analytic error budget, in columns. Only the columns that are neither
	//black nor saturated can carry an error, and each of them can be out by at
	//most half a code value from the card's own 8-bit quantisation. The ring
	//and the read-back are exact: a code that went in comes back out.
	result.budget = static_cast< double >( result.partial ) * 0.5 / 255.0;
	return true;
}

int runWidth()
{
	std::printf( "== width: b * c / v columns, in closed form\n" );

	//Two rasters, four column rates, and one case with a non-integer speed so
	//that nothing here can be passing because the bar happened to land on
	//whole pixels.
	const WidthCase cases[] = {
		{ 320, 180, 0.5, 2.0, 80.0, "320x180  c=0.5 v=2   " },
		{ 320, 180, 1.0, 2.0, 80.0, "320x180  c=1   v=2   " },
		{ 320, 180, 2.0, 2.0, 80.0, "320x180  c=2   v=2   " },
		{ 320, 180, 4.0, 2.0, 80.0, "320x180  c=4   v=2   " },
		{ 320, 180, 4.0, 2.5, 80.0, "320x180  c=4   v=2.5 " },
		{ 1280, 720, 1.0, 8.0, 320.0, "1280x720 c=1   v=8   " },
		{ 1280, 720, 4.0, 8.0, 320.0, "1280x720 c=4   v=8   " },
		{ 1280, 720, 4.0, 5.0, 320.0, "1280x720 c=4   v=5   " },
	};

	double worstError  = 0.0;
	double worstBudget = 0.0;

	for( const WidthCase& c : cases )
	{
		WidthResult r;
		if( !measureWidth( c, r ) )
			return 1;
		if( r.ringLength == 0 )
			continue;

		const double error = std::fabs( r.measured - r.predicted );
		std::printf( "   %s measured %8.3f  want %8.3f  out by %6.3f col   budget %.4f col\n",
		             c.label, r.measured, r.predicted, error, r.budget );

		check( error <= kColumnTolerance,
		       std::string( c.label ) + " is within one column of b*c/v" );
		check( r.budget < kColumnTolerance * 0.25,
		       std::string( c.label ) + " quantisation budget is well inside the tolerance" );

		worstError  = std::max( worstError, error );
		worstBudget = std::max( worstBudget, r.budget );
	}

	headline( "width", "worst error against b*c/v",
	          figure( "%.3f", worstError ) + " col (tolerance "
	              + figure( "%.2f", kColumnTolerance ) + ", quantisation bound "
	              + figure( "%.3f", worstBudget ) + ")" );
	return 0;
}

//---------------------------------------------------------------------------
// --matched. At the film speed, true proportions.
//
// The special case v = c of the relation above: an object crossing at exactly
// the column rate renders b * c / c = b columns, and at a 1:1 Sweep Length a
// column is an output pixel -- so a 64-pixel object comes out 64 pixels wide,
// at any raster, which is what "true proportions" means.
//---------------------------------------------------------------------------
double g_matchedWorst  = 0.0;
double g_matchedBudget = 0.0;

int runMatched()
{
	std::printf( "== matched: at the film speed, the object's own proportions\n" );

	const WidthCase cases[] = {
		{ 320, 180, 2.0, 2.0, 64.0, "320x180  c=v=2" },
		{ 320, 180, 4.0, 4.0, 64.0, "320x180  c=v=4" },
		{ 320, 180, 8.0, 8.0, 64.0, "320x180  c=v=8" },
		{ 1280, 720, 4.0, 4.0, 64.0, "1280x720 c=v=4" },
		{ 1280, 720, 8.0, 8.0, 64.0, "1280x720 c=v=8" },
	};

	for( const WidthCase& c : cases )
	{
		WidthResult r;
		if( !measureWidth( c, r ) )
			return 1;
		if( r.ringLength == 0 )
			continue;

		const double error  = std::fabs( r.measured - c.barWidth );
		const double aspect = r.measured / c.barWidth;

		std::printf( "   %s %6.3f px wide, source is %.0f px  (aspect %.4f)  out by %5.3f col"
		             "   budget %.4f col\n",
		             c.label, r.measured, c.barWidth, aspect, error, r.budget );

		check( error <= kColumnTolerance,
		       std::string( c.label ) + " renders at its own width to within one column" );
		check( r.budget < kColumnTolerance * 0.25,
		       std::string( c.label ) + " quantisation budget is well inside the tolerance" );

		g_matchedWorst  = std::max( g_matchedWorst, error );
		g_matchedBudget = std::max( g_matchedBudget, r.budget );
	}

	//And the point of the whole thing: an object NOT at the film speed does
	//not come out at its own width. Without this, "matched" would be a claim
	//about nothing.
	{
		WidthCase fast = { 320, 180, 4.0, 8.0, 64.0, "320x180  c=4 v=8" };
		WidthResult r;
		if( !measureWidth( fast, r ) )
			return 1;
		const double aspect = r.measured / fast.barWidth;
		std::printf( "   %s at twice the film speed: %6.3f px wide, aspect %.4f\n", fast.label,
		             r.measured, aspect );
		check( std::fabs( aspect - 0.5 ) < 0.02,
		       "twice the film speed squashes the object to half its width" );
	}

	headline( "matched", "worst error against the object's width",
	          figure( "%.3f", g_matchedWorst ) + " col (tolerance "
	              + figure( "%.2f", kColumnTolerance ) + ", quantisation bound "
	              + figure( "%.3f", g_matchedBudget ) + ")" );
	return 0;
}

//---------------------------------------------------------------------------
// --reverse. The other way round comes out mirrored.
//
// Measured as SKEWNESS, which is dimensionless and invariant under the
// stretch, the translation and the scaling the strip applies -- so the
// expected number is a property of the object alone and the check says nothing
// about this machine's raster. The expected value is obtained by integrating
// the very profile the card is drawn from; the tolerance's derived part is the
// worst-case error from sampling that profile on the strip's own column
// lattice, over every phase.
//---------------------------------------------------------------------------

/// Skewness of the analytic wedge, sampled at `spacing` with an offset of
/// `phase * spacing`. A spacing of zero integrates it finely instead.
double wedgeSkew( double left, double b, double ramp, double spacing, double phase )
{
	const double step  = spacing > 0.0 ? spacing : ( b + ramp ) / 20000.0;
	const double first = left - step + phase * step;
	const double last  = left + b + ramp + step;

	std::vector< double > weight, position;
	for( double x = first; x <= last; x += step )
	{
		weight.push_back( wedgeProfile( x, left, b, ramp ) );
		position.push_back( x );
	}
	return momentsOf( weight, position ).skew;
}

int runReverse()
{
	std::printf( "== reverse: the other way round comes out mirrored\n" );

	struct Case
	{
		int width, height;
		double columnsPerFrame, speed, barWidth;
		const char* label;
	};
	const Case cases[] = {
		{ 320, 180, 4.0, 2.0, 80.0, "320x180 " },
		{ 1280, 720, 4.0, 8.0, 320.0, "1280x720" },
	};

	double worstError = 0.0;
	double worstBound = 0.0;

	for( const Case& c : cases )
	{
		const double ramp = 8.0 * c.speed;

		//What the object IS, integrated finely. Not a closed form somebody
		//typed in: the same function the card is drawn from.
		const double source = wedgeSkew( 0.0, c.barWidth, ramp, 0.0, 0.0 );

		//The derived part of the tolerance: the worst that sampling that shape
		//on the strip's own lattice can move its skewness, over every phase.
		const double spacing = c.speed / c.columnsPerFrame;
		double derived       = 0.0;
		for( int p = 0; p < 64; ++p )
			derived = std::max( derived,
			                    std::fabs( wedgeSkew( 0.0, c.barWidth, ramp, spacing,
			                                          static_cast< double >( p ) / 64.0 )
			                               - source ) );

		struct Run
		{
			double speed;
			bool reversedDirection;
			const char* what;
			double expect;
		};

		//Moving RIGHT, the object's right-hand edge reaches the slit first, so
		//the strip holds it mirrored -- and its skewness is negated. Moving
		//left, it is not. Reversing Direction flips the display axis and so
		//flips the sign again.
		const Run runs[] = {
			{ c.speed, false, "rightward, forward", -source },
			{ -c.speed, false, "leftward,  forward", source },
			{ c.speed, true, "rightward, reversed", source },
		};

		double measured[ 3 ] = { 0.0, 0.0, 0.0 };
		double widths[ 3 ]   = { 0.0, 0.0, 0.0 };

		for( int i = 0; i < 3; ++i )
		{
			Rig rig;
			if( !rig.begin( c.width, c.height ) )
				return 1;

			const int slitTexel = c.width / 2;
			rig.set( "Slit Position",
			         static_cast< float >( ( static_cast< double >( slitTexel ) + 0.5 )
			                               / static_cast< double >( c.width ) ) );
			rig.set( "Slit Width", 0.0f );
			rig.set( "Slit Angle", 0.5f );
			rig.set( "Sweep Length", 1.0f );
			rig.set( "Fill", static_cast< float >( kFillBuild ) );
			rig.set( "Direction", runs[ i ].reversedDirection ? 1.0f : 0.0f );
			rig.set( "Interpolate", static_cast< float >( kInterpolateNearest ) );
			rig.set( "Background", static_cast< float >( kBackgroundBlack ) );
			rig.set( "Mix", 1.0f );
			rig.plugin.SetColumnPeriodForTest( 1.0 / ( rig.fps * c.columnsPerFrame ) );

			const double v         = runs[ i ].speed;
			const double footprint = c.barWidth + ramp;

			//The wedge is placed by its LEFT edge rather than its centre, so
			//the plan is shifted by half a footprint.
			const Take take = planTake( static_cast< double >( slitTexel ) + 0.5, v, footprint,
			                            -footprint * 0.5 );

			for( int k = 0; k < take.frames; ++k )
			{
				const double left = take.start + v * static_cast< double >( k );
				if( !rig.frame( k, wedgeFrame( c.width, c.height, left, c.barWidth, ramp ) ) )
					return 1;
			}

			if( rig.plugin.FilledForTest() >= rig.plugin.RingLengthForTest() )
			{
				bad( std::string( c.label ) + ": the ring wrapped during the reverse take" );
				return 1;
			}

			const Frame out    = rig.read();
			const Moments m    = rowMoments( out, c.width, c.height / 2 );
			measured[ i ]      = m.skew;
			int partial        = 0;
			widths[ i ]        = integrateRow( out, c.width, c.height / 2, partial );
		}

		std::printf( "   %s object skew %+.4f, lattice bound %.4f (tolerance %.4f)\n", c.label,
		             source, derived, kSkewTolerance );
		for( int i = 0; i < 3; ++i )
			std::printf( "   %s   %s  skew %+.4f, want %+.4f, out by %.4f   width %.2f col\n",
			             c.label, runs[ i ].what, measured[ i ], runs[ i ].expect,
			             std::fabs( measured[ i ] - runs[ i ].expect ), widths[ i ] );

		check( derived < kSkewTolerance,
		       std::string( c.label ) + " the lattice bound sits inside the tolerance" );

		for( int i = 0; i < 3; ++i )
			check( std::fabs( measured[ i ] - runs[ i ].expect ) <= kSkewTolerance,
			       std::string( c.label ) + " " + runs[ i ].what + " matches the object's own shape" );

		//The claim in plain form: the two directions have opposite handedness,
		//and by a margin -- a check that could pass on noise would not be one.
		check( measured[ 0 ] * measured[ 1 ] < 0.0
		           && std::min( std::fabs( measured[ 0 ] ), std::fabs( measured[ 1 ] ) ) > 0.3,
		       std::string( c.label ) + " the two directions are mirror images, by a margin" );

		//And the widths agree, because reversing changes which way round it is
		//and nothing else.
		check( std::fabs( widths[ 0 ] - widths[ 1 ] ) <= kColumnTolerance,
		       std::string( c.label ) + " reversing does not change the rendered width" );

		for( int i = 0; i < 3; ++i )
			worstError = std::max( worstError, std::fabs( measured[ i ] - runs[ i ].expect ) );
		worstBound = std::max( worstBound, derived );
	}

	headline( "reverse", "worst skew error against the object",
	          figure( "%.4f", worstError ) + " (tolerance " + figure( "%.2f", kSkewTolerance )
	              + ", lattice bound " + figure( "%.4f", worstBound ) + ")" );
	return 0;
}

//---------------------------------------------------------------------------
// --negative. The checks above can actually fail.
//
// A check that cannot fail is not a check. Each of these perturbs the model or
// the input by an amount a real defect would produce, and asserts that the
// assertion it belongs to rejects it.
//---------------------------------------------------------------------------
int runNegative()
{
	std::printf( "== negative: the checks can fail\n" );

	const int failuresBefore = g_failures;

	//1. A column rate 15% out. The width check must reject it.
	{
		const WidthCase c = { 320, 180, 4.0, 2.0, 80.0, "width vs a 15% wrong rate" };
		WidthResult r;
		if( !measureWidth( c, r ) )
			return 1;

		const double wrong = c.barWidth * ( c.columnsPerFrame * 1.15 ) / c.speed;
		const double error = std::fabs( r.measured - wrong );
		std::printf( "   a rate 15%% out predicts %.3f columns; measured %.3f, out by %.3f\n",
		             wrong, r.measured, error );
		check( error > kColumnTolerance, "the width check rejects a 15% error in the column rate" );

		//And a 1% error, which is the interesting boundary: a tolerance of one
		//column on a 160-column measurement is about 0.6%, so 1% must also be
		//rejected. If it is not, the tolerance is too loose for the size of
		//the thing being measured.
		const double slightly = c.barWidth * ( c.columnsPerFrame * 1.01 ) / c.speed;
		std::printf( "   a rate 1%% out predicts %.3f columns; measured %.3f, out by %.3f\n",
		             slightly, r.measured, std::fabs( r.measured - slightly ) );
		check( std::fabs( r.measured - slightly ) > kColumnTolerance,
		       "the width check rejects a 1% error in the column rate" );
	}

	//2. An off-by-one in the ring.
	{
		RingRun run;
		if( !ringRun( 320, 180, 1.0f, kFillScroll, false, 500, run ) )
			return 1;

		std::vector< int > want = expectedStamps( 320, run.ringLength, run.columns, kFillScroll, false );
		//Shift the expectation by one column, which is what an off-by-one in
		//the read index looks like from outside.
		int wrong = 0;
		for( int x = 0; x + 1 < 320; ++x )
			if( run.stamps[ x ] != ( want[ x + 1 ] < 0 ? 0 : want[ x + 1 ] ) )
				++wrong;

		std::printf( "   a one-column shift in the ring mismatches %d of 319 columns\n", wrong );
		check( wrong > 300, "the ring check rejects an off-by-one in the read index" );
	}

	//3. No interpolation, judged against the interpolated expectation.
	{
		Rig rig;
		if( !rig.begin( 320, 180 ) )
			return 1;

		rig.set( "Interpolate", static_cast< float >( kInterpolateNearest ) );
		rig.set( "Fill", static_cast< float >( kFillBuild ) );
		rig.set( "Mix", 1.0f );
		rig.plugin.SetColumnPeriodForTest( 1.0 / ( rig.fps * 4.0 ) );

		for( int k = 0; k < 12; ++k )
			if( !rig.frame( k, flatFrame( 320, 180, ( k % 2 ) ? 200 : 40, ( k % 2 ) ? 200 : 40,
			                              ( k % 2 ) ? 200 : 40 ) ) )
				return 1;

		const Frame out = rig.read();
		int worst       = 0;
		for( int j = 0; j < 44; ++j )
		{
			const int f        = j / 4 + 1;
			const double blend = static_cast< double >( j % 4 + 1 ) / 4.0;
			const double was   = ( ( f - 1 ) % 2 ) ? 200.0 : 40.0;
			const double now   = ( f % 2 ) ? 200.0 : 40.0;
			const double want  = was + blend * ( now - was );
			worst = std::max( worst,
			                  std::abs( rig.at( out, j, 90 )[ 1 ]
			                            - static_cast< int >( std::lround( want ) ) ) );
		}
		std::printf( "   nearest against the linear expectation: worst %d code value(s)\n", worst );
		check( worst > kCodeTolerance,
		       "the interpolation check rejects a plugin that does not interpolate" );
	}

	//4. A moving picture, judged as a static one.
	{
		Rig rig;
		if( !rig.begin( 320, 180 ) )
			return 1;
		rig.set( "Fill", static_cast< float >( kFillBuild ) );
		rig.set( "Mix", 1.0f );
		rig.plugin.SetColumnPeriodForTest( 1.0 / 240.0 );

		for( int k = 0; k < 96; ++k )
			if( !rig.frame( k, demoCard( 320, 180, k ) ) )
				return 1;

		const Frame out = rig.read();
		int worst       = 0;
		for( int y = 0; y < 180; ++y )
		{
			const unsigned char* first = rig.at( out, 0, y );
			for( int x = 1; x < 320; ++x )
				for( int c = 0; c < 4; ++c )
					worst = std::max( worst, std::abs( static_cast< int >( rig.at( out, x, y )[ c ] )
					                                   - static_cast< int >( first[ c ] ) ) );
		}
		std::printf( "   a moving picture judged as a still one: worst %d code value(s)\n", worst );
		check( worst > 0, "the streak check rejects a picture that was moving" );
	}

	//5. The same direction twice, judged as two directions.
	{
		const double barWidth = 80.0, speed = 2.0, ramp = 16.0;
		double skews[ 2 ] = { 0.0, 0.0 };

		for( int i = 0; i < 2; ++i )
		{
			Rig rig;
			if( !rig.begin( 320, 180 ) )
				return 1;
			rig.set( "Slit Position", ( 160.0f + 0.5f ) / 320.0f );
			rig.set( "Slit Width", 0.0f );
			rig.set( "Sweep Length", 1.0f );
			rig.set( "Fill", static_cast< float >( kFillBuild ) );
			rig.set( "Interpolate", static_cast< float >( kInterpolateNearest ) );
			rig.set( "Mix", 1.0f );
			rig.plugin.SetColumnPeriodForTest( 1.0 / ( rig.fps * 4.0 ) );

			//Both runs go the SAME way; only the starting offset differs.
			const double footprint = barWidth + ramp;
			const Take take = planTake( 160.5, speed, footprint, -footprint * 0.5 - 3.0 * i );
			for( int k = 0; k < take.frames; ++k )
				if( !rig.frame( k, wedgeFrame( 320, 180, take.start + speed * k, barWidth, ramp ) ) )
					return 1;

			skews[ i ] = rowMoments( rig.read(), 320, 90 ).skew;
		}

		std::printf( "   two runs the same way: skew %+.4f and %+.4f\n", skews[ 0 ], skews[ 1 ] );
		check( !( skews[ 0 ] * skews[ 1 ] < 0.0 ),
		       "the reverse check rejects two takes that went the same way" );
	}

	//6. A wrong column rate in the schedule itself, with no GL involved.
	{
		const double period = 1.0 / 60.0;
		long long right = 0, wrong = 0;
		double pRight = 0.0, pWrong = 0.0, lastNow = -1.0;

		for( int k = 0; k < 600; ++k )
		{
			const double now = static_cast< double >( k ) / 60.0;
			const double dt  = lastNow >= 0.0 ? now - lastNow : 0.0;
			lastNow          = now;

			strip::Schedule a = strip::advance( pRight, dt, period );
			pRight            = a.phase;
			right += a.columns;

			strip::Schedule b = strip::advance( pWrong, dt, period / 1.15 );
			pWrong            = b.phase;
			wrong += b.columns;
		}

		std::printf( "   600 frames: the right rate gives %lld columns, a 15%% wrong one %lld\n",
		             right, wrong );
		check( std::llabs( right - wrong ) > 1,
		       "the schedule check rejects a 15% error in the column period" );
	}

	headline( "negative", "perturbations correctly rejected",
	          std::to_string( 7 - ( g_failures - failuresBefore ) ) + " of 7" );
	return 0;
}

//---------------------------------------------------------------------------
// --bench
//---------------------------------------------------------------------------
double benchAt( int width, int height, int frames )
{
	Rig rig;
	if( !rig.begin( width, height ) )
		return 0.0;

	const Frame source = demoCard( width, height, 0 );

	//A warm-up that is thrown away: the first frames pay for allocating the
	//ring and both frame copies.
	const int warmup = 20;
	for( int k = 0; k < warmup; ++k )
		rig.frame( k, source );
	glFinish();

	const auto start = std::chrono::steady_clock::now();
	for( int k = 0; k < frames; ++k )
		rig.frame( warmup + k, source );
	//glFinish on both sides, because GL calls queue and an unsynchronised
	//version times how fast a `for` loop hands work to the driver.
	glFinish();
	const auto end = std::chrono::steady_clock::now();

	return std::chrono::duration< double >( end - start ).count() * 1000.0
	       / static_cast< double >( frames );
}

int runBench( int frames )
{
	struct Size
	{
		const char* name;
		int width, height;
	};
	const Size sizes[] = {
		{ "1280x720 ", 1280, 720 },
		{ "1920x1080", 1920, 1080 },
		{ "3840x2160", 3840, 2160 },
	};

	std::printf( "%d frames each, after a 20-frame warm-up, glFinish both sides.\n\n", frames );
	std::printf( "resolution    ms/frame   equivalent fps   %% of a 60fps frame\n" );

	for( const Size& s : sizes )
	{
		const double ms = benchAt( s.width, s.height, frames );
		std::printf( "%s    %7.3f       %8.0f            %5.1f%%\n", s.name, ms,
		             ms > 0.0 ? 1000.0 / ms : 0.0, ms / 16.667 * 100.0 );
	}

	std::printf( "\nCost is one copy, one strip pass and ONE DRAW PER COLUMN. At the\n"
	             "default rate that is four one-pixel-wide draws a frame; at the fastest\n"
	             "column period it is thirty-three. Whatever the settings above were,\n"
	             "they are what was measured; run with --set to measure something else.\n" );
	return 0;
}

//---------------------------------------------------------------------------
void usage()
{
	std::printf(
		"pftest -- render and measure the Photofinish strip camera\n"
		"\n"
		"  --out PATH        render the demo card through the plugin (default /tmp/photofinish.png)\n"
		"  --card PATH       write the demo card alone, undecorated\n"
		"  --size WxH        raster (default 1280x720)\n"
		"  --frames N        frames to render before reading back (default 90)\n"
		"  --fps N           synthetic frame rate driving the clock (default 60)\n"
		"  --set \"Name=V\"    set a parameter by its display name. Repeatable.\n"
		"  --list            print every parameter, its kind and its range, then exit\n"
		"\n"
		"  --schedule        the column schedule, with no GL at all\n"
		"  --static          a still picture is exact horizontal streaks\n"
		"  --ring            the ring holds the last min(N, L) columns, in order\n"
		"  --clock           the same take from t=0 and from t=499217238 ms\n"
		"  --interp          a column between two frames is their blend\n"
		"  --width           a moving bar's rendered width, against b*c/v\n"
		"  --matched         at the film speed, the object's own proportions\n"
		"  --reverse         the other way round comes out mirrored\n"
		"  --negative        every check above can actually fail\n"
		"  --bench           time ProcessOpenGL at 720p, 1080p and 4K\n"
		"  --help\n" );
}

const char* kindOf( Photofinish& plugin, unsigned int index )
{
	if( index >= Photofinish::PT_ABOUT_FIRST )
		return "about";

	switch( plugin.GetParamType( index ) )
	{
	case FF_TYPE_OPTION:
		return "option";
	case FF_TYPE_BOOLEAN:
		return "boolean";
	case FF_TYPE_TEXT:
		return "text";
	case FF_TYPE_EVENT:
		return "event";
	default:
		return "standard";
	}
}
} // namespace

//---------------------------------------------------------------------------
int main( int argc, char** argv )
{
	std::string outPath = "/tmp/photofinish.png";
	std::string cardPath;
	int width  = 1280;
	int height = 720;
	int frames = 90;
	double fps = 60.0;

	bool wantList     = false;
	bool wantSchedule = false;
	bool wantStatic   = false;
	bool wantRing     = false;
	bool wantClock    = false;
	bool wantInterp   = false;
	bool wantWidth    = false;
	bool wantMatched  = false;
	bool wantReverse  = false;
	bool wantNegative = false;
	bool wantBench    = false;

	std::vector< std::string > settings;

	for( int i = 1; i < argc; ++i )
	{
		const std::string argument = argv[ i ];
		const bool hasNext         = i + 1 < argc;

		if( argument == "--help" )
		{
			usage();
			return 0;
		}
		else if( argument == "--out" && hasNext )
			outPath = argv[ ++i ];
		else if( argument == "--card" && hasNext )
			cardPath = argv[ ++i ];
		else if( argument == "--size" && hasNext )
		{
			const std::string size = argv[ ++i ];
			const size_t cross     = size.find( 'x' );
			if( cross == std::string::npos )
			{
				std::fprintf( stderr, "--size wants WxH, e.g. 1280x720\n" );
				return 2;
			}
			width  = std::atoi( size.substr( 0, cross ).c_str() );
			height = std::atoi( size.substr( cross + 1 ).c_str() );
		}
		else if( argument == "--frames" && hasNext )
			frames = std::atoi( argv[ ++i ] );
		else if( argument == "--fps" && hasNext )
			fps = std::strtod( argv[ ++i ], nullptr );
		else if( argument == "--set" && hasNext )
			settings.push_back( argv[ ++i ] );
		else if( argument == "--list" )
			wantList = true;
		else if( argument == "--schedule" )
			wantSchedule = true;
		else if( argument == "--static" )
			wantStatic = true;
		else if( argument == "--ring" )
			wantRing = true;
		else if( argument == "--clock" )
			wantClock = true;
		else if( argument == "--interp" )
			wantInterp = true;
		else if( argument == "--width" )
			wantWidth = true;
		else if( argument == "--matched" )
			wantMatched = true;
		else if( argument == "--reverse" )
			wantReverse = true;
		else if( argument == "--negative" )
			wantNegative = true;
		else if( argument == "--bench" )
			wantBench = true;
		else
		{
			std::fprintf( stderr, "unknown argument: %s\n", argument.c_str() );
			usage();
			return 2;
		}
	}

	if( width <= 0 || height <= 0 || frames <= 0 || fps <= 0.0 )
	{
		std::fprintf( stderr, "size, frames and fps must all be positive\n" );
		return 2;
	}

	//No GL needed, so it is answered before a context is made -- which means
	//it still works on a machine where creating one fails, and in CI.
	if( wantSchedule && !wantStatic && !wantRing && !wantClock && !wantInterp && !wantWidth
	    && !wantMatched && !wantReverse && !wantNegative && !wantBench )
	{
		runSchedule();
		printSummary();
		std::printf( "\n%s\n", g_failures == 0 ? "all checks passed" : "FAILURES above" );
		return g_failures == 0 ? 0 : 1;
	}

	if( !cardPath.empty() )
	{
		const Frame card = demoCard( width, height, 0 );
		if( !writePng( cardPath, width, height, flipRows( card, width, height ) ) )
		{
			std::fprintf( stderr, "could not write %s\n", cardPath.c_str() );
			return 1;
		}
		std::printf( "wrote %s\n", cardPath.c_str() );
		return 0;
	}

	CGLContextObj context = createContext();
	if( context == nullptr )
	{
		std::fprintf( stderr, "could not create an OpenGL context\n" );
		return 1;
	}

	auto finish = [ & ]( int code ) {
		CGLSetCurrentContext( nullptr );
		CGLDestroyContext( context );
		return code;
	};

	const bool anyCheck = wantSchedule || wantStatic || wantRing || wantClock || wantInterp
	                      || wantWidth || wantMatched || wantReverse || wantNegative;

	if( anyCheck )
	{
		if( wantSchedule )
			runSchedule();
		if( wantStatic )
			runStatic();
		if( wantRing )
			runRing();
		if( wantClock )
			runClock();
		if( wantInterp )
			runInterp();
		if( wantWidth )
			runWidth();
		if( wantMatched )
			runMatched();
		if( wantReverse )
			runReverse();
		if( wantNegative )
			runNegative();

		printSummary();
		std::printf( "\n%s\n", g_failures == 0 ? "all checks passed"
		                                       : "FAILURES above" );
		return finish( g_failures == 0 ? 0 : 1 );
	}

	if( wantBench )
		return finish( runBench( frames ) );

	//--list and --out both need a plugin instance.
	Rig rig;
	rig.fps = fps;
	if( !rig.begin( width, height ) )
		return finish( 1 );

	for( const std::string& setting : settings )
	{
		const size_t equals = setting.find( '=' );
		if( equals == std::string::npos )
		{
			std::fprintf( stderr, "--set wants Name=Value\n" );
			return finish( 2 );
		}
		if( !rig.set( setting.substr( 0, equals ),
		              std::strtof( setting.substr( equals + 1 ).c_str(), nullptr ) ) )
			return finish( 2 );
	}

	if( wantList )
	{
		std::printf( "%-3s %-18s %-9s %-8s %s\n", "id", "name", "kind", "default", "range" );
		for( const NamedParameter& p : listParameters( rig.plugin ) )
		{
			const char* kind = kindOf( rig.plugin, p.index );
			if( std::string( kind ) == "about" )
			{
				std::printf( "%-3u %-18s %-9s\n", p.index, p.name.c_str(), "about" );
				continue;
			}

			//An option's range is its element VALUES, which is what --set and
			//the sweep have to use: FFGL keeps an element's display slot and
			//its stored value apart, and only the value is ever stored.
			float high = 1.0f;
			if( rig.plugin.GetParamType( p.index ) == FF_TYPE_OPTION )
				high = static_cast< float >( rig.plugin.GetNumParamElements( p.index ) ) - 1.0f;

			std::printf( "%-3u %-18s %-9s %-8.4f [ 0 .. %g ]\n", p.index, p.name.c_str(), kind,
			             p.value, high );
		}
		return finish( 0 );
	}

	for( int k = 0; k < frames; ++k )
	{
		if( rig.frame( k, demoCard( width, height, k ) ) )
			continue;
		std::fprintf( stderr, "ProcessOpenGL failed on frame %d\n", k );
		return finish( 1 );
	}

	if( !writePng( outPath, width, height, flipRows( rig.read(), width, height ) ) )
	{
		std::fprintf( stderr, "could not write %s\n", outPath.c_str() );
		return finish( 1 );
	}

	std::printf( "wrote %s (%dx%d, %d frames)\n", outPath.c_str(), width, height, frames );
	return finish( 0 );
}
