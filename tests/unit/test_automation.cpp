// Automation: parameters, evaluation, written passes and their undo.
#include <cmath>
#include <filesystem>

#include "vcc/project/Automation.h"
#include "vcc/session/Session.h"
#include "vcc_test.h"

using namespace vcc;
using namespace vcc::project;

namespace {
const session::Catalogs& catalogs()
{
    static auto c = session::Catalogs::load_from_directory(VCC_CONTENT_DIR);
    return *c;
}
std::string tmp(const std::string& name) { return std::string(VCC_TEST_TMP_DIR) + "/" + name; }
AutomationLane lane(const std::string& param, std::vector<AutomationPoint> pts)
{
    AutomationLane l;
    l.param = param;
    l.points = std::move(pts);
    normalise(l);
    return l;
}
}  // namespace

TEST_CASE("parameters are described and map onto the strip")
{
    CHECK(param_info("volume").has_value());
    CHECK(param_info("strip.hmf.gain_db").has_value());
    CHECK(param_info("strip.eq_on")->toggle);
    CHECK(param_info("strip.lf.freq")->log_scale);
    CHECK(param_info("send:bus-7").has_value());
    CHECK(param_info("insert:2:mix").has_value());
    CHECK(param_info("plugin:12").has_value());
    CHECK(param_info("insert_plugin:0:3").has_value());
    CHECK(!param_info("strip.nonsense").has_value());
    CHECK(!param_info("plugin:x").has_value());
    CHECK(!param_info("bogus").has_value());

    Track t;
    t.sends.emplace_back("bus-fx", -9.0);
    InsertSlot comp;
    comp.kind = "compressor";
    t.inserts.push_back(comp);
    CHECK(set_static_value(t, "strip.hmf.gain_db", 4.5));
    CHECK_EQ(t.strip.hmf.gain_db, 4.5);
    CHECK(set_static_value(t, "strip.eq_on", 0.7));
    CHECK(t.strip.eq_on);
    CHECK(set_static_value(t, "volume", 50.0));
    CHECK_EQ(t.gain_db, 12.0);  // clamped
    CHECK_EQ(*static_value(t, "send:bus-fx"), -9.0);
    CHECK(!static_value(t, "send:nope").has_value());
    CHECK_EQ(*static_value(t, "insert:0:ratio"), 3.0);  // default
    CHECK(set_static_value(t, "insert:0:ratio", 6.0));
    CHECK_EQ(t.inserts[0].params.at("ratio"), 6.0);

    ProjectData d;
    const auto params = automatable_params(t, d);
    bool has_send = false, has_insert = false;
    for (const auto& p : params) {
        has_send |= p.id == "send:bus-fx";
        has_insert |= p.id == "insert:0:threshold_db";
    }
    CHECK(has_send);
    CHECK(has_insert);
}

TEST_CASE("lanes evaluate linear, step and smooth segments")
{
    const auto l = lane("volume", {{960, -6.0, "linear"}, {0, 0.0, "linear"}, {1920, -6.0, "step"}, {2880, 0.0, "smooth"},
                                   {3840, 6.0, "linear"}});
    CHECK_EQ(l.points.front().tick, 0);  // sorted
    CHECK(!value_at(AutomationLane{}, 0).has_value());
    CHECK_EQ(*value_at(l, -100), 0.0);
    CHECK_EQ(*value_at(l, 480), -3.0);
    CHECK_EQ(*value_at(l, 2400), -6.0);  // step holds
    CHECK_EQ(*value_at(l, 2880), 0.0);
    CHECK(std::abs(*value_at(l, 3360) - 3.0) < 1e-9);  // smooth midpoint
    CHECK(*value_at(l, 3000) < 0.0 + 6.0 * (120.0 / 960.0));  // eases in
    CHECK_EQ(*value_at(l, 99999), 6.0);

    const auto sw = lane("mute", {{0, 0.2, "linear"}, {100, 0.9, "linear"}});
    CHECK_EQ(sw.points[0].value, 0.0);
    CHECK_EQ(sw.points[1].value, 1.0);
    CHECK_EQ(sw.points[0].shape, std::string("step"));
    CHECK_EQ(*value_at(sw, 50), 0.0);
}

