/**
 * Photofinish — browser demo.
 *
 * A strip camera. The one idea, from `AGENTS.md`: **a photo-finish camera has
 * no two-dimensional frame. It has one column of sensor, and the film moves
 * past it — so the horizontal axis of the picture is TIME, not space.** Anything
 * static becomes a streak, anything moving is stretched or squashed by one over
 * its speed, something crossing at exactly the film speed keeps its true
 * proportions, and something crossing the other way comes out backwards. None
 * of that is coded anywhere: one column is sampled per tick and pushed onto a
 * ring, and all four fall out.
 *
 * ---------------------------------------------------- what is the plugin's own
 *
 * The three passes. `VERTEX`, `COPY`, `SLIT` and `STRIP` below are
 * `kVertexShader`, `kCopyShader`, `kSlitShader` and `kStripShader` from
 * `source/Shaders.cpp`, copied across unedited; `demo/tools/check_shaders.py`
 * compares them character for character and `tools/verify.sh` runs it.
 *
 * ---------------------------------------------------- what is a port
 *
 * The clock and the ring's bookkeeping, which is the other half of the plugin:
 * `Strip.cpp` (`advance`, `blendAt`, `kColumnSnap`, `kMaxFrameDelta`) in full,
 * `Controls.cpp` in full, `ColumnPeriod()`, the sorted option lists
 * `declareOptions` builds, and `EnsureBuffers()` / `ProcessOpenGL()` pass for
 * pass — the two frame copies, the seeding of the previous frame, the ring
 * cleared whenever its shape moves, the column cap, Fill = Once stopping, and
 * one draw per column into a one-pixel-wide viewport. Nothing checks that port
 * but a reader; `pftest --ring`, `--static`, `--clock` and `--resize` check the
 * C++ and have never heard of this page.
 *
 * **The page keeps its buffers across frames**, as the plugin does. The ring and
 * the previous frame live in PassBuffers the kit never clears; the kit clears
 * only the canvas. That is what the effect is — a strip that is rebuilt from
 * scratch each frame would be a different effect, and a wrong one.
 *
 * ---------------------------------------------------- what is not the plugin
 *
 * **The clock is the page's, in seconds.** The plugin measures whether its host
 * sends seconds or milliseconds by voting against a steady clock; the page is
 * the host here and hands over seconds outright, which is what `pftest` does
 * through `SetClockScaleForTest( 1.0 )`. Only differences of it are used, as in
 * the plugin. Pause stops the film, because the host's clock stops; Step is one
 * 60 fps frame of it.
 *
 * **There is no host tempo.** `Sync = Beat` and `Bar` take their rate from
 * `SetBeatInfo`, which a browser does not have; the page runs them at 120 bpm,
 * which is the FFGL SDK's own default and what Resolume sends until a tempo is
 * set. The sweep is a rate, not a lock to the bar line, in the plugin too.
 *
 * **The About block is absent**, as on every page in this suite.
 *
 * ---------------------------------------------------- decided, not asked
 *
 * **The clip list starts on Lights on black.** Three blobs cross the slit in
 * both directions at different speeds, which puts the stretched, the squashed
 * and the backwards runner in the first ten seconds. The Geometry card, which
 * is static, is on the list too: a static picture through a strip camera is
 * nothing but horizontal streaks, which is the first bullet of the idea.
 *
 * **There is a line under the canvas** giving the ring's shape, the write head
 * and the columns taken this frame, because the ring is state a visitor cannot
 * otherwise see — and Sweep Length clearing it is otherwise indistinguishable
 * from a glitch.
 */

import { mountDemo } from './vendor/demo.js';
import { Program, PassBuffer, bindTexture } from './vendor/gl.js';

//---------------------------------------------------------------------------
// Shaders — verbatim from source/Shaders.cpp. Do not edit here.
//---------------------------------------------------------------------------

const VERTEX = `#version 410 core

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
`;

const COPY = `#version 410 core

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
`;

const SLIT = `#version 410 core

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
`;

const STRIP = `#version 410 core

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
`;

//===========================================================================
// The port: Controls.cpp.
//===========================================================================

const clamp = (v, lo, hi) => (v < lo ? lo : v > hi ? hi : v);

