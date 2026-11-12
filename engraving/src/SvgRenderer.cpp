#include "vcc/engraving/SvgRenderer.h"

#include "vcc/engraving/GlyphSubstitute.h"

#include <cstdint>
#include <sstream>
#include <string>

namespace vcc::engraving {
namespace {

/// SMuFL fonts are designed on an em square equal to the staff height, which
/// is four staff spaces. So one em is four spaces, and that is the only
/// conversion needed to size the music font.
constexpr double kStaffSpacesPerEm = 4.0;

std::string escape_xml(const std::string& text)
{
    std::string escaped;
    escaped.reserve(text.size());
    for (const char character : text) {
        switch (character) {
            case '&': escaped += "&amp;"; break;
            case '<': escaped += "&lt;"; break;
            case '>': escaped += "&gt;"; break;
            case '"': escaped += "&quot;"; break;
            case '\'': escaped += "&apos;"; break;
            default: escaped += character; break;
        }
    }
    return escaped;
}

/// Glyphs are written as numeric character references rather than as UTF-8
/// bytes, so the file is pure ASCII and survives any pipeline that mangles
/// encodings.
std::string character_reference(char32_t codepoint)
{
    std::ostringstream out;
    out << "&#x" << std::hex << std::uppercase << static_cast<std::uint32_t>(codepoint) << ";";
    return out.str();
}

std::uint32_t byte_at(const std::string& bytes, std::size_t index)
{
    return static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[index]));
}

std::string base64(const std::string& bytes)
{
    static const char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    std::size_t index = 0;
    while (index + 2 < bytes.size()) {
        const std::uint32_t triple = (byte_at(bytes, index) << 16) | (byte_at(bytes, index + 1) << 8)
                                     | byte_at(bytes, index + 2);
        out += kAlphabet[(triple >> 18) & 63];
        out += kAlphabet[(triple >> 12) & 63];
        out += kAlphabet[(triple >> 6) & 63];
        out += kAlphabet[triple & 63];
        index += 3;
    }
    const std::size_t rest = bytes.size() - index;
    if (rest == 1) {
        const std::uint32_t value = byte_at(bytes, index) << 16;
        out += kAlphabet[(value >> 18) & 63];
        out += kAlphabet[(value >> 12) & 63];
        out += "==";
    } else if (rest == 2) {
        const std::uint32_t value = (byte_at(bytes, index) << 16) | (byte_at(bytes, index + 1) << 8);
        out += kAlphabet[(value >> 18) & 63];
        out += kAlphabet[(value >> 12) & 63];
        out += kAlphabet[(value >> 6) & 63];
        out += '=';
    }
    return out;
}

const char* colour_for(ItemRole role) noexcept
{
    switch (role) {
        case ItemRole::StaffLine:
        case ItemRole::LedgerLine:
        case ItemRole::Barline:       return "#3a3a3a";
        case ItemRole::RulerMark:
        case ItemRole::RulerLabel:    return "#8a8a8a";
        case ItemRole::Cursor:        return "#c84040";
        default: break;
    }
    return "#101010";
}


}  // namespace

