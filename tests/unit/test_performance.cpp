// Performance overrides (Main Spec §8): per-note onset, length, velocity and
// CC offsets keyed by stable score note id, kept apart from the score.
#include <algorithm>

#include "vcc/core/Json.h"
#include "vcc/editing/ScoreEditor.h"
#include "vcc/notation/ScoreFile.h"
#include "vcc/project/Performance.h"
#include "vcc/project/Render.h"
#include "vcc/session/Session.h"
#include "vcc_test.h"

using namespace vcc;
using project::NoteOverride;

namespace {
const session::Catalogs& catalogs()
{
    static auto c = session::Catalogs::load_from_directory(VCC_CONTENT_DIR);
    return *c;
}
std::string tmp(const std::string& name) { return std::string(VCC_TEST_TMP_DIR) + "/" + name; }

notation::Pitch P(notation::Step step, int octave = 4)
{
    notation::Pitch p;
    p.step = step;
    p.octave = static_cast<std::int8_t>(octave);
    return p;
}

/// Four quarter notes C D E F, the third tied into a fourth E (a tie chain
/// sounds as one note). Returns the ids in order of entry.
std::vector<notation::EventId> write_line(session::Session& s, const std::string& track)
{
    notation::Score score = s.data().find_track(track)->active_version()->score;
    editing::ScoreEditor editor(score);
    editing::EditCursor cursor;
    editor.set_cursor(cursor);
    editor.set_duration(notation::NoteValue::Quarter);
    std::vector<notation::EventId> ids;
    for (auto step : {notation::Step::C, notation::Step::D, notation::Step::E, notation::Step::E, notation::Step::F}) {
        auto r = editor.insert_pitch(P(step));
        if (!r.succeeded()) return {};
        ids.push_back(r.new_event);
    }
    editor.set_selection({ids[2]});
    if (!editor.toggle_tie().succeeded()) return {};
    if (!s.commit_score(track, score, "Enter notes")) return {};
    return ids;
}

const project::Track& track(const session::Session& s, const std::string& id) { return *s.data().find_track(id); }

/// The rendered note that came from score note `id`.
NoteEvent note_of(const project::RenderedTrack& r, notation::EventId id)
{
    for (std::size_t i = 0; i < r.notes.size(); ++i)
        if (r.note_ids[i] == id) return r.notes[i];
    return {};
}
}  // namespace

TEST_CASE("rendered notes carry the id of the score note they came from")
{
    session::Session s(catalogs());
    s.new_empty();
    const std::string t = s.add_track("Flute", "woodwinds.flute");
    const auto ids = write_line(s, t);
    REQUIRE(ids.size() == 5);
    const auto r = project::render_track(track(s, t));
    REQUIRE(r.notes.size() == 4);  // the tie chain is one note
    REQUIRE(r.note_ids.size() == r.notes.size());
    CHECK_EQ(note_of(r, ids[0]).pitch, 60);  // a flute sounds as written
    CHECK_EQ(note_of(r, ids[2]).duration > note_of(r, ids[1]).duration, true);
    // Only notes that sound on their own can carry an override.
    const auto performable = project::performable_notes(track(s, t));
    CHECK_EQ(performable.size(), 4u);
    CHECK(std::find(performable.begin(), performable.end(), ids[3]) == performable.end());
}

TEST_CASE("overrides move timing, length and velocity without touching the score")
{
    session::Session s(catalogs());
    s.new_empty();
    const std::string t = s.add_track("Flute", "woodwinds.flute");
    const auto ids = write_line(s, t);
    REQUIRE(ids.size() == 5);
    const std::string score_before = json::write(notation::score_to_json(track(s, t).active_version()->score));
    const auto written = project::render_track(track(s, t));

    REQUIRE(s.edit_performance(t, {ids[1]}, "Nudge", [](notation::EventId, NoteOverride& o) {
        o.onset = -40;
        o.length = 300;
        o.velocity = 12;
    }));
    const auto played = project::render_track(track(s, t));
    const NoteEvent w = note_of(written, ids[1]);
    const NoteEvent p = note_of(played, ids[1]);
    CHECK_EQ(p.start, w.start - 40);
    CHECK_EQ(p.duration, w.duration + 300);
    CHECK_EQ(p.velocity, w.velocity + 12);
    CHECK_EQ(p.pitch, w.pitch);
    // Other notes are untouched, and the written render ignores overrides.
    CHECK(note_of(played, ids[0]) == note_of(written, ids[0]));
    CHECK(note_of(project::render_track(track(s, t), false), ids[1]) == w);
    // The score is exactly as it was: printed parts and MusicXML never see this.
    CHECK_EQ(json::write(notation::score_to_json(track(s, t).active_version()->score)), score_before);

    // A continuation of a tie chain and an unknown id are refused.
    CHECK(!s.edit_performance(t, {ids[3]}, "Nudge tie", [](notation::EventId, NoteOverride& o) { o.onset = 10; }));
    CHECK(!s.edit_performance(t, {999999}, "Nudge none", [](notation::EventId, NoteOverride& o) { o.onset = 10; }));

    // Undo returns the note to how it is written.
    REQUIRE(s.undo());
    CHECK(note_of(project::render_track(track(s, t)), ids[1]) == w);
    CHECK(s.performance_of(t)->empty());
}

