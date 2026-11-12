// The full score: parts aligned on one timeline, names, brackets, hidden
// empty staves, transposed and concert views, and a printed PDF.
#include <cmath>
#include <map>

#include "vcc/core/Files.h"
#include "vcc/engraving/PdfRenderer.h"
#include "vcc/engraving/ScoreLayout.h"
#include "vcc/interchange/Transposition.h"
#include "vcc_test.h"

using namespace vcc;
using namespace vcc::engraving;
using notation::kTicksPerQuarter;

namespace {

notation::Score part(notation::Clef clef, int transposition, int bars, int octave, bool rest_after_bar2 = false,
                     bool grand = false)
{
    notation::Score s;
    notation::Staff st = s.staff(0);
    st.clef = clef;
    st.transposition_semitones = static_cast<std::int8_t>(transposition);
    s.set_staff(0, st);
    if (grand) {
        notation::Staff lower;
        lower.clef = notation::Clef::Bass;
        s.add_staff(lower);
    }
    s.set_key_signature(0, notation::KeySignature{-1, false});  // concert F major
    for (int bar = 0; bar < bars; ++bar) {
        if (rest_after_bar2 && bar >= 2) break;
        for (int beat = 0; beat < 4; ++beat) {
            notation::ScoreNote n;
            n.start_tick = (bar * 4 + beat) * kTicksPerQuarter;
            n.duration = notation::quarter();
            n.pitch = {static_cast<notation::Step>((bar + beat) % 7), static_cast<std::int8_t>(octave), 0};
            s.insert_note(0, n);
            if (grand) {
                n.pitch.octave = static_cast<std::int8_t>(octave - 2);
                s.insert_note(1, n);
            }
        }
    }
    return s;
}

std::size_t count_text(const PrintResult& r, const std::string& text)
{
    std::size_t n = 0;
    for (const auto& p : r.pages)
        for (const auto& t : p.layout.display_list.texts) n += t.text == text;
    return n;
}

}  // namespace

TEST_CASE("transposed and concert views of a B-flat clarinet part")
{
    const auto cl = part(notation::Clef::Treble, -2, 1, 5);
    const auto written = interchange::display_score(cl, false);
    CHECK_EQ(static_cast<int>(written.key_signature_at(0).fifths), 1);  // concert F -> written G
    CHECK(written.notes(0).front().pitch == cl.notes(0).front().pitch);
    const auto concert = interchange::display_score(cl, true);
    CHECK_EQ(static_cast<int>(concert.key_signature_at(0).fifths), -1);
    CHECK_EQ(concert.staff(0).transposition_semitones, 0);
    // Written C5 sounds B-flat 4.
    const auto& c = concert.notes(0).front().pitch;
    CHECK(c.step == notation::Step::B && c.alteration == -1 && c.octave == 4);
    // A non-transposing part is untouched.
    const auto fl = part(notation::Clef::Treble, 0, 1, 5);
    CHECK(interchange::display_score(fl, true).notes(0).front().pitch == fl.notes(0).front().pitch);
}

TEST_CASE("a full score aligns every part, names them and prints")
{
    const auto fl = part(notation::Clef::Treble, 0, 12, 5);
    const auto cl = interchange::display_score(part(notation::Clef::Treble, -2, 12, 5, true), false);
    const auto pno = part(notation::Clef::Treble, 0, 12, 4, false, true);
    const auto vln = part(notation::Clef::Treble, 0, 12, 4);
    const auto vc = part(notation::Clef::Bass, 0, 12, 3);
    std::vector<ScorePart> parts = {{&fl, "Flute", "Fl.", "woodwinds"},
                                    {&cl, "Clarinet in B\xE2\x99\xAD", "Cl.", "woodwinds"},
                                    {&pno, "Piano", "Pno.", ""},
                                    {&vln, "Violins I", "Vln. I", "strings"},
                                    {&vc, "Violoncellos", "Vc.", "strings"}};
    LayoutEngine engine;
    ScoreOptions opts;
    opts.title = "Test Suite";
    opts.subtitle = "Transposed score";
    opts.composer = "A. Composer";
    page_size_for(297, 420, 1.25, opts);
    CHECK(std::abs(opts.page_width - 237.6) < 0.01);
    const auto r = lay_out_score(parts, engine, opts);
    REQUIRE(!r.pages.empty());
    CHECK(r.system_count >= 3u);
    CHECK_EQ(r.bar_count, 12);

    // Full names once, abbreviations on later systems.
    CHECK_EQ(count_text(r, "Flute"), 1u);
    CHECK(count_text(r, "Fl.") >= 2u);
    CHECK_EQ(count_text(r, "Test Suite"), 1u);
    // The clarinet rests from bar 3, so it is hidden on later systems.
    CHECK(count_text(r, "Cl.") < count_text(r, "Fl."));

    // Every part's beat lines up: notes at the same tick share an x.
    std::map<notation::Ticks, std::vector<double>> xs;
    for (const auto& p : r.pages)
        for (const auto& n : p.layout.notes)
            if (!n.is_rest && n.start_tick < 4 * kTicksPerQuarter) xs[n.start_tick].push_back(n.centre.x);
    REQUIRE(xs.size() == 4u);
    for (const auto& [tick, list] : xs) {
        CHECK(list.size() >= 5u);
        const auto [lo, hi] = std::minmax_element(list.begin(), list.end());
        CHECK(*hi - *lo < 0.6);  // seconds shift a head by a notehead; nothing more
    }

    // Brackets (quads) and a brace (curves) are drawn on every system.
    std::size_t quads = 0;
    for (const auto& p : r.pages) quads += p.layout.display_list.quads.size();
    CHECK(quads >= 2 * r.system_count);

    // Without hiding, every system has every part.
    ScoreOptions all = opts;
    all.hide_empty_staves = false;
    const auto full = lay_out_score(parts, engine, all);
    CHECK_EQ(count_text(full, "Cl."), count_text(full, "Fl."));

    PdfOptions po;
    po.points_per_staff_space = 72.0 / 25.4 * 1.25;
    po.music_font = *files::read_file(std::string(VCC_CONTENT_DIR) + "/../resources/fonts/Bravura/Bravura.otf");
    std::string error;
    const std::string pdf = render_pages_pdf(r, po, nullptr, &error);
    REQUIRE(!pdf.empty());
    CHECK(pdf.find("/MediaBox [0 0 841.") != std::string::npos);  // A3 width in points
    files::write_file_atomic(std::string(VCC_TEST_TMP_DIR) + "/full-score.pdf", pdf);

    CHECK(lay_out_score({}, engine, opts).pages.empty());
}
