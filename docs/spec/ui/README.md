# Composer 2 — UI package for the coding agent

This package defines the look and behavior of the Composer 2 Edit window. Composer 2 has two workspaces on the same project:

- **Composer view:** a Pro Tools–style track layout where every lane is a notation staff.
- **Producer view:** a full DAW with MIDI, Instrument, Audio and Aux tracks, piano roll, print-score-to-audio, mixer and mastering.

Both views share a hardware-console look: anodized panels, raised keycaps with LEDs, backlit LCDs, knobs and meters.

## Start here

1. Open `mockups/composer-view.html` and `mockups/producer-view.html` in a browser at 1680 px wide or more. The nameplate switch links the two pages.
2. Read `SPEC.md` end to end. It holds the sizes, tokens, rules and acceptance checklists.
3. Import `tokens/tokens.css` (or `tokens/tokens.json`) as the theme before building any component.

## What's in the box

| Path | What it is | How to use it |
| --- | --- | --- |
| `SPEC.md` | The full UI spec | Source of truth for sizes, tokens and behavior |
| `mockups/composer-view.html` | Composer view mockup, standalone | Source of truth for look and placement. The keyboard zoom slider, −/+, Fit and Ctrl/Cmd + wheel work |
| `mockups/producer-view.html` | Producer view mockup, standalone | The Violins I arrow expands and collapses the piano roll |
| `tokens/tokens.css` | CSS custom properties + component recipes | Drop in as the base stylesheet |
| `tokens/tokens.json` | Same tokens as JSON | For a JS theme object or design-token tooling |
| `generators/*.py` | Python scripts that generated the mockups | Exact geometry and notation math (note positions, beaming, ledger lines, piano roll, waveforms). Run `python3 gen_composer.py out.dc.html` |

(Kept out of the repository: the Pro Tools reference screenshot, which is
Avid's, and the design-canvas sources, which need the canvas runtime.)

## Review the theme options in the mockups

Append query parameters to either mockup URL:

- `?led=%23FF5A4E` sets the LED color (red; also try `%234FD1FF` or `%239BF07A`).
- `?lcd=%239BF07A` sets the LCD color (green; also try `%23FFC266` or `%239CC2FF`).
- `?paper=1` sets the cream "paper" staves (Composer view).

## Notes for the build

- The mockups are static HTML/SVG made for visual reference, not production code. Rebuild the UI as components in the app's framework, and drive it from one shared data model, so a note edited in the piano roll also changes on the staff.
- The music is a placeholder 8-bar D major cue (bars 41–48). Render real project data in the build.
- Notation in the mockup uses the free Noto Music font. Use Bravura (SMuFL) in production so clefs and accidentals sit exactly on their anchors.
- The controls are smaller than 44 px touch targets on purpose: this is a dense desktop pro tool.
- Build order and the acceptance checklists are at the end of `SPEC.md`.

## Links

- Live design canvas (both views): https://claude.ai/artifact/DbJq7rEJcNuy8vPSgVBF95
- Live spec doc: https://claude.ai/artifact/RZkzLQ8u6DT3Xd5oKHfvab
