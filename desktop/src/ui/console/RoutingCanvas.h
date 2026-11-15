// Visual Routing (Add-On Features spec §5): the whole signal path as a
// zoomable, pannable, searchable canvas. Nodes are the tracks, buses, the
// master and the hardware output, laid out left to right by signal flow;
// lines are outputs (solid) and sends (dashed, with level and pre/post).
//
// It is drawn from project::routing_graph on every change, so it and the
// mixer are two views of one routing; every edit goes through the session
// (undoable, saved, and applied to the engine by its usual snapshot swap),
// and a connection that would loop is refused with the path that loops.
#pragma once

#include <memory>
#include <set>
#include <string>

#include "ui/AppContext.h"
#include "ui/hw/HwControls.h"
#include "vcc/project/Routing.h"

namespace vcc::desktop::console {

class RoutingCanvas final : public juce::Component, private juce::Timer {
public:
    explicit RoutingCanvas(AppContext& context);
    ~RoutingCanvas() override;

    const project::RoutingGraph& graph() const noexcept { return graph_; }
    /// Re-reads the project (the timer does this as things change).
    void refresh();

    /// Connects `from` to bus `to`: 0 routes its output there, 1 adds a
    /// post-fader send, 2 a pre-fader send. A refusal says why (also on the
    /// canvas and the status line).
    bool connect(const std::string& from, const std::string& to, int how);
    const juce::String& message() const noexcept { return message_; }

    void select_node(const std::string& id);
    /// Selects an edge (index into graph().edges) and opens its inspector.
    void select_edge(int index);
    int selected_edge() const noexcept { return edge_; }
    /// Shows only what the selected node feeds and what feeds it.
    void set_isolate(bool on);
    void fit();
    void zoom_by(double factor, juce::Point<float> about);
    /// Highlights matches and centres the first; returns how many.
    int search(const juce::String& text);

    /// Where things are drawn (component coordinates), for hit tests and checks.
    juce::Rectangle<float> node_bounds(const std::string& id) const;
    juce::Point<float> output_port(const std::string& id) const;
    juce::Point<float> edge_point(int index, float t = 0.5f) const;
    juce::Rectangle<float> insert_bounds(const std::string& id, int slot) const;
    bool node_shown(const std::string& id) const;

    // The inspector's controls (for checks).
    juce::Slider& level_slider() { return level_; }
    juce::ToggleButton& pre_toggle() { return pre_; }
    juce::ToggleButton& mute_toggle() { return mute_; }
    juce::TextButton& remove_button() { return remove_; }

    void paint(juce::Graphics& g) override;
    void paintOverChildren(juce::Graphics& g) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;
    void mouseMove(const juce::MouseEvent& e) override;
    void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override;

private:
    void timerCallback() override;
    juce::Rectangle<float> world_node(const project::RouteNode& n) const;
    juce::Point<float> to_view(juce::Point<float> world) const;
    juce::Point<float> to_world(juce::Point<float> view) const;
    juce::Path edge_path(const project::RouteEdge& e) const;
    const project::RouteNode* node_at(juce::Point<float> view) const;
    int edge_at(juce::Point<float> view) const;
    bool port_hit(const project::RouteNode& n, juce::Point<float> view) const;
    std::set<std::string> traced() const;
    void show_inspector();
    void say(const juce::String& text);
    juce::Rectangle<float> minimap_area() const;
    juce::Rectangle<float> world_extent() const;

    AppContext& context_;
    project::RoutingGraph graph_;
    std::string signature_;
    std::string node_;  ///< Selected node id.
    int edge_ = -1;     ///< Selected edge.
    bool isolate_ = false;
    std::set<std::string> matches_;
    double zoom_ = 1.0;
    juce::Point<float> pan_{0.0f, 0.0f};
    juce::String message_;
    juce::uint32 message_at_ = 0;
    std::map<std::string, float> levels_;  ///< Meters, decaying.

    enum class Drag { None, Pan, Wire, Insert } drag_ = Drag::None;
    std::string drag_from_;
    int drag_slot_ = -1;
    juce::Point<float> drag_at_, pan_start_;

    // Toolbar and inspector.
    juce::TextEditor search_;
    hw::Keycap fit_{"Fit"}, isolate_key_{"Isolate", true}, zoom_in_{"+"}, zoom_out_{"-"};
    juce::Component inspector_;
    juce::Label title_;
    juce::Slider level_{juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight}, pan_slider_{juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight};
    juce::ToggleButton pre_{"Pre-fader"}, mute_{"Mute"};
    juce::TextButton remove_{"Remove"};
    bool loading_inspector_ = false;
};

/// Opens the routing window (or brings it to the front).
void show_routing(AppContext& context);
/// The open canvas, or null (for checks).
RoutingCanvas* routing_canvas();

}  // namespace vcc::desktop::console
