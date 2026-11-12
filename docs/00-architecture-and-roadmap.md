# VC Composer 2: architecture and roadmap

VC Composer 2 is a standalone, notation-first scoring workstation for film,
TV and game composers. It is not a plug-in. The product definition is
`docs/spec/VC_Composer_2_Development_Spec_v1.0.md` (the "Main Spec") plus
`docs/spec/VC_Composer_2_Templates_Supplement_v1.0.md` (the "Supplement").

This document covers Main Spec §22 work order 19: repository plan, module
boundaries, build system, test infrastructure and targets.

## Layers

```
┌──────────────────────────────────────────────────────────────────────────┐
│ Presentation (JUCE)  Composer · Sequencer · Mixer · Track & Library panel · │  built
│   desktop/src/ui     template browser · Chord Explorer · Ostinato Creator   │
│                      picture window · cue manager · console mixer ·         │
│                      recording, audio clips with waveforms and takes ·      │
│                      automation lanes · MusicXML and PDF parts menus        │
├──────────────────────────────────────────────────────────────────────────┤
│ Services             Audition · VST3/AU plug-ins hosted and scanned out of  │  built
│   desktop/src/audio  process (per instance, plug-in, maker or shared) ·     │
│                      plug-in state in the project · autosave                │
│                      FFmpeg picture decoding · render-by-cue bounces ·      │
│                      stem export with a sum-to-mix check · audio clips      │
│                      streamed from disk                                     │
│                      (collaboration sync: not yet)                          │
├──────────────────────────────────────────────────────────────────────────┤
│ Realtime engine      Snapshot-driven playback · placeholder synth · group/  │  built
│   PlaybackEngine     console strip DSP on every track/bus · inserts · sends │
│                      solo-in-place · meters · clip playback · recording ·   │
│                      automation read/touch/latch/write · plug-in delay      │
│                      compensation · click · live MIDI · offline render      │
├──────────────────────────────────────────────────────────────────────────┤
│ vcc_interchange      XML parser/writer · MusicXML 4.0 import and export    │
├──────────────────────────────────────────────────────────────────────────┤
│ vcc_picture/cues/media  picture sync, hit-point tempo fit, cut comparison, │  built
│                      cue hierarchy, cue sheets, spotting notes, decoding   │
├──────────────────────────────────────────────────────────────────────────┤
│ vcc_session          the open document for the app: edits, undo, file,     │  built
│                      autosave/recovery, change notification, generators   │
├──────────────────────────────────────────────────────────────────────────┤
│ Domain (this repo, no framework dependency)                                │
│   vcc_templates  instrument registry · template registry · resolver ·      │  built
│                  instantiate (one undo step) · custom templates            │
│   vcc_project    tracks · versions (score + MIDI) · buses · cues ·         │  built
│                  snapshots · undo · collision policy · save/load ·         │
│                  render_track · deliverable MIDI export                    │
│   vcc_ostinato   seeded portable generator · variants · locks              │  built
│   vcc_theory     spelling · keys/modes · tunings · chords · Roman          │  built
│                  numerals · voicing/voice leading · progressions           │
│   vcc_editing    note entry · editing commands · undo journal              │  ported
│   vcc_engraving  layout · SMuFL · hit testing · SVG · print · full score · │  ported
│                  PDF with the music font embedded (own deflate and         │
│                  OpenType reader)                                          │
│   vcc_notation   score model · spelling · markings · expression maps ·     │  ported
│                  score→MIDI · event diff · MIDI import · score file        │
│   vcc_composer   shortcut profiles · settings · expression-map library     │  ported
│   vcc_engine     sample-accurate MIDI scheduler                            │  ported
│   vcc_music/audio  SMF read/write · lock-free ring buffer · metronome      │  ported
│   vcc_core       JSON · ticks/meter/tempo · timecode · atomic files ·      │  built
│                  ids · note events · SMF writer                            │
└──────────────────────────────────────────────────────────────────────────┘

"Ported" modules come from the original VC Composer
(`02-original-vc-composer-reuse-audit.md`), with their original tests in
`tests/ported/`.
```

Rules that hold across layers:

- **Domain code is framework-free C++17.** It builds and tests with only
  CMake and a compiler on macOS, Windows and Linux. JUCE appears only in the
  presentation, services and realtime layers. If the framework decision
  changes (see 01), the domain is unaffected.
