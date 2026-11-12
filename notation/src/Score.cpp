#include "vcc/notation/Score.h"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace vcc::notation {
namespace {

/// Sorts by start tick, then by sounding pitch, so a chord reads bottom to
/// top and rendering can walk the vector in musical order.
bool note_order(const ScoreNote& a, const ScoreNote& b) noexcept
{
    if (a.start_tick != b.start_tick)
        return a.start_tick < b.start_tick;
    if (a.voice != b.voice)
        return a.voice < b.voice;
    // Grace notes before the note they ornament, in their own order.
    if (a.grace != b.grace)
        return a.grace;
    if (a.grace && a.grace_order != b.grace_order)
        return a.grace_order < b.grace_order;
    if (a.is_rest != b.is_rest)
        return b.is_rest;  // Notes before rests at the same position.
    return a.pitch < b.pitch;
}

}  // namespace

bool ScoreNote::is_valid() const noexcept
{
    if (start_tick < 0)
        return false;
    if (!duration.is_valid() || duration.ticks() <= 0)
        return false;
    if (!is_rest && !pitch.is_valid())
        return false;
    // A rest cannot be tied or belong to a chord; both would be nonsense
    // that the renderer would have to guess about.
    // A grace note takes no time, so a rest or a tie would be nothing.
    if (grace && (is_rest || tied_to_next))
        return false;
    // One staff either way: a grand staff has two.
    if (staff_shift < -1 || staff_shift > 1)
        return false;
    if (is_rest && (tied_to_next || in_chord))
        return false;
    return true;
}

Ticks measure_length_ticks(const music::TimeSignature& time_signature) noexcept
{
    if (!time_signature.is_valid())
        return kTicksPerWhole;  // Fall back to 4/4 rather than zero-length bars.
    // A 6/8 bar is six eighth notes: 6 * (whole / 8).
    return static_cast<Ticks>(time_signature.numerator) * kTicksPerWhole
         / static_cast<Ticks>(time_signature.denominator);
}

Score::Score()
{
    // Every score has at least one staff and a signature in force from the
    // start, so callers never have to check for their absence.
    staves_.push_back(Staff{});
    staff_notes_.emplace_back();
    longest_note_ticks_.push_back(0);
    time_signatures_.push_back(TimeSignatureChange{0, music::TimeSignature{4, 4}});
    key_signatures_.push_back(KeySignatureChange{0, KeySignature{}});
}

Staff& Score::staff(std::size_t index)
{
    if (index >= staves_.size())
        throw std::out_of_range("Score::staff: index out of range");
    return staves_[index];
}

const Staff& Score::staff(std::size_t index) const
{
    if (index >= staves_.size())
        throw std::out_of_range("Score::staff: index out of range");
    return staves_[index];
}

bool Score::set_staff(std::size_t index, const Staff& new_staff)
{
    if (index >= staves_.size())
        return false;
    staves_[index] = new_staff;
    touch();
    return true;
}

std::size_t Score::add_staff(const Staff& new_staff)
{
    staves_.push_back(new_staff);
    staff_notes_.emplace_back();
    longest_note_ticks_.push_back(0);
    touch();
    return staves_.size() - 1;
}

void Score::make_grand_staff()
{
    staves_.resize(1);
    staff_notes_.resize(1);
    longest_note_ticks_.resize(1);

    staves_[0].clef = Clef::Treble;
    staves_[0].name = "Right hand";

    Staff bass;
    bass.clef = Clef::Bass;
    bass.name = "Left hand";
    staves_.push_back(bass);
    staff_notes_.emplace_back();
    longest_note_ticks_.push_back(0);
    touch();
}

bool Score::set_time_signature(Ticks tick, const music::TimeSignature& time_signature)
{
    if (tick < 0 || !time_signature.is_valid())
        return false;

    // A meter change has to land on a barline. Allowing one mid-measure
    // would leave the bar structure permanently out of step with the host
    // timeline, which acceptance criterion 3 forbids.
    if (tick > 0) {
        const Measure containing = measure_at(tick);
        if (containing.start_tick != tick)
            return false;
    }

    const auto existing = std::find_if(
        time_signatures_.begin(), time_signatures_.end(),
        [tick](const TimeSignatureChange& change) { return change.tick == tick; });

    if (existing != time_signatures_.end()) {
        existing->time_signature = time_signature;
    } else {
        time_signatures_.push_back(TimeSignatureChange{tick, time_signature});
        std::sort(time_signatures_.begin(), time_signatures_.end(),
                  [](const TimeSignatureChange& a, const TimeSignatureChange& b) {
                      return a.tick < b.tick;
                  });
    }
    touch();
    return true;
}

