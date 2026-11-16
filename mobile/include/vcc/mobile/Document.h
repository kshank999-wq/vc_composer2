// The iPad companion's window onto the shared core (Add-On spec §6.1: "a
// distinct application sharing a core score serialization layer and editing
// rules with the desktop product").
//
// Everything the iPad app does to a project goes through here, and this
// calls the same session, project, editing and engraving code the desktop
// uses: a file saved on iPad is the desktop's own project format, a note
// entered on iPad is entered by the desktop's score editor, and the score is
// engraved by the desktop's layout engine.
//
// The header uses the standard library only, so the Objective-C++ bridge in
// the app (and an XCFramework's public headers) needs nothing else. All
// geometry is in staff spaces, y down, as the engraving produces it.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "vcc/mobile/Handwriting.h"

namespace vcc::mobile {

// --- Drawing: the engraving's display list, flattened -----------------------

struct DrawGlyph {
    std::uint32_t codepoint = 0;  ///< In the SMuFL font (Bravura).
    double x = 0, y = 0, scale = 1.0;
    int role = 0;                 ///< engraving::ItemRole.
    std::uint64_t source = 0;     ///< The score event, 0 for structure.
};
struct DrawLine {
    double x1 = 0, y1 = 0, x2 = 0, y2 = 0, thickness = 0.125;
    int role = 0;
    std::uint64_t source = 0;
};
struct DrawQuad {
    double x[4] = {}, y[4] = {};
    int role = 0;
    std::uint64_t source = 0;
};
struct DrawCurve {
    double x[4] = {}, y[4] = {};  ///< Start, control, control, end.
    double thickness = 0.14, end_thickness = 0.05;
    bool tapered = false;
    int role = 0;
    std::uint64_t source = 0;
};
struct DrawText {
    std::string text;
    double x = 0, y = 0, size = 1.6;
    bool italic = false, bold = false;
    int role = 0;
};
struct Drawing {
    std::vector<DrawGlyph> glyphs;
    std::vector<DrawLine> lines;
    std::vector<DrawQuad> quads;
    std::vector<DrawCurve> curves;
    std::vector<DrawText> texts;
    /// Everything drawn (staff spaces): size a canvas from this.
    double left = 0, top = 0, right = 0, bottom = 0;
    /// y of the top line of each staff.
    std::vector<double> staff_top;
};

// --- The project ---------------------------------------------------------------

struct TrackInfo {
    std::string id;
    std::string name;
    std::string family;  ///< "strings", "woodwinds"...
    int staves = 1;
    /// Expanded to articulation tracks on the desktop: still one staff here.
    bool expanded = false;
};

/// Note values the entry palette offers, as notation::NoteValue numbers.
enum class Value : int { Whole = 1, Half = 2, Quarter = 3, Eighth = 4, Sixteenth = 5, ThirtySecond = 6 };

/// Where a tap or a written notehead lands: a staff, a grid tick and the
/// pitch that staff position spells in the clef and key there.
struct Placement {
    bool valid = false;
    std::int64_t tick = 0;
    int staff = 0;
    int staff_position = 0;  ///< 0 = bottom line, steps upward.
    int midi = 60;           ///< Sounding.
    std::string pitch_name;  ///< Written: "F#4".
    /// The written pitch the staff position spells (step 0 = C .. 6 = B).
    int step = 0, octave = 4, alteration = 0;
};

/// A note as it plays, for the iPad's own lightweight audition.
struct PlayedNote {
    double start = 0, duration = 0;  ///< Seconds.
    int pitch = 60, velocity = 80;
    std::string track;
};

class Document {
public:
    /// `content_dir` holds the shipped content (instruments, templates):
    /// the app bundles the desktop's content/ folder. `font_metadata` is
    /// Bravura's bravura_metadata.json (empty: look beside the content, as
    /// the source tree has it).
    static std::unique_ptr<Document> create(const std::string& content_dir, const std::string& font_metadata = {},
                                            std::string* error = nullptr);
    ~Document();
    Document(const Document&) = delete;
    Document& operator=(const Document&) = delete;

    // Files: the desktop's project format, unchanged.
    void new_empty(const std::string& first_instrument = "keys.piano");
    bool new_from_template(const std::string& template_id, std::string* error = nullptr);
    bool load(const std::string& path, std::string* error = nullptr);
    bool save_to(const std::string& path, std::string* error = nullptr);

    std::string title() const;
    std::vector<TrackInfo> tracks() const;
    std::string add_track(const std::string& name, const std::string& instrument_id);
    int bar_count() const;
    double tempo_bpm() const;
    /// The project's time base (ticks per quarter note).
    static std::int64_t ticks_per_quarter();
    std::string meter() const;  ///< "4/4" at the start.
    std::string key() const;    ///< "D major".

    /// Engraves a track as one continuous system (the desktop's Composer
    /// lane), `spaces_per_quarter` wide per quarter note.
    Drawing layout_track(const std::string& track_id, double spaces_per_quarter = 6.0);

    /// Hit testing against the track's last layout.
    Placement place(const std::string& track_id, double x, double y, Value grid = Value::Sixteenth) const;
    std::uint64_t note_at(const std::string& track_id, double x, double y) const;
    /// Where the last layout puts a tick, and a staff position on a staff
    /// (for the entry preview's shadow note).
    double x_for_tick(const std::string& track_id, std::int64_t tick) const;
    double y_for(const std::string& track_id, int staff, int staff_position) const;

    // Editing: the desktop's score editor; each call is one undo step.
    /// `alteration` other than `kKeyAccidental` overrides the key's (-2..2).
    static constexpr int kKeyAccidental = 99;
    bool enter_note(const std::string& track_id, const Placement& at, Value value, int dots = 0,
                    int alteration = kKeyAccidental);
    bool enter_rest(const std::string& track_id, const Placement& at, Value value, int dots = 0);
    bool remove_note(const std::string& track_id, std::uint64_t note);
    /// -2..2: double flat .. double sharp, 0 natural.
    bool set_accidental(const std::string& track_id, std::uint64_t note, int alteration);
    /// Handwriting: strokes written over the track's last layout, read into
    /// a preview (nothing changes yet). The composer corrects it - choosing a
    /// doubtful head's other reading, say - and commits it.
    Recognition recognize(const std::string& track_id, const std::vector<Stroke>& strokes) const;
    /// Writes a (corrected) recognition into the score as one undo step.
    bool commit(const std::string& track_id, const Recognition& recognition);

    bool undo();
    bool redo();
    bool can_undo() const;
    bool can_redo() const;
    std::string undo_label() const;
    /// Changes on every edit: redraw when it moves.
    std::uint64_t revision() const;

    /// Every staff's notes as they play (one part once, as the desktop plays it).
    std::vector<PlayedNote> performance() const;

private:
    Document();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace vcc::mobile
