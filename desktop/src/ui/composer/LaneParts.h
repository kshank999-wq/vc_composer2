// A Composer view lane's parts (docs/spec/ui/SPEC.md, "Track header" and
// "Phrase blocks"): the 220 px track header built from the hardware
// controls, and the layer of phrase blocks drawn over the staff.
#pragma once

#include <string>

#include "ui/AppContext.h"
#include "ui/hw/HwControls.h"

namespace vcc::ui {
class NotationView;
}

namespace vcc::desktop::composer_ui {

class TrackHeader final : public juce::Component {
public:
    static constexpr int kWidth = 220;
    TrackHeader(AppContext& context, std::string track_id);
    void set_number(int n);
    /// Re-reads the track: name, sound, keys, knobs, staff tag.
    void refresh();
    void push_meter(float left_db, float right_db);

    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent& e) override;

    hw::Keycap& record_key() { return rec_; }
    hw::Keycap& solo_key() { return solo_; }
    hw::Keycap& mute_key() { return mute_; }
    hw::SmallKnob& volume() { return vol_; }
    const hw::Lcd& scribble() const { return scribble_; }

private:
    void show_menu();
    AppContext& context_;
    std::string track_;
    hw::Lcd number_{{}, 11.0f}, scribble_{{}, 12.0f}, vol_lcd_{{}, 9.5f}, pan_lcd_{{}, 9.5f};
    hw::Keycap rec_{"R", true}, solo_{"S", true}, mute_{"M", true}, tag_{"Treble"};
    hw::SmallKnob vol_, pan_;
    hw::Meter meter_{hw::Meter::Orientation::Vertical, 12};
};

/// Phrase blocks over a staff. Takes the mouse only on a block's name strip
/// and edges, so clicks on the music reach the staff underneath.
class PhraseLayer final : public juce::Component {
public:
    PhraseLayer(AppContext& context, std::string track_id, ui::NotationView& view);
    bool hitTest(int x, int y) override;
    void paint(juce::Graphics& g) override;
    void mouseMove(const juce::MouseEvent& e) override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;
    static constexpr int kNameStrip = 14;

private:
    enum class Part { None, Name, Left, Right };
    struct Hit {
        int index = -1;
        Part part = Part::None;
    };
    Hit hit(juce::Point<int> p) const;
    juce::Rectangle<float> block_rect(Ticks from, Ticks to) const;
    void show_menu(int index);
    Ticks snap(Ticks t, bool to_bar) const;
    float music_left() const;

    AppContext& context_;
    std::string track_;
    ui::NotationView& view_;
    Hit drag_;
    Ticks down_tick_ = 0, preview_from_ = 0, preview_to_ = 0;
    bool dragging_ = false;
    int gesture_ = 0;
};

}  // namespace vcc::desktop::composer_ui
