// Template packages (Templates Supplement §3 "import/export"): a
// .vcctemplates file another composer can open - custom templates and,
// when asked, the saved patches that play their instruments, so the
// template arrives with its sounds where the same plug-ins are installed.
//
//   { "format": "vcc-template-package", "schema_version": 1,
//     "name", "author", "created",
//     "templates": <template pack>, "patches": <mapping pack, optional> }
//
// On import every template is validated; a template whose id is a built-in
// one comes in as a copy ("user.<id>", name marked imported), a custom one
// with the same id is replaced. Patches go through Library::import_pack.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "vcc/core/Json.h"
#include "vcc/library/Library.h"
#include "vcc/templates/Templates.h"

namespace vcc::library {

struct PackageOptions {
    std::string name;
    std::string author;
    std::string created;          ///< ISO 8601.
    bool include_patches = true;  ///< The saved patches for the templates' instruments.
    bool with_states = false;     ///< And their plug-in settings.
};

/// The package for these templates (unknown ids are left out). `library`
/// may be null (no patches).
json::Value export_template_package(const templates::TemplateRegistry& registry, const Library* library,
                                    const std::vector<std::string>& template_ids, const PackageOptions& options);

struct PackageSummary {
    std::string name, author, created;
    std::vector<std::string> template_names;
    int patches = 0;
};
/// What a package holds, without importing it; nullopt when it is not one.
std::optional<PackageSummary> describe_template_package(const json::Value& doc);

struct PackageImport {
    bool ok = false;
    std::string error;                       ///< Why nothing was imported.
    std::vector<std::string> template_ids;   ///< Added or replaced, as they are now named.
    int replaced = 0;                        ///< Custom templates of the same id replaced.
    int patches = 0;
    std::vector<std::string> warnings;       ///< Templates skipped, with the reason; patch notes.
};
PackageImport import_template_package(const json::Value& doc, templates::TemplateRegistry& registry, Library* library);

}  // namespace vcc::library