std::string render_svg(const LayoutResult& layout, const SvgOptions& options,
                       const GlyphTable* glyphs)
{
    const double scale = options.pixels_per_staff_space > 0.0 ? options.pixels_per_staff_space
                                                              : 12.0;

    // The drawn bounds rather than the page: this sizes an image, so an item
    // outside the margins has to be inside it rather than clipped.
    Rect extent = layout.drawn_bounds;
    if (extent.width <= 0.0 || extent.height <= 0.0)
        extent = layout.extent;
    if (extent.width <= 0.0 || extent.height <= 0.0)
        extent = layout.display_list.bounds.expanded(1.0);
    if (extent.width <= 0.0 || extent.height <= 0.0)
        extent = Rect{0.0, 0.0, 1.0, 1.0};

    const double x = extent.x * scale;
    const double y = extent.y * scale;
    const double width = extent.width * scale;
    const double height = extent.height * scale;

    std::ostringstream svg;
    svg.setf(std::ios::fixed);
    svg.precision(3);

    svg << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    svg << "<svg xmlns=\"http://www.w3.org/2000/svg\" version=\"1.1\" width=\"" << width
        << "\" height=\"" << height << "\" viewBox=\"" << x << " " << y << " " << width << " "
        << height << "\">\n";
    svg << "<title>" << escape_xml(options.title) << "</title>\n";

    if (!options.embedded_font.empty()) {
        // The font travels with the page, so a player's machine with no
        // music font installed prints the same notation. Base64 rather
        // than a file beside it: one file is one thing to send.
        svg << "<defs><style type=\"text/css\">@font-face{font-family:\""
            << escape_xml(options.music_font) << "\";src:url(data:font/"
            << escape_xml(options.embedded_font_format) << ";base64,"
            << base64(options.embedded_font) << ") format(\""
            << escape_xml(options.embedded_font_format) << "\");}</style></defs>\n";
    }

    if (!options.background.empty()) {
        svg << "<rect x=\"" << x << "\" y=\"" << y << "\" width=\"" << width << "\" height=\""
            << height << "\" fill=\"" << escape_xml(options.background) << "\"/>\n";
    }

    for (const LineItem& line : layout.display_list.lines) {
        svg << "<line x1=\"" << line.from.x * scale << "\" y1=\"" << line.from.y * scale
            << "\" x2=\"" << line.to.x * scale << "\" y2=\"" << line.to.y * scale
            << "\" stroke=\"" << colour_for(line.role) << "\" stroke-width=\""
            << line.thickness * scale << "\"/>\n";
    }

    for (const QuadItem& quad : layout.display_list.quads) {
        svg << "<polygon points=\"";
        for (int corner = 0; corner < 4; ++corner) {
            if (corner > 0)
                svg << " ";
            svg << quad.corners[corner].x * scale << "," << quad.corners[corner].y * scale;
        }
        svg << "\" fill=\"" << colour_for(quad.role) << "\"/>\n";
    }

    for (const CurveItem& curve : layout.display_list.curves) {
        if (curve.tapered) {
            // A filled lens between two curves whose control points are
            // pushed apart, outlined at the end thickness: thin at the
            // ends, `thickness` at the middle. The midpoint of a cubic
            // moves three quarters of what its control points move, so
            // the push is two thirds of the thickness.
            const double push = curve.thickness / 1.5 * 0.5;
            svg << "<path d=\"M " << curve.start.x * scale << " " << curve.start.y * scale
                << " C " << curve.control_one.x * scale << " " << (curve.control_one.y - push) * scale
                << ", " << curve.control_two.x * scale << " " << (curve.control_two.y - push) * scale
                << ", " << curve.end.x * scale << " " << curve.end.y * scale
                << " C " << curve.control_two.x * scale << " " << (curve.control_two.y + push) * scale
                << ", " << curve.control_one.x * scale << " " << (curve.control_one.y + push) * scale
                << ", " << curve.start.x * scale << " " << curve.start.y * scale << " Z\" fill=\""
                << colour_for(curve.role) << "\" stroke=\"" << colour_for(curve.role)
                << "\" stroke-width=\"" << curve.end_thickness * scale << "\"/>\n";
            continue;
        }
        svg << "<path d=\"M " << curve.start.x * scale << " " << curve.start.y * scale << " C "
            << curve.control_one.x * scale << " " << curve.control_one.y * scale << ", "
            << curve.control_two.x * scale << " " << curve.control_two.y * scale << ", "
            << curve.end.x * scale << " " << curve.end.y * scale << "\" fill=\"none\" stroke=\""
            << colour_for(curve.role) << "\" stroke-width=\"" << curve.thickness * scale
            << "\"/>\n";
    }

    // Glyph metrics are not available here, so the layout's own bounds are
    // what a box is drawn from - which is exactly what should be checked.
    if (options.draw_glyph_boxes) {
        for (const GlyphItem& glyph : layout.display_list.glyphs) {
            svg << "<rect x=\"" << glyph.bounds.x * scale << "\" y=\"" << glyph.bounds.y * scale
                << "\" width=\"" << glyph.bounds.width * scale << "\" height=\""
                << glyph.bounds.height * scale
                << "\" fill=\"none\" stroke=\"#c0d8f0\" stroke-width=\"0.5\"/>\n";
        }
    }

    const GlyphTable defaults;
    const GlyphTable& table = glyphs != nullptr ? *glyphs : defaults;
    for (const GlyphItem& glyph : layout.display_list.glyphs) {
        const GlyphMetrics& metrics = table.metrics(glyph.glyph);
        if (metrics.codepoint == 0)
            continue;

        const char* colour = colour_for(glyph.role);

        if (!options.substitute_glyphs) {
            svg << "<text x=\"" << glyph.origin.x * scale << "\" y=\"" << glyph.origin.y * scale
                << "\" font-family=\"" << escape_xml(options.music_font) << "\" font-size=\""
                << kStaffSpacesPerEm * scale * glyph.scale << "\" fill=\"" << colour << "\">"
                << character_reference(metrics.codepoint) << "</text>\n";
            continue;
        }

        const double advance = metrics.advance_width * glyph.scale;
        const double glyph_x = glyph.origin.x * scale;
        const double glyph_y = glyph.origin.y * scale;

        const GlyphSubstitute stand_in = substitute_for(glyph.glyph);

        switch (stand_in.kind) {
            case SubstituteKind::FilledEllipse:
            case SubstituteKind::OpenEllipse:
                svg << "<ellipse cx=\"" << glyph_x + advance * 0.5 * scale << "\" cy=\""
                    << glyph_y << "\" rx=\"" << advance * 0.5 * scale << "\" ry=\""
                    << 0.52 * scale << "\" fill=\""
                    << (stand_in.kind == SubstituteKind::OpenEllipse ? "none" : colour)
                    << "\" stroke=\"" << colour << "\" stroke-width=\"" << 0.13 * scale
                    << "\"/>\n";
                break;

            case SubstituteKind::Dot:
                svg << "<circle cx=\"" << glyph_x + advance * 0.5 * scale << "\" cy=\""
                    << glyph_y << "\" r=\"" << 0.18 * scale << "\" fill=\"" << colour
                    << "\"/>\n";
                break;

            case SubstituteKind::Box:
                svg << "<rect x=\"" << glyph_x << "\" y=\""
                    << glyph_y - stand_in.height * 0.5 * scale << "\" width=\""
                    << advance * 0.7 * scale << "\" height=\"" << stand_in.height * scale
                    << "\" fill=\"none\" stroke=\"" << colour << "\" stroke-width=\""
                    << 0.13 * scale << "\"/>\n";
                break;

            case SubstituteKind::Triangle: {
                const double tip = stand_in.height * scale;
                svg << "<polygon points=\"" << glyph_x << "," << glyph_y << " "
                    << glyph_x + 0.9 * scale << "," << glyph_y + tip * 0.35 << " " << glyph_x
                    << "," << glyph_y + tip << "\" fill=\"" << colour << "\"/>\n";
                break;
            }

            case SubstituteKind::Letters:
                // Sized and baselined so the letter reads where the music
                // glyph would have sat.
                svg << "<text x=\"" << glyph_x << "\" y=\"" << glyph_y + 0.55 * scale
                    << "\" font-family=\"" << escape_xml(options.text_font)
                    << "\" font-size=\"" << 1.7 * scale << "\" fill=\"" << colour << "\">"
                    << escape_xml(stand_in.text) << "</text>\n";
                break;

            case SubstituteKind::None:
                break;
        }
    }

    for (const TextItem& text : layout.display_list.texts) {
        svg << "<text x=\"" << text.origin.x * scale << "\" y=\"" << text.origin.y * scale
            << "\" font-family=\"" << escape_xml(options.text_font) << "\" font-size=\""
            << text.size * scale << "\"";
        if (text.italic)
            svg << " font-style=\"italic\"";
        if (text.bold)
            svg << " font-weight=\"bold\"";
        svg << " fill=\"" << colour_for(text.role) << "\">" << escape_xml(text.text)
            << "</text>\n";
    }

    svg << "</svg>\n";
    return svg.str();
}

}  // namespace vcc::engraving
