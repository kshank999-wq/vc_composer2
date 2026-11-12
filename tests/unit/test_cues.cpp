#include "vcc/cues/Cues.h"
#include "vcc/picture/Picture.h"
#include "vcc/session/Session.h"
#include "vcc_test.h"

using namespace vcc;

namespace {
const session::Catalogs& catalogs()
{
    static auto c = session::Catalogs::load_from_directory(VCC_CONTENT_DIR);
    return *c;
}
}  // namespace

TEST_CASE("cues get reel-numbered codes, hierarchy and default deliverables")
{
    session::Session s(catalogs());
    s.new_empty();
    s.set_production({"The Lighthouse", "Studio", "A. Director", "K. Composer"});
    std::string r1 = s.add_reel("Reel 1");
    std::string r2 = s.add_reel("Reel 2");
    std::string sc = s.add_scene(r1, "Sc. 4 - storm");
    std::string a = s.add_cue(0, 8 * kTicksPerWhole, r1, "Arrival");
    std::string b = s.add_cue(8 * kTicksPerWhole, 16 * kTicksPerWhole, r1, "Storm");
    std::string c = s.add_cue(0, 4 * kTicksPerWhole, r2, "Morning");
    CHECK_EQ(s.data().find_cue(a)->code, std::string("1M1"));
    CHECK_EQ(s.data().find_cue(b)->code, std::string("1M2"));
    CHECK_EQ(s.data().find_cue(c)->code, std::string("2M1"));
    CHECK_EQ(s.data().find_cue(a)->composer, std::string("K. Composer"));
    CHECK_EQ(s.data().find_cue(a)->deliverables.size(), 3u);
    REQUIRE(s.update_cue(a, [&](project::Cue& cue) { cue.scene_id = sc; cue.status = "approved"; }));
    // Persisted through save/load.
    auto loaded = project::Project::from_json(s.project().to_json());
    REQUIRE(loaded);
    CHECK_EQ(loaded->data().find_cue(a)->scene_id, sc);
    CHECK_EQ(loaded->data().production.title, std::string("The Lighthouse"));
    CHECK_EQ(loaded->data().reels.size(), 2u);
}

TEST_CASE("locked cues refuse edits unless the composer confirmed")
{
    session::Session s(catalogs());
    s.new_empty();
    std::string a = s.add_cue(0, 4 * kTicksPerWhole, {}, "Main title");
    CHECK_EQ(s.data().find_cue(a)->code, std::string("M1"));
    REQUIRE(s.update_cue(a, [](project::Cue& c) { c.status = "locked"; }));
    CHECK(!s.update_cue(a, [](project::Cue& c) { c.title = "Changed"; }));
    CHECK(!s.remove_cue(a));
    CHECK(s.update_cue(a, [](project::Cue& c) { c.title = "Changed"; }, "Edit locked cue", true));
    CHECK_EQ(s.data().find_cue(a)->title, std::string("Changed"));
}

TEST_CASE("cue times, spotting drift and spotting a cue to picture")
{
    session::Session s(catalogs());
    s.new_empty();
    s.set_tempo(120);
    std::string a = s.add_cue(kTicksPerWhole * 2, kTicksPerWhole * 6, {}, "Chase");
    auto t = cues::times_of(*s.data().find_cue(a), s.data());
    CHECK_EQ(t.in_tc, std::string("01:00:04:00"));
    CHECK_EQ(t.out_tc, std::string("01:00:12:00"));
    CHECK_EQ(t.duration, std::string("0:08"));
    CHECK(!cues::spot_drift_frames(*s.data().find_cue(a), s.data()));
    REQUIRE(s.spot_cue(a, "01:00:04:12", "01:00:20:00", false));
    CHECK_EQ(*cues::spot_drift_frames(*s.data().find_cue(a), s.data()), -12);
    REQUIRE(s.spot_cue(a, "01:00:04:12", "01:00:20:00", true));
    CHECK(std::llabs(*cues::spot_drift_frames(*s.data().find_cue(a), s.data())) <= 1);
    CHECK(!s.spot_cue(a, "01:00:20:00", "01:00:04:12", false));
}

