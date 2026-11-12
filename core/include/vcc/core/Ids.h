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

    /// "trk-0001", "note-0002", ... Width grows past 9999 as needed.
    std::string next(const char* prefix);

    std::uint64_t peek() const noexcept { return next_; }
    /// Never moves backwards; used when loading a file whose counter is ahead.
    void advance_to(std::uint64_t value) noexcept
    {
        if (value > next_) next_ = value;
    }

private:
    std::uint64_t next_;
};

}  // namespace vcc
