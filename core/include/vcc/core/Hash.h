// SHA-256 (FIPS 180-4): the content identity of a shared project's media
// files and revisions. Streaming, so a picture file of several gigabytes is
// hashed without being held in memory.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace vcc::hash {

class Sha256 {
public:
    Sha256() noexcept { reset(); }
    void reset() noexcept;
    void update(const void* data, std::size_t size) noexcept;
    /// The digest as 64 lowercase hex digits. Ends this hash (reset to reuse).
    std::string hex();

private:
    void block(const std::uint8_t* p) noexcept;
    std::array<std::uint32_t, 8> h_{};
    std::array<std::uint8_t, 64> buf_{};
    std::size_t used_ = 0;
    std::uint64_t bits_ = 0;
};

std::string sha256_hex(const std::string& data);
/// A file's digest; nullopt if it cannot be read. `progress` (bytes read so
/// far) may return false to stop (then nullopt).
std::optional<std::string> sha256_file(const std::string& path,
                                       const std::function<bool(std::uint64_t)>& progress = {});

}  // namespace vcc::hash
