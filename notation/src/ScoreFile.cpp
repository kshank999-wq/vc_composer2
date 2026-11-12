#include "vcc/notation/ScoreFile.h"

#include <cstddef>
#include <cmath>
#include <limits>

namespace vcc::notation {
namespace {

// ---------------------------------------------------------------------------
// Writing
// ---------------------------------------------------------------------------

json::Value number(double value)
{
    return json::Value::make_number(value);
}

json::Value integer(std::int64_t value)
{
    return json::Value::make_number(static_cast<double>(value));
}

json::Value text(const std::string& value)
{
    return json::Value::make_string(value);
}

json::Value articulations_to_json(ArticulationMask mask)
{
    json::Value list = json::Value::make_array();
    for (const Articulation articulation : all_articulations()) {
        if (has_articulation(mask, articulation))
            list.push_back(text(to_string(articulation)));
    }
    return list;
}

json::Value note_to_json(const ScoreNote& note)
{
    json::Value out = json::Value::make_object();
    out.set("id", integer(static_cast<std::int64_t>(note.id)));
    out.set("tick", integer(note.start_tick));

    out.set("value", text(to_string(note.duration.value)));
    out.set("dots", integer(note.duration.dots));

    json::Value tuplet = json::Value::make_object();
    tuplet.set("actual", integer(note.duration.tuplet.actual));
    tuplet.set("normal", integer(note.duration.tuplet.normal));
    out.set("tuplet", std::move(tuplet));

    // Only for a tuplet inside another, so a file with none reads exactly
    // as one written before there were any.
    if (note.duration.outer.is_tuplet()) {
        json::Value outer = json::Value::make_object();
        outer.set("actual", integer(note.duration.outer.actual));
        outer.set("normal", integer(note.duration.outer.normal));
        out.set("outer", std::move(outer));
    }

    out.set("rest", json::Value::make_bool(note.is_rest));

    // Written even for a rest, whose pitch the model calls meaningless. It
    // is cheaper to write three fields than to reason later about whether a
    // round trip that dropped them really produced the same score.
    out.set("step", text(to_string(note.pitch.step)));
    out.set("octave", integer(note.pitch.octave));
    out.set("alteration", integer(note.pitch.alteration));

    out.set("voice", integer(note.voice));
    out.set("tied", json::Value::make_bool(note.tied_to_next));
    out.set("chord", json::Value::make_bool(note.in_chord));
    out.set("grace", json::Value::make_bool(note.grace));
    out.set("grace_order", integer(note.grace_order));
    out.set("grace_slashed", json::Value::make_bool(note.grace_slashed));
    out.set("staff_shift", integer(note.staff_shift));
    out.set("articulations", articulations_to_json(note.articulations));
    return out;
}

json::Value staff_to_json(const Staff& staff, const std::vector<ScoreNote>& notes)
{
    json::Value out = json::Value::make_object();
    out.set("clef", text(to_string(staff.clef)));
    out.set("transposition_semitones", integer(staff.transposition_semitones));
    out.set("octave_shift", integer(staff.octave_shift));
    out.set("name", text(staff.name));

    json::Value note_list = json::Value::make_array();
    for (const ScoreNote& note : notes)
        note_list.push_back(note_to_json(note));
    out.set("notes", std::move(note_list));
    return out;
}

// ---------------------------------------------------------------------------
// Reading
// ---------------------------------------------------------------------------

bool read_int(const json::Value& object, const char* key, std::int64_t& out)
{
    const json::Value& field = object[key];
    if (!field.is_number())
        return false;
    out = field.as_int();
    return true;
}

bool read_double(const json::Value& object, const char* key, double& out)
{
    const json::Value& field = object[key];
    if (!field.is_number())
        return false;
    out = field.as_number();
    return true;
}

bool read_string(const json::Value& object, const char* key, std::string& out)
{
    const json::Value& field = object[key];
    if (!field.is_string())
        return false;
    out = field.as_string();
    return true;
}

bool read_bool(const json::Value& object, const char* key, bool& out)
{
    const json::Value& field = object[key];
    if (!field.is_bool())
        return false;
    out = field.as_bool();
    return true;
}

/// What reading one optional field found.
///
/// Three outcomes rather than two, and the third is the whole reason this
/// exists. A field that is **absent** leaves the default standing, which is
/// ordinary: an older file simply did not carry it. A field that is **there
/// and unusable** is a different thing entirely - the file says something
/// this build cannot hold - and letting that leave the default standing puts
/// a plausible value where the composer's own should be, silently.
///
/// Collapsing the two into one `false` is what made this file's behaviour
/// worse the more corrupt its input was. An octave of 12 fits in the field
/// and is refused as out of range, naming the note; an octave of 1000 does
/// not fit, so the read "failed", the default of 4 stood, and the note
/// loaded at the wrong pitch with nothing said at all.
enum class Field {
    Absent,
    Read,
    Unusable,
};

/// Reads a small integer that has to fit, refusing one that does not rather
/// than truncating it into something that looks deliberate.
///
/// The caller decides what a refusal means, and has to look: `Unusable` is
/// never the same answer as `Absent`.
template <typename Integer>
Field read_bounded(const json::Value& object, const char* key, Integer& out)
{
    const json::Value& field = object[key];
    if (field.is_null())
        return Field::Absent;
    if (!field.is_number())
        return Field::Unusable;

    const std::int64_t value = field.as_int();
    if (value < static_cast<std::int64_t>(std::numeric_limits<Integer>::min())
        || value > static_cast<std::int64_t>(std::numeric_limits<Integer>::max())) {
        return Field::Unusable;
    }
    out = static_cast<Integer>(value);
    return Field::Read;
}

std::string at(const char* what, std::size_t index)
{
    return std::string(what) + " " + std::to_string(index + 1);
}

/// Reads one note. Returns false when it is not a note this build can place,
/// and says why in `why`.
bool note_from_json(const json::Value& value, ScoreNote& out, std::string& why,
                    std::vector<std::string>& warnings, const std::string& where)
{
    if (!value.is_object()) {
        why = "it is not an object";
        return false;
    }

    // Required, because defaulting any of these would silently move or
    // renumber music. Everything else takes the model's own default.
    std::int64_t id = 0;
    if (!read_int(value, "id", id) || id <= 0) {
        why = "it has no id";
        return false;
    }
    out.id = static_cast<EventId>(id);

    std::int64_t tick = 0;
    if (!read_int(value, "tick", tick) || tick < 0) {
        why = "it has no position";
        return false;
    }
    out.start_tick = tick;

    std::string value_name;
    if (!read_string(value, "value", value_name)
        || !note_value_from_string(value_name, out.duration.value)) {
        why = value_name.empty() ? "it has no note value"
                                 : "its note value '" + value_name + "' is not one this build "
                                                                     "knows";
        return false;
    }

    // From here on, a field that is there and unusable refuses the note
    // rather than leaving a default in its place. A note whose duration or
    // pitch could not be read is not the note the composer wrote, and one
    // silently rewritten to a plausible value is worse than one the loader
    // says it could not place: the second can be looked at, the first
    // cannot be noticed.
    std::uint8_t dots = 0;
    if (read_bounded(value, "dots", dots) == Field::Unusable) {
        why = "its dot count is not one this build can hold";
        return false;
    }
    out.duration.dots = dots;

    const json::Value& tuplet = value["tuplet"];
    if (tuplet.is_object()) {
        std::uint16_t actual = 1;
        std::uint16_t normal = 1;
        if (read_bounded(tuplet, "actual", actual) == Field::Unusable
            || read_bounded(tuplet, "normal", normal) == Field::Unusable) {
            why = "its tuplet ratio is not one this build can hold";
            return false;
        }
        out.duration.tuplet = Tuplet{actual, normal};
        if (!out.duration.tuplet.is_valid()) {
            why = "its tuplet ratio is not usable";
            return false;
        }
    }

    const json::Value& outer = value["outer"];
    if (outer.is_object()) {
        std::uint16_t actual = 1;
        std::uint16_t normal = 1;
        if (read_bounded(outer, "actual", actual) == Field::Unusable
            || read_bounded(outer, "normal", normal) == Field::Unusable) {
            why = "its outer tuplet ratio is not one this build can hold";
            return false;
        }
        out.duration.outer = Tuplet{actual, normal};
        if (!out.duration.outer.is_valid()) {
            why = "its outer tuplet ratio is not usable";
            return false;
        }
        if (out.duration.outer.is_tuplet() && !out.duration.tuplet.is_tuplet()) {
            why = "its outer tuplet has no tuplet inside it";
            return false;
        }
    }

    read_bool(value, "rest", out.is_rest);

    std::string step_name;
    if (read_string(value, "step", step_name)) {
        if (!step_from_string(step_name, out.pitch.step)) {
            why = "its step '" + step_name + "' is not one this build knows";
            return false;
        }
    }
    if (read_bounded(value, "octave", out.pitch.octave) == Field::Unusable) {
        why = "its octave is not one this build can hold";
        return false;
    }
    if (read_bounded(value, "alteration", out.pitch.alteration) == Field::Unusable) {
        why = "its alteration is not one this build can hold";
        return false;
    }
    if (read_bounded(value, "voice", out.voice) == Field::Unusable) {
        why = "its voice is not one this build can hold";
        return false;
    }
    read_bool(value, "tied", out.tied_to_next);
    read_bool(value, "chord", out.in_chord);
    // Absent in every file written before there were grace notes, which
    // reads as the plain note it was.
    read_bool(value, "grace", out.grace);
    if (read_bounded(value, "grace_order", out.grace_order) == Field::Unusable) {
        why = "its place among the grace notes is not one this build can hold";
        return false;
    }
    read_bool(value, "grace_slashed", out.grace_slashed);
    if (read_bounded(value, "staff_shift", out.staff_shift) == Field::Unusable) {
        why = "its staff shift is not one this build can hold";
        return false;
    }

    const json::Value& articulations = value["articulations"];
    for (std::size_t i = 0; i < articulations.size(); ++i) {
        const json::Value& entry = articulations.at(i);
        Articulation articulation = Articulation::None;
        if (!entry.is_string() || !articulation_from_string(entry.as_string(), articulation)) {
            // Kept rather than refused: a marking this build does not know
            // is one articulation lost, and losing the note as well would
            // be worse. Named, so the composer is not left to notice.
            warnings.push_back(where + " has an articulation this build does not know"
                               + (entry.is_string() ? ": '" + entry.as_string() + "'" : ""));
            continue;
        }
        out.articulations = out.articulations | to_mask(articulation);
    }

    return true;
}

}  // namespace

json::Value score_to_json(const Score& score)
{
    json::Value root = json::Value::make_object();

    json::Value staves = json::Value::make_array();
    for (std::size_t index = 0; index < score.staff_count(); ++index)
        staves.push_back(staff_to_json(score.staff(index), score.notes(index)));
    root.set("staves", std::move(staves));

    json::Value meters = json::Value::make_array();
    for (const TimeSignatureChange& change : score.time_signature_changes()) {
        json::Value entry = json::Value::make_object();
        entry.set("tick", integer(change.tick));
        entry.set("numerator", integer(change.time_signature.numerator));
        entry.set("denominator", integer(change.time_signature.denominator));
        meters.push_back(std::move(entry));
    }
    root.set("time_signatures", std::move(meters));

    json::Value keys = json::Value::make_array();
    for (const KeySignatureChange& change : score.key_signature_changes()) {
        json::Value entry = json::Value::make_object();
        entry.set("tick", integer(change.tick));
        entry.set("fifths", integer(change.key.fifths));
        entry.set("minor", json::Value::make_bool(change.key.is_minor));
        keys.push_back(std::move(entry));
    }
    root.set("key_signatures", std::move(keys));

    json::Value dynamics = json::Value::make_array();
    for (const DynamicMarking& marking : score.dynamics()) {
        json::Value entry = json::Value::make_object();
        entry.set("tick", integer(marking.tick));
        entry.set("level", text(to_string(marking.level)));
        entry.set("voice", integer(marking.voice));
        entry.set("staff", integer(marking.staff));
        entry.set("text", text(marking.text));
        dynamics.push_back(std::move(entry));
    }
    root.set("dynamics", std::move(dynamics));

    json::Value hairpins = json::Value::make_array();
    for (const Hairpin& hairpin : score.hairpins()) {
        json::Value entry = json::Value::make_object();
        entry.set("start_tick", integer(hairpin.start_tick));
        entry.set("end_tick", integer(hairpin.end_tick));
        entry.set("kind", text(to_string(hairpin.kind)));
        entry.set("shape", text(to_string(hairpin.shape)));
        entry.set("voice", integer(hairpin.voice));
        entry.set("staff", integer(hairpin.staff));

        // Written only when set: their absence is what tells the renderer to
        // take the levels from the dynamics around the hairpin, so writing
        // them always would change what the file means.
        if (hairpin.has_start_level)
            entry.set("start_level", text(to_string(hairpin.start_level)));
        if (hairpin.has_end_level)
            entry.set("end_level", text(to_string(hairpin.end_level)));

        hairpins.push_back(std::move(entry));
    }
    root.set("hairpins", std::move(hairpins));

    json::Value slurs = json::Value::make_array();
    for (const Slur& slur : score.slurs()) {
        json::Value entry = json::Value::make_object();
        entry.set("start_tick", integer(slur.start_tick));
        entry.set("end_tick", integer(slur.end_tick));
        entry.set("voice", integer(slur.voice));
        entry.set("staff", integer(slur.staff));
        slurs.push_back(std::move(entry));
    }
    root.set("slurs", std::move(slurs));

    json::Value tempos = json::Value::make_array();
    for (const TempoMarking& marking : score.tempos()) {
        json::Value entry = json::Value::make_object();
        entry.set("tick", integer(marking.tick));
        entry.set("bpm", number(marking.beats_per_minute));
        entry.set("beat_unit", text(to_string(marking.beat_unit)));
        entry.set("beat_unit_dots", integer(marking.beat_unit_dots));
        entry.set("text", text(marking.text));
        tempos.push_back(std::move(entry));
    }
    root.set("tempos", std::move(tempos));

    json::Value texts = json::Value::make_array();
    for (const TextMarking& marking : score.texts()) {
        json::Value entry = json::Value::make_object();
        entry.set("tick", integer(marking.tick));
        entry.set("voice", integer(marking.voice));
        entry.set("text", text(marking.text));
        texts.push_back(std::move(entry));
    }
    root.set("texts", std::move(texts));

    return root;
}

ScoreLoadResult score_from_json(const json::Value& value, Score& out)
{
    ScoreLoadResult result;

    // Reset to a new score first, staves included, and left that way on
    // failure. `clear` alone keeps the staff configuration, which would
    // leave a failed load holding a grand staff with no music on it - not
    // half a cue, but not empty either, and not what a caller was promised.
    out.clear();
    out.reset_staves({Staff{}});

    if (!value.is_object()) {
        result.error = "a score has to be an object";
        return result;
    }

    const json::Value& staves = value["staves"];
    if (!staves.is_array() || staves.size() == 0) {
        result.error = "the file names no staves, so there is no score in it";
        return result;
    }

    // --- Staves and their notes --------------------------------------------
    //
    // Read in two passes. The staves are replaced wholesale first, because a
    // score loaded into one that already had staves would otherwise keep
    // them and grow a staff on every reopen; only then can notes be placed
    // by staff index.
    struct StaffEntry {
        Staff staff;
        const json::Value* notes = nullptr;
    };
    std::vector<StaffEntry> entries;
    entries.reserve(staves.size());

    for (std::size_t index = 0; index < staves.size(); ++index) {
        const json::Value& entry = staves.at(index);
        const std::string where = at("staff", index);

        if (!entry.is_object()) {
            result.warnings.push_back(where + " is not a staff and was skipped");
            continue;
        }

        StaffEntry read;
        std::string clef_name;
        if (read_string(entry, "clef", clef_name)
            && !clef_from_string(clef_name, read.staff.clef)) {
            // The staff is still a staff. Its clef is what this build cannot
            // read, and defaulting it silently would change the notation
            // without saying so.
            result.warnings.push_back(where + " has a clef this build does not know ('"
                                      + clef_name + "'); it reads as treble");
        }
        // Kept rather than refused, because refusing a staff would take
        // every note on it with it - but named, which is the part that was
        // missing. A staff that quietly loses its transposition plays a
        // whole part at the wrong pitch.
        if (read_bounded(entry, "transposition_semitones", read.staff.transposition_semitones)
            == Field::Unusable) {
            result.warnings.push_back(at("staff", index)
                                      + " has a transposition this build cannot hold, so it is"
                                        " read as none");
        }
        if (read_bounded(entry, "octave_shift", read.staff.octave_shift) == Field::Unusable) {
            result.warnings.push_back(at("staff", index)
                                      + " has an octave shift this build cannot hold, so it is"
                                        " read as none");
        }
        read_string(entry, "name", read.staff.name);
        read.notes = &entry["notes"];

        entries.push_back(std::move(read));
    }

    if (entries.empty()) {
        result.error = "none of the staves in the file could be read";
        out.clear();
        return result;
    }

    std::vector<Staff> staff_list;
    staff_list.reserve(entries.size());
    for (const StaffEntry& entry : entries)
        staff_list.push_back(entry.staff);
    out.reset_staves(staff_list);
    result.staves_read = entries.size();

    for (std::size_t index = 0; index < entries.size(); ++index) {
        const json::Value& notes = *entries[index].notes;
        const std::string where = at("staff", index);

        for (std::size_t note_index = 0; note_index < notes.size(); ++note_index) {
            ScoreNote note;
            std::string why;
            const std::string note_where = where + ", " + at("note", note_index);

            if (!note_from_json(notes.at(note_index), note, why, result.warnings,
                                note_where)) {
                result.warnings.push_back(note_where + " was skipped: " + why);
                continue;
            }

            // Restored rather than inserted, so the id it was saved with is
            // the id it has - which is what spec 13's diff layer depends on.
            if (!out.restore_note(index, note)) {
                result.warnings.push_back(note_where
                                          + " was skipped: its id is already in use, or it is "
                                            "not a note this model can hold");
                continue;
            }
            ++result.notes_read;
        }
    }

    // --- Signatures ---------------------------------------------------------
    const json::Value& meters = value["time_signatures"];
    for (std::size_t index = 0; index < meters.size(); ++index) {
        const json::Value& entry = meters.at(index);
        std::int64_t tick = 0;
        std::uint16_t numerator = 0;
        std::uint16_t denominator = 0;
        if (!entry.is_object() || !read_int(entry, "tick", tick)
            || read_bounded(entry, "numerator", numerator) != Field::Read
            || read_bounded(entry, "denominator", denominator) != Field::Read
            || !out.set_time_signature(tick, music::TimeSignature{numerator, denominator})) {
            result.warnings.push_back(at("time signature", index)
                                      + " was skipped: it is not one this model can hold");
            continue;
        }
        ++result.markings_read;
    }

    const json::Value& keys = value["key_signatures"];
    for (std::size_t index = 0; index < keys.size(); ++index) {
        const json::Value& entry = keys.at(index);
        std::int64_t tick = 0;
        KeySignature key;
        if (!entry.is_object() || !read_int(entry, "tick", tick)
            || read_bounded(entry, "fifths", key.fifths) != Field::Read) {
            result.warnings.push_back(at("key signature", index)
                                      + " was skipped: it is not one this model can hold");
            continue;
        }
        read_bool(entry, "minor", key.is_minor);
        if (!out.set_key_signature(tick, key)) {
            result.warnings.push_back(at("key signature", index)
                                      + " was skipped: it is not one this model can hold");
            continue;
        }
        ++result.markings_read;
    }

    // --- Markings -----------------------------------------------------------
    const json::Value& dynamics = value["dynamics"];
    for (std::size_t index = 0; index < dynamics.size(); ++index) {
        const json::Value& entry = dynamics.at(index);
        DynamicMarking marking;
        std::int64_t tick = 0;
        std::string level_name;
        if (!entry.is_object() || !read_int(entry, "tick", tick)
            || !read_string(entry, "level", level_name)
            || !dynamic_from_string(level_name, marking.level)) {
            result.warnings.push_back(at("dynamic", index)
                                      + " was skipped: it is not one this build can read");
            continue;
        }
        marking.tick = tick;
        if (read_bounded(entry, "voice", marking.voice) == Field::Unusable) {
            result.warnings.push_back(at("dynamic", index)
                                      + " has a voice this build cannot hold, so it is read as"
                                        " the first");
        }
        // Absent in every file from before markings had a staff: the first.
        if (read_bounded(entry, "staff", marking.staff) == Field::Unusable) {
            result.warnings.push_back(at("dynamic", index)
                                      + " has a staff this build cannot hold, so it is read as"
                                        " the first");
        }
        read_string(entry, "text", marking.text);
        if (!marking.is_valid()) {
            result.warnings.push_back(at("dynamic", index) + " was skipped: it is not usable");
            continue;
        }
        out.add_dynamic(marking);
        ++result.markings_read;
    }

    const json::Value& hairpins = value["hairpins"];
    for (std::size_t index = 0; index < hairpins.size(); ++index) {
        const json::Value& entry = hairpins.at(index);
        Hairpin hairpin;
        std::int64_t start = 0;
        std::int64_t end = 0;
        std::string kind_name;
        if (!entry.is_object() || !read_int(entry, "start_tick", start)
            || !read_int(entry, "end_tick", end) || !read_string(entry, "kind", kind_name)
            || !hairpin_kind_from_string(kind_name, hairpin.kind)) {
            result.warnings.push_back(at("hairpin", index)
                                      + " was skipped: it is not one this build can read");
            continue;
        }
        hairpin.start_tick = start;
        hairpin.end_tick = end;

        std::string shape_name;
        if (read_string(entry, "shape", shape_name)
            && !curve_shape_from_string(shape_name, hairpin.shape)) {
            result.warnings.push_back(at("hairpin", index)
                                      + " has a curve shape this build does not know ('"
                                      + shape_name + "'); it reads as linear");
        }
        if (read_bounded(entry, "voice", hairpin.voice) == Field::Unusable) {
            result.warnings.push_back(at("hairpin", index)
                                      + " has a voice this build cannot hold, so it is read as"
                                        " the first");
        }

        if (read_bounded(entry, "staff", hairpin.staff) == Field::Unusable) {
            result.warnings.push_back(at("hairpin", index)
                                      + " has a staff this build cannot hold, so it is read as"
                                        " the first");
        }

        std::string level_name;
        if (read_string(entry, "start_level", level_name))
            hairpin.has_start_level = dynamic_from_string(level_name, hairpin.start_level);
        if (read_string(entry, "end_level", level_name))
            hairpin.has_end_level = dynamic_from_string(level_name, hairpin.end_level);

        if (!hairpin.is_valid()) {
            result.warnings.push_back(at("hairpin", index) + " was skipped: it is not usable");
            continue;
        }
        out.add_hairpin(hairpin);
        ++result.markings_read;
    }

    const json::Value& slurs = value["slurs"];
    for (std::size_t index = 0; index < slurs.size(); ++index) {
        const json::Value& entry = slurs.at(index);
        Slur slur;
        std::int64_t start = 0;
        std::int64_t end = 0;
        if (!entry.is_object() || !read_int(entry, "start_tick", start)
            || !read_int(entry, "end_tick", end)) {
            result.warnings.push_back(at("slur", index) + " was skipped: it has no range");
            continue;
        }
        slur.start_tick = start;
        slur.end_tick = end;
        if (read_bounded(entry, "voice", slur.voice) == Field::Unusable) {
            result.warnings.push_back(at("slur", index)
                                      + " has a voice this build cannot hold, so it is read as"
                                        " the first");
        }
        // Absent in files from before slurs had a staff: the first staff,
        // which is the only one they could have meant.
        if (read_bounded(entry, "staff", slur.staff) == Field::Unusable) {
            result.warnings.push_back(at("slur", index)
                                      + " has a staff this build cannot hold, so it is read as"
                                        " the first");
        }
        if (slur.staff >= out.staff_count()) {
            result.warnings.push_back(at("slur", index)
                                      + " names a staff the part does not have, so it is read"
                                        " as the first");
            slur.staff = 0;
        }
        if (!slur.is_valid()) {
            result.warnings.push_back(at("slur", index) + " was skipped: it is not usable");
            continue;
        }
        out.add_slur(slur);
        ++result.markings_read;
    }

    const json::Value& tempos = value["tempos"];
    for (std::size_t index = 0; index < tempos.size(); ++index) {
        const json::Value& entry = tempos.at(index);
        TempoMarking marking;
        std::int64_t tick = 0;
        double bpm = 0.0;
        if (!entry.is_object() || !read_int(entry, "tick", tick)
            || !read_double(entry, "bpm", bpm)) {
            result.warnings.push_back(at("tempo", index) + " was skipped: it has no tempo");
            continue;
        }
        marking.tick = tick;
        marking.beats_per_minute = bpm;

        std::string unit_name;
        if (read_string(entry, "beat_unit", unit_name)
            && !note_value_from_string(unit_name, marking.beat_unit)) {
            result.warnings.push_back(at("tempo", index)
                                      + " has a beat unit this build does not know ('"
                                      + unit_name + "'); it reads as a quarter note");
        }
        if (read_bounded(entry, "beat_unit_dots", marking.beat_unit_dots)
            == Field::Unusable) {
            result.warnings.push_back(at("tempo", index)
                                      + " has a beat unit this build cannot hold, so it is read"
                                        " as undotted");
        }
        read_string(entry, "text", marking.text);

        if (!marking.is_valid()) {
            result.warnings.push_back(at("tempo", index) + " was skipped: it is not usable");
            continue;
        }
        out.add_tempo(marking);
        ++result.markings_read;
    }

    const json::Value& texts = value["texts"];
    for (std::size_t index = 0; index < texts.size(); ++index) {
        const json::Value& entry = texts.at(index);
        TextMarking marking;
        std::int64_t tick = 0;
        if (!entry.is_object() || !read_int(entry, "tick", tick)
            || !read_string(entry, "text", marking.text) || marking.text.empty()) {
            result.warnings.push_back(at("text marking", index)
                                      + " was skipped: it has no text or no position");
            continue;
        }
        marking.tick = tick;
        if (read_bounded(entry, "voice", marking.voice) == Field::Unusable) {
            result.warnings.push_back(at("text marking", index)
                                      + " has a voice this build cannot hold, so it is read as"
                                        " the first");
        }
        out.add_text(marking);
        ++result.markings_read;
    }

    result.ok = true;
    return result;
}

}  // namespace vcc::notation
