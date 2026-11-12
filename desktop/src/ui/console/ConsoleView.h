// The console: a row of channel strips (tracks, then group and FX buses)
// with the master section pinned on the right. The processing panels scroll
// vertically together; the fader panels are always visible; the whole row
// scrolls horizontally. Meters are the only thing repainted at frame rate.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "ui/AppContext.h"
#include "ui/console/ConsoleStrip.h"

namespace vcc::desktop::console {

class ConsoleView final : public juce::Component,
                          public TipHost,
                          private juce::Timer,
                          private juce::ScrollBar::Listener {
public:
    explicit ConsoleView(AppContext& context);
    ~ConsoleView() override;

    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override;

    void show_tip(juce::Component& source, const juce::String& text) override;
    void hide_tip() override;

    // For tests and the window: what is on the console.
    int strip_count() const noexcept { return static_cast<int>(strips_.size()); }
    Strip* strip_for(const std::string& id) const;
    Strip* master() const noexcept { return master_.get(); }
    void set_narrow(bool narrow);
    void set_section_shown(Section s, bool shown);
    void scroll_to(int x, int y);

private:
    struct Tip;
    void sync();
    void rebuild();
    void layout_all();
    void relayout_strips();
    void timerCallback() override;
    void scrollBarMoved(juce::ScrollBar* bar, double start) override;
    void apply_scroll();
    void load_settings();
    void save_settings() const;
    void update_header_buttons();
    void add_bus(bool fx);

    AppContext& context_;
    ConsoleLayout layout_;
    std::vector<std::unique_ptr<Strip>> strips_;
    std::unique_ptr<Strip> master_;
    std::vector<std::string> keys_;
    int track_count_ = 0;

    juce::Component top_clip_, top_row_, bottom_clip_, bottom_row_, master_top_clip_;
    juce::ScrollBar hbar_{false}, vbar_{true};
    std::vector<std::unique_ptr<LedButton>> section_buttons_;
    LedButton narrow_button_{"NARROW", paint::led_amber, "Narrow strips"};
    LedButton add_fx_{"+ FX BUS", paint::led_white, "Add an FX (aux) bus, fed by sends"};
    LedButton add_group_{"+ GROUP", paint::led_white, "Add a group bus (subgroup / stem)"};
    std::unique_ptr<Tip> tip_;

    juce::Rectangle<int> header_, legend_, strips_area_;
    int top_h_ = 0, bottom_h_ = 0, bus_gap_x_ = -1;
    int hx_ = 0, vy_ = 0;
    int listener_ = 0;
    int ticks_ = 0;
};

}  // namespace vcc::desktop::console
