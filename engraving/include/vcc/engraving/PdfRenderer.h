// Printed parts as PDF (Main Spec §16).
//
// A PDF is what a player is sent and what a copyist prints, so this writes
// one directly from the same display lists the SVG renderer and the window
// draw: what prints is what the composer saw. It is framework-free and has
// no third-party dependency - the deflate encoder and the OpenType reader it
// needs are here too.
//
// The music font is embedded (its CFF outlines, as a CID-keyed font indexed
// by glyph id), so the page prints the same on a machine with no music font.
// Bravura is under the SIL Open Font License, which permits embedding.
// Text uses the PDF standard fonts (Helvetica family), which every reader
// has, encoded as WinAnsi; characters outside it print as '?'.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "vcc/engraving/PrintLayout.h"

namespace vcc::engraving {

/// An OpenType font, read far enough to embed it: glyph ids by codepoint,
/// advances, and the CFF table.
class OpenTypeFont {
public:
    /// Reads the font. False (with a reason) for anything that is not a
    /// CFF-flavoured OpenType font ("OTTO").
    bool load(const std::string& bytes, std::string* error = nullptr);
    bool loaded() const noexcept { return !cff_.empty(); }
    /// 0 (.notdef) when the font has no glyph for the codepoint.
    std::uint16_t glyph_id(char32_t codepoint) const noexcept;
    /// Advance in font units.
    std::uint16_t advance(std::uint16_t glyph_id) const noexcept;
    std::uint16_t units_per_em() const noexcept { return units_per_em_; }
    const std::string& cff() const noexcept { return cff_; }
    const std::string& postscript_name() const noexcept { return name_; }
    std::size_t glyph_count() const noexcept { return advances_.size(); }

private:
    std::string cff_;
    std::string name_ = "MusicFont";
    std::uint16_t units_per_em_ = 1000;
    std::vector<std::uint16_t> advances_;
    /// Sorted (codepoint, glyph id) pairs from the best Unicode cmap.
    std::vector<std::pair<char32_t, std::uint16_t>> cmap_;
};

/// zlib-wrapped deflate (RFC 1950/1951) with LZ77 matching and fixed Huffman
/// codes: what PDF's /FlateDecode reads.
std::string zlib_compress(const std::string& data);

struct PdfOptions {
    /// Points per staff space. 5.046 is a staff space of 1.78 mm, the
    /// default PrintOptions rastral: an A4 PrintOptions page becomes an A4
    /// sheet.
    double points_per_staff_space = 72.0 / 25.4 * 1.78;
    /// The music font's bytes (Bravura.otf). Empty draws stand-in shapes
    /// for glyphs, as the SVG renderer's `substitute_glyphs` does.
    std::string music_font;
    std::string title;
    std::string author;
    /// Document dates, "D:20261112120000Z" style; empty omits them so tests
    /// stay byte-stable.
    std::string creation_date;
    /// Compress content streams and the font.
    bool compress = true;
};

/// One part's pages, in the order they print.
struct PdfDocumentPart {
    const PrintResult* pages = nullptr;
    /// Outline (bookmark) entry, e.g. "Violin I". Empty: no outline entry.
    std::string bookmark;
};

/// Writes every part's pages into one PDF (a part per bookmark), for a
/// "print all parts" job, or a single part when there is one entry.
/// `glyphs` must be the table the layout used (it supplies codepoints).
/// Returns the file's bytes; `error` explains an empty result.
std::string render_pdf(const std::vector<PdfDocumentPart>& parts, const PdfOptions& options = {},
                       const GlyphTable* glyphs = nullptr, std::string* error = nullptr);

/// Convenience for one part.
std::string render_pages_pdf(const PrintResult& pages, const PdfOptions& options = {},
                             const GlyphTable* glyphs = nullptr, std::string* error = nullptr);

}  // namespace vcc::engraving
