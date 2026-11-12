#include "vcc/ui/MusicFont.h"

#include <cstddef>

namespace vcc::ui {
namespace {

/// A SMuFL font's em square is the staff height: four staff spaces.
constexpr double kStaffSpacesPerEm = 4.0;

/// Reads a two-element [x, y] array of numbers. Returns false for anything
/// else, so a malformed metadata file degrades to the defaults rather than
/// producing nonsense geometry.
bool read_point(const juce::var& value, double& x, double& y)
{
    const juce::Array<juce::var>* array = value.getArray();
    if (array == nullptr || array->size() != 2)
        return false;
    if (!(*array)[0].isDouble() && !(*array)[0].isInt())
        return false;
    if (!(*array)[1].isDouble() && !(*array)[1].isInt())
        return false;
    x = static_cast<double>((*array)[0]);
    y = static_cast<double>((*array)[1]);
    return true;
}

/// JUCE's DynamicObject exposes its properties only through a non-const
/// reference, so these pointers are non-const even though nothing here
/// writes through them.
juce::DynamicObject* object_property(const juce::var& document, const char* name)
{
    juce::DynamicObject* root = document.getDynamicObject();
    if (root == nullptr)
        return nullptr;
    const juce::var* found = root->getProperties().getVarPointer(juce::Identifier(name));
    return found != nullptr ? found->getDynamicObject() : nullptr;
}

}  // namespace

char32_t parse_smufl_codepoint(const juce::String& text) noexcept
{
    if (!text.startsWithIgnoreCase("U+"))
        return 0;

    const juce::String digits = text.substring(2).trim();
    if (digits.isEmpty())
        return 0;
    for (int index = 0; index < digits.length(); ++index) {
        if (juce::CharacterFunctions::getHexDigitValue(digits[index]) < 0)
            return 0;
    }

    const juce::int64 value = digits.getHexValue64();
    if (value <= 0 || value > 0x10FFFF)
        return 0;
    return static_cast<char32_t>(value);
}

std::size_t apply_smufl_advance_widths(const juce::var& metadata, engraving::GlyphTable& table)
{
    juce::DynamicObject* widths = object_property(metadata, "glyphAdvanceWidths");
    if (widths == nullptr)
        return 0;

    std::size_t applied = 0;
    for (const auto& entry : widths->getProperties()) {
        engraving::Glyph glyph = engraving::Glyph::None;
        if (!engraving::glyph_from_smufl_name(entry.name.toString().toStdString(), glyph))
            continue;
        if (!entry.value.isDouble() && !entry.value.isInt())
            continue;

        const double width = static_cast<double>(entry.value);
        if (!(width > 0.0))
            continue;

        engraving::GlyphMetrics metrics = table.metrics(glyph);
        metrics.advance_width = width;
        table.set_metrics(glyph, metrics);
        ++applied;
    }
    return applied;
}

std::size_t apply_smufl_bounding_boxes(const juce::var& metadata, engraving::GlyphTable& table)
{
    juce::DynamicObject* boxes = object_property(metadata, "glyphBBoxes");
    if (boxes == nullptr)
        return 0;

    std::size_t applied = 0;
    for (const auto& entry : boxes->getProperties()) {
        engraving::Glyph glyph = engraving::Glyph::None;
        if (!engraving::glyph_from_smufl_name(entry.name.toString().toStdString(), glyph))
            continue;

        juce::DynamicObject* box = entry.value.getDynamicObject();
        if (box == nullptr)
            continue;

        const juce::var* north_east = box->getProperties().getVarPointer("bBoxNE");
        const juce::var* south_west = box->getProperties().getVarPointer("bBoxSW");
        if (north_east == nullptr || south_west == nullptr)
            continue;

        double right = 0.0;
        double top = 0.0;
        double left = 0.0;
        double bottom = 0.0;
        if (!read_point(*north_east, right, top) || !read_point(*south_west, left, bottom))
            continue;

        engraving::GlyphMetrics metrics = table.metrics(glyph);
        metrics.bbox_left = left;
        metrics.bbox_right = right;
        // SMuFL measures upward from the baseline; this engine measures
        // downward, like every screen coordinate system. So the north-east
        // corner is the top, negated.
        metrics.bbox_top = -top;
        metrics.bbox_bottom = -bottom;
        table.set_metrics(glyph, metrics);
        ++applied;
    }
    return applied;
}

std::size_t apply_smufl_codepoints(const juce::var& glyph_names, engraving::GlyphTable& table)
{
    juce::DynamicObject* root = glyph_names.getDynamicObject();
    if (root == nullptr)
        return 0;

    std::size_t applied = 0;
    for (const auto& entry : root->getProperties()) {
        engraving::Glyph glyph = engraving::Glyph::None;
        if (!engraving::glyph_from_smufl_name(entry.name.toString().toStdString(), glyph))
            continue;

        juce::DynamicObject* description = entry.value.getDynamicObject();
        if (description == nullptr)
            continue;

        const juce::var* codepoint = description->getProperties().getVarPointer("codepoint");
        if (codepoint == nullptr)
            continue;

        const char32_t parsed = parse_smufl_codepoint(codepoint->toString());
        if (parsed == 0)
            continue;

        engraving::GlyphMetrics metrics = table.metrics(glyph);
        metrics.codepoint = parsed;
        table.set_metrics(glyph, metrics);
        ++applied;
    }
    return applied;
}

juce::StringArray MusicFont::known_font_families()
{
    // Bravura first: it is SMuFL's reference font, and the built-in metrics
    // in Smufl.h were measured from it, so it is the one font where a load
    // without metadata is still metrically right.
    return {"Bravura", "Petaluma", "Leland", "MuseJazz", "Finale Maestro", "Sebastian"};
}

juce::String MusicFont::first_installed_font_family()
{
    const juce::StringArray installed = juce::Font::findAllTypefaceNames();
    for (const juce::String& wanted : known_font_families()) {
        if (installed.contains(wanted))
            return wanted;
    }
    return {};
}

MusicFontLoad MusicFont::load_installed(const juce::String& family_name,
                                        engraving::GlyphTable& table)
{
    MusicFontLoad result;

    if (family_name.isEmpty()) {
        result.message = "No music font family named";
        return result;
    }

    // Asked for by name rather than searched for, because a name this system
    // does not have produces a substituted typeface rather than a failure -
    // and a substituted text font drawing music codepoints is worse than no
    // font at all, since the window would look like it was working.
    if (!juce::Font::findAllTypefaceNames().contains(family_name)) {
        result.message = family_name + " is not installed on this system";
        return result;
    }

    typeface_ = juce::Typeface::createSystemTypefaceFor(
        juce::FontOptions(family_name, 12.0f, juce::Font::plain));
    if (typeface_ == nullptr) {
        result.message = family_name + " is installed but could not be loaded";
        return result;
    }

    result.typeface_loaded = true;
    name_ = family_name;

    // Nothing is overridden: a system font brings no metadata. The built-in
    // metrics stand in, and `glyphs_overridden` staying at zero is what the
    // UI reads to say so.
    result.message = "Using the installed " + family_name
                   + " with built-in metrics (no metadata alongside an installed font)";
    return result;
}

MusicFontLoad MusicFont::load(const juce::File& font_file, const juce::File& metadata_file,
                              const juce::File& glyph_names_file, engraving::GlyphTable& table)
{
    MusicFontLoad result;

    if (!font_file.existsAsFile()) {
        result.message = "No music font at " + font_file.getFullPathName();
        return result;
    }

    juce::MemoryBlock bytes;
    if (!font_file.loadFileAsData(bytes) || bytes.getSize() == 0) {
        result.message = "Could not read the music font at " + font_file.getFullPathName();
        return result;
    }

    typeface_ = juce::Typeface::createSystemTypefaceFor(bytes.getData(), bytes.getSize());
    if (typeface_ == nullptr) {
        result.message = font_file.getFileName() + " is not a font this system can load";
        return result;
    }

    result.typeface_loaded = true;
    name_ = typeface_->getName();

    const juce::var metadata =
        metadata_file.existsAsFile() ? juce::JSON::parse(metadata_file) : juce::var();
    const juce::var names =
        glyph_names_file.existsAsFile() ? juce::JSON::parse(glyph_names_file) : juce::var();
    apply_metadata(metadata, names, table, result);
    return result;
}

MusicFontLoad MusicFont::load_from_memory(const void* font_bytes, std::size_t font_size,
                                          const juce::String& metadata_json,
                                          engraving::GlyphTable& table)
{
    MusicFontLoad result;

    if (font_bytes == nullptr || font_size == 0) {
        result.message = "No font bytes to load";
        return result;
    }

    typeface_ = juce::Typeface::createSystemTypefaceFor(font_bytes, font_size);
    if (typeface_ == nullptr) {
        result.message = "The embedded font is not one this system can load";
        return result;
    }

    result.typeface_loaded = true;
    name_ = typeface_->getName();

    apply_metadata(juce::JSON::parse(metadata_json), juce::var(), table, result);
    return result;
}

void MusicFont::apply_metadata(const juce::var& metadata, const juce::var& glyph_names,
                               engraving::GlyphTable& table, MusicFontLoad& result) const
{
    const std::size_t before = table.overridden_count();

    if (metadata.isObject()) {
        result.advances_loaded = apply_smufl_advance_widths(metadata, table) > 0;
        result.bounding_boxes_loaded = apply_smufl_bounding_boxes(metadata, table) > 0;
    }

    if (glyph_names.isObject())
        result.codepoints_loaded = apply_smufl_codepoints(glyph_names, table) > 0;

    result.glyphs_overridden = table.overridden_count() - before;

    // Say what is missing rather than reporting a bare success: a font loaded
    // without its metrics still draws, but the spacing comes from this
    // engine's defaults, and that is worth knowing.
    if (result.advances_loaded && result.bounding_boxes_loaded)
        result.message = name_ + " loaded with its own metrics";
    else
        result.message = name_ + " loaded, using built-in metrics (no font metadata found)";
}

MusicFontLoad MusicFont::load_from_directory(const juce::File& directory,
                                             engraving::GlyphTable& table)
{
    MusicFontLoad result;

    if (!directory.isDirectory()) {
        result.message = "Not a directory: " + directory.getFullPathName();
        return result;
    }

    // SMuFL fonts ship as <Name>.otf beside <name>_metadata.json, with
    // glyphnames.json from the specification itself. Take the first font in
    // the directory rather than requiring a particular name, since a user
    // may have renamed it.
    const juce::Array<juce::File> fonts =
        directory.findChildFiles(juce::File::findFiles, false, "*.otf;*.ttf;*.woff");
    if (fonts.isEmpty()) {
        result.message = "No .otf or .ttf font in " + directory.getFullPathName();
        return result;
    }

    const juce::File font = fonts.getFirst();

    juce::File metadata = directory.getChildFile(
        font.getFileNameWithoutExtension().toLowerCase() + "_metadata.json");
    if (!metadata.existsAsFile()) {
        const juce::Array<juce::File> candidates =
            directory.findChildFiles(juce::File::findFiles, false, "*_metadata.json");
        if (!candidates.isEmpty())
            metadata = candidates.getFirst();
    }

    return load(font, metadata, directory.getChildFile("glyphnames.json"), table);
}

void MusicFont::unload()
{
    typeface_ = nullptr;
    name_ = {};
}

juce::Font MusicFont::font_for_staff_space(double pixels_per_staff_space) const
{
    const float em = static_cast<float>(kStaffSpacesPerEm * pixels_per_staff_space);
    if (typeface_ == nullptr)
        return juce::Font(juce::FontOptions(em));

    // The em, which JUCE calls the point height - not JUCE's "height", which
    // is ascent plus descent. A music font's ascent and descent reach far
    // beyond its em to cover the clefs and long stems: Bravura's sum to four
    // times its em. Sized by height, every glyph drew at a quarter of its
    // size, and the generated test font, whose ascent and descent happen to
    // sum to exactly its em, could not tell the difference.
    return juce::Font(juce::FontOptions(typeface_).withPointHeight(em));
}

}  // namespace vcc::ui
