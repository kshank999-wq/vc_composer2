// MusicXML import/export (interchange/) and the Session entry points.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include "vcc/interchange/MusicXml.h"
#include "vcc/interchange/Xml.h"
#include "vcc/project/Render.h"
#include "vcc/project/RenderCache.h"
#include "vcc/session/Session.h"
#include "vcc/theory/Chord.h"
#include "vcc_test.h"

using namespace vcc;
namespace nt = vcc::notation;
namespace ix = vcc::interchange;
using ix::xml::Element;

namespace {

const session::Catalogs& catalogs()
{
    static auto c = session::Catalogs::load_from_directory(VCC_CONTENT_DIR);
    return *c;
}

std::string fixture(const std::string& name)
{
    std::ifstream in(std::string(VCC_MUSICXML_DATA_DIR) + "/" + name, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

nt::Pitch P(nt::Step s, int octave, int alter = 0)
{
    return nt::Pitch{s, static_cast<std::int8_t>(octave), static_cast<std::int8_t>(alter)};
}

nt::ScoreNote note(Ticks start, nt::Duration d, nt::Pitch p, int voice = 0)
{
    nt::ScoreNote n;
    n.start_tick = start;
    n.duration = d;
    n.pitch = p;
    n.voice = static_cast<std::uint8_t>(voice);
    return n;
}

nt::ScoreNote rest(Ticks start, nt::Duration d, int voice = 0)
{
    nt::ScoreNote n;
    n.start_tick = start;
    n.duration = d;
    n.is_rest = true;
    n.voice = static_cast<std::uint8_t>(voice);
    return n;
}

nt::Duration trip(nt::NoteValue v) { return nt::Duration{v, 0, {3, 2}, {}}; }
nt::Duration nested(nt::NoteValue v) { return nt::Duration{v, 0, {3, 2}, {3, 2}}; }

constexpr Ticks Q = kTicksPerQuarter;
constexpr Ticks BAR = 4 * Q;

// --- DOM helpers ----------------------------------------------------------------

std::vector<const Element*> all(const Element& e, const std::string& name)
{
    return e.children_named(name);
}

const Element* part_by_id(const Element& root, const std::string& id)
{
    for (const auto* p : root.children_named("part"))
        if (p->attribute("id") == id) return p;
    return nullptr;
}

std::vector<const Element*> notes_of(const Element& measure) { return measure.children_named("note"); }

int count_named(const Element& e, const std::string& name)
{
    int n = e.name == name ? 1 : 0;
    for (const auto& c : e.children) n += count_named(c, name);
    return n;
}

const Element* find_first(const Element& e, const std::string& name)
{
    if (e.name == name) return &e;
    for (const auto& c : e.children)
        if (const Element* f = find_first(c, name)) return f;
    return nullptr;
}

std::string pitch_of(const Element& n)
{
    const Element* p = n.child("pitch");
    if (!p) return n.child("rest") ? "rest" : "?";
    std::string s = p->child_text("step");
    const std::string a = p->child_text("alter");
    if (a == "1") s += "#";
    if (a == "-1") s += "b";
    return s + p->child_text("octave");
}

bool has_tie(const Element& n, const std::string& type)
{
    for (const auto* t : n.children_named("tie"))
        if (t->attribute("type") == type) return true;
    return false;
}

// --- Score comparison --------------------------------------------------------

/// A note with its tie chain merged: what a round trip must preserve even
/// when the export cuts a note at a barline.
struct Flat {
    Ticks start = 0;
    Ticks length = 0;
    int midi = -1;
    int voice = 0;
    bool rest = false;
    bool grace = false;
    bool chord = false;
    nt::ArticulationMask articulations = 0;
    bool operator<(const Flat& o) const
    {
        return std::tie(start, voice, grace, midi, length) < std::tie(o.start, o.voice, o.grace, o.midi, o.length);
    }
    bool operator==(const Flat& o) const
    {
        return start == o.start && length == o.length && midi == o.midi && voice == o.voice && rest == o.rest &&
               grace == o.grace && chord == o.chord && articulations == o.articulations;
    }
};

std::ostream& operator<<(std::ostream& os, const Flat& f)
{
    return os << "{" << f.start << "+" << f.length << " midi " << f.midi << " v" << f.voice << (f.rest ? " rest" : "")
              << (f.grace ? " grace" : "") << (f.chord ? " chord" : "") << " art " << f.articulations << "}";
}

std::vector<Flat> flatten(const nt::Score& score, std::size_t staff)
{
    const auto& notes = score.notes(staff);
    std::vector<bool> used(notes.size(), false);
    std::vector<Flat> out;
    for (std::size_t i = 0; i < notes.size(); ++i) {
        if (used[i]) continue;
        used[i] = true;
        const auto& n = notes[i];
        Flat f;
        f.start = n.start_tick;
        f.length = n.grace ? 0 : n.duration.ticks();
        f.midi = n.is_rest ? -1 : nt::midi_note_number(n.pitch);
        f.voice = n.voice;
        f.rest = n.is_rest;
        f.grace = n.grace;
        f.chord = n.in_chord;
        f.articulations = n.articulations;
        std::size_t cur = i;
        while (notes[cur].tied_to_next) {
            bool found = false;
            for (std::size_t j = 0; j < notes.size(); ++j)
                if (!used[j] && !notes[j].grace && notes[j].voice == notes[cur].voice &&
                    notes[j].start_tick == notes[cur].end_tick() &&
                    nt::midi_note_number(notes[j].pitch) == f.midi) {
                    used[j] = true;
                    f.length += notes[j].duration.ticks();
                    cur = j;
                    found = true;
                    break;
                }
            if (!found) break;
        }
        out.push_back(f);
    }
    std::sort(out.begin(), out.end());
    return out;
}

void check_same_notes(const nt::Score& a, const nt::Score& b)
{
    REQUIRE(a.staff_count() == b.staff_count());
    for (std::size_t s = 0; s < a.staff_count(); ++s) {
        const auto fa = flatten(a, s), fb = flatten(b, s);
        CHECK_EQ(fa.size(), fb.size());
        for (std::size_t i = 0; i < std::min(fa.size(), fb.size()); ++i) {
            if (!(fa[i] == fb[i])) {
                std::ostringstream ss;
                ss << "staff " << s << " note " << i << ": " << fa[i] << " vs " << fb[i];
                vcc_test::fail(__FILE__, __LINE__, ss.str());
            }
        }
    }
}

// --- The export project -----------------------------------------------------------

struct Built {
    std::unique_ptr<session::Session> session;
    std::string clarinet, piano;
};

nt::Score clarinet_score(const nt::Score& base)
{
    nt::Score s = base;
    nt::Staff st = s.staff(0);
    st.transposition_semitones = -2;
    s.set_staff(0, st);
    nt::ScoreNote c5 = note(0, nt::quarter(), P(nt::Step::C, 5));
    c5.articulations = nt::Articulation::Accent | nt::Articulation::Staccato;
    s.insert_note(0, c5);
    s.insert_note(0, note(Q, trip(nt::NoteValue::Eighth), P(nt::Step::D, 5)));
    s.insert_note(0, note(Q + 960, trip(nt::NoteValue::Eighth), P(nt::Step::E, 5)));
    s.insert_note(0, note(Q + 1920, trip(nt::NoteValue::Eighth), P(nt::Step::F, 5)));
    s.insert_note(0, note(2 * Q, nt::half(1), P(nt::Step::A, 5)));  // crosses into bar 2
    nt::ScoreNote grace = note(BAR + Q, nt::eighth(), P(nt::Step::B, 4));
    grace.grace = true;
    s.insert_note(0, grace);
    nt::ScoreNote c5b = note(BAR + Q, nt::quarter(1), P(nt::Step::C, 5));
    c5b.articulations = nt::to_mask(nt::Articulation::Tenuto);
    s.insert_note(0, c5b);
    s.insert_note(0, note(BAR + Q + Q * 3 / 2, nt::eighth(), P(nt::Step::D, 5)));
    s.insert_note(0, rest(BAR + 3 * Q, nt::quarter()));
    // Bar 3 (3/4): a quarter triplet whose middle quarter is an eighth triplet.
    const Ticks b3 = 2 * BAR;
    s.insert_note(0, note(b3, trip(nt::NoteValue::Quarter), P(nt::Step::G, 5)));
    s.insert_note(0, note(b3 + 1920, nested(nt::NoteValue::Eighth), P(nt::Step::A, 5)));
    s.insert_note(0, note(b3 + 2560, nested(nt::NoteValue::Eighth), P(nt::Step::B, 5)));
    nt::ScoreNote c6 = note(b3 + 3200, nested(nt::NoteValue::Eighth), P(nt::Step::C, 6));
    c6.articulations = nt::Articulation::Trill | nt::Articulation::Fermata;
    s.insert_note(0, c6);
    s.insert_note(0, note(b3 + 3840, trip(nt::NoteValue::Quarter), P(nt::Step::G, 5)));
    nt::ScoreNote e5 = note(b3 + 2 * Q, nt::quarter(), P(nt::Step::E, 5));
    e5.articulations = (nt::Articulation::Marcato | nt::Articulation::Portato) |
                       (nt::Articulation::Falloff | nt::Articulation::UpBow);
    s.insert_note(0, e5);

    s.add_dynamic({0, nt::DynamicLevel::Piano, 0, {}, 0});
    s.add_dynamic({BAR, nt::DynamicLevel::Forte, 0, {}, 0});
    nt::Hairpin h;
    h.start_tick = Q;
    h.end_tick = BAR;
    h.kind = nt::HairpinKind::Crescendo;
    s.add_hairpin(h);
    nt::Slur sl;
    sl.start_tick = Q;
    sl.end_tick = 2 * Q;
    s.add_slur(sl);
    nt::TempoMarking tm;
    tm.tick = 0;
    tm.beats_per_minute = 100;
    tm.text = "Allegro";
    s.add_tempo(tm);
    s.add_text({0, 0, "dolce"});
    s.set_key_signature(b3, nt::KeySignature{2, false});
    return s;
}

nt::Score piano_score(const nt::Score& base)
{
    nt::Score s = base;
    std::vector<nt::Staff> staves(2);
    staves[0].clef = nt::Clef::Treble;
    staves[0].name = "Piano (upper)";
    staves[1].clef = nt::Clef::Bass;
    staves[1].name = "Piano (lower)";
    s.reset_staves(staves);
    s.insert_chord(0, 0, nt::half(), {P(nt::Step::C, 5), P(nt::Step::E, 5), P(nt::Step::G, 5)}, 0);
    s.insert_note(0, note(0, nt::quarter(), P(nt::Step::C, 4), 1));
    s.insert_note(0, note(Q, nt::quarter(), P(nt::Step::D, 4), 1));
    nt::ScoreNote c3 = note(0, nt::whole(), P(nt::Step::C, 3));
    c3.tied_to_next = true;
    s.insert_note(1, c3);
    s.insert_note(1, note(BAR, nt::whole(), P(nt::Step::C, 3)));
    s.add_dynamic({0, nt::DynamicLevel::MezzoForte, 0, {}, 1});
    s.set_key_signature(2 * BAR, nt::KeySignature{2, false});
    return s;
}

Built build_project()
{
    Built b;
    b.session = std::make_unique<session::Session>(catalogs());
    session::Session& s = *b.session;
    s.new_empty();
    s.edit("setup", [](project::ProjectData& d) {
        d.name = "Export Test";
        d.production.composer = "A. Tester";
        d.key = "F major";
        d.tempo = TempoMap(100.0);
        d.meter = MeterMap(TimeSignature{4, 4});
        d.meter.set(3, TimeSignature{3, 4});
        return true;
    });
    b.clarinet = s.add_track("Clarinet in Bb", "woodwinds.clarinet_bb");
    b.piano = s.add_track("Piano", "keys.piano");
    s.commit_score(b.clarinet, clarinet_score(s.data().find_track(b.clarinet)->active_version()->score), "notes");
    s.commit_score(b.piano, piano_score(s.data().find_track(b.piano)->active_version()->score), "notes");
    return b;
}

/// Keeps an exported document for the xmllint check.
void keep(const std::string& name, const std::string& xml)
{
    std::ofstream out(std::string(VCC_TEST_TMP_DIR) + "/" + name, std::ios::binary);
    out << xml;
}

ix::ExportOptions dated()
{
    ix::ExportOptions o;
    o.encoding_date = "2026-11-12";
    return o;
}

}  // namespace

// =============================================================================
// XML
// =============================================================================

TEST_CASE("xml: declaration, DOCTYPE with internal subset, comments, CDATA, entities and BOM")
{
    const std::string doc =
        "\xEF\xBB\xBF<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<!DOCTYPE score-partwise PUBLIC \"-//Recordare//DTD MusicXML 4.0 Partwise//EN\"\n"
        "  \"http://www.musicxml.org/dtds/partwise.dtd\" [ <!ENTITY x \"]>\"> <!-- ] > --> ]>\n"
        "<!-- a comment with <tags> -->\n"
        "<root a='1' b=\"two &amp; &quot;three&quot;\">\r\n"
        "  <?processing instruction?>\n"
        "  <t>&lt;&gt;&amp;&apos;&quot;&#65;&#x42;&#x266D;&#128512;</t>\n"
        "  <c><![CDATA[<not a tag> & stuff]]></c>\n"
        "  <e/><e x=\"y\" />\n"
        "  <u>&unknown; kept</u>\n"
        "</root>\n"
        "<!-- trailing comment -->\n";
    auto r = ix::xml::parse(doc);
    REQUIRE(r.ok);
    CHECK_EQ(r.root.name, std::string("root"));
    CHECK_EQ(r.root.attribute("a"), std::string("1"));
    CHECK_EQ(r.root.attribute("b"), std::string("two & \"three\""));
    CHECK_EQ(r.root.child_text("t"), std::string("<>&'\"AB\xE2\x99\xAD\xF0\x9F\x98\x80"));
    CHECK_EQ(r.root.child_text("c"), std::string("<not a tag> & stuff"));
    CHECK_EQ(r.root.children_named("e").size(), 2u);
    CHECK_EQ(r.root.children_named("e")[1]->attribute("x"), std::string("y"));
    CHECK_EQ(r.root.child_text("u"), std::string("&unknown; kept"));
    CHECK_EQ(r.root.line, 5);
    CHECK_EQ(r.root.child("t")->line, 7);
    CHECK(r.root.child("missing") == nullptr);
    CHECK_EQ(r.root.child_text("missing", "fallback"), std::string("fallback"));
}

TEST_CASE("xml: malformed documents fail with the line of the problem")
{
    struct Bad {
        const char* doc;
        int line;
        const char* contains;
    };
    const Bad cases[] = {
        {"<a>\n<b>\n</a>", 3, "mismatched end tag </a>, expected </b>"},
        {"<a>\n<b>\n", 3, "<b> opened at line 2 is not closed"},
        {"<a>\n x & y</a>", 2, "unescaped '&'"},
        {"<a/>\n<b/>", 2, "second root"},
        {"<a/>\ntext", 2, "text after the root"},
        {"<a x='1' x='2'/>", 1, "duplicate attribute"},
        {"<a>\n<!-- never closed", 2, "unterminated comment"},
        {"", 1, "no root element"},
        {"<?xml version='1.0'?>\n<a b=c/>", 2, "not quoted"},
        {"<a>&#xD800;</a>", 1, "not a valid character"},
        {"<a>\n\n</b>", 3, "mismatched"},
        {"</a>", 1, "without a matching start tag"},
        {"<a><![CDATA[x</a>", 1, "unterminated CDATA"},
        {"<!DOCTYPE a [ <!ELEMENT a ANY>", 1, "unterminated DOCTYPE"},
        {"\xFF\xFE<", 1, "UTF-16"},
    };
    for (const auto& c : cases) {
        auto r = ix::xml::parse(c.doc);
        CHECK(!r.ok);
        CHECK_EQ(r.error_line, c.line);
        if (r.error.find(c.contains) == std::string::npos)
            vcc_test::fail(__FILE__, __LINE__, std::string("error '") + r.error + "' lacks '" + c.contains + "'");
        CHECK(r.error.rfind("line " + std::to_string(c.line) + ":", 0) == 0);
    }
}

TEST_CASE("xml: deep nesting is refused, not a stack overflow")
{
    std::string doc;
    for (int i = 0; i < 100000; ++i) doc += "<a>";
    auto r = ix::xml::parse(doc);
    CHECK(!r.ok);
    CHECK(r.error.find("nested too deeply") != std::string::npos);
}

TEST_CASE("xml: writer escapes, indents and reads back")
{
    ix::xml::Writer w;
    w.declaration();
    w.open("root", {{"q", "a\"b<c>&\n"}});
    w.leaf("t", "x < y & z > w \"ok\"");
    w.empty("e", {{"k", "v"}});
    w.open("inner");
    w.leaf("ctl", std::string("a\x01" "b"));
    std::string out = w.finish();
    CHECK(out.find("  <t>x &lt; y &amp; z &gt; w \"ok\"</t>") != std::string::npos);
    CHECK(out.find("    <ctl>ab</ctl>") != std::string::npos);
    CHECK(out.find("</root>") != std::string::npos);
    auto r = ix::xml::parse(out);
    REQUIRE(r.ok);
    CHECK_EQ(r.root.attribute("q"), std::string("a\"b<c>&\n"));
    CHECK_EQ(r.root.child_text("t"), std::string("x < y & z > w \"ok\""));
    CHECK_EQ(r.root.child("inner")->child_text("ctl"), std::string("ab"));
}

// =============================================================================
// Export
// =============================================================================

TEST_CASE("export: a transposing part and a grand staff, checked element by element")
{
    Built b = build_project();
    std::vector<std::string> warnings;
    const std::string xml = b.session->export_musicxml(dated(), &warnings);
    CHECK(xml.find("<!DOCTYPE score-partwise PUBLIC \"-//Recordare//DTD MusicXML 4.0 Partwise//EN\"") !=
          std::string::npos);
    auto r = ix::xml::parse(xml);
    REQUIRE(r.ok);
    const Element& root = r.root;
    CHECK_EQ(root.name, std::string("score-partwise"));
    CHECK_EQ(root.attribute("version"), std::string("4.0"));
    CHECK_EQ(root.child_text("movement-title"), std::string("Export Test"));
    const Element* enc = root.child("identification")->child("encoding");
    CHECK_EQ(enc->child_text("software"), std::string("VC Composer 2"));
    CHECK_EQ(enc->child_text("encoding-date"), std::string("2026-11-12"));
    CHECK_EQ(root.child("identification")->child_text("creator"), std::string("A. Tester"));

    // Part list.
    const auto sps = root.child("part-list")->children_named("score-part");
    REQUIRE(sps.size() == 2u);
    CHECK_EQ(sps[0]->child_text("part-name"), std::string("Clarinet in Bb"));
    CHECK_EQ(sps[0]->child_text("part-abbreviation"), std::string("Cl."));
    CHECK_EQ(sps[0]->child("midi-instrument")->child_text("midi-program"), std::string("72"));
    CHECK_EQ(sps[0]->child("midi-instrument")->child_text("midi-channel"), std::string("1"));
    CHECK_EQ(sps[1]->child_text("part-name"), std::string("Piano"));
    CHECK_EQ(sps[1]->child("midi-instrument")->child_text("midi-program"), std::string("1"));

    // --- Clarinet ---
    const Element* p1 = part_by_id(root, "P1");
    REQUIRE(p1);
    const auto m = all(*p1, "measure");
    REQUIRE(m.size() == 3u);
    CHECK_EQ(m[0]->attribute("number"), std::string("1"));
    const Element* a = m[0]->child("attributes");
    REQUIRE(a);
    // Ticks used: 640 (nested), 960, 1440, ... -> gcd 160 -> 18 per quarter.
    CHECK_EQ(a->child_text("divisions"), std::string("18"));
    CHECK_EQ(a->child("key")->child_text("fifths"), std::string("1"));  // F major sounding, G major written
    CHECK_EQ(a->child("key")->child_text("mode"), std::string("major"));
    CHECK_EQ(a->child("time")->child_text("beats"), std::string("4"));
    CHECK_EQ(a->child("time")->child_text("beat-type"), std::string("4"));
    CHECK_EQ(a->child("clef")->child_text("sign"), std::string("G"));
    CHECK_EQ(a->child("transpose")->child_text("diatonic"), std::string("-1"));
    CHECK_EQ(a->child("transpose")->child_text("chromatic"), std::string("-2"));
    CHECK(a->child("staves") == nullptr);

    auto n1 = notes_of(*m[0]);
    REQUIRE(n1.size() == 5u);
    CHECK_EQ(pitch_of(*n1[0]), std::string("C5"));
    CHECK_EQ(n1[0]->child_text("duration"), std::string("18"));
    CHECK_EQ(n1[0]->child_text("type"), std::string("quarter"));
    CHECK(find_first(*n1[0], "accent") != nullptr);
    CHECK(find_first(*n1[0], "staccato") != nullptr);
    // Triplet with a bracket and a slur over it.
    for (int i = 1; i <= 3; ++i) {
        CHECK_EQ(n1[static_cast<std::size_t>(i)]->child_text("type"), std::string("eighth"));
        CHECK_EQ(n1[static_cast<std::size_t>(i)]->child_text("duration"), std::string("6"));
        CHECK_EQ(n1[static_cast<std::size_t>(i)]->child("time-modification")->child_text("actual-notes"),
                 std::string("3"));
    }
    CHECK_EQ(find_first(*n1[1], "tuplet")->attribute("type"), std::string("start"));
    CHECK_EQ(find_first(*n1[3], "tuplet")->attribute("type"), std::string("stop"));
    CHECK(find_first(*n1[2], "tuplet") == nullptr);
    CHECK_EQ(find_first(*n1[1], "slur")->attribute("type"), std::string("start"));
    CHECK_EQ(find_first(*n1[3], "slur")->attribute("type"), std::string("stop"));
    CHECK_EQ(find_first(*n1[1], "slur")->attribute("number"), find_first(*n1[3], "slur")->attribute("number"));
    // F5 needs a natural against the written key of G major.
    CHECK_EQ(pitch_of(*n1[3]), std::string("F5"));
    CHECK_EQ(n1[3]->child_text("accidental"), std::string("natural"));
    CHECK(n1[0]->child("accidental") == nullptr);
    // The dotted half crossing the barline: a tied half here...
    CHECK_EQ(pitch_of(*n1[4]), std::string("A5"));
    CHECK_EQ(n1[4]->child_text("type"), std::string("half"));
    CHECK(n1[4]->child("dot") == nullptr);
    CHECK_EQ(n1[4]->child_text("duration"), std::string("36"));
    CHECK(has_tie(*n1[4], "start"));
    CHECK(find_first(*n1[4], "tied") != nullptr);

    // ...and a quarter in bar 2.
    auto n2 = notes_of(*m[1]);
    REQUIRE(n2.size() == 5u);
    CHECK_EQ(pitch_of(*n2[0]), std::string("A5"));
    CHECK_EQ(n2[0]->child_text("type"), std::string("quarter"));
    CHECK(has_tie(*n2[0], "stop"));
    CHECK(!has_tie(*n2[0], "start"));
    // Grace note, then the dotted quarter it ornaments.
    CHECK(n2[1]->child("grace") != nullptr);
    CHECK_EQ(n2[1]->child("grace")->attribute("slash"), std::string("yes"));
    CHECK(n2[1]->child("duration") == nullptr);
    CHECK_EQ(pitch_of(*n2[1]), std::string("B4"));
    CHECK_EQ(pitch_of(*n2[2]), std::string("C5"));
    CHECK_EQ(n2[2]->children_named("dot").size(), 1u);
    CHECK(find_first(*n2[2], "tenuto") != nullptr);
    CHECK(n2[4]->child("rest") != nullptr);
    CHECK_EQ(n2[4]->child_text("type"), std::string("quarter"));

    // Bar 3: meter and key change, nested tuplets.
    const Element* a3 = m[2]->child("attributes");
    REQUIRE(a3);
    CHECK_EQ(a3->child("time")->child_text("beats"), std::string("3"));
    CHECK_EQ(a3->child("key")->child_text("fifths"), std::string("4"));  // D major sounding, E major written
    CHECK(a3->child("divisions") == nullptr);
    auto n3 = notes_of(*m[2]);
    REQUIRE(n3.size() == 6u);
    CHECK_EQ(n3[0]->child("time-modification")->child_text("actual-notes"), std::string("3"));
    CHECK_EQ(n3[1]->child("time-modification")->child_text("actual-notes"), std::string("9"));
    CHECK_EQ(n3[1]->child("time-modification")->child_text("normal-notes"), std::string("4"));
    CHECK_EQ(n3[1]->child_text("duration"), std::string("4"));
    int starts = 0, stops = 0;
    for (const auto* n : n3)
        for (const auto* nots : n->children_named("notations"))
            for (const auto* t : nots->children_named("tuplet")) {
                if (t->attribute("type") == "start") ++starts;
                if (t->attribute("type") == "stop") ++stops;
            }
    CHECK_EQ(starts, 2);
    CHECK_EQ(stops, 2);
    CHECK(find_first(*n3[1], "tuplet-actual") != nullptr);
    CHECK(find_first(*n3[3], "trill-mark") != nullptr);
    CHECK(find_first(*n3[3], "fermata") != nullptr);
    CHECK(find_first(*n3[5], "strong-accent") != nullptr);
    CHECK(find_first(*n3[5], "detached-legato") != nullptr);
    CHECK(find_first(*n3[5], "falloff") != nullptr);
    CHECK(find_first(*n3[5], "up-bow") != nullptr);

    // Directions: tempo, dynamics, hairpin, words.
    CHECK(find_first(*p1, "metronome") != nullptr);
    CHECK_EQ(find_first(*p1, "metronome")->child_text("per-minute"), std::string("100"));
    CHECK_EQ(find_first(*p1, "sound")->attribute("tempo"), std::string("100"));
    bool allegro = false, dolce = false;
    for (const auto* d : m[0]->children_named("direction"))
        for (const auto* dt : d->children_named("direction-type"))
            if (const Element* w = dt->child("words")) {
                allegro = allegro || w->trimmed_text() == "Allegro";
                dolce = dolce || w->trimmed_text() == "dolce";
            }
    CHECK(allegro);
    CHECK(dolce);
    CHECK(find_first(*m[0], "p") != nullptr);
    CHECK(find_first(*m[1], "f") != nullptr);
    std::vector<std::string> wedges;
    for (const auto* mm : m)
        for (const auto* d : mm->children_named("direction"))
            if (const Element* w = find_first(*d, "wedge")) wedges.push_back(w->attribute("type"));
    CHECK_EQ(wedges.size(), 2u);
    if (wedges.size() == 2) {
        CHECK_EQ(wedges[0], std::string("crescendo"));
        CHECK_EQ(wedges[1], std::string("stop"));
    }

    // --- Piano ---
    const Element* p2 = part_by_id(root, "P2");
    REQUIRE(p2);
    const auto pm = all(*p2, "measure");
    REQUIRE(pm.size() == 3u);
    const Element* pa = pm[0]->child("attributes");
    CHECK_EQ(pa->child_text("staves"), std::string("2"));
    CHECK_EQ(pa->children_named("clef").size(), 2u);
    CHECK_EQ(pa->children_named("clef")[1]->attribute("number"), std::string("2"));
    CHECK_EQ(pa->children_named("clef")[1]->child_text("sign"), std::string("F"));
    CHECK(pa->child("transpose") == nullptr);
    CHECK_EQ(pa->child("key")->child_text("fifths"), std::string("-1"));
    CHECK(find_first(*p2, "metronome") == nullptr);  // tempo goes in the first part only
    auto pn = notes_of(*pm[0]);
    REQUIRE(pn.size() == 6u);
    CHECK(pn[0]->child("chord") == nullptr);
    CHECK(pn[1]->child("chord") != nullptr);
    CHECK(pn[2]->child("chord") != nullptr);
    CHECK_EQ(pn[0]->child_text("voice"), std::string("1"));
    CHECK_EQ(pn[3]->child_text("voice"), std::string("2"));
    CHECK_EQ(pn[5]->child_text("voice"), std::string("5"));
    CHECK_EQ(pn[5]->child_text("staff"), std::string("2"));
    CHECK(has_tie(*pn[5], "start"));
    CHECK_EQ(count_named(*pm[0], "backup"), 2);
    // An mf between the staves of the lower hand.
    bool mf_on_2 = false;
    for (const auto* d : pm[0]->children_named("direction"))
        if (find_first(*d, "mf")) mf_on_2 = d->child_text("staff") == "2";
    CHECK(mf_on_2);
    // Bar 3 of the piano is empty: a measure rest on each staff.
    auto pr = notes_of(*pm[2]);
    REQUIRE(pr.size() == 2u);
    CHECK_EQ(pr[0]->child("rest")->attribute("measure"), std::string("yes"));
    CHECK_EQ(pr[1]->child_text("staff"), std::string("2"));
    CHECK_EQ(pr[0]->child_text("duration"), std::string("3"));  // 3/4 at 1 division per quarter
}

TEST_CASE("export: concert pitch drops <transpose> and sounds the notes")
{
    Built b = build_project();
    ix::ExportOptions o = dated();
    o.concert_pitch = true;
    o.track_ids = {b.clarinet};
    const std::string xml = b.session->export_musicxml(o);
    keep("export_concert.musicxml", xml);
    auto r = ix::xml::parse(xml);
    REQUIRE(r.ok);
    CHECK_EQ(r.root.child("part-list")->children_named("score-part").size(), 1u);
    const Element* p1 = part_by_id(r.root, "P1");
    REQUIRE(p1);
    const Element* a = all(*p1, "measure")[0]->child("attributes");
    CHECK(a->child("transpose") == nullptr);
    CHECK_EQ(a->child("key")->child_text("fifths"), std::string("-1"));
    const auto n = notes_of(*all(*p1, "measure")[0]);
    CHECK_EQ(pitch_of(*n[0]), std::string("Bb4"));  // written C5 on a B-flat clarinet
    CHECK(n[0]->child("accidental") == nullptr);    // Bb is in F major
    CHECK_EQ(pitch_of(*n[3]), std::string("Eb5"));  // written F5
    CHECK_EQ(n[3]->child_text("accidental"), std::string("flat"));
}

TEST_CASE("export: minimal divisions, octave clefs, percussion and an empty project")
{
    session::Session s(catalogs());
    s.new_empty();
    const std::string g = s.add_track("Guitar", "rhythm.acoustic_guitar");
    const std::string d = s.add_track("Drums", "rhythm.drum_kit");
    nt::Score gs = s.data().find_track(g)->active_version()->score;
    nt::Staff st = gs.staff(0);
    st.clef = nt::Clef::TrebleOctaveDown;
    st.octave_shift = -1;
    gs.set_staff(0, st);
    gs.insert_note(0, note(0, nt::quarter(), P(nt::Step::E, 4)));
    gs.insert_note(0, note(Q, nt::half(), P(nt::Step::G, 4)));
    s.commit_score(g, gs, "g");
    nt::Score ds = s.data().find_track(d)->active_version()->score;
    nt::Staff dst = ds.staff(0);
    dst.clef = nt::Clef::Percussion;
    ds.set_staff(0, dst);
    ds.insert_note(0, note(0, nt::quarter(), P(nt::Step::C, 2)));  // 36 kick
    ds.insert_note(0, note(Q, nt::quarter(), P(nt::Step::D, 2)));  // 38 snare
    s.commit_score(d, ds, "d");

    std::string xml = s.export_musicxml(dated());
    keep("export_guitar_drums.musicxml", xml);
    auto r = ix::xml::parse(xml);
    REQUIRE(r.ok);
    const Element* p1 = part_by_id(r.root, "P1");
    const Element* a = all(*p1, "measure")[0]->child("attributes");
    CHECK_EQ(a->child_text("divisions"), std::string("1"));
    CHECK_EQ(a->child("clef")->child_text("clef-octave-change"), std::string("-1"));
    CHECK(a->child("transpose") == nullptr);
    CHECK_EQ(pitch_of(*notes_of(*all(*p1, "measure")[0])[0]), std::string("E3"));  // sounding octave under 8vb

    const Element* p2 = part_by_id(r.root, "P2");
    const auto sp = r.root.child("part-list")->children_named("score-part");
    CHECK_EQ(sp[1]->children_named("score-instrument").size(), 2u);
    CHECK_EQ(sp[1]->children_named("midi-instrument")[0]->child_text("midi-unpitched"), std::string("37"));
    CHECK_EQ(sp[1]->children_named("midi-instrument")[0]->child_text("midi-channel"), std::string("10"));
    const auto dn = notes_of(*all(*p2, "measure")[0]);
    REQUIRE(dn.size() >= 2u);
    CHECK(dn[0]->child("unpitched") != nullptr);
    CHECK_EQ(dn[0]->child("instrument")->attribute("id"), std::string("P2-I37"));
    CHECK_EQ(all(*p2, "measure")[0]->child("attributes")->child("clef")->child_text("sign"),
             std::string("percussion"));

    // Round trip of both.
    auto imp = ix::import_musicxml(xml);
    REQUIRE(imp.ok);
    REQUIRE(imp.parts.size() == 2u);
    CHECK(imp.parts[0].score.staff(0).clef == nt::Clef::TrebleOctaveDown);
    CHECK_EQ(static_cast<int>(imp.parts[0].score.staff(0).octave_shift), -1);
    CHECK_EQ(imp.parts[0].score.staff(0).total_transposition(), -12);
    check_same_notes(s.data().find_track(g)->active_version()->score, imp.parts[0].score);
    CHECK(imp.parts[1].is_percussion);
    check_same_notes(s.data().find_track(d)->active_version()->score, imp.parts[1].score);

    // No tracks: still a valid document.
    session::Session empty(catalogs());
    empty.new_empty();
    std::vector<std::string> warnings;
    auto e = ix::xml::parse(empty.export_musicxml(dated(), &warnings));
    REQUIRE(e.ok);
    CHECK(e.root.child("part-list") != nullptr);
    CHECK(!warnings.empty());
}

TEST_CASE("export: a short first bar is written as a pickup and read back as one")
{
    session::Session s(catalogs());
    s.new_empty();
    s.edit("meter", [](project::ProjectData& d) {
        d.meter = MeterMap(TimeSignature{1, 4});
        d.meter.set(2, TimeSignature{4, 4});
        return true;
    });
    const std::string t = s.add_track("Violin", "strings.violin");
    nt::Score sc = s.data().find_track(t)->active_version()->score;
    sc.insert_note(0, note(0, nt::quarter(), P(nt::Step::G, 4)));
    sc.insert_note(0, note(Q, nt::whole(), P(nt::Step::C, 5)));
    s.commit_score(t, sc, "n");
    const std::string xml = s.export_musicxml(dated());
    keep("export_pickup.musicxml", xml);
    auto r = ix::xml::parse(xml);
    REQUIRE(r.ok);
    const auto m = all(*part_by_id(r.root, "P1"), "measure");
    REQUIRE(m.size() == 2u);
    CHECK_EQ(m[0]->attribute("number"), std::string("0"));
    CHECK_EQ(m[0]->attribute("implicit"), std::string("yes"));
    CHECK_EQ(m[0]->child("attributes")->child("time")->child_text("beats"), std::string("4"));
    CHECK_EQ(m[1]->attribute("number"), std::string("1"));
    CHECK(m[1]->child("attributes") == nullptr);

    auto imp = ix::import_musicxml(xml);
    REQUIRE(imp.ok);
    CHECK(imp.has_pickup);
    CHECK_EQ(imp.pickup_ticks, Q);
    REQUIRE(imp.time_signatures.size() == 2u);
    CHECK_EQ(imp.time_signatures[0].numerator, 1);
    CHECK_EQ(imp.time_signatures[1].bar, 2);
    CHECK_EQ(imp.time_signatures[1].tick, Q);
    CHECK_EQ(imp.time_signatures[1].numerator, 4);
    check_same_notes(sc, imp.parts[0].score);

    ix::ExportOptions plain = dated();
    plain.detect_pickup = false;
    auto r2 = ix::xml::parse(s.export_musicxml(plain));
    REQUIRE(r2.ok);
    const auto m2 = all(*part_by_id(r2.root, "P1"), "measure");
    CHECK_EQ(m2[0]->attribute("number"), std::string("1"));
    CHECK_EQ(m2[0]->child("attributes")->child("time")->child_text("beats"), std::string("1"));
}

#ifdef VCC_XMLLINT
// Well-formedness always; validation against the MusicXML 4.0 schema when
// VCC_MUSICXML_XSD names a local copy of musicxml.xsd (with xml.xsd and
// xlink.xsd beside it, imports pointed at them).
TEST_CASE("export: xmllint finds the documents well-formed (and valid, given the XSD)")
{
    Built b = build_project();
    keep("export_test.musicxml", b.session->export_musicxml(dated()));
    const char* xsd = std::getenv("VCC_MUSICXML_XSD");
    for (const char* name : {"export_test.musicxml", "export_concert.musicxml", "export_guitar_drums.musicxml",
                             "export_pickup.musicxml"}) {
        const std::string path = std::string(VCC_TEST_TMP_DIR) + "/" + name;
        std::string cmd = std::string(VCC_XMLLINT) + " --noout --nonet";
        if (xsd && *xsd) cmd += std::string(" --schema \"") + xsd + "\"";
        cmd += " \"" + path + "\"";
        CHECK_EQ(std::system(cmd.c_str()), 0);
    }
}
#endif

// =============================================================================
// Round trip
// =============================================================================

TEST_CASE("round trip: notes, ties, tuplets, grace notes, markings and maps survive export and import")
{
    Built b = build_project();
    const auto& d = b.session->data();
    const std::string xml = b.session->export_musicxml(dated());
    auto imp = ix::import_musicxml(xml);
    REQUIRE(imp.ok);
    REQUIRE(imp.parts.size() == 2u);
    CHECK_EQ(imp.title, std::string("Export Test"));
    CHECK_EQ(imp.composer, std::string("A. Tester"));

    const nt::Score& cl = d.find_track(b.clarinet)->active_version()->score;
    const nt::Score& pn = d.find_track(b.piano)->active_version()->score;
    const nt::Score& icl = imp.parts[0].score;
    const nt::Score& ipn = imp.parts[1].score;

    CHECK_EQ(imp.parts[0].name, std::string("Clarinet in Bb"));
    CHECK_EQ(imp.parts[0].abbreviation, std::string("Cl."));
    CHECK_EQ(imp.parts[0].midi_program, 71);
    CHECK_EQ(static_cast<int>(icl.staff(0).transposition_semitones), -2);
    CHECK_EQ(ipn.staff_count(), 2u);
    CHECK(ipn.staff(1).clef == nt::Clef::Bass);

    check_same_notes(cl, icl);
    check_same_notes(pn, ipn);

    // The nested tuplet comes back nested.
    bool found_nested = false;
    for (const auto& n : icl.notes(0))
        if (n.start_tick == 2 * BAR + 1920) {
            found_nested = true;
            CHECK(n.duration.is_nested());
            CHECK(n.duration == nested(nt::NoteValue::Eighth));
        }
    CHECK(found_nested);
    // The barline-crossing note comes back as a tied half and quarter.
    int a5 = 0;
    for (const auto& n : icl.notes(0))
        if (n.pitch == P(nt::Step::A, 5) && !n.duration.is_nested()) {
            ++a5;
            if (n.start_tick == 2 * Q) {
                CHECK(n.tied_to_next);
                CHECK(n.duration == nt::half());
            }
            if (n.start_tick == BAR) CHECK(n.duration == nt::quarter());
        }
    CHECK_EQ(a5, 2);

    // Markings.
    REQUIRE(icl.dynamics().size() == 2u);
    CHECK_EQ(icl.dynamics()[0].tick, 0);
    CHECK(icl.dynamics()[0].level == nt::DynamicLevel::Piano);
    CHECK_EQ(icl.dynamics()[1].tick, BAR);
    CHECK(icl.dynamics()[1].level == nt::DynamicLevel::Forte);
    REQUIRE(ipn.dynamics().size() == 1u);
    CHECK_EQ(static_cast<int>(ipn.dynamics()[0].staff), 1);
    CHECK(ipn.dynamics()[0].level == nt::DynamicLevel::MezzoForte);
    REQUIRE(icl.hairpins().size() == 1u);
    CHECK_EQ(icl.hairpins()[0].start_tick, Q);
    CHECK_EQ(icl.hairpins()[0].end_tick, BAR);
    CHECK(icl.hairpins()[0].kind == nt::HairpinKind::Crescendo);
    REQUIRE(icl.slurs().size() == 1u);
    CHECK_EQ(icl.slurs()[0].start_tick, Q);
    CHECK_EQ(icl.slurs()[0].end_tick, 2 * Q);
    REQUIRE(icl.tempos().size() == 1u);
    CHECK_EQ(icl.tempos()[0].text, std::string("Allegro"));
    CHECK_NEAR(icl.tempos()[0].beats_per_minute, 100.0, 1e-9);
    REQUIRE(icl.texts().size() == 1u);
    CHECK_EQ(icl.texts()[0].text, std::string("dolce"));

    // Maps.
    REQUIRE(imp.tempos.size() == 1u);
    CHECK_NEAR(imp.tempos[0].bpm, 100.0, 1e-9);
    REQUIRE(imp.time_signatures.size() == 2u);
    CHECK_EQ(imp.time_signatures[1].bar, 3);
    CHECK_EQ(imp.time_signatures[1].tick, 2 * BAR);
    CHECK_EQ(imp.time_signatures[1].numerator, 3);
    REQUIRE(imp.keys.size() == 2u);
    CHECK_EQ(imp.keys[0].fifths, -1);  // concert, from the clarinet's written G major
    CHECK_EQ(imp.keys[1].tick, 2 * BAR);
    CHECK_EQ(imp.keys[1].fifths, 2);
    CHECK_EQ(static_cast<int>(icl.key_signature_at(0).fifths), -1);
    CHECK_EQ(static_cast<int>(icl.key_signature_at(2 * BAR).fifths), 2);
    CHECK_EQ(static_cast<int>(ipn.key_signature_at(2 * BAR).fifths), 2);
    CHECK(!imp.has_pickup);

    // And the import exports to the same document (a fixed point).
    session::Session s2(catalogs());
    s2.new_empty();
    auto sum = s2.import_musicxml(xml, true);
    REQUIRE(sum.ok);
    CHECK_EQ(s2.export_musicxml(dated()), xml);
}

// =============================================================================
// Import of hand-written fixtures
// =============================================================================

TEST_CASE("import: Finale-style file with divisions of 1")
{
    auto r = ix::import_musicxml(fixture("finale_style_div1.musicxml"));
    REQUIRE(r.ok);
    CHECK_EQ(r.title, std::string("Scale Study & Coda"));
    CHECK_EQ(r.composer, std::string("Test Writer"));
    REQUIRE(r.parts.size() == 1u);
    const auto& p = r.parts[0];
    CHECK_EQ(p.name, std::string("Flute"));
    CHECK_EQ(p.abbreviation, std::string("Fl."));
    CHECK_EQ(p.midi_program, 73);
    CHECK_EQ(p.midi_channel, 0);
    CHECK_EQ(r.measure_count, 3);
    REQUIRE(r.time_signatures.size() == 1u);
    CHECK_EQ(r.time_signatures[0].numerator, 3);
    REQUIRE(r.keys.size() == 1u);
    CHECK_EQ(r.keys[0].fifths, -1);
    REQUIRE(r.tempos.size() == 1u);
    CHECK_NEAR(r.tempos[0].bpm, 96.0, 1e-9);
    CHECK(p.score.tempos().empty());  // <sound tempo> alone is playback, not a marking
    const auto& n = p.score.notes(0);
    REQUIRE(n.size() == 5u);
    CHECK(n[0].pitch == P(nt::Step::F, 4));
    CHECK_EQ(n[1].start_tick, Q);
    CHECK(n[3].pitch == P(nt::Step::B, 4, -1));
    CHECK_EQ(n[3].start_tick, 3 * Q);
    CHECK(n[4].duration == nt::half());
    CHECK_EQ(n[4].start_tick, 4 * Q);
    CHECK(nt::has_articulation(n[4].articulations, nt::Articulation::Fermata));
    bool lyrics = false;
    for (const auto& w : r.warnings) lyrics = lyrics || w == "1 lyrics ignored";
    CHECK(lyrics);
}

TEST_CASE("import: Sibelius-style file with divisions of 480, a pickup and a B-flat clarinet")
{
    auto r = ix::import_musicxml(fixture("sibelius_style_div480.musicxml"));
    REQUIRE(r.ok);
    CHECK_EQ(r.title, std::string("Little Phrase in C"));
    REQUIRE(r.parts.size() == 1u);
    const auto& p = r.parts[0];
    CHECK_EQ(p.name, std::string("Clarinet in B\xE2\x99\xAD"));
    CHECK_EQ(p.midi_program, 71);
    CHECK_EQ(static_cast<int>(p.score.staff(0).transposition_semitones), -2);
    CHECK(r.has_pickup);
    CHECK_EQ(r.pickup_ticks, Q);
    REQUIRE(r.time_signatures.size() == 2u);
    CHECK_EQ(r.time_signatures[0].numerator, 1);
    CHECK_EQ(r.time_signatures[0].denominator, 4);
    CHECK_EQ(r.time_signatures[1].tick, Q);
    CHECK_EQ(r.keys[0].fifths, 0);  // written D major on a B-flat instrument is concert C
    REQUIRE(r.tempos.size() == 1u);
    CHECK_NEAR(r.tempos[0].bpm, 72.0, 1e-9);
    REQUIRE(p.score.tempos().size() == 1u);
    CHECK_EQ(p.score.tempos()[0].text, std::string("Andante"));
    REQUIRE(p.score.texts().size() == 1u);
    CHECK_EQ(p.score.texts()[0].text, std::string("dolce"));

    const auto& n = p.score.notes(0);
    // E4, three triplets, B4, C#5 (tied), C#5, grace D5, E5, F#5, rest, D5.
    REQUIRE(n.size() == 12u);
    CHECK(n[0].pitch == P(nt::Step::E, 4));  // written pitch kept
    CHECK_EQ(n[1].start_tick, Q);
    CHECK(n[1].duration == trip(nt::NoteValue::Eighth));
    CHECK_EQ(n[2].start_tick, Q + 960);
    CHECK_EQ(n[4].start_tick, 2 * Q);
    CHECK(nt::has_articulation(n[4].articulations, nt::Articulation::Accent));
    CHECK(nt::has_articulation(n[4].articulations, nt::Articulation::Staccato));
    CHECK(n[5].tied_to_next);
    CHECK(n[5].pitch == P(nt::Step::C, 5, 1));
    CHECK_EQ(n[6].start_tick, Q + BAR);
    CHECK(!n[6].tied_to_next);
    // The grace note sorts with the dotted quarter it ornaments.
    const nt::ScoreNote* grace = nullptr;
    for (const auto& x : n)
        if (x.grace) grace = &x;
    REQUIRE(grace);
    CHECK_EQ(grace->start_tick, Q + BAR + Q);
    CHECK(grace->grace_slashed);
    CHECK(grace->pitch == P(nt::Step::D, 5));
    CHECK(n.back().pitch == P(nt::Step::D, 5));
    CHECK(n.back().duration == nt::whole());
    CHECK_EQ(n.back().start_tick, Q + 2 * BAR);
    int rests = 0;
    for (const auto& x : n) rests += x.is_rest ? 1 : 0;
    CHECK_EQ(rests, 1);

    REQUIRE(p.score.slurs().size() == 1u);
    CHECK_EQ(p.score.slurs()[0].start_tick, Q);
    CHECK_EQ(p.score.slurs()[0].end_tick, 2 * Q);
    REQUIRE(p.score.hairpins().size() == 1u);
    CHECK_EQ(p.score.hairpins()[0].start_tick, 2 * Q);
    CHECK_EQ(p.score.hairpins()[0].end_tick, Q + BAR);
    REQUIRE(p.score.dynamics().size() == 2u);
    CHECK(p.score.dynamics()[1].level == nt::DynamicLevel::Forte);
    CHECK_EQ(p.score.dynamics()[1].tick, Q + BAR);
}

TEST_CASE("import: MuseScore-style grand staff with voices, backup/forward and changes")
{
    auto r = ix::import_musicxml(fixture("musescore_style_piano.musicxml"));
    REQUIRE(r.ok);
    REQUIRE(r.parts.size() == 1u);
    const nt::Score& s = r.parts[0].score;
    REQUIRE(s.staff_count() == 2u);
    CHECK(s.staff(0).clef == nt::Clef::Treble);
    CHECK(s.staff(1).clef == nt::Clef::Bass);  // the later treble clef is not representable
    CHECK_EQ(s.staff(1).name, std::string("Piano (lower)"));

    const auto& up = s.notes(0);
    REQUIRE(up.size() == 5u);
    int chord = 0;
    for (const auto& n : up) chord += n.in_chord ? 1 : 0;
    CHECK_EQ(chord, 2);
    const nt::ScoreNote* g4 = nullptr;
    for (const auto& n : up)
        if (n.pitch == P(nt::Step::G, 4)) g4 = &n;
    REQUIRE(g4);
    CHECK_EQ(static_cast<int>(g4->voice), 1);
    CHECK(g4->duration == nt::half());
    CHECK_EQ(up.back().start_tick, 2 * Q);
    CHECK(up.back().duration == nt::half(1));

    const auto& low = s.notes(1);
    REQUIRE(low.size() == 4u);
    CHECK(low[0].pitch == P(nt::Step::C, 3));
    CHECK(low[0].duration == nt::eighth());
    CHECK_EQ(low[1].start_tick, Q);  // after the <forward>
    CHECK(low[1].pitch == P(nt::Step::G, 2));
    CHECK_EQ(static_cast<int>(low[1].voice), 0);
    CHECK_EQ(low[2].start_tick, 2 * Q);
    CHECK(low[3].is_rest);
    CHECK_EQ(low[3].start_tick, 4 * Q);

    REQUIRE(r.time_signatures.size() == 2u);
    CHECK_EQ(r.time_signatures[1].tick, 2 * Q);
    CHECK_EQ(r.time_signatures[1].numerator, 3);
    REQUIRE(r.keys.size() == 2u);
    CHECK_EQ(r.keys[1].fifths, 1);
    CHECK_EQ(r.keys[1].tick, 2 * Q);
    bool clef_warning = false;
    for (const auto& w : r.warnings) clef_warning = clef_warning || w.find("clef changes") != std::string::npos;
    CHECK(clef_warning);
    // Its chord symbol (kind major, empty text) is read, not ignored.
    REQUIRE(r.chords.size() == 1u);
    CHECK_EQ(r.chords[0].tick, 0);
    CHECK_EQ(r.chords[0].text, std::string("C"));
}

TEST_CASE("import: a percussion part maps instruments to General MIDI drums")
{
    auto r = ix::import_musicxml(fixture("percussion.musicxml"));
    REQUIRE(r.ok);
    const auto& p = r.parts[0];
    CHECK(p.is_percussion);
    CHECK_EQ(p.midi_channel, 9);
    CHECK(p.score.staff(0).clef == nt::Clef::Percussion);
    const auto& n = p.score.notes(0);
    REQUIRE(n.size() == 4u);
    CHECK_EQ(nt::midi_note_number(n[0].pitch), 35);
    CHECK_EQ(nt::midi_note_number(n[1].pitch), 38);
    CHECK(nt::has_articulation(n[1].articulations, nt::Articulation::Accent));

    session::Session s(catalogs());
    s.new_empty();
    auto sum = s.import_musicxml(fixture("percussion.musicxml"), true);
    REQUIRE(sum.ok);
    const project::Track* t = s.data().find_track(sum.track_ids[0]);
    CHECK_EQ(t->instrument_standard_id, std::string("rhythm.drum_kit"));
    CHECK_EQ(t->midi_channel, 9);
}

TEST_CASE("import: score-timewise is converted")
{
    auto r = ix::import_musicxml(fixture("timewise.musicxml"));
    REQUIRE(r.ok);
    CHECK_EQ(r.title, std::string("Timewise Duet"));
    REQUIRE(r.parts.size() == 2u);
    CHECK_EQ(r.parts[1].name, std::string("Cello"));
    CHECK(r.parts[1].score.staff(0).clef == nt::Clef::Bass);
    REQUIRE(r.parts[0].score.notes(0).size() == 2u);
    CHECK_EQ(r.parts[0].score.notes(0)[1].start_tick, BAR);
    CHECK(r.parts[1].score.notes(0)[1].pitch == P(nt::Step::G, 2));
    bool converted = false;
    for (const auto& w : r.warnings) converted = converted || w.find("timewise") != std::string::npos;
    CHECK(converted);
}

TEST_CASE("import: malformed and foreign files fail cleanly with a line number")
{
    auto bad = ix::import_musicxml(fixture("malformed_mismatch.musicxml"));
    CHECK(!bad.ok);
    CHECK(bad.error.rfind("line 14:", 0) == 0);
    CHECK(bad.error.find("expected </pitch>") != std::string::npos);
    CHECK(bad.parts.empty());

    auto html = ix::import_musicxml(fixture("not_musicxml.xml"));
    CHECK(!html.ok);
    CHECK(html.error.find("not a MusicXML score") != std::string::npos);
    CHECK(html.error.rfind("line 2:", 0) == 0);

    auto empty = ix::import_musicxml("<score-partwise version=\"4.0\"><part-list/></score-partwise>");
    CHECK(!empty.ok);
    CHECK(empty.error.find("no parts") != std::string::npos);

    auto truncated = ix::import_musicxml(fixture("finale_style_div1.musicxml").substr(0, 2000));
    CHECK(!truncated.ok);
    CHECK(truncated.error.rfind("line ", 0) == 0);
}

TEST_CASE("import: tolerant of odd but legal input")
{
    // No divisions, a backup past the start, an unknown element, a 5-note
    // value without <type>, a hairpin without an end, a cue note.
    const std::string doc = R"(<?xml version="1.0"?>
<score-partwise>
  <part-list><score-part id="A"><part-name>Viola</part-name></score-part></part-list>
  <part id="A">
    <measure number="1">
      <attributes><time><beats>5</beats><beat-type>8</beat-type></time><clef><sign>C</sign><line>3</line></clef></attributes>
      <note><pitch><step>C</step><octave>4</octave></pitch><duration>5</duration><voice>1</voice></note>
      <backup><duration>99</duration></backup>
      <note><cue/><pitch><step>D</step><octave>4</octave></pitch><duration>1</duration><voice>2</voice><type>eighth</type></note>
      <direction><direction-type><wedge type="diminuendo"/></direction-type></direction>
      <frobnicate/>
    </measure>
  </part>
</score-partwise>)";
    auto r = ix::import_musicxml(doc);
    REQUIRE(r.ok);
    const auto& s = r.parts[0].score;
    CHECK(s.staff(0).clef == nt::Clef::Alto);
    // 5 quarters at the assumed 1 division per quarter: an overfull 5/8 bar
    // lengthened to 10/8, the note a tie chain (whole + quarter).
    REQUIRE(r.time_signatures.size() == 1u);
    CHECK_EQ(r.time_signatures[0].numerator, 10);
    CHECK_EQ(r.time_signatures[0].denominator, 8);
    const auto& n = s.notes(0);
    REQUIRE(n.size() == 2u);
    CHECK(n[0].tied_to_next);
    CHECK_EQ(n[0].duration.ticks() + n[1].duration.ticks(), 5 * Q);
    std::string all_warnings;
    for (const auto& w : r.warnings) all_warnings += w + "\n";
    CHECK(all_warnings.find("divisions") != std::string::npos);
    CHECK(all_warnings.find("clamped") != std::string::npos);
    CHECK(all_warnings.find("cue notes") != std::string::npos);
    CHECK(all_warnings.find("hairpins without an end") != std::string::npos);
    CHECK(all_warnings.find("frobnicate") != std::string::npos);
}

TEST_CASE("import: truncated and corrupted documents never crash")
{
    Built b = build_project();
    const std::string xml = b.session->export_musicxml(dated());
    int failures = 0;
    for (std::size_t len = 0; len < xml.size(); len += 97) {
        auto r = ix::import_musicxml(xml.substr(0, len));
        failures += r.ok ? 0 : 1;
        if (!r.ok) CHECK(r.error.rfind("line ", 0) == 0);
    }
    CHECK(failures > 0);
    std::uint32_t seed = 12345;
    const char bytes[] = {'<', '>', '/', '&', '"', '0', '9', '-', ' ', 'x'};
    for (int round = 0; round < 300; ++round) {
        std::string doc = xml;
        for (int k = 0; k < 4; ++k) {
            seed = seed * 1664525u + 1013904223u;
            const std::size_t at = seed % doc.size();
            seed = seed * 1664525u + 1013904223u;
            doc[at] = bytes[seed % sizeof(bytes)];
        }
        auto r = ix::import_musicxml(doc);
        if (r.ok) CHECK(!r.parts.empty());
        else CHECK(!r.error.empty());
    }
}

// =============================================================================
// Session
// =============================================================================

TEST_CASE("session: import as one undo step, replacing or adding to the project")
{
    session::Session s(catalogs());
    s.new_empty();
    const std::string existing = s.add_track("Oboe", "woodwinds.oboe");
    const std::size_t steps = s.project().history_size();

    auto sum = s.import_musicxml(fixture("sibelius_style_div480.musicxml"), true);
    REQUIRE(sum.ok);
    CHECK_EQ(s.project().history_size(), steps + 1);
    CHECK_EQ(s.undo_label(), std::string("Import MusicXML"));
    REQUIRE(sum.track_ids.size() == 1u);
    CHECK(s.data().find_track(existing) == nullptr);
    const project::Track* t = s.data().find_track(sum.track_ids[0]);
    REQUIRE(t);
    CHECK_EQ(t->instrument_standard_id, std::string("woodwinds.clarinet_bb"));
    CHECK_EQ(t->family, std::string("woodwinds"));
    CHECK_EQ(t->assignment.placeholder_program, 71);
    CHECK_EQ(s.data().name, std::string("Little Phrase in C"));
    CHECK_EQ(s.data().production.composer, std::string("Test Writer"));
    CHECK_EQ(s.data().key, std::string("C major"));
    CHECK_NEAR(s.data().tempo.bpm_at(0), 72.0, 1e-9);
    CHECK(s.data().meter.at_bar(1) == (TimeSignature{1, 4}));
    CHECK(s.data().meter.at_bar(2) == (TimeSignature{4, 4}));
    CHECK_EQ(s.data().meter.bar_start(2), Q);
    // The score was synced to that meter and still holds every note.
    const nt::Score& sc = t->active_version()->score;
    CHECK_EQ(sc.total_note_count(), 12u);
    CHECK_EQ(sc.measure_at(Q).start_tick, Q);
    CHECK_EQ(static_cast<int>(sc.staff(0).transposition_semitones), -2);
    CHECK_EQ(s.selected_track(), sum.track_ids[0]);

    // Added to the project: the project's meter and key stay, with a warning.
    auto add = s.import_musicxml(fixture("musescore_style_piano.musicxml"), false);
    REQUIRE(add.ok);
    CHECK_EQ(s.data().tracks.size(), 2u);
    const project::Track* piano = s.data().find_track(add.track_ids[0]);
    CHECK_EQ(piano->instrument_standard_id, std::string("keys.piano"));
    CHECK(s.data().meter.at_bar(1) == (TimeSignature{1, 4}));
    bool meter_warning = false;
    for (const auto& w : add.warnings) meter_warning = meter_warning || w.find("meter was kept") != std::string::npos;
    CHECK(meter_warning);

    // Undo removes the import in one step.
    REQUIRE(s.undo());
    CHECK_EQ(s.data().tracks.size(), 1u);
    REQUIRE(s.undo());
    CHECK(s.data().find_track(existing) != nullptr);
    CHECK_EQ(s.data().tracks.size(), 1u);

    // A failed import changes nothing.
    const std::size_t before = s.project().history_size();
    auto bad = s.import_musicxml(fixture("malformed_mismatch.musicxml"), true);
    CHECK(!bad.ok);
    CHECK(bad.error.find("line 14") != std::string::npos);
    CHECK_EQ(s.project().history_size(), before);
}

TEST_CASE("session: instruments are guessed from part names, transposition and program")
{
    session::Session s(catalogs());
    auto part = [](const std::string& name, int transposition = 0, int staves = 1, int program = -1) {
        ix::ImportedPart p;
        p.name = name;
        p.midi_program = program;
        std::vector<nt::Staff> st(static_cast<std::size_t>(staves));
        st[0].transposition_semitones = static_cast<std::int8_t>(transposition);
        p.score.reset_staves(st);
        return p;
    };
    CHECK_EQ(s.guess_instrument(part("Violin")), std::string("strings.violin"));
    CHECK_EQ(s.guess_instrument(part("Violin I")), std::string("strings.violin"));
    CHECK_EQ(s.guess_instrument(part("Violoncello")), std::string("strings.cello"));
    CHECK_EQ(s.guess_instrument(part("Contrabass", -12)), std::string("strings.double_bass"));
    CHECK_EQ(s.guess_instrument(part("Clarinet in A", -3)), std::string("woodwinds.clarinet_a"));
    CHECK_EQ(s.guess_instrument(part("Clarinet", -2)), std::string("woodwinds.clarinet_bb"));
    CHECK_EQ(s.guess_instrument(part("Horn 1", -7)), std::string("brass.horn_f"));
    CHECK_EQ(s.guess_instrument(part("Flute 2")), std::string("woodwinds.flute"));
    CHECK_EQ(s.guess_instrument(part("Acoustic Grand Piano", 0, 2)), std::string("keys.piano"));
    CHECK_EQ(s.guess_instrument(part("Staff", 0, 1, 40)), std::string("strings.violin"));
    CHECK_EQ(s.guess_instrument(part("Mystery")), std::string("keys.piano"));
}

TEST_CASE("chord symbols: session edits, save and reopen, MusicXML round trip through a transposing part")
{
    session::Session s(catalogs());
    s.new_empty();
    const std::string cl = s.add_track("Clarinet in Bb", "woodwinds.clarinet_bb");
    s.add_track("Piano", "keys.piano");
    const Ticks bar = 4 * Q;

    // Setting, replacing, refusing and removing, each one undo step.
    CHECK(s.set_chord_symbol(0, "C"));
    CHECK(s.undo_label() == "Chord symbol C");
    CHECK(s.set_chord_symbol(bar + 2 * Q, " G7/B "));
    CHECK(s.set_chord_symbol(2 * bar, "F#m7b5"));
    CHECK(s.set_chord_symbol(3 * bar, "Bbmaj9"));
    CHECK(!s.set_chord_symbol(3 * bar, "Bbmaj9"));  // unchanged: no step
    CHECK(!s.set_chord_symbol(4 * bar, "not a chord"));
    REQUIRE(s.data().chords.size() == 4);
    CHECK(s.data().chords[1].text == "G7/B");
    CHECK(s.chord_symbol_at(bar + 3 * Q) && s.chord_symbol_at(bar + 3 * Q)->text == "G7/B");
    CHECK(s.chord_symbol_at(bar) && s.chord_symbol_at(bar)->text == "C");
    CHECK(s.set_chord_symbol(0, "Cmaj7"));
    CHECK(s.data().chords[0].text == "Cmaj7");
    s.undo();
    CHECK(s.data().chords[0].text == "C");
    CHECK(s.set_chord_symbol(4 * bar, "Am"));
    CHECK(s.remove_chord_symbol(4 * bar));
    CHECK(s.undo_label() == "Remove chord symbol");
    CHECK(s.data().chords.size() == 4);
    // Replacing a span: only the symbols inside it go.
    CHECK(s.replace_chord_symbols(bar, 3 * bar, {{bar, "Dm7"}, {2 * bar, "G7"}}));
    REQUIRE(s.data().chords.size() == 4);
    CHECK(s.data().chords[1].text == "Dm7");
    CHECK(s.data().chords[3].text == "Bbmaj9");
    s.undo();

    // Saved and reopened.
    const std::string path = std::string(VCC_TEST_TMP_DIR) + "/chords.vcc2";
    REQUIRE(s.save_as(path));
    session::Session reopened(catalogs());
    REQUIRE(reopened.open(path));
    REQUIRE(reopened.data().chords.size() == 4);
    CHECK(reopened.data().chords[2].tick == 2 * bar);
    CHECK(reopened.data().chords[2].text == "F#m7b5");

    // Written export: the clarinet (first part) carries them a tone up.
    {
        const std::string xml = s.export_musicxml(dated());
        keep("export_chord_symbols.musicxml", xml);
        auto r = ix::xml::parse(xml);
        REQUIRE(r.ok);
        const Element* p1 = part_by_id(r.root, "P1");
        REQUIRE(p1);
        std::vector<std::string> roots;
        for (const auto* m : p1->children_named("measure"))
            for (const auto* h : m->children_named("harmony")) {
                std::string root = h->child("root")->child_text("root-step") + h->child("root")->child_text("root-alter");
                roots.push_back(root + ":" + h->child_text("kind"));
            }
        REQUIRE(roots.size() == 4);
        CHECK(roots[0] == "D:major");
        CHECK(roots[1] == "A:dominant");
        CHECK(roots[2] == "G1:half-diminished");
        CHECK(roots[3] == "C:major-ninth");
        // The bass of G7/B, written: C#.
        const Element* m2 = p1->children_named("measure")[1];
        const Element* h2 = m2->children_named("harmony")[0];
        REQUIRE(h2->child("bass"));
        CHECK(h2->child("bass")->child_text("bass-step") == "C");
        CHECK(h2->child("bass")->child_text("bass-alter") == "1");
        // P2 (the piano) has none.
        const Element* p2 = part_by_id(r.root, "P2");
        REQUIRE(p2);
        for (const auto* m : p2->children_named("measure")) CHECK(m->children_named("harmony").empty());

        // Read back: at concert pitch, at their ticks.
        auto imp = ix::import_musicxml(xml);
        REQUIRE(imp.ok);
        REQUIRE(imp.chords.size() == 4);
        CHECK(imp.chords[0].text == "C");
        CHECK(imp.chords[1].tick == bar + 2 * Q);
        CHECK(imp.chords[1].text == "G7/B");
        CHECK(imp.chords[2].text == "F#m7b5");
        CHECK(imp.chords[3].text == "Bbmaj9");
        session::Session s2(catalogs());
        auto sum = s2.import_musicxml(xml, true);
        CHECK(sum.ok);
        REQUIRE(s2.data().chords.size() == 4);
        CHECK(s2.data().chords[3].tick == 3 * bar);
    }
    // Concert export: as written in the project.
    {
        auto opts = dated();
        opts.concert_pitch = true;
        auto r = ix::xml::parse(s.export_musicxml(opts));
        REQUIRE(r.ok);
        const Element* h = part_by_id(r.root, "P1")->children_named("measure")[0]->children_named("harmony")[0];
        CHECK(h->child("root")->child_text("root-step") == "C");
    }
    // A <harmony> from another program: kind and degrees, no text.
    {
        const std::string other = R"(<?xml version="1.0"?>
<score-partwise version="4.0"><part-list><score-part id="P1"><part-name>Lead</part-name></score-part></part-list>
<part id="P1"><measure number="1"><attributes><divisions>1</divisions><time><beats>4</beats><beat-type>4</beat-type></time></attributes>
<harmony><root><root-step>E</root-step><root-alter>-1</root-alter></root><kind>dominant</kind>
<degree><degree-value>9</degree-value><degree-alter>-1</degree-alter><degree-type>add</degree-type></degree></harmony>
<note><pitch><step>E</step><octave>4</octave></pitch><duration>2</duration><type>half</type></note>
<harmony><root><root-step>A</root-step></root><kind>minor-seventh</kind><bass><bass-step>G</bass-step></bass></harmony>
<note><pitch><step>E</step><octave>4</octave></pitch><duration>2</duration><type>half</type></note>
</measure></part></score-partwise>)";
        auto imp = ix::import_musicxml(other);
        REQUIRE(imp.ok);
        REQUIRE(imp.chords.size() == 2);
        CHECK(imp.chords[0].text == "Eb7b9");
        CHECK(imp.chords[1].tick == 2 * Q);
        CHECK(imp.chords[1].text == "Am7/G");
    }
    // The new forms survive export and import: altered, added, omitted, power.
    {
        session::Session s3(catalogs());
        s3.new_empty();
        s3.add_track("Piano", "keys.piano");
        const std::vector<std::string> forms = {"C7(b5,b9)", "Eb7(#9,b13)", "D(add9)", "G5", "A7(no3)"};
        for (std::size_t i = 0; i < forms.size(); ++i) REQUIRE(s3.set_chord_symbol(static_cast<Ticks>(i) * bar, forms[i]));
        auto imp = ix::import_musicxml(s3.export_musicxml(dated()));
        REQUIRE(imp.ok);
        REQUIRE(imp.chords.size() == forms.size());
        for (std::size_t i = 0; i < forms.size(); ++i)
            CHECK_EQ(vcc::theory::parse_chord_symbol(imp.chords[i].text)->name(), forms[i]);
    }
}

