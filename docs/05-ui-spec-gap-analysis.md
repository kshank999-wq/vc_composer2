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
| Print score to audio | Partly. Stem and mix bounces render offline to WAV, and any track can already hold audio clips. Nothing yet prints a track onto its own audio lane. |
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
- Print to audio onto the instrument track's own audio lane (see
  Decisions; this replaces the spec's separate "<Track>_PRINT_01" Audio
  tracks), with the region growing while it records, real time as well
  as offline, and a progress LCD.
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

## Decisions (November 13, 2026)

1. **One app, two views of the same data.** Composer and Producer are
   two tabs (two buttons) at the top of one window, over one project. The
   notation is the source; it renders to MIDI; Producer shows that MIDI.
   Switching keeps the playhead, selection, zoom and scroll. Shortcut:
   Ctrl+Tab on both platforms, plus Cmd+` on macOS, because macOS keeps
   Cmd+Tab for switching apps.
2. **Every instrument track carries its own audio.** In Producer view an
   instrument track has its MIDI lane and, under it in the same track, an
   audio lane. "Print to audio" records the track's instrument output onto
   that lane. It is not a separate Audio track. The track then plays
   MIDI, its printed audio, or both, and is mixed as one channel.
   - The model already allows this: any track version can hold audio
     clips, and the engine plays them.
   - What is new is the choice of source, the print path, and showing
     both lanes.
   - Plain Audio tracks (live recordings) and Aux tracks stay as they are.
3. **Note-value keys follow Finale and Sibelius**, which agree:
   - 1 is a 64th, 2 a 32nd, 3 a 16th, 4 an eighth, 5 a quarter, 6 a half,
     7 a whole and 8 a breve;
   - `.` is a dot and 0 is a rest;
   - this is the spec's own layout (7 whole … 2 32nd), extended to 1
     and 8;
   - the app's current reversed layout becomes a selectable profile.
4. **Ticks.** Editing and every display count 960 per quarter. The engine
   keeps 2880 inside, an exact factor of three. Musical time stays
   separate from the audio sample clock: they meet only through the tempo
   map, and audio stays placed in seconds.
5. **Licensing: free formats only for now.**
   - WAV for prints, bounces and the master.
   - FLAC is free (BSD-licensed and already inside JUCE), so it is
     allowed.
   - MP3 master export is wanted eventually but not now. It needs an
     encoder (LAME, LGPL), to be decided when the time comes.

## Build order (revised)

1. **Theme and base controls.** *Done (November 2026).* `desktop/src/ui/hw/`:
   - `HwTheme` holds the tokens and the runtime theme. LED, LCD and Dark
     or Paper staves are switched from View › Theme, repaint every window
     at once, and are remembered.
   - The three fonts are embedded.
   - `HwControls` has the panel, keycap (toggle, LED dot, glyphs, solid
     R/S/M), LED, LCD, small knob, ring knob (15 LEDs), meters (20 dB/s
     fall, 1.5 s peak hold), fader (+6 … −∞) and the pitch and mod
     wheels.
   - View › Control Gallery lays them out like the mockup's top deck and
     toolbar. The headless check photographs it in two themes and tests
     each control's behaviour.

   The original step 1 description: Tokens with runtime LED, LCD and Dark or
   Paper staff colours; the embedded fonts; and the controls: panel,
   keycap, LED, LCD, small knob, ring knob, meters, fader, wheel.
2. **The window shell with the two tabs.** *Done (November 2026).*
   `desktop/src/ui/shell/` replaces the old transport bar.
   - Top deck:
     - nameplate with the Composer | Producer switch;
     - transport keys;
     - main counter in bars|beats|ticks at 960 per quarter, a min:sec or
       timecode sub-counter, and a start/end/length readout of the ruler
       range;
     - Tempo, Meter and Key LCDs (click to change) and the Click key;
     - master meters on their own peak reader.
   - Toolbar per view.
   - Edit window frame with the session name and the stage bar (Compose,
     Score › export menu, Mix › mixer, Master › stems).
   - Left rack: the track list plus Markers (cues, click to locate) in
     Composer; New track plus the track list in Producer.
   - Right panel: a live Selection readout in Composer; a channel for the
     selected track in Producer (fader, pan, meters, M/S, automation mode,
     inserts, output).
   - Status strip.
   - Ctrl+Tab (and Cmd+` on macOS) switches views.
   - The deck shrinks, then wraps to two rows; under 1280 px the right
     panel drops below the edit window.
   - Not wired yet: Loop, Count-in, Wait note, pre-roll, MIDI tracks to
     hardware and the Page view; they are shown disabled with a tooltip.

   The original step 2 description: Composer | Producer on the
   nameplate.
   - Top deck: transport, counters in 960-tick bars|beats|ticks,
     Tempo/Meter/Key LCDs, master meters.
   - Toolbar frame, left rack, edit window frame, right panel, bottom
     deck, status strip, stage bar.
   - Both tabs show the existing views inside the new shell first, so the
     app works at every step.
   - Switching keeps the playhead, selection, zoom and scroll.
