# Composer 2 — Edit Window UI Spec

Nov 13, 2026 · Ken

## Overview

Build the Composer 2 Edit window: a Pro Tools–style track layout where each track lane is a notation staff, not an audio waveform. The whole UI should feel like a hardware console, with anodized panels, raised keycaps with LEDs, backlit LCD readouts, knobs, LED meters and an 88-key keyboard deck.

- **Visual reference:** `mockups/composer-view.html` and `mockups/producer-view.html` in this package (open in a browser). They are the source of truth for look and placement. This spec is the source of truth for sizes, tokens and behavior.
- **Desktop only:** target window 1680 × 1240 px, minimum 1280 × 800. It is dense pro-tool UI, so controls are smaller than 44 px touch targets on purpose.
- **Music in the mockup is placeholder:** an 8-bar D major cue (bars 41–48, Verse 2 into Chorus 2) on six tracks. Render real project data in the build.
- **Session name** shown in the edit window title: "Sunset at the Border 2026".

## Design tokens

Define everything below as CSS custom properties (or the framework's theme object). LED, LCD and staff colors must be themeable at runtime, because the user can switch them. Ready-made files: `tokens/tokens.css` and `tokens/tokens.json`.

### Themeable colors

| Token | Default | Options / notes |
| --- | --- | --- |
| `--led` | #FFB547 amber | #FF5A4E red, #4FD1FF cyan, #9BF07A green. Lit buttons, selection, playhead accents, lit keys |
| `--ledglow` | `--led` at 60% alpha | Used in glow box-shadows |
| `--lcd` | #6EE7D2 cyan | #9BF07A green, #FFC266 amber, #9CC2FF blue. All LCD text, playhead line |
| `--glow` | `--lcd` at 55% alpha | LCD text-shadow `0 0 6px` |
| `--paper` | #14171B (Dark) / #F1ECE1 (Paper) | Staff lane background |
| `--ink` | #E7EBF0 (Dark) / #1B1A18 (Paper) | Note heads, stems, clefs, dynamics |
| `--staff` | #525B66 (Dark) / #8C8579 (Paper) | Staff lines and barlines |
| `--grid` | white 4% (Dark) / black 7% (Paper) | Beat and bar grid lines in lanes |

### Fixed colors

| Use | Value |
| --- | --- |
| Page / chassis | #0C0D0F |
| Panel fill | linear-gradient #23272C → #1A1D21 |
| Panel edge | 1px #08090A, inset top highlight white 7% |
| Silkscreen label text | #8E98A3 |
| Primary text | #D3D9E0; bright #E7EBF0 |
| LCD background | #06100D |
| Unlit LED / meter segment | #2B2E32 / #24282D |
| Meter green / amber / red | #3DDC84 / #FFC24A / #FF5A4E |
| Marker flag | #F5D547 |
| Record arm / Solo / Mute | #FF5A4E / #FFC24A / #4FD1FF |

### Track colors

| Track | Color |
| --- | --- |
| Flute 1 | #4FA8E8 |
| Violins I | #E0699A |
| Piano | #A78BFA |
| Cellos | #2EC4B6 |
| Contrabass | #8FC85A |
| Djembe | #E46F4B |
| Udu (live audio) | #D98C5F |

### Type

| Role | Font | Size / weight |
| --- | --- | --- |
| Silkscreen labels | Barlow Condensed | 10–11 px, 600, uppercase, letter-spacing 0.14em |
| Keycap legends | Barlow Condensed | 11–12 px, 600, uppercase, letter-spacing 0.06em |
| Brand "COMPOSER" | Barlow Condensed | 24 px, 700, letter-spacing 0.22em; the "2" is 28 px in `--led` with glow |
| LCD readouts | Share Tech Mono | 10–36 px (main counter 36 px) |
| UI body | Inter | 12–13 px, 400–600 |
| Notation glyphs | Noto Music in the mockup; Bravura (SMuFL) in the build | Staff space 7 px, glyph font-size 28 px |
| Dynamics | Georgia italic bold | 15 px |

All four web fonts are free on Google Fonts. For production engraving, swap Noto Music for a SMuFL font such as Bravura so the clef and accidental anchor points are exact.

### Shapes and depth

- **Panel:** radius 8 px, shadow `0 2px 10px rgba(0,0,0,.55)`.
- **Keycap:** radius 5 px, gradient #3C4148 → #2A2E34, inset top highlight white 15%, a 2 px solid #08090A drop below to read as raised.
- **LCD:** radius 4 px, inset shadow `0 2px 8px rgba(0,0,0,.95)`, bottom bevel highlight white 8%.
- **Spacing:** 10 px between panels, 14 px page padding, 5–7 px between keys in a group.

## Layout

The window is five horizontal bands stacked top to bottom. The middle band splits into three columns. Sizes are at the 1680 px reference width.

| # | Region | Size | Contents (left to right) |
| --- | --- | --- | --- |
| 1 | Top deck | 96 px tall, panels wrap | Brand nameplate (268 px, 4 corner screws, Composer \| Producer switch) · Transport (7 keys, 46 × 40 px) · Counter (main LCD 250 px + start/end/length LCD 150 px) · Session (Tempo, Meter, Key LCDs + Click, Count-in, Wait note) · Master meters (flexible width, min 200 px) |
| 2 | Toolbar | ~58 px | Entry mode · View · Tools · Note value keypad · Accidental · Grid · Zoom, each group with a silkscreen label above and a groove divider between |
| 3a | Left rack | 184 px wide | Tracks list (show/hide eye per track) · Groups (LED per group) · Markers (click to locate) |
| 3b | Edit window | Fills the rest (min 640 px) | Title bar with session name, Stage bar, Link sel / Follow / Show velocity · Overview strip (30 px) · Ruler (88 px) · track rows. Scrolls horizontally inside its own box |
| 3c | Inspector | 268 px wide | Selection LCD · Articulation (3 × 2) · Dynamics (4 × 2) · Performance ring knobs · Voice 1–4 · Transpose |
| 4 | Keyboard deck | 140–236 px (grows with key zoom) | Header with key zoom controls · Pitch and Mod wheels + Octave · 88-key keyboard in a horizontal scroller · Step input LCD |
| 5 | Status strip | 16 px | Sample rate, CPU, disk, key/meter, concert pitch, autosave time in Share Tech Mono 11 px |

### Edit window internals

- **Track row** = header (220 px wide) + lane. Row height is 108 px for a single staff and 184 px for a grand staff.
- **Lane width** = 46 px clef zone + bars × bar width. Bar width is 120 px at 100% horizontal zoom.
- **Ruler** = four 22 px rows: Bars|Beats, Markers, Chords, Tempo. A 220 px label column sits over the track headers.
- **Track headers stay pinned** when the lanes scroll horizontally (sticky left). The ruler scrolls with the lanes.

### Narrow windows

Under about 1280 px the top-deck panels wrap to a second line, and the inspector drops below the edit window. The edit window and keyboard always keep their own horizontal scroll rather than shrinking content.

## Components

Build these as reusable components first. Every screen region is assembled from them.

| Component | Size | Default state | Active / lit state | Notes |
| --- | --- | --- | --- | --- |
| Keycap | 22–28 px tall; transport 46 × 40 | Gradient fill, 2 px dark drop below | Lighter fill, 1 px inset ring in `--led`, glow, white legend, drop shrinks to 1 px (pressed in) | Optional LED dot left of the legend. Use a real `<button>` with `aria-pressed` |
| LED | 6 px circle | #2B2E32 with inset shadow | Fill color + `0 0 6px` glow | Default color `--led`; MIDI-in uses green |
| LCD | Any | #06100D, inset shadow, `--lcd` text with glow | — | Secondary LCD labels at 65% opacity |
| Small knob | 26–30 px | Radial gradient cap, white pointer, collar ring | Rotation −135° to +135° | Silk label above, LCD value below. Drag vertically to change; double-click resets |
| Ring knob | 84 px (50 px cap) | 15 LEDs on a 270° arc | LEDs lit up to the value in `--led` | Used for Velocity and Humanize. Knurled cap with an `--led` pointer |
| Vertical meter | 6 px wide, 12 segments (20 on grand staff) | Unlit segments | Green to n−4, amber to n−1, red top | One per track header, right edge |
| Horizontal meter | 30 segments, 7 px tall | Unlit | Green to 22, amber to 28, red 29–30 | Master L and R |
| Wheel | 28 × 84 px slot | Ribbed wheel in a recessed slot | Pitch springs back to center; Mod stays put | Pitch bend and Modulation |
| Screw | 9 px | Radial gradient head with a slot at a random angle | — | Decorative. Nameplate and keyboard deck only |
| Fader (Producer) | 6 px slot, 120–150 px throw | Metal cap 28 × 16 px with a center line | — | dB scale +6 … −∞ on the left |

### Track header (220 px)

1. A 5 px color stripe on the left edge in the track color, with a soft glow.
2. Row 1: track number LCD (20 px) and a scribble-strip LCD with the name in caps (13 px) and the instrument patch (10 px).
3. Row 2: R / S / M keys (24 × 22 px). When on, they fill solid red, amber or blue with dark text. Below them sits a staff tag (Treble, Bass, Perc, Grand staff, Split C4).
4. Volume and Pan small knobs on the right of row 2.
5. The vertical meter in a 14 px dark well at the far right.
6. The grand staff header adds Ped and 8va keys.

## Score lanes

Each lane is one SVG (or canvas) layer drawn in this order: grid, phrase blocks, selection, staff, notes, dynamics/slurs/hairpins, playhead. Notes render as engraved notation, not piano-roll bars.

### Staff geometry

| Value | Single staff | Grand staff |
| --- | --- | --- |
| Lane height | 108 px | 184 px |
| Staff line spacing | 7 px | 7 px |
| Top line y | 40 | Treble 40, Bass 112 |
| Clef zone | 46 px, sticky on horizontal scroll | 46 px + brace at x = 1.5 |
| Bar width at 100% zoom | 120 px | 120 px |
| Note x | barStart + 16 + beat × (barWidth − 28) / 4 | same |

A pitch's y position is `bottomLineY − (diatonicIndex − bottomLineIndex) × 3.5`, where diatonicIndex = octave × 7 + letter (C = 0 … B = 6). The bottom line is E4 for treble and G2 for bass.

### Engraving rules

- **Note heads:** ellipse 4.4 × 3.1 px rotated −20°. Filled for quarters and shorter; outlined (1.6 px) for halves and wholes.
- **Stems:** 24 px, 1.3 px stroke. Up on the right of the head when the note sits below the middle line, down on the left otherwise. Chords use the average pitch.
- **Beams:** 3.6 px thick, flat, joining eighths in beat pairs. A lone eighth gets a flag.
- **Ledger lines:** 15 px wide, one per two steps outside the staff.
- **Dots, rests, accidentals:** dots 8 px right of the head. Rests and sharps use music-font glyphs.
- **Key signature:** sharps sit at x = 27 and 34 in the clef zone.
- **Percussion staff:** a double-bar clef with 5 lines. Bass = F4, Tone = C5, Slap = E5 with an × head. All stems up.
- **Dynamics:** italic bold 15 px, 24 px below the bottom line. Hairpins sit 19 px below it. Slurs are filled crescents above the highest note.

### Phrase blocks (the "clips")

- Rounded rect (radius 5) in the track color at 9% fill and 55% stroke, inset 2 px from bar lines.
- A solid 14 px name strip runs across the top with dark uppercase Barlow Condensed text, for example "FLUTE · CHORUS MELODY".
- Blocks can be dragged, trimmed at either edge (snaps to Grid), split, duplicated and renamed.

### Selection and playhead

- **Selection:** a dashed `--led` rectangle at 12% fill over the selected range. Selected notes, stems and beams turn `--led`.
- **Playhead:** a 1.6 px `--lcd` line through the ruler and every lane, with a 12 px triangle cap in the Bars row.

### Ruler rows

| Row | Shows |
| --- | --- |
| Bars \| Beats | Bar numbers in Share Tech Mono 12 px, beat ticks, an `--led` 3 px loop-range bar along the bottom |
| Markers | Yellow flag tags (#F5D547) with dark caps text. A gray flag marks tempo directives such as "RIT." |
| Chords | Chord-symbol pills per bar (Inter 600, 11.5 px). Click to edit |
| Tempo | Tempo line in `--lcd` with ♩ = 96 at the start and ramps drawn as slopes (96 → 88) |

The **overview strip** above the ruler shows the whole song: one 3 px row per track with its phrase blocks, plus an `--lcd` viewport box for the visible bars. Drag the box to scroll.

## 88-key keyboard and key zoom

The keyboard deck shows a full 88-key piano, A0 to C8. A key-zoom slider makes the keys bigger; when they no longer fit, the keyboard scrolls sideways.

### Keys

| Item | Spec |
| --- | --- |
| Range | A0–C8: 52 white keys, 36 black keys |
| White key width | 22 px × zoom (1× = 1144 px total, fits the deck at 1680 px) |
| Keyboard height | 96 px + (zoom − 1) × 48 px, so 96 px at 1× and 192 px at 3× |
| Black key | 60% of a white key's width, 62% of the height, centered on the line between two white keys |
| White key fill | Gradient #F5F4F0 → #E3E2DC → #CFCEC8, inset 4 px bottom shadow |
| Black key fill | Gradient #2E2E2E → #0D0D0D |
| Lit key | Solid `--led` with glow. Lit keys show the notes in the current selection, or the notes sounding during playback and step input |
| Labels | C1…C8 printed at the bottom of each C key (Share Tech Mono 9 px). Middle C (C4) also gets a small `--lcd` dot |
| Frame | Recessed #08090A well, inset shadow, 4 px top lip |

Position black keys from the white-key index so they scale exactly with zoom. For a black key after white key i (0-based, of 52): `left = (i + 1) / 52 − 0.3 / 52` and `width = 0.6 / 52`, both as fractions of the keyboard's width.

### Zoom controls (deck header, right side)

| Control | Behavior |
| --- | --- |
| Key zoom slider | Range 1.00×–3.00×, step 0.05. Updates live while dragging |
| − / + keys | Step ±0.25×, clamped to 1–3 |
| Zoom LCD | Shows the value, for example "1.75×" |
| 88 · Fit | Back to 1×, all 88 keys visible |
| Zoom to selection | Sets about 2.5× and centers the selected range |
| Ctrl/Cmd + mouse wheel over the keyboard | Zooms around the pointer |
| Mouse wheel / trackpad | Scrolls sideways when zoomed |

### Behavior rules

1. **Keep an anchor centered when zooming.** The anchor is the selection's middle note, else the sounding notes, else middle C. Set `scrollLeft = (anchorWhiteIndex + 0.5) × keyWidth − viewportWidth / 2`.
2. **Auto-follow:** when Follow is on and a lit note is off-screen, scroll it into view with a short ease (150 ms).
3. **Remember** the zoom level per project.
4. **Playing the keys:** click or touch sounds the note (velocity from the click's vertical position on the key) and, in Step mode, enters it at the step cursor with the current note value. Shift-click adds to a chord.
5. **Octave ±** shifts what computer-keyboard input plays. It does not move the on-screen keyboard. Its LCD shows the offset (±0).
6. The deck grows taller with zoom. The edit window above shrinks to make room; it never overlaps the edit window.

## Interaction and state

One app-wide store drives every panel. A change anywhere (a click on a staff, a key on the keyboard, a knob) updates the selection, the inspector, the keyboard LEDs and the counters together.

### Global state

- `workspace`: Composer or Producer.
- `stage`: Compose, Score, Mix or Master.
- `transport`: playing, recording, looping, position (bars|beats|ticks at 960 ticks per quarter note), loop range, pre/post-roll.
- `entryMode`: Insert, Overwrite, Step or Real-time. Exactly one is lit.
- `view`: Track, Page or Mixer. This spec covers Track.
- `tool`: Select, Pencil, Eraser, Slur, Lyrics or Hand (Composer); Zoomer, Trim, Selector, Grabber, Scissors, Pencil, Smart (Producer).
- `noteValue` (whole through 32nd, plus dotted and triplet flags), `accidental`, `voice` (1–4), `grid` (default 1/16).
- `selection`: track ids, bar/beat range, note ids.
- `theme`: `--led`, `--lcd`, staff paper (Dark or Paper).
- `keyZoom` (1–3) and `editZoom` (horizontal bar width).

### Keyboard shortcuts

| Key | Action |
| --- | --- |
| Space | Play / stop |
| R | Record |
| L | Loop on/off |
| Enter | Return to zero (in Step mode: commit the step) |
| 7 6 5 4 3 2 | Note value: whole, half, quarter, eighth, 16th, 32nd |
| . | Toggle dotted |
| / | Toggle triplet |
| 0 | Rest |
| A–G | Enter that pitch at the step cursor |
| Up / Down | Raise / lower the selected notes a step; with Shift, an octave |
| − / = | Flat / sharp |
| Cmd/Ctrl + 1–4 | Voice 1–4 |
| Cmd/Ctrl + − / = | Edit window zoom out / in |
| Cmd/Ctrl + Tab | Toggle Composer / Producer view |

The number legends printed on the note-value keycaps (7–2, ., /, 0) must match this table.

### Panel behavior

- **Inspector Selection LCD** updates live: track, range, note count and pitch span, value, velocity range.
- **Articulation and Dynamics keys** apply to the selection and light when every selected note has that mark.
- **Velocity and Humanize ring knobs:** drag vertically, Shift for fine. The LED ring fills to match.
- **R / S / M keys:** Solo dims all non-soloed lanes to 50%. Mute grays the lane's notes.
- **Markers list and ruler flags** locate the playhead on click. Double-click renames.
- **Overview strip:** click to jump, drag the viewport box to scroll.
- **Meters** fall at about 20 dB per second with a 1.5 s peak hold.

## Producer view (DAW)

Composer 2 has two workspaces on the same project: **Composer view** (score staves, everything above) and **Producer view**, a full traditional DAW. One switch flips between them. The tracks, regions, transport, markers and selection are the same data in both, so nothing is converted or lost when switching. Mockup: `mockups/producer-view.html`.

### The view switch

- A two-position Composer | Producer switch on the brand nameplate (top left). The active side is lit with an LED; the session name sits in the edit window title.
- Shortcut: Cmd/Ctrl + Tab toggles views. The switch keeps the playhead position, selection, zoom and scroll position.
- A **Stage** bar in the edit window title shows the workflow: Compose › Score › Mix › Master. The current stage is lit, finished stages show a green LED. Clicking a stage opens its tools: Compose = Composer view, Score = page view and parts export (PDF/MusicXML), Mix = Producer view with the mixer, Master = the Master panel and export.

### What changes in Producer view

| Area | Composer view | Producer view |
| --- | --- | --- |
| Track lanes | Notation staves | MIDI regions with mini notes, audio waveforms, automation lanes |
| Expanded track | — | Full piano roll with vertical keyboard and a velocity lane |
| Toolbar | Entry mode, note values, accidentals | Edit mode (Shuffle, Slip, Spot, Grid), Zoomer, Trim, Selector, Grabber, Scissors, Pencil, Smart tool, Grid/Nudge, Quantize, Punch, Loop rec, New track |
| Left rack | Tracks, Groups, Markers | New track panel, Tracks with type tags, Clips list |
| Right panel | Note inspector | Channel strip, Print score to audio, Master |
| Bottom deck | 88-key keyboard | Mixer (tab to switch back to Keyboard) |

### Track types

| Type | Tag color | Lane shows | Typical use |
| --- | --- | --- | --- |
| Instrument | #C9A2FF | MIDI regions, played through a virtual instrument on the track | Orchestral parts from the score |
| MIDI | #4FD1FF | MIDI regions, sent to external hardware or another plug-in | Drum module, synth rack |
| Audio | #3DDC84 | Waveform regions (WAV, mono or stereo) | Printed score tracks, live recordings |
| Aux / Bus | #F5D547 | No regions; automation only | Submixes, reverb returns |

The **New track** panel has four keys (MIDI, Instrument, Audio, Aux/Bus), a count field, a Mono/Stereo key and Create. Every score track from Composer view appears here as an Instrument track by default.

### Producer track header (220 px)

- Expand/collapse chevron (`aria-expanded`), type tag, number and name LCD.
- R / S / M plus **I** (input monitor, green) on Audio, Instrument and MIDI tracks; volume LCD on the right.
- Expanded only: IN › OUT routing LCD, insert slots with LEDs, Print to audio and Freeze keys, Piano roll | Score toggle.
- Collapsed height 72 px; audio 84 px; aux 60 px; expanded instrument 272 px. Drag the bottom edge to resize any track.

### Expanded piano roll

- 9 px per semitone; visible range follows the track's notes (the mockup shows E4–C6). Black-key rows shaded, C rows marked.
- Vertical keyboard in the 46 px gutter, C labels. Notes are rounded bars in the track color; selected notes are `--led`.
- Velocity lane below (about 48 px): one stalk per note, draggable. Selected velocities in `--led`.
- Notes edited here update the staves in Composer view, and the reverse.

### Print score to audio

This is how the composed score becomes WAV files. It records each Instrument or MIDI track's output onto a new Audio track.

1. Tick source tracks (checkboxes) in the Print panel, or press Print to audio in a track header.
2. Choose Range (Selection or Song), Output (Per track, Stems, or one Mix), Mode (Real-time or Offline), and format (default WAV, 48 kHz, 24-bit).
3. Options: Include FX (print with inserts) and Add to session (create the Audio tracks automatically).
4. Press **Record score to audio** (large red key). New Audio tracks named "<Track>_PRINT_01.wav" appear under each source with a red recording region that grows to the playhead.
5. A progress LCD shows the current track, count (1 / 3) and position. When done, the source instrument tracks can be hidden or made inactive to save CPU.

### Mix and master

- **Channel strip** (right panel): five insert slots (A–E) with bypass LEDs, sends (knobs), pan, a long-throw fader with dB scale, stereo meter, automation mode (Read, Touch, Latch) and output.
- **Mixer deck** (bottom): one strip per track (insert LEDs, pan, 120 px fader, stereo meter, dB LCD, S/M, colored name plate) plus a wider Master strip outlined in `--led`. Scrolls sideways when tracks don't fit.
- **Automation:** volume, pan, sends and plug-in parameters as breakpoint lines over the lane (aux track in the mockup shows volume automation in #F5D547).
- **Master panel:** integrated loudness (LUFS) and true peak LCDs, master chain slots (EQ, glue comp, limiter), a loudness target key (−14 LUFS default) and Export master (WAV / MP3 / FLAC, with dither on 16-bit).

### Standard DAW features the build must include

- Record audio and MIDI with punch in/out, loop recording with takes, pre-roll and count-in.
- Non-destructive region editing: trim, split, fade/crossfade, gain, nudge, duplicate, consolidate, comping.
- MIDI: quantize (input and after), transpose, velocity scaling, MIDI CC lanes.
- Plug-in hosting (VST3 / AU), sidechain, buses and sends, track freeze, delay compensation.
- Import audio (drag in WAV/AIFF/MP3), tempo and time-signature maps shared with the score, markers, undo history, bounce/export.

## Build notes and acceptance checklist

Build in this order: tokens and the base components, then the static layout, then score rendering, then the keyboard deck, then interactions, then Producer view. Compare each step side by side with the mockups before moving on.

### Build notes

- **Render lanes as SVG** (or a canvas layer for very long scores). Only draw bars in or near the viewport; long scores have hundreds of bars.
- **Use real buttons and labels.** Every keycap is a `<button>` with `aria-pressed` for toggles and an `aria-label` on icon-only keys. The zoom slider is a labeled `<input type="range">`.
- **Hardware feel comes from shading, not images.** Gradients, inset shadows and glows only; no bitmap textures except an optional brushed-metal pattern on the nameplate.
- **Engraving font:** use Bravura (SMuFL) in the build, and place glyphs by SMuFL anchors.
- **Keep the four theme variables** (`--led`, `--lcd`, staff paper, plus derived glows) switchable at runtime from Preferences.
- **One data model for both views.** Notes, regions and tracks are stored once; Composer view renders them as notation, Producer view as piano roll / regions.

### Acceptance checklist

- [ ] At 1680 × 1240 the layout matches the mockup region by region, within 4 px.
- [ ] All colors, fonts and radii come from the tokens; no hard-coded one-offs.
- [ ] Keycaps show a raised state and a lit/pressed state with LED glow.
- [ ] Six track types render: treble, bass, grand staff, and percussion with × heads.
- [ ] Beams, ledger lines, dots, rests, slurs, hairpins and dynamics place correctly on both clefs.
- [ ] Selecting notes highlights them in `--led` and updates the inspector and the lit keys.
- [ ] The playhead moves smoothly through the ruler and every lane during playback.
- [ ] Track headers stay pinned during horizontal scroll; the clef zone stays visible.
- [ ] The keyboard shows 88 keys (A0–C8) at 1× without scrolling at 1680 px.
- [ ] Key zoom from 1× to 3× works by slider, −/+ keys and Ctrl/Cmd + wheel, and keeps the anchor note centered.
- [ ] 88 · Fit and Zoom to selection behave as specified.
- [ ] Switching LED color, LCD color and Dark/Paper staff updates the whole UI with no reload.
- [ ] All shortcuts in the table work, and the keycap legends match.
- [ ] Under 1280 px wide the panels wrap and nothing overlaps.

### Producer view checklist

- [ ] The Composer | Producer switch flips views in under 100 ms and keeps playhead, selection, zoom and scroll.
- [ ] Notes edited in the piano roll show up on the staves, and the reverse.
- [ ] New track creates MIDI, Instrument, Audio and Aux/Bus tracks, mono or stereo, in any count.
- [ ] Any track expands and collapses; the expanded instrument track shows piano roll plus velocity lane.
- [ ] Record score to audio prints per track, as stems or as one mix, in real time or offline, to 48 kHz / 24-bit WAV on new Audio tracks.
- [ ] Mixer strips, channel strip and fader values stay in sync; automation reads and writes in Read, Touch and Latch.
- [ ] Master panel shows live LUFS and true peak, and Export master writes WAV, MP3 and FLAC.
- [ ] Stage bar reflects progress through Compose, Score, Mix and Master.
