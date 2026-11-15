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
- MIDI CC lanes, consolidate, comping across MIDI takes (MIDI takes
  land on take lanes and can be promoted; swiping parts of them is not
  there yet).

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
   - Not wired yet: the Page view, shown disabled with a tooltip. (Loop,
     MIDI tracks to hardware, Count-in, pre-roll and Wait note have since
     been wired; see steps 3, 7 and 10.)

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
   - **Follow.** The **Follow** key in the edit window's title bar (also
     View › Follow Playhead, Cmd/Ctrl+Shift+F) controls three things while
     playing:
     - the Composer view scrolling with the playhead;
     - the Producer view scrolling with it;
     - the keyboard bringing off-screen lit notes into view.
     It is on by default, lit when on, and remembered between sessions
     (`AppContext::follow`, UI setting "follow"). Off, the views stay
     where you put them. The desktop check's Follow section covers both
     views.
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
7. **Print to audio. Done** (offline), onto each track's own audio lane
   (`audio/PrintToAudio`, `ui/shell/PrintPanel`).
   - **Choosing tracks.**
     - Tick tracks in the Print panel (Producer right column, under the
       channel strip) and press **Record score to audio**.
     - Or press **Print** in a track's expanded header for that track
       alone.
     - Several tracks print in one go, and the whole print is one undo
       step.
   - **How each track renders.** Each track renders offline through its
     own private engine with a print tap.
     - Only that track renders, from its MIDI alone (never its existing
       clips). The MIDI renders even while the track is playing earlier
       printed audio.
     - The tap takes the instrument's signal before the channel strip.
       **Include FX** takes it after the strip's processing and inserts
       instead.
     - The fader, pan, sends and buses are never printed.
     - The tap's latency (the instrument's, plus the inserts' when they
       are included) is rendered and dropped, so the print lines up.
   - **Placing the print.**
     - The print plays back through the same strip as the MIDI did, so it
       sounds the same. The check measures a null: −38.44 dB both ways.
     - With Include FX, the track is marked `printed_fx`. While it plays
       its print, the engine bypasses its channel processing and inserts,
       so nothing is applied twice. Its settings are untouched and come
       back on MIDI or Both.
     - **Freeze** prints with FX and plays the print. Freeze again goes
       back to the MIDI.
     - Earlier prints that a new print overlaps move to a take lane
       (`Session::place_prints`).
   - **Range.**
     - Song prints from the start and ends the clip where the sound ends.
       Trailing silence is trimmed; 2 s of release tail is rendered.
     - Selection prints at least the dragged range.
   - **Files.**
     - Prints are 48 kHz / 24-bit stereo WAV.
     - They are named `<Track>_PRINT_NN.wav`, numbered past the files
       already there, in the project's `Audio Files`. An untitled project
       uses the same scratch folder as recording.
   - **When it finishes.**
     - **Play audio** (on by default) switches each printed track to play
       its audio lane.
     - A track with nothing to play in the range places nothing and says
       so.
   - **Progress.** The LCD reads, for example, "CELLI  2 / 3  BAR 14", with
     a percentage and a red bar. The print renders in slices on a timer,
     so the window stays live, and the red key becomes Cancel while it
     runs.
   - **Real-time printing. Done**, with external MIDI out:
     - **MIDI tracks.** New track › MIDI makes one
       (`Session::add_midi_track`). The channel strip's **MIDI ›** readout
       picks an output device (`Track::midi_output`, saved, one undo step).
       The notes then go to the hardware, not the track's instrument, and
       the hardware is heard through the track's input (shown as RET).
     - **How MIDI goes out** (`audio/MidiOut`). The audio thread stamps
       each message with when it should sound: the block's time, plus the
       delay compensation, plus the output latency, so the hardware lines
       up with what is heard. It pushes the message onto a lock-free queue,
       and a sender thread waits for that time and sends it.
     - **Notes off.** All notes off goes out on stop and on any jump.
     - **Offline renders** never play real hardware. Tests register a
       virtual port.
     - **The sender thread** starts only once a track uses external MIDI.
     - **Print panel: Offline | Real-time.**
       - Real-time plays the range live and records every ticked track in
         one pass (`PlaybackEngine::start_live_print`).
       - An instrument track records its instrument (before the strip, or
         after its inserts with Include FX), from its MIDI, never its
         clips.
       - A MIDI track on hardware records its audio input, its synth's
         return, with the round-trip latency and the compensation taken
         off.
       - The audio thread only copies into threaded disk writers. The
         print ends itself after the range and a 2 s tail, stopping the
         transport.
       - Stopping or Cancel throws it away; finished, it is placed like an
         offline print (`place_live_prints`), as one undo step.
       - It needs a running audio device and says so otherwise. An offline
         print of a hardware track says to print it in real time.
     - **Short windows.** When both panels do not fit, the Producer column
       shows **Channel | Print** tabs. A header's Print or Freeze switches
       to the Print tab.
     - **Checks.**
       - A virtual synth receives the MIDI track's notes.
       - Its return is what lands in the file.
       - An instrument printed in real time nulls bit-exactly against its
         offline print.
       - Both land on their audio lanes as one undo step.
   - **Checks.** The `place_prints` unit test, and the desktop check's
     print section covering format, placement, undo, the null, selection,
     Freeze and a silent track. Screenshots 32 and 33.
