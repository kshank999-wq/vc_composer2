#include "vcc/notation/MidiImport.h"

#include <algorithm>
#include <cstdlib>
#include <map>
#include <numeric>
#include <utility>

#include "vcc/notation/ExpressionMap.h"

namespace vcc::notation {
namespace {

/// One note, as the file describes it: a sounding pitch over a span, at a
/// velocity.
struct SoundingNote {
    Ticks start = 0;
    Ticks end = 0;
    int note_number = 0;
    std::uint8_t velocity = 0;
};

/// A track's meta events, as they affect the score.
struct ConductorData {
    std::vector<TimeSignatureChange> meters;
    std::vector<KeySignatureChange> keys;
    std::vector<TempoMarking> tempos;

    /// Markers and text events, in file order, with the whitespace round
    /// them gone. Empty ones are left out: a marker with nothing to say is
    /// nothing to write.
    std::vector<TextMarking> texts;
};

std::string trimmed_text(const std::vector<std::uint8_t>& data)
{
    std::string text(data.begin(), data.end());
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && (text[begin] == ' ' || text[begin] == '\t' || text[begin] == '\r'
                           || text[begin] == '\n' || text[begin] == '\0'))
        ++begin;
    while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\r'
                           || text[end - 1] == '\n' || text[end - 1] == '\0'))
        --end;
    return text.substr(begin, end - begin);
}

Ticks nearest_multiple(Ticks tick, Ticks grid) noexcept
{
    if (grid <= 0)
        return tick;
    const Ticks half = grid / 2;
    if (tick >= 0)
        return ((tick + half) / grid) * grid;
    return -(((-tick + half) / grid) * grid);
}

/// Rescales a tick from the file's resolution to the engine's.
///
/// Rounded to nearest rather than truncated: truncation biases every note
/// early, and a whole part shifted a tick earlier than it was played is a
/// systematic error where rounding is a random one.
Ticks rescale(std::int64_t tick, std::uint16_t ticks_per_quarter) noexcept
{
    if (ticks_per_quarter == 0)
        return 0;
    if (ticks_per_quarter == kTicksPerQuarter)
        return tick;

    const std::int64_t numerator = tick * kTicksPerQuarter;
    const std::int64_t denominator = ticks_per_quarter;
    const std::int64_t half = denominator / 2;
    return tick >= 0 ? (numerator + half) / denominator : -((-numerator + half) / denominator);
}

/// Reads the meta events of one track for what they say about the score.
void collect_conductor(const music::SmfTrack& track, std::uint16_t ppq, ConductorData& out)
{
    for (const music::SmfMetaEvent& meta : track.meta) {
        const Ticks tick = rescale(meta.tick, ppq);

        if (meta.type == music::smf_meta::kTimeSignature && meta.data.size() >= 2) {
            const int numerator = meta.data[0];
            const int exponent = meta.data[1];
            if (numerator <= 0 || exponent < 0 || exponent > 7)
                continue;
            const int denominator = 1 << exponent;
            out.meters.push_back(TimeSignatureChange{
                tick, music::TimeSignature{static_cast<std::uint16_t>(numerator),
                                           static_cast<std::uint16_t>(denominator)}});
            continue;
        }

        if (meta.type == music::smf_meta::kKeySignature && meta.data.size() >= 2) {
            KeySignature key;
            key.fifths = static_cast<std::int8_t>(meta.data[0]);
            key.is_minor = meta.data[1] != 0;
            if (key.is_valid())
                out.keys.push_back(KeySignatureChange{tick, key});
            continue;
        }

        if (meta.type == music::smf_meta::kTempo && meta.data.size() >= 3) {
            const std::int64_t microseconds = (static_cast<std::int64_t>(meta.data[0]) << 16)
                                              | (static_cast<std::int64_t>(meta.data[1]) << 8)
                                              | static_cast<std::int64_t>(meta.data[2]);
            if (microseconds <= 0)
                continue;
            TempoMarking marking;
            marking.tick = tick;
            marking.beats_per_minute = 60000000.0 / static_cast<double>(microseconds);
            marking.beat_unit = NoteValue::Quarter;
            if (marking.is_valid())
                out.tempos.push_back(marking);
            continue;
        }

        // Markers, and plain text events, which older sequencers used for
        // the same thing. Not the track name, the copyright or the
        // instrument name: those name the track, not a place in the music.
        if (meta.type == music::smf_meta::kMarker || meta.type == music::smf_meta::kText) {
            TextMarking marking;
            marking.tick = tick;
            marking.text = trimmed_text(meta.data);
            if (marking.is_valid())
                out.texts.push_back(marking);
            continue;
        }
    }
}

