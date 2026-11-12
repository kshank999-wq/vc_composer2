#include "vcc/engraving/PdfRenderer.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <sstream>

#include "vcc/engraving/GlyphSubstitute.h"

namespace vcc::engraving {

// --- OpenType ------------------------------------------------------------------------

namespace {

std::uint32_t be16(const std::string& b, std::size_t at)
{
    if (at + 2 > b.size()) return 0;
    return (static_cast<std::uint32_t>(static_cast<std::uint8_t>(b[at])) << 8) |
           static_cast<std::uint32_t>(static_cast<std::uint8_t>(b[at + 1]));
}

std::uint32_t be32(const std::string& b, std::size_t at) { return (be16(b, at) << 16) | be16(b, at + 2); }

}  // namespace

bool OpenTypeFont::load(const std::string& bytes, std::string* error)
{
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        cff_.clear();
        return false;
    };
    cff_.clear();
    advances_.clear();
    cmap_.clear();
    if (bytes.size() < 12 || bytes.compare(0, 4, "OTTO") != 0)
        return fail("not a CFF-flavoured OpenType font (OTTO)");
    const std::size_t tables = be16(bytes, 4);
    std::map<std::string, std::pair<std::size_t, std::size_t>> dir;
    for (std::size_t i = 0; i < tables; ++i) {
        const std::size_t rec = 12 + 16 * i;
        if (rec + 16 > bytes.size()) return fail("truncated table directory");
        const std::size_t off = be32(bytes, rec + 8), len = be32(bytes, rec + 12);
        if (off + len > bytes.size()) return fail("table outside the file");
        dir[bytes.substr(rec, 4)] = {off, len};
    }
    for (const char* need : {"CFF ", "cmap", "head", "hhea", "hmtx"})
        if (!dir.count(need)) return fail(std::string("no '") + need + "' table");

    units_per_em_ = static_cast<std::uint16_t>(be16(bytes, dir["head"].first + 18));
    if (units_per_em_ == 0) units_per_em_ = 1000;

    // Advances: hhea.numberOfHMetrics long metrics; the rest repeat the last.
    const std::size_t metrics = be16(bytes, dir["hhea"].first + 34);
    std::size_t glyphs = metrics;
    if (dir.count("maxp")) glyphs = std::max<std::size_t>(glyphs, be16(bytes, dir["maxp"].first + 4));
    const std::size_t hmtx = dir["hmtx"].first;
    for (std::size_t g = 0; g < glyphs; ++g) {
        const std::size_t i = std::min(g, metrics == 0 ? 0 : metrics - 1);
        advances_.push_back(static_cast<std::uint16_t>(be16(bytes, hmtx + 4 * i)));
    }

