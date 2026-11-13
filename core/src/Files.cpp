#include "vcc/core/Files.h"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

#if defined(_WIN32)
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <io.h>
#  include <windows.h>
#else
#  include <fcntl.h>
#  include <unistd.h>
#endif

namespace vcc::files {

namespace fs = std::filesystem;

std::optional<std::string> read_file(const std::string& path, std::string* error)
{
    std::ifstream in(fs::u8path(path), std::ios::binary);
    if (!in) {
        if (error) *error = "the file could not be opened";
        return std::nullopt;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    if (in.bad()) {
        if (error) *error = "reading the file failed";
        return std::nullopt;
    }
    return ss.str();
}

namespace {

std::atomic<unsigned> g_counter{0};

bool flush_to_disk(std::FILE* f)
{
    if (std::fflush(f) != 0) return false;
#if defined(_WIN32)
    return _commit(_fileno(f)) == 0;
#else
    return ::fsync(::fileno(f)) == 0;
#endif
}

bool replace_file(const std::string& from, const std::string& to)
{
#if defined(_WIN32)
    return MoveFileExW(fs::u8path(from).wstring().c_str(), fs::u8path(to).wstring().c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    return std::rename(from.c_str(), to.c_str()) == 0;
#endif
}

// UTF-8 paths on every platform: Windows' narrow C calls use the ANSI code
// page, so a name with an em dash or an accent would be mangled there.
std::FILE* open_for_writing(const std::string& path)
{
#if defined(_WIN32)
    return _wfopen(fs::u8path(path).wstring().c_str(), L"wb");
#else
    return std::fopen(path.c_str(), "wb");
#endif
}

void remove_file(const std::string& path)
{
    std::error_code ignored;
    fs::remove(fs::u8path(path), ignored);
}

void sync_directory_of(const std::string& path)
{
#if !defined(_WIN32)
    fs::path dir = fs::u8path(path).parent_path();
    if (dir.empty()) dir = ".";
    int dfd = ::open(dir.c_str(), O_RDONLY);
    if (dfd >= 0) {
        ::fsync(dfd);
        ::close(dfd);
    }
#else
    (void)path;
#endif
}

}  // namespace

std::string temporary_path_for(const std::string& path) { return path + ".vcc-partial"; }

FileWriteResult write_file_atomically(const std::string& path, const std::string& body)
{
    FileWriteResult result;
    if (path.empty()) {
        result.error = "no file was chosen";
        return result;
    }
    const std::string temporary = temporary_path_for(path) + "." + std::to_string(g_counter.fetch_add(1));

    std::FILE* f = open_for_writing(temporary);
    if (!f) {
        result.error = "could not open that folder for writing";
        return result;
    }
    bool ok = body.empty() || std::fwrite(body.data(), 1, body.size(), f) == body.size();
    ok = flush_to_disk(f) && ok;
    ok = (std::fclose(f) == 0) && ok;
    if (!ok) {
        remove_file(temporary);
        result.error = "writing the file failed";
        return result;
    }
    if (!replace_file(temporary, path)) {
        remove_file(temporary);
        result.error = "could not put the finished file in place";
        return result;
    }
    sync_directory_of(path);
    result.ok = true;
    result.byte_count = body.size();
    return result;
}

bool write_file_atomic(const std::string& path, const std::string& contents, std::string* error)
{
    FileWriteResult r = write_file_atomically(path, contents);
    if (!r.ok && error) *error = r.error;
    return r.ok;
}

}  // namespace vcc::files
