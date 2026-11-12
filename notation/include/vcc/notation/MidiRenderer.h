// Copyright (c) VC Composer. All rights reserved.
//
// Notation-to-MIDI translation (spec 02 section 6, acceptance criteria 2, 5, 6).
//
// Spec 6 states the principle: "Notation is not merely visual. It is the
// editing source for the MIDI performance." This is the module that carries
// out the table in that section - pitch and duration to note events,
// dynamics to velocity or controller, articulations to velocity, length,
// keyswitch or controller, hairpins to a controller curve.
//
// It produces events in *ticks*, not samples. Converting to a sample
// timeline needs a tempo, which belongs to the host session and can change
// under the plug-in; keeping the renderer in musical time means its output
// is correct regardless, and the host adapter converts at the last moment.
//
// Nothing here touches the host, the bridge or the audio thread. Rendering
// allocates and is done on a UI or worker thread, and its output is then
// handed to the real-time path.
#pragma once

#include <cstdint>
#include <vector>

#include "vcc/music/MidiEvent.h"
#include "vcc/notation/ExpressionMap.h"
#include "vcc/notation/Score.h"

namespace vcc::notation {

/// A MIDI event at a musical position.
///
/// `event.sample_position` is deliberately left unused: the position that
/// matters here is `tick`, and filling in a sample position would imply a
/// tempo the renderer does not know.
/// What part a rendered event plays for the note that produced it.
///
/// One note can emit several events of the same MIDI type: a keyswitch
/// note-on, then its own note-on. Without something to tell them apart, the
/// only distinguishing field is the note number - and keying on that means a
/// transposed note looks like a different event entirely, so the host is
/// told to delete and recreate it rather than to change it. That is the
/// whole-region rewrite spec 13 exists to avoid, in miniature.
enum class EventRole : std::uint8_t {
    /// The sounding note itself.
    Note = 0,
    /// An articulation keyswitch, and its release.
    Keyswitch,
    /// A controller message: dynamics, hairpins, an articulation's CC.
    Controller,
};

const char* to_string(EventRole role) noexcept;

struct TimedMidiEvent {
    Ticks tick = 0;
    music::MidiEvent event{};

    /// The score note this came from, or `kInvalidEventId` for events that
    /// belong to a marking rather than a note - a hairpin's controller
    /// messages. This is the link spec 13 requires: "Maintain stable IDs
    /// linking notation events to host MIDI events where technically
    /// possible."
    EventId source_id = kInvalidEventId;

    /// What this event is for. Part of the event's identity, so that two
    /// events a note emits are never confused with each other.
    EventRole role = EventRole::Note;
};

/// The result of rendering a score, or part of one.
struct RenderResult {
    /// Ordered by tick, then by the order events must be sent within a tick:
    /// keyswitches and controllers before the note-on they apply to.
    std::vector<TimedMidiEvent> events;

    /// Notes whose pitch fell outside MIDI's range after transposition.
    /// Reported rather than silently dropped, so the UI can say which note
    /// is unplayable instead of the composer discovering silence.
    std::vector<EventId> out_of_range_notes;

    /// Articulations present in the score that the expression map has no
    /// rule for. Not an error - a library without a pizzicato patch simply
    /// cannot play one - but the UI should say so rather than imply the
    /// marking had an effect.
    std::vector<Articulation> unmapped_articulations;

    std::size_t note_count = 0;
    std::size_t controller_count = 0;
    std::size_t keyswitch_count = 0;

    void clear();
};

/// Options controlling a render.
struct RenderOptions {
    /// MIDI channel for the rendered events, 0-15.
    std::uint8_t channel = 0;

    /// Render only this range, in ticks. An empty range renders everything.
    /// Used to re-render just the edited region (spec 13's event-diff
    /// requirement is served by ScoreDiff; this is the coarser tool for
    /// re-rendering a bar the user just changed).
    Ticks from_tick = 0;
    Ticks to_tick = 0;

