// Collaboration on the desktop: the sharing files beside a project, and a
// sync between two copies, without a network (the desktop app moves the
// documents; this is what it does with them).
#include <filesystem>

#include "vcc/core/Files.h"
#include "vcc/session/Session.h"
#include "vcc/session/Relink.h"
#include "vcc/session/Shared.h"
#include "vcc/core/Hash.h"
#include <fstream>
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
    shared::Link l{"3f0c-uuid", "Night Cue", "editor", 4, 0x1234, {}};
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

namespace {
namespace fs = std::filesystem;
void write(const fs::path& p, const std::string& text)
{
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
}
std::string read(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}
}  // namespace

TEST_CASE("shared media: paths from other people stay inside the project folder")
{
    for (const char* ok : {"Audio Files/Vox take 1.wav", "Picture/Reel 2.mov", "a.wav", "Audio Files/..x.wav"})
        CHECK(shared::safe_media_path(ok));
    for (const char* bad : {"", "/etc/passwd", "../x.wav", "Audio Files/../../x.wav", "C:/x.wav", "a\\b.wav", "Audio Files//x.wav",
                            "./x.wav", "x/"})
        CHECK(!shared::safe_media_path(bad));
    const std::string h(64, 'a');
    shared::Manifest good{{"Audio Files/a.wav", h, 4}, {"Picture/r.mov", h, 9}};
    CHECK(shared::manifest_from_json(shared::manifest_to_json(good)) == good);
    auto doc = shared::manifest_to_json(good);
    auto evil = json::parse(R"({"path":"../../.bashrc","sha256":")" + h + R"(","sizeBytes":4})");
    doc.push_back(*evil);
    CHECK(shared::manifest_from_json(doc).size() == 2u);  // the climbing path is dropped
}

TEST_CASE("shared media: outside files are gathered into the project folder, relinked and listed by content")
{
    const fs::path root = fs::u8path(tmp("shared-media-gather"));
    fs::remove_all(root);
    const auto take = root / "Elsewhere" / "take.wav";
    const auto reel = root / "Movies" / "reel.mov";
    write(take, "RIFF-audio-one");
    write(reel, "movie-bytes");
    const auto file = (root / "Night Cue" / "Night Cue.vcc2").u8string();
    fs::create_directories(root / "Night Cue");

    Session s(catalogs());
    s.new_empty();
    const auto trk = s.add_audio_track("Vox", false);
    s.edit("Clip", [&](project::ProjectData& d) {
        project::AudioClip c;
        c.id = "clp-1";
        c.file = take.u8string();
        c.length = 1.0;
        d.find_track(trk)->active_version()->audio_clips.push_back(c);
        project::PictureVersion p;
        p.id = "pic-1";
        p.path = reel.u8string();
        d.pictures.push_back(p);
        return true;
    });
    REQUIRE(s.save_as(file));

    auto copies = shared::plan_gather(s.data(), file);
    REQUIRE(copies.size() == 2u);
    REQUIRE(shared::run_copies(copies));
    const auto relinks = shared::relinks_of(copies);
    s.edit("Gather", [&](project::ProjectData& d) { return session::relink::apply(d, relinks, file) > 0; });
    CHECK(s.data().find_track(trk)->active_version()->audio_clips[0].file == "Audio Files/take.wav");
    CHECK(s.data().pictures[0].path == "Picture/reel.mov");
    CHECK(read(root / "Night Cue" / "Audio Files" / "take.wav") == "RIFF-audio-one");
    CHECK(fs::exists(take));  // the original stays where it was
    CHECK(shared::plan_gather(s.data(), file).empty());  // gathered once

    shared::HashCache cache(file);
    const auto listed = shared::list_media(s.data(), file, cache);
    REQUIRE(listed.manifest.size() == 2u);
    CHECK(listed.manifest[0].path == "Audio Files/take.wav");
    CHECK(listed.manifest[0].sha256 == hash::sha256_hex("RIFF-audio-one"));
    CHECK(listed.manifest[0].size == 14u);
    CHECK(listed.manifest[1].path == "Picture/reel.mov");
    CHECK(listed.missing.empty() && listed.too_large.empty() && listed.outside.empty());
    REQUIRE(cache.save());
    CHECK(fs::exists(file + ".shared-media.json"));

    // A file used but not here keeps the entry the server already has.
    fs::remove(root / "Night Cue" / "Picture" / "reel.mov");
    shared::HashCache again(file);
    const shared::Manifest known{{"Picture/reel.mov", std::string(64, 'e'), 11}};
    const auto l2 = shared::list_media(s.data(), file, again, known);
    CHECK(l2.manifest.size() == 2u);
    CHECK(shared::find_media(l2.manifest, "Picture/reel.mov")->sha256 == std::string(64, 'e'));
    const auto l3 = shared::list_media(s.data(), file, again);
    CHECK(l3.manifest.size() == 1u);
    CHECK(l3.missing == std::vector<std::string>{"Picture/reel.mov"});
}

TEST_CASE("shared media: two takes at one path both survive; theirs is fetched where it differs")
{
    const fs::path root = fs::u8path(tmp("shared-media-clash"));
    fs::remove_all(root);
    const auto file = (root / "P" / "P.vcc2").u8string();
    write(root / "P" / "Audio Files" / "Vox 1.wav", "ann's take");
    write(root / "P" / "Audio Files" / "Pad.wav", "the same pad");
    const auto ann = hash::sha256_hex("ann's take"), ben = hash::sha256_hex("ben's take"), pad = hash::sha256_hex("the same pad");
    const shared::Manifest ours{{"Audio Files/Pad.wav", pad, 12}, {"Audio Files/Vox 1.wav", ann, 10}};
    const shared::Manifest base{{"Audio Files/Pad.wav", pad, 12}};
    const shared::Manifest theirs{{"Audio Files/Pad.wav", pad, 12}, {"Audio Files/Vox 1.wav", ben, 10}};

    const auto moved = shared::separate_clashes(ours, base, theirs, file, 0x1a2b);
    REQUIRE(moved.size() == 1u);
    REQUIRE(moved.count("Audio Files/Vox 1.wav"));
    const auto kept = fs::u8path(moved.at("Audio Files/Vox 1.wav"));
    CHECK(kept.filename().u8string() == "Vox 1 (1a2b).wav");
    CHECK(read(kept) == "ann's take");

    // A file I did not change since the base is simply replaced by theirs.
    const shared::Manifest base2{{"Audio Files/Vox 1.wav", ann, 10}};
    CHECK(shared::separate_clashes(ours, base2, theirs, file, 0x1a2b).empty());

    // After the merge the project uses theirs at the path: it is fetched.
    project::ProjectData merged;
    project::Track t;
    t.id = "trk-1";
    project::TrackVersion v;
    v.id = "ver-1";
    for (const char* f : {"Audio Files/Vox 1.wav", "Audio Files/Pad.wav"}) {
        project::AudioClip c;
        c.id = f;
        c.file = f;
        v.audio_clips.push_back(c);
    }
    t.versions.push_back(v);
    t.active_version_id = "ver-1";
    merged.tracks.push_back(t);
    shared::HashCache cache(file);
    auto fetch = shared::to_fetch(merged, theirs, file, cache);
    REQUIRE(fetch.size() == 1u);
    CHECK(fetch[0].path == "Audio Files/Vox 1.wav");
    CHECK(fetch[0].sha256 == ben);
    // Once it is here, nothing more.
    write(root / "P" / "Audio Files" / "Vox 1.wav", "ben's take");
    shared::HashCache fresh(file);
    CHECK(shared::to_fetch(merged, theirs, file, fresh).empty());
}
