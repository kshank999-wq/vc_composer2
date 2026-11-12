#include "vcc/notation/ScoreMidiFile.h"

#include <algorithm>

namespace vcc::notation {
namespace {

/// Bar number as a musician would say it, for a message about a position.
std::string bar_of(const Score& score, Ticks tick)
{
    return std::to_string(score.to_bar_beat(tick).measure);
}

/// Appends a meta event, or a sentence saying why the format cannot hold it.
void add_meta(music::SmfTrack& track, const music::SmfMetaEvent& meta, bool ok,
              std::string&& complaint, ScoreMidiResult& result)
{
    if (!ok) {
        result.unrepresentable.push_back(std::move(complaint));
        return;
    }
    track.meta.push_back(meta);
}

/// The conductor track: the tempo map, the meters and the keys, and no notes.
music::SmfTrack build_conductor_track(const Score& score, const ScoreMidiOptions& options,
                                      Ticks from, Ticks to, bool whole_score, Ticks shift,
                                      ScoreMidiResult& result)
{
    music::SmfTrack track;
    track.name = options.conductor_track_name;

    const auto in_range = [&](Ticks tick) {
        return tick > from && (whole_score || tick < to);
    };

    // What is in force where the export begins, written at the file's start.
    // Without it a passage starting at bar 40 would be read in whatever the
    // reader assumes rather than in the meter the composer wrote.
    //
    // The meter and key checks below cannot fire against a score as the
    // model stands: Score refuses a denominator that is not a power of two
    // and a key beyond seven accidentals, which is exactly what the file
    // format can carry. They are kept as the one place that would notice if
    // the model were relaxed, since the alternative is a file that says
    // something the composer did not write. The tempo check is not
    // defensive - a score can hold a tempo slow enough to overflow the
    // format's 24-bit field, and the option's default tempo is not
    // validated at all.
    {
        bool ok = false;
        const music::TimeSignature signature = score.time_signature_at(from);
        const music::SmfMetaEvent meta = music::smf_time_signature(0, signature, ok);
        add_meta(track, meta, ok,
                 "the " + std::to_string(signature.numerator) + "/"
                     + std::to_string(signature.denominator) + " meter at bar "
                     + bar_of(score, from)
                     + " cannot be written to a MIDI file: the format stores the "
                       "denominator as a power of two",
                 result);
    }
    {
        bool ok = false;
        const KeySignature key = score.key_signature_at(from);
        const music::SmfMetaEvent meta = music::smf_key_signature(0, key.fifths, key.is_minor, ok);
        add_meta(track, meta, ok,
                 "the key signature at bar " + bar_of(score, from)
                     + " has more than seven accidentals, which a MIDI file cannot carry",
                 result);
    }
    {
        // A score with no tempo marking anywhere takes the option's tempo;
        // one with markings takes whatever is in force here, which is the
        // score's own answer and not this function's to second-guess.
        const double tempo = score.tempos().empty() ? options.default_tempo_bpm
                                                     : score.tempo_at(from);
        bool ok = false;
        const music::SmfMetaEvent meta = music::smf_tempo(0, tempo, ok);
        add_meta(track, meta, ok,
                 "a tempo of " + std::to_string(tempo)
                     + " quarter notes per minute is outside the range a MIDI file can "
                       "express",
                 result);
    }

    // Then every change inside the range. A change exactly at `from` is the
    // one already written above, so it is not written twice.
    for (const TimeSignatureChange& change : score.time_signature_changes()) {
        if (!in_range(change.tick))
            continue;
        bool ok = false;
        const music::SmfMetaEvent meta =
            music::smf_time_signature(to_host_ticks(change.tick - shift), change.time_signature, ok);
        add_meta(track, meta, ok,
                 "the " + std::to_string(change.time_signature.numerator) + "/"
                     + std::to_string(change.time_signature.denominator) + " meter at bar "
                     + bar_of(score, change.tick)
                     + " cannot be written to a MIDI file: the format stores the "
                       "denominator as a power of two",
                 result);
    }
    for (const KeySignatureChange& change : score.key_signature_changes()) {
        if (!in_range(change.tick))
            continue;
        bool ok = false;
        const music::SmfMetaEvent meta =
            music::smf_key_signature(to_host_ticks(change.tick - shift), change.key.fifths,
                                     change.key.is_minor, ok);
        add_meta(track, meta, ok,
                 "the key signature at bar " + bar_of(score, change.tick)
                     + " has more than seven accidentals, which a MIDI file cannot carry",
                 result);
    }
    for (const TempoMarking& marking : score.tempos()) {
        if (!in_range(marking.tick))
            continue;
        bool ok = false;
        const music::SmfMetaEvent meta = music::smf_tempo(
            to_host_ticks(marking.tick - shift), marking.quarter_notes_per_minute(), ok);
        add_meta(track, meta, ok,
                 "the tempo at bar " + bar_of(score, marking.tick)
                     + " is outside the range a MIDI file can express",
                 result);
    }

    return track;
}

/// Turns rendered events into file events, shifting them into the file's own
/// time.
void add_rendered(const RenderResult& rendered, Ticks shift, music::SmfTrack& track,
                  ScoreMidiResult& result)
{
    track.events.reserve(track.events.size() + rendered.events.size());

    // Events that fall before the start are moved to it, keeping their order
    // - the second to tick 1, the third to tick 2, and so on.
    //
    // Collapsing them all onto tick 0 would not do. A keyswitch is a note-on
    // and a note-off a tick apart, and at one tick the writer orders a
    // note-off before a note-on, so the switch's release would be written
    // before the switch itself and the keyswitch note would be left sounding
    // for the rest of the cue. A file tick is 1/960 of a quarter note; the
    // order is what matters here, and the displacement is not audible.
    Ticks next_clamped_tick = 0;

    for (const TimedMidiEvent& event : rendered.events) {
        const Ticks score_tick = event.tick - shift;
        if (!on_host_tick(score_tick))
            ++result.rounded_to_file_ticks;
        Ticks tick = to_host_ticks(score_tick);
        if (tick < 0) {
            // A keyswitch the renderer placed ahead of the note it applies
            // to, where the note is the first in the exported range.
            tick = next_clamped_tick++;
            ++result.clamped_to_start;
        }
        track.events.push_back(music::SmfEvent{tick, event.event});
    }

    result.note_count += rendered.note_count;
    result.controller_count += rendered.controller_count;
    result.keyswitch_count += rendered.keyswitch_count;

    result.out_of_range_notes.insert(result.out_of_range_notes.end(),
                                     rendered.out_of_range_notes.begin(),
                                     rendered.out_of_range_notes.end());
    for (const Articulation articulation : rendered.unmapped_articulations) {
        if (std::find(result.unmapped_articulations.begin(),
                      result.unmapped_articulations.end(), articulation)
            == result.unmapped_articulations.end()) {
            result.unmapped_articulations.push_back(articulation);
        }
    }
}

std::string track_name_for(const Score& score, const ScoreMidiOptions& options,
                           std::size_t staff_index, bool per_staff)
{
    if (!options.track_name.empty()) {
        if (!per_staff)
            return options.track_name;
        return options.track_name + " " + std::to_string(staff_index + 1);
    }

    if (staff_index < score.staff_count() && !score.staff(staff_index).name.empty())
        return score.staff(staff_index).name;

    // Never empty: a DAW shows this as the imported track's name, and an
    // unnamed track is one the composer cannot find again.
    if (!per_staff)
        return "VC Composer";
    return "Staff " + std::to_string(staff_index + 1);
}

/// The text markings in the exported range, as marker meta events.
void add_markers(const Score& score, Ticks from, Ticks to, bool whole_score, Ticks shift,
                 music::SmfTrack& track, ScoreMidiResult& result)
{
    for (const TextMarking& marking : score.texts()) {
        // At the start as well, unlike a signature change: a marker at the
        // first tick is not already written by anything else.
        if (marking.tick < from || (!whole_score && marking.tick >= to) || marking.text.empty())
            continue;
        track.meta.push_back(music::smf_marker(to_host_ticks(marking.tick - shift), marking.text));
        ++result.marker_count;
    }
}

}  // namespace

ScoreMidiResult build_score_midi(const Score& score, const MidiRenderer& renderer,
                                 const ScoreMidiOptions& options)
{
    ScoreMidiResult result;
    // The host's resolution, not the score's: see the header.
    result.file.ticks_per_quarter = static_cast<std::uint16_t>(music::kTicksPerQuarter);

    const bool whole_score = options.render.renders_whole_score();
    const Ticks from = std::max<Ticks>(0, options.render.from_tick);
    const Ticks to = options.render.to_tick;
    const Ticks shift = options.rebase_to_range_start ? from : 0;

    if (options.write_conductor_track) {
        result.file.tracks.push_back(
            build_conductor_track(score, options, from, to, whole_score, shift, result));
        result.has_conductor_track = true;
    }

    // Rendered through a copy, so the caller's renderer keeps its own channel
    // and range settings.
    MidiRenderer working = renderer;

    if (options.one_track_per_staff) {
        for (std::size_t staff = 0; staff < score.staff_count(); ++staff) {
            RenderOptions render = options.render;
            render.channel = static_cast<std::uint8_t>((options.first_channel + staff) % 16);
            working.set_options(render);

            music::SmfTrack track;
            track.name = track_name_for(score, options, staff, true);
            add_rendered(working.render_staff(score, staff), shift, track, result);
            result.file.tracks.push_back(std::move(track));
            ++result.music_track_count;
        }
    } else {
        RenderOptions render = options.render;
        render.channel = options.first_channel;
        working.set_options(render);

        music::SmfTrack track;
        track.name = track_name_for(score, options, 0, false);
        add_rendered(working.render(score), shift, track, result);
        result.file.tracks.push_back(std::move(track));
        ++result.music_track_count;
    }

    // The markers, on the conductor track when there is one and on the
    // first music track otherwise - never on a track of their own, which a
    // DAW would import as an empty instrument.
    if (options.write_text_as_markers && !result.file.tracks.empty())
        add_markers(score, from, to, whole_score, shift, result.file.tracks.front(), result);

    // Format 0 means one track by definition, and is the form every reader
    // understands; format 1 is what a tempo map in its own track requires.
    result.file.format = result.file.tracks.size() == 1 ? music::SmfFormat::SingleTrack
                                                        : music::SmfFormat::MultiTrack;
    return result;
}

music::SmfWriteResult write_score_midi(const Score& score, const MidiRenderer& renderer,
                                       const std::string& path,
                                       const ScoreMidiOptions& options,
                                       ScoreMidiResult* built)
{
    ScoreMidiResult result = build_score_midi(score, renderer, options);
    const music::SmfWriteResult written = music::write_smf_file(result.file, path);
    if (built != nullptr)
        *built = std::move(result);
    return written;
}

}  // namespace vcc::notation
