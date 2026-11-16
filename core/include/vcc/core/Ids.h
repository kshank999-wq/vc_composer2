// Stable identifiers.
//
// Main Spec §8 and §20: the same track ID and label appear in the Composer,
// Sequencer and Mixer views, and a note keeps its ID through edits, saves and
// version switches. IDs are short strings with a type prefix ("trk-0007",
// "bus-0002"). They are allocated from a per-project counter that is saved
// with the project, so an ID is never reused - not even after undo - and two
// loads of the same file produce the same IDs.
#pragma once

#include <cstdint>
#include <string>

namespace vcc {

class IdAllocator {
public:
    explicit IdAllocator(std::uint64_t next = 1) noexcept : next_(next) {}

    /// "trk-0001", "note-0002", ... Width grows past 9999 as needed. With a
    /// site (a shared project's copy, collaboration): "trk-1a2b-0001", so
    /// two people's new tracks never share an id.
    std::string next(const char* prefix);
    void set_site(std::uint32_t site) noexcept { site_ = site; }
    std::uint32_t site() const noexcept { return site_; }

    std::uint64_t peek() const noexcept { return next_; }
    /// Never moves backwards; used when loading a file whose counter is ahead.
    void advance_to(std::uint64_t value) noexcept
    {
        if (value > next_) next_ = value;
    }

private:
    std::uint64_t next_;
    std::uint32_t site_ = 0;
};

}  // namespace vcc
