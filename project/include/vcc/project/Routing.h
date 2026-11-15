// The project's signal flow as a graph (Add-On Features spec §5): what the
// Visual Routing canvas draws and edits. It is read from the project every
// time - there is no second routing state - and the session's routing
// edits refuse anything that would let a signal reach itself.
#pragma once

#include <string>
#include <vector>

#include "vcc/project/Project.h"

namespace vcc::project {

struct RouteNode {
    enum class Kind { Track, Bus, Master, Hardware };
    Kind kind = Kind::Track;
    std::string id;          ///< Track id, bus id, "bus-master", or "hw-out".
    std::string name;
    std::string bus_kind;    ///< "fx", "group" for buses.
    bool stereo = true;      ///< Channel format.
    bool mute = false;
    double gain_db = 0.0;
    double pan = 0.0;
    struct Insert {
        std::string name;
        bool bypass = false;
    };
    std::vector<Insert> inserts;  ///< In processing order.
    int column = 0;          ///< Left-to-right signal-flow layout.
    int row = 0;             ///< Order within its column.
};

struct RouteEdge {
    enum class Kind { Output, Send };
    Kind kind = Kind::Output;
    std::string from, to;
    int send_index = -1;     ///< Kind::Send: which of the source's sends.
    double level_db = 0.0;
    bool pre_fader = false;
    bool mute = false;
};

struct RoutingGraph {
    std::vector<RouteNode> nodes;
    std::vector<RouteEdge> edges;
    int columns = 0;
    const RouteNode* node(const std::string& id) const noexcept;
    /// Everything that feeds `id` (directly or not), and everything it feeds.
    std::vector<std::string> upstream(const std::string& id) const;
    std::vector<std::string> downstream(const std::string& id) const;
};

/// The graph: audio-carrying tracks (not folders or video), the buses, the
/// master and the hardware output, laid out left to right by signal flow.
RoutingGraph routing_graph(const ProjectData& data);

/// Whether a connection from `from` (a track or bus) into bus `to` would let
/// a signal come back to where it started. With a loop, `why` gets the path
/// ("FX Verb -> Strings -> FX Verb").
bool would_loop(const ProjectData& data, const std::string& from, const std::string& to, std::string* why = nullptr);

}  // namespace vcc::project