TEST_CASE("dashboard filters, overdue and overlaps")
{
    session::Session s(catalogs());
    s.new_empty();
    std::string r = s.add_reel("Reel 1");
    std::string a = s.add_cue(0, 8 * kTicksPerWhole, r, "Arrival");
    std::string b = s.add_cue(6 * kTicksPerWhole, 12 * kTicksPerWhole, r, "Transition");
    s.update_cue(a, [](project::Cue& c) { c.deadline = "2026-11-01"; c.status = "writing"; });
    s.update_cue(b, [](project::Cue& c) { c.deadline = "2026-11-01"; c.status = "approved"; });
    cues::CueFilter f;
    f.overdue_only = true;
    f.today = "2026-11-12";
    auto late = cues::list(s.data(), f);
    REQUIRE(late.size() == 1u);
    CHECK_EQ(late[0]->id, a);
    f = {};
    f.query = "transit";
    CHECK_EQ(cues::list(s.data(), f).size(), 1u);
    f = {};
    f.status = "approved";
    CHECK_EQ(cues::list(s.data(), f).size(), 1u);
    auto o = cues::overlaps(s.data());
    REQUIRE(o.size() == 1u);
    CHECK_EQ(o[0].first, a);
}

TEST_CASE("cue sheet CSV, spotting notes and file naming")
{
    session::Session s(catalogs());
    s.new_empty();
    s.set_production({"Night, Again", "", "D. Rector", "K. Composer"});
    std::string r = s.add_reel("Reel 1");
    std::string a = s.add_cue(0, 4 * kTicksPerWhole, r, "Opening \"Night\"");
    s.add_marker(1.0, "hit", "door slam");
    std::string csv = cues::cue_sheet_csv(s.data());
    CHECK(csv.find("Cue,Title,Reel") == 0);
    CHECK(csv.find("\"Opening \"\"Night\"\"\"") != std::string::npos);  // RFC 4180 quoting
    CHECK(csv.find("Reel 1") != std::string::npos);
    std::string notes = cues::spotting_notes(s.data());
    CHECK(notes.find("1M1") != std::string::npos);
    CHECK(notes.find("door slam") != std::string::npos);
    CHECK(notes.find("D. Rector") != std::string::npos);
    std::string name = cues::render_name("", *s.data().find_cue(a), s.data());
    CHECK_EQ(name, std::string("Night, Again_1M1_Opening _Night__v1"));
    CHECK_EQ(cues::render_name("{reel}/{code}:{rev}", *s.data().find_cue(a), s.data()), std::string("Reel 1_1M1_1"));
}

TEST_CASE("pictures, markers and the tempo fit go through the session as undo steps")
{
    session::Session s(catalogs());
    s.new_empty();
    s.set_tempo(120);
    project::PictureVersion p;
    p.path = "cut1.mov";
    p.start_timecode = "00:59:58:00";
    std::string c1 = s.import_picture(p);
    std::string c2 = s.import_picture(p);
    CHECK_EQ(s.data().active_picture_id, c2);
    CHECK_EQ(s.data().pictures[1].name, std::string("Cut 2"));
    REQUIRE(s.set_active_picture(c1));
    for (int i = 0; i < 10; ++i) s.set_picture_offset(c1, 0.1 * i);
    REQUIRE(s.undo());  // one step for the whole nudge
    CHECK_NEAR(s.data().active_picture()->offset_seconds, 0.0, 1e-9);

    std::string hit = s.add_marker(8.4, "hit", "explosion");
    std::string err;
    REQUIRE(s.fit_tempo_to_marker(hit, 4 * kTicksPerWhole, kTicksPerWhole, &err));
    CHECK_NEAR(s.data().tempo.seconds_at(4 * kTicksPerWhole), 8.4, 1e-9);
    // The hit itself never moved: markers are picture time.
    CHECK_NEAR(s.data().markers[0].seconds, 8.4, 1e-12);
    REQUIRE(s.undo());
    CHECK_NEAR(s.data().tempo.bpm_at(2 * kTicksPerWhole), 120.0, 1e-9);
    REQUIRE(s.remove_picture(c2));
    REQUIRE(s.remove_marker(hit));
    auto reopened = project::Project::from_json(s.project().to_json());
    REQUIRE(reopened);
    CHECK_EQ(reopened->data().pictures.size(), 1u);
    CHECK_EQ(reopened->data().pictures[0].start_timecode, std::string("00:59:58:00"));
}
