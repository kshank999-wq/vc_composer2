ADD-ON FEATURES \| DEVELOPMENT SPECIFICATION \| VERSION 1.2

Prepared for coding agent • November 14, 2026

Status: proposed extension to existing VC Composer DAW desktop DAW; not
a replacement or standalone DAW rebuild.

# 1. Executive brief

Implement eight add-ons across the notation-first Composer workspace,
production/mixing workspace, and an iPad companion: (1) one-action chord
entry, (2) instrument range guardrails, (3) visual audio routing, (4)
iPad handwriting, (5) external jog/shuttle support, (6)
contextual/detachable notation tools, (7) Circle of Fifths and key
reference, and (8) rapid per-track transposition from the left-hand
Trans control. All are extensions to the existing product.

# 2. Architecture and integration constraints

- Desktop target: existing VC Composer DAW application on macOS and
  Windows. iPadOS companion is a separately built and distributed app
  using a shared score/project model and sync protocol.

- Score model is canonical: chords become individually editable pitched
  note events; note edits update the corresponding MIDI events without
  creating duplicate tracks.

- Separate compositional constraints (notation, pitch, instrument
  profiles) from real-time audio graph routing. UI operations must be
  undoable, deterministic, and safe while playback is running.

- Reuse existing staff, track, measures, time signature, key signature,
  MIDI tick timeline (visible PPQ 960, internal 28,800 if current engine
  uses that), instruments, and plugin instances. Do not introduce
  arbitrary tick rounding.

- Cross-device editing should support versioned changes and conflicts
  instead of silent overwrites. Features must not break MusicXML, MIDI,
  score PDF, or session save/reopen.

# 3. Feature A — Composer Chord Mode

## 3.1 User experience

A Chord Mode toggle in the Composer workspace opens a compact floating
chord-entry palette. Composer chooses root pitch, chord quality,
extensions, alterations, bass/inversion, register, spacing/voicing, and
rhythmic duration; clicking/tapping a target note position on a staff
inserts the entire chord in one operation. Root selection is separate
from the inversion: “root position” is the default, first and second
inversions are available where musically applicable.

## 3.2 Palette controls

- Root: chromatic A–G with accidentals; display enharmonic spelling
  appropriate to selected key, with manual override.

- Quality: major, minor, diminished, augmented, suspended 2/4, power
  chord; support major/minor triads as initial release.

- Sevenths/extensions: dominant 7, major 7, minor 7, diminished 7,
  half-diminished 7, added 6, add9, 9, 11, 13, major/minor 9/11/13 where
  defined. Show distinct “add” vs “stacked extension” behaviors.

- Alterations and omissions: b5/#5, b9/#9, \#11, b13; omit 3/5 and
  optional slash-bass. Validate contradictions and allow custom pitch
  sets.

- Voicing: root/1st/2nd/3rd inversion when valid, close/open position,
  octave, spread, drop-2 (optional advanced), playability preview;
  present valid inversions for chord tone count.

- Preview/audition with selected track instrument, optional MIDI input
  voicing capture; buttons: Insert, Replace Selected, Apply to Next
  Beat/Measure, Cancel. Floating palette movable, pinnable and
  persistent by workspace.

## 3.3 Entry rules and edge cases

- A single staff insertion creates a simultaneous note collection at the
  chosen rhythmic offset, using the active duration; preserve measure
  boundaries, ties, rests, tuplets, voice/layer assignment, and clef
  transposition rules.

- For transposing instruments, treat chord root selection as
  concert-pitch vs written-pitch according to the current score display
  preference; do not accidentally double-transpose.

- Offer explicit handling if chord notes exceed instrument range:
  prevent, revoice, choose octave, or override according to range policy
  in Feature B.

- On edit, keep chord metadata (root, quality, alterations, inversion,
  chosen voicing) as helpful non-destructive metadata while the actual
  note pitches remain first-class editable score/MIDI events.

- One undo step for chord insertion/replacement; redo yields identical
  pitches and spelling. Playback should sound each inserted chord
  accurately.

## 3.4 Example acceptance checks

- In C major, selecting C + major + root position and clicking a
  quarter-note position enters C4/E4/G4 as one action (register
  configurable).

