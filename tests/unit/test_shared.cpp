// Collaboration on the desktop: the sharing files beside a project, and a
// sync between two copies, without a network (the desktop app moves the
// documents; this is what it does with them).
#include <filesystem>

#include "vcc/core/Files.h"
#include "vcc/session/Session.h"
#include "vcc/session/Shared.h"
#include "vcc_test.h"

using namespace vcc;
using session::Session;
namespace shared = session::shared;

namespace {
const session::Catalogs& catalogs()
{
    static auto c = session::Catalogs::load_from_directory(VCC_CONTENT_DIR);
    return *c;
}
std::string tmp(const std::string& name) { return std::string(VCC_TEST_TMP_DIR) + "/" + name; }
bool has_track(const Session& s, const std::string& name)
{
    for (const auto& t : s.data().tracks)
        if (t.name == name) return true;
    return false;
}
}  // namespace

TEST_CASE("shared: the link and base files beside a project")
{
    const auto file = tmp("shared-link.vcc2");
    shared::unlink(file);
    CHECK(!shared::read_link(file));
    CHECK(!shared::read_base(file));
    shared::Link l{"3f0c-uuid", "Night Cue", "editor", 4, 0x1234};
    REQUIRE(shared::write_link(file, l));
    REQUIRE(shared::write_base(file, R"({"tracks":[]})"));
    const auto back = shared::read_link(file);
    REQUIRE(back);
    CHECK(back->project_id == "3f0c-uuid" && back->name == "Night Cue" && back->role == "editor");
    CHECK(back->base == 4 && back->site == 0x1234u);
    CHECK(shared::read_base(file).has_value());
    shared::unlink(file);
    CHECK(!shared::read_link(file));
    CHECK(!shared::read_base(file));

    CHECK(shared::can_push("owner") && shared::can_push("editor") && shared::can_push("arranger"));
    CHECK(!shared::can_push("reviewer") && !shared::can_push("viewer"));
    CHECK(shared::role_label("reviewer") == "Reviewer / director");
    for (std::uint64_t r : {0ull, 1ull, 0xfffffeull, 0xffffffull, ~0ull}) {
        const auto s = shared::site_from(r);
        CHECK(s >= 1u && s <= 0xffffffu);
    }
}

TEST_CASE("shared: two composers sync - separate work combines, the base moves, nothing is left to push")
{
    Session ann(catalogs()), ben(catalogs());
    ann.new_empty();
    ann.set_site(1);
    ann.add_track("Violins I", "violin");
    // Ann shares it: revision 1 is her document.
    const auto rev1 = ann.project().to_json();

    // Ben opens revision 1 (no base yet: the server's copy is taken whole).
    ben.new_empty();
    auto first = shared::pull(std::nullopt, ben.project().to_json(), rev1);
    REQUIRE(first.clean());
    REQUIRE(ben.adopt_document(first.merged, "Open shared project"));
    ben.set_site(2);
    CHECK(has_track(ben, "Violins I"));
    CHECK(!shared::changed_since(rev1, ben.project().to_json()));

    // Both work at once.
    ann.add_track("Horns", "horn");
    ben.add_track("Harp", "harp");

    // Ann pushes first: revision 2.
    CHECK(shared::changed_since(rev1, ann.project().to_json()));
    const auto rev2 = ann.project().to_json();

    // Ben's push naming revision 1 would be refused; he pulls revision 2.
    auto r = shared::pull(rev1, ben.project().to_json(), rev2);
    CHECK(r.clean());
    REQUIRE(ben.adopt_document(r.merged, "Sync"));
    CHECK(has_track(ben, "Violins I"));
    CHECK(has_track(ben, "Horns"));
    CHECK(has_track(ben, "Harp"));
    // His document differs from revision 2 by the harp: that is his push (revision 3).
    CHECK(shared::changed_since(rev2, ben.project().to_json()));
    const auto rev3 = ben.project().to_json();

    // Ann pulls revision 3 with nothing of her own since: she simply gets it.
    auto a = shared::pull(rev2, ann.project().to_json(), rev3);
    CHECK(a.clean());
    REQUIRE(ann.adopt_document(a.merged, "Sync"));
    CHECK(has_track(ann, "Harp"));
    CHECK(!shared::changed_since(rev3, ann.project().to_json()));
    // The sync is one undo step.
    CHECK(ann.project().undo_label() == "Sync");
}

TEST_CASE("shared: a part another composer has locked is theirs until they unlock it")
{
    Session s(catalogs());
    s.new_empty();
    const auto vln = s.add_track("Violins I", "violin");
    int changes = 0;
    s.add_listener([&](session::Change c) { changes += c == session::Change::Document; });
    s.set_track_locks({{vln, "Ben"}});
    CHECK(changes == 1);
    s.set_track_locks({{vln, "Ben"}});  // the same locks: nothing to tell
    CHECK(changes == 1);
    CHECK(!s.edit("Rename", [&](project::ProjectData& d) { d.find_track(vln)->name = "Vln"; return true; }));
    CHECK(s.project().last_refusal().find("Ben is writing Violins I") == 0);
    // Opening another document forgets the locks.
    s.new_empty();
    CHECK(s.track_locks().empty());
}