3. **Shared selection and shortcuts.** *Done (November 2026).*
   - **Shared selection.** `Session::selection()` holds the track, a time
     range and score notes, the single selection everything reads and
     writes.
     - The Composer view's note selection, the piano roll's, and a range
       dragged on the ruler all set it. Each view takes it on when it
       changes elsewhere.
     - The top deck's start/end/length counter shows its range.
     - The inspector shows track, range, note count and pitch span, value
       and velocity range.
     - The piano roll's keyboard lights the selected pitches in the LED
       colour.
     - Edits drop deleted notes from it.
     - A plain click on a note selects just that note.
   - **Keyboard profile "Composer 2"** is the default and follows the
     spec's table, numbered as in Finale and Sibelius:
     - 1 is a 64th, 2 a 32nd, 3 a 16th, 4 an eighth, 5 a quarter, 6 a
       half, 7 a whole and 8 a breve;
     - `.` dot, `/` triplet, 0 rest, - flat, = sharp, 9 natural;
     - Shift+Up/Down for an octave, Cmd/Ctrl+1–4 for the voices, keypad
       Enter to tie.
     - Right after typing a note, a value key sets the next note's value;
       with notes the user selected, it re-times them.
     - Enter returns to zero and Cmd/Ctrl −/= zooms.
     - Picture and Cue Manager moved to Cmd/Ctrl+Alt+P/U, freeing
       Cmd/Ctrl+1–4.
     - Edit › Keyboard Shortcuts switches between Composer 2, VC Composer,
       Finale style and Sibelius style, and remembers the choice.
   - **Loop (L).** L, the deck's Loop key and Transport › Loop Playback
     loop the shared selection's range. With no range, they loop the bar
     the playhead is in and show it as the range.
     - The loop follows the range as it changes; clearing the range ends
       the loop.
     - The engine jumps back at the loop's end to the exact sample: notes
       off and delay lines cleared, as for any jump. Playback that starts
       after the loop's end plays on.
   - The note-value keycaps with their number legends arrived in step 4.

   The original step 3 description: One selection drives the counters,
   the inspector, the lit keys and both views. Add the shortcut table,
   with the Finale/Sibelius note values as the default profile.
