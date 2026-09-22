#include "Strip.h"

#include <algorithm>
#include <cmath>

namespace photofinish::strip
{

Schedule advance( double phase, double dt, double tpc )
{
	Schedule out;
	//Not clamped at zero: `advance` hands back a slightly negative phase
	//whenever the snap fires, and clamping it here would reintroduce exactly
	//the drift the snap's remainder was shaped to avoid.
	out.phase = std::clamp( phase, -kColumnSnap, 1.0 );

	if( !( tpc > 0.0 ) || !( dt > 0.0 ) )
		return out;

	const double x = out.phase + dt / tpc;

	//The snap is on the FLOOR and not on x itself, so the phase carried
	//forward is the true remainder rather than the snapped one. Adding the
	//snap to the phase every frame would drift it by a whole column every
	//million frames; taking the floor of x + snap and subtracting it from x
	//cannot, because the remainder is allowed to go slightly negative.
	const double whole = std::floor( x + kColumnSnap );

	out.columns = whole > 0.0 ? static_cast< int >( whole ) : 0;
	out.phase   = std::clamp( x - whole, -kColumnSnap, 1.0 );
	return out;
}

double blendAt( int index, double phase, double dt, double tpc )
{
	if( !( dt > 0.0 ) || !( tpc > 0.0 ) )
		return 1.0;

	const double within = ( static_cast< double >( index ) + 1.0 - phase ) * tpc / dt;
	return std::clamp( within, 0.0, 1.0 );
}

} // namespace photofinish::strip
