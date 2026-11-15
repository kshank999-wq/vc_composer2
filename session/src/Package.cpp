#include "vcc/session/Package.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <map>
#include <set>
#include <system_error>

#include "vcc/core/Files.h"
#include "vcc/core/Json.h"

namespace vcc::session::package {

namespace fs = std::filesystem;

namespace {

bool is_absolute(const std::string& p)
{
    return !p.empty() && (p[0] == '/' || p[0] == '\\' || (p.size() > 2 && p[1] == ':'));
}

std::string project_dir(const std::string& project_path)
{
    if (project_path.empty()) return {};
    return fs::u8path(project_path).parent_path().u8string();
}

std::string human_bytes(std::uintmax_t b)
{
    char buf[64];
    if (b >= 1024ull * 1024 * 1024) std::snprintf(buf, sizeof buf, "%.2f GB", static_cast<double>(b) / (1024.0 * 1024 * 1024));
    else if (b >= 1024ull * 1024) std::snprintf(buf, sizeof buf, "%.1f MB", static_cast<double>(b) / (1024.0 * 1024));
    else if (b >= 1024ull) std::snprintf(buf, sizeof buf, "%.1f KB", static_cast<double>(b) / 1024.0);
    else std::snprintf(buf, sizeof buf, "%llu bytes", static_cast<unsigned long long>(b));
    return buf;
}

void add_dependency(Inventory& inv, Dependency::Kind kind, const project::PluginRef& p, const std::string& user)
{
    if (p.empty()) return;
    const std::string name = p.name.empty() ? p.identifier : p.name;
    for (auto& d : inv.dependencies)
        if (d.kind == kind && d.name == name && d.format == p.format) {
            if (std::find(d.used_by.begin(), d.used_by.end(), user) == d.used_by.end()) d.used_by.push_back(user);
            return;
        }
    inv.dependencies.push_back({kind, name, p.vendor, p.format, {user}});
}

}  // namespace

std::uintmax_t Inventory::total_bytes() const noexcept
{
    std::uintmax_t t = 0;
    for (const auto& a : assets) t += a.exists ? a.bytes : 0;
    return t;
}

int Inventory::missing() const noexcept
{
    return static_cast<int>(std::count_if(assets.begin(), assets.end(), [](const Asset& a) { return !a.exists; }));
}

std::string safe_name(const std::string& name)
{
    std::string out;
    for (unsigned char c : name) {
        if (c < 32 || std::string("<>:\"/\\|?*").find(static_cast<char>(c)) != std::string::npos) out += '_';
        else out += static_cast<char>(c);
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == '.')) out.pop_back();
    while (!out.empty() && out.front() == ' ') out.erase(out.begin());
    return out.empty() ? std::string("Untitled") : out;
}

std::string resolve(const std::string& stored, const std::string& project_path)
{
    if (stored.empty()) return {};
    const std::string dir = project_dir(project_path);
    std::error_code ec;
    if (!is_absolute(stored)) return dir.empty() ? stored : (fs::u8path(dir) / fs::u8path(stored)).u8string();
    if (fs::exists(fs::u8path(stored), ec) || dir.empty()) return stored;
    // Moved with the project: the same name beside it.
    const auto beside = fs::u8path(dir) / fs::u8path(stored).filename();
    return fs::exists(beside, ec) ? beside.u8string() : stored;
}

Inventory inventory(const project::ProjectData& d, const std::string& project_path)
{
    Inventory inv;
    std::map<std::string, std::size_t> by_source;
    auto add = [&](Asset::Kind kind, const std::string& stored, const std::string& user) {
        if (stored.empty()) return;
        const std::string source = resolve(stored, project_path);
        std::error_code ec;
        const std::string key = fs::u8path(source).lexically_normal().u8string();
        auto it = by_source.find(key);
        if (it == by_source.end()) {
            Asset a;
            a.kind = kind;
            a.stored = stored;
            a.source = key;
            a.exists = fs::is_regular_file(fs::u8path(key), ec);
            a.bytes = a.exists ? fs::file_size(fs::u8path(key), ec) : 0;
            it = by_source.emplace(key, inv.assets.size()).first;
            inv.assets.push_back(std::move(a));
        }
        auto& users = inv.assets[it->second].used_by;
        if (std::find(users.begin(), users.end(), user) == users.end()) users.push_back(user);
    };
    for (const auto& t : d.tracks) {
        for (const auto& v : t.versions)
            for (const auto& c : v.audio_clips) add(Asset::Kind::Audio, c.file, t.name + " - " + v.name);
        add_dependency(inv, Dependency::Kind::InstrumentPlugin, t.instrument_plugin, t.name);
        for (const auto& s : t.inserts)
            if (s.kind == "plugin") add_dependency(inv, Dependency::Kind::EffectPlugin, s.plugin, t.name);
        for (const auto& lib : t.plugin_requirements) {
            project::PluginRef r;
            r.identifier = r.name = lib;
            add_dependency(inv, Dependency::Kind::Library, r, t.name);
        }
    }
    for (const auto& b : d.buses)
        for (const auto& s : b.inserts)
            if (s.kind == "plugin") add_dependency(inv, Dependency::Kind::EffectPlugin, s.plugin, b.name);
    for (const auto& p : d.pictures) add(Asset::Kind::Picture, p.path, "Picture: " + p.name);
    return inv;
}

