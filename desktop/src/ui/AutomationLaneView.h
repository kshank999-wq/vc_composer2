// Automation lanes in the Sequencer (Main Spec §12): one row per shown lane
// under its track or bus, with a header (parameter chooser, the owner's
// automation mode, enable, hide, remove, add) and the lane's curve over the
// timeline. Breakpoints are added, dragged, selected, typed, shaped, drawn
// with a pencil and cleared by range; every gesture is one undo step
// (Session::edit_lane). The curve is drawn by sampling project::value_at, so
// what you see is exactly what the engine plays.
#pragma once

#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "ui/AppContext.h"
#include "vcc/project/Automation.h"

namespace vcc::desktop {

namespace automation_ui {

/// Where a value sits on a lane, 0 (bottom) .. 1 (top). Volume and sends use
/// a fader law (more room near 0 dB), log_scale params a log mapping, the
/// rest a linear one.
double to_norm(const project::ParamInfo& p, double value);
double from_norm(const project::ParamInfo& p, double norm);
/// "-6.0 dB", "-inf dB", "C", "L35", "1.20 kHz", "45 ms", "4.0:1", "On", "62%".
juce::String format_value(const project::ParamInfo& p, double value);
/// Reads what format_value writes (and plain numbers); clamped to the range.
std::optional<double> parse_value(const project::ParamInfo& p, const juce::String& text);

struct GridLine {
    double value = 0.0;
    juce::String label;
};
/// Value grid lines for a lane of the given height, spaced so labels fit.
std::vector<GridLine> grid_lines(const project::ParamInfo& p, int height_px);

/// off grey, read green, touch yellow, latch orange, write red.
juce::Colour mode_colour(const std::string& mode);

/// One entry of the parameter chooser.
struct ParamChoice {
    std::string id;
    std::string name;
    std::string section;  ///< "" for Volume/Pan/Mute (top level), else "Sends", "EQ", ...
};
/// Everything automatable on a track or bus, grouped in sections, including
/// the instrument plug-in's and insert plug-ins' parameters from the engine.
std::vector<ParamChoice> param_choices(AppContext& context, const std::string& owner_id);
/// Shows the chooser; `chosen` gets {param id, display name}.
void show_param_menu(AppContext& context, const std::string& owner_id, juce::Component* target,
                     const std::function<void(const std::string&, const std::string&)>& chosen);

}  // namespace automation_ui

/// What an automation lane needs from the timeline it sits in.
class AutomationTimeline {
public:
    virtual ~AutomationTimeline() = default;
    virtual AppContext& context() const = 0;
    /// Width of the header column; the timeline starts there (lane-local x).
    virtual int header_width() const = 0;
    virtual double x_of_tick(double tick) const = 0;
    virtual double tick_at_x(double x) const = 0;
    /// Snaps to the sequencer's grid (follows the zoom).
    virtual Ticks snap_tick(Ticks tick) const = 0;
    virtual Ticks playhead_tick() const = 0;
    /// The ruler selection; empty when first >= second.
    virtual std::pair<Ticks, Ticks> selected_range() const = 0;
    virtual bool range_is_punch() const = 0;
    /// Bar/beat lines and bar shading behind a lane.
    virtual void paint_timeline_grid(juce::Graphics& g, juce::Rectangle<int> area) const = 0;
    virtual bool pencil_mode() const = 0;
    virtual void set_pencil_mode(bool on) = 0;
    /// A lane's row was resized by dragging its bottom edge.
    virtual void set_lane_height(const std::string& owner_id, const std::string& lane_id, int height) = 0;
};

class AutomationLaneView final : public juce::Component {
public:
    static constexpr int kDefaultHeight = 70;
    static constexpr int kMinHeight = 34;
    static constexpr int kMaxHeight = 260;
    static constexpr int kPlaceholderHeight = 40;

    struct Data {
        std::string owner_id;
        juce::String owner_name;
        juce::Colour colour;
        std::string mode = "read";
        /// Empty: the "choose a parameter" row an owner shows when it has no shown lane.
        std::optional<project::AutomationLane> lane;
    };