    // cmap: prefer (3,10) format 12, then (3,1)/(0,x) format 4.
    const std::size_t cmap = dir["cmap"].first;
    const std::size_t subtables = be16(bytes, cmap + 2);
    std::size_t best = 0;
    int best_rank = -1;
    for (std::size_t i = 0; i < subtables; ++i) {
        const std::size_t rec = cmap + 4 + 8 * i;
        const auto platform = be16(bytes, rec), encoding = be16(bytes, rec + 2);
        const std::size_t off = cmap + be32(bytes, rec + 4);
        const auto format = be16(bytes, off);
        int rank = -1;
        if (format == 12 && (platform == 3 || platform == 0)) rank = 2;
        else if (format == 4 && ((platform == 3 && encoding == 1) || platform == 0)) rank = 1;
        if (rank > best_rank) {
            best_rank = rank;
            best = off;
        }
    }
    if (best_rank < 0) return fail("no Unicode cmap");
    if (be16(bytes, best) == 12) {
        const std::size_t groups = be32(bytes, best + 12);
        for (std::size_t g = 0; g < groups; ++g) {
            const std::size_t rec = best + 16 + 12 * g;
            const char32_t first = be32(bytes, rec), last = be32(bytes, rec + 4);
            const std::uint32_t gid = be32(bytes, rec + 8);
            for (char32_t c = first; c <= last && c - first < 0x110000; ++c)
                cmap_.emplace_back(c, static_cast<std::uint16_t>(gid + (c - first)));
        }
    } else {
        const std::size_t segs = be16(bytes, best + 6) / 2;
        const std::size_t ends = best + 14, starts = ends + 2 * segs + 2, deltas = starts + 2 * segs,
                          ranges = deltas + 2 * segs;
        for (std::size_t s = 0; s < segs; ++s) {
            const std::uint32_t end = be16(bytes, ends + 2 * s), start = be16(bytes, starts + 2 * s);
            const std::uint32_t delta = be16(bytes, deltas + 2 * s), range = be16(bytes, ranges + 2 * s);
            if (start == 0xFFFF) continue;
            for (std::uint32_t c = start; c <= end; ++c) {
                std::uint32_t gid;
                if (range == 0) {
                    gid = (c + delta) & 0xFFFF;
                } else {
                    const std::size_t at = ranges + 2 * s + range + 2 * (c - start);
                    gid = be16(bytes, at);
                    if (gid != 0) gid = (gid + delta) & 0xFFFF;
                }
                if (gid != 0) cmap_.emplace_back(static_cast<char32_t>(c), static_cast<std::uint16_t>(gid));
            }
        }
    }
    std::sort(cmap_.begin(), cmap_.end());

    // PostScript name (name id 6), for the font dictionary.
    if (dir.count("name")) {
        const std::size_t name = dir["name"].first;
        const std::size_t count = be16(bytes, name + 2), strings = name + be16(bytes, name + 4);
        for (std::size_t i = 0; i < count; ++i) {
            const std::size_t rec = name + 6 + 12 * i;
            if (be16(bytes, rec + 6) != 6) continue;
            const auto platform = be16(bytes, rec);
            const std::size_t len = be16(bytes, rec + 8), off = strings + be16(bytes, rec + 10);
            std::string s;
            for (std::size_t k = 0; k < len && off + k < bytes.size(); ++k) {
                const char ch = bytes[off + k];
                if (platform == 3 || platform == 0) {
                    if (k % 2 == 1) s += ch;  // UTF-16BE, ASCII range
                } else {
                    s += ch;
                }
            }
            std::string clean;
            for (char ch : s)
                if (std::isalnum(static_cast<unsigned char>(ch)) || ch == '-' || ch == '_') clean += ch;
            if (!clean.empty()) {
                name_ = clean;
                break;
            }
        }
    }
    cff_ = bytes.substr(dir["CFF "].first, dir["CFF "].second);
    return true;
}

std::uint16_t OpenTypeFont::glyph_id(char32_t codepoint) const noexcept
{
    auto it = std::lower_bound(cmap_.begin(), cmap_.end(), std::make_pair(codepoint, std::uint16_t{0}));
    return it != cmap_.end() && it->first == codepoint ? it->second : 0;
}

std::uint16_t OpenTypeFont::advance(std::uint16_t glyph_id) const noexcept
{
    return glyph_id < advances_.size() ? advances_[glyph_id] : 0;
}

// --- Deflate -----------------------------------------------------------------------------

