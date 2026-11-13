// One console channel strip: a track, a group or FX bus, or the master. The
// strip has two panels: the upper processing panel (input, filters, EQ,
// dynamics, inserts, sends, output), which scrolls vertically with all other
// strips, and the lower panel (pan, buttons, fader, meter, scribble strip),
// which is always visible.
#pragma once

#include <array>
#include <map>
#include <memory>
#include <set>
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

/// Sets every strip's automation mode (tracks, buses, master) as one undo step.
void set_all_automation_modes(AppContext& context, const std::string& mode);

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
    /// Controls driven by automation follow the engine's current values.
    /// Cheap when no control on the strip has a lane (called every frame).
    void follow_automation();
    /// Reads the processing delay (a couple of times a second).
    void delay_tick();

    // --- Automation (also used by tests) ---------------------------------------------------
    /// The owner id the engine and Session know the strip by ("bus-master" for the master).
    const std::string& owner() const noexcept { return id_; }
    /// The automation param id of a control on this strip, or empty.
    std::string param_of(const juce::Component* control) const;
    /// The control bound to a param id, or null.
    juce::Component* control_for(const std::string& param) const;
    /// Every automatable control and its param id.
    std::vector<std::pair<juce::Component*, std::string>> automation_controls() const;
    /// The mode selector's choices: this strip (one undo step), or every strip.
    void choose_automation_mode(const std::string& mode, bool all_strips);
    /// The context-menu items of an automatable control.
    void show_automation_lane(const std::string& param);
    void toggle_lane_enabled(const std::string& param);
    juce::Button& automation_button() noexcept { return auto_mode_; }
    DelayReadout& delay_readout() noexcept { return delay_; }

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
    void bind_strip_knob(Knob& knob, const juce::String& what, double project::ChannelStrip::*field,
                         const std::string& param);
    void bind_band_knob(Knob& knob, const juce::String& what, project::EqBand project::ChannelStrip::*band,
                        double project::EqBand::*field, const std::string& param);
    /// `param` empty: not automatable (DYN PRE).
    void bind_strip_button(LedButton& button, const juce::String& what, bool project::ChannelStrip::*field,
                           const std::string& param);
    void bind_band_button(LedButton& button, const juce::String& what, project::EqBand project::ChannelStrip::*band,
                          bool project::EqBand::*field);
    void show_input_menu();
    void show_output_menu();
    void show_monitor_menu();
    void show_insert_menu(int index, bool lamp);
    int insert_count() const;
    void show_send_menu(const std::string& bus_id);
    void show_name_menu();
    void edit_gain_text();
    void show_automation_mode_menu(bool all_strips);
    void show_automation_menu(juce::Component& target, const std::string& param);
    void add_automation_items(juce::PopupMenu& m, const std::string& param) const;
    bool handle_automation_item(int result, const std::string& param);
    juce::String lane_name(const std::string& param) const;

    // Automation plumbing. A gesture on a control whose parameter the engine
    // is writing goes to the engine (begin / touch / end) instead of the
    // document; otherwise it is an ordinary edit (one undo step per gesture).
    enum class ControlType { Knob, Fader, Button };
    struct Binding {
        juce::Component* control = nullptr;
        ControlType type = ControlType::Knob;
        std::string param;
        bool following = false;  ///< Showing the engine's automated value.
    };
    void bind(juce::Component& control, ControlType type, std::string param);
    void bind_knob(Knob& knob, const juce::String& what, const std::string& param, std::function<void(double)> write);
    void bind_toggle(LedButton& button, const std::string& param, std::function<void(bool)> write);
    bool start_touch(const std::string& param);
    bool touch(const std::string& param, double value);
    void end_touch(const std::string& param);
    void show_control_value(Binding& b, double value);
    void show_dependent(const std::string& param, double value);
    void update_curve();
    void refresh_automation(const std::vector<project::AutomationLane>& lanes, const std::string& mode);
    std::string engine_id() const;
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

    // Automation state (refreshed from the document).
    std::vector<Binding> bindings_;
    std::vector<std::size_t> followers_;  ///< Bindings with an enabled lane while the mode is not off.
    std::map<std::string, std::pair<std::string, bool>> lanes_;  ///< param -> lane id, enabled.
    std::string mode_ = "read";
    std::set<std::string> touching_;  ///< Params with an engine touch open.
    std::array<bool, 4> band_on_{{true, true, true, true}};  ///< LF, LMF, HMF, HF (for the curve).
    std::array<double, 4> band_q_{{0.7, 1.0, 1.0, 0.7}};

    // Input
    Lcd input_;
    DelayReadout delay_;
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
    ModeSelector auto_mode_;
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
    juce::Rectangle<int> pan_text_area_, scale_area_, meter_scale_area_, auto_label_area_;
    std::vector<std::string> output_ids_;
};

}  // namespace vcc::desktop::console
