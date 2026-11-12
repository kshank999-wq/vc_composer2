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
│ Presentation (JUCE)  Composer · Sequencer · Mixer · Library · Cue · Video  │  not yet built
├──────────────────────────────────────────────────────────────────────────┤
│ Services             Audition · Plug-in host (VST3) · Media/video · Sync    │  not yet built
├──────────────────────────────────────────────────────────────────────────┤
│ Realtime engine      Audio graph · MIDI scheduler · PDC · offline bounce   │  not yet built
├──────────────────────────────────────────────────────────────────────────┤
│ Domain (this repo, no framework dependency)                                │
│   vcc_templates  instrument registry · template registry · resolver ·      │  built
│                  instantiate (one undo step) · custom templates            │
│   vcc_project    tracks · versions · buses · cues · snapshots · undo ·     │  built
│                  collision policy · versioned atomic save/load             │
│   vcc_ostinato   seeded portable generator · variants · locks              │  built
│   vcc_theory     spelling · keys/modes · tunings · chords · Roman          │  built
│                  numerals · voicing/voice leading · progressions           │
│   vcc_core       JSON · ticks/meter/tempo · timecode · atomic files ·      │  built
│                  ids · note events · SMF writer                            │
│   notation/engraving/editing (port from original VC Composer, see 02)      │  next
└──────────────────────────────────────────────────────────────────────────┘
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

## Notation ↔ MIDI contract (Main Spec §8): plan

The original VC Composer `notation` module already has the canonical score
(stable event IDs, written vs sounding pitch, ties, tuplets, markings), a
score → MIDI renderer with expression maps, and an event-diff layer so an edit
costs its own size. In VC Composer 2:

1. Each `TrackVersion` will own a score part (staff, voices and notes), not only
   MIDI regions. Today's `MidiRegion` stays as the home for performance-only
   material and generated MIDI that has not been notated.
2. Stores are separate, as §8 asks: **score intent** (notation), **performance
   overrides** (per-note onset/velocity/CC offsets keyed by stable note ID),
   and the **rendered cache** (MIDI by dirty time range).
3. MIDI → notation edits update notation when the result is representable.
   Otherwise they stay as overrides, and a reconciliation prompt appears for
   ambiguous tuplets, overlaps and spelling.

## Build and test

```sh
cmake -B build -G Ninja -DVCC_WARNINGS_AS_ERRORS=ON
cmake --build build
ctest --test-dir build --output-on-failure
python3 tools/content/validate_content.py
```

- Eleven test binaries, each a CTest entry, using a minimal built-in harness
  (`tests/support/vcc_test.h`). There is no third-party test dependency.
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
| Main §20: ten edit/save/reopen cycles preserve articulations | `test_project` |
| Main §20: temp tracks excluded from deliverables | `test_project`, `test_workflow` |

## Repository layout

```
core/ theory/ ostinato/ project/ templates/   domain libraries (include/ + src/)
content/                                      shipped JSON content
tests/unit/ tests/support/                    test binaries and harness
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
| Main 0: discovery | Reuse audit, framework/licensing decision, VST3 host spike | Reuse audit done (02). **Framework licence needs a decision** (01) |
| Main 1: core scoring MVP | Desktop shell, notation entry, shared track panel, MIDI mirror, playback, shortcuts, save/reload, basic VST3 | Next: port notation/engraving/editing (02), then JUCE shell |
| Main 2: scoring workflow | Template wizard UI, library search/mapping UI, articulation engine, video + timecode, temp tracks, cues | Data model and timecode done; UI and media pending |
| Main 3: DAW production | Audio record/edit, mixer/buses/inserts/sends, automation, device I/O, PDC, stems/score export | Bus/routing model done; engine pending |
| Main 4: collaboration | Versions, snapshots, A/B review, sync, roles, comments, packaging | Versions and snapshots done (local); sync pending |
| Main 5: hardening | Performance, plug-in matrix, video sync, recovery, accessibility, signed installers | Pending |

### Immediate next steps

1. Port the original VC Composer `notation`, `engraving` and `editing` modules
   (see 02). This needs a go-ahead to build that code here.
2. Decide the JUCE licence (01). Then stand up the desktop shell: track panel,
   Composer view using the ported engraver, and a Sequencer piano roll on
   `ProjectData`.
3. Build the realtime engine: device I/O, the MIDI scheduler (port the
   original's sample-accurate scheduler and real-time discipline test), a
   GM/placeholder synth for templates with no libraries, and a VST3 instrument
   host with sandboxed scanning.
4. Build the Chord Explorer and Ostinato Creator pop-ups on the finished
   engines, with audition through the placeholder synth.
