// The Edit window's top deck (docs/spec/ui/SPEC.md, "Layout", band 1):
// the brand nameplate with the Composer | Producer switch, the transport,
// the counters, the session readouts and the master meters. One deck for
// both views: the switch flips the view below it, and nothing else moves.
#pragma once

#include <memory>
#include <vector>

#include "ui/AppContext.h"
#include "ui/hw/HwControls.h"

namespace vcc::desktop::shell {

/// A position as the counters show it: bars|beats|ticks, the ticks counted
/// at 960 to the quarter note (the engine keeps 2880; a third exactly).
juce::String bars_beats_ticks(const MeterMap& meter, Ticks tick);
/// A length as bars|beats|ticks (0|0|000 for none), measured in the meter
/// where it starts.
juce::String length_bbt(const MeterMap& meter, Ticks from, Ticks to);

class TopDeck final : public juce::Component {
public:
    explicit TopDeck(AppContext& context);
    ~TopDeck() override;

    /// Called on the window's timer: position, transport keys, meters.
    void update_position();
    void update_from_document();
    /// Shows which view is up (the switch never changes it by itself; it
    /// asks through `on_view`).
    void set_producer(bool producer);
    std::function<void(bool producer)> on_view;
    /// The selection or punch range the start/end/length readout shows;
    /// empty (from == to) shows the playhead and no length.
    void set_range(Ticks from, Ticks to);

    /// Height for a width: the deck wraps to a second row under ~1280 px.
    static int height_for(int width);
    void resized() override;
    void paint(juce::Graphics& g) override;
    void paintOverChildren(juce::Graphics& g) override;

    // For the check.
    hw::Keycap& composer_key() { return *view_[0]; }
    hw::Keycap& producer_key() { return *view_[1]; }
    hw::Keycap& play_key() { return *transport_[4]; }
    hw::Keycap& record_key() { return *transport_[5]; }
    hw::Lcd& main_counter() { return main_; }
    hw::Lcd& range_lcd() { return range_; }
    hw::Lcd& tempo_lcd() { return tempo_; }
    hw::Meter& master_meter(int side) { return side == 0 ? master_l_ : master_r_; }

private:
    void edit_tempo();
    void choose_meter();
    void choose_key();

    AppContext& context_;
    int listener_ = 0;
    hw::Panel nameplate_{{}, true}, transport_panel_{"Transport"}, counter_panel_, session_panel_, master_panel_{"Master"};
    std::vector<std::unique_ptr<hw::Keycap>> view_, transport_, session_keys_;
    hw::Lcd main_{"1|1|000", 34.0f}, sub_{"00:00.000", 15.0f}, range_{{}, 11.0f};
    hw::Lcd tempo_{{}, 16.0f}, meter_{{}, 16.0f}, key_{{}, 16.0f}, peak_{{}, 10.0f};
    hw::Meter master_l_{hw::Meter::Orientation::Horizontal, 30}, master_r_{hw::Meter::Orientation::Horizontal, 30};
    hw::Led midi_in_{hw::tok::meter_green};
    bool show_timecode_ = false;
    Ticks range_from_ = 0, range_to_ = 0;
    float peak_hold_db_ = -100.0f;
    int peak_age_ = 0;
    int blink_ = 0;
};

}  // namespace vcc::desktop::shell
