#include "vcc/notation/MidiRenderer.h"

#include <algorithm>
#include <tuple>
#include <set>
#include <cmath>

namespace vcc::notation {

const char* to_string(EventRole role) noexcept
{
    switch (role) {
        case EventRole::Note:       return "note";
        case EventRole::Keyswitch:  return "keyswitch";
        case EventRole::Controller: break;
    }
    return "controller";
}
namespace {

std::uint8_t clamp_midi_value(double value) noexcept
{
    if (!(value > 0.0))
        return 0;
    if (value >= 127.0)
        return 127;
    return static_cast<std::uint8_t>(value + 0.5);
}

/// Velocity is clamped to 1, never 0: a velocity-0 note-on is a note-off in
/// MIDI, so a heavily scaled-down pianissimo must not become silence.
std::uint8_t clamp_velocity(double value) noexcept
{
    if (value < 1.0)
        return 1;
    if (value >= 127.0)
        return 127;
    return static_cast<std::uint8_t>(value + 0.5);
}

/// Send order within a single tick. Keyswitches and controllers have to
/// arrive before the note they apply to, or the instrument plays the note
/// with the previous articulation and level.
int send_priority(const music::MidiEvent& event) noexcept
{
    switch (event.type) {
        case music::MidiEventType::NoteOff:       return 0;  // Release first.
        case music::MidiEventType::ControlChange: return 1;
        case music::MidiEventType::ProgramChange: return 1;
        case music::MidiEventType::PitchBend:     return 1;
        case music::MidiEventType::NoteOn:        return 3;
        default:                                  return 2;
    }
}

}  // namespace

void RenderResult::clear()
{
    events.clear();
    out_of_range_notes.clear();
    unmapped_articulations.clear();
    note_count = 0;
    controller_count = 0;
    keyswitch_count = 0;
}

MidiRenderer::MidiRenderer() : map_(make_default_expression_map()) {}

MidiRenderer::MidiRenderer(ExpressionMap map) : map_(std::move(map)) {}

void MidiRenderer::set_expression_map(ExpressionMap map)
{
    map_ = std::move(map);
}

Ticks MidiRenderer::sounding_ticks(Ticks written_ticks, ArticulationMask articulations) const
{
    // The default shortening exists only to keep repeated unmarked notes
    // from overlapping. A written length articulation is an explicit
    // instruction and replaces it rather than compounding with it -
    // otherwise tenuto, which means "held for its full value", would come
    // out at 95% of the written length and could never reach 100%.
    //
    // Multiple length rules still multiply with each other, so a note
    // carrying both staccato and portato is shortened by both.
    double scale = 1.0;
    bool has_length_rule = false;

    for (const ArticulationRule& rule : map_.articulations) {
        if (!has_articulation(articulations, rule.articulation))
            continue;
        if (rule.action == ArticulationAction::ScaleLength
            || rule.action == ArticulationAction::ScaleVelocityAndLength) {
            scale *= rule.length_scale;
            has_length_rule = true;
        }
    }

    if (!has_length_rule)
        scale = map_.default_length_scale;

    Ticks result = static_cast<Ticks>(static_cast<double>(written_ticks) * scale + 0.5);

    // Never shorter than the floor, and never longer than written: a note
    // that outlasts its notation would overlap the next one.
    result = std::max(result, map_.minimum_note_ticks);
    result = std::min(result, written_ticks);
    return std::max<Ticks>(result, 1);
}

std::uint8_t MidiRenderer::velocity_for(const Score& score, const ScoreNote& note,
                                        std::size_t staff_index) const
{
    const DynamicLevel level = score.dynamic_at(note.start_tick, note.voice, staff_index);
    double velocity = static_cast<double>(map_.velocity_for(level));

    // A hairpin covering this note overrides the static dynamic, so a note
    // in the middle of a crescendo is louder than the dynamic that opened it.
    if (const Hairpin* hairpin = score.hairpin_at(note.start_tick, note.voice, staff_index)) {
        const DynamicLevel start_level =
            hairpin->has_start_level ? hairpin->start_level
                                     : score.dynamic_at(hairpin->start_tick, note.voice, hairpin->staff);
        const DynamicLevel end_level =
            hairpin->has_end_level
                ? hairpin->end_level
                : (hairpin->kind == HairpinKind::Crescendo
                       ? DynamicLevel::Fortissimo
                       : DynamicLevel::Pianissimo);

        const double span = static_cast<double>(hairpin->length());
        const double position =
            span > 0.0 ? static_cast<double>(note.start_tick - hairpin->start_tick) / span : 0.0;
        const double shaped = evaluate_curve(
            hairpin->shape != CurveShape::Linear ? hairpin->shape : map_.default_hairpin_shape,
            position);

        const double from = static_cast<double>(map_.velocity_for(start_level));
        const double to = static_cast<double>(map_.velocity_for(end_level));
        velocity = from + (to - from) * shaped;
    }

    for (const ArticulationRule& rule : map_.articulations) {
        if (!has_articulation(note.articulations, rule.articulation))
            continue;
        if (rule.action == ArticulationAction::ScaleVelocity
            || rule.action == ArticulationAction::ScaleVelocityAndLength) {
            velocity *= rule.velocity_scale;
        }
    }

    return clamp_velocity(velocity);
}

void MidiRenderer::render_note(const Score& score, const Staff& staff, std::size_t staff_index,
                               const std::vector<ScoreNote>& staff_notes,
                               std::size_t note_index, RenderResult& result) const
{
    const ScoreNote& note = staff_notes[note_index];
    if (note.is_rest)
        return;

    // A tie chain sounds as one note. Follow it to the end and use the
    // combined length, so the instrument is not retriggered mid-tie.
    Ticks written_length = note.duration.ticks();
    std::size_t chain_index = note_index;

    while (staff_notes[chain_index].tied_to_next) {
        // The next note of the same pitch and voice continues the tie.
        bool extended = false;
        for (std::size_t next = chain_index + 1; next < staff_notes.size(); ++next) {
            const ScoreNote& candidate = staff_notes[next];
            if (candidate.is_rest || candidate.voice != staff_notes[chain_index].voice)
                continue;
            if (candidate.pitch != staff_notes[chain_index].pitch)
                continue;
            if (candidate.start_tick != staff_notes[chain_index].end_tick())
                continue;

            written_length += candidate.duration.ticks();
            chain_index = next;
            extended = true;
            break;
        }
        if (!extended)
            break;  // Tie flag with nothing to tie to; treat it as the end.
    }

    // Transposition is a sounding shift, applied here and never to the
    // written pitch (spec 5.1).
    const int written_note = midi_note_number(note.pitch);
    if (written_note < 0) {
        result.out_of_range_notes.push_back(note.id);
        return;
    }

    const int sounding = written_note + staff.total_transposition();
    if (sounding < 0 || sounding > 127) {
        result.out_of_range_notes.push_back(note.id);
        return;
    }

    const std::uint8_t velocity = velocity_for(score, note, staff_index);
    Ticks length = sounding_ticks(written_length, note.articulations);

    // A grace note sounds just before the beat it ornaments, as an
    // acciaccatura is played: the grace notes before a tick each take a
    // thirty-second, the last ending on the tick, whatever value they are
    // written in. What they steal from the note before them is the note
    // before them's problem, as it is in performance.
    Ticks sounding_start = note.start_tick;
    if (note.grace) {
        constexpr Ticks kGraceTicks = kTicksPerQuarter / 8;
        std::size_t after = 0;
        for (const ScoreNote& other : staff_notes) {
            if (other.grace && other.start_tick == note.start_tick && other.voice == note.voice
                && other.grace_order > note.grace_order)
                ++after;
        }
        sounding_start = std::max<Ticks>(
            0, note.start_tick - static_cast<Ticks>(after + 1) * kGraceTicks);
        length = kGraceTicks - kGraceTicks / 8;
    }

    // Keyswitches and per-articulation controllers, ahead of the note.
    for (const ArticulationRule& rule : map_.articulations) {
        if (!has_articulation(note.articulations, rule.articulation))
            continue;

        if (rule.action == ArticulationAction::Keyswitch && options_.render_keyswitches) {
            TimedMidiEvent keyswitch;
            keyswitch.tick = std::max<Ticks>(0, sounding_start - options_.keyswitch_lead_ticks);
            keyswitch.event = music::make_note_on(options_.channel, rule.keyswitch_note,
                                                  rule.keyswitch_velocity);
            keyswitch.source_id = note.id;
            keyswitch.role = EventRole::Keyswitch;
            result.events.push_back(keyswitch);
            ++result.keyswitch_count;

            // Release it immediately: a held keyswitch can consume a voice
            // or latch the wrong articulation in some instruments.
            TimedMidiEvent release;
            release.tick = keyswitch.tick + 1;
            release.event = music::make_note_off(options_.channel, rule.keyswitch_note, 0);
            release.source_id = note.id;
            release.role = EventRole::Keyswitch;
            result.events.push_back(release);
        } else if (rule.action == ArticulationAction::ControlChange) {
            TimedMidiEvent controller;
            controller.tick = std::max<Ticks>(0, sounding_start - 1);
            controller.event = music::make_control_change(options_.channel, rule.cc_number,
                                                          rule.cc_value);
            controller.source_id = note.id;
            controller.role = EventRole::Controller;
            result.events.push_back(controller);
            ++result.controller_count;
        }
    }

    // Report articulations the map has no rule for, so the UI can say the
    // marking will not be heard rather than implying it was applied.
    static const Articulation kChecked[] = {
        Articulation::Accent,    Articulation::Staccato,  Articulation::Staccatissimo,
        Articulation::Tenuto,    Articulation::Marcato,   Articulation::Legato,
        Articulation::Portato,   Articulation::Spiccato,  Articulation::Pizzicato,
        Articulation::Tremolo,   Articulation::Trill,     Articulation::Harmonic,
    };
    for (const Articulation articulation : kChecked) {
        if (!has_articulation(note.articulations, articulation))
            continue;
        if (map_.rule_for(articulation) != nullptr)
            continue;
        if (std::find(result.unmapped_articulations.begin(), result.unmapped_articulations.end(),
                      articulation)
            == result.unmapped_articulations.end()) {
            result.unmapped_articulations.push_back(articulation);
        }
    }

    TimedMidiEvent note_on;
    note_on.tick = sounding_start;
    note_on.event = music::make_note_on(options_.channel, static_cast<std::uint8_t>(sounding),
                                        velocity);
    note_on.source_id = note.id;
    result.events.push_back(note_on);

    TimedMidiEvent note_off;
    note_off.tick = sounding_start + length;
    note_off.event = music::make_note_off(options_.channel, static_cast<std::uint8_t>(sounding), 0);
    note_off.source_id = note.id;
    result.events.push_back(note_off);

    ++result.note_count;
}

void MidiRenderer::render_dynamic(const DynamicMarking& marking, RenderResult& result) const
{
    if (map_.dynamic_target == DynamicTarget::Velocity)
        return;  // This map expresses dynamics through velocity alone.

    TimedMidiEvent controller;
    controller.tick = marking.tick;
    controller.event = music::make_control_change(options_.channel, map_.dynamics_cc,
                                                  map_.cc_value_for(marking.level));
    result.events.push_back(controller);
    ++result.controller_count;
}

bool MidiRenderer::dynamics_cc_at(const Score& score, Ticks tick, std::uint8_t& value,
                                  std::size_t staff_index) const
{
    // The markings that govern this staff: its own, and another staff's
    // only while this staff has none of its own in force - the same rule
    // render_staff sends by, so a chase lands where the stream would be.
    const auto governs = [&](std::uint8_t marked, Ticks at) {
        return marked == staff_index || !score.staff_has_dynamics(staff_index, at);
    };

    if (map_.dynamic_target == DynamicTarget::Velocity)
        return false;  // This map expresses dynamics through velocity alone.

    bool found = false;
    Ticks latest = 0;

    const auto consider = [&](Ticks at, std::uint8_t candidate) {
        // `>=` so that a hairpin starting exactly where a marking sits wins:
        // the hairpin is the later instruction, and it is what the renderer
        // would have sent last at that tick.
        if (!found || at >= latest) {
            found = true;
            latest = at;
            value = candidate;
        }
    };

    if (options_.render_dynamics) {
        for (const DynamicMarking& marking : score.dynamics()) {
            if (marking.tick <= tick && governs(marking.staff, marking.tick))
                consider(marking.tick, map_.cc_value_for(marking.level));
        }
    }

    if (options_.render_hairpins) {
        for (const Hairpin& hairpin : score.hairpins()) {
            if (hairpin.start_tick > tick || !governs(hairpin.staff, hairpin.start_tick))
                continue;

            // Inside the hairpin, the value is where the curve has reached;
            // past its end, the value it arrived at. Either way this is the
            // same arithmetic render_hairpin does, so a chase lands on a
            // value the stream itself would have produced.
            const DynamicLevel start_level =
                hairpin.has_start_level ? hairpin.start_level
                                        : score.dynamic_at(hairpin.start_tick, hairpin.voice,
                                                           hairpin.staff);
            const DynamicLevel end_level =
                hairpin.has_end_level ? hairpin.end_level
                                      : (hairpin.kind == HairpinKind::Crescendo
                                             ? DynamicLevel::Fortissimo
                                             : DynamicLevel::Pianissimo);

            const double from = static_cast<double>(map_.cc_value_for(start_level));
            const double to = static_cast<double>(map_.cc_value_for(end_level));

            const CurveShape shape =
                hairpin.shape != CurveShape::Linear ? hairpin.shape : map_.default_hairpin_shape;

            const Ticks span = hairpin.length();
            const Ticks reached = std::min(tick, hairpin.end_tick);
            const double position =
                span > 0 ? static_cast<double>(reached - hairpin.start_tick)
                               / static_cast<double>(span)
                         : 1.0;

            consider(reached, clamp_midi_value(from + (to - from) * evaluate_curve(shape, position)));
        }
    }

    return found;
}

void MidiRenderer::render_hairpin(const Score& score, const Hairpin& hairpin,
                                  RenderResult& result) const
{
    // A hairpin only produces controller data when dynamics go through a
    // controller. On a velocity-only map the shaping is already applied to
    // each note's velocity in velocity_for().
    if (map_.dynamic_target == DynamicTarget::Velocity)
        return;

    const DynamicLevel start_level =
        hairpin.has_start_level ? hairpin.start_level
                                : score.dynamic_at(hairpin.start_tick, hairpin.voice, hairpin.staff);
    const DynamicLevel end_level =
        hairpin.has_end_level ? hairpin.end_level
                              : (hairpin.kind == HairpinKind::Crescendo
                                     ? DynamicLevel::Fortissimo
                                     : DynamicLevel::Pianissimo);

    const double from = static_cast<double>(map_.cc_value_for(start_level));
    const double to = static_cast<double>(map_.cc_value_for(end_level));

    const CurveShape shape =
        hairpin.shape != CurveShape::Linear ? hairpin.shape : map_.default_hairpin_shape;

    const Ticks span = hairpin.length();
    const Ticks step = std::max<Ticks>(1, map_.hairpin_resolution_ticks);

    // Inclusive of both endpoints, so the curve actually reaches its target
    // value rather than stopping one step short.
    for (Ticks offset = 0; offset <= span; offset += step) {
        const double position = span > 0 ? static_cast<double>(offset) / static_cast<double>(span)
                                         : 1.0;
        const double shaped = evaluate_curve(shape, position);

        TimedMidiEvent controller;
        controller.tick = hairpin.start_tick + offset;
        controller.event = music::make_control_change(options_.channel, map_.dynamics_cc,
                                                      clamp_midi_value(from + (to - from) * shaped));
        result.events.push_back(controller);
        ++result.controller_count;
    }

    // Guarantee the final value when the span is not a whole number of
    // steps, so a crescendo always finishes at its written dynamic.
    if (span % step != 0) {
        TimedMidiEvent controller;
        controller.tick = hairpin.end_tick;
        controller.event = music::make_control_change(options_.channel, map_.dynamics_cc,
                                                      clamp_midi_value(to));
        result.events.push_back(controller);
        ++result.controller_count;
    }
}

RenderResult MidiRenderer::render_staff(const Score& score, std::size_t staff_index) const
{
    RenderResult result;
    if (staff_index >= score.staff_count())
        return result;

    const Staff& staff = score.staff(staff_index);
    const std::vector<ScoreNote>& staff_notes = score.notes(staff_index);

    const bool whole = options_.renders_whole_score();

    // Where tied notes end, by voice and written pitch: a note starting at
    // one of these continues a tie. Collected once, so finding out costs a
    // lookup rather than a pass over every earlier note (which made a long
    // part quadratic to render). A tied note ending where another starts
    // began before it, so it is always an earlier note in the staff.
    std::set<std::tuple<int, int, int, int, Ticks>> tie_ends;
    for (const ScoreNote& candidate : staff_notes)
        if (candidate.tied_to_next && !candidate.is_rest)
            tie_ends.emplace(candidate.voice, static_cast<int>(candidate.pitch.step), candidate.pitch.octave,
                             candidate.pitch.alteration, candidate.end_tick());

    for (std::size_t index = 0; index < staff_notes.size(); ++index) {
        const ScoreNote& note = staff_notes[index];
        if (!whole && (note.end_tick() <= options_.from_tick
                       || note.start_tick >= options_.to_tick)) {
            continue;
        }

        // A note continuing a tie is rendered as part of the chain that
        // started it, not on its own.
        const bool continues_tie =
            !tie_ends.empty()
            && tie_ends.count(std::make_tuple(static_cast<int>(note.voice), static_cast<int>(note.pitch.step),
                                              static_cast<int>(note.pitch.octave),
                                              static_cast<int>(note.pitch.alteration), note.start_tick))
                   > 0;
        if (continues_tie)
            continue;

        render_note(score, staff, staff_index, staff_notes, index, result);
    }

    // The markings that govern this staff: its own, and another staff's
    // only while this staff has none of its own in force - one marking
    // between a piano's staves is both hands' until the other hand is
    // marked for itself, and a part marked staff by staff is read staff
    // by staff.
    const auto governs = [&](std::uint8_t marked, Ticks at) {
        return marked == staff_index || !score.staff_has_dynamics(staff_index, at);
    };

    if (options_.render_dynamics) {
        for (const DynamicMarking& marking : score.dynamics()) {
            if (!whole && (marking.tick < options_.from_tick || marking.tick >= options_.to_tick))
                continue;
            if (!governs(marking.staff, marking.tick))
                continue;
            render_dynamic(marking, result);
        }
    }

    if (options_.render_hairpins) {
        for (const Hairpin& hairpin : score.hairpins()) {
            if (!whole && (hairpin.end_tick <= options_.from_tick
                           || hairpin.start_tick >= options_.to_tick)) {
                continue;
            }
            if (!governs(hairpin.staff, hairpin.start_tick))
                continue;
            render_hairpin(score, hairpin, result);
        }
    }

    std::stable_sort(result.events.begin(), result.events.end(),
                     [](const TimedMidiEvent& a, const TimedMidiEvent& b) {
                         if (a.tick != b.tick)
                             return a.tick < b.tick;
                         return send_priority(a.event) < send_priority(b.event);
                     });

    return result;
}

RenderResult MidiRenderer::render(const Score& score) const
{
    RenderResult merged;

    for (std::size_t staff_index = 0; staff_index < score.staff_count(); ++staff_index) {
        RenderResult staff_result = render_staff(score, staff_index);

        merged.events.insert(merged.events.end(), staff_result.events.begin(),
                             staff_result.events.end());
        merged.out_of_range_notes.insert(merged.out_of_range_notes.end(),
                                         staff_result.out_of_range_notes.begin(),
                                         staff_result.out_of_range_notes.end());
        merged.note_count += staff_result.note_count;
        merged.keyswitch_count += staff_result.keyswitch_count;

        // Dynamics and hairpins belong to the score rather than to a staff,
        // so they would otherwise be rendered once per staff.
        if (staff_index == 0)
            merged.controller_count += staff_result.controller_count;

        for (const Articulation articulation : staff_result.unmapped_articulations) {
            if (std::find(merged.unmapped_articulations.begin(),
                          merged.unmapped_articulations.end(), articulation)
                == merged.unmapped_articulations.end()) {
                merged.unmapped_articulations.push_back(articulation);
            }
        }
    }

    // Drop the duplicate marking events the per-staff renders produced.
    if (score.staff_count() > 1) {
        std::vector<TimedMidiEvent> deduplicated;
        deduplicated.reserve(merged.events.size());
        for (const TimedMidiEvent& event : merged.events) {
            if (event.source_id == kInvalidEventId
                && event.event.type == music::MidiEventType::ControlChange) {
                const bool already_present =
                    std::any_of(deduplicated.begin(), deduplicated.end(),
                                [&event](const TimedMidiEvent& other) {
                                    return other.tick == event.tick
                                        && other.source_id == kInvalidEventId
                                        && other.event.type == event.event.type
                                        && other.event.data1 == event.event.data1
                                        && other.event.data2 == event.event.data2;
                                });
                if (already_present)
                    continue;
            }
            deduplicated.push_back(event);
        }
        merged.events = std::move(deduplicated);
    }

    std::stable_sort(merged.events.begin(), merged.events.end(),
                     [](const TimedMidiEvent& a, const TimedMidiEvent& b) {
                         if (a.tick != b.tick)
                             return a.tick < b.tick;
                         return send_priority(a.event) < send_priority(b.event);
                     });

    return merged;
}

std::int64_t ticks_to_samples(const Score& score, Ticks tick, double sample_rate) noexcept
{
    if (!(sample_rate > 0.0) || tick <= 0)
        return 0;

    // Integrate across tempo changes rather than assuming one tempo, so a
    // score with a tempo change lands correctly on both sides of it.
    double seconds = 0.0;
    Ticks cursor = 0;
    double tempo = 120.0;

    const std::vector<TempoMarking>& tempos = score.tempos();
    for (const TempoMarking& marking : tempos) {
        if (marking.tick >= tick)
            break;
        if (marking.tick > cursor) {
            const double quarters =
                static_cast<double>(marking.tick - cursor) / static_cast<double>(kTicksPerQuarter);
            seconds += quarters * 60.0 / tempo;
            cursor = marking.tick;
        }
        tempo = marking.quarter_notes_per_minute();
    }

    const double quarters =
        static_cast<double>(tick - cursor) / static_cast<double>(kTicksPerQuarter);
    seconds += quarters * 60.0 / tempo;

    return static_cast<std::int64_t>(seconds * sample_rate + 0.5);
}

Ticks samples_to_ticks(const Score& score, std::int64_t samples, double sample_rate) noexcept
{
    if (!(sample_rate > 0.0) || samples <= 0)
        return 0;

    const double target_seconds = static_cast<double>(samples) / sample_rate;

    double seconds = 0.0;
    Ticks cursor = 0;
    double tempo = 120.0;

    for (const TempoMarking& marking : score.tempos()) {
        if (marking.tick <= cursor) {
            tempo = marking.quarter_notes_per_minute();
            continue;
        }

        const double quarters =
            static_cast<double>(marking.tick - cursor) / static_cast<double>(kTicksPerQuarter);
        const double span_seconds = quarters * 60.0 / tempo;

        if (seconds + span_seconds >= target_seconds) {
            const double remaining = target_seconds - seconds;
            const double remaining_quarters = remaining * tempo / 60.0;
            return cursor
                 + static_cast<Ticks>(remaining_quarters * kTicksPerQuarter + 0.5);
        }

        seconds += span_seconds;
        cursor = marking.tick;
        tempo = marking.quarter_notes_per_minute();
    }

    const double remaining = target_seconds - seconds;
    const double remaining_quarters = remaining * tempo / 60.0;
    return cursor + static_cast<Ticks>(remaining_quarters * kTicksPerQuarter + 0.5);
}

}  // namespace vcc::notation
