# Bravura

The SMuFL reference music font, by Steinberg Media Technologies GmbH,
embedded into the VC Composer application and the plug-in so that notation
is drawn with a real music font on every machine, with nothing to install.

| File | What it is |
|---|---|
| `Bravura.otf` | The font, unmodified |
| `bravura_metadata.json` | Its SMuFL metadata (`Bravura.json` upstream): advance widths and bounding boxes, which the engraver reads so spacing is the font's rather than the built-in defaults |
| `OFL.txt` | The licence |
| `FONTLOG.txt` | The font's own change log, which the licence asks to travel with it |

Source: https://github.com/steinbergmedia/bravura, `redist/`, at commit
`37b194378b710cc40e406ab6c4b07608bb9548ae` (font version 1.482). To update,
replace the four files from a newer `redist/` and record the commit here.

## Licence

Bravura is licensed under the SIL Open Font License, Version 1.1, with the
Reserved Font Name "Bravura". The full text is in `OFL.txt`. What that
allows and asks, as it applies here:

- The font may be bundled, embedded in software, redistributed and sold
  as part of a larger work, provided the copyright notice and this licence
  travel with it. They do: `OFL.txt` and `FONTLOG.txt` are in this
  directory, and a distributed build of VC Composer has to carry them too
  (`docs/aax/03-signing-and-release.md`).
- The font is not modified here. A modified version could not use the name
  "Bravura", and none is made.
- The font itself is not sold on its own, and is not.

The rest of VC Composer is not under the OFL; only these files are.
