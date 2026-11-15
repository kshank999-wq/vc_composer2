#include "vcc/project/Routing.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>

namespace vcc::project {

namespace {

const std::string kMaster = "bus-master";
const std::string kHardware = "hw-out";

std::string insert_name(const InsertSlot& s)
{
    if (s.kind == "plugin") return s.plugin.name.empty() ? std::string("Plug-in") : s.plugin.name;
    if (s.kind == "eq") return "EQ";
    std::string n = s.kind;
    if (!n.empty()) n[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(n[0])));
    return n;
}

bool carries_audio(const Track& t)
{
    return t.kind != TrackKind::Folder && t.kind != TrackKind::Video;
}

/// Where a bus id leads: the bus, or the master for "" or an unknown id.
std::string resolve(const ProjectData& d, const std::string& id)
{
    if (id.empty() || id == "master") return kMaster;
    return d.find_bus(id) ? id : kMaster;
}

/// The buses a source feeds directly (its output and its sends).
std::vector<std::string> feeds(const ProjectData& d, const std::string& id)
{
    std::vector<std::string> out;
    if (const Track* t = d.find_track(id)) {
        out.push_back(resolve(d, t->output_bus_id));
        for (const auto& s : t->sends)
            if (d.find_bus(s.bus_id)) out.push_back(s.bus_id);
    } else if (const Bus* b = d.find_bus(id)) {
        if (b->id == kMaster) return out;
        out.push_back(resolve(d, b->output));
        for (const auto& s : b->sends)
            if (d.find_bus(s.bus_id)) out.push_back(s.bus_id);
    }
    return out;
}

}  // namespace

const RouteNode* RoutingGraph::node(const std::string& id) const noexcept
{
    for (const auto& n : nodes)
        if (n.id == id) return &n;
    return nullptr;
}

std::vector<std::string> RoutingGraph::downstream(const std::string& id) const
{
    std::set<std::string> seen;
    std::vector<std::string> stack{id}, out;
    while (!stack.empty()) {
        const auto x = stack.back();
        stack.pop_back();
        for (const auto& e : edges)
            if (e.from == x && seen.insert(e.to).second) {
                out.push_back(e.to);
                stack.push_back(e.to);
            }
    }
    return out;
}

std::vector<std::string> RoutingGraph::upstream(const std::string& id) const
{
    std::set<std::string> seen;
    std::vector<std::string> stack{id}, out;
    while (!stack.empty()) {
        const auto x = stack.back();
        stack.pop_back();
        for (const auto& e : edges)
            if (e.to == x && seen.insert(e.from).second) {
                out.push_back(e.from);
                stack.push_back(e.from);
            }
    }
    return out;
}

bool would_loop(const ProjectData& d, const std::string& from, const std::string& to, std::string* why)
{
    if (from == to) {
        if (why) *why = "a bus cannot feed itself";
        return true;
    }
    if (d.find_track(from)) return false;  // nothing feeds a track
    // Does `to` already reach `from`? Then from -> to closes a loop.
    std::map<std::string, std::string> parent;
    std::vector<std::string> stack{to};
    std::set<std::string> seen{to};
    while (!stack.empty()) {
        const auto x = stack.back();
        stack.pop_back();
        for (const auto& y : feeds(d, x)) {
            if (!seen.insert(y).second) continue;
            parent[y] = x;
            if (y == from) {
                if (why) {
                    auto name = [&](const std::string& id) {
                        const Bus* b = d.find_bus(id);
                        return b ? b->name : id;
                    };
                    // from -> to -> ... -> from
                    std::vector<std::string> path{from};
                    for (std::string at = from; at != to;) {
                        at = parent[at];
                        path.push_back(at);
                    }
                    std::reverse(path.begin(), path.end());
                    std::string text = name(from);
                    for (const auto& p : path) text += " -> " + name(p);
                    *why = "it would loop: " + text;
                }
                return true;
            }
            stack.push_back(y);
        }
    }
    return false;
}