/// Pairs note-ons with their releases.
///
/// FIFO per channel and note number, which is what every sequencer does: two
/// overlapping instances of the same pitch are released oldest-first, and
/// treating them any other way would swap their lengths.
std::vector<SoundingNote> pair_notes(const music::SmfTrack& track, std::uint16_t ppq,
                                     std::size_t& unreleased)
{
    struct Open {
        Ticks start = 0;
        std::uint8_t velocity = 0;
    };

    std::map<int, std::vector<Open>> open;  // Keyed by channel * 128 + note.
    std::vector<SoundingNote> notes;
    Ticks last_tick = 0;

    for (const music::SmfEvent& event : track.events) {
        const Ticks tick = rescale(event.tick, ppq);
        last_tick = std::max(last_tick, tick);

        const bool is_on = event.event.type == music::MidiEventType::NoteOn
                           && event.event.data2 > 0;
        const bool is_off = event.event.type == music::MidiEventType::NoteOff
                            || (event.event.type == music::MidiEventType::NoteOn
                                && event.event.data2 == 0);
        if (!is_on && !is_off)
            continue;

        const int key = static_cast<int>(event.event.channel) * 128
                        + static_cast<int>(event.event.data1);

        if (is_on) {
            open[key].push_back(Open{tick, event.event.data2});
            continue;
        }

        std::vector<Open>& queue = open[key];
        if (queue.empty())
            continue;  // A release for something that never sounded.

        SoundingNote note;
        note.start = queue.front().start;
        note.end = tick;
        note.note_number = static_cast<int>(event.event.data1);
        note.velocity = queue.front().velocity;
        queue.erase(queue.begin());
        notes.push_back(note);
    }

    // Anything still held. Closing it at the end of the track is a guess,
    // and counted so it is not a silent one.
    for (auto& entry : open) {
        for (const Open& held : entry.second) {
            SoundingNote note;
            note.start = held.start;
            note.end = std::max(last_tick, held.start + 1);
            note.note_number = entry.first % 128;
            note.velocity = held.velocity;
            notes.push_back(note);
            ++unreleased;
        }
    }

    std::stable_sort(notes.begin(), notes.end(),
                     [](const SoundingNote& a, const SoundingNote& b) {
                         if (a.start != b.start)
                             return a.start < b.start;
                         return a.note_number < b.note_number;
                     });
    return notes;
}

/// Every written length one note can carry, longest first.
///
/// Wider than `decompose_ticks`, which is plain and dotted values only. It
/// has to be: the quantizer's whole point is that it will snap a note to the
/// triplet grid, and a triplet length is not the sum of any plain and dotted
/// values. An eighth-note triplet is a third of a quarter, and a third is a
/// sixteenth plus a 64th plus a 48th that nothing can write - so with only
/// plain values to choose from, every triplet in the file would be dropped
/// as unnotatable, which is the opposite of what `allow_triplets` is for.
///
/// Ordered longest-first, and so that the simplest notation would win any
/// tie: a plain value before a tuplet, fewer dots before more. No two
/// entries are the same length today - the excluded dotted triplets were
/// where the collisions came from - and the ordering is what keeps that from
/// mattering if a value is ever added.
const std::vector<Duration>& notatable_durations()
{
    static const std::vector<Duration> table = [] {
        std::vector<Duration> out;
        for (int index = static_cast<int>(NoteValue::Breve);
             index <= static_cast<int>(NoteValue::SixtyFourth); ++index) {
            for (int dots = 0; dots <= 3; ++dots) {
                const Duration plain{static_cast<NoteValue>(index),
                                     static_cast<std::uint8_t>(dots), Tuplet{1, 1}};
                if (plain.is_valid() && plain.ticks() > 0)
                    out.push_back(plain);

                // Triplets undotted only. One dot on a triplet is exactly
                // the plain value again (three halves of two thirds is
                // one), and two or three dots give 7:6 and 5:4 of it -
                // lengths nobody writes, which would then win over the tie
                // chain a composer expects. A triple-dotted triplet quarter
                // is 1200 ticks; five sixteenths is a quarter tied to a
                // sixteenth.
                if (dots != 0)
                    continue;
                const Duration tripleted{static_cast<NoteValue>(index), 0, Tuplet{3, 2}};
                if (tripleted.is_valid() && tripleted.ticks() > 0)
                    out.push_back(tripleted);
            }
        }
        std::stable_sort(out.begin(), out.end(), [](const Duration& a, const Duration& b) {
            if (a.ticks() != b.ticks())
                return a.ticks() > b.ticks();
            if (a.tuplet.is_tuplet() != b.tuplet.is_tuplet())
                return !a.tuplet.is_tuplet();
            return a.dots < b.dots;
        });
        return out;
    }();
    return table;
}