std::string report_text(const project::ProjectData& d, const Inventory& inv, const Options& options)
{
    std::string r;
    r += "Package of \"" + d.name + "\"\n\n";
    int audio = 0, picture = 0;
    for (const auto& a : inv.assets) (a.kind == Asset::Kind::Audio ? audio : picture) += 1;
    r += "Media: " + std::to_string(audio) + " audio file" + (audio == 1 ? "" : "s") + ", " + std::to_string(picture) +
         " picture file" + (picture == 1 ? "" : "s") +
         (inv.missing() > 0 ? ", " + std::to_string(inv.missing()) + " missing" : std::string()) + " (" +
         human_bytes(inv.total_bytes()) + ")";
    if (!options.include_picture && picture > 0) r += " - picture not included";
    r += "\n";
    if (inv.missing() > 0) {
        r += "\nMISSING (" + std::to_string(inv.missing()) + ") - not in the package; relink them on arrival:\n";
        for (const auto& a : inv.assets)
            if (!a.exists) {
                r += "  " + a.stored + "\n    used by ";
                for (std::size_t i = 0; i < a.used_by.size(); ++i) r += (i ? ", " : "") + a.used_by[i];
                r += "\n";
            }
    }
    r += "\nIncluded:\n";
    bool any = false;
    for (const auto& a : inv.assets) {
        if (!a.exists || (a.kind == Asset::Kind::Picture && !options.include_picture)) continue;
        any = true;
        r += "  " + (a.packaged.empty() ? fs::u8path(a.source).filename().u8string() : a.packaged) + "  (" +
             human_bytes(a.bytes) + ")\n";
    }
    if (!any) r += "  (no media files)\n";
    r += "\nNeeded on the receiving machine (never bundled: licensed per machine):\n";
    if (inv.dependencies.empty()) r += "  Nothing beyond VC Composer 2.\n";
    const char* titles[] = {"Instrument plug-ins", "Effect plug-ins", "Sample libraries"};
    for (int k = 0; k < 3; ++k) {
        bool header = false;
        for (const auto& dep : inv.dependencies) {
            if (static_cast<int>(dep.kind) != k) continue;
            if (!header) r += "  " + std::string(titles[k]) + ":\n";
            header = true;
            r += "    " + dep.name;
            if (!dep.vendor.empty()) r += " (" + dep.vendor + ")";
            if (!dep.format.empty()) r += " [" + dep.format + "]";
            r += " - ";
            for (std::size_t i = 0; i < dep.used_by.size(); ++i) r += (i ? ", " : "") + dep.used_by[i];
            r += "\n";
        }
    }
    r += "\nA missing plug-in leaves its track on the placeholder sound, its settings kept; installing it\n"
         "and reopening restores it.\n";
    return r;
}

