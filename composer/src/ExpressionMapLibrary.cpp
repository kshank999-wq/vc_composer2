#include "vcc/composer/ExpressionMapLibrary.h"

#include <algorithm>
#include <cmath>

namespace vcc::composer {
namespace {

constexpr const char* kNameKey = "name";
constexpr const char* kLibraryKey = "library";
constexpr const char* kDynamicTargetKey = "dynamics.target";
constexpr const char* kDynamicsCcKey = "dynamics.cc";
constexpr const char* kVelocityPrefix = "dynamics.velocity";
constexpr const char* kCcValuePrefix = "dynamics.cc_value";
constexpr const char* kHairpinShapeKey = "hairpin.shape";
constexpr const char* kHairpinResolutionKey = "hairpin.resolution_ticks";
constexpr const char* kLengthScaleKey = "length.default_scale";
constexpr const char* kMinimumTicksKey = "length.minimum_ticks";
constexpr const char* kRulePrefix = "rule";

std::string level_key(const char* prefix, std::size_t index)
{
    return std::string(prefix) + "." + notation::to_string(static_cast<notation::DynamicLevel>(index));
}

std::string rule_key(std::size_t index, const char* field)
{
    return std::string(kRulePrefix) + "." + std::to_string(index) + "." + field;
}

std::string section_name(std::size_t index)
{
    return std::string(kExpressionMapSectionPrefix) + "." + std::to_string(index);
}

bool is_map_section(const std::string& name)
{
    const std::string prefix = std::string(kExpressionMapSectionPrefix) + ".";
    if (name.compare(0, prefix.size(), prefix) != 0 || name.size() == prefix.size())
        return false;
    for (std::size_t i = prefix.size(); i < name.size(); ++i) {
        if (name[i] < '0' || name[i] > '9')
            return false;
    }
    return true;
}

/// Reads a 0..127 value, saying which when it is not one.
bool read_midi_value(const composer::SettingsSection& section, const std::string& key,
                     std::uint8_t& out, const char* what, std::vector<std::string>& warnings)
{
    std::int64_t value = 0;
    const auto reading = section.read_int(key, value);
    if (reading == composer::SettingsSection::Reading::Absent)
        return false;
    if (reading == composer::SettingsSection::Reading::Unusable || value < 0 || value > 127) {
        warnings.push_back(std::string("the ") + what + " was not a MIDI value (0 to 127)");
        return false;
    }
    out = static_cast<std::uint8_t>(value);
    return true;
}

}  // namespace

const notation::ExpressionMap* ExpressionMapLibrary::find(const std::string& name) const noexcept
{
    for (const notation::ExpressionMap& map : maps) {
        if (map.name == name)
            return &map;
    }
    return nullptr;
}

std::size_t ExpressionMapLibrary::index_of(const std::string& name) const noexcept
{
    for (std::size_t index = 0; index < maps.size(); ++index) {
        if (maps[index].name == name)
            return index;
    }
    return maps.size();
}

bool ExpressionMapLibrary::is_built_in(const std::string& name) const noexcept
{
    return name == kDefaultExpressionMapName || name == kOrchestralExpressionMapName;
}

const notation::ExpressionMap& ExpressionMapLibrary::default_map() const noexcept
{
    if (const notation::ExpressionMap* map = find(kDefaultExpressionMapName))
        return *map;
    // A library is always built from the built-ins, so this is a library
    // somebody emptied by hand; the engine's own default is the answer.
    static const notation::ExpressionMap fallback = notation::make_default_expression_map();
    return fallback;
}

bool ExpressionMapLibrary::store(const notation::ExpressionMap& map)
{
    if (map.name.empty() || is_built_in(map.name) || !map.is_valid())
        return false;

    const std::size_t index = index_of(map.name);
    if (index < maps.size())
        maps[index] = map;
    else
        maps.push_back(map);
    return true;
}

bool ExpressionMapLibrary::remove(const std::string& name)
{
    if (is_built_in(name))
        return false;
    const std::size_t index = index_of(name);
    if (index >= maps.size())
        return false;
    maps.erase(maps.begin() + static_cast<std::ptrdiff_t>(index));
    return true;
}

std::string ExpressionMapLibrary::next_after(const std::string& name) const
{
    if (maps.empty())
        return {};
    const std::size_t index = index_of(name);
    if (index >= maps.size())
        return maps.front().name;
    return maps[(index + 1) % maps.size()].name;
}

std::string ExpressionMapLibrary::copy_name_for(const std::string& name) const
{
    const std::string base = name + " copy";
    if (find(base) == nullptr)
        return base;
    for (int number = 2;; ++number) {
        const std::string candidate = base + " " + std::to_string(number);
        if (find(candidate) == nullptr)
            return candidate;
    }
}

ExpressionMapLibrary built_in_expression_maps()
{
    ExpressionMapLibrary library;
    library.maps.push_back(notation::make_default_expression_map());
    library.maps.push_back(notation::make_orchestral_cc_expression_map());
    return library;
}

void expression_map_to_settings(const notation::ExpressionMap& map,
                                composer::SettingsSection& out)
{
    out.clear();
    out.set(kNameKey, map.name);
    if (!map.library.empty())
        out.set(kLibraryKey, map.library);

    out.set(kDynamicTargetKey, notation::to_string(map.dynamic_target));
    out.set_int(kDynamicsCcKey, map.dynamics_cc);
    for (std::size_t index = 0; index < notation::kDynamicLevelCount; ++index) {
        out.set_int(level_key(kVelocityPrefix, index), map.dynamic_velocity[index]);
        out.set_int(level_key(kCcValuePrefix, index), map.dynamic_cc_value[index]);
    }

    out.set(kHairpinShapeKey, notation::to_string(map.default_hairpin_shape));
    out.set_int(kHairpinResolutionKey, map.hairpin_resolution_ticks);
    out.set_double(kLengthScaleKey, map.default_length_scale);
    out.set_int(kMinimumTicksKey, map.minimum_note_ticks);

    for (std::size_t index = 0; index < map.articulations.size(); ++index) {
        const notation::ArticulationRule& rule = map.articulations[index];
        out.set(rule_key(index, "articulation"), notation::to_string(rule.articulation));
        out.set(rule_key(index, "action"), notation::to_string(rule.action));
        out.set_double(rule_key(index, "velocity_scale"), rule.velocity_scale);
        out.set_double(rule_key(index, "length_scale"), rule.length_scale);
        out.set_int(rule_key(index, "keyswitch_note"), rule.keyswitch_note);
        out.set_int(rule_key(index, "keyswitch_velocity"), rule.keyswitch_velocity);
        out.set_int(rule_key(index, "cc_number"), rule.cc_number);
        out.set_int(rule_key(index, "cc_value"), rule.cc_value);
    }
}

bool expression_map_from_settings(const composer::SettingsSection& section,
                                  notation::ExpressionMap& out,
                                  std::vector<std::string>& warnings)
{
    const std::string name = section.get(kNameKey);
    if (name.empty()) {
        warnings.push_back("a map in the settings has no name, so it was left out");
        return false;
    }

    // Everything starts from the engine's defaults, so a key the file does
    // not carry - one from before the setting existed - keeps a value that
    // plays.
    notation::ExpressionMap map = notation::make_default_expression_map();
    map.articulations.clear();
    map.name = name;
    map.library = section.get(kLibraryKey);

    if (section.has(kDynamicTargetKey)) {
        notation::DynamicTarget target = map.dynamic_target;
        if (notation::dynamic_target_from_string(section.get(kDynamicTargetKey), target))
            map.dynamic_target = target;
        else
            warnings.push_back(name + ": the dynamics target was not velocity, control change "
                                      "or both, so velocity stands");
    }
    read_midi_value(section, kDynamicsCcKey, map.dynamics_cc, "dynamics controller", warnings);
    for (std::size_t index = 0; index < notation::kDynamicLevelCount; ++index) {
        read_midi_value(section, level_key(kVelocityPrefix, index), map.dynamic_velocity[index],
                        "velocity for a dynamic", warnings);
        read_midi_value(section, level_key(kCcValuePrefix, index), map.dynamic_cc_value[index],
                        "controller value for a dynamic", warnings);
    }

    if (section.has(kHairpinShapeKey)) {
        notation::CurveShape shape = map.default_hairpin_shape;
        if (notation::curve_shape_from_string(section.get(kHairpinShapeKey), shape))
            map.default_hairpin_shape = shape;
        else
            warnings.push_back(name + ": the hairpin shape was not one this build draws");
    }

    std::int64_t resolution = 0;
    const auto resolution_reading = section.read_int(kHairpinResolutionKey, resolution);
    if (resolution_reading == composer::SettingsSection::Reading::Read && resolution > 0)
        map.hairpin_resolution_ticks = static_cast<notation::Ticks>(resolution);
    else if (resolution_reading != composer::SettingsSection::Reading::Absent)
        warnings.push_back(name + ": the hairpin resolution was not a number of ticks");

    double scale = 0.0;
    const auto scale_reading = section.read_double(kLengthScaleKey, scale);
    if (scale_reading == composer::SettingsSection::Reading::Read && scale > 0.0 && scale <= 2.0)
        map.default_length_scale = scale;
    else if (scale_reading != composer::SettingsSection::Reading::Absent)
        warnings.push_back(name + ": the default note length was not a proportion");

    std::int64_t minimum = 0;
    const auto minimum_reading = section.read_int(kMinimumTicksKey, minimum);
    if (minimum_reading == composer::SettingsSection::Reading::Read && minimum >= 0)
        map.minimum_note_ticks = static_cast<notation::Ticks>(minimum);
    else if (minimum_reading != composer::SettingsSection::Reading::Absent)
        warnings.push_back(name + ": the shortest note was not a number of ticks");

    for (const std::size_t index : section.indices(kRulePrefix)) {
        notation::ArticulationRule rule;
        const std::string which = section.get(rule_key(index, "articulation"));
        if (!notation::articulation_from_string(which, rule.articulation)
            || rule.articulation == notation::Articulation::None) {
            warnings.push_back(name + ": a rule names an articulation this build does not "
                                      "have, so it was left out");
            continue;
        }
        if (!notation::articulation_action_from_string(section.get(rule_key(index, "action")),
                                                       rule.action)) {
            warnings.push_back(name + ": the rule for " + which
                               + " has an action this build cannot do, so it was left out");
            continue;
        }

        bool usable = true;
        const auto read_scale = [&](const char* field, double& into) {
            double value = 0.0;
            const auto reading = section.read_double(rule_key(index, field), value);
            if (reading == composer::SettingsSection::Reading::Read)
                into = value;
            else if (reading != composer::SettingsSection::Reading::Absent)
                usable = false;
        };
        read_scale("velocity_scale", rule.velocity_scale);
        read_scale("length_scale", rule.length_scale);
        std::vector<std::string> rule_warnings;
        read_midi_value(section, rule_key(index, "keyswitch_note"), rule.keyswitch_note,
                        "keyswitch note", rule_warnings);
        read_midi_value(section, rule_key(index, "keyswitch_velocity"), rule.keyswitch_velocity,
                        "keyswitch velocity", rule_warnings);
        read_midi_value(section, rule_key(index, "cc_number"), rule.cc_number,
                        "controller number", rule_warnings);
        read_midi_value(section, rule_key(index, "cc_value"), rule.cc_value,
                        "controller value", rule_warnings);

        if (!usable || !rule_warnings.empty() || !rule.is_valid()) {
            warnings.push_back(name + ": the rule for " + which
                               + " has a value the engine cannot use, so it was left out");
            continue;
        }
        map.set_rule(rule);
    }

    if (!map.is_valid()) {
        warnings.push_back(name + " is not a map the engine can play, so it was left out");
        return false;
    }

    out = std::move(map);
    return true;
}

void expression_maps_to_store(const ExpressionMapLibrary& library, composer::SettingsStore& store)
{
    // Whole rather than merged: a map removed today must not come back from
    // a longer list tomorrow.
    for (const std::string& name : store.section_names()) {
        if (is_map_section(name))
            store.remove_section(name);
    }

    std::size_t written = 0;
    for (const notation::ExpressionMap& map : library.maps) {
        if (library.is_built_in(map.name))
            continue;
        expression_map_to_settings(map, store.section(section_name(written)));
        ++written;
    }
}

std::vector<std::string> expression_maps_from_store(const composer::SettingsStore& store,
                                                    ExpressionMapLibrary& out)
{
    std::vector<std::string> warnings;
    for (const std::string& name : store.section_names()) {
        if (!is_map_section(name))
            continue;
        const composer::SettingsSection* section = store.find_section(name);
        if (section == nullptr)
            continue;

        notation::ExpressionMap map;
        if (!expression_map_from_settings(*section, map, warnings))
            continue;
        if (out.is_built_in(map.name)) {
            warnings.push_back("a map in the settings is called " + map.name
                               + ", which is built in, so it was left out");
            continue;
        }
        out.store(map);
    }
    return warnings;
}

}  // namespace vcc::composer
