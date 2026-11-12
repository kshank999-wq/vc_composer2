// Copyright (c) VC Composer. All rights reserved.
//
// Score to display list (spec 02 sections 4.3, 4.4, 5.1, 5.2, 5.3).
//
// Turns a score plus a timeline into positioned glyphs, lines, beams and
// curves. It does not draw, and it does not know about pixels: see
// DisplayList for why that seam is where it is.
//
// Horizontal position comes from the TimelineMap, never from engraving
// spacing - see docs/aax/10-notation-view-design.md for why, and what it
// costs.
#pragma once

#include <cstdint>
#include <string>
#include <map>
#include <vector>

#include "vcc/engraving/DisplayList.h"
#include "vcc/engraving/Geometry.h"
#include "vcc/engraving/Smufl.h"
#include "vcc/engraving/TimelineMap.h"
#include "vcc/notation/Score.h"

namespace vcc::engraving {

/// Where a marking sits relative to a staff.
enum class Placement : std::uint8_t { Above = 0, Below };

/// Layout settings. All distances in staff spaces.
struct LayoutOptions {
    /// Vertical gap between staves of a grand staff.
    double staff_gap = 8.0;

    /// Room reserved at the left for clef, key and time signature. The
    /// timeline governs everything to the right of it, so this is the one
    /// horizontal measurement layout owns. Advisory: a seven-flat key
    /// signature needs more, and layout widens the header rather than
    /// letting the signature collide with the first note.
    double header_width = 8.0;

    /// Draw the clef, key and time signature at the left edge.
    bool draw_header = true;

    /// Whether the header carries the time signature. A printed part shows
    /// it on the first line and where the meter changes, not on every
    /// line; a window shows it always, since any line may be the first in
    /// view.
    bool header_time_signature = true;

    /// Draw barlines through the staff.
    bool draw_barlines = true;

    /// Draw the bar/beat ruler above the top staff (spec 4.3).
    bool draw_ruler = true;
    double ruler_height = 2.0;

    /// Vertical room reserved above and below for markings. Above holds a
    /// voice's dynamic over the staff and a text over that: the view
    /// positions itself from the page rather than from what was drawn, so
    /// what the page does not reserve room for is clipped there.
    double margin_above = 8.0;
    double margin_below = 6.0;

    /// Dynamics and hairpins sit below the staff by convention.
    Placement dynamics_placement = Placement::Below;

    /// Beam notes shorter than a quarter, grouped by beat.
    bool draw_beams = true;

    /// Draw ties and slurs.
    bool draw_ties = true;
    bool draw_slurs = true;

    /// Draw articulations.
    bool draw_articulations = true;

    /// Text markings - rehearsal letters, cue points, performance text -
    /// above the top staff at their tick.
    bool draw_texts = true;

    /// Draw tuplet numbers, and brackets where the group is not held
    /// together by a beam.
    bool draw_tuplets = true;

    /// Draw the edit cursor at `cursor_tick`.
    bool draw_cursor = false;
    notation::Ticks cursor_tick = 0;

    /// Staff the cursor is on. A value at or past the staff count draws the
    /// cursor through the whole system, which is what a transport playhead
    /// wants.
    std::size_t cursor_staff = 0;

    bool is_valid() const noexcept;
};

/// A laid-out note, kept so hit testing and the cursor can work against
/// positions rather than re-deriving them.
struct LaidOutNote {
    notation::EventId id = notation::kInvalidEventId;
    std::size_t staff_index = 0;

    notation::Ticks start_tick = 0;
    notation::Ticks end_tick = 0;

    /// Notehead centre.
    Point centre{};
    Rect notehead_bounds{};

    int staff_position = 0;
    bool is_rest = false;
    bool stem_up = true;

    /// The voice the note is in, so a view can tell a click on the alto
    /// line from one on the soprano without a second lookup.
    std::uint8_t voice = 0;

    /// The stem, when the note has one: where it stands and where its free
    /// end is. What a slur fitted to the notes it joins starts and ends
    /// at, on the stem side.
    bool has_stem = false;
    double stem_x = 0.0;
    double stem_end_y = 0.0;