8. **Mix. Done.** The channel strip and the mixer deck, both on the
   console engine and its automation.
   - **Channel strip** (Producer right column, `shell::ChannelPanel`):
     - Inserts A–E with bypass LEDs (`shell::InsertSlots`). Click an LED
       to bypass or activate the insert. Click a name to open its window,
       or to add a processor to an empty slot. Right-click for the full
       slot menu.
     - A send knob per FX bus.
     - Pan, the long-throw fader with its dB scale, and the stereo meter.
     - Read / Touch / Latch / Write, and M / S.
     - Clickable IN and OUT readouts. An instrument track's input is what
       it records onto its audio lane.
     - In a short column, the inserts become a row of lights and the mode
       keys go two by two.
   - **Mixer deck** (`shell::MixerDeck`). It is the Producer view's bottom
     deck by default, and the Mix stage opens it.
     - Keyboard | Mixer keys switch the bottom deck; the Console key opens
       the full console window.
     - One 78 px strip per track and group/FX bus: a coloured name plate,
       insert lights (click to bypass, double-click to open), pan, the
       fader beside its stereo meter, a dB readout, and S/M.
     - The wider master strip is outlined in the LED colour and pinned at
       the right, while the other strips scroll sideways.
     - Clicking a strip's plate selects its track.
   - **Shared code.**
     - The insert, output and input menus, insert bypass, and writing a
       fader, pan or send now live in `console::MixActions`, used by the
       console window, the channel strip and the deck alike.
     - `console::MixWriter` turns a gesture into automation while a pass
       is writing that parameter (Touch, Latch, Write while playing), and
       into one undo step per gesture otherwise.
     - Faders and pans follow automation as it plays.
   - **Meters.** The engine has three independent meter readers per strip
     (`take_deck_meter(id, reader)`), for the top deck and headers, the
     channel strip, and the mixer deck, so no display takes another's
     peaks.
   - **Checks.** The desktop check's mix section; screenshot 34.