4. **Composer view lanes to the spec. Done.**
   - **Rows.** Each instrument track is one row: a 220 px header, then
     its lane.
     - Rows are 108 px for one staff and 184 px for a grand staff, with a
       7 px staff space and a 120 px bar of 4/4 at 100%.
     - The clef zone is 46 px and stays put as the lanes scroll.
     - A key signature widens the clef zone, and every lane then takes
       the widest one, so bars line up from track to track.
     - Staves take the theme's paper, ink and staff colours. The
       selection and the cursor are in the LED colour; the playhead is in
       the LCD colour.
   - **Track header** (`ui/composer/LaneParts`):
     - the colour stripe, a number LCD and a scribble strip with the name
       and sound;
     - R, S and M keys that fill solid when on;
     - a staff tag (Treble, Bass, Alto, Perc, Grand staff and so on);
     - Vol and Pan knobs with LCD values, one console gesture each;
     - the track's meter;
     - a right-click menu: a new phrase from the selection or at the
       playhead, and the track menu.
   - **Ruler** (`ui/composer/ComposerRuler`):
     - The overview strip shows the whole piece, one row per track, with
       its phrases (or bars that have notes) and a box for what is
       visible. Click or drag the box to scroll.
     - Bars|Beats shows bar numbers, beat ticks, the selection's range bar
       and the playhead cap. Click to locate; drag to set the range,
       snapped to beats.
     - Markers shows cue flags and hit points; click one to locate.
     - Chords reads a symbol for each bar from every track's notes, spelt
       in the key (`ChordDetect`).
     - Tempo shows the tempo line and its marking.
   - **Phrase blocks.** `TrackVersion::phrases` holds an id, a name and a
     span, and is saved with the project.
     - Session functions: add, rename, set span, move, split, duplicate
       and remove. Each is one undo step, and a drag is one undo step.
     - Moving a block carries its notes, dynamics, hairpins and slurs, and
       refuses to land on other notes.
     - Duplicating a block copies its notes with their performance
       overrides.
     - On screen, a block has a 9% fill, a 55% outline and a solid name
       strip ("CELLI · CHORUS MELODY").
     - Drag the name to move by bars and the edges to trim on beats.
       Right-click to rename, split at the playhead, duplicate, select its
       notes or remove the block.
   - **Toolbar.** The Composer toolbar has:
     - Note entry, with an LED;
     - a note-value keypad in Bravura (whole to 32nd, dot, triplet,
       rest);
     - accidentals.
     The keys print their shortcut from the active profile, as a keypad
     prints its numbers (7–2, `.`, `/`, 0, −, 9, =). They follow a
     profile switch, and the value, dot, triplet and rest keys light from
     the editor.
   - **Inspector** (`ui/composer/Inspector`), under the selection panel,
     or in columns under the edit window below 1280 px:
     - Articulation, 3 × 2: accent, staccato, tenuto, marcato,
       staccatissimo and slur. A key lights when every selected note has
       its mark.
     - Dynamics, 4 × 2: pp to ff, plus crescendo and diminuendo. They are
       written where the selection starts, as in Finale and Sibelius.
     - Velocity sets the selected notes' performed velocity, and Humanize
       humanizes them when released. Both write performance overrides, so
       the score is unchanged. A velocity set with the Velocity knob
       survives Humanize.
     - Voice 1–4.
     - Transpose by a semitone or an octave
       (`Session::transpose_notes`, which follows ties and spells the
       notes in the key).
   - **New document.** The editor's cursor and the scroll start afresh
     with a new document (`Session::document_generation`).
   - **Checks.** `tests/unit/test_phrases` and the desktop check's
     Composer-lane section; screenshots 28 and 29.