    /// True when this note's head is offset to the right because it forms a
    /// second with another note of the same chord.
    bool is_offset = false;

    /// A grace note: drawn small before the note it ornaments, taking no
    /// time of its own, so `end_tick` is `start_tick`.
    bool is_grace = false;
};

/// Layout output: the display list plus what a caller needs to interact
/// with it.
struct LayoutResult {
    DisplayList display_list;

    /// Every note and rest that was laid out, in score order.
    std::vector<LaidOutNote> notes;

    /// y of the top line of each staff, in the order the score holds them.
    std::vector<double> staff_top_y;

    /// The page: the left edge is the music origin less the header, the top
    /// is the ruler, and the bottom is the margin below the lowest staff.
    ///
    /// Stable by construction - it depends on the view range, the staves and
    /// the options, and on nothing that happened to be drawn. A renderer
    /// positions itself from this, so anything else would mean one stray
    /// item could shift the whole score sideways.
    Rect extent{};

    /// Everything actually drawn, which can reach outside the page: a note
    /// on six ledger lines, a marking pushed clear of it. A renderer sizing
    /// a canvas or an image uses this so nothing is clipped.
    Rect drawn_bounds{};

    /// Notes in view whose duration is narrower than a notehead at this
    /// zoom. Reported, not re-spaced.
    std::size_t crowded_note_count = 0;

    void clear();
};

/// Lays out a score.
class LayoutEngine {
public:
    LayoutEngine();
    LayoutEngine(GlyphTable glyphs, LayoutOptions options);

    void set_options(const LayoutOptions& options) { options_ = options; }
    const LayoutOptions& options() const noexcept { return options_; }

    void set_glyph_table(GlyphTable glyphs) { glyphs_ = std::move(glyphs); }
    const GlyphTable& glyph_table() const noexcept { return glyphs_; }

    void set_staff_metrics(const StaffMetrics& metrics) { staff_prototype_ = metrics; }
    const StaffMetrics& staff_metrics() const noexcept { return staff_prototype_; }

    /// Lays out the tick range `[from_tick, to_tick)`.
    ///
    /// Rendering only the visible range is what keeps a long score
    /// affordable: a thousand-bar score costs the same as a four-bar one at
    /// the same zoom.
    ///
    /// That was written as an intention and was not true. The range query
    /// underneath walked every note on the staff, so the cost followed the
    /// *cue* rather than the window: measured on a fixed four-bar window,
    /// 125 microseconds at four bars against 1205 at eight thousand. The
    /// query now binary-searches its start and stops at its end, and the
    /// same measurement is flat - about 119 microseconds at every size. The
    /// view lays out on every playhead move, so this is the promise worth
    /// keeping rather than restating.
    LayoutResult layout(const notation::Score& score, const TimelineMap& timeline,
                        notation::Ticks from_tick, notation::Ticks to_tick) const;

private:
    struct StaffContext {
        std::size_t index = 0;
        StaffMetrics metrics{};
        notation::Clef clef = notation::Clef::Treble;
    };

    /// A group of notes sharing a start tick on one staff and voice: a chord
    /// if more than one.
    ///
    /// The parallel vectors are filled in together by `build_chords` and are
    /// always the same length as `notes`.
    struct Chord {
        notation::Ticks start_tick = 0;
        notation::Ticks end_tick = 0;
        std::uint8_t voice = 0;

        /// Bottom to top, which is how a chord is read and how the offset
        /// and accidental rules are stated.
        std::vector<const notation::ScoreNote*> notes;
        std::vector<int> positions;
        std::vector<bool> needs_accidental;
        std::vector<bool> offset;

        /// True for a note whose head another voice's chord draws: a
        /// unison of one value between the voices shares one head, with
        /// the up stem on its right and the down stem on its left.
        std::vector<bool> shared_head;

        /// Where each note's augmentation dots go, as a staff position:
        /// a space, chosen by the voice where two voices share a measure.
        std::vector<int> dot_positions;

        /// x of the dot column, shared by every chord at this tick so the
        /// dots of two voices line up clear of both voices' heads.
        double dot_x = 0.0;

        /// The chord whose heads this one shares, as an index into the
        /// chord vector, or SIZE_MAX. Moved with it.
        std::size_t shares_heads_with = static_cast<std::size_t>(-1);

