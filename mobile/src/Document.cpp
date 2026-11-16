#include "vcc/mobile/Document.h"

#include <algorithm>
#include <map>

#include "vcc/core/Files.h"
#include "vcc/core/Json.h"
#include "vcc/editing/ScoreEditor.h"
#include "vcc/engraving/HitTest.h"
#include "vcc/engraving/LayoutEngine.h"
#include "vcc/engraving/Smufl.h"
#include "vcc/engraving/TimelineMap.h"
#include "vcc/project/Render.h"
#include "vcc/session/Session.h"

namespace vcc::mobile {

namespace nt = notation;
namespace en = engraving;

namespace {

/// SMuFL metadata (advance widths and bounding boxes) over the built-in
/// defaults, as the desktop's music font loader applies it.
void apply_font_metadata(const std::string& path, en::GlyphTable& table)
{
    const auto text = files::read_file(path);
    if (!text) return;
    const auto doc = json::parse_value(*text);
    if (const auto* widths = doc.get("glyphAdvanceWidths"); widths && widths->is_object())
        for (const auto& [name, value] : widths->as_object()) {
            en::Glyph glyph = en::Glyph::None;
            if (!en::glyph_from_smufl_name(name, glyph) || !value.is_number() || !(value.as_double() > 0.0)) continue;
            auto m = table.metrics(glyph);
            m.advance_width = value.as_double();
            table.set_metrics(glyph, m);
        }
    if (const auto* boxes = doc.get("glyphBBoxes"); boxes && boxes->is_object())
        for (const auto& [name, box] : boxes->as_object()) {
            en::Glyph glyph = en::Glyph::None;
            if (!en::glyph_from_smufl_name(name, glyph)) continue;
            const auto* ne = box.get("bBoxNE");
            const auto* sw = box.get("bBoxSW");
            if (!ne || !sw || ne->as_array().size() != 2 || sw->as_array().size() != 2) continue;
            auto m = table.metrics(glyph);
            m.bbox_right = ne->as_array()[0].as_double();
            m.bbox_top = -ne->as_array()[1].as_double();  // SMuFL measures up; the engine down
            m.bbox_left = sw->as_array()[0].as_double();
            m.bbox_bottom = -sw->as_array()[1].as_double();
            table.set_metrics(glyph, m);
        }
}

std::string pitch_name(const nt::Pitch& p)
{
    std::string s = nt::to_string(p.step);
    for (int i = 0; i < p.alteration; ++i) s += "#";
    for (int i = 0; i > p.alteration; --i) s += "b";
    return s + std::to_string(p.octave);
}

}  // namespace

struct Document::Impl {
    std::unique_ptr<session::Catalogs> catalogs;
    std::unique_ptr<session::Session> session;
    en::LayoutEngine engine;
    /// Each track's last layout and the timeline it used, for hit testing.
    struct Laid {
        en::LayoutResult result;
        en::TimelineMap timeline;
    };
    std::map<std::string, Laid> laid;

    const project::Track* track(const std::string& id) const { return session->data().find_track(id); }

