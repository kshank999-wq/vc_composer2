#!/usr/bin/env python3
"""Builds a small SMuFL font, so the glyph path can be exercised at all.

    scripts/make-test-music-font.py <smufl-font-gen binary> <output directory>

Why this exists. Bravura is embedded in every build now
(resources/fonts/Bravura), so the glyph path is exercised by a real font as
well. This one is kept for what Bravura cannot show: that the engine names
every glyph it draws - the generator reads the engine's own table, so a glyph
added there without a codepoint fails here - and that metadata is genuinely
applied, since its advances are chosen to differ from the built-in defaults
where Bravura's, which the defaults were measured from, do not.

This builds a real font instead: an OpenType file with a glyph at every
codepoint `engraving/Smufl.cpp` names, its own SMuFL metadata, and a
`glyphnames.json`. It is generated rather than committed, because a binary
font in a source tree is the thing the licence rules above exist to avoid.

**What it can and cannot show.** It shows that a SMuFL font is found, loaded,
its metadata read and applied, and its glyphs - not the renderer's primitive
stand-ins - drawn for the codepoints the engine asks for. It shows nothing
whatever about how Bravura, Petaluma or Leland *look*: the outlines here are
deliberately plain marks, not music. A composer with a real font installed is
still the only way to see real notation, and that has not been seen.

The glyphs are plain on purpose. A generator drawing its own idea of a treble
clef would be this repository agreeing with itself about a shape nobody
checked, and a wrong one would look convincing. A mark that is obviously not
a clef cannot be mistaken for evidence that clefs render correctly.

Needs fonttools. A missing one exits 2 with a message rather than passing.
"""
import json
import os
import subprocess
import sys

try:
    from fontTools.fontBuilder import FontBuilder
    from fontTools.pens.t2CharStringPen import T2CharStringPen
except ImportError:  # pragma: no cover - reported, not worked around
    print("[ SKIP  ] fonttools is not installed, so no test font can be built",
          file=sys.stderr)
    sys.exit(2)

# A SMuFL font is designed on an em square equal to the staff height, which is
# four staff spaces. So one staff space is a quarter of the em, and every
# measurement below is in font units with that in mind.
UNITS_PER_EM = 1000
STAFF_SPACE = UNITS_PER_EM / 4.0

FAMILY = "VCC Test Music"


def glyph_outline(pen, index, advance_spaces):
    """A plain mark, sized from the glyph's own advance.

    Two rectangles: a full-height bar at the origin, and a shorter one whose
    height steps with the index. Nothing here resembles a musical symbol, and
    that is deliberate - see the module docstring.
    """
    width = advance_spaces * STAFF_SPACE

    # The upright, always present, so every glyph has ink at its origin.
    pen.moveTo((0, -STAFF_SPACE * 0.5))
    pen.lineTo((width * 0.18, -STAFF_SPACE * 0.5))
    pen.lineTo((width * 0.18, STAFF_SPACE * 0.5))
    pen.lineTo((0, STAFF_SPACE * 0.5))
    pen.closePath()

    # A second mark whose height varies, so two glyphs are never identical
    # bitmaps. Kept inside the advance so the box the metadata states is true.
    height = STAFF_SPACE * (0.15 + 0.05 * float(index % 8))
    pen.moveTo((width * 0.3, -height))
    pen.lineTo((width * 0.92, -height))
    pen.lineTo((width * 0.92, height))
    pen.lineTo((width * 0.3, height))
    pen.closePath()


def build(glyphs, out_dir):
    os.makedirs(out_dir, exist_ok=True)

    # Advances deliberately differ from the engine's built-in defaults, so a
    # metadata load that silently does nothing is visible: the table's values
    # after loading must be these, not the ones it was constructed with.
    for index, glyph in enumerate(glyphs):
        glyph["advance_spaces"] = round(glyph["default_advance"] * 1.25 + 0.1, 4)
        glyph["index"] = index

    order = [".notdef"] + [g["name"] for g in glyphs]
    builder = FontBuilder(UNITS_PER_EM, isTTF=False)
    builder.setupGlyphOrder(order)
    builder.setupCharacterMap({g["codepoint"]: g["name"] for g in glyphs})

    charstrings = {}
    metrics = {}

    blank = T2CharStringPen(0, None)
    charstrings[".notdef"] = blank.getCharString()
    metrics[".notdef"] = (0, 0)

    boxes = {}
    for glyph in glyphs:
        width = glyph["advance_spaces"] * STAFF_SPACE
        pen = T2CharStringPen(round(width), None)
        glyph_outline(pen, glyph["index"], glyph["advance_spaces"])
        charstrings[glyph["name"]] = pen.getCharString()
        metrics[glyph["name"]] = (round(width), 0)

        # The real extent of what was just drawn, in staff spaces, with y
        # pointing up as SMuFL states boxes. The loader flips it.
        height = 0.5
        boxes[glyph["name"]] = {
            "bBoxSW": [0.0, -height],
            "bBoxNE": [glyph["advance_spaces"], height],
        }

    builder.setupCFF(FAMILY, {"FullName": FAMILY}, charstrings, {})
    builder.setupHorizontalMetrics(metrics)
    builder.setupHorizontalHeader(ascent=int(STAFF_SPACE * 2), descent=-int(STAFF_SPACE * 2))
    builder.setupNameTable({
        "familyName": FAMILY,
        "styleName": "Regular",
        "fullName": FAMILY,
        "psName": FAMILY.replace(" ", ""),
        "version": "1.0",
    })
    builder.setupOS2(sTypoAscender=int(STAFF_SPACE * 2), usWinAscent=int(STAFF_SPACE * 2),
                     usWinDescent=int(STAFF_SPACE * 2))
    builder.setupPost()

    font_path = os.path.join(out_dir, "VCCTestMusic.otf")
    builder.save(font_path)

    # The font's own SMuFL metadata, named the way the loader looks for it.
    metadata = {
        "fontName": FAMILY,
        "fontVersion": 1.0,
        "glyphAdvanceWidths": {g["name"]: g["advance_spaces"] for g in glyphs},
        "glyphBBoxes": boxes,
    }
    metadata_path = os.path.join(out_dir, "VCCTestMusic_metadata.json")
    with open(metadata_path, "w") as handle:
        json.dump(metadata, handle, indent=1)

    # SMuFL's own name-to-codepoint document, as shipped beside a real font.
    names = {g["name"]: {"codepoint": "U+%04X" % g["codepoint"]} for g in glyphs}
    names_path = os.path.join(out_dir, "glyphnames.json")
    with open(names_path, "w") as handle:
        json.dump(names, handle, indent=1)

    return font_path, metadata_path, names_path


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2

    dumper, out_dir = argv[1], argv[2]
    if not os.path.exists(dumper):
        print("[ FAIL  ] no smufl-font-gen binary at the path given", file=sys.stderr)
        return 1

    done = subprocess.run([dumper], capture_output=True, text=True)
    if done.returncode != 0:
        print("[ FAIL  ] the glyph table could not be read: " + done.stderr.strip(),
              file=sys.stderr)
        return 1

    glyphs = json.loads(done.stdout)
    if not glyphs:
        print("[ FAIL  ] the glyph table came back empty", file=sys.stderr)
        return 1

    font, metadata, names = build(glyphs, out_dir)
    print("built a %d-glyph SMuFL font" % len(glyphs))
    for path in (font, metadata, names):
        print("  " + path)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