namespace {

class BitWriter {
public:
    explicit BitWriter(std::string& out) : out_(out) {}
    void bits(std::uint32_t value, int count)
    {
        for (int i = 0; i < count; ++i) {
            acc_ |= ((value >> i) & 1u) << n_;
            if (++n_ == 8) flush_byte();
        }
    }
    /// Huffman codes go most-significant bit first.
    void code(std::uint32_t code, int length)
    {
        for (int i = length - 1; i >= 0; --i) {
            acc_ |= ((code >> i) & 1u) << n_;
            if (++n_ == 8) flush_byte();
        }
    }
    void align()
    {
        if (n_ > 0) flush_byte();
    }

private:
    void flush_byte()
    {
        out_ += static_cast<char>(acc_);
        acc_ = 0;
        n_ = 0;
    }
    std::string& out_;
    std::uint32_t acc_ = 0;
    int n_ = 0;
};

void fixed_literal(BitWriter& w, int symbol)
{
    if (symbol <= 143) w.code(static_cast<std::uint32_t>(0x30 + symbol), 8);
    else if (symbol <= 255) w.code(static_cast<std::uint32_t>(0x190 + symbol - 144), 9);
    else if (symbol <= 279) w.code(static_cast<std::uint32_t>(symbol - 256), 7);
    else w.code(static_cast<std::uint32_t>(0xC0 + symbol - 280), 8);
}

constexpr std::array<int, 29> kLengthBase = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                             31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr std::array<int, 29> kLengthExtra = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                              2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr std::array<int, 30> kDistBase = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,
                                           33,  49,  65,  97,  129, 193,  257,  385,  513,  769,
                                           1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr std::array<int, 30> kDistExtra = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6,
                                            6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

void emit_match(BitWriter& w, int length, int distance)
{
    int li = 28;
    while (kLengthBase[static_cast<std::size_t>(li)] > length) --li;
    fixed_literal(w, 257 + li);
    w.bits(static_cast<std::uint32_t>(length - kLengthBase[static_cast<std::size_t>(li)]),
           kLengthExtra[static_cast<std::size_t>(li)]);
    int di = 29;
    while (kDistBase[static_cast<std::size_t>(di)] > distance) --di;
    w.code(static_cast<std::uint32_t>(di), 5);
    w.bits(static_cast<std::uint32_t>(distance - kDistBase[static_cast<std::size_t>(di)]),
           kDistExtra[static_cast<std::size_t>(di)]);
}

}  // namespace

std::string zlib_compress(const std::string& data)
{
    std::string out;
    out += static_cast<char>(0x78);
    out += static_cast<char>(0x01);
    BitWriter w(out);
    w.bits(1, 1);  // final block
    w.bits(1, 2);  // fixed Huffman
    constexpr std::size_t kWindow = 32768, kHashSize = 1u << 15, kMaxChain = 48;
    std::vector<int> head(kHashSize, -1);
    std::vector<int> prev(data.size(), -1);
    auto byte = [&](std::size_t i) { return static_cast<std::uint8_t>(data[i]); };
    auto hash = [&](std::size_t i) {
        return ((static_cast<std::size_t>(byte(i)) << 10) ^ (static_cast<std::size_t>(byte(i + 1)) << 5) ^
                byte(i + 2)) & (kHashSize - 1);
    };
    auto insert = [&](std::size_t i) {
        if (i + 2 >= data.size()) return;
        const std::size_t h = hash(i);
        prev[i] = head[h];
        head[h] = static_cast<int>(i);
    };
    std::size_t i = 0;
    while (i < data.size()) {
        int best_len = 0, best_dist = 0;
        if (i + 2 < data.size()) {
            int candidate = head[hash(i)];
            std::size_t chain = 0;
            const std::size_t max_len = std::min<std::size_t>(258, data.size() - i);
            while (candidate >= 0 && chain++ < kMaxChain) {
                const std::size_t c = static_cast<std::size_t>(candidate);
                if (i - c > kWindow - 1) break;
                std::size_t len = 0;
                while (len < max_len && data[c + len] == data[i + len]) ++len;
                if (static_cast<int>(len) > best_len) {
                    best_len = static_cast<int>(len);
                    best_dist = static_cast<int>(i - c);
                    if (len == max_len) break;
                }
                candidate = prev[c];
            }
        }
        if (best_len >= 3) {
            emit_match(w, best_len, best_dist);
            for (int k = 0; k < best_len; ++k) insert(i + static_cast<std::size_t>(k));
            i += static_cast<std::size_t>(best_len);
        } else {
            fixed_literal(w, byte(i));
            insert(i);
            ++i;
        }
    }
    fixed_literal(w, 256);
    w.align();
    // Adler-32, big-endian.
    std::uint32_t a = 1, b = 0;
    for (char c : data) {
        a = (a + static_cast<std::uint8_t>(c)) % 65521u;
        b = (b + a) % 65521u;
    }
    const std::uint32_t adler = (b << 16) | a;
    for (int s = 24; s >= 0; s -= 8) out += static_cast<char>((adler >> s) & 0xFF);
    return out;
}

