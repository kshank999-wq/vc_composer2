# Original VC Composer: reuse audit and port record

Main Spec §19 Phase 0 and §21 ask for "a feature-by-feature parity audit"
of the original VC Composer. The original repository (`kshank999-wq/COMPOSER`)
was a Pro Tools AAX bridge: a standalone companion window plus a plug-in that
fed MIDI into Pro Tools. That route is abandoned. Its musical engine is what
VC Composer 2 needs, and has been ported.

**Port source:** `kshank999-wq/COMPOSER` at `726c4bd`. Before porting, its own
suite was built and run unmodified: 67 of 67 tests passed.

## What was ported, and how

| Original | Now | Notes |
|---|---|---|
| `shared/music` | `music/` (`vcc_music`) | MIDI events, Standard MIDI File read/write, host-time types (960 PPQ). Unchanged. |
| `shared/audio` | `audio/` (`vcc_audio`) | Lock-free SPSC ring buffer, audio config, metronome. Unchanged. |
| `notation/` | `notation/` (`vcc_notation`) | Score model, pitch/spelling, markings, expression maps, score→MIDI renderer, event diff, MIDI import, score file. Only change: the score file's JSON include. |
| `engraving/` | `engraving/` (`vcc_engraving`) | Layout, SMuFL, hit testing, SVG, print layout; `smufl-font-gen` tool. Unchanged. |
| `editing/` | `editing/` (`vcc_editing`) | Note entry, editing commands, undo journal. Unchanged. |
| `composer/` | `composer/` (`vcc_composer`) | Shortcut profiles, settings store and migrations, instrument catalogue, session template. |
| `app/ExpressionMapLibrary`, `app/ShortcutProfileLibrary` | `composer/` | Lifted out of the companion app. Namespace `vcc::app` → `vcc::composer`. |
| `plugins/aax/core` MIDI scheduler | `engine/` (`vcc_engine`) | Sample-accurate scheduler. Namespace `vcc::plugin` → `vcc::engine`. |
| `shared/json` | merged into `vcc_core` JSON | Core JSON now also provides the original API (`make_*`, `set`, `push_back`, `at`, `size`, `has`, null-safe `operator[]`, `to_text`, `Kind`, `parse_value`, depth limit 64). Integral numbers write as integers, so score files are byte-for-byte as before. |
| `shared/files` | merged into `vcc_core` Files | One atomic writer for both APIs. It keeps the original's path-free error messages and adds `fsync` of the file and its directory. |
| `resources/fonts/Bravura` | `resources/fonts/Bravura` | SIL OFL; licence and FONTLOG included. |
| `scripts/make-test-music-font.py` | `tools/fonts/` | Generates the SMuFL test font. |
| 25 test suites | `tests/ported/` | Kept with their own harness (`VCC_TEST`/`VCC_CHECK`). `test_json` is now `test_json_compat` and runs against the merged core JSON. |

Changes made to ported tests:

- Two library cases that round-tripped through the companion app's settings
  file were removed with that file (one each in the expression-map and
  shortcut-profile suites).
- An unused constant that Clang rejects under `-Werror` was deleted.

All ported suites pass under GCC 13 and Clang, both with `-Werror`, and under
ASan + UBSan. The ported code also compiles cleanly under VC Composer 2's
stricter warning set (`-Wold-style-cast`).

## Integrated into VC Composer 2

- **Track versions own a score.** `project::TrackVersion::score` is a
  `notation::Score`; MIDI regions sit beside it for performance material and
  generated MIDI. Duplicating a version copies its score, and switching
  versions swaps notation and MIDI together (Main Spec §8 "Versions").
- **One meter, one key.** The project's meter map and key are canonical. Every
  edit writes them into every score (`ProjectData::sync_scores`), so the
  Composer view's bars and the Sequencer's bars cannot drift apart.
- **Staves from the instrument registry.** Template instantiation sets each
  staff's clef, transposition, octave-clef shift and grand staff from
  `content/instruments`. Examples: viola reads alto; B♭ clarinet −2; horn −7;
  guitar treble-8vb, sounding −12; double bass −12; piano two staves; drums
  percussion clef.
- **Rendering.** `project::render_track` renders the score through the track's
  expression map (`default` or `orchestral_cc`) and adds the MIDI regions.
  Keyswitches are kept apart from the notes, which are pitched music.
  `export_project_midi` writes deliverable MIDI with notes and dynamics
  controllers and leaves temp tracks out. The `vcc export-midi` command uses it.
- **Persistence.** Each version's score is saved with the original score-file
  format inside the project file. Older project files without scores still
  load, and a corrupt score is refused by name.
- **Tests:** `tests/unit/test_score_integration.cpp` covers:
  - the Main Spec §22 vertical slice (three notes, staccato, dynamics → MIDI);
  - written versus sounding pitch;
  - ten edit/save/reopen cycles keeping articulations and stable note IDs
    (Main Spec §20);
  - independent version scores;
  - meter and key propagation;
  - backward compatibility;
  - controller export.

## Not ported

| Original | Why |
|---|---|
| `bridge/`, `shared/protocol` | IPC between the companion and the plug-in. VC Composer 2 is one process. |
| `session-control/` | Pro Tools Scripting SDK client. |
| `plugins/aax/juce`, AAX docs and signing | Plug-in wrapper for Pro Tools. The macOS packaging/notarisation approach is reusable when installers are built (Main Spec §3). |
| `app/` (rest) | Companion-window state bound to Pro Tools tracks. Its view-model and headless UI-test patterns are the model for the VC Composer 2 shell. |
| `ui/juce` | The notation view, renderer, SMuFL font loader and Bravura embedding come over with the JUCE desktop shell. That waits on the JUCE licence decision (`01-decisions-and-license-gates.md`, O1). |
| `test_realtime_discipline` | Its allocation-counting harness drives the plug-in core and bridge. It is to be rebuilt around `vcc_engine` with the realtime engine. |

## Still to consolidate

Two kinds of duplication remain from the port. Neither changes behaviour, and
both should be resolved when the code around them is next touched:

- **Two time bases.** `vcc_music` keeps the host's 960-PPQ types. `vcc_core` and
  `vcc_notation` use 2880. Conversion happens at the scheduler and MIDI-file
  boundary, as in the original.
- **Two instrument lists.** `composer::InstrumentCatalog` and
  `content/instruments`. The catalogue's library-patch favorites should merge
  into `templates::InstalledPatch`.

## Feature parity checklist

| Feature | Engine | In VC Composer 2 projects |
|---|---|---|
| Notation entry (keyboard, mouse, step), voices, chords, grace notes, nested tuplets | ✅ ported (`ScoreEditor`) | ✅ edits a track version's score as one undo step; UI pending |
| Engraving: beams, slurs/ties, cross-staff, dynamics/hairpins by voice, print/SVG | ✅ ported | Composer view pending (JUCE shell) |
| Score → MIDI with expression maps, keyswitches, hairpins, transposition | ✅ ported | ✅ `render_track`, `export_project_midi` |
| MIDI import with voice separation and keyboard split | ✅ ported | Import command pending |
| Shortcut profiles (Finale-style, Sibelius-style, default) | ✅ ported | Shell pending; profiles to be reviewed per 01 |
| Settings store and migration | ✅ ported | VC Composer 2 settings file pending |
| Undo preserving note IDs | ✅ ported (editor journal) | ✅ project undo; note IDs survive save/load |
| MIDI file export with markers | ✅ | ✅ cue markers |
| Atomic, crash-safe save | ✅ merged | ✅ |
| Sample-accurate MIDI scheduler | ✅ ported (`vcc_engine`) | Realtime engine pending |
