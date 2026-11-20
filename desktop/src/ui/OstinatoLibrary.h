// The composer's own ostinato presets: a variant they liked, saved with the
// Ostinato Creator settings it came from, named, and kept in folders they
// make. Stored in the user's data folder (ostinato_presets.json), so it
// outlives projects and updates.
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>

#include "vcc/core/Json.h"

namespace vcc::desktop {

struct OstinatoUserPreset {
    std::string id;
    std::string name;
    std::string folder;       ///< Empty: not in a folder.
    std::string base_preset;  ///< The catalog preset it started from.
    std::string tonic, mode;  ///< The key, as the creator's boxes show it.
    int follow = 1;           ///< The creator's Follow choice.
    int length_bars = 8;
    double density = 90.0, swing = 0.0;  ///< As the sliders show them.
    bool evolve = false, call_response = false;
    std::string seed;
    json::Value variant;      ///< ostinato::Variant::to_json: the exact figure.
    std::string saved;        ///< When, for the list.
};

class OstinatoLibrary {
public:
    /// The one library, read from the user's data folder.
    static OstinatoLibrary& get();
    /// Reads (and later writes) another file instead (the checks).
    void use_file(const juce::File& file);

    const std::vector<std::string>& folders() const noexcept { return folders_; }
    const std::vector<OstinatoUserPreset>& presets() const noexcept { return presets_; }
    const OstinatoUserPreset* find(const std::string& id) const;

    /// Saves a preset (a new id), creating its folder if needed. Returns the id.
    std::string add(OstinatoUserPreset preset);
    bool rename(const std::string& id, const std::string& name);
    bool move(const std::string& id, const std::string& folder);
    bool remove(const std::string& id);
    bool add_folder(const std::string& name);
    bool rename_folder(const std::string& from, const std::string& to);
    /// Removes a folder; its presets stay, out of any folder.
    bool remove_folder(const std::string& name);

private:
    void load();
    void save() const;
    juce::File file_;
    std::vector<std::string> folders_;
    std::vector<OstinatoUserPreset> presets_;
};

/// The Custom Presets window: folders on the left, their presets on the
/// right; Load puts one back in the Ostinato Creator.
void show_ostinato_library(std::function<void(const OstinatoUserPreset&)> load);

}  // namespace vcc::desktop