        /// Another voice shares the measure, so the voice rather than the
        /// pitches decided the stem, and articulations and ties go on the
        /// voice's outer side.
        bool in_shared_measure = false;

        bool stem_up = true;
        int beam_count = 0;

        /// Part of a beam group of two or more, so it takes no flag.
        bool beamed = false;

        /// A grace chord: the grace notes of one order before this tick in
        /// this voice, drawn small to the left of the chord they ornament,
        /// stem up, flagged, never beamed, and slashed for an acciaccatura.
        bool grace = false;
        std::uint8_t grace_order = 0;
        bool grace_slashed = true;

        /// The staff the chord belongs to, and the one it is drawn on: the
        /// same unless its notes cross to the staff above or below
        /// (`ScoreNote::staff_shift`). Its positions are on the drawn
        /// staff; its laid-out notes report the home staff.
        std::size_t home_staff = 0;
        const StaffContext* drawn_on = nullptr;

        double x = 0.0;

        /// Filled in by layout: where the stem is and where its free end
        /// sits, so a beam can be drawn against the same numbers the stem
        /// was drawn with rather than recomputing them.
        double stem_x = 0.0;
        double stem_end_y = 0.0;
        bool has_stem_end_override = false;
    };

    /// Chords beamed together, with the line their stems end on.
    struct BeamGroup {
        std::vector<std::size_t> chords;  ///< Indices into the chord vector.
        bool stem_up = true;
        Point start{};
        Point end{};

        /// Members drawn on both staves of a grand staff: the beam runs
        /// flat between the staves and each stem reaches it from its own
        /// side, the upper staff's down and the lower staff's up.
        bool cross_staff = false;

        double y_at(double x) const noexcept;
    };

    void layout_staff_lines(const StaffContext& staff, double from_x, double to_x,
                            LayoutResult& result) const;

    /// Lays out the clef, key signature and time signature, and returns the
    /// width they occupy.
    ///
    /// One function for measuring and for drawing, selected by whether
    /// `staff` and `result` are given, because the left margin has to be
    /// sized from the width before anything can be drawn at it - and two
    /// functions doing the same arithmetic would drift apart.
    double layout_header(const notation::Score& score, const StaffContext* staff,
                         notation::Ticks at_tick, double left_x, LayoutResult* result) const;

    /// Where the grace notes at a tick reach to on the left, across the
    /// staves: a barline at that tick moves left of them, since grace
    /// notes belong after the barline, with the note they ornament.
    using GraceEdges = std::map<notation::Ticks, double>;

    void layout_barlines(const notation::Score& score, const TimelineMap& timeline,
                         const std::vector<StaffContext>& staves, notation::Ticks from_tick,
                         notation::Ticks to_tick, const GraceEdges& grace_edges,
                         LayoutResult& result) const;

    void layout_ruler(const notation::Score& score, const TimelineMap& timeline,
                      notation::Ticks from_tick, notation::Ticks to_tick,
                      LayoutResult& result) const;

    /// Groups a staff's visible notes into chords, resolving staff
    /// positions, second-interval offsets and which notes need an
    /// accidental. Notes before `from_tick` in the same measure are walked
    /// for their effect on accidental state but are not returned.
    ///
    /// `shared_measures` are the measures, sorted, in which more than one
    /// voice is written. In those the stems go by voice rather than by
    /// pitch, and a down-stem voice whose notes would touch the up-stem
    /// voice's is moved a notehead to the right.
    std::vector<Chord> build_chords(const notation::Score& score, const StaffContext& staff,
                                    const std::vector<StaffContext>& staves,
                                    const TimelineMap& timeline,
                                    const std::vector<notation::ScoreNote>& notes,
                                    notation::Ticks from_tick,
                                    const std::vector<std::int32_t>& shared_measures,
                                    GraceEdges& grace_edges) const;

    /// Assigns beam groups by beat and computes each group's beam line,
    /// which also fixes the stem lengths of its chords.
    std::vector<BeamGroup> build_beam_groups(const notation::Score& score,
                                             const StaffContext& staff,
                                             std::vector<Chord>& chords) const;