TEST_CASE("instrument ranges: policies, issues at sounding pitch, moving into range, saved")
{
    using RP = session::Session::RangePolicy;
    session::Session s(catalogs());
    s.new_empty();
    const std::string fl = s.add_track("Flute", "woodwinds.flute");
    const std::string cl = s.add_track("Clarinet in Bb", "woodwinds.clarinet_bb");
    const std::string pad = s.add_track("Pad", "synth.pad_warm");
    const std::string timp = s.add_track("Timpani", "percussion.timpani");
    const std::string snare = s.add_track("Snare", "percussion.snare_drum");
    auto track = [&](const std::string& id) { return s.data().find_track(id); };

    // Defaults: Strict for acoustic and pitched percussion, Off for synths and unpitched.
    auto lf = s.range_limits(*track(fl));
    CHECK(lf.policy == RP::Strict && lf.policy_is_default);
    CHECK_EQ(lf.low, 59);
    CHECK_EQ(lf.high, 96);
    CHECK_EQ(lf.comfortable_low, 62);
    CHECK(lf.has_extended);
    CHECK(s.range_limits(*track(timp)).policy == RP::Strict);
    CHECK(s.range_limits(*track(pad)).policy == RP::Off);
    CHECK(s.range_limits(*track(snare)).policy == RP::Off);

    // Flute: A3 (57) below, C4 fine, D7 (98) above.
    nt::Score fs = track(fl)->active_version()->score;
    fs.insert_note(0, note(0, nt::quarter(), P(nt::Step::A, 3)));
    fs.insert_note(0, note(Q, nt::quarter(), P(nt::Step::C, 4)));
    fs.insert_note(0, note(2 * Q, nt::quarter(), P(nt::Step::D, 7)));
    REQUIRE(s.commit_score(fl, fs, "notes"));
    auto issues = s.range_issues(*track(fl));
    REQUIRE(issues.size() == 2u);
    CHECK(!issues[0].above);
    CHECK_EQ(issues[0].sounding, 57);
    CHECK(issues[1].above);
    CHECK_EQ(issues[1].sounding, 98);
    // Extended takes the flute to D7; Off checks nothing; the policy is one undo step and saved.
    CHECK(s.set_range_policy(fl, "extended"));
    CHECK_EQ(s.undo_label(), std::string("Range policy"));
    CHECK_EQ(s.range_issues(*track(fl)).size(), 1u);
    CHECK(s.set_range_policy(fl, "off"));
    CHECK(s.range_issues(*track(fl)).empty());
    CHECK(!s.set_range_policy(fl, "loose"));
    s.undo();
    s.undo();
    CHECK(s.range_limits(*track(fl)).policy == RP::Strict);
    // Move into range: by octaves, one undo step.
    CHECK_EQ(s.move_into_range(fl), 2);
    CHECK_EQ(s.undo_label(), std::string("Move into range"));
    CHECK(s.range_issues(*track(fl)).empty());
    {
        std::vector<int> midi;
        for (const auto& n : track(fl)->active_version()->score.notes(0)) midi.push_back(nt::midi_note_number(n.pitch));
        std::sort(midi.begin(), midi.end());
        CHECK(midi == std::vector<int>({60, 69, 86}));  // C4, A4, D6
    }
    s.undo();
    CHECK_EQ(s.range_issues(*track(fl)).size(), 2u);

    // Clarinet: written D3 sounds C3 (48), under its low D (50).
    nt::Score cs = track(cl)->active_version()->score;
    cs.insert_note(0, note(0, nt::quarter(), P(nt::Step::D, 3)));
    cs.insert_note(0, note(Q, nt::quarter(), P(nt::Step::E, 3)));  // sounds D3: the lowest note
    REQUIRE(s.commit_score(cl, cs, "notes"));
    issues = s.range_issues(*track(cl));
    REQUIRE(issues.size() == 1u);
    CHECK_EQ(issues[0].sounding, 48);

    // Saved and reopened.
    REQUIRE(s.set_range_policy(cl, "warn"));
    const std::string path = std::string(VCC_TEST_TMP_DIR) + "/ranges.vcc2";
    REQUIRE(s.save_as(path));
    session::Session again(catalogs());
    REQUIRE(again.open(path));
    CHECK(again.range_limits(*again.data().find_track(cl)).policy == RP::Warn);
    CHECK(again.range_limits(*again.data().find_track(fl)).policy_is_default);
}