/// Writes `length` as a tie chain of written values, longest first.
///
/// Returns the number of parts written, and leaves in `unwritten` whatever
/// it could not reach - which is at most a few ticks, and is reported rather
/// than being allowed to lengthen the note to something it never was. Zero
/// parts means the length was shorter than the shortest value notation has.
std::size_t notate_length(Ticks length, Duration* out, std::size_t capacity,
                          Ticks& unwritten)
{
    unwritten = length > 0 ? length : 0;
    if (out == nullptr || capacity == 0 || length <= 0)
        return 0;

    std::size_t written = 0;
    Ticks remaining = length;

    // One pass down the table, taking each value as many times as it fits.
    // The table is sorted longest-first, so this is the same greedy choice
    // decompose_ticks makes, and it is how a tie chain is conventionally
    // notated.
    for (const Duration& candidate : notatable_durations()) {
        const Ticks ticks = candidate.ticks();
        while (ticks <= remaining && written < capacity) {
            out[written++] = candidate;
            remaining -= ticks;
        }
        if (remaining == 0 || written >= capacity)
            break;
    }

    unwritten = remaining;
    return written;
}

Clef clef_for(const std::vector<SoundingNote>& notes) noexcept
{
    if (notes.empty())
        return Clef::Treble;

    std::vector<int> pitches;
    pitches.reserve(notes.size());
    for (const SoundingNote& note : notes)
        pitches.push_back(note.note_number);
    std::sort(pitches.begin(), pitches.end());

    // The median rather than the mean: one very low pedal note should not
    // move a treble part onto a bass staff.
    const int median = pitches[pitches.size() / 2];

    // Middle C is the conventional dividing line, and it is where a part
    // that straddles it reads about equally either way.
    return median < 60 ? Clef::Bass : Clef::Treble;
}

std::string track_name_for(const music::SmfTrack& track, std::size_t index)
{
    if (!track.name.empty())
        return track.name;

    // Never blank: an unnamed part is one the composer cannot pick out of a
    // list.
    return "Part " + std::to_string(index + 1);
}

}  // namespace

Ticks grid_ticks(NoteValue value) noexcept
{
    // Range-checked rather than handed straight to base_ticks, which
    // answers for its last enumerator by falling through its switch. That
    // is right for base_ticks and wrong here: a value cast in from outside
    // the enum - a number out of a settings file, say - would come back as
    // a 64th note grid instead of as "not a note value", and a bad setting
    // would quantize a whole part to a grid nobody chose.
    const auto index = static_cast<int>(value);
    if (index < static_cast<int>(NoteValue::Breve)
        || index > static_cast<int>(NoteValue::SixtyFourth))
        return 0;
    return base_ticks(value);
}

