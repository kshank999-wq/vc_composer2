#include "vcc/core/Files.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
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
        if (error) *error = "cannot open " + path;
        return std::nullopt;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    if (in.bad()) {
        if (error) *error = "read failed: " + path;
        return std::nullopt;
    }
    return ss.str();
}

namespace {

std::string temp_name_for(const fs::path& target)
{
    std::random_device rd;
    std::uniform_int_distribution<unsigned> dist(0, 0xFFFFFF);
    char suffix[16];
    std::snprintf(suffix, sizeof suffix, ".%06x.tmp", dist(rd));
    return target.string() + suffix;
}

bool flush_to_disk(std::FILE* f)
{
    if (std::fflush(f) != 0) return false;
#if defined(_WIN32)
    return _commit(_fileno(f)) == 0;
#else
    return ::fsync(::fileno(f)) == 0;
#endif
}

bool replace_file(const fs::path& from, const fs::path& to)
{
#if defined(_WIN32)
    return MoveFileExW(from.wstring().c_str(), to.wstring().c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    return std::rename(from.c_str(), to.c_str()) == 0;
#endif
}

}  // namespace

bool write_file_atomic(const std::string& path, const std::string& contents, std::string* error)
{
    const fs::path target = fs::u8path(path);
    const fs::path temp = fs::u8path(temp_name_for(target));

    std::FILE* f = std::fopen(temp.string().c_str(), "wb");
    if (!f) {
        if (error) *error = "cannot create temporary beside " + path + ": " + std::strerror(errno);
        return false;
    }
    bool ok = contents.empty() || std::fwrite(contents.data(), 1, contents.size(), f) == contents.size();
    ok = ok && flush_to_disk(f);
    int saved_errno = errno;
    ok = (std::fclose(f) == 0) && ok;
    if (!ok) {
        std::error_code ec;
        fs::remove(temp, ec);
        if (error) *error = "write failed for " + path + ": " + std::strerror(saved_errno);
        return false;
    }
    if (!replace_file(temp, target)) {
        saved_errno = errno;
        std::error_code ec;
        fs::remove(temp, ec);
        if (error) *error = "cannot replace " + path + ": " + std::strerror(saved_errno);
        return false;
    }
#if !defined(_WIN32)
    // Make the rename itself durable.
    fs::path dir = target.parent_path();
    if (dir.empty()) dir = ".";
    int dfd = ::open(dir.c_str(), O_RDONLY);
    if (dfd >= 0) {
        ::fsync(dfd);
        ::close(dfd);
    }
#endif
    return true;
}

}  // namespace vcc::files
