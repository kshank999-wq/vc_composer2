// File reading and crash-safe writing.
//
// Main Spec §12/§17: "crash-resilient journaling", "atomic saves". Every save
// in VC Composer 2 goes through write_file_atomic: the bytes go to a temporary
// file beside the target, are flushed to disk, and only then replace the
// target in one rename. A crash, full disk or I/O error at any point leaves
// the previous file exactly as it was.
#pragma once

#include <optional>
#include <string>

namespace vcc::files {

/// Reads a whole file. Returns nullopt (and sets `error`) on failure.
std::optional<std::string> read_file(const std::string& path, std::string* error = nullptr);

/// Writes `contents` to `path` atomically. Returns false and sets `error` on
/// failure, in which case `path` is untouched and no temporary is left.
bool write_file_atomic(const std::string& path, const std::string& contents,
                       std::string* error = nullptr);

}  // namespace vcc::files
