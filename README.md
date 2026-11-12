# VC Composer 2

A standalone, notation-first scoring workstation for film, TV and game
composers. You write on staves, hear expressive library playback, edit the
mirrored MIDI, mix through professional routing, score to picture, compare
track and cue versions, and export deliverables, all in one desktop
application for macOS and Windows.

This replaces the original VC Composer, a Pro Tools AAX companion. Its notation
engine is carried forward; the Pro Tools bridge is not
(see [`docs/02-original-vc-composer-reuse-audit.md`](docs/02-original-vc-composer-reuse-audit.md)).

## Specifications

- [`docs/spec/VC_Composer_2_Development_Spec_v1.0.md`](docs/spec/VC_Composer_2_Development_Spec_v1.0.md), the Main Spec
- [`docs/spec/VC_Composer_2_Templates_Supplement_v1.0.md`](docs/spec/VC_Composer_2_Templates_Supplement_v1.0.md), templates, Chord Explorer, Ostinato Creator

## What exists today

The framework-free domain layer, built and tested on every commit:

| Library | Contents |
|---|---|
| `vcc_core` | Ordered JSON; exact musical time (2880 PPQ); meter and tempo maps; film/TV timecode incl. 29.97/59.94 drop frame; crash-safe atomic saves; stable IDs; note events; Standard MIDI File export |
| `vcc_theory` | Spelled pitch; 17 modes; historical tunings and cent-based pitch collections (maqam, raga, gamelan...); 32 chord qualities; Roman numerals with inversions, pedals and applied chords; voicings (closed/open/drop-2/drop-3/spread) with deterministic voice leading; progression catalog and renderer |
| `vcc_ostinato` | Portable seeded generator; A/B/C variants; one-aspect re-rolls with locks; chord-follow; evolving 4-bar segments; call/response; drum mode; exact N bars in any meter |
| `vcc_project` | One track list for all views; instrument/MIDI/audio/aux/reference/video/folder tracks; non-destructive track versions; cue snapshots; buses, sends, inserts; cues; undo/redo per action; replace/merge/new-lane collision policy; versioned, migrating, atomic save/load |
| `vcc_templates` | Instrument registry, template validation and search, library resolver (exact → fallback → labelled placeholder), one-step template instantiation (create/merge/replace), custom templates, pack import/export |

Content in `content/`:

- **203 templates**: 37 cinematic, 33 modern/production-era, 45 regional and
  traditional, 17 historical-era, and 71 composer palettes covering 54 composers.
- **314 instruments**, 193 articulation intents, 29 progressions, and 20
  ostinato presets.

The schema is in [`docs/04-template-content-schema.md`](docs/04-template-content-schema.md).

Not built yet: the desktop UI, the audio engine, VST3 hosting and video. See
[`docs/00-architecture-and-roadmap.md`](docs/00-architecture-and-roadmap.md).

## Build and test

Requires CMake ≥ 3.22 and a C++17 compiler. There are no other dependencies.

```sh
cmake -B build -DVCC_WARNINGS_AS_ERRORS=ON
cmake --build build
ctest --test-dir build --output-on-failure
python3 tools/content/validate_content.py
```

## Try it

```sh
./build/bin/vcc templates horror
./build/bin/vcc template cinematic.horror.psychological
./build/bin/vcc chords pop_uplift --key "G major" --start 5
./build/bin/vcc chords minor_jazz --key "E minor" --style drop2 --midi minor-jazz.mid
./build/bin/vcc ostinato urgent_string_spiccato --key "D minor" --meter 7/8 --progression suspense_pedal --midi ostinato.mid
./build/bin/vcc new cinematic.horror.psychological horror.vcc2.json
./build/bin/vcc export-midi horror.vcc2.json horror.mid
```

The `.mid` files open in any DAW or MIDI player.

## Documents

- [`docs/00-architecture-and-roadmap.md`](docs/00-architecture-and-roadmap.md): layers, rules, test coverage, roadmap
- [`docs/01-decisions-and-license-gates.md`](docs/01-decisions-and-license-gates.md): decisions, open questions, licence gates
- [`docs/02-original-vc-composer-reuse-audit.md`](docs/02-original-vc-composer-reuse-audit.md): what carries over from VC Composer
- [`docs/03-compatibility-matrix.md`](docs/03-compatibility-matrix.md): release-gated OS, driver, plug-in, provider and video matrix
- [`docs/04-template-content-schema.md`](docs/04-template-content-schema.md): content schema