    explicit AutomationLaneView(AutomationTimeline& timeline);
    ~AutomationLaneView() override;

    /// New document state (keeps the selection of points that still exist).
    void set_data(Data data);
    const Data& data() const noexcept { return data_; }
    const std::optional<project::ParamInfo>& info() const noexcept { return info_; }

    /// Repaints the header readout when the value at the playhead changed.
    void playhead_moved();

    // --- Edits (also used by tests) ------------------------------------------------------
    const std::set<Ticks>& selection() const noexcept { return selection_; }
    void set_selection(std::set<Ticks> ticks);
    bool delete_selection();
    /// Flattens the lane inside the ruler range (guard points keep the outside).
    bool clear_range(Ticks from, Ticks to);
    bool set_shape(const std::set<Ticks>& points, const std::string& shape);
    bool set_point_value(Ticks tick, double value);
    void copy_selection() const;
    bool paste_at(Ticks tick);

    enum class Part { Name, Readout, Mode, Power, Hide, Remove, Add };
    juce::Rectangle<int> part_bounds(Part part) const;
    /// Lane-local position of a value at a tick.
    juce::Point<float> position_of(Ticks tick, double value) const;
    /// The value text at the playhead shown in the header.
    juce::String readout() const;

    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseMove(const juce::MouseEvent& e) override;
    void mouseExit(const juce::MouseEvent& e) override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;
    void mouseDoubleClick(const juce::MouseEvent& e) override;
    bool keyPressed(const juce::KeyPress& key) override;
    void focusLost(FocusChangeType cause) override;

private:
    enum class Drag { None, Points, Marquee, Pencil, Resize };

    const std::vector<project::AutomationPoint>& points() const { return work_.points; }
    juce::Rectangle<int> body() const;
    float y_of_norm(double norm) const;
    double norm_of_y(float y) const;
    float y_of_value(double v) const;
    double value_of_y(float y) const;
    double x_of(Ticks t) const;
    Ticks tick_of(float x) const;
    int point_at(juce::Point<float> p) const;
    std::optional<double> curve_value(Ticks t) const;
    juce::Colour curve_colour() const;
    bool live() const;  ///< Lane enabled and the owner's mode plays it.
    std::string fresh_label(const std::string& base) const;
    bool commit(const std::string& label, std::vector<project::AutomationPoint> pts);
    juce::String position_text(Ticks t) const;

    void paint_header(juce::Graphics& g);
    void paint_body(juce::Graphics& g, juce::Rectangle<int> clip);
    void header_click(const juce::MouseEvent& e);
    void show_header_menu();
    void show_mode_menu();
    void show_chooser(bool replace);
    void confirm_remove();
    void show_point_menu(int index);
    void show_segment_menu(juce::Point<float> at);
    void edit_value(Ticks tick);
    void close_editor(bool commit_text);
    void begin_points_drag(Ticks anchor, const std::string& label, bool moved);
    void commit_pencil();

    AutomationTimeline& tl_;
    Data data_;
    std::optional<project::ParamInfo> info_;
    project::AutomationLane work_;  ///< The lane as drawn (the document's, or a gesture's preview).
    std::set<Ticks> selection_;

    Drag drag_ = Drag::None;
    std::string drag_label_;
    bool moved_ = false;
    std::vector<project::AutomationPoint> origin_;
    std::set<Ticks> origin_selection_;
    Ticks anchor_tick_ = 0;
    double anchor_norm_ = 0.0;
    juce::Rectangle<float> marquee_;
    std::vector<project::AutomationPoint> pencil_;
    int resize_start_ = 0;
    juce::String bubble_;
    juce::Point<float> bubble_at_;
    int hover_point_ = -1;
    std::optional<Part> hover_part_;
    juce::String last_readout_;
    std::unique_ptr<juce::TextEditor> editor_;
    Ticks editor_tick_ = 0;
};

}  // namespace vcc::desktop
