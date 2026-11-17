// Licence standing on this computer: the same rules as the owner's other
// desktops (see vcc/licensing/Standing.h).
#include "vcc/licensing/Standing.h"
#include "vcc/licensing/Update.h"
#include "vcc_test.h"

using namespace vcc;
using namespace vcc::licensing;

namespace {
std::int64_t at(const char* iso) { return *parse_iso(iso); }
constexpr std::int64_t kDay = 86400;
}  // namespace

TEST_CASE("ISO times read and write in UTC, with offsets and fractions")
{
    CHECK_EQ(at("1970-01-01T00:00:00Z"), std::int64_t{0});
    CHECK_EQ(at("2026-11-16"), at("2026-11-16T00:00:00Z"));
    CHECK_EQ(at("2026-11-16T10:00:00.123Z"), at("2026-11-16T10:00:00Z"));
    CHECK_EQ(at("2026-11-16T12:00:00+02:00"), at("2026-11-16T10:00:00Z"));
    CHECK_EQ(at("2026-11-16T08:00:00-0200"), at("2026-11-16T10:00:00Z"));
    CHECK_EQ(format_iso(at("2028-02-29T23:59:59Z")), std::string("2028-02-29T23:59:59.000Z"));
    CHECK_EQ(format_iso(0), std::string("1970-01-01T00:00:00.000Z"));
    CHECK(!parse_iso("16/11/2026"));
    CHECK(!parse_iso("2026-13-01"));
    CHECK(!parse_iso("2026-11-16T10:00:00Zjunk"));
}

TEST_CASE("the server's answer: the liveliest licence, or none")
{
    auto c = claim_from_response(*json::parse(R"({"standing":{"status":"active","expiresAt":"2027-01-01T00:00:00Z","grant":"academic"}})"));
    REQUIRE(c.has_value());
    CHECK_EQ(c->status, std::string("active"));
    CHECK_EQ(c->grant, std::string("academic"));
    CHECK(!claim_from_response(*json::parse(R"({"standing":null})")));
    CHECK(!claim_from_response(*json::parse(R"({"standing":{"status":"weird"}})")));
    CHECK(licence_live("active", "", at("2026-11-16")));
    CHECK(licence_live("active", "2026-12-01", at("2026-11-16")));
    CHECK(!licence_live("active", "2026-11-01", at("2026-11-16")));
    CHECK(!licence_live("revoked", "", at("2026-11-16")));
}

TEST_CASE("saving: never activated is off; nothing heard or live is on")
{
    const auto now = at("2026-11-16T10:00:00Z");
    auto s = saving_standing(std::nullopt, now, false);
    CHECK(!s.can_save && s.needs_activation && !s.warning);
    CHECK(saving_standing(std::nullopt, now, std::nullopt).can_save);  // a build that does not licence saving
    s = saving_standing(std::nullopt, now, true);
    CHECK(s.can_save && s.notice.empty());
    const auto live = note_standing(std::nullopt, {"active", "2027-11-16T00:00:00Z", ""}, now);
    CHECK(live.seen_lapsed_at.empty());
    s = saving_standing(live, now, true);
    CHECK(s.can_save && s.notice.empty());
}

TEST_CASE("a lapse is announced for seven days from when this computer first saw it, then saving stops")
{
    const auto first = at("2026-11-16T10:00:00Z");
    const Claim ended{"expired", "2026-11-10T00:00:00Z", ""};
    auto noted = note_standing(std::nullopt, ended, first);
    CHECK_EQ(noted.seen_lapsed_at, format_iso(first));
    // Asking again three days later keeps the first sighting: the week does not restart.
    noted = note_standing(noted, ended, first + 3 * kDay);
    CHECK_EQ(noted.seen_lapsed_at, format_iso(first));
    auto s = saving_standing(noted, first + 3 * kDay, true);
    CHECK(s.can_save && s.warning);
    CHECK(s.notice.find("2026-11-10") != std::string::npos && s.notice.find("until 2026-11-23") != std::string::npos);
    s = saving_standing(noted, first + 8 * kDay, true);
    CHECK(!s.can_save && !s.warning && !s.needs_activation);
    // Renewing clears it; a later lapse gets a new week.
    noted = note_standing(noted, {"active", "2027-11-16T00:00:00Z", ""}, first + 9 * kDay);
    CHECK(noted.seen_lapsed_at.empty());
    CHECK(saving_standing(noted, first + 9 * kDay, true).can_save);
}

