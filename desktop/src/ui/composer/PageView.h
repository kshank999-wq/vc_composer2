// The Composer view's Page view (docs/spec/ui): the full score on pages,
// exactly as it prints (the same layout as File > Export Full Score), drawn
// as sheets on the desk. Scroll with the wheel, zoom with Cmd/Ctrl+wheel or
// the zoom keys; a click on a system moves the playhead to its first bar.
// It follows edits (re-laid a moment after the score changes) and marks the
// system the playhead is in. Reading and checking the layout only: notes are
// written in the Track view.
#pragma once

#include <memory>

#include "ui/AppContext.h"
#include "ui/ScoreExchange.h"
#include "ui/hw/HwControls.h"

namespace vcc::desktop {

class PageView final : public juce::Component, private juce::Timer, private juce::ScrollBar::Listener {
public:
    explicit PageView(AppContext& context);
    ~PageView() override;

    /// Lays the score out again now (otherwise it happens a moment after an edit).
    void relayout();
    void set_playhead(Ticks tick);
    /// Back to the first page (Return to Start).
    void scroll_to_start()
    {
        scroll_y_ = 0;
        vbar_.setCurrentRangeStart(0.0, juce::dontSendNotification);
        repaint();
    }
    /// Zoom in or out a step (the zoom keys, Cmd/Ctrl +/-).
    void zoom(bool larger);
    double zoom_factor() const noexcept { return zoom_; }

    // For the check.
    int page_count() const noexcept;
    /// Page `i`'s rectangle on screen (empty when out of range).
    juce::Rectangle<float> page_rect(int i) const;
    /// The bar a click at `p` would go to (0: none).
    int bar_at(juce::Point<float> p) const;
    const FullScoreLayout* layout() const noexcept { return layout_.get(); }
    /// The page settings (paper, concert pitch, bars per system, empty
    /// staves, order), shared with the full-score export and remembered.
    const FullScoreSettings& settings() const noexcept { return settings_; }
    void set_settings(const FullScoreSettings& s);
    hw::Keycap& paper_key() { return paper_; }
    hw::Keycap& concert_key() { return concert_; }
    hw::Keycap& bars_key() { return bars_; }
    hw::Keycap& hide_key() { return hide_; }
    hw::Keycap& order_key() { return order_; }
    hw::Keycap& landscape_key() { return landscape_; }
    /// Sheet Info: the title, composer and other credits, and which print.
    hw::Keycap& sheet_info_key() { return sheet_info_; }
    /// The heading at the top of the first page (a click there opens Sheet Info).
    bool title_at(juce::Point<float> p) const;
    /// Pages side by side (2) when the view is wide, else one under another (1).
    int columns() const noexcept;

    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override;
    void visibilityChanged() override;

private:
    void timerCallback() override;
    void scrollBarMoved(juce::ScrollBar*, double start) override;
    void update_scroll();
    double px_per_space() const;
    int content_height() const;

    AppContext& context_;
    int listener_ = 0;
    std::unique_ptr<FullScoreLayout> layout_;
    juce::String message_;
    bool dirty_ = true;
    int dirty_age_ = 0;
    double zoom_ = 1.0;  ///< 1: a page as wide as the view allows, up to its printed size.
    int scroll_y_ = 0;
    int playhead_bar_ = 1;
    juce::ScrollBar vbar_{true};
    FullScoreSettings settings_;
    hw::Keycap paper_{"Paper"}, concert_{"Concert pitch", true}, bars_{"Bars"}, hide_{"Hide empty", true},
        order_{"Orchestral order", true}, landscape_{"Landscape", true}, sheet_info_{"Sheet Info"};
    void refresh_keys();
    int bar_top() const noexcept { return 34; }
};

}  // namespace vcc::desktop