TEST_CASE("track Trans: one track, heard once, reset, bypass, saved, applied to notes")
{
    session::Session s(catalogs());
    s.new_empty();
    const std::string a = s.add_track("Piano A", "keys.piano");
    const std::string b = s.add_track("Piano B", "keys.piano");
    const std::string cl = s.add_track("Clarinet in Bb", "woodwinds.clarinet_bb");
    const std::string audio = s.add_audio_track("Vox", false);
    for (const auto& id : {a, b}) {
        nt::Score sc = s.data().find_track(id)->active_version()->score;
        sc.insert_note(0, note(0, nt::quarter(), P(nt::Step::C, 4)));
        REQUIRE(s.commit_score(id, sc, "C4"));
    }
    {
        nt::Score sc = s.data().find_track(cl)->active_version()->score;
        sc.insert_note(0, note(0, nt::quarter(), P(nt::Step::D, 4)));  // written D4 sounds C4
        REQUIRE(s.commit_score(cl, sc, "D4"));
    }
    auto heard = [&](const std::string& id) {
        const auto r = project::render_track(*s.data().find_track(id));
        return r.notes.empty() ? -1 : r.notes.front().pitch;
    };
    CHECK_EQ(heard(a), 60);
    CHECK_EQ(heard(cl), 60);
    // What playback renders through (the cache) follows the shift too.
    project::RenderCache cache;
    CHECK_EQ(cache.render(*s.data().find_track(a)).notes.front().pitch, 60);

    // +2 on A: A plays D4, B stays C4; two presses are one undo step.
    CHECK(s.set_track_transpose(a, 1));
    CHECK(s.set_track_transpose(a, 2));
    CHECK_EQ(heard(a), 62);
    CHECK_EQ(heard(b), 60);
    CHECK_EQ(cache.render(*s.data().find_track(a)).notes.front().pitch, 62);
    CHECK_EQ(s.undo_label(), std::string("Transpose Piano A"));
    s.undo();
    CHECK_EQ(heard(a), 60);
    s.redo();
    CHECK_EQ(heard(a), 62);
    // Bypass (A/B) and playback-only mode.
    CHECK(s.set_transpose_bypass(a, true));
    CHECK_EQ(heard(a), 60);
    CHECK(s.set_transpose_bypass(a, false));
    CHECK(s.set_transpose_mode(a, "playback"));
    CHECK_EQ(heard(a), 62);
    CHECK(!s.set_transpose_mode(a, "sideways"));
    CHECK(s.set_transpose_mode(a, "notation"));
    // The written notes never change by themselves.
    CHECK_EQ(nt::midi_note_number(s.data().find_track(a)->active_version()->score.notes(0).front().pitch), 60);

    // The clarinet: +2 sounds D4, transposed once (written D4 -> concert C4 -> +2).
    CHECK(s.set_track_transpose(cl, 2));
    CHECK_EQ(heard(cl), 62);
    // Octave down, and saved.
    CHECK(s.set_track_transpose(b, -12));
    CHECK_EQ(heard(b), 48);
    const std::string path = std::string(VCC_TEST_TMP_DIR) + "/trans.vcc2";
    REQUIRE(s.save_as(path));
    session::Session again(catalogs());
    REQUIRE(again.open(path));
    CHECK_EQ(again.data().find_track(a)->transpose, 2);
    CHECK_EQ(again.data().find_track(b)->transpose, -12);

    // Apply to notes: written once, the shift back to 0, one undo step.
    CHECK(s.commit_track_transpose(a));
    CHECK_EQ(s.undo_label(), std::string("Apply transpose to notes"));
    CHECK_EQ(s.data().find_track(a)->transpose, 0);
    const auto& written = s.data().find_track(a)->active_version()->score.notes(0).front().pitch;
    CHECK_EQ(nt::midi_note_number(written), 62);
    CHECK(written.step == nt::Step::D);
    CHECK_EQ(heard(a), 62);
    s.undo();
    CHECK_EQ(s.data().find_track(a)->transpose, 2);
    CHECK_EQ(heard(a), 62);
    CHECK_EQ(nt::midi_note_number(s.data().find_track(a)->active_version()->score.notes(0).front().pitch), 60);
    // The clarinet's written D4 becomes written E4.
    CHECK(s.commit_track_transpose(cl));
    const auto& cw = s.data().find_track(cl)->active_version()->score.notes(0).front().pitch;
    CHECK(cw.step == nt::Step::E);
    CHECK_EQ(cw.alteration, 0);
    CHECK_EQ(heard(cl), 62);

    // Ranges count the shift: a flute's C7 up a tone is above its top.
    const std::string fl = s.add_track("Flute", "woodwinds.flute");
    {
        nt::Score sc = s.data().find_track(fl)->active_version()->score;
        sc.insert_note(0, note(0, nt::quarter(), P(nt::Step::C, 7)));
        REQUIRE(s.commit_score(fl, sc, "C7"));
    }
    CHECK(s.range_issues(*s.data().find_track(fl)).empty());
    CHECK(s.set_track_transpose(fl, 2));
    CHECK_EQ(s.range_issues(*s.data().find_track(fl)).size(), 1u);

    // Not every track transposes; several at once when chosen.
    std::string why;
    CHECK(!s.can_transpose(*s.data().find_track(audio), &why));
    CHECK(!why.empty());
    CHECK(!s.set_track_transpose(audio, 2));
    const std::string snare = s.add_track("Snare", "percussion.snare_drum");
    CHECK(!s.can_transpose(*s.data().find_track(snare)));
    CHECK(s.set_tracks_transpose({a, b, audio}, 5));
    CHECK_EQ(s.data().find_track(a)->transpose, 5);
    CHECK_EQ(s.data().find_track(b)->transpose, 5);
    CHECK_EQ(s.undo_label(), std::string("Transpose tracks"));

    CHECK_EQ(session::Session::interval_name(2), std::string("major 2nd up"));
    CHECK_EQ(session::Session::interval_name(-12), std::string("octave down"));
    CHECK_EQ(session::Session::interval_name(7), std::string("perfect 5th up"));
    CHECK_EQ(session::Session::interval_name(-14), std::string("major 2nd + an octave down"));
}