TEST_CASE("overrides are clamped and an override back to zero disappears")
{
    session::Session s(catalogs());
    s.new_empty();
    const std::string t = s.add_track("Flute", "woodwinds.flute");
    const auto ids = write_line(s, t);
    REQUIRE(ids.size() == 5);
    REQUIRE(s.edit_performance(t, {ids[0]}, "Extreme", [](notation::EventId, NoteOverride& o) {
        o.onset = -100000;
        o.length = -100000;
        o.velocity = -500;
        o.cc = 900;
    }));
    const auto& o = s.performance_of(t)->at(ids[0]);
    CHECK_EQ(o.onset, -2 * kTicksPerQuarter);
    CHECK_EQ(o.velocity, -127);
    CHECK_EQ(o.cc, 127);
    const NoteEvent n = note_of(project::render_track(track(s, t)), ids[0]);
    CHECK_EQ(n.start, 0);       // never before the start of the project
    CHECK(n.duration >= 1);     // never shorter than a tick
    CHECK_EQ(n.velocity, 1);    // never silent

    REQUIRE(s.edit_performance(t, {ids[0]}, "Zero", [](notation::EventId, NoteOverride& x) { x = {}; }));
    CHECK(s.performance_of(t)->empty());
}

TEST_CASE("a drag coalesces into one undo step and a reset is one step")
{
    session::Session s(catalogs());
    s.new_empty();
    const std::string t = s.add_track("Flute", "woodwinds.flute");
    const auto ids = write_line(s, t);
    REQUIRE(ids.size() == 5);
    for (int i = 1; i <= 5; ++i)
        REQUIRE(s.edit_performance(t, {ids[0], ids[1]}, "Performance timing",
                                   [i](notation::EventId, NoteOverride& o) { o.onset = 10 * i; }));
    CHECK_EQ(s.performance_of(t)->size(), 2u);
    CHECK_EQ(s.performance_of(t)->at(ids[1]).onset, 50);
    CHECK(s.clear_performance(t, {ids[0]}));
    CHECK_EQ(s.performance_of(t)->size(), 1u);
    REQUIRE(s.undo());  // the reset
    CHECK_EQ(s.performance_of(t)->size(), 2u);
    REQUIRE(s.undo());  // the whole drag
    CHECK(s.performance_of(t)->empty());
    CHECK(!s.clear_performance(t));  // nothing to clear
}

TEST_CASE("humanize is seeded, per note, and keeps CC offsets")
{
    session::Session s(catalogs());
    s.new_empty();
    const std::string t = s.add_track("Flute", "woodwinds.flute");
    const auto ids = write_line(s, t);
    REQUIRE(ids.size() == 5);
    REQUIRE(s.edit_performance(t, {ids[0]}, "CC", [](notation::EventId, NoteOverride& o) { o.cc = 9; }));

    auto settings = project::humanize_preset("natural");
    settings.seed = 42;
    REQUIRE(s.humanize(t, {}, settings));
    const auto first = *s.performance_of(t);
    CHECK_EQ(first.size(), 4u);  // every sounding note, not the tie continuation
    CHECK_EQ(first.at(ids[0]).cc, 9);
    bool varied = false;
    for (const auto& [id, o] : first) {
        CHECK(std::abs(o.onset) <= settings.timing);
        CHECK(std::abs(o.velocity) <= settings.velocity);
        CHECK(std::abs(o.length) <= settings.length);
        varied = varied || o.onset != first.begin()->second.onset;
    }
    CHECK(varied);

    // The same seed gives the same result, whatever else is selected.
    REQUIRE(s.clear_performance(t));
    REQUIRE(s.humanize(t, {ids[1]}, settings));
    CHECK(s.performance_of(t)->at(ids[1]) == first.at(ids[1]));
    // Another seed gives another performance.
    settings.seed = 43;
    REQUIRE(s.humanize(t, {}, settings));
    bool differs = false;
    for (const auto& [id, o] : *s.performance_of(t)) differs = differs || !(o == first.at(id));
    CHECK(differs);
    // The presets differ in how far they go.
    CHECK(project::humanize_preset("subtle").timing < project::humanize_preset("loose").timing);
}