Result collect(const project::Project& project, const std::string& project_path, const std::string& folder,
               const Options& options, const Progress& progress)
{
    Result res;
    res.folder = folder;
    const auto& d = project.data();
    std::error_code ec;
    const fs::path root = fs::u8path(folder);
    if (fs::exists(root, ec) && !fs::is_empty(root, ec)) {
        res.error = "the folder " + folder + " already exists and is not empty";
        return res;
    }
    const bool created_root = !fs::exists(root, ec);
    fs::create_directories(root / "Audio Files", ec);
    if (!ec && options.include_picture) fs::create_directories(root / "Picture", ec);
    if (ec) {
        res.error = "could not create " + folder + ": " + ec.message();
        return res;
    }
    auto fail = [&](const std::string& why, bool cancelled = false) {
        std::error_code e2;
        if (created_root) fs::remove_all(root, e2);
        else
            for (const auto& entry : fs::directory_iterator(root, e2)) fs::remove_all(entry.path(), e2);
        res.ok = false;
        res.cancelled = cancelled;
        res.error = why;
        return res;
    };

    res.inventory = inventory(d, project_path);
    auto& inv = res.inventory;
    // Names inside the package: the file's own name, numbered when two
    // different files share one.
    std::set<std::string> taken;
    std::uintmax_t done = 0;
    const std::uintmax_t total = std::max<std::uintmax_t>(1, inv.total_bytes());
    std::map<std::string, std::string> packaged_by_stored;
    for (auto& a : inv.assets) {
        if (!a.exists) continue;
        if (a.kind == Asset::Kind::Picture && !options.include_picture) continue;
        const std::string dir = a.kind == Asset::Kind::Audio ? "Audio Files" : "Picture";
        const fs::path name = fs::u8path(a.source).filename();
        std::string candidate = dir + "/" + name.u8string();
        for (int n = 2; taken.count(candidate); ++n)
            candidate = dir + "/" + name.stem().u8string() + " " + std::to_string(n) + name.extension().u8string();
        taken.insert(candidate);
        fs::copy_file(fs::u8path(a.source), root / fs::u8path(candidate), fs::copy_options::overwrite_existing, ec);
        if (ec) return fail("could not copy " + a.source + ": " + ec.message());
        a.packaged = candidate;
        packaged_by_stored[a.stored] = candidate;
        ++res.copied;
        done += a.bytes;
        if (progress && !progress(static_cast<double>(done) / static_cast<double>(total)))
            return fail("cancelled", true);
    }

    // The project, pointing at its packaged media.
    project::Project out = project;
    out.modify_unrecorded([&](project::ProjectData& pd) {
        for (auto& t : pd.tracks)
            for (auto& v : t.versions)
                for (auto& c : v.audio_clips)
                    if (const auto it = packaged_by_stored.find(c.file); it != packaged_by_stored.end()) c.file = it->second;
        for (auto& p : pd.pictures) {
            if (const auto it = packaged_by_stored.find(p.path); it != packaged_by_stored.end()) p.path = it->second;
            else if (!is_absolute(p.path)) p.path = resolve(p.path, project_path);  // left out: still found here
        }
    });
    const std::string file_name = safe_name(d.name) + ".vcc2";
    res.project_file = (root / fs::u8path(file_name)).u8string();
    std::string err;
    if (!out.save(res.project_file, &err)) return fail("could not save the project: " + err);

    const std::string report = report_text(d, inv, options);
    if (!files::write_file_atomic((root / "Package Report.txt").u8string(), report, &err))
        return fail("could not write the report: " + err);
    json::Array media, deps;
    for (const auto& a : inv.assets) {
        json::Array users;
        for (const auto& u : a.used_by) users.emplace_back(u);
        media.emplace_back(json::Object{{"kind", a.kind == Asset::Kind::Audio ? "audio" : "picture"},
                                        {"original", a.stored},
                                        {"packaged", a.packaged},
                                        {"bytes", static_cast<long long>(a.bytes)},
                                        {"missing", !a.exists},
                                        {"used_by", users}});
    }
    for (const auto& dep : inv.dependencies) {
        json::Array users;
        for (const auto& u : dep.used_by) users.emplace_back(u);
        const char* kind = dep.kind == Dependency::Kind::InstrumentPlugin ? "instrument_plugin"
                           : dep.kind == Dependency::Kind::EffectPlugin   ? "effect_plugin"
                                                                          : "library";
        deps.emplace_back(json::Object{{"kind", kind}, {"name", dep.name}, {"vendor", dep.vendor},
                                       {"format", dep.format}, {"used_by", users}});
    }
    const json::Value manifest = json::Object{{"format", "vcc2-package"}, {"version", 1},
                                              {"project", file_name}, {"media", media}, {"needs", deps}};
    if (!files::write_file_atomic((root / "package.json").u8string(), json::write(manifest), &err))
        return fail("could not write the manifest: " + err);
    if (progress) progress(1.0);
    res.ok = true;
    return res;
}

}  // namespace vcc::session::package