bool Score::set_key_signature(Ticks tick, const KeySignature& key)
{
    if (tick < 0 || !key.is_valid())
        return false;

    const auto existing =
        std::find_if(key_signatures_.begin(), key_signatures_.end(),
                     [tick](const KeySignatureChange& change) { return change.tick == tick; });

    if (existing != key_signatures_.end()) {
        existing->key = key;
    } else {
        key_signatures_.push_back(KeySignatureChange{tick, key});
        std::sort(key_signatures_.begin(), key_signatures_.end(),
                  [](const KeySignatureChange& a, const KeySignatureChange& b) {
                      return a.tick < b.tick;
                  });
    }
    touch();
    return true;
}

music::TimeSignature Score::time_signature_at(Ticks tick) const noexcept
{
    music::TimeSignature result{4, 4};
    for (const TimeSignatureChange& change : time_signatures_) {
        if (change.tick <= tick)
            result = change.time_signature;
        else
            break;
    }
    return result;
}

KeySignature Score::key_signature_at(Ticks tick) const noexcept
{
    KeySignature result{};
    for (const KeySignatureChange& change : key_signatures_) {
        if (change.tick <= tick)
            result = change.key;
        else
            break;
    }
    return result;
}

Measure Score::measure_at(Ticks tick) const noexcept
{
    if (tick < 0)
        tick = 0;

    // Walk measure by measure through each meter span. Scores are thousands
    // of bars at most, and doing it this way means the bar numbering cannot
    // disagree with the time-signature list.
    Ticks cursor = 0;
    std::int32_t number = 1;

    for (std::size_t index = 0; index < time_signatures_.size(); ++index) {
        const music::TimeSignature& meter = time_signatures_[index].time_signature;
        const Ticks length = measure_length_ticks(meter);
        const Ticks span_end = (index + 1 < time_signatures_.size())
                                   ? time_signatures_[index + 1].tick
                                   : std::numeric_limits<Ticks>::max();

        while (cursor < span_end) {
            if (tick < cursor + length || length <= 0) {
                Measure result;
                result.number = number;
                result.start_tick = cursor;
                result.length_ticks = length;
                result.time_signature = meter;
                result.key = key_signature_at(cursor);
                return result;
            }
            cursor += length;
            ++number;

            // Guard against a pathological meter producing an endless loop.
            if (length <= 0)
                break;
        }
    }

    Measure fallback;
    fallback.number = number;
    fallback.start_tick = cursor;
    fallback.length_ticks = kTicksPerWhole;
    return fallback;
}

Measure Score::measure(std::int32_t number) const noexcept
{
    if (number < 1)
        number = 1;
    return measure_at(measure_start_tick(number));
}

Ticks Score::measure_start_tick(std::int32_t number) const noexcept
{
    if (number <= 1)
        return 0;

    Ticks cursor = 0;
    std::int32_t current = 1;

    for (std::size_t index = 0; index < time_signatures_.size(); ++index) {
        const Ticks length = measure_length_ticks(time_signatures_[index].time_signature);
        const Ticks span_end = (index + 1 < time_signatures_.size())
                                   ? time_signatures_[index + 1].tick
                                   : std::numeric_limits<Ticks>::max();
        if (length <= 0)
            break;

        while (cursor < span_end) {
            if (current == number)
                return cursor;
            cursor += length;
            ++current;
        }
    }

    // Past the last meter change: extend with the final meter.
    const Ticks length = measure_length_ticks(time_signatures_.back().time_signature);
    return cursor + static_cast<Ticks>(number - current) * length;
}