- Selecting G7/B generates a dominant seventh in first inversion with B
  as bass, accurate notation and synchronized MIDI.

- Selecting C7(b5,b9) correctly constructs and spells the requested
  pitch classes or prompts only for conflicting overrides.

# 4. Feature B — Instrument Range-Aware Notation

## 4.1 Functional behavior

Each assigned instrument loads an instrument profile. The notation
interface visually marks out-of-range pitch positions in a subtle
gray/disabled state and blocks accidental note placement above/below its
configured playable range. Enforce both lower and upper boundaries,
including score click-entry, chord entry, MIDI step entry, and imported
events as appropriate.

## 4.2 Instrument profile and UI

- Instrument profile fields: unique ID, instrument name/family, clef,
  lowest and highest sounding MIDI pitch, written transposition,
  practical/comfortable range, extended range (optional), mode/technique
  overrides (optional), and provenance/version.

- Range display: fade invalid ledger-line/staff placement targets and
  show precise min/max in a hover/tap tooltip; draw shaded zones
  alongside the staff if graying the staff itself would obscure existing
  notation.

- Policy settings per instrument/track: Strict (reject), Warn (allow
  with marker), Extended (where defined), Off (for experimental/synth
  instruments). Default Strict for acoustics, configurable for unusual
  techniques.

- Show a non-modal warning when imported MIDI/MusicXML or instrument
  reassignment yields existing out-of-range notes; do not delete
  existing notes automatically. Provide an inspection/report panel.

- Use written-pitch range for transposing notation display while
  assessing sounding-pitch constraints correctly; handle instrument
  switches, percussion/unpitched maps, keyboard ranges, sample-library
  key switches, and octave-transposing instruments.

- Distinguish a physical instrument’s range from its currently loaded
  sample-library articulation zone; show an optional articulation/sample
  availability warning, not an incorrect hard physical limit.

## 4.3 Acceptance checks

- Adding an orchestral instrument loads its documented playable range
  and disables pitch entry outside limits in Strict mode.

- Changing to another instrument recalculates visual boundaries without
  deleting original notes; concert/written pitch switching remains
  accurate.

- Pasting out-of-range material displays an actionable warning and
  offers transpose, revoice, keep as flagged, or cancel.

# 5. Feature C — Visual Audio Routing Workspace

## 5.1 Product concept

Add a “Visual Routing” button/tab in the Production workspace that opens
a zoomable, pannable, searchable signal-flow canvas. The full audio path
is visible as editable connections, not only tiny mixer dropdowns.
Support instrument and audio tracks, inserts, sends, returns/auxes,
subgroups, buses, master, monitor, and physical interface outputs where
supported.

## 5.2 Canvas interactions

- Nodes: track source, instrument/plugin, insert FX (ordered), pre/post
  fader, sends, aux/return, bus, master, hardware output. Color/shape
  coding and readable labels; each node carries channel format
  (mono/stereo/multichannel) and meter status.

- Drag from output port to compatible input port to create route; click
  line to inspect gain, pan, mute, send pre/post-fader state, and
  channel assignment. Right-click/long-press to disconnect or reroute.

- Reorder inserts by drag/drop using the existing engine’s ordered FX
  chain, never creating ambiguous parallel paths without explicit
  support.

- Provide auto-layout (left-to-right signal flow), fit-to-screen,
  isolate selected track, trace upstream/downstream, grouping and
  collapsed buses, and a mini-map for large sessions.

- Mirror mixer edits bidirectionally in real time; do not create a
  second independent graph state. Show pending/inactive connections and
  plugin bypass correctly.

- Block feedback cycles by default; if the audio engine explicitly
  supports controlled feedback loops, require explicit warning and
  safeguards. Validate channel mismatches and disconnected outputs.

- All routing mutations undoable, save/reopen stable, and applied to the
  real-time DSP graph using a safe graph-swap or lock-free mechanism
  without blocking audio callbacks.

## 5.3 Acceptance checks

- A user can visually route a strings track → reverb send → aux return →
  orchestral bus → master → hardware out, hear the result, and see
  matching mixer controls.