    /// Runs the desktop's score editor on a copy and commits it as one step.
    template <typename Fn>
    bool edit(const std::string& track_id, const std::string& label, Fn&& fn)
    {
        const auto* t = track(track_id);
        if (!t || !t->active_version()) return false;
        nt::Score score = t->active_version()->score;
        editing::ScoreEditor editor(score);
        if (!fn(editor, score)) return false;
        return session->commit_score(track_id, score, label);
    }
};

Document::Document() : impl_(std::make_unique<Impl>()) {}
Document::~Document() = default;

std::unique_ptr<Document> Document::create(const std::string& content_dir, const std::string& font_metadata,
                                           std::string* error)
{
    std::unique_ptr<Document> d(new Document());
    d->impl_->catalogs = session::Catalogs::load_from_directory(content_dir);
    if (!d->impl_->catalogs || d->impl_->catalogs->instruments.all().empty()) {
        if (error) *error = "The instrument and template content was not found in " + content_dir;
        return nullptr;
    }
    d->impl_->session = std::make_unique<session::Session>(*d->impl_->catalogs);
    en::GlyphTable glyphs;
    apply_font_metadata(font_metadata.empty() ? content_dir + "/../resources/fonts/Bravura/bravura_metadata.json" : font_metadata,
                        glyphs);
    d->impl_->engine.set_glyph_table(std::move(glyphs));
    d->new_empty();
    return d;
}

void Document::new_empty(const std::string& first_instrument)
{
    impl_->session->new_empty();
    impl_->laid.clear();
    if (!first_instrument.empty())
        if (const auto* def = impl_->catalogs->instruments.find(first_instrument))
            impl_->session->add_track(def->name, first_instrument);
}

bool Document::new_from_template(const std::string& template_id, std::string* error)
{
    impl_->laid.clear();
    const auto r = impl_->session->new_from_template(template_id);
    if (!r.ok && error) *error = r.error;
    return r.ok;
}

bool Document::load(const std::string& path, std::string* error)
{
    impl_->laid.clear();
    return impl_->session->open(path, error);
}

bool Document::save_to(const std::string& path, std::string* error) { return impl_->session->save_as(path, error); }

std::string Document::title() const { return impl_->session->data().name; }

std::vector<TrackInfo> Document::tracks() const
{
    std::vector<TrackInfo> out;
    for (const auto* t : impl_->session->staff_tracks()) {
        TrackInfo i;
        i.id = t->id;
        i.name = t->name;
        i.family = t->family;
        i.staves = t->active_version() ? static_cast<int>(t->active_version()->score.staff_count()) : 1;
        i.expanded = t->articulations_expanded;
        out.push_back(std::move(i));
    }
    return out;
}

std::string Document::add_track(const std::string& name, const std::string& instrument_id)
{
    return impl_->session->add_track(name, instrument_id);
}

int Document::bar_count() const
{
    const auto& d = impl_->session->data();
    return std::max(4, d.meter.bar_at(std::max<Ticks>(0, impl_->session->content_end() - 1)) + 1);
}

double Document::tempo_bpm() const { return impl_->session->data().tempo.bpm_at(0); }
std::string Document::meter() const { return impl_->session->data().meter.at_bar(1).to_string(); }
std::string Document::key() const { return impl_->session->data().key; }

Drawing Document::layout_track(const std::string& track_id, double spaces_per_quarter)
{
    Drawing out;
    const auto* t = impl_->track(track_id);
    if (!t || !t->active_version()) return out;
    const nt::Score& score = t->active_version()->score;
    Impl::Laid laid;
    laid.timeline.set_staff_spaces_per_quarter(spaces_per_quarter);
    const auto& d = impl_->session->data();
    const Ticks end = d.meter.bar_start(bar_count() + 1);
    laid.result = impl_->engine.layout(score, laid.timeline, 0, end);
    const auto& list = laid.result.display_list;
    const auto& glyphs = impl_->engine.glyph_table();
    for (const auto& g : list.glyphs)
        out.glyphs.push_back({static_cast<std::uint32_t>(glyphs.metrics(g.glyph).codepoint), g.origin.x, g.origin.y, g.scale,
                              static_cast<int>(g.role), g.source_id});
    for (const auto& l : list.lines)
        out.lines.push_back({l.from.x, l.from.y, l.to.x, l.to.y, l.thickness, static_cast<int>(l.role), l.source_id});
    for (const auto& q : list.quads) {
        DrawQuad o;
        for (int i = 0; i < 4; ++i) o.x[i] = q.corners[i].x, o.y[i] = q.corners[i].y;
        o.role = static_cast<int>(q.role);
        o.source = q.source_id;
        out.quads.push_back(o);
    }
    for (const auto& c : list.curves) {
        DrawCurve o;
        const en::Point pts[4] = {c.start, c.control_one, c.control_two, c.end};
        for (int i = 0; i < 4; ++i) o.x[i] = pts[i].x, o.y[i] = pts[i].y;
        o.thickness = c.thickness;
        o.end_thickness = c.end_thickness;
        o.tapered = c.tapered;
        o.role = static_cast<int>(c.role);
        o.source = c.source_id;
        out.curves.push_back(o);
    }
    for (const auto& x : list.texts)
        out.texts.push_back({x.text, x.origin.x, x.origin.y, x.size, x.italic, x.bold, static_cast<int>(x.role)});
    const auto& b = laid.result.drawn_bounds;
    const auto& e = laid.result.extent;
    out.left = std::min(b.x, e.x);
    out.top = std::min(b.y, e.y);
    out.right = std::max(b.x + b.width, e.x + e.width);
    out.bottom = std::max(b.y + b.height, e.y + e.height);
    out.staff_top = laid.result.staff_top_y;
    impl_->laid[track_id] = std::move(laid);
    return out;
}

Placement Document::place(const std::string& track_id, double x, double y, Value grid) const
{
    Placement p;
    const auto it = impl_->laid.find(track_id);
    const auto* t = impl_->track(track_id);
    if (it == impl_->laid.end() || !t || !t->active_version()) return p;
    const nt::Score& score = t->active_version()->score;
    const Ticks quantum = nt::Duration{static_cast<nt::NoteValue>(static_cast<int>(grid))}.ticks();
    const auto at = en::insertion_point_at(it->second.result, score, it->second.timeline, en::Point{x, y}, quantum);
    if (!at.valid) return p;
    p.valid = true;
    p.tick = at.tick;
    p.staff = static_cast<int>(at.staff_index);
    p.staff_position = at.staff_position;
    p.midi = nt::midi_note_number(at.pitch) + score.staff(at.staff_index).total_transposition();
    p.pitch_name = pitch_name(at.pitch);
    p.step = static_cast<int>(at.pitch.step);
    p.octave = at.pitch.octave;
    p.alteration = at.pitch.alteration;
    return p;
}

std::uint64_t Document::note_at(const std::string& track_id, double x, double y) const
{
    const auto it = impl_->laid.find(track_id);
    if (it == impl_->laid.end()) return 0;
    const auto hit = en::hit_test(it->second.result, en::Point{x, y});
    return hit.is_note() ? hit.id : 0;
}

double Document::x_for_tick(const std::string& track_id, std::int64_t tick) const
{
    const auto it = impl_->laid.find(track_id);
    return it == impl_->laid.end() ? 0.0 : it->second.timeline.x_for_tick(tick);
}

double Document::y_for(const std::string& track_id, int staff, int staff_position) const
{
    const auto it = impl_->laid.find(track_id);
    if (it == impl_->laid.end() || staff < 0 || static_cast<std::size_t>(staff) >= it->second.result.staff_top_y.size()) return 0.0;
    // Staff position 0 is the bottom line, four spaces below the top one.
    return it->second.result.staff_top_y[static_cast<std::size_t>(staff)] + 4.0 - 0.5 * staff_position;
}

bool Document::enter_note(const std::string& track_id, const Placement& at, Value value, int dots, int alteration)
{
    if (!at.valid) return false;
    return impl_->edit(track_id, "Enter note", [&](editing::ScoreEditor& ed, nt::Score& score) {
        if (static_cast<std::size_t>(at.staff) >= score.staff_count()) return false;
        ed.set_duration(static_cast<nt::NoteValue>(static_cast<int>(value)));
        ed.set_dots(dots);
        nt::Pitch pitch;
        pitch.step = static_cast<nt::Step>(std::clamp(at.step, 0, 6));
        pitch.octave = static_cast<std::int8_t>(at.octave);
        pitch.alteration = static_cast<std::int8_t>(alteration == kKeyAccidental ? at.alteration : std::clamp(alteration, -2, 2));
        return ed.insert_at(static_cast<std::size_t>(at.staff), at.tick, pitch).succeeded();
    });
}

bool Document::enter_rest(const std::string& track_id, const Placement& at, Value value, int dots)
{
    if (!at.valid) return false;
    return impl_->edit(track_id, "Enter rest", [&](editing::ScoreEditor& ed, nt::Score& score) {
        if (static_cast<std::size_t>(at.staff) >= score.staff_count()) return false;
        editing::EditCursor c;
        c.staff_index = static_cast<std::size_t>(at.staff);
        c.tick = at.tick;
        ed.set_cursor(c);
        ed.set_duration(static_cast<nt::NoteValue>(static_cast<int>(value)));
        ed.set_dots(dots);
        return ed.insert_rest().succeeded();
    });
}

bool Document::remove_note(const std::string& track_id, std::uint64_t note)
{
    return impl_->edit(track_id, "Delete note", [&](editing::ScoreEditor& ed, nt::Score& score) {
        if (!score.find_note(note)) return false;
        ed.set_selection({note});
        return ed.delete_selection().succeeded();
    });
}

bool Document::set_accidental(const std::string& track_id, std::uint64_t note, int alteration)
{
    return impl_->edit(track_id, "Accidental", [&](editing::ScoreEditor& ed, nt::Score& score) {
        if (!score.find_note(note)) return false;
        ed.set_selection({note});
        return ed.set_accidental(std::clamp(alteration, -2, 2)).succeeded();
    });
}

bool Document::undo() { return impl_->session->undo(); }
bool Document::redo() { return impl_->session->redo(); }
bool Document::can_undo() const { return impl_->session->can_undo(); }
bool Document::can_redo() const { return impl_->session->can_redo(); }
std::string Document::undo_label() const { return impl_->session->undo_label(); }
std::uint64_t Document::revision() const { return impl_->session->change_count(); }

std::vector<PlayedNote> Document::performance() const
{
    std::vector<PlayedNote> out;
    const auto& d = impl_->session->data();
    for (const auto* t : impl_->session->staff_tracks()) {
        if (t->mute) continue;
        const auto r = project::render_track(*t);
        for (const auto& n : r.notes) {
            PlayedNote p;
            p.start = d.tempo.seconds_at(n.start);
            p.duration = std::max(0.01, d.tempo.seconds_at(n.end()) - p.start);
            p.pitch = n.pitch;
            p.velocity = n.velocity;
            p.track = t->id;
            out.push_back(std::move(p));
        }
    }
    std::sort(out.begin(), out.end(), [](const PlayedNote& a, const PlayedNote& b) { return a.start < b.start; });
    return out;
}

}  // namespace vcc::mobile
