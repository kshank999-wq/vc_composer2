// The keyboard deck (docs/spec/ui/SPEC.md, "88-key keyboard and key zoom"):
// an 88-key piano, A0 to C8, that zooms from 1x to 3x around an anchor note
// and scrolls sideways when it no longer fits; pitch and mod wheels; the
// octave shift for MIDI keyboards; and step input into the Composer view.
#pragma once

#include <bitset>
#include <functional>
#include <optional>

#include "ui/AppContext.h"
#include "ui/hw/HwControls.h"
#include "vcc/editing/EditCursor.h"
#include "vcc/project/RenderCache.h"

namespace vcc::desktop::shell {

/// The keys themselves: 52 white and 36 black, scaled by the zoom.
class PianoKeys final : public juce::Component {
public:
    static constexpr int kLowest = 21, kHighest = 108;  ///< A0, C8.
    static constexpr int kWhiteKeys = 52;
    static constexpr double kWhiteWidth = 22.0;

    PianoKeys();
    void set_zoom(double zoom);
    double key_width() const noexcept { return kWhiteWidth * zoom_; }
    void set_lit(const std::bitset<128>& lit);
    const std::bitset<128>& lit() const noexcept { return lit_; }
    /// The white-key index (0-51) a note sits on (a black key: the one below it).
    static int white_index(int midi);
    static bool is_black(int midi);
    /// The note under a point, or -1.
    int note_at(juce::Point<float> p) const;
    /// The centre x of a note's key.
    float centre_of(int midi) const;

    /// A key pressed (velocity 1-127, from how far down the key it was hit)
    /// and released.
    std::function<void(int note, int velocity, bool shift)> on_press;
    std::function<void(int note)> on_release;
    /// Ctrl/Cmd + wheel: zoom about x (in this component).
    std::function<void(float steps, float x)> on_zoom_wheel;

    void paint(juce::Graphics& g) override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;
    void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override;

private:
    int velocity_at(int note, juce::Point<float> p) const;
    double zoom_ = 1.0;
    std::bitset<128> lit_;
    int down_ = -1;
};

class KeyboardDeck final : public hw::Panel, private juce::Timer {
public:
    explicit KeyboardDeck(AppContext& context);
    ~KeyboardDeck() override;

    /// The deck grows with the key zoom: 140 px at 1x, 236 px at 3x.
    int preferred_height() const;
    std::function<void()> on_height_changed;
    /// The Mixer key (Producer view): swaps the deck for the mixer.
    std::function<void()> on_mixer;
    void show_mixer_key(bool show);
    hw::Keycap& mixer_key() noexcept { return mixer_; }

    /// Step input: enter a note at the Composer view's cursor (false when it
    /// cannot), and the editor's state for the readout.
    std::function<bool(int midi, bool chord)> on_step;
    std::function<void(composer::Command)> on_command;
    std::function<std::optional<editing::EditCursor>()> cursor;

    /// Zooms to `zoom` (clamped 1-3), keeping the anchor note centred.
    void set_zoom(double zoom);
    double zoom() const noexcept { return zoom_; }
    /// Zoom to selection: about 2.5x, centred on the selection's notes.
    void zoom_to_selection();
    /// Centres the keyboard on a note.
    void centre_on(int midi);
    /// The note the zoom keeps centred: the selection's middle note, else
    /// the sounding notes, else middle C.
    int anchor_note() const;
    /// The first and last note fully in view.
    std::pair<int, int> visible_range() const;
    void set_octave(int octaves);
    /// Recomputes the lit keys and the step readout (the timer does this).
    void refresh();

    PianoKeys& keys() noexcept { return keys_; }
    hw::Keycap& chord_key() noexcept { return chord_; }
    hw::Keycap& enter_key() noexcept { return enter_; }
    const hw::Lcd& zoom_lcd() const noexcept { return zoom_lcd_; }
    const hw::Lcd& step_lcd() const noexcept { return step_lcd_; }
    const hw::Lcd& octave_lcd() const noexcept { return octave_lcd_; }
    juce::Slider& zoom_slider() noexcept { return slider_; }
    hw::Wheel& pitch_wheel() noexcept { return pitch_; }
    hw::Wheel& mod_wheel() noexcept { return mod_; }

    void resized() override;
    void paintOverChildren(juce::Graphics& g) override;

private:
    class ZoomSlider;
    void timerCallback() override { refresh(); }
    void scroll_to(double left, bool animate);
    void press(int note, int velocity, bool shift);
    std::bitset<128> selection_notes() const;

    AppContext& context_;
    int listener_ = 0;
    PianoKeys keys_;
    juce::Viewport viewport_;
    std::unique_ptr<ZoomSlider> slider_owner_;
    juce::Slider& slider_;
    hw::Keycap minus_{"-"}, plus_{"+"}, fit_{juce::String::fromUTF8("88 \xc2\xb7 Fit")}, to_sel_{"Zoom to selection"},
        middle_c_{"Middle C"}, mixer_{"Mixer"}, oct_down_{"-"}, oct_up_{"+"}, chord_{"Chord", true}, tie_{"Tie"}, enter_{"Enter", true};
    hw::Lcd range_lcd_{juce::String::fromUTF8("88 KEYS \xc2\xb7 A0\xe2\x80\x93" "C8"), 10.5f}, zoom_lcd_{"1.00x", 12.0f},
        octave_lcd_{juce::String::fromUTF8("\xc2\xb1" "0"), 12.0f}, step_lcd_{{}, 11.0f};
    hw::Wheel pitch_{true}, mod_{false};
    double zoom_ = 1.0;
    int octave_ = 0;
    int last_velocity_ = 92;
    std::uint64_t generation_ = ~std::uint64_t{0};
    // A short ease when following a lit note off-screen.
    double scroll_from_ = 0.0, scroll_to_ = 0.0;
    int scroll_step_ = -1;
    std::bitset<128> last_lit_;
    mutable project::RenderCache render_;
};

}  // namespace vcc::desktop::shell