/// lo * (hi/lo)^t, the usual geometric mapping.
const geometric = (lo, hi, t) => lo * Math.pow(hi / lo, clamp(t, 0.0, 1.0));

/// C++ std::lround: half away from zero. Math.round is half towards +infinity,
/// which differs on negative halves only -- none of these are negative, but the
/// port says what it means.
const lround = (x) => Math.sign(x) * Math.round(Math.abs(x));

/// Steps on the Sweep Length ladder: what stops a drag clearing the ring on
/// every pixel.
const kSweepSteps = 48;

const SlitPosition = (v) => clamp(v, 0.0, 1.0);

function SlitWidthPixels(v) {
  const pixels = geometric(1.0, 64.0, v);
  return Math.max(1, lround(pixels));
}

const SlitLean = (v) => clamp((v - 0.5) * 2.0, -1.0, 1.0);

const TimePerColumnSeconds = (v) => geometric(0.0005, 0.5, v);

function SweepColumns(v, axisPixels) {
  const longest = Math.max(8, axisPixels);
  // Quantise the SLIDER, not the answer.
  const t = Math.round(clamp(v, 0.0, 1.0) * kSweepSteps) / kSweepSteps;
  const columns = geometric(8.0, longest, t);
  return clamp(lround(columns), 8, longest);
}

//===========================================================================
// The port: Strip.cpp, the whole of the clock.
//===========================================================================

/// How close to a column boundary counts as being on it, in columns -- sized
/// against the clock's origin, not against a frame period. See Strip.h.
const kColumnSnap = 1e-6;

/// The most host time one frame may advance the strip by, in seconds.
const kMaxFrameDelta = 0.25;

function advance(phase, dt, tpc) {
  const out = { columns: 0, phase: clamp(phase, -kColumnSnap, 1.0) };
  if (!(tpc > 0.0) || !(dt > 0.0)) return out;

  const x = out.phase + dt / tpc;
  // The snap is on the FLOOR, not on x, so the remainder carried forward is the
  // true one and is allowed to go very slightly negative.
  const whole = Math.floor(x + kColumnSnap);
  out.columns = whole > 0.0 ? whole : 0;
  out.phase = clamp(x - whole, -kColumnSnap, 1.0);
  return out;
}

function blendAt(index, phase, dt, tpc) {
  if (!(dt > 0.0) || !(tpc > 0.0)) return 1.0;
  const within = ((index + 1.0 - phase) * tpc) / dt;
  return clamp(within, 0.0, 1.0);
}

//===========================================================================
// The option lists, as the constructor declares them.
//
// declareOptions() sorts a list by name, case-insensitively and stably, and
// gives each display slot the VALUE it has always had -- so the dropdown reads
// alphabetically and a saved composition keeps its meaning. The kit's dropdown
// stores the slot, so the page carries the slot -> value table the host would.
// Sync and Interpolate are declared in order, because position is their
// meaning.
//===========================================================================

const kAxisHorizontal = 0;
const kAxisVertical = 1;
const kDirectionReverse = 1;
const kSyncFree = 0;
const kSyncBar = 2;
const kInterpolateLinear = 1;
const kFillOnce = 2;

function declareOptions(names) {
  const order = names.map((_, i) => i);
  // Array.prototype.sort is stable, as std::stable_sort is.
  order.sort((a, b) => {
    const x = names[a].toLowerCase();
    const y = names[b].toLowerCase();
    return x < y ? -1 : x > y ? 1 : 0;
  });
  return { elements: order.map((i) => names[i]), values: order };
}

function declareOptionsInOrder(names) {
  return { elements: [...names], values: names.map((_, i) => i) };
}

const OPTIONS = {
  axis: declareOptions(['Horizontal', 'Vertical']),
  direction: declareOptions(['Forward', 'Reverse']),
  sync: declareOptionsInOrder(['Free', 'Beat', 'Bar']),
  interpolate: declareOptionsInOrder(['Nearest', 'Linear']),
  fill: declareOptions(['Build', 'Scroll', 'Once']),
  background: declareOptions(['Black', 'Source', 'Transparent']),
};