BarBeat Score::to_bar_beat(Ticks tick) const noexcept
{
    const Measure containing = measure_at(tick);
    const music::TimeSignature& meter = containing.time_signature;

    // One beat is a note of the denominator's value.
    const Ticks beat_ticks = kTicksPerWhole / static_cast<Ticks>(meter.denominator);

    BarBeat result;
    result.measure = containing.number;
    result.beat = beat_ticks > 0
                      ? 1.0 + static_cast<double>(tick - containing.start_tick)
                                  / static_cast<double>(beat_ticks)
                      : 1.0;
    return result;
}

Ticks Score::from_bar_beat(const BarBeat& position) const noexcept
{
    const Ticks measure_start = measure_start_tick(position.measure);
    const music::TimeSignature meter = time_signature_at(measure_start);
    const Ticks beat_ticks = kTicksPerWhole / static_cast<Ticks>(meter.denominator);
    const double beats_in = position.beat - 1.0;
    return measure_start + static_cast<Ticks>(beats_in * static_cast<double>(beat_ticks) + 0.5);
}

const std::vector<ScoreNote>& Score::notes(std::size_t staff_index) const
{
    if (staff_index >= staff_notes_.size())
        throw std::out_of_range("Score::notes: staff index out of range");
    return staff_notes_[staff_index];
}

void Score::note_reaches(std::size_t staff_index, const ScoreNote& note)
{
    if (staff_index >= longest_note_ticks_.size())
        longest_note_ticks_.resize(staff_index + 1, 0);

    const Ticks sounding = note.duration.ticks();
    if (sounding > longest_note_ticks_[staff_index])
        longest_note_ticks_[staff_index] = sounding;
}

void Score::sort_staff(std::size_t staff_index)
{
    std::sort(staff_notes_[staff_index].begin(), staff_notes_[staff_index].end(), note_order);
}

EventId Score::insert_note(std::size_t staff_index, const ScoreNote& note)
{
    if (staff_index >= staff_notes_.size())
        return kInvalidEventId;
    if (!note.is_valid())
        return kInvalidEventId;

    ScoreNote stored = note;
    stored.id = next_id();
    staff_notes_[staff_index].push_back(stored);
    note_reaches(staff_index, stored);
    sort_staff(staff_index);
    touch();
    return stored.id;
}

std::vector<EventId> Score::insert_chord(std::size_t staff_index, Ticks start_tick,
                                         const Duration& duration,
                                         const std::vector<Pitch>& pitches, std::uint8_t voice)
{
    std::vector<EventId> ids;
    if (staff_index >= staff_notes_.size() || pitches.empty())
        return ids;

    ids.reserve(pitches.size());
    for (const Pitch& pitch : pitches) {
        ScoreNote note;
        note.start_tick = start_tick;
        note.duration = duration;
        note.pitch = pitch;
        note.voice = voice;
        // More than one pitch makes it a chord; a single pitch is just a note.
        note.in_chord = pitches.size() > 1;

        const EventId id = insert_note(staff_index, note);
        if (id == kInvalidEventId) {
            // Roll back, so a chord with one bad pitch does not leave a
            // partial chord in the score.
            for (const EventId inserted : ids)
                remove_note(inserted);
            return {};
        }
        ids.push_back(id);
    }
    return ids;
}

bool Score::remove_note(EventId id)
{
    if (id == kInvalidEventId)
        return false;

    for (std::vector<ScoreNote>& notes_on_staff : staff_notes_) {
        const auto found = std::find_if(notes_on_staff.begin(), notes_on_staff.end(),
                                        [id](const ScoreNote& note) { return note.id == id; });
        if (found != notes_on_staff.end()) {
            notes_on_staff.erase(found);
            touch();
            return true;
        }
    }
    return false;
}

bool Score::restore_note(std::size_t staff_index, const ScoreNote& note)
{
    if (staff_index >= staff_notes_.size())
        return false;
    if (note.id == kInvalidEventId || !note.is_valid())
        return false;
    if (find_note(note.id) != nullptr)
        return false;

    staff_notes_[staff_index].push_back(note);
    note_reaches(staff_index, note);
    sort_staff(staff_index);

    // Keep the counter ahead of every id in the score, so a later insert
    // cannot hand out an id this note already holds.
    if (note.id > last_event_id_)
        last_event_id_ = note.id;

    touch();
    return true;
}