TEST_CASE("a written pass replaces only its range and is thinned")
{
    auto l = lane("volume", {{0, -10.0, "linear"}, {10000, -10.0, "linear"}});
    std::vector<AutomationPoint> written;
    for (Ticks t = 2000; t <= 4000; t += 10) written.push_back({t, -10.0 + static_cast<double>(t - 2000) / 200.0, "linear"});
    write_pass(l, 2000, 4000, written);
    // A straight ramp thins to its ends; guard points keep the outside.
    CHECK(l.points.size() <= 5u);
    CHECK_EQ(*value_at(l, 1000), -10.0);
    CHECK(std::abs(*value_at(l, 3000) - -5.0) < 0.3);
    CHECK(std::abs(*value_at(l, 4000) - 0.0) < 0.3);
    CHECK_EQ(*value_at(l, 4001), -10.0);
    CHECK_EQ(*value_at(l, 9000), -10.0);

    std::vector<AutomationPoint> pts;
    for (int i = 0; i < 100; ++i) pts.push_back({i * 10, (i % 2) ? 1.0 : 0.0, "linear"});
    thin(pts, 0.1);
    CHECK_EQ(pts.size(), 100u);  // nothing to remove from a zig-zag
}

TEST_CASE("session: lanes, modes and a written pass are undoable and saved")
{
    session::Session s(catalogs());
    s.new_empty();
    const std::string t = s.add_track("Horns", "brass.horns");
    REQUIRE(s.update_track_mix(t, "Volume Horns", [](Track& x) { x.gain_db = -4.0; }));

    CHECK(s.set_automation_mode(t, "touch"));
    CHECK_EQ(s.data().find_track(t)->automation_mode, std::string("touch"));
    CHECK(!s.set_automation_mode(t, "bogus"));
    CHECK(!s.set_automation_mode("nope", "read"));

    const std::string id = s.ensure_lane(t, "volume");
    REQUIRE(!id.empty());
    CHECK_EQ(s.ensure_lane(t, "volume"), id);
    CHECK(s.ensure_lane(t, "bogus").empty());
    const auto* l = find_lane(*s.lanes_of(t), "volume");
    REQUIRE(l);
    CHECK_EQ(l->points.size(), 1u);
    CHECK_EQ(l->points[0].value, -4.0);  // seeded with the fader

    for (int i = 0; i < 5; ++i)
        CHECK(s.edit_lane(t, id, "Move breakpoint", [&](AutomationLane& x) {
            x.points.push_back({1920, -10.0 - i, "linear"});
            x.param = "pan";  // ignored
        }));
    l = find_lane(*s.lanes_of(t), "volume");
    REQUIRE(l);
    CHECK_EQ(l->points.size(), 2u);
    CHECK_EQ(l->points[1].value, -14.0);
    CHECK_EQ(s.undo_label(), std::string("Move breakpoint"));

    // A pass over the master bus and a new send lane, one undo step.
    const std::string fx = s.add_bus("Hall", "fx");
    REQUIRE(s.update_track_mix(t, "Send", [&](Track& x) { x.sends.emplace_back(fx, -12.0); }));
    std::vector<session::Session::AutomationWrite> w(2);
    w[0].owner_id = "bus-master";
    w[0].param = "volume";
    w[0].from = 0;
    w[0].to = 2880;
    w[0].points = {{0, 0.0, "linear"}, {2880, -6.0, "linear"}};
    w[1].owner_id = t;
    w[1].param = "send:" + fx;
    w[1].from = 960;
    w[1].to = 1920;
    w[1].points = {{960, -12.0, "linear"}, {1920, 0.0, "linear"}};
    CHECK(s.commit_automation_pass(w));
    CHECK_EQ(s.undo_label(), std::string("Write automation"));
    CHECK(find_lane(*s.lanes_of("bus-master"), "volume"));
    const auto* send_lane = find_lane(*s.lanes_of(t), "send:" + fx);
    REQUIRE(send_lane);
    CHECK_EQ(*value_at(*send_lane, 0), -12.0);
    CHECK_EQ(*value_at(*send_lane, 1920), 0.0);
    CHECK_EQ(*value_at(*send_lane, 5000), -12.0);

    std::filesystem::create_directories(tmp("auto"));
    REQUIRE(s.save_as(tmp("auto/a.vcc2")));
    session::Session r(catalogs());
    REQUIRE(r.open(tmp("auto/a.vcc2")));
    CHECK_EQ(r.data().find_track(t)->automation_mode, std::string("touch"));
    CHECK_EQ(r.lanes_of(t)->size(), 2u);
    CHECK_EQ(*value_at(*find_lane(*r.lanes_of("bus-master"), "volume"), 1440), -3.0);

    CHECK(s.undo());
    CHECK(!find_lane(*s.lanes_of("bus-master"), "volume"));
    CHECK(s.remove_lane(t, id));
    CHECK(!find_lane(*s.lanes_of(t), "volume"));
    CHECK(s.undo());
    CHECK(find_lane(*s.lanes_of(t), "volume"));
}
