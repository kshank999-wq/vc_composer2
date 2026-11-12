// Copyright (c) VC Composer. All rights reserved.
//
// Drawing a display list on screen (spec 02 sections 4.3, 5.1, 5.3).
//
// The second back end for `engraving::DisplayList`, alongside SVG. It holds
// no state and makes no layout decisions: everything about where things go
// was settled by the layout engine, which is why that engine is testable
// without any of this.
#pragma once

#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>

#include "vcc/engraving/LayoutEngine.h"
#include "vcc/ui/MusicFont.h"

namespace vcc::ui {

struct NotationColours {
    juce::Colour background{juce::Colour::fromRGB(250, 249, 246)};
    juce::Colour staff{juce::Colour::fromRGB(58, 58, 58)};
    juce::Colour ink{juce::Colour::fromRGB(16, 16, 16)};
    juce::Colour ruler{juce::Colour::fromRGB(138, 138, 138)};
    juce::Colour cursor{juce::Colour::fromRGB(200, 64, 64)};
    juce::Colour playhead{juce::Colour::fromRGB(64, 128, 200)};
    juce::Colour selection{juce::Colour::fromRGB(120, 170, 235)};
};

/// The staff size a view is drawn at before anybody changes it.
///
/// Named rather than written twice: `NotationView::kDefaultPixelsPerStaffSpace`
/// is this, and a zoom of 100% means exactly this. The two were briefly
/// different numbers, and the notation quietly came out a fifth smaller than
/// it had been.
constexpr double kDefaultPixelsPerStaffSpace = 10.0;

struct NotationRenderSettings {
    /// Vertical zoom: the one number that sizes a staff. Horizontal zoom
    /// lives in the timeline, because the timeline governs x.
    double pixels_per_staff_space = kDefaultPixelsPerStaffSpace;

    NotationColours colours{};

    /// Pixel position of staff-space (0, 0). Scrolling is a change to this,
    /// not a relayout.
    juce::Point<float> origin{0.0f, 0.0f};

    /// Interface font for bar numbers and text markings.
    juce::String text_font_name;
};

/// Draws a display list, and nothing else.
///
/// Everything about where things go was settled by the layout engine; this
/// is the part the plug-in window needs, because a strip carried over the
/// bridge is a display list's worth of geometry and no more.
void draw_display_list(juce::Graphics& graphics, const engraving::DisplayList& list,
                       const engraving::GlyphTable& glyphs,
                       const NotationRenderSettings& settings,
                       const MusicFont* music_font = nullptr);

/// Draws a laid-out score.
///
/// `music_font` may be null, or loaded without the glyph the layout asked
/// for. Either way the geometry is still drawn, with primitive stand-ins for
/// the glyphs: a view that silently goes blank because a font is missing is
/// the worst of the available behaviours.
void draw_notation(juce::Graphics& graphics, const engraving::LayoutResult& layout,
                   const engraving::GlyphTable& glyphs,
                   const NotationRenderSettings& settings,
                   const MusicFont* music_font = nullptr,
                   const std::vector<notation::EventId>* selection = nullptr);

/// Settings that place `bounds` inside `area`, as large as will fit.
///
/// The plug-in window's problem, and not the companion window's: the
/// application chose the strip's coordinates to suit its own page, and the
/// plug-in has a fixed panel to put them in and no say in either. So it
/// scales to fit and centres what is left over.
///
/// Never larger than `max_pixels_per_staff_space`, because a strip holding
/// one bar would otherwise be blown up to fill the panel and a strip holding
/// four would not - and notation that changes size as the music goes past is
/// harder to read than notation that is simply small.
///
/// An empty or degenerate `bounds` gets the default scale rather than a
/// division by zero.
NotationRenderSettings fit_into(const engraving::Rect& bounds, juce::Rectangle<int> area,
                                double max_pixels_per_staff_space = kDefaultPixelsPerStaffSpace);

/// Size in pixels the layout needs at these settings, for a scroll view.
juce::Rectangle<int> notation_bounds(const engraving::LayoutResult& layout,
                                     const NotationRenderSettings& settings);

}  // namespace vcc::ui