/// The stored value for a dropdown slot -- optionValue() in the plugin, after
/// the host has turned the slot into the element's value.
const optionValue = (id, slot) => OPTIONS[id].values[clamp(Math.round(slot), 0, OPTIONS[id].values.length - 1)];
/// The slot showing a given value, for defaults and presets.
const slotOf = (id, value) => OPTIONS[id].values.indexOf(value);

/// What the SDK and Resolume send until a tempo is set. The page has no host
/// transport, so this is the tempo Sync = Beat / Bar runs at.
const kPageBpm = 120.0;

/// What the line under the canvas reports. Written by the renderer.
const telemetry = { ringLength: 0, ringRows: 0, writePos: 0, filled: 0, columns: 0, period: 0, dt: 0, scanPixels: 960 };

//===========================================================================
// The plugin, pass for pass: EnsureBuffers(), ColumnPeriod(), ProcessOpenGL().
//===========================================================================

function createRenderer(gl, quad) {
  const copyShader = new Program(gl, VERTEX, COPY, 'copy');
  const slitShader = new Program(gl, VERTEX, SLIT, 'slit');
  const stripShader = new Program(gl, VERTEX, STRIP, 'strip');

  // The two frame copies are linear (a tilted or wide slit lands between
  // texels, which is sampling SPACE); the ring is nearest, always, because a
  // column is one instant.
  const frames = [new PassBuffer(gl, { filter: 'linear' }), new PassBuffer(gl, { filter: 'linear' })];
  const ring = new PassBuffer(gl, { filter: 'nearest' });

  let frameIndex = 0;
  let framesSeeded = false;
  let pictureWidthWas = 0;
  let pictureHeightWas = 0;

  let ringLength = 0;
  let ringRows = 0;
  let writePos = 0;
  let filled = 0;

  let lastNow = -1.0;
  let columnPhase = 0.0;

  function ensureBuffers(pictureWidth, pictureHeight, wantLength, wantRows) {
    for (const buffer of frames) buffer.ensure(pictureWidth, pictureHeight, gl.RGBA8);

    if (pictureWidth !== pictureWidthWas || pictureHeight !== pictureHeightWas) {
      // The buffers have just been reallocated, so neither holds a previous
      // frame any more.
      framesSeeded = false;
      pictureWidthWas = pictureWidth;
      pictureHeightWas = pictureHeight;
    }

    const shapeMoved = wantLength !== ringLength || wantRows !== ringRows;
    ring.ensure(wantLength, wantRows, gl.RGBA8);

    if (shapeMoved) {
      // Changing the length REINTERPRETS the ring rather than shuffling it, so
      // it starts empty.
      ring.clearTo(0, 0, 0, 0);
      gl.bindFramebuffer(gl.FRAMEBUFFER, null);
      ringLength = wantLength;
      ringRows = wantRows;
      writePos = 0;
      filled = 0;
      columnPhase = 0.0;
    }
  }

  function columnPeriod(params) {
    const sync = optionValue('sync', params.get('sync'));
    if (sync === kSyncFree || ringLength <= 0) return TimePerColumnSeconds(params.get('timePerColumn'));

    const tempo = kPageBpm;
    const barSeconds = 240.0 / tempo; // four beats to the bar
    const sweep = sync === kSyncBar ? barSeconds : barSeconds / 4.0;
    return sweep / ringLength;
  }

  return {
    render({ input, params, width, height, time }) {
      const p = (id) => params.get(id);
      const pictureWidth = width;
      const pictureHeight = height;

      //------------------------------------------------------------------
      // Time. Differences only -- see Strip.h. The page's clock is seconds.
      //------------------------------------------------------------------
      const now = time;
      const dt = lastNow >= 0.0 ? clamp(now - lastNow, 0.0, kMaxFrameDelta) : 0.0;
      lastNow = now;

      //------------------------------------------------------------------
      // What the controls say.
      //------------------------------------------------------------------
      const axis = optionValue('axis', p('axis'));
      const direction = optionValue('direction', p('direction'));
      const fill = optionValue('fill', p('fill'));
      const background = optionValue('background', p('background'));
      const interp = optionValue('interpolate', p('interpolate'));
      const frozen = p('freeze') >= 0.5;

      const vertical = axis === kAxisVertical;
      const scanPixels = vertical ? pictureHeight : pictureWidth;
      const slitPixels = vertical ? pictureWidth : pictureHeight;

      const wantLength = SweepColumns(p('sweepLength'), scanPixels);
      const wantRows = Math.max(1, slitPixels);

      ensureBuffers(pictureWidth, pictureHeight, wantLength, wantRows);

      const period = columnPeriod(params);

      //------------------------------------------------------------------
      // 1. The picture, into one of the two frame copies.
      //------------------------------------------------------------------
      frameIndex = 1 - frameIndex;
      const frameNow = frames[frameIndex];
      const frameWas = frames[1 - frameIndex];

      gl.disable(gl.BLEND);
      const copyInto = (target) => {
        target.bind();
        copyShader.use();
        bindTexture(gl, 0, input.texture);
        copyShader.setSampler('SourceTexture', 0);
        copyShader.set('MaxUV', 1.0, 1.0);
        copyShader.set('HalfTexel', 0.5 / pictureWidth, 0.5 / pictureHeight);
        quad.draw();
      };

      copyInto(frameNow);
      if (!framesSeeded) {
        // The first frame has no previous frame, and an unseeded one is not
        // "black for a frame": it is blended into every column taken in it.
        copyInto(frameWas);
        framesSeeded = true;
      }

      //------------------------------------------------------------------
      // 2. The columns this frame owes.
      //------------------------------------------------------------------
      const phaseBefore = columnPhase;
      const sched = frozen ? { columns: 0, phase: columnPhase } : advance(columnPhase, dt, period);
      columnPhase = sched.phase;

      // Never more than a whole ring in one frame.
      let columns = Math.min(sched.columns, ringLength);
      // Once is the mode that stops. Build and Scroll wrap for ever.
      if (fill === kFillOnce && filled + columns > ringLength) columns = Math.max(0, ringLength - filled);

      if (columns > 0) {
        ring.bind();
        slitShader.use();
        bindTexture(gl, 0, frameNow.texture);
        bindTexture(gl, 1, frameWas.texture);

        const taps = SlitWidthPixels(p('slitWidth'));
        const acrossPixels = vertical ? pictureHeight : pictureWidth;

        slitShader.setSampler('FrameNow', 0);
        slitShader.setSampler('FrameWas', 1);
        slitShader.set('TimeIsLinear', interp === kInterpolateLinear ? 1.0 : 0.0);
        slitShader.set('SlitCentre', SlitPosition(p('slitPosition')));
        slitShader.set('SlitSpan', taps / acrossPixels);
        slitShader.set('SlitSlope', SlitLean(p('slitAngle')));
        slitShader.set('AxisVertical', vertical ? 1.0 : 0.0);
        slitShader.setInt('TapCount', taps);
        slitShader.set('HalfTexel', 0.5 / pictureWidth, 0.5 / pictureHeight);

        for (let i = 0; i < columns; i += 1) {
          slitShader.set('Blend', blendAt(i, phaseBefore, dt, period));

          // ONE COLUMN of the ring: a viewport one pixel wide rasterises
          // exactly this column of the full-frame quad and nothing else.
          gl.viewport(writePos, 0, 1, ringRows);
          quad.draw();

          writePos = (writePos + 1) % ringLength;
          if (filled < ringLength) filled += 1;
        }
      }

      //------------------------------------------------------------------
      // 3. The strip, to the canvas, at the host's viewport.
      //------------------------------------------------------------------
      gl.bindFramebuffer(gl.FRAMEBUFFER, null);
      gl.viewport(0, 0, width, height);

      stripShader.use();
      bindTexture(gl, 0, ring.texture);
      bindTexture(gl, 1, frameNow.texture);
      stripShader.setSampler('RingTexture', 0);
      stripShader.setSampler('SourceTexture', 1);
      stripShader.setInt('RingLength', ringLength);
      stripShader.setInt('RingRows', ringRows);
      stripShader.setInt('WritePos', writePos);
      stripShader.setInt('FilledCount', filled);
      stripShader.setInt('FillMode', fill);
      stripShader.setInt('BackgroundMode', background);
      stripShader.set('AxisVertical', vertical ? 1.0 : 0.0);
      stripShader.set('Reversed', direction === kDirectionReverse ? 1.0 : 0.0);
      stripShader.set('MixAmount', clamp(p('mix'), 0.0, 1.0));
      stripShader.set('HalfTexel', 0.5 / pictureWidth, 0.5 / pictureHeight);
      quad.draw();

      Object.assign(telemetry, { ringLength, ringRows, writePos, filled, columns, period, dt, scanPixels });
    },
  };
}

