#!/usr/bin/env bash
#
# Everything that can be checked without a host, in one go.
#
#   tools/verify.sh
#
# It builds FRESH and UNIVERSAL, because the two failures this repo cannot see
# any other way are both invisible in a build log:
#
#   - CMake latches the architecture list when the first target is created, so
#     a late -DCMAKE_OSX_ARCHITECTURES is silently ignored and an arm64-only
#     binary is reported as a success. `lipo` is the only witness.
#   - a stale build directory configured for one architecture will happily
#     produce a correct-looking bundle for the other.
#
# The checks, and what each one answers that none of the others can:
#
#   shaders     does every shader compile, through a real GLSL compiler, before
#               a host has to find out. A shader that will not compile presents
#               to an operator as "the effect does nothing", with the real
#               message buried in the diagnostics log.
#   schedule    the column schedule, with no GL at all -- the one part of this
#               plugin a machine with no GPU can still prove.
#   static      a still picture renders as exact horizontal streaks. Bitwise.
#   ring        the strip holds the last min(N, L) columns, in order, at two
#               rasters and at a magnifying Sweep Length.
#   clock       the same take from t=0 and from 499,217,238 ms, bitwise. This
#               plugin is a clock with a picture attached and that number is
#               where a float stops resolving a frame.
#   interp      a column taken between two frames is their blend, to one code.
#   width       a moving bar's rendered width against b*c/v, to one column.
#   matched     at the film speed, the object's own proportions, in absolute
#               pixels at two rasters.
#   reverse     the other way round comes out mirrored, measured as skewness.
#   negative    every check above, perturbed, asserted to FAIL. A check that
#               cannot fail is not a check.
#   sweep       no control is silently dead. A GLSL uniform whose name does not
#               match the C++ is ignored without a word.
#   plugMain    does the bundle contain a plugin at all -- a file-scope
#               CFFGLPluginInfo nothing names, which a linker may drop while
#               still producing a bundle that loads and exports plugMain.
#   lipo        is the macOS build really universal.
#   plist       does CFBundleExecutable name the binary that is actually on
#               disk. If it does not, codesign reports "code object is not
#               signed at all" about a NESTED object and mentions neither the
#               plist nor the cause -- and that is a release-time failure with
#               no local symptom.
#   codesign    the exact command the release job runs, against a copy.
#   oxbow       instantiation and 120 frames in a real FFGL host, which nothing
#               else here reaches, plus the name, the id and the type as a host
#               sees them.
#   bench       the render cost. Not pass/fail -- there is no threshold worth
#               asserting on somebody else's GPU -- but a verify run leaves a
#               timing on the record, which is what turns "it feels slower"
#               into a comparison.
#
# The last four are release-job work done locally on purpose. A check that only
# runs in CI, after a tag, is a check that will catch you after the tag.
#
set -uo pipefail

cd "$(dirname "$0")/.."

BUILD="${BUILD:-build-verify}"
failures=0

step() { printf '\n\033[1m== %s\033[0m\n' "$1"; }
pass() { printf '   \033[32mok\033[0m   %s\n' "$1"; }
fail() { printf '   \033[31mFAIL\033[0m %s\n' "$1"; failures=$(( failures + 1 )); }

