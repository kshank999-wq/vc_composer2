# UI spec: what is built and what is not

`docs/spec/ui/` holds the Edit window UI spec: `SPEC.md`, the two mockups,
the design tokens and the generators. This note compares it with the app
as built (November 2026) and proposes a build order. The spec is the
source of truth for sizes, tokens and behaviour. The mockups are the
source of truth for look and placement.

## Already there underneath

Most of what the spec asks the UI to show already exists in the engine
and the document model. The work is mostly presentation.

| Spec area | Built today |
|---|---|
| One data model for both views | Yes. Composer and Sequencer read the same `ProjectData`. A piano-roll edit is written into the score, or kept as a performance override. |
| Transport, record, punch, loop takes, count-in | Yes (transport bar, recording, take lanes). |
| Instrument, MIDI, Audio, Aux tracks | `TrackKind` has Instrument, MidiOnly, Audio and Aux. Buses (group, FX, master) are separate. |
| Piano roll with velocity lane | Yes, as a bottom pane under the Sequencer. The spec puts it inline, in an expanded track. |
| Regions: trim, split, fade, gain, takes | Yes for audio clips. MIDI regions can be moved and notated, but have no trim or split yet. |
| Plug-in hosting, inserts, sends, PDC, automation | Yes: Read, Touch, Latch and Write, with lanes for every control. |
| Mixer with strips and master | Yes: the hardware-style console. |
| Print score to audio | Partly. Stem and mix bounces render offline to WAV. Nothing yet records them onto new Audio tracks in the session. |
| Parts and score export (Score stage) | Yes: PDF parts, full score PDF, MusicXML. |
| Markers, tempo map, meter map | Yes in the model. Chord symbols per bar are not stored yet. |

## Missing

### Look and shell (both views)

- Tokens as a runtime theme:
  - switchable `--led` and `--lcd` colours;
  - Dark or Paper staves;
  - the chassis, panel, keycap, LCD and LED recipes.
- Fonts: Barlow Condensed, Share Tech Mono and Inter (all SIL OFL) need
  to be embedded. Bravura is already embedded.
- Top deck:
  - nameplate with the Composer | Producer switch;
  - transport keys;
  - main counter LCD and start/end/length LCD;
  - Tempo, Meter and Key LCDs with Click, Count-in and Wait note;
  - master meters.
- The grouped toolbar with silkscreen labels.
- A four-row ruler (Bars|Beats, Markers, Chords, Tempo) and the overview
  strip with its draggable viewport.
- Stage bar: Compose › Score › Mix › Master.
- Status strip.

### Composer view

- Left rack: tracks with show/hide eyes, groups with LEDs, markers.
- Track header to spec:
  - number LCD and scribble strip;
  - R / S / M;
  - staff tag;
  - Volume and Pan knobs;
  - meter;
  - Ped and 8va on a grand staff.
- Phrase blocks: named, coloured ranges on a staff that can be dragged,
  trimmed, split, duplicated and renamed. The model has no such object
  yet; it needs one. Proposal: named, track-scoped ranges over the score,
  so moving a block moves its notes.
- Inspector:
  - selection LCD;
  - articulation and dynamics keys;
  - Velocity and Humanize ring knobs (these map onto performance
    overrides);
  - voice;
  - transpose.
- The 88-key keyboard deck:
  - key zoom from 1× to 3×, keeping the anchor note centred;
  - lit keys for the selection;
  - pitch and mod wheels;
  - octave shift;
  - step-input LCD.
- Engraving per the spec's geometry: 7 px staff space, 120 px per bar at
  100%, sticky clef zone and headers. The current Composer view uses the
  ported engraver's own layout.

### Producer view

- Lanes that expand and collapse per track:
  - mini notes on MIDI regions, waveforms on audio regions, automation;
  - the piano roll inline in an expanded track, instead of a bottom pane;
  - track heights resizable by dragging.
- The New track panel: type, count, mono or stereo.
- A Clips list in the left rack, with Import.
- Right panel: channel strip, the Print score to audio panel and the
  Master panel.
- Print to new Audio tracks:
  - "<Track>_PRINT_01.wav", with the region growing while it records;
  - real-time as well as offline;
  - progress LCD;
  - hiding or deactivating the source tracks afterwards.
- Master panel:
  - integrated LUFS and true-peak meters;
  - master chain slots;
  - loudness target;
  - Export master to WAV, MP3 and FLAC, with dither on 16-bit.
- A Mixer deck at the bottom, with a tab back to the keyboard. The
  console is a separate window today.
- Producer toolbar:
  - Shuffle, Slip, Spot and Grid edit modes;
  - the Smart tool;
  - nudge;
  - quantize, input quantize and merge.
- MIDI CC lanes, consolidate, comping across MIDI takes.

## Where the spec and the app disagree (needs a decision)

1. **Cmd+Tab toggles views.** On macOS, Cmd+Tab is the system app
   switcher and never reaches the app. Proposal: Ctrl+Tab on both
   platforms, and Cmd+` or a Cmd+1/2-style chord on macOS. Cmd+1–4 are
   taken by voices in the spec.
2. **Note-value keys.** The spec has 7 for whole, 6 half, 5 quarter,
   4 eighth, 3 16th and 2 32nd, with `.` for dotted, `/` for triplet and
   0 for rest. The app's default profile has 1 for whole up to 7 for a
   64th. Proposal: the spec's layout becomes the default profile, and the
   current one stays as a selectable profile.
3. **Ticks in the counter.** The spec counts 960 per quarter. The engine
   uses 2880. Display at 960 (an exact third) and keep 2880 inside.
   Nested tuplets show the nearest tick.
4. **MP3 export.** This needs an encoder. LAME is LGPL; it would ship
   like FFmpeg, as a separate library, and is recorded as a licence gate
   in `01-decisions-and-license-gates.md`. FLAC and WAV need nothing new.
5. **Producer edit modes (Shuffle, Slip, Spot, Grid)** are
   Pro Tools-specific behaviour. Implementing behaviour is fine; the look
   must stay our own (the spec says the same of the Avid screenshot).
6. **Workspaces.** The spec has two views; the app has Composer and
   Sequencer plus a separate console window. Sequencer becomes Producer
   view, and the console moves into the Mixer deck and channel strip.
   It can stay available as a full window too.

## Proposed build order

This follows the spec's own order. Each step is compared side by side
with the mockups at 1680 × 1240, using the same headless screenshots as
the rest of the app.

1. Theme tokens and base components: panel, keycap, LED, LCD, small knob,
   ring knob, meters, fader, wheel. Embed the fonts.
2. Shell layout: top deck, toolbar, left rack, edit window frame,
   inspector, keyboard deck, status strip. Include the Composer | Producer
   switch and the stage bar, wrapping under 1280 px.
3. Composer view lanes:
   - spec geometry;
   - phrase blocks (with their data model);
   - four-row ruler and overview;
   - sticky headers and clef zone;
   - selection in `--led`.
4. Keyboard deck: 88 keys, zoom, lit keys, step input.
5. Interactions: one store for selection and inspector, and the shortcut
   table.
6. Producer view:
   - track types and the New track panel;
   - expandable lanes with the inline piano roll;
   - waveforms and automation;
   - channel strip;
   - Mixer deck.
7. Print score to audio onto new Audio tracks (offline first, then real
   time).
8. Master panel: LUFS and true peak, master chain, Export master (WAV and
   FLAC first; MP3 after the licence decision).