- **One document, one track list.** Composer, Sequencer and Mixer views read
  `project::ProjectData` and write through `Project::edit`, so each user action
  is exactly one undo step (Main Spec §4, §20).
- **Content is data.** Templates, instruments, articulations, progressions and
  ostinato presets live in `content/` as versioned JSON and are validated on
  every build. New packs ship without a binary change (Supplement §6.2, §8).
- **Nothing on the audio thread allocates, locks or does I/O.** Generators run
  on worker threads and hand finished `NoteEvent` vectors to the scheduler
  (Supplement §10.2, §12). The original VC Composer measured this with a
  counting allocator; that test is ported with the scheduler.
- **Never silent.** A missing library yields a labelled placeholder plus a
  message. Auditions never write to the project. A collision asks before it
  overwrites. A newer project file is refused, not half-read.

## Musical time

All positions are integer ticks at **2880 per quarter**, the resolution the
original notation engine adopted so nested tuplets land on whole ticks. Bars
come from the meter map only. Seconds come from the tempo map only, so tempo
edits never move notes off their bars (Supplement §9.3). Timecode
(`vcc/core/Time.h`) covers 23.976, 24, 25, 29.97 DF/NDF, 30, 47.952, 48, 50,
59.94 DF/NDF and 60. Drop-frame counting is tested against known reference
points.

## Notation ↔ MIDI contract (Main Spec §8)

**Done:**

- Each `TrackVersion` owns a `notation::Score`: stable event IDs, written vs
  sounding pitch, ties, tuplets and markings. `MidiRegion`s hold
  performance-only material and generated MIDI that has not been notated.
- `render_track` turns score plus regions into the MIDI the Sequencer and
  export see. Notation → sequence is therefore always current.
- The project's meter map and key are written into every score on every edit.

**Next:**

1. **Performance overrides.** Per-note onset, length, velocity and CC offsets,
   keyed by stable note ID and kept apart from score intent.
2. **Rendered cache.** MIDI by dirty time range, using the ported event diff,
   so an edit costs its own size.
3. **MIDI → notation.** A piano-roll edit to a linked note updates notation
   when the result is representable, and is otherwise kept as an override. A
   reconciliation prompt covers ambiguous tuplets, overlaps and spelling.

## Build and test

```sh
cmake -B build -G Ninja -DVCC_WARNINGS_AS_ERRORS=ON
cmake --build build
ctest --test-dir build --output-on-failure
python3 tools/content/validate_content.py
```

- 37 test binaries, each a CTest entry. Twelve are in `tests/unit`, using
  `tests/support/vcc_test.h`; 25 are ported from the original VC Composer in
  `tests/ported`, with their own harness. There is no third-party test
  dependency.
- They are clean under GCC 13 and Clang with `-Werror`, and under
  `-DVCC_SANITIZE=address,undefined`.
- CI (`.github/workflows/ci.yml`) builds and tests on Ubuntu, macOS and Windows
  (MSVC), and runs the Python content validator.

Spec test coverage at this stage:

| Spec item | Test |
|---|---|
| Supplement acceptance 7 (Horror tracks without plug-ins), T01 | `test_templates` |
| Acceptance 8 (Comedy content, distinct moods), T02 | `test_templates` |
| Acceptance 9 (regional traditions separate/searchable) | `test_templates` |
| Acceptance 10 (Bach/Vivaldi/Mozart/Beethoven variants), T03 | `test_templates` |
| Acceptance 11 (blues/jazz/rock/industrial/dark hybrid) | `test_templates` |
| Acceptance 12 (I–V–vi–IV in G at bar N) | `test_progressions` |
| Acceptance 13 (audition without writing; 8 bars, one undo) | `test_workflow` |
| Acceptance 14 (16-bar ostinato, 3 variants, editable) | `test_ostinato`, `test_workflow` |
| Acceptance 15 (merge never erases cues/buses/temp) | `test_templates` |
| T04 (minor ii–V–i spelling in E minor) | `test_chords` |
| T05 (preview leaves temp track untouched) | `test_workflow` |
| T06 (16 bars in 7/8) / T07 (same seed → same output) | `test_ostinato` |
| T08 (collision policy) / T09 (undo template as a unit) | `test_project`, `test_templates` |
| T10 (malformed pack diagnostics) | `test_templates` |
| Main §20: versions alternate without overwriting; snapshots | `test_project` |
| Main §20: ten edit/save/reopen cycles preserve articulations | `test_project`, `test_score_integration` |
| Main §22 vertical slice: one stave, three notes, staccato/dynamics → MIDI | `test_score_integration` |
| Notation engine, engraving, editing, MIDI files, scheduler | `tests/ported/*` (25 suites) |
| Main §20: temp tracks excluded from deliverables | `test_project`, `test_workflow` |

