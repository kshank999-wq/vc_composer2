# Template content schema (schema_version 1)

Templates Supplement §8 asks for shipped presets as versioned, declarative
JSON validated against a schema, with no vendor plug-ins baked in. This file
is that schema. `vcc_templates` (`templates/src/TemplateSchema.cpp`) enforces
it, and `tests/unit/test_content_packs.cpp` loads and instantiates every shipped
template on every build.

## Files

```
content/
  instruments/*.json      Standard instrument registry, merged from every file
  articulations.json      Controlled articulation-intent vocabulary
  progressions.json       Chord Explorer progression catalog
  ostinato_presets.json   Ostinato Creator style presets
  templates/*.json        Template packs (each file is one pack)
```

A content file never names a sample library, a plug-in product or a patch file.
A template asks for *roles* (an instrument standard ID plus articulation
intents). `InstrumentResolver` turns those into whatever the composer has
installed, falls back to an approved alternative, or leaves a labelled
placeholder.

## Instrument registry: `content/instruments/<pack>.json`

```json
{
  "schema_version": 1,
  "pack": "core",
  "instruments": [
    {
      "id": "strings.violin",
      "name": "Violin",
      "short_name": "Vln.",
      "family": "strings",
      "clef": "treble",
      "grand_staff": false,
      "lower_clef": "bass",
      "transposition": 0,
      "range": [55, 103],
      "score_order": 600,
      "gm_program": 40,
      "unpitched": false,
      "tags": ["bowed", "orchestral"],
      "region": "",
      "note": ""
    }
  ]
}
```

| Field | Rules |
|---|---|
| `id` | Required, unique across all packs. Write it as `<family-ish prefix>.<snake_name>`. Never rename an ID once it ships. |
| `name`, `short_name` | Required display strings. |
| `family` | Required. One of `strings`, `woodwinds`, `brass`, `percussion`, `keys`, `synth`, `traditional`, `rhythm`, `vocals`. This sets the generated folder (Supplement §8 order). |
| `clef` | `treble`, `bass`, `alto`, `tenor`, `treble_8vb`, `treble_8va`, `bass_8vb`, `percussion`. Default `treble`. |
| `grand_staff`, `lower_clef` | Optional; for piano, harp, organ and similar. |
| `transposition` | Written → sounding semitones (Bb clarinet = -2, horn in F = -7, double bass = -12, piccolo = +12). Default 0. Octave clefs add their own octave: an instrument reading `treble_8vb` (guitar, tenor voice) keeps `transposition` 0 and sounds an octave lower through the clef. |
| `range` | Sounding MIDI range `[low, high]`, 0–127. Required. |
| `score_order` | Integer; lower values print higher in the score. Default 1000. |
| `gm_program` | 0–127 General MIDI program used for the built-in placeholder and preview sound. Use -1 with `unpitched: true` for GM drum-channel instruments. |
| `unpitched` | True for percussion that uses drum mapping instead of pitched keys. |
| `region`, `note` | Optional context, e.g. "Ireland", "historical reconstruction". |

## Articulation intents: `content/articulations.json`

Each template track's `articulation_map` lists **intent IDs** from this
vocabulary, such as `spiccato` or `sul_ponticello`. An intent is not a keyswitch.
The composer's mapping pack later binds intents to keyswitches, CCs, channels
or program changes for a particular library (Main Spec §11). An unknown ID is a
validation **warning**: the track keeps the intent as a free-text technique so
nothing is lost.

## Template pack: `content/templates/<pack>.json`

```json
{
  "schema_version": 1,
  "pack": "cinematic",
  "content_version": "1.0.0",
  "templates": [ { ...template... } ]
}
```

### Template