// --- PDF ---------------------------------------------------------------------------------

namespace {

constexpr double kStaffSpacesPerEm = 4.0;

struct Rgb {
    double r, g, b;
};

Rgb colour_for(ItemRole role) noexcept
{
    switch (role) {
        case ItemRole::StaffLine:
        case ItemRole::LedgerLine:
        case ItemRole::Barline: return {0.227, 0.227, 0.227};
        case ItemRole::RulerMark:
        case ItemRole::RulerLabel: return {0.54, 0.54, 0.54};
        case ItemRole::Cursor: return {0.784, 0.25, 0.25};
        default: break;
    }
    return {0.063, 0.063, 0.063};
}

/// Numbers with up to three decimals, no exponent, no trailing zeros.
std::string num(double v)
{
    if (!std::isfinite(v)) v = 0.0;
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.3f", v);
    std::string s = buf;
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    if (s == "-0" || s.empty()) s = "0";
    return s;
}

/// A PDF literal string from WinAnsi bytes.
std::string literal(const std::string& bytes)
{
    std::string out = "(";
    for (char ch : bytes) {
        const auto c = static_cast<unsigned char>(ch);
        if (ch == '(' || ch == ')' || ch == '\\') {
            out += '\\';
            out += ch;
        } else if (c < 32 || c > 126) {
            char buf[8];
            std::snprintf(buf, sizeof buf, "\\%03o", c);
            out += buf;
        } else {
            out += ch;
        }
    }
    return out + ")";
}

/// UTF-8 to WinAnsi (cp1252); unmappable characters become '?'.
std::string to_winansi(const std::string& utf8)
{
    static const std::map<char32_t, unsigned char> specials = {
        {0x20AC, 0x80}, {0x201A, 0x82}, {0x0192, 0x83}, {0x201E, 0x84}, {0x2026, 0x85}, {0x2020, 0x86},
        {0x2021, 0x87}, {0x02C6, 0x88}, {0x2030, 0x89}, {0x0160, 0x8A}, {0x2039, 0x8B}, {0x0152, 0x8C},
        {0x017D, 0x8E}, {0x2018, 0x91}, {0x2019, 0x92}, {0x201C, 0x93}, {0x201D, 0x94}, {0x2022, 0x95},
        {0x2013, 0x96}, {0x2014, 0x97}, {0x02DC, 0x98}, {0x2122, 0x99}, {0x0161, 0x9A}, {0x203A, 0x9B},
        {0x0153, 0x9C}, {0x017E, 0x9E}, {0x0178, 0x9F}, {0x266D, 'b'},  {0x266F, '#'}, {0x266E, 'n'}};
    std::string out;
    for (std::size_t i = 0; i < utf8.size();) {
        const auto c0 = static_cast<unsigned char>(utf8[i]);
        char32_t cp = 0;
        int extra = 0;
        if (c0 < 0x80) cp = c0;
        else if ((c0 & 0xE0) == 0xC0) { cp = c0 & 0x1F; extra = 1; }
        else if ((c0 & 0xF0) == 0xE0) { cp = c0 & 0x0F; extra = 2; }
        else if ((c0 & 0xF8) == 0xF0) { cp = c0 & 0x07; extra = 3; }
        else { ++i; out += '?'; continue; }
        ++i;
        for (int k = 0; k < extra && i < utf8.size(); ++k, ++i)
            cp = (cp << 6) | (static_cast<unsigned char>(utf8[i]) & 0x3F);
        if (cp < 0x80 || (cp >= 0xA0 && cp <= 0xFF)) out += static_cast<char>(cp);
        else if (auto it = specials.find(cp); it != specials.end()) out += static_cast<char>(it->second);
        else out += '?';
    }
    return out;
}

class PdfWriter {
public:
    int reserve() { offsets_.push_back(0); return static_cast<int>(offsets_.size()); }
    void object(int id, const std::string& body)
    {
        offsets_[static_cast<std::size_t>(id - 1)] = out_.size();
        out_ += std::to_string(id) + " 0 obj\n" + body + "\nendobj\n";
    }
    void stream(int id, const std::string& dict_extra, const std::string& data, bool compress)
    {
        const std::string payload = compress ? zlib_compress(data) : data;
        std::string dict = "<< /Length " + std::to_string(payload.size());
        if (compress) dict += " /Filter /FlateDecode";
        dict += dict_extra + " >>\nstream\n";
        offsets_[static_cast<std::size_t>(id - 1)] = out_.size();
        out_ += std::to_string(id) + " 0 obj\n" + dict;
        out_ += payload;
        out_ += "\nendstream\nendobj\n";
    }
    std::string finish(int root, int info)
    {
        const std::size_t xref = out_.size();
        out_ += "xref\n0 " + std::to_string(offsets_.size() + 1) + "\n0000000000 65535 f \n";
        for (std::size_t off : offsets_) {
            char buf[32];
            std::snprintf(buf, sizeof buf, "%010zu 00000 n \n", off);
            out_ += buf;
        }
        out_ += "trailer\n<< /Size " + std::to_string(offsets_.size() + 1) + " /Root " + std::to_string(root) +
                " 0 R /Info " + std::to_string(info) + " 0 R >>\nstartxref\n" + std::to_string(xref) +
                "\n%%EOF\n";
        return out_;
    }
    std::string out_ = "%PDF-1.4\n%\xE2\xE3\xCF\xD3\n";

private:
    std::vector<std::size_t> offsets_;
};

std::string ellipse_path(double cx, double cy, double rx, double ry)
{
    const double k = 0.5523;
    std::ostringstream p;
    p << num(cx + rx) << " " << num(cy) << " m\n"
      << num(cx + rx) << " " << num(cy + ry * k) << " " << num(cx + rx * k) << " " << num(cy + ry) << " " << num(cx)
      << " " << num(cy + ry) << " c\n"
      << num(cx - rx * k) << " " << num(cy + ry) << " " << num(cx - rx) << " " << num(cy + ry * k) << " "
      << num(cx - rx) << " " << num(cy) << " c\n"
      << num(cx - rx) << " " << num(cy - ry * k) << " " << num(cx - rx * k) << " " << num(cy - ry) << " " << num(cx)
      << " " << num(cy - ry) << " c\n"
      << num(cx + rx * k) << " " << num(cy - ry) << " " << num(cx + rx) << " " << num(cy - ry * k) << " "
      << num(cx + rx) << " " << num(cy) << " c\n";
    return p.str();
}

/// One page's content stream. Coordinates are drawn in staff spaces with y
/// down, under a matrix that maps them onto the PDF's y-up points.
std::string page_content(const LayoutResult& layout, double scale, double page_height_pt, const GlyphTable& table,
                         const OpenTypeFont* font, std::set<std::uint16_t>& used_glyphs)
{
    std::ostringstream c;
    auto colour = [&](Rgb rgb, bool fill) {
        c << num(rgb.r) << " " << num(rgb.g) << " " << num(rgb.b) << (fill ? " rg\n" : " RG\n");
    };
    const Rect page = layout.extent;
    c << "q\n" << num(scale) << " 0 0 " << num(-scale) << " " << num(-page.x * scale) << " "
      << num(page_height_pt + page.y * scale) << " cm\n";
    c << "1 J 1 j\n";  // round caps and joins, as an engraver's pen

    const auto& dl = layout.display_list;
    for (const LineItem& line : dl.lines) {
        colour(colour_for(line.role), false);
        c << num(line.thickness) << " w " << num(line.from.x) << " " << num(line.from.y) << " m " << num(line.to.x)
          << " " << num(line.to.y) << " l S\n";
    }
    for (const QuadItem& q : dl.quads) {
        colour(colour_for(q.role), true);
        c << num(q.corners[0].x) << " " << num(q.corners[0].y) << " m";
        for (int k = 1; k < 4; ++k) c << " " << num(q.corners[k].x) << " " << num(q.corners[k].y) << " l";
        c << " h f\n";
    }
    for (const CurveItem& cv : dl.curves) {
        const Rgb rgb = colour_for(cv.role);
        if (cv.tapered) {
            const double push = cv.thickness / 1.5 * 0.5;
            colour(rgb, true);
            colour(rgb, false);
            c << num(cv.end_thickness) << " w " << num(cv.start.x) << " " << num(cv.start.y) << " m "
              << num(cv.control_one.x) << " " << num(cv.control_one.y - push) << " " << num(cv.control_two.x) << " "
              << num(cv.control_two.y - push) << " " << num(cv.end.x) << " " << num(cv.end.y) << " c "
              << num(cv.control_two.x) << " " << num(cv.control_two.y + push) << " " << num(cv.control_one.x) << " "
              << num(cv.control_one.y + push) << " " << num(cv.start.x) << " " << num(cv.start.y) << " c h B\n";
        } else {
            colour(rgb, false);
            c << num(cv.thickness) << " w " << num(cv.start.x) << " " << num(cv.start.y) << " m "
              << num(cv.control_one.x) << " " << num(cv.control_one.y) << " " << num(cv.control_two.x) << " "
              << num(cv.control_two.y) << " " << num(cv.end.x) << " " << num(cv.end.y) << " c S\n";
        }
    }

    // Glyphs: the text matrix flips y back so the font draws upright.
    for (const GlyphItem& g : dl.glyphs) {
        const GlyphMetrics& m = table.metrics(g.glyph);
        if (m.codepoint == 0) continue;
        const Rgb rgb = colour_for(g.role);
        const std::uint16_t gid = font ? font->glyph_id(m.codepoint) : 0;
        if (font && gid != 0) {
            used_glyphs.insert(gid);
            colour(rgb, true);
            char hex[8];
            std::snprintf(hex, sizeof hex, "%04X", gid);
            c << "BT /M 1 Tf " << num(kStaffSpacesPerEm * g.scale) << " 0 0 " << num(-kStaffSpacesPerEm * g.scale)
              << " " << num(g.origin.x) << " " << num(g.origin.y) << " Tm <" << hex << "> Tj ET\n";
            continue;
        }
        // No font (or no glyph in it): the same stand-ins as the SVG renderer.
        const double advance = m.advance_width * g.scale;
        const GlyphSubstitute s = substitute_for(g.glyph);
        switch (s.kind) {
            case SubstituteKind::FilledEllipse:
                colour(rgb, true);
                c << ellipse_path(g.origin.x + advance * 0.5, g.origin.y, advance * 0.5, 0.52) << "f\n";
                break;
            case SubstituteKind::OpenEllipse:
                colour(rgb, false);
                c << "0.13 w\n" << ellipse_path(g.origin.x + advance * 0.5, g.origin.y, advance * 0.5, 0.52) << "S\n";
                break;
            case SubstituteKind::Dot:
                colour(rgb, true);
                c << ellipse_path(g.origin.x + advance * 0.5, g.origin.y, 0.18, 0.18) << "f\n";
                break;
            case SubstituteKind::Box:
                colour(rgb, false);
                c << "0.13 w " << num(g.origin.x) << " " << num(g.origin.y - s.height * 0.5) << " "
                  << num(advance * 0.7) << " " << num(s.height) << " re S\n";
                break;
            case SubstituteKind::Triangle:
                colour(rgb, true);
                c << num(g.origin.x) << " " << num(g.origin.y) << " m " << num(g.origin.x + 0.9) << " "
                  << num(g.origin.y + s.height * 0.35) << " l " << num(g.origin.x) << " "
                  << num(g.origin.y + s.height) << " l h f\n";
                break;
            case SubstituteKind::Letters:
                colour(rgb, true);
                c << "BT /F1 1 Tf 1.7 0 0 -1.7 " << num(g.origin.x) << " " << num(g.origin.y + 0.55) << " Tm "
                  << literal(to_winansi(s.text)) << " Tj ET\n";
                break;
            case SubstituteKind::None: break;
        }
    }
    for (const TextItem& t : dl.texts) {
        colour(colour_for(t.role), true);
        const char* f = t.bold ? (t.italic ? "/F4" : "/F3") : (t.italic ? "/F2" : "/F1");
        c << "BT " << f << " 1 Tf " << num(t.size) << " 0 0 " << num(-t.size) << " " << num(t.origin.x) << " "
          << num(t.origin.y) << " Tm " << literal(to_winansi(t.text)) << " Tj ET\n";
    }
    c << "Q\n";
    return c.str();
}

/// A text string for the document information dictionary (PDFDocEncoding
/// is close enough to WinAnsi for names and titles).
std::string info_string(const std::string& utf8) { return literal(to_winansi(utf8)); }

}  // namespace

