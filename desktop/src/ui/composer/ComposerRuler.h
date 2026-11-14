// The Composer view's top band (docs/spec/ui/SPEC.md, "Ruler rows" and the
// overview strip): the whole piece in one strip with the visible bars
// boxed, then four 22 px rows - Bars|Beats, Markers, Chords, Tempo - that
// line up with the staves below.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "ui/AppContext.h"
#include "vcc/project/RenderCache.h"

namespace vcc::desktop::composer_ui {

/// How the ruler reaches the staves' timeline (owned by the Composer view).
struct TimelineAccess {
    std::function<float(Ticks)> x_of;     ///< In the ruler's coordinates.
    std::function<Ticks(float)> tick_at;
    std::function<Ticks()> origin;
    std::function<Ticks()> visible;
    std::function<void(Ticks)> scroll_to;  ///< New left edge.
};

class ComposerRuler final : public juce::Component, private juce::ChangeListener {
public:
    static constexpr int kLabelWidth = 220;
    static constexpr int kOverview = 30;
    static constexpr int kRow = 22;
    static constexpr int kHeight = kOverview + 6 + 4 * kRow;

    ComposerRuler(AppContext& context, TimelineAccess access);
    ~ComposerRuler() override;

    void set_playhead(Ticks tick);
    /// The chord symbols read from the score, bar by bar (for the check).
    const std::vector<std::string>& chords() const noexcept { return chords_; }

    void paint(juce::Graphics& g) override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;

private:
    void changeListenerCallback(juce::ChangeBroadcaster*) override { repaint(); }
    void refresh();
    juce::Rectangle<int> overview_area() const;
    juce::Rectangle<int> row(int i) const;  ///< 0 bars, 1 markers, 2 chords, 3 tempo (content part).
    Ticks song_end() const;
    void overview_scroll(float x);

    AppContext& context_;
    TimelineAccess access_;
    int listener_ = 0;
    project::RenderCache render_;
    std::vector<std::string> chords_;
    /// Per music track: its colour and the spans worth showing (phrases, or bars with notes).
    struct Lane {
        juce::Colour colour;
        std::vector<std::pair<Ticks, Ticks>> spans;
    };
    std::vector<Lane> lanes_;
    Ticks playhead_ = 0;
    enum class Drag { None, Overview, Range } drag_ = Drag::None;
    Ticks drag_from_ = 0;
};

}  // namespace vcc::desktop::composer_ui