- A visually added insert appears at the correct order in the mixer; an
  insert removed in the mixer disappears from the canvas.

- Attempted unintended feedback loop is rejected and explained. Changes
  persist across reopen without audio pops or loss of playback.

# 6. Feature D — Handwritten Notation & Dedicated iPad App

## 6.1 Platform plan

Ship a native-feeling iPadOS companion app designed for Apple Pencil,
touch, and on-screen score editing; the macOS/Windows Composer view also
supports compatible active pens/touch hardware. The iPad is a distinct
application sharing a core score serialization layer and editing rules
with the desktop product. Do not assume desktop VST/VST3 hosting or full
desktop mixing is available on iPad.

## 6.2 Inking workflow

- Handwriting Mode displays manuscript-style staff paper with
  low-latency ink capture; finger pans/zooms by default, Pencil writes.
  Offer a user-configurable finger-draw option and accidental-touch/palm
  rejection.

- Recognize noteheads, stems, beams, rests, clefs, key/time signatures,
  accidentals, ties, slurs, dynamics, articulations and barlines in
  progressive phases. Recognition shows a preview that can be
  accepted/corrected before committing semantic notation.

- Keep raw ink strokes as an optional separate layer to preserve
  composer sketches and support offline re-recognition. Eraser removes
  ink or semantic notation according to selected tool; lasso
  selects/moves/duplicates measures and notes.

- Support Pencil pressure/tilt only where useful; recognition should not
  require them. Include a normal tap-entry mode, Chord Mode palette,
  contextual tool palette, and multi-touch zoom/navigation.

- Constrain recognized notes using instrument-range settings; visually
  flag ambiguous recognition, rhythm overflow, and conflicts instead of
  silently altering the composer’s intent.

- Provide a local lightweight audition path (built-in sound bank or
  platform instruments) without promising desktop sample libraries on
  iPad. Transport, tempo, metronome, score page/continuous view, and
  optional remote control of the desktop session.

## 6.3 Sync and collaboration

- MVP: explicit Save/Send to Desktop, import from desktop, and versioned
  transfer via documented package format. Later: optional LAN/cloud sync
  with authentication, offline queue, resumable sync, project revision
  IDs, conflict UI, and merge by measures/objects where safe.

- Projects created or modified on iPad must reopen in the desktop app
  with the same notes, rhythms, instrument IDs, score layout intent and
  undoable edit records where possible; avoid claiming arbitrary
  VST/plugin state will sync to iPad.

- Define data ownership, secure transport, storage encryption practices,
  device pairing, and project export/import tests before production
  release.

## 6.4 Acceptance checks

- Handwrite a two-measure melody with Pencil, correct one ambiguous
  note, commit it, and confirm faithful playback and a synchronized
  desktop score.

- Finger gesture zooms while Pencil writes; moving/restoring a
  handwritten phrase preserves music structure.

- Offline iPad edits save locally and transfer without loss;
  simultaneous desktop/iPad edits surface an explicit conflict workflow
  rather than overwriting.

# 7. Feature E — External Jog/Shuttle Wheel Compatibility

Provide a configurable external control surface layer so hardware
shuttle wheels, jog dials, and supported transport controllers can scrub
the Composer/Sequencer timeline. Start with OS-recognized HID devices
and configurable MIDI CC/transport input, with device-specific
integrations added only where SDK/licensing allows.

- Controls: jog frame/tick step, continuous shuttle speed/direction,
  play/pause/stop, locate next/previous marker, return to start, loop
  toggle; optional device buttons mappable through Preferences → Control
  Surfaces.

- Scrub modes: audio scrub where engine supports it; otherwise safe
  stepped transport/seek. In score view, cursor and playhead follow jog
  position, including video timecode when enabled.

- Set sensitivity/dead-zone, shuttle speed curve, snap to
  beat/bar/frame, reverse direction, acceleration and device profiles;
  support plug/unplug and persistence.

- Guard against device conflicts, unsupported proprietary protocol
  assumptions, audio dropout, runaway shuttle behavior, and
  focus-dependent key capture.