    /// Emit the controller messages a hairpin implies. Off when the host
    /// will not accept controller data, or when the user has chosen to draw
    /// their own automation.
    bool render_hairpins = true;

    /// Emit a controller message at each written dynamic, for maps whose
    /// target involves a controller.
    bool render_dynamics = true;

    /// Emit keyswitches for articulations mapped to them.
    bool render_keyswitches = true;

    /// Ticks before a note-on that its keyswitch is sent. Most libraries
    /// need the switch to arrive first; too far ahead and a switch intended
    /// for one note lands during the previous one.
    Ticks keyswitch_lead_ticks = 10;

    bool renders_whole_score() const noexcept { return to_tick <= from_tick; }
};

/// Translates a score into MIDI.
///
/// Stateless between calls apart from the map and options: rendering the
/// same score twice with the same settings produces byte-identical output,
/// which is what makes the diff layer meaningful.
class MidiRenderer {
public:
    MidiRenderer();
    explicit MidiRenderer(ExpressionMap map);

    void set_expression_map(ExpressionMap map);
    const ExpressionMap& expression_map() const noexcept { return map_; }

    void set_options(const RenderOptions& options) { options_ = options; }
    const RenderOptions& options() const noexcept { return options_; }

    /// The dynamics controller value this map's stream would last have sent
    /// at or before `tick`, if any.
    ///
    /// For joining a passage part-way through. A player reading from bar 30
    /// has seen the *mf* at bar 12 and is inside the crescendo that began at
    /// bar 28; a stream that starts at bar 30 has told the instrument
    /// neither, so it plays at whatever level it was last set to - which
    /// after a locate backwards is a level from further on in the piece.
    ///
    /// Computed from the markings rather than by rendering everything before
    /// `tick`: on a ten-minute cue that render measured 5.3 ms against
    /// 0.2 ms for the window itself, and it happens on every edit made
    /// during playback.
    ///
    /// False when this map expresses dynamics through velocity alone, or
    /// when nothing has been written yet.
    bool dynamics_cc_at(const Score& score, Ticks tick, std::uint8_t& value,
                        std::size_t staff_index = 0) const;

    /// Renders one staff of the score.
    RenderResult render_staff(const Score& score, std::size_t staff_index) const;

    /// Renders every staff, merged into one ordered event list. Used for a
    /// grand staff, where both hands play one instrument.
    RenderResult render(const Score& score) const;

private:
    /// Appends the events for one note, following a tie chain to its end so
    /// that tied notes sound as one.
    void render_note(const Score& score, const Staff& staff, std::size_t staff_index,
                     const std::vector<ScoreNote>& staff_notes, std::size_t note_index,
                     RenderResult& result) const;

    /// Appends the controller curve for a hairpin.
    void render_hairpin(const Score& score, const Hairpin& hairpin, RenderResult& result) const;

    /// Appends a controller message for a written dynamic.
    void render_dynamic(const DynamicMarking& marking, RenderResult& result) const;

    /// Sounding length of a note after the default and articulation scaling.
    Ticks sounding_ticks(Ticks written_ticks, ArticulationMask articulations) const;

    /// Velocity for a note, from the dynamic in force and its articulations.
    std::uint8_t velocity_for(const Score& score, const ScoreNote& note,
                              std::size_t staff_index) const;

    ExpressionMap map_;
    RenderOptions options_{};
};

/// Converts a musical position to samples, following the score's tempo
/// markings (spec 6: tempo maps to the host where permitted; until then the
/// renderer's own tempo map is what keeps notation and audio aligned).
///
/// Integrates across tempo changes rather than assuming one tempo, so a
/// score with a ritardando lands correctly.
std::int64_t ticks_to_samples(const Score& score, Ticks tick, double sample_rate) noexcept;

/// Inverse of `ticks_to_samples`.
Ticks samples_to_ticks(const Score& score, std::int64_t samples, double sample_rate) noexcept;

}  // namespace vcc::notation