std::string render_pdf(const std::vector<PdfDocumentPart>& parts, const PdfOptions& options, const GlyphTable* glyphs,
                       std::string* error)
{
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return std::string{};
    };
    std::size_t page_count = 0;
    for (const auto& p : parts) {
        if (!p.pages) return fail("a part has no pages");
        page_count += p.pages->pages.size();
    }
    if (page_count == 0) return fail("nothing to print");
    const double scale = options.points_per_staff_space > 0.0 ? options.points_per_staff_space : 5.046;

    OpenTypeFont font;
    if (!options.music_font.empty()) {
        std::string why;
        if (!font.load(options.music_font, &why)) return fail("music font: " + why);
    }
    const GlyphTable defaults;
    const GlyphTable& table = glyphs ? *glyphs : defaults;

    PdfWriter w;
    const int catalog = w.reserve(), pages_id = w.reserve(), info = w.reserve();
    const int f1 = w.reserve(), f2 = w.reserve(), f3 = w.reserve(), f4 = w.reserve();
    const int music = font.loaded() ? w.reserve() : 0;
    const int outlines = w.reserve();

    std::set<std::uint16_t> used;
    std::vector<int> page_ids;
    std::vector<std::pair<std::string, int>> bookmarks;  // title, first page object
    for (const auto& part : parts) {
        bool first = true;
        for (const PrintedPage& page : part.pages->pages) {
            const Rect ext = page.layout.extent;
            const double wpt = std::max(1.0, ext.width * scale), hpt = std::max(1.0, ext.height * scale);
            const int page_id = w.reserve(), content = w.reserve();
            const std::string data = page_content(page.layout, scale, hpt, table, font.loaded() ? &font : nullptr, used);
            w.stream(content, "", data, options.compress);
            std::string res = "<< /Font << /F1 " + std::to_string(f1) + " 0 R /F2 " + std::to_string(f2) + " 0 R /F3 " +
                              std::to_string(f3) + " 0 R /F4 " + std::to_string(f4) + " 0 R";
            if (music) res += " /M " + std::to_string(music) + " 0 R";
            res += " >> /ProcSet [/PDF /Text] >>";
            w.object(page_id, "<< /Type /Page /Parent " + std::to_string(pages_id) + " 0 R /MediaBox [0 0 " + num(wpt) +
                                  " " + num(hpt) + "] /Resources " + res + " /Contents " + std::to_string(content) +
                                  " 0 R >>");
            page_ids.push_back(page_id);
            if (first && !part.bookmark.empty()) bookmarks.emplace_back(part.bookmark, page_id);
            first = false;
        }
    }

    auto standard = [&](int id, const char* base) {
        w.object(id, std::string("<< /Type /Font /Subtype /Type1 /BaseFont /") + base +
                         " /Encoding /WinAnsiEncoding >>");
    };
    standard(f1, "Helvetica");
    standard(f2, "Helvetica-Oblique");
    standard(f3, "Helvetica-Bold");
    standard(f4, "Helvetica-BoldOblique");

    if (music) {
        const int cid = w.reserve(), descriptor = w.reserve(), file = w.reserve();
        const std::string name = font.postscript_name();
        // Widths of the glyphs used, in 1000ths of an em.
        std::string widths = "[";
        for (std::uint16_t gid : used)
            widths += " " + std::to_string(gid) + " [" +
                      num(1000.0 * font.advance(gid) / std::max<std::uint16_t>(1, font.units_per_em())) + "]";
        widths += " ]";
        w.object(music, "<< /Type /Font /Subtype /Type0 /BaseFont /" + name +
                            " /Encoding /Identity-H /DescendantFonts [" + std::to_string(cid) + " 0 R] >>");
        w.object(cid, "<< /Type /Font /Subtype /CIDFontType0 /BaseFont /" + name +
                          " /CIDSystemInfo << /Registry (Adobe) /Ordering (Identity) /Supplement 0 >> /FontDescriptor " +
                          std::to_string(descriptor) + " 0 R /DW 0 /W " + widths + " >>");
        w.object(descriptor, "<< /Type /FontDescriptor /FontName /" + name +
                                 " /Flags 4 /FontBBox [-1000 -3000 4000 3000] /ItalicAngle 0 /Ascent 2000 /Descent -2000"
                                 " /CapHeight 1000 /StemV 80 /FontFile3 " +
                                 std::to_string(file) + " 0 R >>");
        w.stream(file, " /Subtype /CIDFontType0C", font.cff(), options.compress);
    }

    std::string kids;
    for (int id : page_ids) kids += std::to_string(id) + " 0 R ";
    w.object(pages_id, "<< /Type /Pages /Kids [" + kids + "] /Count " + std::to_string(page_ids.size()) + " >>");

    // Outline: one entry per part.
    std::vector<int> items;
    for (std::size_t i = 0; i < bookmarks.size(); ++i) items.push_back(w.reserve());
    for (std::size_t i = 0; i < bookmarks.size(); ++i) {
        std::string body = "<< /Title " + info_string(bookmarks[i].first) + " /Parent " + std::to_string(outlines) +
                           " 0 R /Dest [" + std::to_string(bookmarks[i].second) + " 0 R /Fit]";
        if (i > 0) body += " /Prev " + std::to_string(items[i - 1]) + " 0 R";
        if (i + 1 < items.size()) body += " /Next " + std::to_string(items[i + 1]) + " 0 R";
        w.object(items[i], body + " >>");
    }
    if (items.empty()) w.object(outlines, "<< /Type /Outlines /Count 0 >>");
    else
        w.object(outlines, "<< /Type /Outlines /First " + std::to_string(items.front()) + " 0 R /Last " +
                               std::to_string(items.back()) + " 0 R /Count " + std::to_string(items.size()) + " >>");
    w.object(catalog, "<< /Type /Catalog /Pages " + std::to_string(pages_id) + " 0 R /Outlines " +
                          std::to_string(outlines) + " 0 R" + (items.empty() ? "" : " /PageMode /UseOutlines") + " >>");
    std::string meta = "<< /Producer (VC Composer 2)";
    if (!options.title.empty()) meta += " /Title " + info_string(options.title);
    if (!options.author.empty()) meta += " /Author " + info_string(options.author);
    if (!options.creation_date.empty()) meta += " /CreationDate " + literal(options.creation_date);
    w.object(info, meta + " >>");
    return w.finish(catalog, info);
}

std::string render_pages_pdf(const PrintResult& pages, const PdfOptions& options, const GlyphTable* glyphs,
                             std::string* error)
{
    return render_pdf({{&pages, {}}}, options, glyphs, error);
}

}  // namespace vcc::engraving