TEST_CASE("a CC offset reaches the note's own controllers and the dynamics controller")
{
    session::Session s(catalogs());
    s.new_empty();
    const std::string t = s.add_track("Violins", "strings.violins_1");
    REQUIRE(s.update_track_mix(t, "Map", [](project::Track& x) { x.expression_map = "orchestral_cc"; }));
    const auto ids = write_line(s, t);
    REQUIRE(ids.size() == 5);
    notation::Score score = track(s, t).active_version()->score;
    score.add_dynamic({0, notation::DynamicLevel::Piano, 0, {}, 0});
    REQUIRE(s.commit_score(t, score, "Dynamic"));
    const auto map = project::expression_map_named("orchestral_cc");
    const int cc = map.dynamics_cc;

    const auto written = project::render_track(track(s, t));
    auto value_at = [&](const project::RenderedTrack& r, Ticks tick) {
        int v = -1;
        for (const auto& c : r.controllers)
            if (c.controller == cc && c.tick <= tick) v = c.value;
        return v;
    };
    const NoteEvent second = note_of(written, ids[1]);
    const int base = value_at(written, second.start);
    REQUIRE(base >= 0);

    REQUIRE(s.edit_performance(t, {ids[1]}, "Lean", [](notation::EventId, NoteOverride& o) { o.cc = 20; }));
    const auto played = project::render_track(track(s, t));
    CHECK_EQ(value_at(played, second.start), std::min(127, base + 20));     // lifted while it sounds
    CHECK_EQ(value_at(played, second.end()), value_at(written, second.end()));  // and put back after
    CHECK_EQ(value_at(played, note_of(written, ids[0]).start), value_at(written, note_of(written, ids[0]).start));
}

TEST_CASE("overrides save with the version, follow a duplicated version, and orphans are dropped")
{
    session::Session s(catalogs());
    s.new_empty();
    const std::string t = s.add_track("Flute", "woodwinds.flute");
    const auto ids = write_line(s, t);
    REQUIRE(ids.size() == 5);
    REQUIRE(s.edit_performance(t, {ids[0], ids[4]}, "Nudge", [](notation::EventId id, NoteOverride& o) {
        o.onset = 15;
        o.velocity = id % 2 ? 7 : -7;
        o.cc = 3;
    }));
    const auto saved = *s.performance_of(t);

    // A deleted note keeps its override in memory (undo brings both back)
    // but it is not written to the file.
    notation::Score score = track(s, t).active_version()->score;
    REQUIRE(score.remove_note(ids[4]));
    REQUIRE(s.commit_score(t, score, "Delete note"));
    CHECK_EQ(s.performance_of(t)->size(), 2u);
    const auto path = tmp("performance.vcc2.json");
    std::string err;
    REQUIRE(s.save_as(path, &err));
    session::Session r(catalogs());
    REQUIRE(r.open(path, &err));
    REQUIRE(r.performance_of(t));
    CHECK_EQ(r.performance_of(t)->size(), 1u);
    CHECK(r.performance_of(t)->at(ids[0]) == saved.at(ids[0]));
    CHECK(project::render_track(track(r, t)).notes == project::render_track(track(s, t)).notes);

    REQUIRE(s.undo());  // the deletion: note and override are both back
    CHECK(note_of(project::render_track(track(s, t)), ids[4]).start ==
          note_of(project::render_track(track(s, t), false), ids[4]).start + 15);

    // A duplicated version starts with the same performance, then they part.
    const auto copy = s.duplicate_version(t);
    REQUIRE(copy);
    CHECK_EQ(s.performance_of(t)->size(), 2u);
    REQUIRE(s.clear_performance(t));
    CHECK(s.performance_of(t)->empty());
    const auto& versions = track(s, t).versions;
    CHECK_EQ(versions.front().performance.size(), 2u);

    project::TrackVersion v = versions.front();
    v.score.remove_note(ids[0]);
    v.performance[ids[1]] = {};
    CHECK_EQ(project::prune_performance(v), 2u);  // one orphan, one empty
}