| Field | Type | Req | Notes |
|---|---|---|---|
| `template_id` | string | ✔ | Unique, dotted: `cinematic.horror.psychological`, `regional.ireland.traditional`, `composer.bach.concerto`. |
| `schema_version` | int | ✔ | `1`. |
| `content_version` | string | ✔ | semver of this template. |
| `display_name` | string | ✔ | "Horror — Psychological". |
| `category` | string | ✔ | `cinematic`, `regional`, `era`, `composer`, `modern`. |
| `subcategory` | string | ✔ | Free grouping key: `horror`, `jazz`, `west_africa`, `baroque`. |
| `tags` | string[] | ✔ | Lower-case search terms: moods, styles, regions, ensemble words. |
| `locale` | string | | Default `en`. |
| `variant` | string | ✔ | `acoustic`, `hybrid`, `minimal`, `large_orchestra`, `chamber`, `traditional`, `fusion`, `cinematic`, `period`, `band`, `electronic`, `solo`. |
| `description` | string | ✔ | One to three sentences for the preview pane. |
| `treatment` | string | | Articulation and treatment notes (the third column of the Supplement's tables). |
| `ensemble_size` | string | | `solo`, `small`, `medium`, `large`. Derived from the track count when absent. |
| `historical_period` | object | | `{ "label": "Late Baroque", "start_year": 1700, "end_year": 1750 }` |
| `composer` | object | | `{ "id": "bach_js", "name": "Johann Sebastian Bach", "born": 1685, "died": 1750, "genres": ["cantata","concerto"], "example_works": ["Brandenburg Concertos"] }` |
| `cultural_context` | object | | `{ "region": "Ireland", "subregion": "", "tradition": "Irish traditional dance music", "instrument_provenance": "...", "tuning_assumptions": "...", "cinematic_approximation": false, "review_status": "unreviewed", "reviewer": "", "sources": [] }` |
| `authenticity_note` | string | | Required when `cultural_context` or `composer` is present. States what the palette is and is not. |
| `recommended_key_modes` | object[] | | `[{ "tonic": "D", "mode": "minor" }]`. Modes: `major`, `minor`, `harmonic_minor`, `melodic_minor`, `dorian`, `phrygian`, `lydian`, `mixolydian`, `aeolian`, `locrian`, `phrygian_dominant`, `double_harmonic`, `whole_tone`, `octatonic`, `major_pentatonic`, `minor_pentatonic`, `blues`, `custom`. |
| `tempo_range_bpm` | [int,int] | ✔ | |
| `meter_options` | string[] | ✔ | `["4/4","6/8"]`, the first is the default. |
| `default_length_bars` | int | | Default 32. |
| `tracks` | object[] | ✔ | At least one, unless `status` is `curation_required`. |
| `buses` | object[] | | When omitted, one bus per family in use plus `Reverb` is generated. |
| `progression_ids` | string[] | | IDs in `content/progressions.json`. |
| `ostinato_preset_ids` | string[] | | IDs in `content/ostinato_presets.json`. |
| `license_metadata` | object | ✔ | `{ "content_license": "VC original", "author": "VC Composer content", "notes": "" }` |
| `status` | string | | `available` (default) or `curation_required`. A `curation_required` template is listed and searchable but cannot be instantiated. Use it for packs that need community-approved content before shipping (Supplement §5, North American Indigenous). |

### Track

| Field | Req | Notes |
|---|---|---|
| `id` | ✔ | Unique within the template; snake_case. |
| `name` | ✔ | Display/track name: "Low Strings — Sul Pont.". |
| `instrument_standard_id` | ✔ | A registry `id`. |
| `role` | ✔ | `melody`, `countermelody`, `harmony`, `bass`, `pad`, `texture`, `ostinato`, `rhythm`, `percussion`, `drone`, `fx`, `solo`, `continuo`, `vocal`. |
| `articulation_map` | | Intent IDs; the first is the default articulation. |
| `fallback_instruments` | | Ordered registry IDs to try when no patch maps the primary instrument. |
| `plugin_requirements` | | Generic capability strings only (`"sampler"`, `"synth"`, `"drum_sampler"`, `"granular"`). Never a product name. |
| `group_id` | | Folder override; defaults to the instrument family. |
| `output_bus_id` | | Bus `id`; defaults to the family bus. |
| `default_gain_pan` | | `{ "gain_db": 0.0, "pan": 0.0 }`, pan from -1 to 1. |
| `color` | | `#rrggbb`; defaults to the family colour. |
| `midi_channel_policy` | | `auto` (default), `fixed:<1-16>`, `drum` (channel 10). |
| `optional` | | When true, the preview marks it optional and Create asks before including it. Default false. |

### Bus

`{ "id": "bus_strings", "name": "Strings", "kind": "group" | "fx" | "master",
"output": "master", "sends": [{"to":"fx_hall","level_db":-12}],
"insert_suggestions": ["eq","compressor"] }`.
Insert suggestions are generic processor kinds. They become real inserts only
when the composer has a matching plug-in (Supplement §8, Routing).
