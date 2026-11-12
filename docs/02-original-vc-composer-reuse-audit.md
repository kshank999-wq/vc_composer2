# Original VC Composer: reuse audit

Main Spec §19 Phase 0 and §21 ask for "a feature-by-feature parity audit"
of the original VC Composer. The original repository (`kshank999-wq/COMPOSER`,
about 390 files and 50k lines of tests) was written as a Pro Tools AAX bridge.
A standalone companion window plus a plug-in fed MIDI into Pro Tools. That
route is abandoned, but most of the musical engine inside it is exactly what
VC Composer 2 needs.

This audit comes from reading the source. The code was **not** built or run in
this session: the environment's safety check blocked executing that
repository without explicit approval. Its README reports 52 suites passing
under `-Werror`, ASan, UBSan and TSan. Re-establish that on import before
relying on it.

## Module by module

| Original module | Size (lines) | What it does | VC Composer 2 |
|---|---|---|---|
| `notation/` | ~6.5k | Score model (staves, grand staff, voices, chords, ties, tuplets incl. nested, stable event IDs); pitch with spelling, keys, clefs; markings; expression maps; score → MIDI renderer (dynamics, articulations, keyswitches, hairpins, transposition, tempo-aware); event diff; MIDI import with voice separation; score file | **Port as-is.** It is the canonical event model Main Spec §8 asks for. Same 2880 PPQ as `vcc_core`. |
| `engraving/` | ~5.8k | Layout engine (clefs, keys, meters, noteheads, accidentals, ledger lines, stems, beams, rests, dots, ties, slurs, dynamics, hairpins, articulations, cross-staff, grace notes), hit testing, SMuFL metadata, SVG renderer, print layout | **Port.** Extend from the timeline-driven strip to page layout and parts (Main Spec §7). |
| `editing/` | ~2.9k | Keyboard/mouse entry, durations, tuplets, accidentals, voices, chords, transpose/respell, articulations, dynamics, slurs, copy/paste, key/meter/clef changes, undo as a journal of inverses | **Port.** Becomes the Composer view's command layer. Its undo keeps note IDs stable, which is better than D6 for note edits. |
| `composer/` | ~3.3k | Shortcut profiles (Finale-style, Sibelius-style, default; platform modifier; remapping), instrument definitions, library catalogue with favorites, session template generator, settings store and migrations | **Partly port.** Port shortcuts and settings migration (review profiles per 01). The instrument catalogue and session templates are superseded by `content/instruments` and `vcc_templates`; merge its favorites concept into `InstalledPatch`. |
| `shared/music` | ~1.5k | MIDI events, Standard MIDI File read/write, musical time | **Port MIDI file reading** (VC Composer 2 only has a writer). Unify time types with `vcc_core`. |
| `shared/files` | ~0.2k | Atomic save | Superseded by `vcc_core/Files`. |
| `shared/json` | ~0.8k | JSON | Superseded by `vcc_core/Json`. |
| `shared/audio` | ~0.6k | Lock-free real-time queues and primitives | **Port** into the realtime engine. |
| `plugins/aax/core` | part of ~2.4k | Host-agnostic sample-accurate MIDI scheduler, transport handling, real-time discipline (allocation-counting test) | **Port the scheduler and its tests** into the realtime engine. Drop the host-context code. |
| `ui/juce` | ~6.7k | Companion window, notation view, renderer, SMuFL font loader, Bravura embedding, headless pixel tests | **Port the notation view, renderer and font embedding** into the Composer workspace. Rework the window shell into the VC Composer 2 layout. |
| `app/` | ~12.5k | Companion window state: per-track documents bound to Pro Tools tracks, view model, settings, recent files | **Mine selectively.** Track binding to Pro Tools goes. The view-model and headless UI-test patterns carry over. |
| `bridge/`, `shared/protocol` | ~6.1k | IPC between the app and the plug-in | **Drop.** VC Composer 2 is one process. Keep the plug-in sandbox/scan isolation ideas for the VST3 scanner. |
| `session-control/` | ~2.9k | Pro Tools Scripting SDK (PTSL) client | **Drop.** |
| `plugins/aax/juce`, AAX docs, signing | — | AAX/VST3/AU wrapper | **Drop** the wrapper. Keep the macOS packaging and notarisation scripts (Main Spec §3 signed installers). |
| `resources/fonts/Bravura` | — | SMuFL font (OFL) | **Port.** |
| `tests/` | ~49k | Unit, fuzz, stress, real-time discipline, host and pixel tests | Port with each module. |

## Order of port

1. `notation` + `shared/music` (time types unified with `vcc_core`). Then attach
   a score part to `project::TrackVersion`, so a track version holds notation
   plus MIDI regions.
2. `editing` → Composer command layer; `engraving` + Bravura.
3. `shared/audio` + scheduler → realtime engine skeleton.
4. `ui/juce` notation view → Composer workspace in the new JUCE shell.
5. Shortcut profiles and settings migration.

## Feature parity checklist (original → VC Composer 2)

- [ ] Notation entry (keyboard, mouse, step-time), voices, chords, grace notes, tuplets incl. nested
- [ ] Engraving: beams, slurs/ties, cross-staff, dynamics/hairpins by voice, print/SVG pages
- [ ] Score → MIDI with expression maps, keyswitches, CC chase on locate
- [ ] MIDI import with voice separation and keyboard split
- [ ] Shortcut profiles (Finale-style, Sibelius-style, default), customizable
- [ ] Instrument transposition and grand staff from the catalog (now `content/instruments`)
- [ ] Library favorites (now via `InstalledPatch.favorite`)
- [ ] Settings persistence and migration
- [ ] Undo/redo preserving note IDs
- [ ] MIDI file export with markers. Done in `vcc_core` (`write_midi_file`)
- [ ] Atomic, crash-safe save. Done in `vcc_core` (`write_file_atomic`)