RoutingGraph routing_graph(const ProjectData& d)
{
    RoutingGraph g;
    auto inserts_of = [](const std::vector<InsertSlot>& slots) {
        std::vector<RouteNode::Insert> out;
        for (const auto& s : slots)
            if (!s.kind.empty()) out.push_back({insert_name(s), s.bypass});
        return out;
    };
    for (const auto& t : d.tracks) {
        if (!carries_audio(t)) continue;
        RouteNode n;
        n.kind = RouteNode::Kind::Track;
        n.id = t.id;
        n.name = t.name;
        n.stereo = t.kind == TrackKind::Audio ? t.input_stereo : true;
        n.mute = t.mute;
        n.gain_db = t.gain_db;
        n.pan = t.pan;
        n.inserts = inserts_of(t.inserts);
        g.nodes.push_back(n);
        g.edges.push_back({RouteEdge::Kind::Output, t.id, resolve(d, t.output_bus_id), -1, t.gain_db, false, t.mute});
        for (std::size_t i = 0; i < t.sends.size(); ++i) {
            const auto& s = t.sends[i];
            if (!d.find_bus(s.bus_id)) continue;
            g.edges.push_back({RouteEdge::Kind::Send, t.id, s.bus_id, static_cast<int>(i), s.level_db, s.pre_fader, s.mute});
        }
    }
    for (const auto& b : d.buses) {
        RouteNode n;
        n.kind = b.id == kMaster ? RouteNode::Kind::Master : RouteNode::Kind::Bus;
        n.id = b.id;
        n.name = b.name;
        n.bus_kind = b.kind;
        n.mute = b.mute;
        n.gain_db = b.gain_db;
        n.pan = b.pan;
        n.inserts = inserts_of(b.inserts);
        g.nodes.push_back(n);
        if (b.id == kMaster) {
            g.edges.push_back({RouteEdge::Kind::Output, b.id, kHardware, -1, b.gain_db, false, b.mute});
            continue;
        }
        g.edges.push_back({RouteEdge::Kind::Output, b.id, resolve(d, b.output), -1, b.gain_db, false, b.mute});
        for (std::size_t i = 0; i < b.sends.size(); ++i) {
            const auto& s = b.sends[i];
            if (!d.find_bus(s.bus_id)) continue;
            g.edges.push_back({RouteEdge::Kind::Send, b.id, s.bus_id, static_cast<int>(i), s.level_db, s.pre_fader, s.mute});
        }
    }
    RouteNode hw;
    hw.kind = RouteNode::Kind::Hardware;
    hw.id = kHardware;
    hw.name = "Hardware out";
    g.nodes.push_back(hw);

    // Columns: tracks first; each bus one past the deepest thing feeding it
    // (bounded, so a loop in old data cannot spin); master, then hardware.
    std::map<std::string, int> depth;
    for (const auto& n : g.nodes)
        if (n.kind == RouteNode::Kind::Track) depth[n.id] = 0;
    for (std::size_t pass = 0; pass < g.nodes.size() + 1; ++pass) {
        bool changed = false;
        for (const auto& e : g.edges) {
            const auto* to = g.node(e.to);
            if (!to || to->kind != RouteNode::Kind::Bus) continue;
            const auto it = depth.find(e.from);
            const int want = (it == depth.end() ? 0 : it->second) + 1;
            if (depth[e.to] < want && want < static_cast<int>(g.nodes.size()) + 2) {
                depth[e.to] = want;
                changed = true;
            }
        }
        if (!changed) break;
    }
    int deepest = 0;
    for (auto& n : g.nodes)
        if (n.kind == RouteNode::Kind::Bus) {
            n.column = std::max(1, depth[n.id]);
            deepest = std::max(deepest, n.column);
        }
    for (auto& n : g.nodes) {
        if (n.kind == RouteNode::Kind::Master) n.column = deepest + 1;
        if (n.kind == RouteNode::Kind::Hardware) n.column = deepest + 2;
    }
    g.columns = deepest + 3;
    std::map<int, int> rows;
    for (auto& n : g.nodes) n.row = rows[n.column]++;
    return g;
}

}  // namespace vcc::project