    /// Draws a grace chord: cue-sized heads and accidental, a short up
    /// stem with its flag, and the slash. `chord.x` was placed by
    /// `build_chords` to the left of the chord it ornaments.
    void layout_grace_chord(const StaffContext& staff, Chord& chord, LayoutResult& result) const;

    /// Places `voice`'s grace chords at this tick leftwards from
    /// `right_edge`, the highest order nearest it.
    void place_grace_chords(std::vector<Chord>& chords, std::size_t first_at_tick,
                            std::uint8_t voice, double right_edge) const;

    void layout_chord(const StaffContext& staff, const TimelineMap& timeline, Chord& chord,
                      LayoutResult& result) const;

    /// `shared` says the rest's measure has more than one voice in it, so
    /// the rest moves a space toward its voice's stem side, clear of the
    /// other voice's rests - and further, when a note of another voice in
    /// `notes` sounds through it close enough to be printed through.
    void layout_rest(const StaffContext& staff, const TimelineMap& timeline,
                     const notation::ScoreNote& rest, bool shared,
                     const std::vector<notation::ScoreNote>& notes, LayoutResult& result) const;

    void layout_beams(const StaffContext& staff, const std::vector<Chord>& chords,
                      const std::vector<BeamGroup>& groups, LayoutResult& result) const;

    /// One event a tuplet group is made of: a chord, or a rest, since a
    /// rest inside a triplet is still one of its three.
    struct TupletEvent {
        notation::Ticks start_tick = 0;
        notation::Ticks end_tick = 0;
        std::uint8_t voice = 0;
        notation::Tuplet ratio{};
        /// The ratio `ratio` sits inside, for a tuplet nested in another;
        /// 1:1 otherwise.
        notation::Tuplet outer{};
        notation::EventId id = notation::kInvalidEventId;

        /// Index into the chord vector, or `kNoChord` for a rest.
        std::size_t chord = kNoChord;
        double x = 0.0;
        double width = 0.0;

        static constexpr std::size_t kNoChord = static_cast<std::size_t>(-1);
    };

    /// A run of tuplet events under one number.
    struct TupletGroup {
        std::vector<std::size_t> events;  ///< Indices into the event vector.
        notation::Tuplet ratio{};
        /// True when the group closed on a whole tuplet span. One that did
        /// not - a lone triplet eighth - is still bracketed, because the
        /// composer needs to see what it is.
        bool complete = false;
        /// 0 for a tuplet that is in no other, or the outer one of a nested
        /// pair; 1 for the tuplet inside it, drawn nearest the notes.
        int level = 0;
    };

    /// Collects the staff's visible chords and rests into tuplet groups, by
    /// voice, in tick order. A group runs while the events share a ratio,
    /// follow each other without a gap and stay inside one measure, and
    /// closes when their sounding length is `normal` note values of some
    /// written value no shorter than the shortest in the group - which is
    /// what "three eighths in the time of two" adds up to.
    ///
    /// Grouped at two levels. The outer level reads each event by its
    /// outermost ratio - a nested tuplet's `outer`, a plain tuplet's own -
    /// so a triplet of eighths whose middle eighth is a triplet of
    /// sixteenths is one group of five events under one 3; the inner level
    /// reads only the nested events, by their own ratio, with the span
    /// measured in the outer tuplet's time, so the three sixteenths close
    /// as a whole triplet of their own. Inner groups come after the outer
    /// ones in the result.
    std::vector<TupletGroup> build_tuplet_groups(const notation::Score& score,
                                                 const TimelineMap& timeline,
                                                 const std::vector<notation::ScoreNote>& notes,
                                                 notation::Ticks from_tick,
                                                 const std::vector<Chord>& chords,
                                                 std::vector<TupletEvent>& events) const;

    /// Draws each group's number, and a bracket around it unless a single
    /// beam already holds the group together, on the stem side and clear of
    /// the staff. Inner groups first, nearest the notes; an outer group
    /// that holds a nested one is always bracketed, and clears the inner
    /// numbers and brackets on its side.
    void layout_tuplets(const StaffContext& staff, const std::vector<Chord>& chords,
                        const std::vector<BeamGroup>& beams,
                        const std::vector<TupletEvent>& events,
                        const std::vector<TupletGroup>& groups, LayoutResult& result) const;

