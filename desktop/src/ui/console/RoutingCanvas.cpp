#include "ui/console/RoutingCanvas.h"

#include <cmath>

#include "ui/Dialogs.h"
#include "ui/hw/HwTheme.h"

namespace vcc::desktop::console {

namespace {

using Kind = project::RouteNode::Kind;
constexpr float kNodeW = 178.0f, kHead = 40.0f, kInsertH = 15.0f, kColGap = 250.0f, kRowGap = 22.0f;
constexpr int kToolbar = 40;

juce::Colour kind_colour(const project::RouteNode& n)
{
    switch (n.kind) {
        case Kind::Track: return juce::Colour(0xff4f8fbf);
        case Kind::Bus: return n.bus_kind == "fx" ? juce::Colour(0xffb07ad8) : juce::Colour(0xff5fb08a);
        case Kind::Master: return juce::Colour(0xffe0a040);
        case Kind::Hardware: return juce::Colour(0xff9aa3ad);
    }
    return juce::Colours::grey;
}

juce::Component::SafePointer<dialogs::ToolWindow>& window_slot()
{
    static juce::Component::SafePointer<dialogs::ToolWindow> slot;
    return slot;
}
RoutingCanvas*& canvas_slot()
{
    static RoutingCanvas* c = nullptr;
    return c;
}

juce::String db(double v) { return (v > 0 ? "+" : "") + juce::String(v, 1) + " dB"; }

}  // namespace

RoutingCanvas::RoutingCanvas(AppContext& context) : context_(context)
{
    setComponentID("routing_canvas");
    setTitle("Visual routing");
    setWantsKeyboardFocus(true);
    canvas_slot() = this;
    search_.setTextToShowWhenEmpty("Search tracks and buses", hw::tok::text.withAlpha(0.5f));
    search_.onReturnKey = [this] { search(search_.getText()); };
    search_.onTextChange = [this] { search(search_.getText()); };
    fit_.onClick = [this] { fit(); };
    isolate_key_.set_led_dot(true);
    isolate_key_.setTooltip("Show only what the selected node feeds and what feeds it");
    isolate_key_.onClick = [this] { set_isolate(isolate_key_.getToggleState()); };
    zoom_in_.onClick = [this] { zoom_by(1.2, getLocalBounds().getCentre().toFloat()); };
    zoom_out_.onClick = [this] { zoom_by(1.0 / 1.2, getLocalBounds().getCentre().toFloat()); };
    for (auto* c : std::initializer_list<juce::Component*>{&search_, &fit_, &isolate_key_, &zoom_in_, &zoom_out_}) addAndMakeVisible(*c);

    // The inspector: what the selected line carries.
    title_.setColour(juce::Label::textColourId, hw::tok::text_bright);
    level_.setRange(-60.0, 12.0, 0.1);
    level_.setTextValueSuffix(" dB");
    pan_slider_.setRange(-1.0, 1.0, 0.01);
    pan_slider_.textFromValueFunction = [](double v) {
        const int p = juce::roundToInt(std::abs(v) * 100.0);
        return p == 0 ? juce::String("Pan C") : juce::String(v < 0 ? "Pan L" : "Pan R") + juce::String(p);
    };
    level_.setTooltip("Level: the send's level, or for an output the source's fader");
    pan_slider_.setTooltip("Pan: the send's pan, or for an output the source's pan");
    level_.setComponentID("routing_level");
    pre_.setComponentID("routing_pre");
    mute_.setComponentID("routing_mute");
    remove_.setComponentID("routing_remove");
    level_.onValueChange = [this] {
        if (loading_inspector_ || edge_ < 0 || edge_ >= static_cast<int>(graph_.edges.size())) return;
        const auto e = graph_.edges[static_cast<std::size_t>(edge_)];
        const double v = level_.getValue();
        if (e.kind == project::RouteEdge::Kind::Send)
            context_.session.update_send(e.from, e.send_index, "Send level " + e.from, [v](project::Send& s) { s.level_db = v; });
        else if (context_.session.data().find_track(e.from))
            context_.session.update_track_mix(e.from, "Volume " + e.from, [v](project::Track& t) { t.gain_db = v; });
        else
            context_.session.set_bus_gain(e.from, v);
    };
    pan_slider_.onValueChange = [this] {
        if (loading_inspector_ || edge_ < 0 || edge_ >= static_cast<int>(graph_.edges.size())) return;
        const auto e = graph_.edges[static_cast<std::size_t>(edge_)];
        const double v = pan_slider_.getValue();
        if (e.kind == project::RouteEdge::Kind::Send)
            context_.session.update_send(e.from, e.send_index, "Send pan " + e.from, [v](project::Send& s) {
                s.pan = v;
                s.pan_follow = false;
            });
        else if (context_.session.data().find_track(e.from))
            context_.session.update_track_mix(e.from, "Pan " + e.from, [v](project::Track& t) { t.pan = v; });
        else
            context_.session.update_bus(e.from, "Pan " + e.from, [v](project::Bus& b) { b.pan = v; });
    };
    pre_.onClick = [this] {
        if (loading_inspector_ || edge_ < 0) return;
        const auto e = graph_.edges[static_cast<std::size_t>(edge_)];
        const bool on = pre_.getToggleState();
        if (e.kind == project::RouteEdge::Kind::Send)
            context_.session.update_send(e.from, e.send_index, std::string(on ? "Pre-fader send" : "Post-fader send"),
                                         [on](project::Send& s) { s.pre_fader = on; });
    };
    mute_.onClick = [this] {
        if (loading_inspector_ || edge_ < 0) return;
        const auto e = graph_.edges[static_cast<std::size_t>(edge_)];
        const bool on = mute_.getToggleState();
        if (e.kind == project::RouteEdge::Kind::Send)
            context_.session.update_send(e.from, e.send_index, std::string(on ? "Mute send" : "Unmute send"),
                                         [on](project::Send& s) { s.mute = on; });
        else if (context_.session.data().find_track(e.from))
            context_.session.set_mute(e.from, on);
        else
            context_.session.update_bus(e.from, on ? "Mute bus" : "Unmute bus", [on](project::Bus& b) { b.mute = on; });
    };
    remove_.onClick = [this] {
        if (edge_ < 0 || edge_ >= static_cast<int>(graph_.edges.size())) return;
        const auto e = graph_.edges[static_cast<std::size_t>(edge_)];
        if (e.kind == project::RouteEdge::Kind::Send) {
            context_.session.remove_send(e.from, e.send_index);
            say("Send removed");
        } else if (e.to != "bus-master" && e.to != "hw-out") {
            context_.session.route_output(e.from, "bus-master");
            say("Output disconnected: back to the master");
        }
        edge_ = -1;
        refresh();
    };
    for (auto* c : std::initializer_list<juce::Component*>{&title_, &level_, &pan_slider_, &pre_, &mute_, &remove_})
        inspector_.addAndMakeVisible(*c);
    addChildComponent(inspector_);
    refresh();
    fit();
    startTimerHz(15);
}

RoutingCanvas::~RoutingCanvas()
{
    stopTimer();
    if (canvas_slot() == this) canvas_slot() = nullptr;
}

void RoutingCanvas::refresh()
{
    auto g = project::routing_graph(context_.session.data());
    std::string sig;
    for (const auto& n : g.nodes) {
        sig += n.id + "|" + n.name + "|" + std::to_string(n.column) + std::to_string(n.row) + (n.mute ? "m" : "") + ";";
        for (const auto& i : n.inserts) sig += i.name + (i.bypass ? "b" : "") + ",";
    }
    for (const auto& e : g.edges)
        sig += e.from + ">" + e.to + std::to_string(static_cast<int>(e.kind)) + std::to_string(e.send_index) +
               juce::String(e.level_db, 1).toStdString() + (e.pre_fader ? "p" : "") + (e.mute ? "m" : "") + ";";
    if (sig == signature_) return;
    signature_ = sig;
    graph_ = std::move(g);
    if (!node_.empty() && !graph_.node(node_)) node_.clear();
    if (edge_ >= static_cast<int>(graph_.edges.size())) edge_ = -1;
    show_inspector();
    repaint();
}

void RoutingCanvas::timerCallback()
{
    refresh();
    // Meters: each node's level, falling back slowly.
    bool moving = false;
    for (const auto& n : graph_.nodes) {
        if (n.kind == Kind::Hardware) continue;
        const auto m = context_.engine.take_deck_meter(n.id, 3);
        float& v = levels_[n.id];
        const float peak = std::max(m.left, m.right);
        const float next = std::max(peak, v * 0.85f);
        moving = moving || std::abs(next - v) > 0.001f;
        v = next;
    }
    if (message_.isNotEmpty() && juce::Time::getMillisecondCounter() - message_at_ > 6000) {
        message_.clear();
        moving = true;
    }
    if (moving) repaint();
}

void RoutingCanvas::say(const juce::String& text)
{
    message_ = text;
    message_at_ = juce::Time::getMillisecondCounter();
    if (context_.status) context_.status(text);
    repaint();
}

bool RoutingCanvas::connect(const std::string& from, const std::string& to, int how)
{
    std::string why;
    bool ok = false;
    const auto* src = graph_.node(from);
    const auto* dst = graph_.node(to);
    const juce::String a = src ? juce::String(src->name) : juce::String(from), b = dst ? juce::String(dst->name) : juce::String(to);
    if (how == 0) ok = context_.session.route_output(from, to, &why);
    else ok = context_.session.add_send(from, to, how == 2, -12.0, &why);
    refresh();
    if (ok) say(how == 0 ? a + " now goes to " + b : a + " sends to " + b + (how == 2 ? " (pre-fader)" : " (post-fader)"));
    else say("Not connected: " + juce::String(why));
    return ok;
}

void RoutingCanvas::select_node(const std::string& id)
{
    node_ = id;
    edge_ = -1;
    show_inspector();
    repaint();
}

void RoutingCanvas::select_edge(int index)
{
    edge_ = index >= 0 && index < static_cast<int>(graph_.edges.size()) ? index : -1;
    show_inspector();
    repaint();
}

void RoutingCanvas::set_isolate(bool on)
{
    isolate_ = on;
    isolate_key_.setToggleState(on, juce::dontSendNotification);
    repaint();
}

int RoutingCanvas::search(const juce::String& text)
{
    matches_.clear();
    const auto t = text.trim().toLowerCase();
    if (t.isNotEmpty())
        for (const auto& n : graph_.nodes)
            if (juce::String(n.name).toLowerCase().contains(t)) matches_.insert(n.id);
    if (!matches_.empty()) {
        // The first match in the middle of the view.
        for (const auto& n : graph_.nodes)
            if (matches_.count(n.id)) {
                const auto c = world_node(n).getCentre();
                const auto view = getLocalBounds().withTrimmedTop(kToolbar).toFloat().getCentre();
                pan_ = {view.x - static_cast<float>(c.x * zoom_), view.y - static_cast<float>(c.y * zoom_)};
                break;
            }
    }
    repaint();
    return static_cast<int>(matches_.size());
}

juce::Rectangle<float> RoutingCanvas::world_node(const project::RouteNode& n) const
{
    const float h = kHead + kInsertH * static_cast<float>(n.inserts.size()) + (n.inserts.empty() ? 0.0f : 6.0f);
    // Rows stack by the heights above them in the column.
    float y = 0.0f;
    for (const auto& m : graph_.nodes)
        if (m.column == n.column && m.row < n.row)
            y += kHead + kInsertH * static_cast<float>(m.inserts.size()) + (m.inserts.empty() ? 0.0f : 6.0f) + kRowGap;
    return {static_cast<float>(n.column) * kColGap, y, kNodeW, h};
}

juce::Point<float> RoutingCanvas::to_view(juce::Point<float> w) const
{
    return {pan_.x + w.x * static_cast<float>(zoom_), pan_.y + w.y * static_cast<float>(zoom_)};
}

juce::Point<float> RoutingCanvas::to_world(juce::Point<float> v) const
{
    return {(v.x - pan_.x) / static_cast<float>(zoom_), (v.y - pan_.y) / static_cast<float>(zoom_)};
}

juce::Rectangle<float> RoutingCanvas::node_bounds(const std::string& id) const
{
    const auto* n = graph_.node(id);
    if (!n) return {};
    const auto w = world_node(*n);
    const auto a = to_view(w.getTopLeft()), b = to_view(w.getBottomRight());
    return {a, b};
}

juce::Point<float> RoutingCanvas::output_port(const std::string& id) const
{
    const auto r = node_bounds(id);
    return {r.getRight(), r.getY() + static_cast<float>(20.0 * zoom_)};
}

juce::Rectangle<float> RoutingCanvas::insert_bounds(const std::string& id, int slot) const
{
    const auto r = node_bounds(id);
    const float z = static_cast<float>(zoom_);
    return {r.getX() + 8 * z, r.getY() + (kHead + kInsertH * static_cast<float>(slot)) * z, r.getWidth() - 16 * z, (kInsertH - 2) * z};
}

std::set<std::string> RoutingCanvas::traced() const
{
    std::set<std::string> out;
    if (node_.empty()) return out;
    out.insert(node_);
    for (const auto& id : graph_.upstream(node_)) out.insert(id);
    for (const auto& id : graph_.downstream(node_)) out.insert(id);
    return out;
}

bool RoutingCanvas::node_shown(const std::string& id) const
{
    if (!isolate_ || node_.empty()) return graph_.node(id) != nullptr;
    return traced().count(id) > 0;
}

juce::Path RoutingCanvas::edge_path(const project::RouteEdge& e) const
{
    juce::Path p;
    const auto* a = graph_.node(e.from);
    const auto* b = graph_.node(e.to);
    if (!a || !b) return p;
    const auto from = output_port(e.from);
    const auto to_r = node_bounds(e.to);
    // Sends land lower on the input side than outputs, so they read apart.
    const float in_y = to_r.getY() + static_cast<float>((e.kind == project::RouteEdge::Kind::Send ? 30.0 : 20.0) * zoom_);
    const juce::Point<float> to{to_r.getX(), in_y};
    p.startNewSubPath(from);
    if (b->column - a->column > 1) {
        // Skipping columns: around the nodes in between, not behind them
        // (a line hidden under a bus would read as feeding it).
        float lane = std::max(from.y, to.y);
        for (const auto& n : graph_.nodes)
            if (n.column > a->column && n.column < b->column && node_shown(n.id))
                lane = std::max(lane, node_bounds(n.id).getBottom() + static_cast<float>(14.0 * zoom_));
        const float bend = static_cast<float>(40.0 * zoom_);
        const juce::Point<float> down{from.x + bend * 1.5f, lane}, up{to.x - bend * 1.5f, lane};
        p.cubicTo(from.translated(bend, 0), {from.x + bend * 0.5f, lane}, down);
        p.lineTo(up);
        p.cubicTo({to.x - bend * 0.5f, lane}, to.translated(-bend, 0), to);
        return p;
    }
    const float dx = std::max(40.0f, (to.x - from.x) * 0.5f);
    p.cubicTo(from.translated(dx, 0), to.translated(-dx, 0), to);
    return p;
}

juce::Point<float> RoutingCanvas::edge_point(int index, float t) const
{
    if (index < 0 || index >= static_cast<int>(graph_.edges.size())) return {};
    const auto p = edge_path(graph_.edges[static_cast<std::size_t>(index)]);
    return p.getPointAlongPath(p.getLength() * t);
}

const project::RouteNode* RoutingCanvas::node_at(juce::Point<float> v) const
{
    for (const auto& n : graph_.nodes)
        if (node_shown(n.id) && node_bounds(n.id).contains(v)) return &n;
    return nullptr;
}

bool RoutingCanvas::port_hit(const project::RouteNode& n, juce::Point<float> v) const
{
    if (n.kind == Kind::Hardware || n.kind == Kind::Master) return false;
    return output_port(n.id).getDistanceFrom(v) < 10.0f;
}

int RoutingCanvas::edge_at(juce::Point<float> v) const
{
    int best = -1;
    float best_d = 7.0f;
    for (std::size_t i = 0; i < graph_.edges.size(); ++i) {
        const auto& e = graph_.edges[i];
        if (!node_shown(e.from) || !node_shown(e.to)) continue;
        const auto p = edge_path(e);
        juce::Point<float> nearest;
        p.getNearestPoint(v, nearest);
        const float d = nearest.getDistanceFrom(v);
        if (d < best_d) {
            best_d = d;
            best = static_cast<int>(i);
        }
    }
    return best;
}

juce::Rectangle<float> RoutingCanvas::world_extent() const
{
    juce::Rectangle<float> r;
    for (const auto& n : graph_.nodes) r = r.isEmpty() ? world_node(n) : r.getUnion(world_node(n));
    return r.expanded(30.0f);
}

void RoutingCanvas::fit()
{
    const auto ext = world_extent();
    auto view = getLocalBounds().withTrimmedTop(kToolbar).toFloat().reduced(10.0f);
    if (inspector_.isVisible()) view.removeFromRight(static_cast<float>(inspector_.getWidth()));
    if (ext.isEmpty() || view.isEmpty()) return;
    zoom_ = juce::jlimit(0.35, 1.6, static_cast<double>(std::min(view.getWidth() / ext.getWidth(), view.getHeight() / ext.getHeight())));
    pan_ = {view.getX() - ext.getX() * static_cast<float>(zoom_) + (view.getWidth() - ext.getWidth() * static_cast<float>(zoom_)) / 2,
            view.getY() - ext.getY() * static_cast<float>(zoom_) + (view.getHeight() - ext.getHeight() * static_cast<float>(zoom_)) / 2};
    repaint();
}

void RoutingCanvas::zoom_by(double factor, juce::Point<float> about)
{
    const auto w = to_world(about);
    zoom_ = juce::jlimit(0.25, 3.0, zoom_ * factor);
    pan_ = {about.x - w.x * static_cast<float>(zoom_), about.y - w.y * static_cast<float>(zoom_)};
    repaint();
}

void RoutingCanvas::show_inspector()
{
    const bool on = edge_ >= 0 && edge_ < static_cast<int>(graph_.edges.size());
    inspector_.setVisible(on);
    if (!on) return;
    const auto& e = graph_.edges[static_cast<std::size_t>(edge_)];
    const auto* a = graph_.node(e.from);
    const auto* b = graph_.node(e.to);
    loading_inspector_ = true;
    const bool send = e.kind == project::RouteEdge::Kind::Send;
    title_.setText((send ? "Send: " : "Output: ") + juce::String(a ? a->name : e.from) + " -> " + juce::String(b ? b->name : e.to),
                   juce::dontSendNotification);
    level_.setValue(send ? e.level_db : (a ? a->gain_db : 0.0), juce::dontSendNotification);
    double pan = a ? a->pan : 0.0;
    if (send)
        if (const auto* t = context_.session.data().find_track(e.from); t && e.send_index < static_cast<int>(t->sends.size()))
            pan = t->sends[static_cast<std::size_t>(e.send_index)].pan;
    pan_slider_.setValue(pan, juce::dontSendNotification);
    pre_.setToggleState(e.pre_fader, juce::dontSendNotification);
    pre_.setEnabled(send);
    mute_.setToggleState(send ? e.mute : (a && a->mute), juce::dontSendNotification);
    remove_.setButtonText(send ? "Remove send" : "Disconnect");
    remove_.setEnabled(send || (e.to != "bus-master" && e.to != "hw-out"));
    loading_inspector_ = false;
    resized();
}

void RoutingCanvas::resized()
{
    auto top = getLocalBounds().removeFromTop(kToolbar).reduced(8, 7);
    search_.setBounds(top.removeFromLeft(220));
    top.removeFromLeft(8);
    fit_.setBounds(top.removeFromLeft(48));
    top.removeFromLeft(4);
    zoom_out_.setBounds(top.removeFromLeft(30));
    top.removeFromLeft(4);
    zoom_in_.setBounds(top.removeFromLeft(30));
    top.removeFromLeft(8);
    isolate_key_.setBounds(top.removeFromLeft(78));
    auto r = getLocalBounds().withTrimmedTop(kToolbar);
    auto ins = r.removeFromRight(250).reduced(8).withHeight(190);
    inspector_.setBounds(ins);
    auto c = inspector_.getLocalBounds().reduced(8);
    title_.setBounds(c.removeFromTop(22));
    c.removeFromTop(4);
    level_.setBounds(c.removeFromTop(26));
    c.removeFromTop(4);
    pan_slider_.setBounds(c.removeFromTop(26));
    c.removeFromTop(6);
    auto row = c.removeFromTop(24);
    pre_.setBounds(row.removeFromLeft(110));
    mute_.setBounds(row);
    c.removeFromTop(8);
    remove_.setBounds(c.removeFromTop(26));
}

void RoutingCanvas::paint(juce::Graphics& g)
{
    const auto& theme = hw::Theme::get();
    g.fillAll(juce::Colour(0xff16191c));
    // A faint grid.
    g.setColour(juce::Colour(0xff1f2327));
    const float step = static_cast<float>(40.0 * zoom_);
    for (float x = std::fmod(pan_.x, step); x < static_cast<float>(getWidth()); x += step)
        g.drawVerticalLine(juce::roundToInt(x), static_cast<float>(kToolbar), static_cast<float>(getHeight()));
    for (float y = std::fmod(pan_.y, step); y < static_cast<float>(getHeight()); y += step)
        g.drawHorizontalLine(juce::roundToInt(y), 0.0f, static_cast<float>(getWidth()));

    const auto trace = traced();
    const float z = static_cast<float>(zoom_);
    // Lines.
    for (std::size_t i = 0; i < graph_.edges.size(); ++i) {
        const auto& e = graph_.edges[i];
        if (!node_shown(e.from) || !node_shown(e.to)) continue;
        const auto path = edge_path(e);
        const bool send = e.kind == project::RouteEdge::Kind::Send;
        const bool on_trace = trace.empty() || (trace.count(e.from) && trace.count(e.to));
        juce::Colour c = send ? juce::Colour(0xffe0a040) : juce::Colour(0xff7fb3d8);
        if (e.mute) c = c.withAlpha(0.35f);
        if (!on_trace) c = c.withAlpha(0.15f);
        if (static_cast<int>(i) == edge_) c = theme.lcd();
        g.setColour(c);
        const float width = (static_cast<int>(i) == edge_ ? 3.0f : 2.0f) * std::max(0.6f, z);
        if (send) {
            juce::Path dashed;
            const float dashes[] = {7.0f * z, 5.0f * z};
            juce::PathStrokeType(width).createDashedStroke(dashed, path, dashes, 2);
            g.fillPath(dashed);
            // Level and pre/post at the middle.
            const auto mid = path.getPointAlongPath(path.getLength() * 0.5f);
            const juce::String text = db(e.level_db) + (e.pre_fader ? " pre" : " post") + (e.mute ? " (muted)" : "");
            g.setFont(hw::fonts::ui(10.5f * std::max(0.8f, z), true));
            const float tw = juce::GlyphArrangement::getStringWidth(g.getCurrentFont(), text) + 10.0f;
            const auto box = juce::Rectangle<float>(mid.x - tw / 2, mid.y - 9, tw, 16);
            g.setColour(juce::Colour(0xff16191c).withAlpha(0.9f));
            g.fillRoundedRectangle(box, 4.0f);
            g.setColour(c);
            g.drawText(text, box, juce::Justification::centred, false);
        } else {
            g.strokePath(path, juce::PathStrokeType(width));
        }
    }
    // Nodes.
    for (const auto& n : graph_.nodes) {
        if (!node_shown(n.id)) continue;
        const auto r = node_bounds(n.id);
        const bool dim = !trace.empty() && !trace.count(n.id);
        const auto col = kind_colour(n).withMultipliedAlpha(dim ? 0.35f : 1.0f);
        g.setColour(juce::Colour(0xff23282d).withMultipliedAlpha(dim ? 0.6f : 1.0f));
        g.fillRoundedRectangle(r, 6.0f * z);
        g.setColour(col);
        g.fillRoundedRectangle(r.withHeight(5.0f * z), 2.0f);
        const bool selected = n.id == node_;
        g.setColour(selected ? theme.lcd() : matches_.count(n.id) ? juce::Colour(0xffe0a040) : col.withAlpha(0.6f));
        g.drawRoundedRectangle(r, 6.0f * z, selected || matches_.count(n.id) ? 2.2f : 1.0f);
        // Name, kind and format.
        g.setColour(hw::tok::text_bright.withMultipliedAlpha(dim ? 0.5f : 1.0f));
        g.setFont(hw::fonts::ui(13.0f * z, true));
        g.drawText(n.name, r.withTrimmedLeft(16 * z).withTrimmedTop(7 * z).withHeight(16 * z), juce::Justification::centredLeft, true);
        juce::String sub = n.kind == Kind::Track ? "Track" : n.kind == Kind::Bus ? (n.bus_kind == "fx" ? "FX / aux" : "Group bus")
                         : n.kind == Kind::Master ? "Master" : "Device output";
        if (n.kind != Kind::Hardware) sub << "  -  " << (n.stereo ? "stereo" : "mono") << "  -  " << db(n.gain_db);
        if (n.mute) sub << "  -  MUTED";
        g.setColour(hw::tok::text.withMultipliedAlpha(dim ? 0.5f : 1.0f));
        g.setFont(hw::fonts::ui(9.5f * z, false));
        g.drawText(sub, r.withTrimmedLeft(16 * z).withTrimmedTop(23 * z).withHeight(13 * z), juce::Justification::centredLeft, true);
        // Meter down the left edge.
        const float level = levels_.count(n.id) ? levels_.at(n.id) : 0.0f;
        const float lh = (r.getHeight() - 12 * z) * juce::jlimit(0.0f, 1.0f, std::sqrt(level));
        g.setColour(juce::Colour(0xff111315));
        g.fillRect(juce::Rectangle<float>(r.getX() + 5 * z, r.getY() + 8 * z, 5 * z, r.getHeight() - 12 * z));
        g.setColour(level > 0.89f ? juce::Colours::red : juce::Colour(0xff58c46a));
        g.fillRect(juce::Rectangle<float>(r.getX() + 5 * z, r.getBottom() - 4 * z - lh, 5 * z, lh));
        // Inserts, in order.
        for (std::size_t k = 0; k < n.inserts.size(); ++k) {
            auto ir = insert_bounds(n.id, static_cast<int>(k));
            if (drag_ == Drag::Insert && drag_from_ == n.id && static_cast<int>(k) == drag_slot_) ir = ir.withY(drag_at_.y - ir.getHeight() / 2);
            g.setColour(juce::Colour(0xff2f353b));
            g.fillRoundedRectangle(ir, 3.0f);
            g.setColour(n.inserts[k].bypass ? hw::tok::text.withAlpha(0.4f) : hw::tok::text_bright);
            g.setFont(hw::fonts::ui(10.0f * z, false));
            g.drawText(juce::String(static_cast<int>(k) + 1) + "  " + juce::String(n.inserts[k].name) + (n.inserts[k].bypass ? " (bypassed)" : ""),
                       ir.reduced(5 * z, 0), juce::Justification::centredLeft, true);
        }
        // Ports.
        if (n.kind != Kind::Track) {
            g.setColour(col);
            g.fillEllipse(juce::Rectangle<float>(9 * z, 9 * z).withCentre({r.getX(), r.getY() + 20 * z}));
        }
        if (n.kind == Kind::Track || n.kind == Kind::Bus) {
            g.setColour(col);
            g.fillEllipse(juce::Rectangle<float>(11 * z, 11 * z).withCentre(output_port(n.id)));
        }
    }
    // A wire being dragged.
    if (drag_ == Drag::Wire) {
        juce::Path p;
        const auto from = output_port(drag_from_);
        p.startNewSubPath(from);
        p.cubicTo(from.translated(60, 0), drag_at_.translated(-60, 0), drag_at_);
        g.setColour(theme.lcd());
        g.strokePath(p, juce::PathStrokeType(2.0f));
        g.setFont(hw::fonts::ui(11.0f, false));
        g.drawText("Drop on a bus: route the output  -  Shift: add a post-fader send  -  Alt: pre-fader",
                   juce::Rectangle<float>(drag_at_.x + 12, drag_at_.y + 6, 520, 16), juce::Justification::centredLeft, false);
    }
}

juce::Rectangle<float> RoutingCanvas::minimap_area() const
{
    auto r = getLocalBounds().toFloat().reduced(10);
    return r.removeFromBottom(110).removeFromLeft(180);
}

void RoutingCanvas::paintOverChildren(juce::Graphics& g)
{
    // Toolbar strip.
    g.setColour(hw::tok::panel_bottom);
    g.fillRect(getLocalBounds().removeFromTop(kToolbar).toFloat().withTrimmedLeft(isolate_key_.getRight() + 8.0f));
    // The mini-map: every node, and the part of the canvas in view.
    const auto mm = minimap_area();
    const auto ext = world_extent();
    if (!ext.isEmpty()) {
        g.setColour(juce::Colour(0xe0101214));
        g.fillRoundedRectangle(mm, 4.0f);
        const float s = std::min(mm.getWidth() / ext.getWidth(), mm.getHeight() / ext.getHeight());
        auto map = [&](juce::Point<float> w) { return juce::Point<float>(mm.getX() + (w.x - ext.getX()) * s, mm.getY() + (w.y - ext.getY()) * s); };
        for (const auto& n : graph_.nodes) {
            const auto w = world_node(n);
            g.setColour(kind_colour(n).withAlpha(node_shown(n.id) ? 0.9f : 0.25f));
            g.fillRect(juce::Rectangle<float>(map(w.getTopLeft()), map(w.getBottomRight())));
        }
        const auto view_tl = to_world({0.0f, static_cast<float>(kToolbar)});
        const auto view_br = to_world({static_cast<float>(getWidth()), static_cast<float>(getHeight())});
        g.setColour(hw::Theme::get().lcd());
        g.drawRect(juce::Rectangle<float>(map(view_tl), map(view_br)).getIntersection(mm), 1.2f);
    }
    // The last thing said (a refusal stays a while).
    if (message_.isNotEmpty()) {
        g.setFont(hw::fonts::ui(12.5f, true));
        const auto box = juce::Rectangle<float>(200, static_cast<float>(getHeight()) - 40.0f, static_cast<float>(getWidth()) - 420.0f, 28.0f);
        g.setColour(juce::Colour(0xf0202428));
        g.fillRoundedRectangle(box, 5.0f);
        g.setColour(message_.startsWith("Not") ? juce::Colour(0xffe06a4a) : hw::tok::text_bright);
        g.drawText(message_, box.reduced(10, 0), juce::Justification::centredLeft, true);
    }
}

void RoutingCanvas::mouseDown(const juce::MouseEvent& e)
{
    grabKeyboardFocus();
    drag_ = Drag::None;
    if (minimap_area().contains(e.position)) {
        const auto mm = minimap_area();
        const auto ext = world_extent();
        const float s = std::min(mm.getWidth() / ext.getWidth(), mm.getHeight() / ext.getHeight());
        const juce::Point<float> w{ext.getX() + (e.position.x - mm.getX()) / s, ext.getY() + (e.position.y - mm.getY()) / s};
        const auto view = getLocalBounds().withTrimmedTop(kToolbar).toFloat().getCentre();
        pan_ = {view.x - w.x * static_cast<float>(zoom_), view.y - w.y * static_cast<float>(zoom_)};
        repaint();
        return;
    }
    // Output ports first: they sit on the node's edge, half outside it.
    for (const auto& n : graph_.nodes)
        if (node_shown(n.id) && port_hit(n, e.position)) {
            drag_ = Drag::Wire;
            drag_from_ = n.id;
            drag_at_ = e.position;
            return;
        }
    if (const auto* n = node_at(e.position)) {
        for (std::size_t k = 0; k < n->inserts.size(); ++k)
            if (insert_bounds(n->id, static_cast<int>(k)).contains(e.position)) {
                drag_ = Drag::Insert;
                drag_from_ = n->id;
                drag_slot_ = static_cast<int>(k);
                drag_at_ = e.position;
                break;
            }
        select_node(n->id);
        return;
    }
    if (const int i = edge_at(e.position); i >= 0) {
        if (e.mods.isPopupMenu()) {
            const auto edge = graph_.edges[static_cast<std::size_t>(i)];
            juce::PopupMenu m;
            m.addItem(edge.kind == project::RouteEdge::Kind::Send ? "Remove send" : "Disconnect (back to the master)",
                      edge.kind == project::RouteEdge::Kind::Send || (edge.to != "bus-master" && edge.to != "hw-out"), false,
                      [this, i] {
                          select_edge(i);
                          remove_.triggerClick();
                      });
            m.showMenuAsync({});
            return;
        }
        select_edge(i);
        return;
    }
    // Empty canvas: pan, and the selection goes.
    node_.clear();
    edge_ = -1;
    show_inspector();
    drag_ = Drag::Pan;
    pan_start_ = pan_;
    repaint();
}

void RoutingCanvas::mouseDrag(const juce::MouseEvent& e)
{
    if (drag_ == Drag::Pan) {
        pan_ = pan_start_ + e.getOffsetFromDragStart().toFloat();
        repaint();
    } else if (drag_ == Drag::Wire || drag_ == Drag::Insert) {
        drag_at_ = e.position;
        repaint();
    }
}

void RoutingCanvas::mouseUp(const juce::MouseEvent& e)
{
    const auto was = drag_;
    drag_ = Drag::None;
    if (was == Drag::Wire) {
        const auto* to = node_at(e.position);
        if (to && to->id != drag_from_ && (to->kind == Kind::Bus || to->kind == Kind::Master))
            connect(drag_from_, to->id, e.mods.isShiftDown() ? 1 : e.mods.isAltDown() ? 2 : 0);
        else if (to)
            say("Not connected: drop on a bus or the master");
    } else if (was == Drag::Insert) {
        // The slot the insert was dropped on.
        const auto* n = graph_.node(drag_from_);
        if (n && !n->inserts.empty()) {
            const auto first = insert_bounds(n->id, 0);
            const int to = juce::jlimit(0, static_cast<int>(n->inserts.size()) - 1,
                                        static_cast<int>(std::floor((e.position.y - first.getY()) / (kInsertH * static_cast<float>(zoom_)))));
            if (to != drag_slot_ && context_.session.move_insert(drag_from_, drag_slot_, to)) say("Insert moved: the chain now runs in this order");
        }
    }
    repaint();
}

void RoutingCanvas::mouseMove(const juce::MouseEvent& e)
{
    bool on_port = false;
    for (const auto& n : graph_.nodes) on_port = on_port || (node_shown(n.id) && port_hit(n, e.position));
    if (on_port) setMouseCursor(juce::MouseCursor::CrosshairCursor);
    else if (edge_at(e.position) >= 0) setMouseCursor(juce::MouseCursor::PointingHandCursor);
    else setMouseCursor(juce::MouseCursor::NormalCursor);
}

void RoutingCanvas::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    if (e.mods.isCommandDown() || e.mods.isCtrlDown()) {
        zoom_by(w.deltaY > 0 ? 1.12 : 1.0 / 1.12, e.position);
        return;
    }
    pan_ += juce::Point<float>(w.deltaX, w.deltaY) * 200.0f;
    repaint();
}

void show_routing(AppContext& context)
{
    dialogs::show_tool(window_slot(), "Visual Routing", 1200, 720, [&context] { return new RoutingCanvas(context); });
}

RoutingCanvas* routing_canvas() { return canvas_slot(); }

}  // namespace vcc::desktop::console