## Repository layout

```
core/ theory/ ostinato/ project/ templates/   VC Composer 2 domain libraries (include/ + src/)
music/ audio/ notation/ engraving/ editing/
composer/ engine/                             ported from the original VC Composer
resources/fonts/Bravura                       SMuFL music font (SIL OFL)
content/                                      shipped JSON content
tests/unit/ tests/support/                    test binaries and harness
tests/ported/                                 original VC Composer suites and harness
tools/vcc-cli/                                `vcc` command-line tool
tools/content/                                content validator
docs/                                         this file, decisions, reuse audit, schema, specs
```

## Roadmap

Phases follow Main Spec §19 and Supplement §14. Phases are capability gates,
not calendar promises; a full DAW is a multi-year programme (Main Spec §19).

| Phase | Scope | Status |
|---|---|---|
| Supplement P0: foundation | Schema, registry, instrument IDs, placeholder resolver | **Done**: 314 instruments, validated |
| Supplement P1: catalog | Cinematic, modern, regional, era and composer packs | **Done**: 203 templates. Cultural packs await expert review |
| Supplement P2: Chord Explorer engine | Keys/modes, progressions, voicings, insertion | **Done** (engine); UI pending |
| Supplement P3: Ostinato engine | Seeded generator, 16-bar variants, insertion | **Done** (engine); UI pending |
| Supplement P4: polish | Custom/save/import/export, mapper warnings, favorites | Engine done; favorites/UI pending |
| Main 0: discovery | Reuse audit, framework/licensing decision, VST3 host spike | Reuse audit and port done (02). **Framework licence needs a decision** (01) |
| Main 1: core scoring MVP | Desktop shell, notation entry, shared track panel, MIDI mirror, playback, shortcuts, save/reload, basic VST3 | **Done** in `desktop/`: Composer view with keyboard and mouse entry, track panel, Sequencer, playback via the placeholder synth or VST3/AU instruments, save/reload/autosave. Verified headlessly on Linux; macOS/Windows built in CI |
| Main 2: scoring workflow | Template wizard UI, library search/mapping UI, articulation engine, video + timecode, temp tracks, cues | Data model and timecode done; UI and media pending |
| Main 3: DAW production | Audio record/edit, mixer/buses/inserts/sends, automation, device I/O, PDC, stems/score export | **Console mixer, channel DSP, buses/sends, audio recording with punch and takes, clip editing, automation (read/touch/latch/write, lanes for every strip control, sends, inserts and plug-in parameters) and plug-in delay compensation done.** MusicXML import/export, PDF parts and full score, and stem export (mix plus stems per project or per cue, wet or dry, with a sum-to-mix check) done. |
| Main 4: collaboration | Versions, snapshots, A/B review, sync, roles, comments, packaging | Versions and snapshots done (local); sync pending |
| Main 5: hardening | Performance, plug-in matrix, video sync, recovery, accessibility, signed installers | Pending |

### Immediate next steps

1. Build the Main Spec §8 performance-override layer and the MIDI → notation
   path (see above).
2. Engine hardening:
   - a futex-style wait on macOS/Windows for plug-ins outside the app (they
     poll with short sleeps today);
   - opening an effect insert's own window from the console;
   - saving the delay-compensation switch with the project.
3. Picture follow-ups:
   - picture + score draft export (needs an encoder);
   - automated conform;
   - Windows FFmpeg in CI;
   - an LGPL FFmpeg for distribution.
4. Notation exchange follow-ups (§16): repeats/endings and lyrics in
   MusicXML, font subsetting to shrink PDFs, collision-aware vertical
   spacing in the full score.
