#include "vcc/composer/SettingsMigration.h"

#include <cstdint>

namespace vcc::composer {
namespace {

/// Format 1 counted 960 ticks to the quarter note; format 2 counts 2880,
/// the notation engine's. The scale between them, written out rather than
/// read from the engine, since a migration describes the formats as they
/// were and must not move when the engine does.
constexpr std::int64_t kFormat2TicksPerFormat1Tick = 3;

/// True for a key whose value is a tick count, by the naming every such
/// key follows: `ticks_before_playhead`, `minimum_ticks`,
/// `part.3.cursor_tick`, `hairpin.resolution_ticks`.
bool holds_ticks(const std::string& key)
{
    const auto ends_with = [&](const char* suffix) {
        const std::string tail(suffix);
        return key.size() >= tail.size()
               && key.compare(key.size() - tail.size(), tail.size(), tail) == 0;
    };
    return key.rfind("ticks_", 0) == 0 || ends_with("_tick") || ends_with("_ticks");
}

/// Multiplies every tick-valued key in every section. One that is not a
/// whole number is left as it is and named: the reader will say the same
/// of it as before, rather than this step inventing a value.
void ticks_from_960_to_2880(SettingsStore& store, std::vector<std::string>& notes)
{
    for (const std::string& name : store.section_names()) {
        SettingsSection& section = store.section(name);
        std::vector<std::string> keys;
        for (const auto& entry : section.values()) {
            if (holds_ticks(entry.first))
                keys.push_back(entry.first);
        }
        for (const std::string& key : keys) {
            std::int64_t value = 0;
            if (section.read_int(key, value) != SettingsSection::Reading::Read) {
                notes.push_back("[" + name + "] " + key + " was not a number of ticks and was left as it was");
                continue;
            }
            section.set_int(key, value * kFormat2TicksPerFormat1Tick);
        }
    }
}

}  // namespace

const std::vector<SettingsMigrationStep>& settings_migration_steps()
{
    static const std::vector<SettingsMigrationStep> steps = {
        {1, "ticks count 2880 to the quarter note, from 960", &ticks_from_960_to_2880},
    };
    return steps;
}

namespace {

const SettingsMigrationStep* step_from(const std::vector<SettingsMigrationStep>& steps,
                                       int version)
{
    for (const SettingsMigrationStep& step : steps) {
        if (step.from == version && step.apply != nullptr)
            return &step;
    }
    return nullptr;
}

}  // namespace

bool steps_lead_to(const std::vector<SettingsMigrationStep>& steps, int target)
{
    for (int version = 1; version < target; ++version) {
        if (step_from(steps, version) == nullptr)
            return false;
    }
    return true;
}

SettingsMigrationResult migrate_settings(SettingsStore& store,
                                         const std::vector<SettingsMigrationStep>& steps,
                                         int target)
{
    SettingsMigrationResult result;
    result.from = store.version();
    result.to = result.from;

    if (result.from > target) {
        result.newer = true;
        return result;
    }

    while (result.to < target) {
        const SettingsMigrationStep* step = step_from(steps, result.to);
        if (step == nullptr) {
            result.stranded = true;
            break;
        }
        result.notes.push_back(std::string("format ") + std::to_string(step->from) + " to "
                               + std::to_string(step->from + 1) + ": " + step->what);
        step->apply(store, result.notes);
        ++result.to;
    }

    // The version follows the content, so a file written after this is
    // this build's format and not the old header over new keys.
    store.set_version(result.to);
    return result;
}

SettingsMigrationResult migrate_settings(SettingsStore& store)
{
    return migrate_settings(store, settings_migration_steps(), kSettingsFormatVersion);
}

}  // namespace vcc::composer
