#include "vcc/library/TemplatePackage.h"

#include <algorithm>
#include <set>

namespace vcc::library {

namespace {
constexpr const char* kFormat = "vcc-template-package";
}

json::Value export_template_package(const templates::TemplateRegistry& registry, const Library* library,
                                    const std::vector<std::string>& ids, const PackageOptions& o)
{
    std::vector<std::string> kept;
    std::set<std::string> instruments;
    for (const auto& id : ids)
        if (const auto* t = registry.get(id)) {
            kept.push_back(id);
            for (const auto& tr : t->tracks) {
                instruments.insert(tr.instrument_standard_id);
                for (const auto& f : tr.fallback_instruments) instruments.insert(f);
            }
        }
    json::Object doc{{"format", kFormat},
                     {"schema_version", 1},
                     {"name", o.name},
                     {"author", o.author},
                     {"created", o.created},
                     {"templates", registry.export_pack(kept, o.name.empty() ? std::string("package") : o.name)}};
    if (library && o.include_patches) {
        std::vector<std::string> patch_ids;
        for (const auto& p : library->patches())
            if (instruments.count(p.instrument_id)) patch_ids.push_back(p.id);
        if (!patch_ids.empty()) doc.set("patches", library->export_pack(o.name, patch_ids, o.with_states));
    }
    return doc;
}

std::optional<PackageSummary> describe_template_package(const json::Value& doc)
{
    if (!doc.is_object() || doc.get_string("format") != kFormat || doc.get_int("schema_version") != 1) return std::nullopt;
    PackageSummary s;
    s.name = doc.get_string("name");
    s.author = doc.get_string("author");
    s.created = doc.get_string("created");
    if (const auto* pack = doc.get("templates"); pack && pack->is_object())
        if (const auto* arr = pack->get("templates"); arr && arr->is_array())
            for (const auto& t : arr->as_array()) s.template_names.push_back(t.get_string("display_name", t.get_string("template_id")));
    if (const auto* patches = doc.get("patches"); patches && patches->is_object())
        if (const auto* arr = patches->get("patches"); arr && arr->is_array()) s.patches = static_cast<int>(arr->as_array().size());
    return s;
}

PackageImport import_template_package(const json::Value& doc, templates::TemplateRegistry& registry, Library* library)
{
    PackageImport r;
    if (!describe_template_package(doc)) {
        r.error = "not a VC Composer template package";
        return r;
    }
    const auto* pack = doc.get("templates");
    const auto* arr = pack && pack->is_object() ? pack->get("templates") : nullptr;
    if (!arr || !arr->is_array() || arr->as_array().empty()) {
        r.error = "the package holds no templates";
        return r;
    }
    for (const auto& original : arr->as_array()) {
        json::Value t = original;
        std::string id = t.get_string("template_id");
        const auto* existing = registry.get(id);
        if (existing && existing->built_in) {
            // A built-in of the same id: in as a copy, never over it.
            id = "user." + id;
            t.set("template_id", id);
            t.set("display_name", t.get_string("display_name", id) + " (imported)");
            existing = registry.get(id);
            if (existing && existing->built_in) {
                r.warnings.push_back(id + ": a built-in template has that id; skipped");
                continue;
            }
        }
        // Validated on its own, then added as the composer's.
        templates::TemplateRegistry one(registry.instruments());
        std::vector<templates::Diagnostic> d;
        one.import_pack(json::Object{{"schema_version", 1}, {"pack", pack->get_string("pack", "import")}, {"templates", json::Array{t}}},
                        d);
        const auto* parsed = one.get(id);
        if (!parsed) {
            r.warnings.push_back(t.get_string("display_name", id) + ": " + (d.empty() ? std::string("invalid") : d.front().to_string()));
            continue;
        }
        std::string error;
        const bool replacing = existing != nullptr;
        if (!registry.add_user_template(*parsed, &error)) {
            r.warnings.push_back(parsed->display_name + ": " + error);
            continue;
        }
        r.replaced += replacing ? 1 : 0;
        r.template_ids.push_back(id);
    }
    if (library)
        if (const auto* patches = doc.get("patches"); patches && patches->is_object())
            r.patches = library->import_pack(*patches, &r.warnings);
    r.ok = !r.template_ids.empty();
    if (!r.ok) r.error = r.warnings.empty() ? "nothing could be imported" : r.warnings.front();
    return r;
}

}  // namespace vcc::library
