// One console channel strip: a track, a group or FX bus, or the master. The
// strip has two panels: the upper processing panel (input, filters, EQ,
// dynamics, inserts, sends, output), which scrolls vertically with all other
// strips, and the lower panel (pan, buttons, fader, meter, scribble strip),
// which is always visible.
#pragma once

#include <array>
#include <memory>
#include <string>
#include <vector>

#include "ui/AppContext.h"
#include "ui/console/ConsoleControls.h"

namespace vcc::desktop::console {

enum class Section { Input, Filters, Eq, Dynamics, Inserts, Sends, Output };
constexpr int kSectionCount = 7;
const char* section_title(Section s);

/// Shared geometry: which sections are shown and how wide strips are.
struct ConsoleLayout {
    std::array<bool, kSectionCount> show{{true, true, true, true, true, true, true}};
    bool narrow = false;
    int fx_count = 0;

    struct Placed {
        Section section;
        int y = 0, height = 0;
    };
    int strip_width() const noexcept { return narrow ? 76 : 100; }
    int master_width() const noexcept { return narrow ? 128 : 160; }
    int section_height(Section s) const noexcept;
    std::vector<Placed> placed() const;
    int top_height() const;
    static constexpr int kTopMargin = 12;
    static constexpr int kBottomFixed = 170;
};

class Strip {
public:
    enum class Kind { Track, Group, Fx, Master };
    struct FxBus {
        std::string id;
        std::string name;
    };

    Strip(AppContext& context, Kind kind, std::string id, const ConsoleLayout& layout, std::vector<FxBus> fx_buses,
          int number = 0);
    ~Strip();
    Strip(const Strip&) = delete;
    Strip& operator=(const Strip&) = delete;

    juce::Component& top() noexcept;
    juce::Component& bottom() noexcept;
    Kind kind() const noexcept { return kind_; }
    const std::string& id() const noexcept { return id_; }

    /// Reads every value from the document (cheap when nothing changed).
    void refresh();
    /// One meter tick (and the record lamp's blink).
    void tick(int tick_count);
    void slow_tick();
    void set_number(int n);
    void update_selection();
    /// The section geometry or width changed.
    void relayout();

private:
    struct Panel;
    struct SendRow;
    friend struct Panel;

    template <typename F> void edit(const juce::String& what, F&& change);
    void paint_top(juce::Graphics& g, juce::Rectangle<int> bounds);
    void paint_bottom(juce::Graphics& g, juce::Rectangle<int> bounds);
    void layout_top();
    void layout_bottom();
    void panel_mouse_down(const juce::MouseEvent& e);
    juce::Colour panel_colour() const;

    void setup_controls();
    void bind_strip_knob(Knob& knob, const juce::String& what, double project::ChannelStrip::*field);
    void bind_band_knob(Knob& knob, const juce::String& what, project::EqBand project::ChannelStrip::*band,
                        double project::EqBand::*field);
    void bind_strip_button(LedButton& button, const juce::String& what, bool project::ChannelStrip::*field);
    void bind_band_button(LedButton& button, const juce::String& what, project::EqBand project::ChannelStrip::*band,
                          bool project::EqBand::*field);
    void show_input_menu();
    void show_output_menu();
    void show_monitor_menu();
    void show_insert_menu(int index, bool lamp);
    void show_send_menu(const std::string& bus_id);
    void show_name_menu();
    void edit_gain_text();
    bool is_track() const noexcept { return kind_ == Kind::Track; }
    bool can_record() const noexcept;

    AppContext& context_;
    Kind kind_;
    std::string id_;
    const ConsoleLayout& layout_;
    std::vector<FxBus> fx_buses_;
    std::unique_ptr<Panel> top_, bottom_;

    // Cached document state for painting.
    std::string name_;
    juce::Colour colour_;
    project::TrackKind track_kind_ = project::TrackKind::Instrument;
    bool record_armed_ = false;
    bool stereo_input_ = false;
    int number_ = 0;
    double pan_value_ = 0.0;
    bool has_plugin_ = false;

    // Input
    Lcd input_;
    Knob trim_;
    LedButton phase_;
    // Filters
    Knob hpf_, lpf_;
    LedButton hpf_in_, lpf_in_;
    // EQ
    EqCurve curve_;
    Knob hf_gain_, hf_freq_, hmf_gain_, hmf_freq_, hmf_q_, lmf_gain_, lmf_freq_, lmf_q_, lf_gain_, lf_freq_;
    LedButton hf_bell_, lf_bell_, eq_in_;
    // Dynamics
    Knob comp_threshold_, comp_ratio_, comp_makeup_, comp_attack_, comp_release_;
    Knob gate_threshold_, gate_range_, gate_release_;
    LedButton dyn_in_, dyn_pre_, gate_in_;
    GrMeter gr_;
    // Inserts, sends, output
    InsertRack inserts_;
    std::vector<std::unique_ptr<SendRow>> sends_;
    Lcd output_;
    // Lower panel
    Knob pan_;
    LedButton rec_, monitor_, mute_, solo_;
    Fader fader_;
    LedMeter meter_;
    Lcd readout_;
    Scribble scribble_;
    std::unique_ptr<juce::TextEditor> gain_editor_;

    struct PrintedLabel {
        juce::Rectangle<float> area;
        juce::String text;
        juce::Colour colour;
    };
    std::vector<PrintedLabel> top_labels_;
    std::vector<ConsoleLayout::Placed> placed_;
    juce::Rectangle<int> pan_text_area_, scale_area_, meter_scale_area_;
    std::vector<std::string> output_ids_;
};

}  // namespace vcc::desktop::console
