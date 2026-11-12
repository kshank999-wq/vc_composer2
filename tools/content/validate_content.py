#!/usr/bin/env python3
"""Fast structural validator for content/ (schema_version 1).

The authoritative validator is the C++ one in templates/ (run by ctest); this
script gives content authors the same rules without a build. Exit code 1 on any
error. Warnings (unknown articulation intents, etc.) do not fail.

usage: validate_content.py [content_dir] [--quiet]
"""
import json, re, sys, pathlib

FAMILIES = {"strings","woodwinds","brass","percussion","keys","synth","traditional","rhythm","vocals"}
CLEFS = {"treble","bass","alto","tenor","treble_8vb","treble_8va","bass_8vb","percussion"}
CATEGORIES = {"cinematic","regional","era","composer","modern"}
VARIANTS = {"acoustic","hybrid","minimal","large_orchestra","chamber","traditional","fusion","cinematic","period","band","electronic","solo"}
ROLES = {"melody","countermelody","harmony","bass","pad","texture","ostinato","rhythm","percussion","drone","fx","solo","continuo","vocal"}
MODES = {"major","minor","harmonic_minor","melodic_minor","dorian","phrygian","lydian","mixolydian","aeolian","locrian",
         "phrygian_dominant","double_harmonic","whole_tone","octatonic","major_pentatonic","minor_pentatonic","blues","custom"}
TONIC = re.compile(r"^[A-G](#|b)?$")
METER = re.compile(r"^([1-9]|1[0-9]|2[0-9])/(1|2|4|8|16)$")
TRACK_ID = re.compile(r"^[a-z][a-z0-9_]*$")
TEMPLATE_ID = re.compile(r"^[a-z][a-z0-9_]*(\.[a-z0-9_]+)+$")

errors, warnings = [], []
def err(where, msg): errors.append(f"ERROR {where}: {msg}")
def warn(where, msg): warnings.append(f"warn  {where}: {msg}")

def load(path):
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except Exception as e:
        err(str(path), f"invalid JSON: {e}"); return None