9. **Master. Done.**
   - **Loudness library** (`vcc/audio/Loudness`, no JUCE). It follows
     ITU-R BS.1770-4 / EBU R 128.
     - K-weighting, re-derived for any sample rate.
     - True peak by 4× polyphase oversampling.
     - 100 ms blocks from a real-time-safe analyser.
     - Momentary (400 ms), short-term (3 s) and integrated loudness, gated
       at −70 LUFS absolute and −10 LU relative.
     - `tests/unit/test_loudness` runs the EBU Tech 3341 cases: −23 and −33
       at 44.1, 48 and 96 kHz, the relative and absolute gates, and the
       inter-sample peak (−3 dBFS samples read 0 dBTP).
   - **Engine.** It measures its master output while it plays (never a
     print's tap) and hands the blocks over a lock-free queue
     (`take_loudness_blocks`, `reset_loudness`).
   - **Master panel.** It fills the Producer right column at the Master
     stage.
     - Readouts: integrated LUFS (large), momentary, short-term, and true
       peak (red edge and OVER above −1 dBTP). They restart with each play,
       or with Reset.
     - The loudness target is saved with the project
       (`ProjectData::loudness_target`, `Session::set_loudness_target`).
       The key steps −14 (streaming, the default), −16 (Apple), −23 (EBU
       R 128), −24 (ATSC). A readout shows the LU to target.
     - **Analyse** renders the song offline and measures it. **Match** sets
       the master fader to meet the target, as one undo step, and warns
       when peaks would then pass −1 dBTP.
     - Master chain keys add the EQ, glue compressor and limiter to the
       master bus in chain order. Pressed again, they bypass; right-click
       opens the processor.
   - **Export master** (`audio/MasterExport`).
     - The whole mix (tracks, buses, master chain) renders offline, song or
       selection plus a 3 s tail.
     - Output is WAV or FLAC, 16- or 24-bit, at 44.1, 48 or 96 kHz, with
       TPDF dither at 16-bit.
     - It is measured as it is written and reports LUFS and dBTP. It
       renders in timer slices with a progress LCD and Cancel.
     - The default file is `Bounces/<title>_master.wav` beside the project.
     - **MP3** (decision D18):
       - Encoded with LAME, loaded at run time and never linked: free, since
         the MP3 patents expired in 2017.
       - 320, 256 or 192 kbps CBR, or VBR V0, at 44.1 or 48 kHz, joint
         stereo, LAME quality 2.
       - An ID3v2 title, and the LAME/Xing header frame for exact length
         and gapless playback.
       - Without LAME, the MP3 key is off and its tooltip says how to get
         it.
       - JUCE's MP3 decoder is now enabled, so MP3s import and play as
         clips. The check uses it to read masters back.
       - CI installs LAME (`libmp3lame0`, `brew install lame`).
       - Disabled keys now dim.
   - **Stems.** The Master stage no longer opens the stem dialog; stem
     export stays in the File menu.
   - **Checks.** The desktop check's master section:
     - the live engine path;
     - target stepping;
     - Analyse then Match landing on −23.00 LUFS;
     - chain order and bypass;
     - a 24-bit/48 kHz WAV and a dithered 16-bit/44.1 kHz FLAC, each read
       back and re-measured.
     Screenshot 35.

10. **Recording workflow. Done.**
    - **Count-in and pre-roll.** The deck's Count-in key opens a menu:
      Off, Count-in 1 or 2 bars, Pre-roll 1 or 2 bars. Its sublegend shows
      the choice, and it is kept for next time.
      - With Record on, Play starts that many bars early (before bar 1
        too: the engine runs at negative positions), at the tempo and
        meter where the recording starts.
      - A count-in plays its own click, metronome or not, with the music
        silent; a pre-roll plays the music in.
      - Nothing is recorded before the start position. Stopping during
        the count-in goes back to it and records nothing.
    - **Wait note.** With Record on, Play waits for the first note from
      the MIDI keyboard or the keyboard deck, then starts there. The key
      flashes while waiting, Stop cancels, and the note that started the
      take is its first note, on the start position.
    - **MIDI recording.** An armed instrument or MIDI track that the
      keyboard plays records what is played.
      - Notes are captured on the audio thread into a lock-free queue, on
        the heard timeline less the output latency, as audio takes are.
      - A note pushed up to 150 ms early starts on the take's start.
      - The pass becomes a region of whole bars ("Record MIDI", one undo
        step); a region it overlaps moves to a take lane.
      - Take lanes are now silent for MIDI too: only the main lane plays.
      - The channel's input menu chooses what an instrument or MIDI track
        records: MIDI from the keyboard (the default) or audio from the
        input (a hardware synth, say). `Track::record_source` is saved.
    - **Loop recording.** Every cycle round the loop is its own take. The
      next cycle's files are opened ahead of the wrap, and the audio
      thread swaps takes at the loop's end to the sample. An unused one is
      deleted on Stop.
    - **Comping.** In an open take lane, a drag across a take selects a
      range (snapped to the grid; Cmd/Ctrl for free). Releasing it makes
      that part the main lane's ("Comp take"), trimming or splitting what
      was there. Alt-drag still moves the take, and a double-click
      promotes the whole take.
    - **Checks.** The desktop check's recording workflow section covers:
      - a 1-bar count-in (click only, take at the position);
      - pre-roll;
      - stopping during a count-in;
      - three loop cycles as three takes;
      - MIDI notes, velocities and bar-aligned regions;
      - silent MIDI take lanes;
      - wait note;
      - a mouse-driven swipe comp and a double-click promote.
      Screenshots `recording-takes` and `recording-comp`.

11. **Clip editing. Done** (audio clips in the Producer view's lanes).
    - **Split at playhead** (Cmd/Ctrl+T, or the clip menu): cuts every
      clip on the selected clip's track that the playhead is inside, all
      lanes. The left part keeps the clip's id and fade in; the right part
      reads on from the cut and keeps the fade out. No part under 5 ms.
    - **Crossfade with the next clip** (clip menu: 10, 20, 50, 100 or
      250 ms): where clips overlap, the fade covers the overlap; where they
      only meet (after a split or a comp), the second clip starts earlier
      into its own file, as far as its file has audio before it, and the
      two fade across each other with the equal-power fades playback uses.
    - **Nudge**: Alt+Left/Right moves the selected clip 10 ms, with Shift
      one step of the grid; repeated nudges are one undo step. Never before
      zero.
    - **Consolidate** (clip menu): the selected clip and every clip on its
      lane chained to it by touching or overlapping are rendered, with
      their gain, fades and mutes and at the highest rate among them, into
      a new 24-bit WAV "<Track>_CONSOL_NN.wav" in Audio Files, which
      replaces them as one undo step. Source files are never changed.
    - Session: `split_audio_clips`, `crossfade_audio_clips`,
      `nudge_audio_clips`, `replace_audio_clips` (unit-tested);
      `audio/Consolidate` renders.
    - **Checks.** The desktop check splits a 440 Hz tone twice (the call
      and Cmd+T), crossfades, nudges with Alt+Right, consolidates and reads
      the new file back sample for sample against the tone, then undoes.
      Screenshot `clip-editing`.
    - **Several clips at once** (added November 15): Shift-click adds a
      clip to the selection (or takes it out); a box dragged across empty
      lanes selects every clip it touches (Shift adds to the selection).
      Delete removes them all and Mute/Unmute in the clip menu changes them
      all, each as one undo step; Alt+arrows nudge them together (one step);
      Consolidate takes the selected clips on the main clip's track; a click
      on a selected clip keeps the selection, so it can be dragged as one.
    - **Splitting MIDI regions:** Cmd/Ctrl+T splits the MIDI regions as well
      as the clips under the playhead, on the tracks of the selected clips,
      or with none selected on the selected track. A note across the cut
      stays whole in the left region (nothing re-attacks); controllers go
      by their tick ("Split region", `Session::split_regions`, unit-tested).
    - Not yet: drawing crossfade curves across the overlap.

12. **MIDI tools. Done** (on played-in MIDI: regions; score notes follow the
    score and are shaped through their performance instead).
    - **Quantize** (sequencer toolbar, or Q in the piano roll): the
      selected notes, or with none every note of the track's main-lane
      regions. Grid 1/4, 1/8, 1/8 triplet, 1/16, 1/16 triplet or 1/32,
      strength 25-100%, swing (every second step late by up to half a
      step) and, optionally, note ends (never shorter than half a step).
      Lengths are kept; regions grow to whole bars to keep notes that move
      past their edges; no two notes of one pitch are left overlapping.
    - **Input quantize** (Quantize menu, per track, saved): MIDI recorded
      onto the track lands on the grid as the take is placed.
    - **Velocity** (toolbar): louder or softer by 10, scale to 120% or 80%,
      even out halfway to the average, or set all to 64, 90, 100 or 127.
    - **Controllers.** Regions now hold CCs (saved; they play on the
      track's channel like its notes). MIDI recording keeps them: the
      sustain pedal, the mod wheel and any other controller played.
      The piano roll's bottom lane chooses (click its name) Velocity or
      CC1 Modulation, CC11 Expression, CC7 Volume, CC10 Pan or CC64
      Sustain; dragging in a CC lane draws the controller across the
      dragged span, half a grid step a point (sustain is on or off).
    - The render cache now notices a region's lane and its controllers
      changing (a promoted take used to keep sounding as before).
    - `project/MidiTools` (quantize, velocity, overlaps) and the session's
      `quantize_regions`, `change_region_velocities`,
      `draw_region_controller`, `set_input_quantize` are unit-tested
      (`test_midi_tools`).
    - **Checks.** Recording with input quantize and the pedal; toolbar
      quantize; the Q key; velocity; a CC1 ramp by call and CC11 by mouse;
      the drawn CC1 in the track's render. Screenshot `midi-tools`.
    - **Pitch bend and channel aftertouch** (added November 15): kept in
      regions with the CCs (controller numbers 128 and 129 past the CCs;
      pitch bend's full 14 bits, 8192 the centre), recorded with the
      notes, saved, played (as their own MIDI messages, never CCs),
      exported to and imported from MIDI files, and drawn in the piano
      roll's bottom lane (Pitch bend, drawn about its centre line;
      Aftertouch). The placeholder sound bends +/-2 semitones (the General
      MIDI default) and swells with aftertouch. Checked: a played-in bend
      and pressure are recorded; a held A bent up halfway sounds at
      493 Hz after 440 (x1.121); the unit test round-trips save, MIDI
      export and import.
    - Not yet: a CC lane in the Composer view, transform tools (legato,
      humanize for regions, transpose), polyphonic aftertouch.

**CI on macOS (November 14, 2026).** The desktop check now passes on
macOS, Linux and Windows. What it took:
- The out-of-process plug-in host ran its message loop through
  `[NSApp run]`, which in this bundle-less helper never handled a message:
  every host connected and then sat until the ping timeout. On macOS it
  now runs the run loop in slices (`runDispatchLoopUntil`). The host logs
  its steps to `VCC_PLUGIN_HOST_LOG`, and CI prints that log and any crash
  reports when the macOS check fails.
- `os_sync_wait_on_address_with_timeout` takes mach time units, not
  nanoseconds; IPC waits on Apple silicon were about 40 times too long.
- The macOS runner has an audio device, which kept driving the engine
  while the check rendered offline. `prepare_offline` now detaches the
  engine from the device.
- Check fixes: `pump()` waits for posted clicks to be handled; IPC wake
  times are measured from the signal; the allocation probe calls
  `operator new` directly; build parallelism is capped.

13. **Drag-and-drop import. Done.**
    - **Onto the sequencer's lanes:** audio (WAV, AIFF, FLAC, MP3) and
      MIDI files dropped on a track land on it at the time under the
      pointer, snapped to the grid; dropped below the tracks they make new
      ones. While dragging, the row it would land on and the drop time are
      highlighted ("Drop here for a new track" below the tracks).
      - Audio dropped on an audio track lands on it; on an instrument or
        MIDI track, on that track's own audio lane. Several files play one
        after another. Inside a saved project they are copied into
        "Audio Files".
      - A MIDI file with one part dropped on an instrument or MIDI track
        joins it as a region from the bar under the pointer (a region it
        overlaps moves to a take lane). A file with several parts, or one
        dropped below the tracks, gives each part a new instrument track
        named after it, its sound from the part's program change
        (channel 10: percussion).
    - **Onto the window elsewhere** (the Composer view, the panels): audio
      on the selected audio track or a new one, at the playhead; MIDI as
      new tracks from the playhead's bar.
    - **File > Import Audio or MIDI...** takes both kinds through the same
      code.
    - A MIDI import keeps notes, velocities and controllers (sustain,
      modulation...), rescales the file's resolution, and is one undo step
      ("Import <file>"). A project with nothing in it yet takes the file's
      tempo and meter.
    - `Session::import_midi_file` is unit-tested; `ui/Import` holds the
      shared audio and MIDI import.
    - **Checks.** A real WAV and two MIDI files are dropped on the lanes:
      a two-part file below the tracks (two tracks, its 96 BPM), the WAV on
      a track's row (its audio lane, copied into the project), a one-part
      file on that row (from the bar under the pointer), a text file
      (nothing changes), and the WAV on the window (a new audio track at
      the playhead). Screenshot `drag-drop`.
    - **MusicXML and projects** (added November 15): a .musicxml, .xml or
      .mxl file dropped on the window adds its parts as tracks with their
      notation (as File > Import MusicXML, one undo step a file); a .vcc2
      project file dropped on the window opens it, after offering to save
      the project that is open. (The sequencer's lanes take audio and MIDI
      and leave these to the window.)
    - Not yet: dragging clips out to the desktop.

14. **MIDI take comping. Done.**
    - MIDI takes now show as take lanes under their instrument track (a
      region with a mini roll of its notes), opened and closed with the
      **TAKES** key beside MIDI / AUDIO / BOTH in the track header (lit
      while open). The main row and the piano roll show only the
      main-lane regions: what plays.
    - **Swipe comping:** dragging across part of a MIDI take (the I-beam
      pointer; snapped to the grid, Cmd/Ctrl for free) makes the notes that
      start in that range, and its controllers there, the main lane's: the
      main lane's own notes starting there go ("Comp take"). They join the
      main-lane region the range starts in, or a new region of whole bars.
    - **Double-click** a MIDI take, or **Promote take** in its right-click
      menu, swaps it onto the main lane whole; **Comp all of it** keeps the
      main lane's regions and replaces only what the take covers.
    - Session: `comp_region_take`, `promote_region_take` (unit-tested).
    - **Checks.** Two recorded takes; the TAKES key; a mouse drag across
      part of the take comps those notes in and keeps the rest; a
      double-click promotes it; only the main lane renders. Screenshot
      `midi-comp`.

15. **Sidechain. Done.**
    - A track's or bus's channel compressor and gate can be keyed from
      another track or bus: the console strip's **KEY** button (beside GATE
      IN in the dynamics section; lit when keyed, its tooltip names the
      source) offers "Its own signal" or any track or bus. A built-in
      compressor or gate insert is keyed the same way, from its slot's menu
      ("Sidechain..."). Saved (`Track::sidechain`, `Bus::sidechain`,
      `InsertSlot::sidechain`); each change is one undo step.
    - The key is the source's signal after its inserts and before its
      fader, so a muted source (a kick that should only duck) still keys.
      The detector reads the key; the gain is applied to the keyed strip.
    - The engine taps each source into a buffer made when the snapshot is
      built (nothing allocates on the audio thread) and processes tracks
      with their key sources first, so a key from a track arrives in the
      same block. A key from a bus reaches a track one block late (buses
      run after the tracks). Delay compensation does not shift the key.
    - Hosted effect plug-ins with their own sidechain inputs are not fed
      yet (the out-of-process host carries one stereo bus).
    - **Checks.** A quiet bass keyed from a muted 1 kHz kick added after it:
      unkeyed it ignores the kick (-38.1 / -38.1 dB); keyed it ducks only
      while the kick sounds (-38.1 to -63.0 dB, 25 dB of gain reduction on
      its meter); unkeyed again it ignores the kick; a keyed compressor
      insert ducks the same; keyed processing allocates nothing.
      `test_producer_tracks` covers the session side and saving.

16. **Full-screen mixer. Done** (revised November 15: hardware proportions).
    - The mixer is meant to be used full screen. The toolbar's **Mixer** key
      (View > Mixer, Cmd/Ctrl+M) or the bottom deck's **Full screen** key
      brings up the channel console over everything under the transport
      deck, from either view. Pressing Mixer again, or choosing a view,
      returns to where it was opened; **COMPACT** in the console's header
      returns to the compact mixer in the Producer view's bottom deck,
      under the tracks and the inline piano roll.
    - It is laid out like a large-format console: input, filters, EQ,
      dynamics, inserts and sends above (scrolling when the screen is not
      tall enough for all of them), then pan, automation, mute/solo, the
      fader and meter, and the scribble strip. The fader's throw is about
      30 % of the strip's height (140-340 px; up to ~36 % when every
      section fits with room to spare), as a 100 mm fader is on a desk -
      not stretched to the window. The console window (a second monitor)
      uses the same proportions.
    - **Checks.** Full screen covers the edit window and its fader is a
      quarter to two-fifths of the strip; COMPACT returns to the 262 px
      bottom deck; Cmd/Ctrl+M from the Composer view opens it and closes it
      back to the Composer view. Screenshot `35b-mixer-full`.

17. **Page view. Done** (the last disabled control).
    - The toolbar's **Track | Page** keys switch the Composer view between
      the track lanes (where notes are written) and the full score on
      pages, exactly as File > Export Full Score prints it: the same
      layout (`lay_out_full_score`, now shared by the view and the PDF),
      A3 transposed score in orchestral order, bracketed by family, with
      the title, instrument names and a staff size that fits the
      orchestra. The **Score** stage opens the Page view; **Compose**
      returns to the Track view.
    - Pages are drawn as sheets on the desk with the screen notation
      renderer. The wheel scrolls; Cmd/Ctrl+wheel and the zoom keys zoom
      (100 % fits a page to the view's width, up to its printed size).
      The system the playhead is in is marked; clicking a system moves
      the playhead to its first bar. The pages are re-laid a moment after
      an edit, and only while they are shown.
    - It shows the score; writing and editing stay in the Track view.
    - **Page settings** (a bar over the pages): Paper (A3, Tabloid, A4,
      Letter), Concert pitch (a score in C; off: transposed), bars per line
      (1-8), Hide empty (staves of parts resting a whole system) and
      Orchestral order (off: track order). They re-lay the pages at once,
      are remembered, and are the same settings File > Export Full Score
      starts from (and saves), so the PDF is what the Page view shows.
    - **Checks.** The Page key (enabled, lit, the lanes hidden); the pages
      are the printed A3 score (aspect 1.414, all the parts); notes typed in
      the Track view lay out as 16 bars in 4 systems; a click on a later
      system moves the playhead to its first bar; zoom; a new track appears
      as another part; the Track key brings the lanes back. Screenshots
      `36-page-view`, `36b-page-view-music`.