bool Score::modify_note(EventId id, const ScoreNote& replacement)
{
    if (id == kInvalidEventId || !replacement.is_valid())
        return false;

    for (std::size_t staff_index = 0; staff_index < staff_notes_.size(); ++staff_index) {
        std::vector<ScoreNote>& notes_on_staff = staff_notes_[staff_index];
        const auto found = std::find_if(notes_on_staff.begin(), notes_on_staff.end(),
                                        [id](const ScoreNote& note) { return note.id == id; });
        if (found != notes_on_staff.end()) {
            ScoreNote updated = replacement;
            // The id is the identity, not part of the content: keeping it
            // is what lets the diff layer report a modification rather than
            // a delete followed by an add (spec 13).
            updated.id = id;
            *found = updated;
            note_reaches(staff_index, updated);
            sort_staff(staff_index);
            touch();
            return true;
        }
    }
    return false;
}

const ScoreNote* Score::find_note(EventId id) const noexcept
{
    if (id == kInvalidEventId)
        return nullptr;

    for (const std::vector<ScoreNote>& notes_on_staff : staff_notes_) {
        for (const ScoreNote& note : notes_on_staff) {
            if (note.id == id)
                return &note;
        }
    }
    return nullptr;
}

std::size_t Score::staff_index_of(EventId id) const noexcept
{
    for (std::size_t staff_index = 0; staff_index < staff_notes_.size(); ++staff_index) {
        for (const ScoreNote& note : staff_notes_[staff_index]) {
            if (note.id == id)
                return staff_index;
        }
    }
    return std::numeric_limits<std::size_t>::max();
}

std::vector<ScoreNote> Score::notes_in_range(std::size_t staff_index, Ticks from, Ticks to) const
{
    std::vector<ScoreNote> result;
    if (staff_index >= staff_notes_.size() || to <= from)
        return result;

    const std::vector<ScoreNote>& notes = staff_notes_[staff_index];

    // Where the scan can start. The notes are sorted by start tick, so the
    // earliest that could still be sounding at `from` is one starting
    // `longest` before it - and nothing earlier needs looking at.
    //
    // This used to walk the whole staff, which made a four-bar window on a
    // long cue cost what the cue cost rather than what the window did. The
    // header promised the opposite, and the view lays out on every playhead
    // move, so the promise was the one worth keeping.
    const Ticks longest = staff_index < longest_note_ticks_.size()
                            ? longest_note_ticks_[staff_index]
                            : Ticks{0};

    // Clamped at zero. `Ticks` is signed today, so `from - longest` at the
    // start of a score merely goes negative and `lower_bound` answers
    // `begin()`, which is right - this is not guarding against a wrap that
    // can currently happen. It is here so that the answer stays right if
    // `Ticks` ever becomes unsigned, where a negative would wrap to an
    // enormous positive and the search would start past the end of the
    // staff and find nothing at all.
    const Ticks earliest = from > longest ? from - longest : Ticks{0};

    const auto first = std::lower_bound(
        notes.begin(), notes.end(), earliest,
        [](const ScoreNote& note, Ticks tick) { return note.start_tick < tick; });

    for (auto it = first; it != notes.end(); ++it) {
        // Sorted by start tick, so once one starts at or after the end of
        // the range, so does every note after it.
        if (it->start_tick >= to)
            break;

        // Overlap, not containment: a note starting before the range and
        // sounding into it is in the range. A grace note sounds for no
        // time, so it is in the range its tick is.
        if (it->end_tick() > from || (it->grace && it->start_tick >= from))
            result.push_back(*it);
    }
    return result;
}

void Score::add_dynamic(const DynamicMarking& marking)
{
    if (!marking.is_valid())
        return;

    // One dynamic per position per voice per staff: a second one at the
    // same tick replaces the first rather than leaving the renderer to
    // pick.
    const auto existing = std::find_if(
        dynamics_.begin(), dynamics_.end(), [&marking](const DynamicMarking& other) {
            return other.tick == marking.tick && other.voice == marking.voice
                && other.staff == marking.staff;
        });

    if (existing != dynamics_.end()) {
        *existing = marking;
    } else {
        dynamics_.push_back(marking);
        std::sort(dynamics_.begin(), dynamics_.end(),
                  [](const DynamicMarking& a, const DynamicMarking& b) {
                      return a.tick < b.tick;
                  });
    }
    touch();
}