//===========================================================================
// The controls, read out of Photofinish's constructor. Same names, same groups,
// same order, same defaults, same dropdown elements in the same (sorted)
// order. Absent: the About block.
//===========================================================================

const std = (id, name, def, group, extra = {}) => ({
  id, name, type: 'standard', default: def, group,
  ...(typeof extra === 'string' ? { hint: extra } : extra),
});
const opt = (id, name, def, group, hint) => ({
  id, name, type: 'option', elements: OPTIONS[id].elements, default: slotOf(id, def), group, hint,
});
const bool = (id, name, def, group, hint) => ({ id, name, type: 'boolean', default: def, group, hint });

const demo = mountDemo({
  name: 'Photofinish',
  pluginId: 'PF01',
  tagline:
    'A strip camera. There is no frame: there is one column of sensor, and the film moves past it, so the horizontal axis of the picture is time rather than space. Anything standing still becomes a streak; anything moving is stretched or squashed by one over its speed; something crossing at exactly the film speed keeps its true proportions; something crossing the other way comes out backwards. Nothing is warped or blurred on purpose — one column is taken per tick and pushed onto a ring, and all of that follows.',
  repo: 'https://github.com/stoatworks-labs/photofinish',
  page: 'https://stoatworks-labs.com/software/photofinish/',
  video: 'https://www.youtube.com/watch?v=gLk9shewRcU',

  // Background = Transparent leaves the unwritten strip empty for the layer
  // below.
  showBackdrop: true,

  params: [
    std('slitPosition', 'Slit Position', 0.5, 'Slit', {
      display: (v) => SlitPosition(v).toFixed(3),
      hint: 'Where the slit sits across the picture: its x for a horizontal sweep, its y for a vertical one.',
    }),
    std('slitWidth', 'Slit Width', 0.0, 'Slit', {
      display: (v) => `${SlitWidthPixels(v)} px`,
      hint: 'Looks like a blur control and is not. A wider slit averages across the direction of travel, and because that axis is time it is motion blur in the time axis — arrived at, not added. It does nothing at all to the other axis.',
    }),
    std('slitAngle', 'Slit Angle', 0.5, 'Slit', {
      display: (v) => `slope ${SlitLean(v).toFixed(2)}`,
      hint: 'The slit’s lean, as a slope in normalised picture space: no trigonometry and no pole at the end of the slider.',
    }),
    opt('axis', 'Axis', kAxisHorizontal, 'Slit',
      'Horizontal: the slit is a vertical line and time runs across. Vertical: the slit is a horizontal line and time runs down.'),
    opt('direction', 'Direction', 0, 'Slit', 'Which end of the scan axis is the newest column.'),

    std('timePerColumn', 'Time Per Column', 0.307, 'Time', {
      // Seconds are the plugin's unit. Columns per 60 fps frame would not fit
      // beside the slider; the line under the canvas says how many were taken.
      display: (v) => `${(TimePerColumnSeconds(v) * 1000).toFixed(2)} ms`,
      hint: 'The film speed, and the only rate in the plugin. Everything else about the picture follows from it and from how fast the subject moves. Below a frame period the columns between two delivered frames are built from both of them, which is what Interpolate is for.',
    }),
    std('sweepLength', 'Sweep Length', 1.0, 'Time', {
      display: (v) => `${SweepColumns(v, telemetry.scanPixels)} columns`,
      hint: 'How many columns the strip holds, 8 up to the full axis, in 48 steps. Changing it clears the ring — the ring is indexed modulo its own length, so a new length would file every column under the wrong time. Below the axis length a column is shown as a block, never as a blend of two instants.',
    }),
    opt('sync', 'Sync', kSyncFree, 'Time',
      'Free: Time Per Column is the rate. Beat or Bar: one whole sweep per beat or per bar, and Time Per Column is ignored. A rate from the tempo, not a lock to the bar line. This page has no host tempo and runs at 120 bpm.'),
    opt('interpolate', 'Interpolate', kInterpolateLinear, 'Time',
      'A column is taken at an instant the host did not deliver. Linear builds it from the two frames around it; Nearest takes whichever is closer.'),

    opt('fill', 'Fill', 1, 'Output',
      'Build: the head marches across and wraps, with a moving seam — a strip printer. Scroll: the newest column is always at the same edge and the picture slides. Once: fill once and stop.'),
    opt('background', 'Background', 0, 'Output', 'What an unwritten column shows: black, the live clip, or nothing.'),
    std('mix', 'Mix', 1.0, 'Output'),
    bool('freeze', 'Freeze', 0, 'Output', 'The film stops. Nothing is written until it is released.'),
  ],

  sources: ['spot', 'scene', 'alpha', 'grid', 'bars', 'detail'],

  // The plugin ships no factory presets, so these are the page's own —
  // expressed entirely in the plugin's parameters and reachable with the sliders.
  presets: {
    'Strip printer (Build, over the clip)': { fill: slotOf('fill', 0), background: slotOf('background', 1) },
    'One column per frame (at 60 fps)': { timePerColumn: 0.5076 },
    'Fast film': { timePerColumn: 0.12 },
    'Slow film': { timePerColumn: 0.65 },
    'Time runs down (vertical)': { axis: slotOf('axis', kAxisVertical) },
    'Leaning slit': { slitAngle: 0.8 },
    'Wide slit: motion blur in time': { slitWidth: 0.75 },
    'Short strip, blocks not blends': { sweepLength: 0.4 },
    'One sweep per bar (120 bpm)': { sync: slotOf('sync', kSyncBar) },
    'Fill once and hold': { fill: slotOf('fill', kFillOnce) },
  },

  differences: [
    'The three passes are the plugin’s own GLSL, and demo/tools/check_shaders.py fails the repository’s verify script if a character of any of the four shader strings drifts. What is a PORT is the clock and the ring’s bookkeeping — Strip.cpp and Controls.cpp in full, ColumnPeriod(), and EnsureBuffers() and ProcessOpenGL() pass for pass. Nothing checks that port but a reader.',
    'This page keeps state between frames exactly as the plugin does: the ring and the previous frame persist, the ring is cleared only when its shape changes, and a composition resize re-seeds both frame copies. Changing the Composition size therefore starts a new strip, as it does in the host.',
    'The clock is the page’s own, in seconds, and only its differences are used. The plugin works out for itself whether its host sends seconds or milliseconds; here the page is the host and says seconds, as the plugin’s harness does. Pause stops the film because the host’s clock has stopped. A backgrounded tab resumes with a jump of at most a quarter of a second, the plugin’s own cap.',
    'Sync = Beat and Bar take their rate from the host’s tempo, and a browser has none: they run here at 120 bpm, the FFGL SDK’s default and what Resolume sends until a tempo is set.',
    'Photofinish has no audio input, so there is no audio caveat beyond that one.',
    'The plugin’s proofs — a static picture is bit-for-bit one column repeated, the ring keeps its time at every Sweep Length, the same take from t = 0 and from 499,217,238 ms is identical, a resize never blends toward black — are pftest in the repository. Nothing on this page measures anything.',
  ],

  createRenderer,
});

//---------------------------------------------------------------------------
// The ring line. Reports the plugin's state; measures nothing. Skipped in
// embed mode, where there is no reader.
//---------------------------------------------------------------------------
if (demo && !new URLSearchParams(window.location.search).has('embed')) {
  const stage = document.querySelector('.stage');
  if (stage) {
    const line = document.createElement('p');
    line.className = 'stage__status';
    stage.append(line);
    setInterval(() => {
      const { ringLength, ringRows, writePos, filled, columns, period } = telemetry;
      if (!ringLength) return;
      const full = filled >= ringLength ? 'full' : `${filled} of ${ringLength} written`;
      line.textContent =
        `Ring: ${ringLength} columns × ${ringRows} rows, ${full}, head at ${writePos}. `
        + `A column every ${(period * 1000).toFixed(2)} ms; ${columns} taken in the last frame.`;
    }, 250);
  }
}
