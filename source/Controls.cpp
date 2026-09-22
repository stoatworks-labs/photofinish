#include "Controls.h"

#include <algorithm>
#include <cmath>

namespace photofinish::controls
{
namespace
{
/// lo * (hi/lo)^t, the usual geometric mapping.
double geometric( double lo, double hi, double t )
{
	return lo * std::pow( hi / lo, std::clamp( t, 0.0, 1.0 ) );
}

/// Steps on the Sweep Length ladder. See the header: this is what stops a
/// drag clearing the ring on every pixel.
constexpr int kSweepSteps = 48;
} // namespace

float SlitPosition( float value )
{
	return std::clamp( value, 0.0f, 1.0f );
}

int SlitWidthPixels( float value )
{
	//Rounded, and never below one: half a tap is not a thing the slit pass can
	//average over, and a zero-tap slit is a black picture with no message.
	const double pixels = geometric( 1.0, 64.0, static_cast< double >( value ) );
	return std::max( 1, static_cast< int >( std::lround( pixels ) ) );
}

float SlitLean( float value )
{
	return std::clamp( ( value - 0.5f ) * 2.0f, -1.0f, 1.0f );
}

double TimePerColumnSeconds( float value )
{
	return geometric( 0.0005, 0.5, static_cast< double >( value ) );
}

int SweepColumns( float value, int axisPixels )
{
	const int longest = std::max( 8, axisPixels );

	//Quantise the SLIDER, not the answer. Quantising the answer would give a
	//ladder whose rungs are one column apart at the short end and dozens apart
	//at the long one, so the same nudge would clear the ring at one end of the
	//control and not at the other.
	const double t =
		std::round( std::clamp( static_cast< double >( value ), 0.0, 1.0 ) * kSweepSteps )
		/ static_cast< double >( kSweepSteps );

	const double columns = geometric( 8.0, static_cast< double >( longest ), t );
	return std::clamp( static_cast< int >( std::lround( columns ) ), 8, longest );
}

} // namespace photofinish::controls
