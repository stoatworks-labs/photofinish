"""The demo's shaders must be the plugin's shaders, character for character.

    python3 demo/tools/check_shaders.py

Called from `tools/verify.sh`. Exit code 1 means the two copies have drifted.

------------------------------------------------------------------- why

`demo/plugin.js` holds a second copy of every GLSL string in
`source/Shaders.cpp`, and two copies drift -- quietly, because a strip camera
that renders a *plausible* strip looks exactly like one that renders the right
one. The page's whole claim is that it runs the plugin's own passes rather than
something reimplemented to look similar, so the claim needs something enforcing
it. Nothing else does: `pftest` drives the real plugin class and has never
heard of this page, and verify.sh's glslc step compiles the C++ copies and
never looks at the JS one.

------------------------------------------------------------------- what it does

Pulls each `R"( ... )"` body out of the C++ and each matching backtick literal
out of `plugin.js`, and compares them exactly -- no whitespace normalisation, no
comment stripping. A comment updated on one side and not the other is exactly
the drift worth catching, because the comments in these shaders carry the
reasoning (why a one-tap slit is a fetch and not an average of one, why the
ring index is integer `%` and never a float `mod`).

The one transformation is a decode, not a normalisation. Several comments quote
an identifier in backticks, and a backtick cannot appear raw inside a
JavaScript template literal, so `plugin.js` escapes it as \\`. This unescapes
that and *rejects any other backslash on the JS side*; there is none in the C++
shader text, so a second escape could only be somebody hiding a difference.

------------------------------------------------------------------- what it cannot

Nothing here checks the *ported* half: `Strip.cpp`'s clock, `Controls.cpp`,
`ColumnPeriod()`, the sorted option lists and the ring bookkeeping in plugin.js
are a hand translation, and only a reader can tell whether they still agree.
Change one of those and change it here too.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))

# JS constant, C++ file, C++ symbol.
SHADERS = [
    ("VERTEX", "source/Shaders.cpp", "kVertexShader"),
    ("COPY", "source/Shaders.cpp", "kCopyShader"),
    ("SLIT", "source/Shaders.cpp", "kSlitShader"),
    ("STRIP", "source/Shaders.cpp", "kStripShader"),
]

def from_cpp(source, symbol):
    match = re.search(r'(?:static )?const char\* const ' + symbol + r' = R"\((.*?)\)";', source, re.S)
    return None if match is None else match.group(1)


def from_js(source, name):
    match = re.search(r'^const ' + name + r' = `(.*?)`;$', source, re.S | re.M)
    if match is None:
        return None, None

    body = match.group(1)
    stray = re.search(r"\\(?!`)", body)
    if stray is not None:
        upto = body[: stray.start()]
        return None, f"backslash that is not an escaped backtick, at line {upto.count(chr(10)) + 1}"
    if "${" in body:
        return None, "template substitution inside a shader literal"

    return body.replace("\\`", "`"), None


def main():
    with open(os.path.join(REPO, "demo", "plugin.js")) as handle:
        js = handle.read()
    cpp_cache = {}

    problems = 0
    for name, path, symbol in SHADERS:
        if path not in cpp_cache:
            with open(os.path.join(REPO, path)) as handle:
                cpp_cache[path] = handle.read()
        cpp_text = from_cpp(cpp_cache[path], symbol)
        js_text, complaint = from_js(js, name)

        if cpp_text is None:
            print(f"FAIL  {symbol} not found in {path}")
            problems += 1
            continue
        if complaint is not None:
            print(f"FAIL  {name} in demo/plugin.js has a {complaint}")
            problems += 1
            continue
        if js_text is None:
            print(f"FAIL  {name} not found in demo/plugin.js")
            problems += 1
            continue

        if cpp_text == js_text:
            print(f"ok    {name:<8} matches {symbol} ({len(cpp_text)} chars)")
            continue

        problems += 1
        print(f"FAIL  {name} has drifted from {symbol} in {path}")
        cpp_lines = cpp_text.splitlines()
        js_lines = js_text.splitlines()
        for i in range(max(len(cpp_lines), len(js_lines))):
            a = cpp_lines[i] if i < len(cpp_lines) else "<missing>"
            b = js_lines[i] if i < len(js_lines) else "<missing>"
            if a != b:
                print(f"        first difference at line {i + 1}")
                print(f"          C++: {a}")
                print(f"          js : {b}")
                break

    print()
    if problems:
        print(f"{problems} problem(s) -- copy the C++ across, do not edit plugin.js by hand")
        return 1

    print(f"all {len(SHADERS)} shaders are identical to the plugin's")
    return 0


if __name__ == "__main__":
    sys.exit(main())
