// File reading and crash-safe writing.
//
// Main Spec §12/§17: "crash-resilient journaling", "atomic saves". Every save
// in VC Composer 2 goes through write_file_atomic: the bytes go to a temporary
// file beside the target, are flushed to disk, and only then replace the
// target in one rename. A crash, full disk or I/O error at any point leaves
// the previous file exactly as it was.
//
// One implementation serves both the VC Composer 2 API (write_file_atomic)
// and the original VC Composer API (write_file_atomically), which the ported
// notation and MIDI-file code use. Error messages never contain the path:
// a film title in a folder name should not end up in a log or a bug report.
#pragma once

#include <cstddef>
#include <optional>
#include <string>

namespace vcc::files {

struct FileWriteResult {
    bool ok = false;
    std::string error;        ///< Plain words, never the path.
    std::size_t byte_count = 0;
};

/// Writes `body` to `path` through a scratch file beside it, flushed to disk
/// before it replaces the target. On any failure the previous file is left
/// exactly as it was and no scratch file remains.
FileWriteResult write_file_atomically(const std::string& path, const std::string& body);

/// The prefix of the scratch files used for `path` ("<path>.vcc-partial").
std::string temporary_path_for(const std::string& path);

/// Reads a whole file. Returns nullopt (and sets `error`) on failure.
std::optional<std::string> read_file(const std::string& path, std::string* error = nullptr);

/// Writes `contents` to `path` atomically. Returns false and sets `error` on
/// failure, in which case `path` is untouched and no temporary is left.
bool write_file_atomic(const std::string& path, const std::string& contents,
                       std::string* error = nullptr);

}  // namespace vcc::files
