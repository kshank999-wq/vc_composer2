// Automation (Main Spec §12): what can be automated, how a lane is evaluated,
// and how a written pass is merged into a lane. Framework-free so the engine,
// the views and the tests share one definition.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "vcc/project/Project.h"

namespace vcc::project {

/// Describes an automatable parameter.
struct ParamInfo {
    std::string id;       ///< "volume", "strip.hmf.gain_db", "send:bus-7"...
    std::string name;     ///< "Volume", "HMF Gain", "Send Hall".
    std::string unit;     ///< "dB", "Hz", "ms", ":1", "" (pan, switches, plug-in).
    double min = 0.0;
    double max = 1.0;
    double default_value = 0.0;
    bool toggle = false;  ///< 0/1 switch: lanes step, the view draws on/off.
    bool log_scale = false;  ///< Frequencies and times: drawn and dragged logarithmically.
};

/// Parameters every track or bus strip offers (volume, pan, mute, the channel
/// strip). Sends, inserts and plug-ins are added per owner by
/// automatable_params().
const std::vector<ParamInfo>& strip_params();

/// Everything automatable on a track: strip params, its sends, built-in
/// insert params. Plug-in parameters are listed by the host, not here.
std::vector<ParamInfo> automatable_params(const Track& track, const ProjectData& data);
std::vector<ParamInfo> automatable_params(const Bus& bus, const ProjectData& data);

/// Range and default for any param id, including "send:", "insert:",
/// "plugin:" and "insert_plugin:" ids (plug-in params are 0..1).
std::optional<ParamInfo> param_info(const std::string& param);

/// The lane's value at `tick`: before the first point the first value, after
/// the last the last value, between points by the left point's shape.
/// An empty lane has no value.
std::optional<double> value_at(const AutomationLane& lane, Ticks tick);

/// The static (un-automated) value of a parameter on a track or bus, or
/// nothing for a param it does not have.
std::optional<double> static_value(const Track& track, const std::string& param);
std::optional<double> static_value(const Bus& bus, const std::string& param);
/// Sets the static value (clamped to the param's range). False if unknown.
bool set_static_value(Track& track, const std::string& param, double value);
bool set_static_value(Bus& bus, const std::string& param, double value);

/// Merges a written pass into a lane: points in [from, to] are replaced by
/// `written` (sorted, clamped to the param range), with guard points at
/// `from` and `to` holding the lane's previous values so the automation
/// outside the pass is unchanged. `written` is thinned first.
void write_pass(AutomationLane& lane, Ticks from, Ticks to, std::vector<AutomationPoint> written);

/// Removes points that a straight line through their neighbours reproduces
/// within `tolerance` (in the param's unit; switches are never thinned).
void thin(std::vector<AutomationPoint>& points, double tolerance);

/// Sorts by tick, keeps the last of equal ticks, clamps values to the
/// param's range and normalises shapes. Every edit path ends here.
void normalise(AutomationLane& lane);

/// Finds a lane by param.
const AutomationLane* find_lane(const std::vector<AutomationLane>& lanes, const std::string& param);
AutomationLane* find_lane(std::vector<AutomationLane>& lanes, const std::string& param);

}  // namespace vcc::project
