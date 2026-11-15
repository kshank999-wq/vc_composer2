#include "vcc/session/Relink.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <set>
#include <system_error>

namespace vcc::session::relink {

namespace fs = std::filesystem;

namespace {

std::string lower(std::string s)
{
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

/// The stored path's parts, last first, lower case ("vln_01.wav", "audio files", ...).
/// Both separators split: a path written on Windows is read anywhere.
std::vector<std::string> parts_reversed(const std::string& path)
{
    std::vector<std::string> out;
    std::string cur;
    for (char c : path) {
        if (c == '/' || c == '\\') {
            if (!cur.empty()) out.push_back(lower(cur));
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) out.push_back(lower(cur));
    std::reverse(out.begin(), out.end());
    return out;
}

std::string stem_of(const std::string& lower_name)
{
    const auto dot = lower_name.rfind('.');
    return dot == std::string::npos || dot == 0 ? lower_name : lower_name.substr(0, dot);
}

bool is_media(const std::string& lower_name)
{
    static const std::set<std::string> exts{".wav", ".wave", ".bwf", ".aif", ".aiff", ".aifc", ".caf", ".flac", ".ogg",
                                            ".mp3", ".m4a", ".mov", ".mp4", ".m4v", ".mxf", ".avi", ".mkv"};
    const auto dot = lower_name.rfind('.');
    return dot != std::string::npos && exts.count(lower_name.substr(dot)) > 0;
}

bool inside(const fs::path& file, const fs::path& folder)
{
    auto f = file.begin();
    for (auto d = folder.begin(); d != folder.end(); ++d, ++f) {
        if (d->empty()) continue;  // a trailing separator
        if (f == file.end() || *f != *d) return false;
    }
    return true;
}

}  // namespace

int SearchResult::found() const noexcept
{
    return static_cast<int>(std::count_if(matches.begin(), matches.end(), [](const Match& m) { return !m.chosen.empty(); }));
}

int SearchResult::ambiguous() const noexcept
{
    return static_cast<int>(std::count_if(matches.begin(), matches.end(), [](const Match& m) { return m.ambiguous(); }));
}

std::vector<package::Asset> missing(const project::ProjectData& data, const std::string& project_path)
{
    std::vector<package::Asset> out;
    for (auto& a : package::inventory(data, project_path).assets)
        if (!a.exists) out.push_back(std::move(a));
    return out;
}

SearchResult search(const std::vector<package::Asset>& missing_assets, const std::vector<std::string>& folders,
                    const Progress& progress)
{
    SearchResult res;
    std::vector<std::vector<std::string>> parts;
    std::multimap<std::string, std::size_t> by_name, by_stem;
    for (std::size_t i = 0; i < missing_assets.size(); ++i) {
        Match m;
        m.stored = missing_assets[i].stored;
        m.kind = missing_assets[i].kind;
        res.matches.push_back(std::move(m));
        parts.push_back(parts_reversed(missing_assets[i].stored));
        if (parts.back().empty()) continue;
        by_name.emplace(parts.back().front(), i);
        if (is_media(parts.back().front())) by_stem.emplace(stem_of(parts.back().front()), i);
    }
    if (missing_assets.empty()) return res;

    std::set<std::string> seen;  // a file reached through two folders counts once
    auto consider = [&](const fs::path& file) {
        const std::string name = lower(file.filename().u8string());
        auto add = [&](std::size_t i, bool same_name) {
            Candidate c;
            c.path = file.lexically_normal().generic_u8string();
            c.same_name = same_name;
            std::error_code ec;
            c.bytes = fs::file_size(file, ec);
            // Folders in common with the stored path, from the file upward.
            const auto found = parts_reversed(c.path);
            const auto& want = parts[i];
            int common = 0;
            for (std::size_t k = 1; k < want.size() && k < found.size() && want[k] == found[k]; ++k) ++common;
            c.score = (same_name ? 1000 : 0) + common * 10 + (file.filename().u8string() == fs::u8path(res.matches[i].stored).filename().u8string() ? 1 : 0);
            res.matches[i].candidates.push_back(std::move(c));
        };
        const auto [nb, ne] = by_name.equal_range(name);
        for (auto it = nb; it != ne; ++it) add(it->second, true);
        if (is_media(name)) {
            const auto [sb, se] = by_stem.equal_range(stem_of(name));
            for (auto it = sb; it != se; ++it)
                if (parts[it->second].front() != name) add(it->second, false);
        }
    };

    for (const auto& folder : folders) {
        if (res.cancelled) break;
        std::error_code ec;
        const fs::path root = fs::u8path(folder);
        if (!fs::is_directory(root, ec)) continue;
        fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
        for (; !ec && it != end; it.increment(ec)) {
            const auto& entry = *it;
            const std::string fname = entry.path().filename().u8string();
            std::error_code e2;
            if (entry.is_directory(e2)) {
                if (!fname.empty() && fname[0] == '.') it.disable_recursion_pending();
                continue;
            }
            if (!entry.is_regular_file(e2)) continue;
            ++res.files_scanned;
            const std::string key = entry.path().lexically_normal().generic_u8string();
            if (seen.insert(key).second) consider(entry.path());
            if (progress && res.files_scanned % 256 == 0 && !progress(res.files_scanned)) {
                res.cancelled = true;
                break;
            }
        }
    }

    for (auto& m : res.matches) {
        std::stable_sort(m.candidates.begin(), m.candidates.end(),
                         [](const Candidate& a, const Candidate& b) { return a.score > b.score; });
        if (m.candidates.empty() || !m.candidates.front().same_name) continue;
        if (m.candidates.size() == 1 || m.candidates[0].score > m.candidates[1].score) m.chosen = m.candidates.front().path;
    }
    if (progress) progress(res.files_scanned);
    return res;
}

std::string stored_path(const std::string& found, const std::string& project_path)
{
    const fs::path file = fs::u8path(found).lexically_normal();
    if (project_path.empty()) return file.generic_u8string();
    const fs::path dir = fs::u8path(project_path).parent_path().lexically_normal();
    if (!dir.empty() && inside(file, dir)) {
        const auto rel = file.lexically_relative(dir);
        if (!rel.empty()) return rel.generic_u8string();
    }
    return file.generic_u8string();
}

int apply(project::ProjectData& data, const std::map<std::string, std::string>& relinks, const std::string& project_path)
{
    std::map<std::string, std::string> to;
    for (const auto& [from, found] : relinks)
        if (!from.empty() && !found.empty()) to[from] = stored_path(found, project_path);
    int changed = 0;
    auto fix = [&](std::string& path) {
        const auto it = to.find(path);
        if (it == to.end() || it->second == path) return;
        path = it->second;
        ++changed;
    };
    for (auto& t : data.tracks)
        for (auto& v : t.versions)
            for (auto& c : v.audio_clips) fix(c.file);
    for (auto& p : data.pictures) fix(p.path);
    return changed;
}

}  // namespace vcc::session::relink