    void layout_ties(const notation::Score& score, const StaffContext& staff,
                     const TimelineMap& timeline, const std::vector<Chord>& chords,
                     LayoutResult& result) const;

    /// Draws the staff's slurs, each fitted to the notes it joins: from
    /// the first note under it to the last, on the side away from their
    /// stems - or on the stem side, from stem end to stem end, where the
    /// stems disagree or the voice decides - and high enough to clear
    /// what lies between. A slur with fewer than two notes under it is one
    /// flat arch above the staff, as every slur used to be.
    /// `shared_measures` says where the voice, rather than the pitches,
    /// chose the stems.
    void layout_slurs(const notation::Score& score, const StaffContext& staff,
                      const TimelineMap& timeline, notation::Ticks from_tick,
                      notation::Ticks to_tick, const std::vector<std::int32_t>& shared_measures,
                      LayoutResult& result) const;

    /// How far out from a staff the markings have to sit, on the side
    /// `below` says, to clear the notes and articulations already drawn on
    /// it. Dynamics at a fixed offset collide with low notes and with marks
    /// below the staff, which is the one collision case common enough to be
    /// worth avoiding.
    double marking_baseline(const StaffContext& staff, bool below,
                            const LayoutResult& result) const;

    /// The two baselines a staff's dynamics and hairpins are drawn on:
    /// above it and below it. Which a marking takes is its voice's business
    /// (`marking_goes_below`).
    struct MarkingBaselines {
        double above = 0.0;
        double below = 0.0;
    };

    /// What the marking layout knows about each staff: where the baselines
    /// are, which measures two voices share, and which side a marking goes
    /// on when its voice has nothing to say - the placement option on a
    /// single staff; between the staves on a grand staff, which is below
    /// every staff but the last and above that one.
    struct MarkingStaff {
        MarkingBaselines baselines;
        std::vector<std::int32_t> shared_measures;
        bool default_below = true;
    };

    /// Which side of its staff a marking covering `from_tick` to `to_tick`
    /// in `voice` on `staff` goes.
    ///
    /// Where the marking's measure is one voice's, the staff's default
    /// decides. Where another voice shares the measure (as for the stems),
    /// the marking goes on its voice's outer side like the voice's slurs
    /// and marks: a down-stem voice's below, an up-stem voice's above -
    /// except voice 0, which is the staff's and keeps the default unless a
    /// down-stem voice has a marking of its own on this staff meeting this
    /// one, in which case this one is the upper voice's and goes above.
    bool marking_goes_below(const notation::Score& score, const MarkingStaff& staff,
                            std::uint8_t staff_index, std::uint8_t voice,
                            notation::Ticks from_tick, notation::Ticks to_tick) const;

    /// Draws the score's text markings above the top staff at their
    /// ticks, lifted clear of the notes and stems under them.
    void layout_texts(const notation::Score& score, const TimelineMap& timeline,
                      const StaffContext& staff, notation::Ticks from_tick,
                      notation::Ticks to_tick, LayoutResult& result) const;

    /// Draws the dynamics, each on its voice's side; where two voices'
    /// dynamics meet at one tick on one side, the higher-numbered voice's
    /// goes a row further out rather than over the other.
    void layout_dynamics(const notation::Score& score, const TimelineMap& timeline,
                         notation::Ticks from_tick, notation::Ticks to_tick,
                         const std::vector<MarkingStaff>& staves, LayoutResult& result) const;

    /// Width, in staff spaces, of the dynamic marking written at `tick` in
    /// `voice`, or zero when there is none. What a hairpin sharing the tick
    /// gives way to: another voice's dynamic is on another side or row.
    double dynamic_width_at(const notation::Score& score, notation::Ticks tick,
                            std::uint8_t voice, std::uint8_t staff) const;

    /// Draws the hairpins, each on its voice's side, and a row further out
    /// where it would otherwise cross a lower-numbered voice's hairpin.
    void layout_hairpins(const notation::Score& score, const TimelineMap& timeline,
                         notation::Ticks from_tick, notation::Ticks to_tick,
                         const std::vector<MarkingStaff>& staves, LayoutResult& result) const;