def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    quiet = "--quiet" in sys.argv
    root = pathlib.Path(args[0] if args else pathlib.Path(__file__).resolve().parents[2] / "content")

    instruments = {}
    for p in sorted((root/"instruments").glob("*.json")):
        d = load(p)
        if not d: continue
        for i in d.get("instruments", []):
            w = f"{p.name}:{i.get('id')}"
            if i.get("id") in instruments: err(w, "duplicate instrument id")
            for f in ("id","name","short_name","family","range"):
                if f not in i: err(w, f"missing {f}")
            if i.get("family") not in FAMILIES: err(w, f"bad family {i.get('family')}")
            if i.get("clef","treble") not in CLEFS: err(w, f"bad clef {i.get('clef')}")
            if i.get("lower_clef","bass") not in CLEFS: err(w, f"bad lower_clef")
            r = i.get("range", [0, 0])
            if not (isinstance(r, list) and len(r) == 2 and 0 <= r[0] <= r[1] <= 127): err(w, f"bad range {r}")
            gm = i.get("gm_program", 0)
            if not (-1 <= gm <= 127): err(w, "bad gm_program")
            if gm == -1 and not i.get("unpitched"): err(w, "gm_program -1 requires unpitched: true")
            instruments[i.get("id")] = i

    artic = {a["id"] for a in (load(root/"articulations.json") or {}).get("articulations", [])}
    progs = {p["id"] for p in (load(root/"progressions.json") or {}).get("progressions", [])} if (root/"progressions.json").exists() else None
    osts = {p["id"] for p in (load(root/"ostinato_presets.json") or {}).get("presets", [])} if (root/"ostinato_presets.json").exists() else None

    template_ids = set(); count = 0
    for p in sorted((root/"templates").glob("*.json")):
        d = load(p)
        if not d: continue
        if d.get("schema_version") != 1: err(p.name, "schema_version must be 1")
        for t in d.get("templates", []):
            count += 1
            tid = t.get("template_id", "?"); w = f"{p.name}:{tid}"
            if not TEMPLATE_ID.match(tid): err(w, "template_id must be dotted lower-case")
            if tid in template_ids: err(w, "duplicate template_id")
            template_ids.add(tid)
            for f in ("schema_version","content_version","display_name","category","subcategory","tags","variant","description","tempo_range_bpm","meter_options","license_metadata"):
                if f not in t: err(w, f"missing {f}")
            if t.get("category") not in CATEGORIES: err(w, f"bad category {t.get('category')}")
            if t.get("variant") not in VARIANTS: err(w, f"bad variant {t.get('variant')}")
            tr = t.get("tempo_range_bpm", [0, 0])
            if not (isinstance(tr, list) and len(tr) == 2 and 20 <= tr[0] <= tr[1] <= 320): err(w, f"bad tempo_range_bpm {tr}")
            for m in t.get("meter_options", []):
                if not METER.match(m): err(w, f"bad meter {m}")
            if not t.get("meter_options"): err(w, "meter_options empty")
            for km in t.get("recommended_key_modes", []):
                if not TONIC.match(km.get("tonic", "")): err(w, f"bad tonic {km}")
                if km.get("mode") not in MODES: err(w, f"bad mode {km}")
            if (t.get("cultural_context") or t.get("composer")) and not t.get("authenticity_note"):
                err(w, "authenticity_note required with cultural_context/composer")
            status = t.get("status", "available")
            if status not in ("available", "curation_required"): err(w, "bad status")
            tracks = t.get("tracks", [])
            if status == "available" and not tracks: err(w, "no tracks")
            bus_ids = {b.get("id") for b in t.get("buses", [])}
            for b in t.get("buses", []):
                if b.get("kind", "group") not in ("group", "fx", "master"): err(w, f"bad bus kind {b}")
            tids = set()
            for k in tracks:
                kw = f"{w}/{k.get('id')}"
                for f in ("id","name","instrument_standard_id","role"):
                    if f not in k: err(kw, f"missing {f}")
                if not TRACK_ID.match(k.get("id", "")): err(kw, "track id must be snake_case")
                if k.get("id") in tids: err(kw, "duplicate track id")
                tids.add(k.get("id"))
                if k.get("instrument_standard_id") not in instruments: err(kw, f"unknown instrument {k.get('instrument_standard_id')}")
                for fb in k.get("fallback_instruments", []):
                    if fb not in instruments: err(kw, f"unknown fallback instrument {fb}")
                if k.get("role") not in ROLES: err(kw, f"bad role {k.get('role')}")
                for a in k.get("articulation_map", []):
                    if a not in artic: warn(kw, f"articulation '{a}' not in vocabulary")
                ob = k.get("output_bus_id")
                if ob and ob not in bus_ids: err(kw, f"output_bus_id {ob} not declared in buses")
                gp = k.get("default_gain_pan", {})
                if not (-1.0 <= gp.get("pan", 0) <= 1.0): err(kw, "pan out of range")
                pol = k.get("midi_channel_policy", "auto")
                if not (pol in ("auto", "drum") or re.match(r"^fixed:([1-9]|1[0-6])$", pol)): err(kw, f"bad midi_channel_policy {pol}")
                for req in k.get("plugin_requirements", []):
                    if req not in ("sampler", "synth", "drum_sampler", "granular", "rompler", "organ_model", "piano_model"):
                        warn(kw, f"plugin requirement '{req}' is not a generic capability")
            if progs is not None:
                for pid in t.get("progression_ids", []):
                    if pid not in progs: err(w, f"unknown progression {pid}")
            if osts is not None:
                for oid in t.get("ostinato_preset_ids", []):
                    if oid not in osts: err(w, f"unknown ostinato preset {oid}")

    out = errors + ([] if quiet else warnings)
    for line in out: print(line)
    print(f"{len(instruments)} instruments, {len(artic)} articulations, {count} templates; {len(errors)} errors, {len(warnings)} warnings")
    return 1 if errors else 0

if __name__ == "__main__":
    sys.exit(main())