TEST_CASE("an old answer is not a refusal, and the words fit how the licence came")
{
    const auto then = at("2026-10-01T10:00:00Z");
    auto noted = note_standing(std::nullopt, {"expired", "2026-09-30T00:00:00Z", "academic"}, then);
    CHECK_EQ(noted.grant, std::string("academic"));
    auto s = saving_standing(noted, then + 40 * kDay, true);  // offline for 40 days
    CHECK(s.can_save && s.warning && s.notice.find("academic licence") != std::string::npos);
    noted = note_standing(std::nullopt, {"revoked", "", "purchase"}, then);
    CHECK(noted.grant.empty());
    s = saving_standing(noted, then + 10 * kDay, true);
    CHECK(!s.can_save && s.notice.find("no longer active") != std::string::npos);
    // Written down and read back.
    const auto back = DeskStanding::from_json(*json::parse(json::write(noted.to_json())));
    REQUIRE(back.has_value());
    CHECK_EQ(back->status, noted.status);
    CHECK_EQ(back->seen_lapsed_at, noted.seen_lapsed_at);
    CHECK(!DeskStanding::from_json(json::Value::make_object()));
}

TEST_CASE("updates: build labels read, and order releases above their previews")
{
    const auto p16 = parse_version("0.1.0-preview.16");
    CHECK(p16.ok);
    CHECK(p16.major == 0 && p16.minor == 1 && p16.patch == 0 && p16.preview == 16);
    CHECK(parse_version("v1.2.3").ok);
    CHECK(parse_version("1.2.3").preview == -1);
    for (const char* bad : {"", "1.2", "1.2.x", "1.2.3-beta.1", "1.2.3-preview.", "1.2.3-preview.4x", "abc"})
        CHECK(!parse_version(bad).ok);

    CHECK(is_newer("0.1.0-preview.17", "0.1.0-preview.16"));
    CHECK(!is_newer("0.1.0-preview.16", "0.1.0-preview.16"));
    CHECK(!is_newer("0.1.0-preview.9", "0.1.0-preview.16"));  // numbers, not text
    CHECK(is_newer("0.1.0", "0.1.0-preview.99"));             // the release beats its previews
    CHECK(!is_newer("0.1.0-preview.99", "0.1.0"));
    CHECK(is_newer("0.2.0-preview.1", "0.1.0"));
    CHECK(is_newer("0.10.0", "0.9.9"));
    CHECK(!is_newer("garbage", "0.1.0"));
    CHECK(!is_newer("0.2.0", "garbage"));
}

TEST_CASE("updates: the site's catalogue offers this platform's newer build, or nothing")
{
    const auto catalogue = *json::parse(R"({"builds":[
        {"platform":"windows","version":"0.1.0-preview.17","releaseNotes":"Fixes","sha256":"aa","minimumOsVersion":"10.0.17763"},
        {"platform":"macos","version":"0.1.0-preview.15","releaseNotes":"Old","sha256":"bb","minimumOsVersion":"11.0"},
        {"platform":"linux","version":"9.9.9"}]})");
    const auto win = pick_update(catalogue, "windows", "0.1.0-preview.16");
    REQUIRE(win.has_value());
    CHECK(win->version == "0.1.0-preview.17");
    CHECK(win->notes == "Fixes");
    CHECK(win->sha256 == "aa");
    CHECK(win->minimum_os == "10.0.17763");
    CHECK(!pick_update(catalogue, "windows", "0.1.0-preview.17"));   // up to date
    CHECK(!pick_update(catalogue, "macos", "0.1.0-preview.16"));     // the site's is older
    CHECK(!pick_update(*json::parse("{}"), "windows", "0.1.0"));
    CHECK(!pick_update(*json::parse(R"({"builds":"nope"})"), "windows", "0.1.0"));
}