void Score::add_hairpin(const Hairpin& hairpin)
{
    if (!hairpin.is_valid())
        return;
    hairpins_.push_back(hairpin);
    std::sort(hairpins_.begin(), hairpins_.end(),
              [](const Hairpin& a, const Hairpin& b) { return a.start_tick < b.start_tick; });
    touch();
}

void Score::add_slur(const Slur& slur)
{
    if (!slur.is_valid())
        return;
    slurs_.push_back(slur);
    touch();
}

void Score::add_tempo(const TempoMarking& marking)
{
    if (!marking.is_valid())
        return;

    const auto existing =
        std::find_if(tempos_.begin(), tempos_.end(), [&marking](const TempoMarking& other) {
            return other.tick == marking.tick;
        });

    if (existing != tempos_.end()) {
        *existing = marking;
    } else {
        tempos_.push_back(marking);
        std::sort(tempos_.begin(), tempos_.end(),
                  [](const TempoMarking& a, const TempoMarking& b) { return a.tick < b.tick; });
    }
    touch();
}

void Score::add_text(const TextMarking& marking)
{
    if (!marking.is_valid())
        return;
    texts_.push_back(marking);
    touch();
}

DynamicLevel Score::dynamic_at(Ticks tick, std::uint8_t voice, std::size_t staff) const noexcept
{
    // Mezzo-forte when nothing has been written: a score with no dynamics
    // still has to play, and silence is not a sensible default.
    DynamicLevel any = DynamicLevel::MezzoForte;
    DynamicLevel own = DynamicLevel::MezzoForte;
    bool has_own = false;

    for (const DynamicMarking& marking : dynamics_) {
        if (marking.tick > tick)
            break;
        // Voice 0 applies to the whole staff; a voice-specific marking wins
        // for that voice.
        if (marking.voice != 0 && marking.voice != voice)
            continue;
        any = marking.level;
        if (marking.staff == staff) {
            own = marking.level;
            has_own = true;
        }
    }
    // The staff's own marking governs it; a staff with none follows the
    // others', as a piano's two hands follow one marking between them.
    return has_own ? own : any;
}

const Hairpin* Score::hairpin_at(Ticks tick, std::uint8_t voice, std::size_t staff) const noexcept
{
    const Hairpin* any = nullptr;
    for (const Hairpin& hairpin : hairpins_) {
        if (hairpin.start_tick > tick)
            break;
        if (tick >= hairpin.end_tick || (hairpin.voice != 0 && hairpin.voice != voice))
            continue;
        if (hairpin.staff == staff)
            return &hairpin;
        if (any == nullptr)
            any = &hairpin;
    }
    return any;
}

bool Score::staff_has_dynamics(std::size_t staff, Ticks tick) const noexcept
{
    for (const DynamicMarking& marking : dynamics_) {
        if (marking.tick > tick)
            break;
        if (marking.staff == staff)
            return true;
    }
    for (const Hairpin& hairpin : hairpins_) {
        if (hairpin.start_tick > tick)
            break;
        if (hairpin.staff == staff)
            return true;
    }
    return false;
}

double Score::tempo_at(Ticks tick) const noexcept
{
    double result = 120.0;  // The conventional default when none is written.
    for (const TempoMarking& marking : tempos_) {
        if (marking.tick > tick)
            break;
        result = marking.quarter_notes_per_minute();
    }
    return result;
}

bool Score::remove_dynamic(Ticks tick, std::uint8_t voice, std::uint8_t staff)
{
    const auto found = std::find_if(dynamics_.begin(), dynamics_.end(),
                                    [tick, voice, staff](const DynamicMarking& marking) {
                                        return marking.tick == tick && marking.voice == voice
                                            && marking.staff == staff;
                                    });
    if (found == dynamics_.end())
        return false;
    dynamics_.erase(found);
    touch();
    return true;
}

bool Score::remove_hairpin(Ticks start_tick, std::uint8_t voice, std::uint8_t staff)
{
    const auto found = std::find_if(hairpins_.begin(), hairpins_.end(),
                                    [start_tick, voice, staff](const Hairpin& hairpin) {
                                        return hairpin.start_tick == start_tick
                                            && hairpin.voice == voice && hairpin.staff == staff;
                                    });
    if (found == hairpins_.end())
        return false;
    hairpins_.erase(found);
    touch();
    return true;
}