DynamicLevel dynamic_for_velocity(std::uint8_t velocity) noexcept
{
    const ExpressionMap defaults;
    std::size_t nearest = 0;
    int nearest_distance = 128;
    for (std::size_t index = 0; index < kDynamicLevelCount; ++index) {
        const int distance = std::abs(static_cast<int>(velocity)
                                      - static_cast<int>(defaults.dynamic_velocity[index]));
        // Strictly nearer wins, so a velocity midway between two levels
        // stays with the quieter one it met first.
        if (distance < nearest_distance) {
            nearest_distance = distance;
            nearest = index;
        }
    }
    return static_cast<DynamicLevel>(nearest);
}

namespace {

/// Writes the dynamics the velocities imply, from every part together.
///
/// An onset is the loudest note starting at a quantized tick - a chord's
/// level is its top. A level is written where it *holds*: at the first of
/// two onsets in a row at that level, with a lone onset at another level
/// between them - an accent, a brushed key - writing nothing. The opening
/// level, whatever it is, goes at the first onset, so the part starts with
/// a dynamic the way a written part does; a file with one onset in it
/// takes that onset's level.
std::size_t write_dynamics(const std::map<Ticks, std::uint8_t>& onsets, Score& out)
{
    if (onsets.empty())
        return 0;

    struct Onset {
        Ticks tick;
        DynamicLevel level;
    };
    std::vector<Onset> levels;
    levels.reserve(onsets.size());
    for (const auto& entry : onsets)
        levels.push_back(Onset{entry.first, dynamic_for_velocity(entry.second)});

    std::size_t written = 0;
    bool have_written = false;
    DynamicLevel current = DynamicLevel::MezzoForte;

    for (std::size_t index = 0; index < levels.size(); ++index) {
        const DynamicLevel level = levels[index].level;
        if (have_written && level == current)
            continue;

        const bool holds = index + 1 < levels.size() && levels[index + 1].level == level;
        const bool alone = levels.size() == 1;
        if (!holds && !alone)
            continue;

        DynamicMarking marking;
        // The first mark goes at the first onset even when the first onset
        // itself was the odd one out: a part opens with its dynamic.
        marking.tick = have_written ? levels[index].tick : levels.front().tick;
        marking.level = level;
        out.add_dynamic(marking);
        ++written;
        have_written = true;
        current = level;
    }
    return written;
}

/// One sound on a staff as the voice allocation sees it: where it begins
/// and ends after quantization, how high it is, and the voice it is given.
struct Sound {
    Ticks start = 0;
    Ticks end = 0;
    int note_number = 0;
    std::uint8_t voice = 0;
};

constexpr std::size_t kVoices = 4;

/// Gives each sound a voice, where the file forces one.
///
/// Sounds that begin and end together are one unit - a chord, in one
/// voice. Two units conflict when they sound together, since one line
/// cannot begin a note while another of its own still sounds. The units are
/// coloured in order of onset, each taking the first colour no earlier
/// conflicting unit holds, which on intervals is the fewest colours there
/// are; units that conflict with nothing stay uncoloured and go in the
/// first voice. Within each passage of connected conflicts the colours are
/// then numbered by pitch, highest first, so the top line takes the
/// up-stem first voice and the one under it the down-stem second - over the
/// passage as a whole, so lines that cross keep their voices through the
/// crossing. A passage that needs more than four lines puts the rest in
/// the fourth, and they are counted so it is said.
std::size_t assign_voices(std::vector<Sound>& sounds)
{
    struct Unit {
        Ticks start = 0;
        Ticks end = 0;
        double pitch = 0.0;
        std::vector<std::size_t> members;
        int colour = -1;
        std::size_t passage = 0;
    };

    std::map<std::pair<Ticks, Ticks>, std::size_t> unit_of;
    std::vector<Unit> units;
    for (std::size_t index = 0; index < sounds.size(); ++index) {
        const Sound& sound = sounds[index];
        const auto key = std::make_pair(sound.start, sound.end);
        const auto found = unit_of.find(key);
        if (found == unit_of.end()) {
            unit_of.emplace(key, units.size());
            Unit unit;
            unit.start = sound.start;
            unit.end = sound.end;
            units.push_back(unit);
        }
        Unit& unit = units[unit_of[key]];
        unit.members.push_back(index);
    }
    for (Unit& unit : units) {
        double total = 0.0;
        for (const std::size_t member : unit.members)
            total += static_cast<double>(sounds[member].note_number);
        unit.pitch = total / static_cast<double>(unit.members.size());
    }

    // The map ordered the units by onset, which is the order they are
    // coloured in; which colour a line gets does not matter yet, since
    // the colours are numbered by pitch below.

    // Passages: connected components of the conflicts, found with a
    // union-find over the units.
    std::vector<std::size_t> parent(units.size());
    std::iota(parent.begin(), parent.end(), std::size_t{0});
    const auto root = [&](std::size_t unit) {
        while (parent[unit] != unit) {
            parent[unit] = parent[parent[unit]];
            unit = parent[unit];
        }
        return unit;
    };

    std::size_t overflowed = 0;
    for (std::size_t position = 0; position < units.size(); ++position) {
        Unit& unit = units[position];
        bool used[kVoices] = {false, false, false, false};
        bool conflicts = false;
        for (std::size_t earlier = 0; earlier < position; ++earlier) {
            Unit& other = units[earlier];
            if (other.start < unit.end && unit.start < other.end) {
                conflicts = true;
                // An earlier unit that conflicted with nothing before this
                // one opens the passage: it takes the first colour, which
                // nothing it sounds with can hold, since anything that
                // did would have coloured it then.
                if (other.colour < 0)
                    other.colour = 0;
                used[other.colour] = true;
                parent[root(position)] = root(earlier);
            }
        }
        if (!conflicts)
            continue;
        int colour = 0;
        while (static_cast<std::size_t>(colour) < kVoices && used[colour])
            ++colour;
        if (static_cast<std::size_t>(colour) >= kVoices) {
            colour = static_cast<int>(kVoices) - 1;
            overflowed += unit.members.size();
        }
        unit.colour = colour;
    }

    // A unit still uncoloured conflicted with nothing before or after it:
    // it is in no passage at all, and goes in the first voice.
    for (Unit& unit : units) {
        if (unit.colour < 0) {
            for (const std::size_t member : unit.members)
                sounds[member].voice = 0;
        }
    }

    // Number each passage's colours by pitch, highest first.
    std::map<std::size_t, std::vector<Unit*>> passages;
    for (std::size_t index = 0; index < units.size(); ++index) {
        if (units[index].colour >= 0)
            passages[root(index)].push_back(&units[index]);
    }
    for (auto& entry : passages) {
        double total[kVoices] = {0.0, 0.0, 0.0, 0.0};
        std::size_t count[kVoices] = {0, 0, 0, 0};
        for (const Unit* unit : entry.second) {
            total[unit->colour] += unit->pitch * static_cast<double>(unit->members.size());
            count[unit->colour] += unit->members.size();
        }
        std::vector<int> colours;
        for (std::size_t colour = 0; colour < kVoices; ++colour) {
            if (count[colour] > 0)
                colours.push_back(static_cast<int>(colour));
        }
        std::stable_sort(colours.begin(), colours.end(), [&](int a, int b) {
            return total[a] / static_cast<double>(count[a])
                   > total[b] / static_cast<double>(count[b]);
        });
        std::uint8_t voice_for[kVoices] = {0, 0, 0, 0};
        for (std::size_t rank = 0; rank < colours.size(); ++rank)
            voice_for[colours[rank]] = static_cast<std::uint8_t>(rank);
        for (Unit* unit : entry.second) {
            for (const std::size_t member : unit->members)
                sounds[member].voice = voice_for[unit->colour];
        }
    }

    return overflowed;
}

}  // namespace

