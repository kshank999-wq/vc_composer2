// The audio and picture files of a shared project (Main Spec §15, §16).
//
// The project document refers to media by path. To share them every path
// must mean the same on every computer, so before a push the media outside
// the project's folder is gathered into it ("Audio Files/", "Picture/") and
// relinked: from then on paths are relative and portable.
//
// Each revision carries a manifest - every media path its project uses, with
// the file's SHA-256 and size. The server keeps each file once per project by
// content, so a file is uploaded once however many revisions use it, and a
// pull downloads only what this computer lacks.
//
// Two people can make different files at the same path (both record "Vox
// take 1"). Before merging, a file of mine that differs from theirs at the
// same path, and that I made or changed since the base, is copied to a new
// name and my project relinked to it: both takes survive the merge.
//
// Paths from other people are written to this computer: only plain relative
// paths inside the project folder are accepted (safe_media_path).
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "vcc/core/Json.h"
#include "vcc/project/Project.h"

namespace vcc::session::shared {

struct MediaEntry {
    std::string path;    ///< Relative to the project's folder, '/' separated.
    std::string sha256;  ///< 64 lowercase hex digits.
    std::uint64_t size = 0;
    friend bool operator==(const MediaEntry& a, const MediaEntry& b)
    {
        return a.path == b.path && a.sha256 == b.sha256 && a.size == b.size;
    }
};
using Manifest = std::vector<MediaEntry>;

/// The largest file shared (the server's limit too).
constexpr std::uint64_t kMaxMediaBytes = 2ull * 1024 * 1024 * 1024;

/// Relative, inside the folder: no "..", no absolute path, no drive, no backslash.
bool safe_media_path(const std::string& path);
json::Value manifest_to_json(const Manifest& m);
/// Unsafe or malformed entries are dropped.
Manifest manifest_from_json(const json::Value& v);
const MediaEntry* find_media(const Manifest& m, const std::string& path);
/// The same files at the same paths, in any order.
bool same_media(Manifest a, Manifest b);
/// Where a manifest path is on this computer.
std::string media_file(const std::string& project_file, const std::string& path);

/// Hashes media with a cache beside the project (<file>.shared-media.json),
/// keyed by path and checked against size and modification time, so a large
/// picture file is hashed once, not at every sync.
class HashCache {
public:
    explicit HashCache(std::string project_file);
    /// The digest of the file at a manifest path; nullopt if it is not there.
    std::optional<std::string> sha256(const std::string& path, const std::function<bool(std::uint64_t)>& progress = {});
    /// Records a digest already known (a file just downloaded and checked).
    void note(const std::string& path, const std::string& sha256);
    bool save() const;

private:
    std::string project_file_;
    json::Value cache_;
};

/// A media file outside the project folder and where it is gathered to.
struct Copy {
    std::string stored;  ///< As the project has it now.
    std::string from;    ///< Absolute.
    std::string to;      ///< Absolute, inside the project folder.
};
/// What gathering `data` would copy (nothing for files already inside).
std::vector<Copy> plan_gather(const project::ProjectData& data, const std::string& project_file);
/// Copies; false (and stops) at the first failure.
bool run_copies(const std::vector<Copy>& copies, std::string* error = nullptr);
/// For relink::apply: stored path -> the file now used.
std::map<std::string, std::string> relinks_of(const std::vector<Copy>& copies);

/// What `data` uses, as a manifest.
struct Listed {
    Manifest manifest;
    std::vector<std::string> missing;    ///< Used but not on this computer (and not known).
    std::vector<std::string> too_large;  ///< Over kMaxMediaBytes: not shared.
    std::vector<std::string> outside;    ///< Still outside the folder (gather first).
};
/// A file used but missing here keeps the entry `known` has for its path
/// (the base's or theirs: the server has it).
Listed list_media(const project::ProjectData& data, const std::string& project_file, HashCache& cache,
                  const Manifest& known = {}, const std::function<bool(std::uint64_t)>& progress = {});

/// Before merging: my files that clash with theirs - same path, other
/// content, and made or changed by me since the base - copied to a new name
/// (" (site 1a2b)" before the extension). Returns stored path -> new file,
/// for relink::apply on my project.
std::map<std::string, std::string> separate_clashes(const Manifest& ours, const Manifest& base, const Manifest& theirs,
                                                    const std::string& project_file, std::uint32_t site,
                                                    std::string* error = nullptr);

/// After merging: the entries of `theirs` that `merged` uses and this
/// computer lacks or has with other content.
Manifest to_fetch(const project::ProjectData& merged, const Manifest& theirs, const std::string& project_file, HashCache& cache);

}  // namespace vcc::session::shared
