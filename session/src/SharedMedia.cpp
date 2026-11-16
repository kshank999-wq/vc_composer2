#include "vcc/session/SharedMedia.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

#include "vcc/core/Files.h"
#include "vcc/core/Hash.h"
#include "vcc/session/Package.h"

namespace vcc::session::shared {

namespace fs = std::filesystem;

namespace {

bool is_hex64(const std::string& s)
{
    return s.size() == 64 && std::all_of(s.begin(), s.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

fs::path folder_of(const std::string& project_file) { return fs::u8path(project_file).parent_path(); }

/// The media paths `data` uses, as stored, with their kind.
std::vector<package::Asset> used(const project::ProjectData& data, const std::string& project_file)
{
    return package::inventory(data, project_file).assets;
}

std::string with_slashes(std::string s)
{
    std::replace(s.begin(), s.end(), '\\', '/');
    return s;
}

}  // namespace

bool safe_media_path(const std::string& path)
{
    if (path.empty() || path.size() > 400) return false;
    if (path.front() == '/' || path.find('\\') != std::string::npos || path.find('\0') != std::string::npos) return false;
    if (path.size() >= 2 && path[1] == ':') return false;
    std::size_t start = 0;
    while (true) {
        const auto slash = path.find('/', start);
        const auto part = path.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        if (part.empty() || part == "." || part == "..") return false;
        if (slash == std::string::npos) return true;
        start = slash + 1;
    }
}

json::Value manifest_to_json(const Manifest& m)
{
    json::Array a;
    for (const auto& e : m)
        a.push_back(json::Object{{"path", e.path}, {"sha256", e.sha256}, {"sizeBytes", static_cast<long long>(e.size)}});
    return json::Value(std::move(a));
}

Manifest manifest_from_json(const json::Value& v)
{
    Manifest m;
    if (!v.is_array()) return m;
    std::set<std::string> seen;
    for (const auto& e : v.as_array()) {
        MediaEntry x{e.get_string("path"), e.get_string("sha256"), static_cast<std::uint64_t>(std::max<std::int64_t>(0, e.get_int("sizeBytes")))};
        if (!safe_media_path(x.path) || !is_hex64(x.sha256) || x.size == 0 || !seen.insert(x.path).second) continue;
        m.push_back(std::move(x));
    }
    return m;
}

const MediaEntry* find_media(const Manifest& m, const std::string& path)
{
    for (const auto& e : m)
        if (e.path == path) return &e;
    return nullptr;
}

bool same_media(Manifest a, Manifest b)
{
    auto by_path = [](const MediaEntry& x, const MediaEntry& y) { return x.path < y.path; };
    std::sort(a.begin(), a.end(), by_path);
    std::sort(b.begin(), b.end(), by_path);
    return a == b;
}

std::string media_file(const std::string& project_file, const std::string& path)
{
    return (folder_of(project_file) / fs::u8path(path)).u8string();
}

// --- The hash cache ------------------------------------------------------------------------

HashCache::HashCache(std::string project_file) : project_file_(std::move(project_file))
{
    std::ifstream in(fs::u8path(project_file_ + ".shared-media.json"), std::ios::binary);
    if (in) {
        std::ostringstream s;
        s << in.rdbuf();
        if (auto v = json::parse(s.str()); v && v->is_object()) cache_ = *v;
    }
    if (!cache_.is_object()) cache_ = json::Value::make_object();
}

std::optional<std::string> HashCache::sha256(const std::string& path, const std::function<bool(std::uint64_t)>& progress)
{
    const auto file = fs::u8path(media_file(project_file_, path));
    std::error_code ec;
    if (!fs::is_regular_file(file, ec)) return std::nullopt;
    const auto size = static_cast<long long>(fs::file_size(file, ec));
    const auto mtime = static_cast<long long>(fs::last_write_time(file, ec).time_since_epoch().count());
    if (const auto* c = cache_.get(path); c && c->get_int("size", -1) == size && c->get_int("mtime", -1) == mtime &&
                                          is_hex64(c->get_string("sha256")))
        return c->get_string("sha256");
    auto digest = hash::sha256_file(file.u8string(), progress);
    if (!digest) return std::nullopt;
    cache_.set(path, json::Object{{"size", size}, {"mtime", mtime}, {"sha256", *digest}});
    return digest;
}

void HashCache::note(const std::string& path, const std::string& sha)
{
    const auto file = fs::u8path(media_file(project_file_, path));
    std::error_code ec;
    const auto size = static_cast<long long>(fs::file_size(file, ec));
    const auto mtime = static_cast<long long>(fs::last_write_time(file, ec).time_since_epoch().count());
    if (!ec) cache_.set(path, json::Object{{"size", size}, {"mtime", mtime}, {"sha256", sha}});
}

bool HashCache::save() const { return files::write_file_atomic(project_file_ + ".shared-media.json", json::write(cache_)); }

// --- Gathering ----------------------------------------------------------------------------------

std::vector<Copy> plan_gather(const project::ProjectData& data, const std::string& project_file)
{
    std::vector<Copy> out;
    if (project_file.empty()) return out;
    const auto dir = folder_of(project_file).lexically_normal();
    std::set<std::string> taken;
    for (const auto& a : used(data, project_file)) {
        if (!a.exists) continue;
        const auto stored = with_slashes(a.stored);
        const bool relative = !fs::u8path(a.stored).is_absolute() && !(a.stored.size() > 1 && a.stored[1] == ':');
        if (relative && safe_media_path(stored)) continue;  // already portable
        const auto source = fs::u8path(a.source).lexically_normal();
        // Inside the folder already (stored absolute): relink only.
        const auto rel = source.lexically_relative(dir).generic_u8string();
        if (!rel.empty() && safe_media_path(rel)) {
            out.push_back({a.stored, source.u8string(), source.u8string()});
            continue;
        }
        const auto sub = dir / (a.kind == package::Asset::Kind::Picture ? "Picture" : "Audio Files");
        const auto stem = source.stem().u8string(), ext = source.extension().u8string();
        std::error_code ec;
        fs::path to;
        for (int n = 1;; ++n) {
            to = sub / fs::u8path(n == 1 ? stem + ext : stem + " " + std::to_string(n) + ext);
            const auto key = to.generic_u8string();
            if (taken.count(key)) continue;
            if (!fs::exists(to, ec)) break;
            // The same file gathered before: use it.
            if (fs::file_size(to, ec) == fs::file_size(source, ec) &&
                hash::sha256_file(to.u8string()) == hash::sha256_file(source.u8string()))
                break;
        }
        taken.insert(to.generic_u8string());
        out.push_back({a.stored, source.u8string(), to.u8string()});
    }
    return out;
}

bool run_copies(const std::vector<Copy>& copies, std::string* error)
{
    for (const auto& c : copies) {
        if (c.from == c.to) continue;
        std::error_code ec;
        if (fs::exists(fs::u8path(c.to), ec)) continue;  // the same file, gathered before
        fs::create_directories(fs::u8path(c.to).parent_path(), ec);
        fs::copy_file(fs::u8path(c.from), fs::u8path(c.to), ec);
        if (ec) {
            if (error) *error = "could not copy " + c.from + " into the project folder: " + ec.message();
            return false;
        }
    }
    return true;
}

std::map<std::string, std::string> relinks_of(const std::vector<Copy>& copies)
{
    std::map<std::string, std::string> r;
    for (const auto& c : copies) r[c.stored] = c.to;
    return r;
}

// --- Manifests ----------------------------------------------------------------------------------

Listed list_media(const project::ProjectData& data, const std::string& project_file, HashCache& cache, const Manifest& known,
                  const std::function<bool(std::uint64_t)>& progress)
{
    Listed out;
    std::set<std::string> seen;
    for (const auto& a : used(data, project_file)) {
        const auto path = with_slashes(a.stored);
        if (!safe_media_path(path)) {
            out.outside.push_back(a.stored);
            continue;
        }
        if (!seen.insert(path).second) continue;
        if (!a.exists) {
            if (const auto* k = find_media(known, path)) out.manifest.push_back(*k);
            else out.missing.push_back(path);
            continue;
        }
        if (a.bytes > kMaxMediaBytes) {
            out.too_large.push_back(path);
            continue;
        }
        const auto sha = cache.sha256(path, progress);
        if (!sha) {
            out.missing.push_back(path);
            continue;
        }
        out.manifest.push_back({path, *sha, static_cast<std::uint64_t>(a.bytes)});
    }
    std::sort(out.manifest.begin(), out.manifest.end(), [](const MediaEntry& x, const MediaEntry& y) { return x.path < y.path; });
    return out;
}

std::map<std::string, std::string> separate_clashes(const Manifest& ours, const Manifest& base, const Manifest& theirs,
                                                    const std::string& project_file, std::uint32_t site, std::string* error)
{
    std::map<std::string, std::string> out;
    char tag[16];
    std::snprintf(tag, sizeof tag, "%x", site);
    for (const auto& mine : ours) {
        const auto* t = find_media(theirs, mine.path);
        if (!t || t->sha256 == mine.sha256) continue;
        const auto* b = find_media(base, mine.path);
        if (b && b->sha256 == mine.sha256) continue;  // unchanged here: theirs simply replaces it
        const auto from = fs::u8path(media_file(project_file, mine.path));
        const auto stem = from.stem().u8string(), ext = from.extension().u8string();
        std::error_code ec;
        fs::path to;
        for (int n = 1;; ++n) {
            to = from.parent_path() / fs::u8path(stem + " (" + tag + (n > 1 ? " " + std::to_string(n) : std::string()) + ")" + ext);
            if (!fs::exists(to, ec)) break;
        }
        // A copy: the original path stays valid until theirs is downloaded over it.
        fs::copy_file(from, to, ec);
        if (ec) {
            if (error) *error = "could not keep " + mine.path + " apart from theirs: " + ec.message();
            return {};
        }
        out[mine.path] = to.u8string();
    }
    return out;
}

Manifest to_fetch(const project::ProjectData& merged, const Manifest& theirs, const std::string& project_file, HashCache& cache)
{
    Manifest out;
    std::set<std::string> uses;
    for (const auto& a : used(merged, project_file)) uses.insert(with_slashes(a.stored));
    for (const auto& e : theirs) {
        if (!uses.count(e.path)) continue;
        const auto have = cache.sha256(e.path);
        if (!have || *have != e.sha256) out.push_back(e);
    }
    return out;
}

}  // namespace vcc::session::shared
