#pragma once

/**
	The passes, as GLSL 4.10 source.

	Three fragment shaders and one vertex shader, and that is the whole of the
	GPU side. There is no mirrored library here and nothing is transcribed
	twice: the arithmetic a test needs to assert on is the column SCHEDULE,
	which is pure C++ in `Strip.cpp` and never reaches a shader, and the ring's
	read mapping, which is proved through rendered pixels by `pftest --ring`
	rather than by a second copy of it in the harness.

	1. **copy**  picture size. The host's texture into one of ours, resolving
	             `MaxUV` and the half-texel inset once so that no later pass
	             has to think about either. Run every frame into whichever of
	             the two frame buffers is not the previous one.

	2. **slit**  ONE COLUMN wide, run once per column the schedule asks for.
	             Samples the two frame copies at the slit, blends between them
	             at the column's own instant, averages the taps across the
	             slit's width, and writes the result into one column of the
	             ring. The viewport for this pass is `( writePos, 0, 1, rows )`
	             -- which is why `uv.x` is 0.5 at every fragment and carries
	             nothing.

	3. **strip** output size. Reads the ring with `texelFetch`, oldest to
	             newest along the scan axis, and puts the background under the
	             columns that have not been written yet.

	**`texelFetch`, not `texture`.** A column of the ring is one instant. A
	filtered read between two columns would return a picture of a moment that
	was never sampled, and at a magnifying Sweep Length it would do that for
	most of the screen. `texelFetch` also removes the sampler from the question
	entirely, which is what lets `pftest --static` assert bit-for-bit equality
	rather than a tolerance -- see AGENTS.md, "Every numeric check".

	**Integer arithmetic on the ring index.** GLSL's float `mod` is
	`x - y * floor( x / y )`, and where the subtraction lands on an exact
	multiple the division can round a hair below the integer, `floor` takes it
	down a whole step, and the result comes back as `y` rather than zero. On a
	ring that is the newest column appearing at the oldest end for one frame in
	a few thousand. Both operands here are non-negative integers, which also
	keeps clear of GLSL leaving `%` and `/` undefined on negative operands.
*/

namespace photofinish
{

extern const char* const kVertexShader;
extern const char* const kCopyShader;
extern const char* const kSlitShader;
extern const char* const kStripShader;

} // namespace photofinish
