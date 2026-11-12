// Copyright (c) VC Composer. All rights reserved.
//
// Temporary files for tests that write one.
//
// Unique per process and per case, so the suite can run in parallel and no
// case can read a file an earlier run left behind - which is the way a
// file-writing test passes while writing nothing at all.
#pragma once

#include <atomic>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace vcc::test {

/// The directory temporary test files go in.
inline std::string temp_directory()
{
#if defined(_WIN32)
    char buffer[MAX_PATH];
    const DWORD length = ::GetTempPathA(MAX_PATH, buffer);
    if (length > 0 && length < MAX_PATH)
        return std::string(buffer, length);
    return ".";
#else
    if (const char* from_environment = std::getenv("TMPDIR"))
        return from_environment;
    return "/tmp";
#endif
}

/// A path no other case will use. The file is not created.
inline std::string unique_temp_path(const char* tag, const char* extension)
{
    static std::atomic<unsigned> counter{0};
    const unsigned index = counter.fetch_add(1) + 1;

#if defined(_WIN32)
    const unsigned long pid = static_cast<unsigned long>(::GetCurrentProcessId());
#else
    const unsigned long pid = static_cast<unsigned long>(::getpid());
#endif

    std::string path = temp_directory();
    if (!path.empty() && path.back() != '/' && path.back() != '\\')
        path += '/';
    path += "vcc-";
    path += tag;
    path += '-';
    path += std::to_string(pid);
    path += '-';
    path += std::to_string(index);
    path += extension;
    return path;
}

/// Reads a whole file. Returns false when it does not exist or cannot be
/// read, which is how a case checks that nothing was written.
inline bool read_file(const std::string& path, std::vector<std::uint8_t>& out)
{
    out.clear();
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return false;
    out.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    return true;
}

/// Removes a file, ignoring its absence. Tests call this rather than leaving
/// their output in the temp directory.
inline void remove_file(const std::string& path)
{
    std::remove(path.c_str());
}

/// Removes a temporary file when it goes out of scope, so a failing
/// assertion does not leave one behind.
class TempFile {
public:
    TempFile(const char* tag, const char* extension)
        : path_(unique_temp_path(tag, extension))
    {
    }

    /// Adopts a path chosen by the caller - a relative one, say, for a case
    /// that is about relative paths. Removed on destruction like any other,
    /// which is what keeps a *failing* case from leaving a file behind: an
    /// assertion throws, and the remaining cleanup in the case body never
    /// runs.
    explicit TempFile(std::string path) : path_(std::move(path)) {}

    ~TempFile() { remove_file(path_); }

    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;

    const std::string& path() const noexcept { return path_; }

private:
    std::string path_;
};

}  // namespace vcc::test
