// Copyright (c) VC Composer. All rights reserved.
//
// Prints the glyphs this engine uses, as JSON, for a font generator to read.
//
// Bravura is embedded in every build now (resources/fonts/Bravura), but a
// real font cannot check one thing the tests need checked: that the engine
// asks for every glyph it names, with advances a font could disagree about.
// `scripts/make-test-music-font.py` builds a small SMuFL font with a glyph
// at every codepoint the engine uses and advances chosen to differ from
// the defaults, and this is where it learns what to put in it.
//
// It reads the engine's own table rather than repeating it. A generator with
// its own list of codepoints would agree with itself and drift from
// `engraving/Smufl.cpp` the moment a glyph was added there, and the test
// built on it would keep passing while covering one glyph fewer.
#include <cstdint>
#include <cstdio>
#include <string>

#include "vcc/engraving/Smufl.h"

namespace {

/// Highest enumerator this walks. Deliberately far past the end of the
/// table: what stops the walk is `smufl_name` returning empty, so a glyph
/// added to the enum is picked up without anything here being edited.
constexpr std::uint16_t kSearchLimit = 4096;

void print_escaped(const char* text)
{
    for (const char* c = text; *c != '\0'; ++c) {
        if (*c == '"' || *c == '\\')
            std::fputc('\\', stdout);
        std::fputc(*c, stdout);
    }
}

}  // namespace

int main()
{
    const vcc::engraving::GlyphTable table;

    std::printf("[\n");
    bool first = true;
    std::size_t count = 0;

    for (std::uint16_t raw = 1; raw < kSearchLimit; ++raw) {
        const auto glyph = static_cast<vcc::engraving::Glyph>(raw);
        const char* name = vcc::engraving::smufl_name(glyph);
        if (name == nullptr || *name == '\0')
            continue;

        const vcc::engraving::GlyphMetrics& metrics = table.metrics(glyph);
        if (metrics.codepoint == 0)
            continue;

        if (!first)
            std::printf(",\n");
        first = false;
        ++count;

        std::printf("  {\"name\": \"");
        print_escaped(name);
        std::printf("\", \"codepoint\": %u, \"default_advance\": %.4f}",
                    static_cast<unsigned>(metrics.codepoint), metrics.advance_width);
    }
    std::printf("\n]\n");

    // A generator handed an empty list would produce an empty font, and a
    // test loading it would report a clean pass having covered nothing. The
    // exit code is what stops that.
    if (count == 0) {
        std::fprintf(stderr, "no glyphs found: the table in engraving/Smufl.cpp was not read\n");
        return 1;
    }
    std::fprintf(stderr, "%zu glyphs\n", count);
    return 0;
}
