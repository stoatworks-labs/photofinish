#include "Shaders.h"

namespace photofinish
{

const char* const kVertexShader = R"(#version 410 core

layout( location = 0 ) in vec4 vPosition;
layout( location = 1 ) in vec2 vUV;

out vec2 uv;

void main()
{
	gl_Position = vPosition;

	//Straight through, in 0..1 picture space. The usual FFGL vertex shader
	//folds MaxUV in here; that happens once, in the copy pass, and every pass
	//after it reads a texture we allocated, where the picture really does fill
	//the texture.
	uv = vUV;
}
)";

//---------------------------------------------------------------------------
// 1. copy -- the host's texture into one of ours.
//---------------------------------------------------------------------------
const char* const kCopyShader = R"(#version 410 core

uniform sampler2D SourceTexture;
uniform vec2 MaxUV;     //the part of the host's texture that is really picture
uniform vec2 HalfTexel; //half a source texel, in picture space

in vec2 uv;
out vec4 fragColor;

void main()
{
	//Half a texel in from the edge. GL_LINEAR at the picture boundary takes
	//half its weight from the texture's undrawn padding, and a slit parked at
	//Slit Position 0 or 1 sits exactly there -- so without this, the two ends
	//of the control fade to black for a reason that is nothing to do with the
	//picture.
	vec2 picture = clamp( uv, HalfTexel, vec2( 1.0 ) - HalfTexel );

	fragColor = texture( SourceTexture, picture * MaxUV );
}
)";

//---------------------------------------------------------------------------
// 2. slit -- one column of the ring.
//---------------------------------------------------------------------------
const char* const kSlitShader = R"(#version 410 core

uniform sampler2D FrameNow;
uniform sampler2D FrameWas;

uniform float Blend;       //0 is the previous frame's instant, 1 is this one's
uniform float TimeIsLinear;//1 interpolates between the two frames, 0 takes the nearer
uniform float SlitCentre;  //where the slit sits on the axis it cuts across, 0..1
uniform float SlitSpan;    //the slit's full width on that axis, 0..1
uniform float SlitSlope;   //cross-axis displacement per unit of slit length
uniform float AxisVertical;//1 when the slit is a horizontal line
uniform int TapCount;
uniform vec2 HalfTexel;

in vec2 uv;
out vec4 fragColor;

vec4 atInstant( vec2 p )
{
	vec2 q = clamp( p, HalfTexel, vec2( 1.0 ) - HalfTexel );

	vec4 was = texture( FrameWas, q );
	vec4 now = texture( FrameNow, q );

	if( TimeIsLinear > 0.5 )
		return mix( was, now, Blend );

	//Nearest. A column is taken at an instant the host did not deliver, so
	//either it is built from the two frames around it or it is the one it is
	//closer to. There is no third answer and the control says which.
	return Blend < 0.5 ? was : now;
}

void main()
{
	//The viewport for this pass is ONE column wide, so uv.x is 0.5 at every
	//fragment and says nothing. uv.y is the position along the slit.
	float along = uv.y;

	//The lean. A slope rather than an angle: no trigonometry, and no pole at
	//the end of the slider.
	float across = SlitCentre + SlitSlope * ( along - 0.5 );

	vec4 total = vec4( 0.0 );
	for( int i = 0; i < TapCount; ++i )
	{
		//Symmetric about the centre, and EXACTLY zero for a single tap:
		//( 0 + 0.5 ) / 1 - 0.5 is zero in floating point as well as in
		//arithmetic, so a one-pixel slit is a fetch and not an average of one.
		float offset = ( float( i ) + 0.5 ) / float( TapCount ) - 0.5;
		float lane   = across + offset * SlitSpan;

		vec2 p = AxisVertical > 0.5 ? vec2( along, lane ) : vec2( lane, along );
		total += atInstant( p );
	}

	fragColor = total / float( TapCount );
}
)";

//---------------------------------------------------------------------------
// 3. strip -- the ring, oldest to newest along the scan axis.
//---------------------------------------------------------------------------
const char* const kStripShader = R"(#version 410 core

uniform sampler2D RingTexture;
uniform sampler2D SourceTexture;

uniform int RingLength;  //columns the strip holds
uniform int RingRows;    //samples along the slit
uniform int WritePos;    //the slot the NEXT column goes into, 0..RingLength-1
uniform int FilledCount; //slots ever written, 0..RingLength
uniform int FillMode;    //0 build, 1 scroll, 2 once
uniform int BackgroundMode;//0 black, 1 source, 2 transparent

uniform float AxisVertical;
uniform float Reversed;
uniform float MixAmount;
uniform vec2 HalfTexel;

in vec2 uv;
out vec4 fragColor;

void main()
{
	vec2 picture = clamp( uv, HalfTexel, vec2( 1.0 ) - HalfTexel );
	vec4 live = texture( SourceTexture, picture );

	//Which way the two axes run. Time runs left to right across a horizontal
	//sweep and top to bottom down a vertical one, so the vertical case reads
	//uv.y upside down -- uv.y is 0 at the BOTTOM.
	float scan  = AxisVertical > 0.5 ? ( 1.0 - uv.y ) : uv.x;
	float along = AxisVertical > 0.5 ? uv.x : uv.y;

	if( Reversed > 0.5 )
		scan = 1.0 - scan;

	//A whole column. A fraction of the way between two of them is a moment
	//nobody sampled, so a magnified strip is blocks of identical pixels and
	//not a gradient between two instants that were never adjacent.
	int slot = clamp( int( floor( scan * float( RingLength ) ) ), 0, RingLength - 1 );
	int row  = clamp( int( floor( along * float( RingRows ) ) ), 0, RingRows - 1 );

	//Scroll reads the ring through the write head, so the newest column is
	//always at the same edge and the whole picture slides. Build and Once read
	//it in place, so the head is a seam that marches across.
	//
	//Integer arithmetic, and both operands non-negative. A float mod here
	//returns the modulus itself instead of zero wherever the division rounds a
	//hair low, which puts the newest column at the oldest end for one frame.
	int index = slot;
	if( FillMode == 1 )
		index = ( slot + WritePos ) % RingLength;

	bool written = FilledCount >= RingLength;
	if( !written )
		written = FillMode == 1 ? ( slot >= RingLength - FilledCount )
		                        : ( slot < WritePos );

	vec4 strip = vec4( 0.0, 0.0, 0.0, 1.0 );
	if( written )
		strip = texelFetch( RingTexture, ivec2( index, row ), 0 );
	else if( BackgroundMode == 1 )
		strip = live;
	else if( BackgroundMode == 2 )
		strip = vec4( 0.0 );

	fragColor = mix( live, strip, MixAmount );
}
)";

} // namespace photofinish
