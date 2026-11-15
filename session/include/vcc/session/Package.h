// Project packaging (Main Spec §16 "Project portability", §15 "local review
// packages"): collect a project and the media it uses into one folder that
// opens anywhere, and say what it still needs there.
//
//   <Name>/<Name>.vcc2        the project, its media paths made relative
//   <Name>/Audio Files/       every audio file any version uses (takes too)
//   <Name>/Picture/           the picture cuts (optional: they can be large)
//   <Name>/Package Report.txt what is in it, what is missing, and the
//                             plug-ins and sample libraries it needs
//   <Name>/package.json       the same, for tools
//
// Plug-ins and sample libraries are never bundled - they are licensed per
// machine (the spec forbids shipping them); the report lists them so the
// receiving studio knows what to install or substitute. Plug-in states
// travel inside the project file as before.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "vcc/project/Project.h"

namespace vcc::session::package {

struct Asset {
    enum class Kind { Audio, Picture };
    Kind kind = Kind::Audio;
    std::string stored;    ///< As the project has it (relative or absolute).
    std::string source;    ///< Where it was found (absolute), or the best guess.
    bool exists = false;
    std::uintmax_t bytes = 0;
    std::vector<std::string> used_by;  ///< "Violins - Version 2", "Picture: Cut 3".
    std::string packaged;  ///< Its path inside the package, once collected.
};

struct Dependency {
    enum class Kind { InstrumentPlugin, EffectPlugin, Library };
    Kind kind = Kind::InstrumentPlugin;
    std::string name;      ///< "Kontakt 8", or a library requirement.
    std::string vendor;
    std::string format;    ///< "VST3", "AudioUnit".
    std::vector<std::string> used_by;
};

struct Inventory {
    std::vector<Asset> assets;
    std::vector<Dependency> dependencies;
    std::uintmax_t total_bytes() const noexcept;
    int missing() const noexcept;
};

/// What the project uses: audio from every version of every track (takes and
/// unused versions too: they are part of the project), picture cuts, plug-ins
/// and library requirements. `project_path` is the saved project file (empty
/// for an unsaved project: then only absolute paths can be found).
Inventory inventory(const project::ProjectData& data, const std::string& project_path);

/// Where a stored media path points: relative paths from the project's
/// folder; an absolute path that is gone, the same name beside the project.
std::string resolve(const std::string& stored, const std::string& project_path);

struct Options {
    bool include_picture = true;
};

struct Result {
    bool ok = false;
    bool cancelled = false;
    std::string error;
    std::string folder;        ///< The package.
    std::string project_file;  ///< The project inside it.
    Inventory inventory;       ///< With `packaged` filled in.
    int copied = 0;
};

/// Called with the fraction done (0..1); return false to cancel.
using Progress = std::function<bool(double)>;

/// Collects `project` into `folder` (created; it must not exist or be
/// empty). Missing media are reported and keep their old path. A cancelled
/// or failed package removes what it wrote.
Result collect(const project::Project& project, const std::string& project_path, const std::string& folder,
               const Options& options = {}, const Progress& progress = {});

/// The report, as written into the package (also shown before packaging).
std::string report_text(const project::ProjectData& data, const Inventory& inv, const Options& options);

/// A name safe as a file or folder name on every platform.
std::string safe_name(const std::string& name);

}  // namespace vcc::session::package
