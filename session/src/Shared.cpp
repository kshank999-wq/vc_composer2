#include "vcc/session/Shared.h"

#include <filesystem>
#include <fstream>
#include <sstream>

#include "vcc/core/Files.h"

namespace vcc::session::shared {

namespace fs = std::filesystem;

json::Value Link::to_json() const
{
    return json::Object{{"project_id", project_id},
                        {"name", name},
                        {"role", role},
                        {"base", static_cast<long long>(base)},
                        {"site", static_cast<long long>(site)}};
}

std::optional<Link> Link::from_json(const json::Value& doc)
{
    if (!doc.is_object() || doc.get_string("project_id").empty()) return std::nullopt;
    Link l;
    l.project_id = doc.get_string("project_id");
    l.name = doc.get_string("name");
    l.role = doc.get_string("role");
    l.base = static_cast<int>(doc.get_int("base", 0));
    l.site = static_cast<std::uint32_t>(doc.get_int("site", 0));
    return l;
}

std::string link_path(const std::string& project_file) { return project_file + ".shared.json"; }
std::string base_path(const std::string& project_file) { return project_file + ".shared-base"; }

namespace {

std::optional<std::string> read_text(const std::string& path)
{
    std::ifstream in(fs::u8path(path), std::ios::binary);
    if (!in) return std::nullopt;
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}

}  // namespace

std::optional<Link> read_link(const std::string& project_file)
{
    const auto text = read_text(link_path(project_file));
    if (!text) return std::nullopt;
    const auto doc = json::parse(*text);
    return doc ? Link::from_json(*doc) : std::nullopt;
}

bool write_link(const std::string& project_file, const Link& link, std::string* error)
{
    return files::write_file_atomic(link_path(project_file), json::write(link.to_json()), error);
}

std::optional<json::Value> read_base(const std::string& project_file)
{
    const auto text = read_text(base_path(project_file));
    if (!text) return std::nullopt;
    return json::parse(*text);
}

bool write_base(const std::string& project_file, const std::string& document_text, std::string* error)
{
    return files::write_file_atomic(base_path(project_file), document_text, error);
}

void unlink(const std::string& project_file)
{
    std::error_code ec;
    fs::remove(fs::u8path(link_path(project_file)), ec);
    fs::remove(fs::u8path(base_path(project_file)), ec);
}

bool can_push(const std::string& role) { return role == "owner" || role == "editor" || role == "arranger"; }

std::string role_label(const std::string& role)
{
    if (role == "owner") return "Owner";
    if (role == "editor") return "Editor / composer";
    if (role == "arranger") return "Arranger / orchestrator";
    if (role == "reviewer") return "Reviewer / director";
    if (role == "viewer") return "View only";
    return role;
}

std::uint32_t site_from(std::uint64_t random)
{
    const auto s = static_cast<std::uint32_t>(random % 0xffffffu);
    return s + 1;  // 1..0xffffff
}

std::string document_text(const json::Value& doc) { return json::write(doc); }

bool changed_since(const json::Value& base, const json::Value& ours) { return !(base == ours); }

project::MergeResult pull(const std::optional<json::Value>& base, const json::Value& ours, const json::Value& theirs)
{
    if (!base) {
        project::MergeResult r;
        r.merged = theirs;
        return r;
    }
    return project::merge_projects(*base, ours, theirs);
}

}  // namespace vcc::session::shared
