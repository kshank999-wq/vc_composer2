#include "vcc/ui/NotationRenderer.h"

#include <algorithm>

#include "vcc/engraving/GlyphSubstitute.h"
#include "vcc/music/MusicalTime.h"

namespace vcc::ui {
namespace {

juce::Colour colour_for(engraving::ItemRole role, const NotationColours& colours)
{
    switch (role) {
        case engraving::ItemRole::StaffLine:
        case engraving::ItemRole::LedgerLine:
        case engraving::ItemRole::Barline:  return colours.staff;
        case engraving::ItemRole::RulerMark:
        case engraving::ItemRole::RulerLabel: return colours.ruler;
        case engraving::ItemRole::Cursor:   return colours.cursor;
        default: break;
    }
    return colours.ink;
}

/// Maps a point in staff spaces to pixels.
struct Projection {
    double scale = 10.0;
    juce::Point<float> origin{};

    juce::Point<float> operator()(const engraving::Point& point) const
    {
        return {origin.x + static_cast<float>(point.x * scale),
                origin.y + static_cast<float>(point.y * scale)};
    }

    float length(double staff_spaces) const
    {
        return static_cast<float>(staff_spaces * scale);
    }

    juce::Rectangle<float> rect(const engraving::Rect& bounds) const
    {
        const juce::Point<float> top_left = (*this)(engraving::Point{bounds.x, bounds.y});
        return {top_left.x, top_left.y, length(bounds.width), length(bounds.height)};
    }
};

/// A stroke thin enough to vanish is drawn at one pixel instead: a staff line
/// that disappears at low zoom is worse than one drawn a little heavy.
float visible_thickness(float thickness)
{
    return std::max(thickness, 1.0f);
}

/// Draws a glyph's primitive stand-in. The caller has already set the
/// colour, so this only deals with shape.
void draw_substitute(juce::Graphics& graphics, const engraving::GlyphItem& item,
                     const engraving::GlyphMetrics& metrics, const Projection& project,
                     const NotationRenderSettings& settings)
{
    const engraving::GlyphSubstitute stand_in = engraving::substitute_for(item.glyph);
    const juce::Point<float> origin = project(item.origin);
    const double advance = metrics.advance_width * item.scale;

    switch (stand_in.kind) {
        case engraving::SubstituteKind::FilledEllipse:
        case engraving::SubstituteKind::OpenEllipse: {
            const juce::Rectangle<float> head(
                origin.x, origin.y - project.length(0.52), project.length(advance),
                project.length(1.04));
            if (stand_in.kind == engraving::SubstituteKind::FilledEllipse)
                graphics.fillEllipse(head);
            else
                graphics.drawEllipse(head, visible_thickness(project.length(0.13)));
            break;
        }

        case engraving::SubstituteKind::Dot: {
            const float radius = project.length(0.18);
            graphics.fillEllipse(origin.x + project.length(advance * 0.5) - radius,
                                 origin.y - radius, radius * 2.0f, radius * 2.0f);
            break;
        }

        case engraving::SubstituteKind::Box: {
            const juce::Rectangle<float> box(
                origin.x, origin.y - project.length(stand_in.height * 0.5),
                project.length(advance * 0.7), project.length(stand_in.height));
            graphics.drawRect(box, visible_thickness(project.length(0.13)));
            break;
        }

        case engraving::SubstituteKind::Triangle: {
            const float tip = project.length(stand_in.height);
            juce::Path flag;
            flag.startNewSubPath(origin.x, origin.y);
            flag.lineTo(origin.x + project.length(0.9), origin.y + tip * 0.35f);
            flag.lineTo(origin.x, origin.y + tip);
            flag.closeSubPath();
            graphics.fillPath(flag);
            break;
        }

        case engraving::SubstituteKind::Letters: {
            graphics.setFont(juce::Font(juce::FontOptions(settings.text_font_name,
                                                          project.length(1.7), juce::Font::plain)));
            graphics.drawSingleLineText(juce::String(stand_in.text), juce::roundToInt(origin.x),
                                        juce::roundToInt(origin.y + project.length(0.55)));
            break;
        }

        case engraving::SubstituteKind::None:
            break;
    }
}

}  // namespace

juce::Rectangle<int> notation_bounds(const engraving::LayoutResult& layout,
                                     const NotationRenderSettings& settings)
{
    const double scale = settings.pixels_per_staff_space;
    return juce::Rectangle<int>(
        juce::roundToInt(layout.extent.width * scale),
        juce::roundToInt(layout.extent.height * scale));
}

void draw_display_list(juce::Graphics& graphics, const engraving::DisplayList& list,
                       const engraving::GlyphTable& glyphs,
                       const NotationRenderSettings& settings, const MusicFont* music_font)
{
    const Projection project{settings.pixels_per_staff_space, settings.origin};

    for (const engraving::LineItem& line : list.lines) {
        graphics.setColour(colour_for(line.role, settings.colours));
        graphics.drawLine(juce::Line<float>(project(line.from), project(line.to)),
                          visible_thickness(project.length(line.thickness)));
    }

    for (const engraving::QuadItem& quad : list.quads) {
        graphics.setColour(colour_for(quad.role, settings.colours));
        juce::Path path;
        path.startNewSubPath(project(quad.corners[0]));
        for (int corner = 1; corner < 4; ++corner)
            path.lineTo(project(quad.corners[corner]));
        path.closeSubPath();
        graphics.fillPath(path);
    }

    for (const engraving::CurveItem& curve : list.curves) {
        graphics.setColour(colour_for(curve.role, settings.colours));
        juce::Path path;
        path.startNewSubPath(project(curve.start));
        if (curve.tapered) {
            // The same lens the SVG draws: two curves with their control
            // points pushed apart, filled, and outlined at the end
            // thickness so the ends are thin rather than vanishing.
            const double push = curve.thickness / 1.5 * 0.5;
            const auto pushed = [&](const engraving::Point& point, double by) {
                return project(engraving::Point{point.x, point.y + by});
            };
            path.cubicTo(pushed(curve.control_one, -push), pushed(curve.control_two, -push),
                         project(curve.end));
            path.cubicTo(pushed(curve.control_two, push), pushed(curve.control_one, push),
                         project(curve.start));
            path.closeSubPath();
            graphics.fillPath(path);
            graphics.strokePath(
                path, juce::PathStrokeType(visible_thickness(project.length(curve.end_thickness))));
            continue;
        }
        path.cubicTo(project(curve.control_one), project(curve.control_two), project(curve.end));
        graphics.strokePath(
            path, juce::PathStrokeType(visible_thickness(project.length(curve.thickness))));
    }

    const bool have_font = music_font != nullptr && music_font->is_loaded();
    const juce::Font music = have_font
                                 ? music_font->font_for_staff_space(
                                       settings.pixels_per_staff_space)
                                 : juce::Font(juce::FontOptions(12.0f));

    for (const engraving::GlyphItem& item : list.glyphs) {
        const engraving::GlyphMetrics& metrics = glyphs.metrics(item.glyph);
        graphics.setColour(colour_for(item.role, settings.colours));

        if (!have_font || metrics.codepoint == 0) {
            draw_substitute(graphics, item, metrics, project, settings);
            continue;
        }

        const juce::Point<float> origin = project(item.origin);
        // Scaled by the em as well: a cue-sized glyph is a smaller em, and
        // scaling JUCE's height instead would re-import the ascent problem
        // that font_for_staff_space exists to avoid.
        graphics.setFont(item.scale == 1.0
                             ? music
                             : music.withPointHeight(music.getHeightInPoints()
                                                     * static_cast<float>(item.scale)));
        graphics.drawSingleLineText(
            juce::String::charToString(static_cast<juce::juce_wchar>(metrics.codepoint)),
            juce::roundToInt(origin.x), juce::roundToInt(origin.y));
    }

    for (const engraving::TextItem& text : list.texts) {
        const juce::Point<float> origin = project(text.origin);
        graphics.setColour(colour_for(text.role, settings.colours));
        graphics.setFont(juce::Font(juce::FontOptions(
            settings.text_font_name, project.length(text.size),
            (text.bold ? juce::Font::bold : 0) | (text.italic ? juce::Font::italic : 0))));
        graphics.drawSingleLineText(text.text, juce::roundToInt(origin.x),
                                    juce::roundToInt(origin.y));
    }
}

NotationRenderSettings fit_into(const engraving::Rect& bounds, juce::Rectangle<int> area,
                                double max_pixels_per_staff_space)
{
    NotationRenderSettings settings;
    settings.pixels_per_staff_space = max_pixels_per_staff_space;

    // Guarded rather than left to the arithmetic. Without this the ratios
    // below are inf for an empty strip and NaN when the panel is empty too,
    // and the result would still come out at the ceiling - but only because
    // std::min returns its first argument when the comparison is false,
    // which is a fact about NaN and not a decision anybody made here. No
    // mutation kills this line for exactly that reason; it is here so the
    // outcome does not depend on that.
    if (bounds.width > 0.0 && bounds.height > 0.0 && area.getWidth() > 0
        && area.getHeight() > 0) {
        const double to_fit = std::min(static_cast<double>(area.getWidth()) / bounds.width,
                                       static_cast<double>(area.getHeight()) / bounds.height);
        settings.pixels_per_staff_space = std::min(max_pixels_per_staff_space, to_fit);
    }

    const double scale = settings.pixels_per_staff_space;
    const double drawn_width = bounds.width * scale;
    const double drawn_height = bounds.height * scale;

    // `origin` is where staff-space (0, 0) lands, so the strip's own top-left
    // is subtracted out before the leftover space is shared.
    settings.origin = {
        static_cast<float>(static_cast<double>(area.getX())
                           + (static_cast<double>(area.getWidth()) - drawn_width) / 2.0
                           - bounds.x * scale),
        static_cast<float>(static_cast<double>(area.getY())
                           + (static_cast<double>(area.getHeight()) - drawn_height) / 2.0
                           - bounds.y * scale),
    };
    return settings;
}

void draw_notation(juce::Graphics& graphics, const engraving::LayoutResult& layout,
                   const engraving::GlyphTable& glyphs,
                   const NotationRenderSettings& settings, const MusicFont* music_font,
                   const std::vector<notation::EventId>* selection)
{
    const engraving::DisplayList& list = layout.display_list;

    // Selection first, so it sits behind the notation rather than over it.
    //
    // This is the one thing the strip cannot do and does not need to: it
    // reads the bounding boxes the layout recorded per score event, which
    // exist for hit testing and selection, and a read-only strip in the
    // plug-in window has neither.
    if (selection != nullptr && !selection->empty()) {
        const Projection project{settings.pixels_per_staff_space, settings.origin};
        graphics.setColour(settings.colours.selection.withAlpha(0.35f));
        for (const notation::EventId id : *selection) {
            for (const engraving::Rect& bounds : list.bounds_for_event(id))
                graphics.fillRoundedRectangle(project.rect(bounds.expanded(0.1)), 2.0f);
        }
    }

    draw_display_list(graphics, list, glyphs, settings, music_font);
}





}  // namespace vcc::ui
