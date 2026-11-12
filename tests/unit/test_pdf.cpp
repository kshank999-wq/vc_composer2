// Printed parts as PDF: the deflate encoder, the OpenType reader, and a part
// written through the real layout engine.
#include <cstdint>
#include <string>

#include "vcc/core/Files.h"
#include "vcc/engraving/PdfRenderer.h"
#include "vcc/engraving/PrintLayout.h"
#include "vcc/notation/Score.h"
#include "vcc_test.h"

using namespace vcc;
using namespace vcc::engraving;

namespace {

std::string font_bytes()
{
    auto f = files::read_file(std::string(VCC_CONTENT_DIR) + "/../resources/fonts/Bravura/Bravura.otf");
    return f ? *f : std::string{};
}

/// A minimal inflate for fixed-Huffman blocks (all the encoder writes).
struct BitReader {
    const std::string& s;
    std::size_t pos = 0;
    int bit = 0;
    int read_bit()
    {
        if (pos >= s.size()) throw 1;
        const int b = (static_cast<unsigned char>(s[pos]) >> bit) & 1;
        if (++bit == 8) { bit = 0; ++pos; }
        return b;
    }
    int bits(int n) { int v = 0; for (int i = 0; i < n; ++i) v |= read_bit() << i; return v; }
};

int fixed_symbol(BitReader& r)
{
    int code = 0;
    for (int len = 1; len <= 9; ++len) {
        code = (code << 1) | r.read_bit();
        if (len == 7 && code <= 0x17) return code + 256;
        if (len == 8 && code >= 0x30 && code <= 0xBF) return code - 0x30;
        if (len == 8 && code >= 0xC0 && code <= 0xC7) return code - 0xC0 + 280;
        if (len == 9 && code >= 0x190) return code - 0x190 + 144;
    }
    throw 2;
}

std::string inflate_fixed(const std::string& z)
{
    static const int lbase[] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
    static const int lextra[] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
    static const int dbase[] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
    static const int dextra[] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};
    const std::string body = z.substr(2);
    BitReader r{body};
    std::string out;
    if (r.bits(1) != 1 || r.bits(2) != 1) throw 3;
    for (;;) {
        const int sym = fixed_symbol(r);
        if (sym < 256) { out += static_cast<char>(sym); continue; }
        if (sym == 256) break;
        const int li = sym - 257;
        const int len = lbase[li] + r.bits(lextra[li]);
        int dcode = 0;
        for (int i = 0; i < 5; ++i) dcode = (dcode << 1) | r.read_bit();
        const int dist = dbase[dcode] + r.bits(dextra[dcode]);
        for (int i = 0; i < len; ++i) out += out[out.size() - static_cast<std::size_t>(dist)];
    }
    return out;
}

notation::Score sample_score(int bars)
{
    notation::Score s;
    for (int bar = 0; bar < bars; ++bar)
        for (int beat = 0; beat < 4; ++beat) {
            notation::ScoreNote n;
            n.start_tick = (bar * 4 + beat) * notation::kTicksPerQuarter;
            n.duration = notation::quarter();
            n.pitch = {static_cast<notation::Step>((bar + beat) % 7), 4, static_cast<std::int8_t>(beat == 2 ? 1 : 0)};
            s.insert_note(0, n);
        }
    return s;
}

std::size_t count(const std::string& hay, const std::string& needle)
{
    std::size_t n = 0;
    for (auto p = hay.find(needle); p != std::string::npos; p = hay.find(needle, p + 1)) ++n;
    return n;
}

}  // namespace

TEST_CASE("deflate output inflates back to the input")
{
    for (const std::string& input :
         {std::string(), std::string("a"), std::string("abcabcabcabcabcabcabc"), std::string(70000, 'x'),
          [] { std::string s; for (int i = 0; i < 50000; ++i) s += static_cast<char>((i * 7919) % 251); return s; }()}) {
        const std::string z = zlib_compress(input);
        CHECK_EQ(static_cast<unsigned char>(z[0]), 0x78u);
        CHECK_EQ(inflate_fixed(z), input);
    }
    CHECK(zlib_compress(std::string(70000, 'x')).size() < 2000u);
}