- Acceptance: a connected supported device is recognized or can be
  manually mapped, jog moves playhead accurately in both views, and
  shuttle remains stable at multiple speeds.

# 8. Feature F — Track-Contextual and Detachable Composition Tools

When a composer selects a staff/track in the notation window, show a
concise, contextual toolbar directly adjacent to that selected staff
rather than requiring the composer to reach to the top-level application
toolbar. Provide a detach/pin option to convert the toolset into a
free-floating window/palette that can be moved to any preferred screen
position.

- Core actions: note value/rest selection, accidentals, articulations,
  ties/slurs, dynamics, chord mode, tuplets, voice/layer, clef,
  expression, eraser/select; tool visibility adapts to selection and
  available actions.

- Toolbar follows active staff without covering notes; collision
  avoidance positions it above/below selected stave based on viewport
  space. Hide/collapse on inactivity or keyboard command.

- Detachable palette: reposition, resize if appropriate, dock/re-dock,
  pin to current monitor, per-workspace persistence, multi-display
  handling and display-disconnect recovery.

- Keyboard shortcuts, stylus access, screen readers and touch targets
  remain consistent; switching staff immediately updates tool context
  but does not discard current tool state unexpectedly.

- Acceptance: selecting any staff opens its nearby tools; detached tools
  stay at chosen location through edit and reopen; notes remain
  unobstructed and actions target the correct staff.

# 9. Feature G — Circle of Fifths & Key Reference Window

## 9.1 User experience

Provide an on-demand floating, resizable Circle of Fifths window
accessible from the Composer toolbar, the contextual staff tools, and
the main View/Reference menu. It is a reference and navigation aid, not
a compulsory key-change dialog. It stays open while the user enters
notes or auditions chords; it can be pinned, moved to another monitor,
or dismissed without changing playback.

## 9.2 Interactive circle and key selection

- Display outer major keys and relative inner minor keys arranged
  clockwise by ascending fifths. Label key signatures (number of
  sharps/flats), and use legible notation that supports enharmonic
  equivalents such as F# major/Gb major.

- Highlight the active score or selected passage key by default. Offer
  Follow Score Key and Manual Reference modes. Clicking a segment in
  Manual Reference changes only the reference window, never the score
  key signature or actual notes.

- Allow a deliberate “Apply Key to Score/Selection” action with
  confirmation when changing score data; reuse the existing
  key-signature/transposition command, undo/redo, and notation/MIDI
  synchronization. Preserve written vs concert key behavior for
  transposing instruments.

- Show immediate neighboring keys, relative major/minor, parallel
  major/minor, and dominant/subdominant relationships, with clear labels
  rather than relying on color alone.

- Provide a compact mode on iPad and touchscreen, sized for Pencil/touch
  targets, with the same underlying key-data service as desktop.

## 9.3 Notes, scale degrees, and chord reference