5. **The keyboard deck. Done** (`ui/shell/KeyboardDeck`).
   - **Keys.** 88 keys, A0 to C8 (52 white, 36 black).
     - At 1× they are 22 px wide and 96 px tall: 1144 px, which fits at
       1680 px.
     - The deck is 140 px at 1× and 236 px at 3×. The edit window above
       shrinks to make room.
     - The key fills, the C labels and the dot on middle C follow the
       spec.
   - **Zoom.**
     - A slider from 1× to 3× in 0.05 steps, − and + keys (±0.25×), and a
       zoom LCD.
     - 88 · Fit returns to 1×.
     - Zoom to selection sets 2.5× around the selected notes.
     - Ctrl/Cmd + wheel zooms around the pointer; the plain wheel scrolls
       sideways.
     - Zooming keeps the anchor centred: the selection's middle note,
       else the sounding notes, else middle C.
     - Each project remembers its zoom (`ProjectData::key_zoom`, saved
       without an undo step).
   - **Lit keys.** Keys light for the selected notes as they sound, or
     for what is sounding while it plays, plus any key held.
     - A lit note off-screen is eased into view (150 ms).
   - **Playing.**
     - A click plays the note on the selected track. The velocity comes
       from how far down the key it lands.
     - With Enter on and note entry on in the Composer view, the note is
       entered at the cursor with the current value. For a transposing
       staff it is entered as written, spelt in that staff's key.
     - Shift-click, or the Chord key, adds to the chord. Tie ties the
       note.
     - The step LCD shows the next bar and beat, the value, the voice and
       the velocity.
   - **Wheels and octave.**
     - The pitch wheel springs back; the mod wheel sends CC 1.
     - Octave ± shifts what a MIDI keyboard plays (shown ±0, +1…). It
       does not move the on-screen keyboard.
   - **Short windows.** When the deck makes the right column short, the
     inspector goes compact and the selection readout gives up height.
   - **Still to come:** the Follow toggle in the edit window's title bar.
     Following is always on for now.
6. **Producer view tracks. Done.**
   - **Rows.** One row per track:
     - Instrument and MIDI tracks are 72 px: the MIDI lane above, and the
       track's own 26 px audio lane at the bottom.
     - Audio tracks are 84 px and aux/bus rows 60 px.
     - Clips on an instrument track's audio lane are the same clips as an
       audio track's. They draw as waveforms, can be moved, trimmed,
       faded and gain-dragged, and have take lanes.
   - **What a track plays.** Each instrument or MIDI track plays its MIDI,
     its audio lane, or both (`Track::playback`, saved, one undo step,
     `Session::set_playback`).
     - The engine sends no notes to the instrument on Audio. On MIDI the
       audio lane is silent and dimmed.
     - The instrument stays loaded either way, for a MIDI keyboard.
   - **Track header** (220 px, to the spec):
     - colour stripe, chevron, type tag (INST, MIDI, AUDIO, AUX),
       number and name LCDs;
     - R, S, M and I (input monitor) keys, the automation key and a
       volume LCD;
     - MIDI | Audio | Both keys on instrument rows.
     When expanded, it adds:
     - the IN › OUT routing LCD;
     - insert slots A–E with LEDs (amber when bypassed);
     - Print and Freeze, shown but not working until step 7;
     - Piano roll | Score.
   - **Expanding.**
     - The chevron, a double-click, or the Piano Roll key opens a track
       at 272 px with the piano roll and its velocity lane inline. The
       roll's keyboard sits at the right of the header column.
     - The open roll follows the selected track.
     - A new document opens with its selected track expanded.
     - The bottom piano-roll panel is gone.
     - The velocity lane is 48 px, as the spec has it.
   - **Resizing.** Drag a header's bottom edge to resize any row; the
     expanded height is remembered apart from the collapsed one.
   - **Clips list.** It sits in the Producer left rack, under New track
     and the tracks, and lists every audio clip, including those on
     instrument tracks' audio lanes. Clicking one selects it, scrolls to
     it and moves the playhead there.
   - **Checks.** `tests/unit/test_producer_tracks` and the desktop check's
     Producer-track section (the audio lane is heard on Audio and Both
     and silent on MIDI); screenshot 31.
7. **Print to audio**, onto the track's own audio lane:
   - per track or for several at once;
   - offline first, then real time;
   - selection or song, with or without effects;
   - 48 kHz / 24-bit WAV;
   - a progress LCD;
   - the instrument switched to its printed audio when done.
8. **Mix:** the channel strip in the right panel and the Mixer deck at the
   bottom, both on the existing console engine and its automation.
9. **Master:** live integrated LUFS and true peak, the master chain slots,
   a loudness target, and Export master to WAV and FLAC. MP3 waits for
   the encoder decision.