Ticks quantize_tick(Ticks tick, Ticks grid, bool allow_triplets) noexcept
{
    if (grid <= 0)
        return tick;

    const Ticks straight = nearest_multiple(tick, grid);
    if (!allow_triplets)
        return straight;

    // Two notes in the time of three of the grid's value, which is what a
    // triplet at this level divides into. The nearer candidate wins, so a
    // straight passage stays straight and a triplet one comes out as
    // triplets.
    const Ticks triplet_grid = (grid * 2) / 3;
    if (triplet_grid <= 0)
        return straight;

    const Ticks tripleted = nearest_multiple(tick, triplet_grid);
    const Ticks straight_distance = std::abs(tick - straight);
    const Ticks triplet_distance = std::abs(tick - tripleted);

    // A tie goes to the straight grid: it is the simpler notation, and the
    // one a composer expects when nothing favours the other.
    return triplet_distance < straight_distance ? tripleted : straight;
}

MidiImportResult import_midi(const music::SmfFile& file, Score& out,
                             const MidiImportOptions& options)
{
    MidiImportResult result;

    out.clear();
    out.reset_staves({Staff{}});

    if (file.ticks_per_quarter == 0) {
        result.error = "that file says there are no ticks in a quarter note";
        return result;
    }
    if (file.tracks.empty()) {
        result.error = "there are no tracks in that file";
        return result;
    }

    const Ticks grid = grid_ticks(options.grid);
    if (grid <= 0) {
        result.error = "the quantization grid is not a note value";
        return result;
    }

    // --- What the file says about the score itself -------------------------
    //
    // Read from every track, not only the first. That is where the
    // convention puts them, but a file is not obliged to follow it and one
    // that does not still has a tempo.
    ConductorData conductor;
    for (const music::SmfTrack& track : file.tracks)
        collect_conductor(track, file.ticks_per_quarter, conductor);

    // --- The notes ---------------------------------------------------------
    struct ImportedTrack {
        std::string name;
        Clef clef = Clef::Treble;
        std::vector<SoundingNote> notes;
    };
    std::vector<ImportedTrack> tracks;

    for (std::size_t index = 0; index < file.tracks.size(); ++index) {
        const music::SmfTrack& track = file.tracks[index];
        std::vector<SoundingNote> notes =
            pair_notes(track, file.ticks_per_quarter, result.unreleased_notes);

        if (notes.empty()) {
            // The conductor track, or an empty one. Its meta events have
            // already been read.
            ++result.tracks_without_notes;
            continue;
        }

        const std::string name = track_name_for(track, index);

        // A keyboard track, split at the point the composer chose: the
        // notes from it up on one staff and the rest on the staff below,
        // both named for the track, each clefed by its own notes. Only
        // where there are notes on both sides - a part that lies wholly
        // above or below the point is one staff.
        if (options.split_point >= 0) {
            std::vector<SoundingNote> upper;
            std::vector<SoundingNote> lower;
            for (const SoundingNote& note : notes)
                (note.note_number >= options.split_point ? upper : lower).push_back(note);
            if (!upper.empty() && !lower.empty()) {
                ImportedTrack top;
                top.name = name;
                top.clef = options.guess_clefs ? clef_for(upper) : Clef::Treble;
                top.notes = std::move(upper);
                tracks.push_back(std::move(top));
                ImportedTrack bottom;
                bottom.name = name;
                bottom.clef = options.guess_clefs ? clef_for(lower) : Clef::Treble;
                bottom.notes = std::move(lower);
                tracks.push_back(std::move(bottom));
                ++result.tracks_split;
                continue;
            }
        }

        ImportedTrack imported;
        imported.name = name;
        imported.clef = options.guess_clefs ? clef_for(notes) : Clef::Treble;
        imported.notes = std::move(notes);
        tracks.push_back(std::move(imported));
    }

    if (tracks.empty()) {
        result.error = "there are no notes in that file";
        out.clear();
        out.reset_staves({Staff{}});
        return result;
    }

    std::vector<Staff> staves;
    staves.reserve(tracks.size());
    for (const ImportedTrack& track : tracks) {
        Staff staff;
        staff.name = track.name;
        staff.clef = track.clef;
        staves.push_back(staff);
    }
    out.reset_staves(staves);
    result.staves_read = staves.size();

    // Signatures before the notes, so a note is spelled against the key that
    // is actually in force where it falls.
    for (const TimeSignatureChange& change : conductor.meters) {
        if (!out.set_time_signature(change.tick, change.time_signature)) {
            result.warnings.push_back(
                "a " + std::to_string(change.time_signature.numerator) + "/"
                + std::to_string(change.time_signature.denominator)
                + " meter in the file does not fall on a bar line and was left out");
        }
    }
    for (const KeySignatureChange& change : conductor.keys)
        out.set_key_signature(change.tick, change.key);
    if (conductor.keys.empty() && options.default_key.is_valid())
        out.set_key_signature(0, options.default_key);
    for (const TempoMarking& marking : conductor.tempos)
        out.add_tempo(marking);

    if (options.read_markers) {
        // Once each: a file that carries the same marker on two tracks -
        // one per part, as some exporters write them - is one marker.
        for (const TextMarking& marking : conductor.texts) {
            bool already = false;
            for (const TextMarking& written : out.texts()) {
                if (written.tick == marking.tick && written.text == marking.text) {
                    already = true;
                    break;
                }
            }
            if (already)
                continue;
            out.add_text(marking);
            ++result.markers_read;
        }
    }

    // --- Quantize, spell, and write ----------------------------------------
    std::size_t shortened = 0;

    // Every onset's loudest velocity, across the parts: what the dynamics
    // are read from, once the notes are in.
    std::map<Ticks, std::uint8_t> onset_velocities;

    std::size_t overflowed_voices = 0;

    for (std::size_t staff_index = 0; staff_index < tracks.size(); ++staff_index) {
        const std::vector<SoundingNote>& notes = tracks[staff_index].notes;

        // Each sound's written span and the tie chain that writes it, kept
        // apart until the voices are decided: a chain is one sound, and
        // every part of it goes in the sound's voice.
        std::vector<Sound> sounds;
        std::vector<std::vector<ScoreNote>> chains;

        for (const SoundingNote& sounding : notes) {
            const Ticks start = quantize_tick(sounding.start, grid, options.allow_triplets);
            result.largest_move = std::max(result.largest_move,
                                           std::abs(sounding.start - start));

            std::uint8_t& loudest = onset_velocities[start];
            loudest = std::max(loudest, sounding.velocity);

            Ticks length = sounding.end - sounding.start;
            if (options.quantize_lengths) {
                const Ticks quantized_end =
                    quantize_tick(sounding.end, grid, options.allow_triplets);
                length = quantized_end - start;
                // Counted apart from the onset move: a release that moved
                // is normal (nothing writes note-offs at the written
                // length), an onset that moved is a rhythm that was
                // interpreted.
                result.largest_length_move = std::max(
                    result.largest_length_move, std::abs(sounding.end - quantized_end));
            }

            const Ticks floor = options.drop_shorter_than > 0 ? options.drop_shorter_than : 1;
            if (length < floor) {
                // A stuck key or a graced release, not notation.
                ++result.notes_dropped;
                continue;
            }

            Duration chain[8];
            Ticks unwritten = 0;
            const std::size_t parts =
                notate_length(length, chain, sizeof(chain) / sizeof(chain[0]), unwritten);
            if (parts == 0) {
                // Shorter than the shortest written value there is, which
                // after the floor check above means a handful of ticks.
                ++result.notes_dropped;
                continue;
            }
            if (unwritten > 0) {
                // Written shorter than it sounded, by less than the
                // shortest value notation has. Counted, because a note
                // that came back at a different length is exactly the kind
                // of quiet change this import exists to report.
                ++shortened;
                result.largest_length_move = std::max(result.largest_length_move, unwritten);
            }

            const Pitch pitch =
                spell_midi_note(sounding.note_number, out.key_signature_at(start));

            std::vector<ScoreNote> written;
            Ticks at = start;
            for (std::size_t part = 0; part < parts; ++part) {
                ScoreNote note;
                note.start_tick = at;
                note.duration = chain[part];
                note.pitch = pitch;
                // Tied to what follows, for every part but the last: a tie
                // chain sounds as one note, which is what the file held.
                note.tied_to_next = part + 1 < parts;
                written.push_back(note);
                at += chain[part].ticks();
            }

            Sound sound;
            sound.start = start;
            sound.end = at;
            sound.note_number = sounding.note_number;
            sounds.push_back(sound);
            chains.push_back(std::move(written));
        }

        // The voices, where the file forces them - decided on the written
        // spans, so an overlap the grid removed is not a voice.
        if (options.infer_voices)
            overflowed_voices += assign_voices(sounds);

        // Grouped by quantized onset and voice, so simultaneous notes can
        // be marked as a chord - which the file does not say and notation
        // needs - and the parts of a tie chain are considered for chording
        // alongside anything else that begins at the same tick in the
        // same voice.
        std::map<std::pair<Ticks, std::uint8_t>, std::vector<ScoreNote>> by_onset;
        for (std::size_t index = 0; index < sounds.size(); ++index) {
            for (ScoreNote& note : chains[index]) {
                note.voice = sounds[index].voice;
                by_onset[std::make_pair(note.start_tick, note.voice)].push_back(note);
            }
        }

        for (auto& entry : by_onset) {
            std::vector<ScoreNote>& at_this_onset = entry.second;

            // Two or more notes starting together in one voice are a
            // chord. Nothing in the file says so; notation has to.
            const bool is_chord = at_this_onset.size() > 1;
            for (ScoreNote& note : at_this_onset) {
                note.in_chord = is_chord;
                if (out.insert_note(staff_index, note) != kInvalidEventId) {
                    ++result.notes_read;
                    if (note.voice != 0)
                        ++result.notes_in_other_voices;
                } else {
                    ++result.notes_dropped;
                }
            }
        }
    }

    if (options.read_dynamics) {
        result.dynamics_read = write_dynamics(onset_velocities, out);
        // The staves of one split track are one part, so its dynamics were
        // read from one performance; the warning is for several.
        if (result.dynamics_read > 0 && tracks.size() - result.tracks_split > 1) {
            // A dynamic is score-wide, so with several parts it was read
            // from all of them together - which is right for a tutti and
            // wrong for a solo over an accompaniment, and the composer is
            // the one who knows which this is.
            result.warnings.push_back("the dynamics were read from the velocities of all "
                                      + std::to_string(tracks.size() - result.tracks_split)
                                      + " parts together, so each applies to every part");
        }
    }

    if (overflowed_voices > 0) {
        // Written rather than dropped, since the notes were played; but a
        // fourth voice holding two lines is not notation, and the composer
        // has to be told to look.
        result.warnings.push_back(
            std::to_string(overflowed_voices)
            + (overflowed_voices == 1 ? " note sounded" : " notes sounded")
            + " over more lines than four voices hold and "
            + (overflowed_voices == 1 ? "was" : "were") + " written in the fourth");
    }
    if (result.notes_dropped > 0) {
        result.warnings.push_back(
            std::to_string(result.notes_dropped)
            + (result.notes_dropped == 1 ? " note was" : " notes were")
            + " too short to notate and were left out");
    }
    if (shortened > 0) {
        result.warnings.push_back(
            std::to_string(shortened) + (shortened == 1 ? " note was" : " notes were")
            + " written slightly shorter than played, to a length notation has");
    }
    if (result.unreleased_notes > 0) {
        result.warnings.push_back(
            std::to_string(result.unreleased_notes)
            + (result.unreleased_notes == 1 ? " note was" : " notes were")
            + " never released in the file, and end where their track does");
    }
    // The furthest the quantizer could ever move an onset. With triplets
    // allowed the candidates are the multiples of the grid *and* of two
    // thirds of it, which interleave: their widest gap is two thirds of the
    // grid, so nothing can move by more than a third of it. Without
    // triplets the candidates are a grid unit apart, and the worst case is
    // half of that.
    //
    // The threshold is half the worst case, either way. Comparing against
    // the whole of it would put the warning on the one tick exactly midway
    // between two candidates, so it would almost never fire - and with
    // triplets allowed it could not fire at all, a third of the grid being
    // less than half of it.
    const Ticks worst_case = options.allow_triplets ? grid / 3 : grid / 2;
    if (result.largest_move > 0 && result.largest_move * 2 >= worst_case) {
        // The number that says whether the rhythm survived: a note moved
        // by half of what the grid could do was as near the wrong candidate
        // as the right one, and the composer should look.
        result.warnings.push_back("the rhythm was moved by up to "
                                  + std::to_string(result.largest_move)
                                  + " ticks to fit the grid");
    }

    result.ok = true;
    return result;
}

}  // namespace vcc::notation