TEST_CASE("the OpenType reader finds Bravura's glyphs")
{
    const std::string bytes = font_bytes();
    REQUIRE(!bytes.empty());
    OpenTypeFont font;
    std::string error;
    REQUIRE(font.load(bytes, &error));
    CHECK(font.glyph_id(0xE050) != 0);  // gClef
    CHECK(font.glyph_id(0xE0A4) != 0);  // noteheadBlack
    CHECK_EQ(font.glyph_id(0x10FFFF), 0);
    CHECK(font.advance(font.glyph_id(0xE0A4)) > 0);
    CHECK(font.units_per_em() > 0);
    CHECK(font.cff().size() > 100000u);
    CHECK_EQ(font.postscript_name(), std::string("Bravura"));
    CHECK(!OpenTypeFont().load("not a font", &error));
    CHECK(!error.empty());
}

TEST_CASE("a part prints to a valid PDF with the music font embedded")
{
    LayoutEngine engine;
    PrintOptions po;
    po.title = "Suite — Overture";
    po.subtitle = "Violin I";
    const auto laid = lay_out_pages(sample_score(40), engine, po);
    REQUIRE(laid.pages.size() >= 2u);

    PdfOptions opts;
    opts.music_font = font_bytes();
    opts.title = "Suite";
    opts.compress = false;
    std::string error;
    const std::string pdf = render_pdf({{&laid, "Violin I"}, {&laid, "Violin II"}}, opts, nullptr, &error);
    REQUIRE(!pdf.empty());
    CHECK(pdf.rfind("%PDF-1.4", 0) == 0);
    CHECK(pdf.find("%%EOF") != std::string::npos);
    CHECK_EQ(count(pdf, "/Type /Page "), 2 * laid.pages.size());
    CHECK(pdf.find("/FontFile3") != std::string::npos);
    CHECK(pdf.find("/CIDFontType0C") != std::string::npos);
    CHECK(pdf.find("/Title (Violin I)") != std::string::npos);
    CHECK(pdf.find("(Suite \\227 Overture)") != std::string::npos);  // em dash in WinAnsi
    CHECK(pdf.find("> Tj") != std::string::npos);  // music glyphs by id
    // A4 at the default rastral.
    CHECK(pdf.find("/MediaBox [0 0 595.") != std::string::npos);

    // The cross-reference table points at every object.
    const auto xref = pdf.rfind("\nxref\n") + 1;
    REQUIRE(xref > 1);
    const auto startxref = std::stoul(pdf.substr(pdf.rfind("startxref\n") + 10));
    CHECK_EQ(startxref, xref);
    std::size_t line = pdf.find('\n', pdf.find('\n', xref + 5) + 1) + 1;  // skip "0 N" and the free entry
    int checked = 0;
    while (pdf.compare(line, 7, "trailer") != 0) {
        const auto off = std::stoul(pdf.substr(line, 10));
        CHECK(pdf.compare(off, std::to_string(checked + 1).size() + 6, std::to_string(checked + 1) + " 0 obj") == 0);
        ++checked;
        line += 20;
    }
    CHECK(checked > 10);

    // Compressed is much smaller, and without a font it still prints.
    opts.compress = true;
    const std::string small = render_pages_pdf(laid, opts);
    CHECK(small.size() * 10 < pdf.size() * 6);  // the font compresses least
    files::write_file_atomic(std::string(VCC_TEST_TMP_DIR) + "/part.pdf", small);
    files::write_file_atomic(std::string(VCC_TEST_TMP_DIR) + "/parts-uncompressed.pdf", pdf);
    PdfOptions nofont;
    const std::string plain = render_pages_pdf(laid, nofont);
    CHECK(!plain.empty());
    CHECK(plain.find("/FontFile3") == std::string::npos);

    PrintResult empty;
    CHECK(render_pages_pdf(empty, opts, nullptr, &error).empty());
    CHECK(!error.empty());
}