    /// Draws a note's articulations: on the side away from the stem, or -
    /// where `on_stem_side` says another voice shares the measure - on the
    /// voice's outer side, beyond the stem's free end at `stem_end_y`.
    void layout_articulations(const StaffContext& staff, const notation::ScoreNote& note,
                              const Point& notehead_centre, bool stem_up, bool on_stem_side,
                              double stem_end_y, LayoutResult& result) const;

    /// Emits a glyph with its bounds filled in from the glyph table.
    /// Records a missing glyph rather than emitting a blank.
    void emit_glyph(Glyph glyph, const Point& origin, ItemRole role,
                    notation::EventId source_id, LayoutResult& result,
                    double scale = 1.0) const;

    /// Advance width of a glyph, in staff spaces.
    double advance_of(Glyph glyph) const noexcept;

    /// x of the stem for a chord: the right edge of the notehead column for
    /// an up stem, the left edge for a down stem.
    double stem_x_for(const Chord& chord, const StaffMetrics& metrics) const noexcept;

    Glyph notehead_for(const notation::Duration& duration) const noexcept;
    Glyph rest_for(const notation::Duration& duration) const noexcept;
    Glyph flag_for(const notation::Duration& duration, bool stem_up) const noexcept;
    Glyph accidental_for(int alteration) const noexcept;
    Glyph clef_glyph_for(notation::Clef clef) const noexcept;

    /// Staff position at which a clef is drawn: its reference line.
    int clef_staff_position(notation::Clef clef) const noexcept;

    GlyphTable glyphs_;
    LayoutOptions options_{};
    StaffMetrics staff_prototype_{};
};

/// The number of beams a written value needs: one for an eighth, two for a
/// sixteenth, and so on. 0 for a quarter or longer.
int beam_count_for(const notation::Duration& duration) noexcept;

/// What is written over a tuplet: "3" for a triplet, "5" for a quintuplet,
/// "2" for a duplet - the number alone whenever the ratio is the one a
/// reader assumes for it - and "5:3" where it is not.
std::string tuplet_label(const notation::Tuplet& ratio);

/// True when `events` of `ratio` whose sounding lengths add up to
/// `sounding_ticks` and whose shortest written value is `shortest_written`
/// make a whole tuplet: `normal` written values of some value no shorter
/// than the shortest written. Three triplet eighths sound as two eighths;
/// three sextuplet sixteenths sound as two sixteenths as well, but they are
/// not a whole 6:4, and the shortest-value rule is what tells them apart.
bool tuplet_span_is_complete(const notation::Tuplet& ratio, notation::Ticks sounding_ticks,
                             notation::Ticks shortest_written) noexcept;

/// Conventional stem direction for a single note: up below the middle line,
/// down at or above it.
bool stem_up_for_position(int staff_position) noexcept;

/// Stem direction for a chord, decided by whichever note is furthest from
/// the middle line - the conventional rule.
bool stem_up_for_chord(const std::vector<int>& staff_positions) noexcept;

/// Stem direction for a voice sharing its staff with another: the first and
/// third voices up, the second and fourth down, whatever their pitches. That
/// is what tells the two lines apart on the page.
bool stem_up_for_voice(std::uint8_t voice) noexcept;

/// The measures, by number and in order, in which `notes` are written in
/// more than one voice. A note or rest counts in the measure it starts in.
/// Where a measure has one voice, that voice is engraved by pitch like any
/// single line; the voice rules apply only where there is another voice to
/// be told apart from.
std::vector<std::int32_t> measures_with_several_voices(
    const notation::Score& score, const std::vector<notation::ScoreNote>& notes);

/// Staff positions the accidentals of a key signature are drawn at, in the
/// conventional order (F C G D A E B for sharps, B E A D G C F for flats).
/// Writes at most `capacity` values and returns how many, which is
/// `abs(fifths)`.
std::size_t key_signature_positions(const notation::KeySignature& key, notation::Clef clef,
                                    int* out, std::size_t capacity) noexcept;

}  // namespace vcc::engraving