- When a key is selected, list its seven diatonic scale notes in correct
  letter-name order and spelling (e.g., D major: D, E, F#, G, A, B, C#).
  Display scale degree 1–7, tonic, subdominant, and dominant labels;
  show key signature on a miniature staff.

- For natural minor, display its seven notes; offer explicit Harmonic
  Minor and Melodic Minor (ascending/descending) reference options
  without silently changing the score key signature. Include a clear
  indication when altered sixth/seventh scale degrees are displayed.

- Show the diatonic triads for the selected scale using Roman numerals
  and chord names, plus optional diatonic seventh chords. Distinguish
  chords borrowed/altered outside the selected scale rather than
  labeling them diatonic.

- Support note highlighting by clicking a scale-degree or chord:
  highlight member notes on the reference panel and allow audition
  through the chosen track instrument where available. “Send to Chord
  Mode” pre-fills root, quality, extensions, and enharmonic spelling;
  inserting music still requires an explicit user action.

- If no key signature is set, default the reference to C major/A minor
  and label it as a reference default; when selection spans key changes,
  indicate which measure/selection anchor supplies the active key.

- Use a shared KeyTheoryService for circle order, pitch classes, note
  spelling, scale generation, Roman numeral analysis, and chord
  composition, avoiding inconsistent hard-coded key tables across
  desktop and iPad.

## 9.4 Acceptance checks

- In G major, the window highlights G and displays G, A, B, C, D, E, F#
  with one sharp; triads are G, Am, Bm, C, D, Em, F# diminished.

- Switching reference to Eb major displays Eb, F, G, Ab, Bb, C, D and
  three flats; the score remains unchanged until the user explicitly
  applies a key change.

- In A minor, natural and harmonic minor views correctly distinguish G
  from G#; a selection in a later modulation updates the active
  reference when Follow Score Key is on.

- Clicking a displayed diatonic chord and choosing Send to Chord Mode
  pre-populates its constituent chord settings without duplicating notes
  or changing the score; keyboard, touch, and screen-reader navigation
  work.

# 10. Feature H — Track Transpose (Trans) Control

## 10.1 Placement and fast workflow

Every instrument or pitched MIDI track in Composer and Sequencer views
has a clearly labeled “Trans” control in its left-hand track
menu/inspector. Clicking it opens a compact inline popover anchored to
that track, without navigating away from the score or halting playback.
Changes preview immediately; the control applies only to the selected
track unless the user explicitly chooses a multi-track selection.

- Main controls: semitone offset (+/− buttons and numeric entry), octave
  up/down shortcuts, interval name (e.g., perfect fifth), and target key
  selector where musically appropriate. Show the current offset
  prominently (e.g., +2 st) and provide Reset to 0 and A/B bypass.

- On-the-fly performance mode: transpose audible MIDI output and
  displayed notation together by default, using a non-destructive
  per-track transpose layer. A clearly labeled preference lets the user
  choose “Playback only” when retaining an untransposed written
  reference is desired.

- Optional permanent “Apply to Notes” action bakes the offset into
  selected score/MIDI note events after confirmation and resets the live
  offset to zero; reversible as one undo step. Never silently rewrite
  underlying score notes simply because a user adjusts Trans.

- Support mouse, keyboard and touch/stylus access, with minimum-sized
  touch controls on iPad. Keep the popover visible while auditioning;
  allow immediate +1/−1 semitone changes without reopening it.

- Indicate transposition status on each track header (small +/− semitone
  badge); provide explicit multi-track transpose for selected tracks but
  never change other tracks implicitly.

## 10.2 Pitch semantics and score behavior

- Track Trans is a user-selected musical pitch shift, distinct from an
  instrument’s inherent written-to-concert pitch transposition (e.g.,
  B-flat clarinet or horn in F). Maintain separate fields for instrument
  transposition and user transpose offset, and combine them exactly once
  in concert pitch, written pitch and playback pipelines.

- When the non-destructive track offset is active, notation pitch
  positions, accidental spelling and playback reflect the selected
  behavior; preserve rhythm, measures, voices, note duration,
  articulations, dynamics and automation. Show an option to preserve
  original notation in playback-only mode.

- Key-aware operations: selecting a target key calculates an interval
  from the track’s active reference key or an explicitly selected source
  key. Offer chromatic transposition (preserves semitone spacing) and
  diatonic transposition (preserves interval spelling) as distinct
  choices, with sensible enharmonic spelling.

- Global score key signatures must not be modified by a per-track Trans
  action. For independent track key-signature support, use existing
  architecture if available; otherwise show explicit warning/notation
  strategy rather than silently altering the entire score.

- Apply range rules from Feature B after transposition: flag pitches
  outside acoustic instrument range; allow Revert, Auto-octave (with
  preview), and Override only per track range policy. Do not clip notes
  or change octaves silently.

- Unpitched percussion and audio-only tracks: disable note transpose
  with explanatory tooltip. Audio clip pitch-shift is outside this
  initial feature unless the existing DAW has a reliable time-preserving
  pitch processor; never silently repitch audio.

- Chord Mode (Feature A) and Circle of Fifths reference (Feature G) must
  respect the selected track’s effective pitch/key without modifying the
  reference key of unrelated tracks. Notes inserted while Trans is
  active must sound exactly once transposed—never double shifted.

- Live changes at playback should be scheduled safely to the MIDI/event
  pipeline, avoiding stuck notes, skipped note-offs, loud
  discontinuities or real-time audio-thread locks; define note retrigger
  policy for held notes, sustain pedal and overlapping notes.

## 10.3 State, commands, and persistence

- Persist per-track transpose offset, playback-only/notation-visible
  setting, spelling preference, and optional A/B bypass state in project
  files and templates. Existing projects default to zero offset.

- Expose deterministic commands: SetTrackTranspose(trackId, semitones,
  viewMode), ResetTrackTranspose, PreviewTrackTranspose,
  CommitTrackTransposeToNotes, and SetTransposeBypass; use the existing
  undo/redo architecture and stable track identifiers.

- Keep staff notes, piano-roll representation, MIDI output, offline
  bounce and export paths consistent. Export explicitly chooses either
  current sounding transpose (MIDI render) or original score notes;
  MusicXML/score PDF must reflect the user-chosen written/export
  interpretation.

- Support hot changes during play/loop, transport stopped, track
  mute/solo, automation, instrument changes, and save/reopen. Do not
  require plugin-specific transpose features.

## 10.4 Acceptance tests

- A C4 note on Track A plays and displays D4 after +2 semitones in
  Trans; Track B remains unchanged. Reset returns Track A to C4.

- An octave-down (−12) shift updates live audition, score display and
  exported MIDI as selected; a project save/reopen preserves the offset
  and undo restores the previous state.

- B-flat clarinet with user Trans +2 sounds and notates correctly under
  written-pitch and concert-pitch display modes without applying the
  clarinet transposition twice.

- While looping, rotating transpose +1/−1 repeatedly does not leave
  stuck MIDI notes; a range violation is visibly flagged and never
  silently corrected.

- Commit to Notes writes pitches once, sets the active transpose offset
  to 0, and is recoverable with one undo; playback-only mode leaves
  notated pitches unchanged.

- The Trans popover is reachable from every eligible track left menu,
  with correct selected-track focus and no effect on unselected tracks.

# 11. Shared technical implementation contract

| Item              | Specification                                                                                                                                                                                                                                                                                                                                                                                              |
|-------------------|------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| Suggested modules | ChordDefinition/VoicingService; InstrumentRangeRegistry/RangeValidator; RoutingGraphAdapter/Canvas; InkCapture/NotationRecognizer; ControllerInputService; ContextualPaletteManager.; KeyTheoryService/CircleOfFifthsPanel; TrackTransposeService/TransposePopover                                                                                                                                         |
| Core events       | ScoreNotesInserted/Changed; InstrumentAssigned; RangePolicyChanged; AudioRouteAdded/Removed; InsertOrderChanged; InkRecognized; ControllerJogged; ActiveStaffChanged.; ReferenceKeyChanged; ScoreKeySignatureChanged; TrackTransposeChanged; TrackTransposeCommitted                                                                                                                                       |
| Persistence       | Version existing project schema; persist chord metadata, instrument profile reference/policy, routing graph UI layout, tool palette position and control-surface mappings. Ink layers and iPad revision IDs use sidecar or embedded versioned fields.; circle reference follow/manual preference, selected reference key, panel geometry; per-track transpose offset, mode, spelling preference and bypass |
| Versioning        | Backward compatible read where feasible. Unknown extension fields must not corrupt older project data; define migration and fallback when loading a newer project.                                                                                                                                                                                                                                         |
| Undo/redo         | One coherent command per gesture/action, covering both presentation state and domain model. Routing changes safely coordinate UI undo with DSP updates. Include track transpose change and commit operations.                                                                                                                                                                                              |
| Performance       | No UI allocations/locks on real-time audio callback. Large score/routing graphs must render incrementally. Ink stroke input must remain responsive during playback.                                                                                                                                                                                                                                        |
| Testing           | Unit tests for pitch spelling, interval sets, range/transposition, DAG/graph validation, controller mapping; integration tests score↔MIDI↔audio; platform UI and project roundtrip tests.; theory spelling and scale/chord tests for all major/minor keys and altered-minor variants; per-track live transposition, written/sounding pitch, MIDI note-off safety and export semantics                      |

# 12. Delivery plan for coding agent

## Phase 0 — Inspect existing implementation

- Map current notation object model, pitch/transposition rules,
  score-to-MIDI synchronization, instrument registry, mixer DSP routing
  architecture, platform UI framework, audio device/controller APIs and
  project save version.

- Identify actual desktop language/framework and current supported iPad
  sharing strategy; write targeted architecture decision records before
  introducing libraries or duplicating audio graph logic.

## Phase 1 — Desktop notation productivity

- Implement shared chord definition engine, palette, insertion commands,
  score/MIDI synchronization, unit and UI tests; then instrument-range
  registry, visual constraints, policy and validation paths. Implement
  KeyTheoryService and Circle of Fifths/key reference panel using the
  same chord/spelling primitives. Then implement the per-track Trans
  popover and transpose event pipeline with instrument transposition and
  range-policy checks.

- Implement contextual toolbar/detach behavior and integrate Chord Mode
  and range feedback.

## Phase 2 — Production visualization and hardware

- Build a read-only visual routing canvas from the actual mixer graph,
  then add graph edits, validations, cross-view live sync, undo and safe
  audio-thread updates.

- Add device discovery, generic HID/MIDI mapping, jog/shuttle transport
  adapter, preferences and hardware test matrix.

## Phase 3 — iPad handwriting

- Extract/shared notation serialization contract and deterministic score
  operations; build iPad native layout, Pencil ink layer,
  recognition/correction MVP, local playback and versioned desktop
  transfer.

- Add optional advanced recognition and live sync only after reliable
  offline save/reopen and conflict handling are proven.

# 13. Definition of done

- Each feature is accessible without interrupting existing
  Composer/Sequencer workflows, with sensible presets and user help
  text.

- End-to-end demonstrations: chord entered once → displayed & played
  correctly; invalid acoustic note cannot be accidentally entered;
  visible sends match audible mix; Pencil handwriting becomes editable
  notation; jog wheel navigates synchronized score/video; contextual
  palette follows or detaches. Circle of Fifths displays correct
  current-key notes/chords and never modifies the score merely from
  selecting a reference key. Track Trans +2 semitones changes one track
  only while playing, survives save/reopen, and does not
  double-transpose a transposing instrument.

- Automated regression covers score/MIDI parity, transposing instrument
  limits, audio graph validity, undo/redo, save/restore, accessibility
  and Windows/macOS screen/input variations. Include active transpose,
  range conflicts, playback-only mode, commit/undo, export and live
  note-offs.

- Document supported hardware protocols and iPad limitations explicitly;
  no implication of universal shuttle-wheel or VST3 compatibility on
  iPad.

# 14. Decisions to confirm during implementation

- Initial chord voicing behavior: default register relative to staff
  cursor, preserve melody top note, or automatic voice-leading from
  prior chord? Recommend cursor-relative plus manual voicing, with
  voice-leading as optional future feature. Align Circle of Fifths chord
  handoff with Chord Mode API.

- Instrument range strictness: recommend Strict default for traditional
  acoustic instruments, Warn for imported material, unrestricted
  profiles for electronic instruments.

- Handwriting release scope: recommend notes/rests/accidentals/rhythm
  first, then articulations, slurs and more advanced symbols based on
  recognition accuracy.

- iPad connectivity: start with explicit transfer, then evaluate LAN and
  cloud synchronization. Confirm Apple developer/distribution and
  preferred code-sharing architecture.

- Identify initial tested jog/shuttle device models and whether
  vendor-specific drivers are required; no universal compatibility claim
  until tested.

- Track Trans default: non-destructive pitch and notation shift; offer
  an explicit Playback only choice and a separate Commit to Notes
  action. Validate this design against the existing score/MIDI model.

- Circle of Fifths scope: initially prioritize major, natural minor,
  harmonic minor and melodic minor key references; defer other modes
  (Dorian, Mixolydian, etc.) until theory/spelling tests are validated.
  Track Trans defaults to semitone/octave adjustment; target-key and
  notation/export interpretation must be decided explicitly in UI.

IMPLEMENTATION NOTE: This specification defines expected behavior and
recommended architecture. The coding agent must reconcile it with the
existing repository, preserve established APIs and document deviations
before implementing irreversible changes.