#---------------------------------------------------------------------------
# Every shader, through a real GLSL compiler.
#
# --target-env=opengl4.5 with -fauto-map-locations: glslc targets SPIR-V, which
# demands an explicit layout( location ) on every uniform and varying. Those are
# Vulkan rules and not GLSL ones, and without the flag every shader "fails" for
# reasons that have nothing to do with the code.
#
# glslc is optional -- `brew install shaderc` -- so a machine without it skips
# rather than fails.
#---------------------------------------------------------------------------
shaders_compile() {
	local dir bad=0 n=0 shader

	if ! command -v glslc >/dev/null 2>&1; then
		printf '   skipped: glslc not installed (brew install shaderc)\n'
		return 0
	fi

	dir="$( mktemp -d )"

	python3 - "$dir" <<'SHADERS_PY'
import re, sys, pathlib
out = pathlib.Path( sys.argv[ 1 ] )

# Where this repo keeps its GLSL. Every shader here is a complete one: nothing
# is assembled at run time, because nothing is mirrored between GLSL and C++.
FILES = [
	"source/Shaders.cpp",
]

# A shader may be several adjacent raw strings (MSVC caps one literal at about
# 16 KB), so everything up to the terminating semicolon is joined.
named = {}
for f in FILES:
	text = pathlib.Path( f ).read_text()
	for m in re.finditer( r'(\w+)\s*=\s*((?:\s*(?://[^\n]*\n)*\s*R"\(.*?\)")+)\s*;', text, re.S ):
		named[ m.group( 1 ) ] = "".join( re.findall( r'R"\((.*?)\)"', m.group( 2 ), re.S ) )

def emit( name, body ):
	# The vertex shader is the one that writes gl_Position; everything else is
	# a fragment shader. glslc takes the stage from the extension.
	ext = ".vert" if re.search( r"\bgl_Position\s*=", body ) else ".frag"
	( out / ( name + ext ) ).write_text( body )

for name, body in named.items():
	if body.lstrip().startswith( "#version" ) and "void main" in body:
		emit( name, body )
SHADERS_PY

	for shader in "$dir"/*.vert "$dir"/*.frag; do
		[ -e "$shader" ] || continue
		n=$(( n + 1 ))
		if ! glslc --target-env=opengl4.5 -fauto-map-locations \
			   "$shader" -o /dev/null 2>"$dir/err"; then
			printf '   %s does not compile\n' "$( basename "$shader" )"
			sed "s|$dir/||; s|^|      |" "$dir/err"
			bad=$(( bad + 1 ))
		fi
	done

	if [ "$n" -eq 0 ]; then
		# No shaders at all is a FAILURE, not a pass. It means the extraction
		# above has lost track of where this repo keeps its GLSL, and a check
		# that silently looks at nothing is worse than no check.
		printf '   no shaders were extracted -- the extraction has gone stale\n'
		rm -rf "$dir"
		return 1
	fi

	# Four: the vertex shader, copy, slit and strip.
	if [ "$bad" -eq 0 ]; then
		printf '   %d shaders, all compile\n' "$n"
	fi
	rm -rf "$dir"
	return "$bad"
}

step "shaders"
if shaders_compile; then
	pass "every shader compiles"
else
	fail "a shader does not compile"
fi

step "build (fresh, universal, Release)"
rm -rf "$BUILD"
if ! cmake -B "$BUILD" -DCMAKE_BUILD_TYPE=Release >/dev/null 2>&1; then
	fail "configure failed -- run: cmake -B $BUILD -DCMAKE_BUILD_TYPE=Release"
	exit 1
fi
if cmake --build "$BUILD" --parallel >/dev/null 2>&1; then
	pass "builds"
else
	fail "build failed -- run: cmake --build $BUILD"
	exit 1
fi

PFTEST="$BUILD/pftest"

step "checks"
# One process, so the GL context is stood up once. Each check prints its own
# numbers; the summary here is the verdict.
if "$PFTEST" --schedule --static --ring --clock --interp --width --matched \
             --reverse --negative > /tmp/photofinish-checks.txt 2>&1; then
	grep -c '^   ok' /tmp/photofinish-checks.txt \
		| xargs -I{} printf '   {} assertions passed (full output: /tmp/photofinish-checks.txt)\n'
	pass "every closed-form check"
else
	printf '\n'
	grep -E '^   (FAIL|ok)' /tmp/photofinish-checks.txt | grep FAIL | sed 's/^/   /'
	fail "a check failed -- see /tmp/photofinish-checks.txt"
fi

# The headline number from every check, on the record, whether or not anything
# failed. A run that only says "all checks passed" says nothing about how much
# margin there was, and margin is the only thing that says whether a tolerance
# is honest.
sed -n '/^== summary/,/^$/p' /tmp/photofinish-checks.txt | sed 's/^/   /'

step "sweep"
if python3 tools/sweep.py --binary "$PFTEST" > /tmp/photofinish-sweep.txt 2>&1; then
	tail -1 /tmp/photofinish-sweep.txt | sed 's/^/   /'
	pass "no control is silently dead"
else
	tail -4 /tmp/photofinish-sweep.txt | sed 's/^/   /'
	fail "tools/sweep.py reports a dead control"
fi

BUNDLE="$BUILD/Photofinish.bundle"
BIN="$BUNDLE/Contents/MacOS/Photofinish"

if [ "$(uname)" = "Darwin" ] && [ -d "$BUNDLE" ]; then
	step "registration"
	# `nm ... | grep -q X` FAILS when grep FINDS its match under `set -o pipefail`:
	# grep exits at once, nm takes SIGPIPE, and the pipeline reports failure.
	# Capture and match instead of piping.
	syms=$(nm -gU "$BIN" 2>/dev/null)
	case "$syms" in
		*_plugMain*) pass "exports plugMain" ;;
		*) fail "no plugMain -- the bundle contains no plugin" ;;
	esac

	step "lipo"
	archs=$(lipo -archs "$BIN" 2>/dev/null)
	printf '   architectures: %s\n' "$archs"
	case "$archs" in *arm64*) pass "arm64 present" ;; *) fail "no arm64 (got: $archs)" ;; esac
	case "$archs" in *x86_64*) pass "x86_64 present" ;; *) fail "no x86_64 (got: $archs) -- a universal build was asked for" ;; esac

	step "plist"
	exe=$(/usr/libexec/PlistBuddy -c "Print :CFBundleExecutable" "$BUNDLE/Contents/Info.plist" 2>/dev/null)
	ident=$(/usr/libexec/PlistBuddy -c "Print :CFBundleIdentifier" "$BUNDLE/Contents/Info.plist" 2>/dev/null)
	version=$(/usr/libexec/PlistBuddy -c "Print :CFBundleVersion" "$BUNDLE/Contents/Info.plist" 2>/dev/null)
	if [ -n "$exe" ] && [ -f "$BUNDLE/Contents/MacOS/$exe" ]; then
		pass "CFBundleExecutable ($exe) is on disk"
	else
		fail "CFBundleExecutable is '$exe' but no such binary exists -- codesign will fail after the tag"
	fi
	[ "$ident" = "com.stoatworks.ffgl.photofinish" ] \
		&& pass "CFBundleIdentifier is $ident" \
		|| fail "CFBundleIdentifier is '$ident', expected com.stoatworks.ffgl.photofinish"
	[ "$version" = "0.1.0" ] \
		&& pass "CFBundleVersion is $version" \
		|| fail "CFBundleVersion is '$version', expected 0.1.0"

	step "codesign"
	tmp=$(mktemp -d)
	cp -R "$BUNDLE" "$tmp/" 2>/dev/null
	if codesign --force --sign - --timestamp=none "$tmp/Photofinish.bundle" >/dev/null 2>&1; then
		pass "ad-hoc signs (the command the release job runs)"
	else
		fail "ad-hoc signing failed"
		codesign --force --sign - --timestamp=none "$tmp/Photofinish.bundle" 2>&1 | sed 's/^/       /'
	fi
	rm -rf "$tmp"

	step "oxbow"
	OXBOW="${OXBOW:-../oxbow/build/oxbow}"
	[ -x "$OXBOW" ] || OXBOW="$HOME/Projects/resolume/oxbow/build/oxbow"
	if [ -x "$OXBOW" ]; then
		out=$("$OXBOW" selftest "$BUNDLE" 2>&1)

		# The identity a host actually sees. The FFGL name field is not
		# null-terminated, so a name over 16 characters is truncated silently
		# and the only place that shows is here.
		grep -q '^name: *SW Photofinish$' <<<"$out" \
			&& pass "name is 'SW Photofinish' (14 of the 16 characters a host reads)" \
			|| fail "name is not 'SW Photofinish': $(grep '^name:' <<<"$out")"
		grep -q '^id: *PF01$' <<<"$out" \
			&& pass "id is PF01" \
			|| fail "id is not PF01: $(grep '^id:' <<<"$out")"
		grep -q '^type: *effect$' <<<"$out" \
			&& pass "type is effect" \
			|| fail "type is not effect: $(grep '^type:' <<<"$out")"

		case "$out" in
			*"FF_INSTANTIATE_GL failed"*) fail "instantiation failed -- see: $OXBOW selftest $BUNDLE" ;;
			*"selftest:    PASS"*) pass "registers, instantiates and renders 120 frames" ;;
			*) fail "oxbow did not report PASS -- see: $OXBOW selftest $BUNDLE" ;;
		esac

		# Worth knowing rather than asserting: oxbow renders its 120 frames as
		# fast as it can and does not drive SetTime, so the plugin falls back
		# to the wall clock, almost no time passes, and only a handful of
		# columns are written. A low "lit pixels" figure here is the effect
		# being a CLOCK, not the effect being broken.
		grep -E '^(gl|frames|lit pixels|gl error):' <<<"$out" | sed 's/^/   /'
	else
		printf '   skipped: oxbow not built at %s\n' "$OXBOW"
	fi
fi

step "bench (for the record, not pass/fail)"
"$PFTEST" --bench --frames 60 2>&1 | sed -n '3,7p' | sed 's/^/   /'

printf '\n'
if [ "$failures" -eq 0 ]; then
	printf '\033[32mall checks passed\033[0m\n'
else
	printf '\033[31m%d check(s) failed\033[0m\n' "$failures"
fi
exit $(( failures > 0 ? 1 : 0 ))