bool Score::remove_slur(const Slur& wanted)
{
    const auto found = std::find_if(slurs_.begin(), slurs_.end(), [&wanted](const Slur& slur) {
        return slur.start_tick == wanted.start_tick && slur.end_tick == wanted.end_tick
            && slur.voice == wanted.voice && slur.staff == wanted.staff;
    });
    if (found == slurs_.end())
        return false;
    slurs_.erase(found);
    touch();
    return true;
}

bool Score::remove_text(Ticks tick, std::uint8_t voice)
{
    const auto found = std::find_if(texts_.begin(), texts_.end(),
                                    [tick, voice](const TextMarking& marking) {
                                        return marking.tick == tick && marking.voice == voice;
                                    });
    if (found == texts_.end())
        return false;
    texts_.erase(found);
    touch();
    return true;
}

bool Score::remove_time_signature(Ticks tick)
{
    if (tick <= 0)
        return false;  // The signature in force from the start always exists.

    const auto found = std::find_if(
        time_signatures_.begin(), time_signatures_.end(),
        [tick](const TimeSignatureChange& change) { return change.tick == tick; });
    if (found == time_signatures_.end())
        return false;
    time_signatures_.erase(found);
    touch();
    return true;
}

bool Score::remove_key_signature(Ticks tick)
{
    if (tick <= 0)
        return false;

    const auto found = std::find_if(
        key_signatures_.begin(), key_signatures_.end(),
        [tick](const KeySignatureChange& change) { return change.tick == tick; });
    if (found == key_signatures_.end())
        return false;
    key_signatures_.erase(found);
    touch();
    return true;
}

Ticks Score::end_tick() const noexcept
{
    Ticks last = 0;
    for (const std::vector<ScoreNote>& notes_on_staff : staff_notes_) {
        for (const ScoreNote& note : notes_on_staff)
            last = std::max(last, note.end_tick());
    }
    for (const Hairpin& hairpin : hairpins_)
        last = std::max(last, hairpin.end_tick);
    for (const DynamicMarking& marking : dynamics_)
        last = std::max(last, marking.tick);
    for (const TempoMarking& marking : tempos_)
        last = std::max(last, marking.tick);
    return last;
}

std::size_t Score::total_note_count() const noexcept
{
    std::size_t total = 0;
    for (const std::vector<ScoreNote>& notes_on_staff : staff_notes_)
        total += notes_on_staff.size();
    return total;
}

bool Score::reset_staves(const std::vector<Staff>& staves)
{
    if (staves.empty())
        return false;

    staves_ = staves;
    staff_notes_.assign(staves_.size(), {});
    touch();
    return true;
}

bool Score::remove_staff(std::size_t index)
{
    if (index >= staves_.size() || staves_.size() == 1 || !staff_notes_[index].empty())
        return false;

    staves_.erase(staves_.begin() + static_cast<std::ptrdiff_t>(index));
    staff_notes_.erase(staff_notes_.begin() + static_cast<std::ptrdiff_t>(index));
    longest_note_ticks_.erase(longest_note_ticks_.begin() + static_cast<std::ptrdiff_t>(index));

    for (auto it = slurs_.begin(); it != slurs_.end();) {
        if (it->staff == index) {
            it = slurs_.erase(it);
            continue;
        }
        if (it->staff > index)
            --it->staff;
        ++it;
    }

    touch();
    return true;
}

void Score::clear()
{
    for (std::vector<ScoreNote>& notes_on_staff : staff_notes_)
        notes_on_staff.clear();
    dynamics_.clear();
    hairpins_.clear();
    slurs_.clear();
    tempos_.clear();
    texts_.clear();

    time_signatures_.clear();
    time_signatures_.push_back(TimeSignatureChange{0, music::TimeSignature{4, 4}});
    key_signatures_.clear();
    key_signatures_.push_back(KeySignatureChange{0, KeySignature{}});

    // Ids are deliberately not reset: a score that is cleared and refilled
    // must not reuse an id the host still associates with a deleted note.
    touch();
}

}  // namespace vcc::notation
