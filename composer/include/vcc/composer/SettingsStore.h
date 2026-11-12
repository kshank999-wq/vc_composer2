// Copyright (c) VC Composer. All rights reserved.
//
// Versioned local settings persistence (spec 02 section 13).
//
// Spec 13 requires: "Persist user favorites, mappings, shortcut profiles and
// templates in a versioned local settings database/file."
//
// The format is deliberately plain text with sections and keys, not a
// database and not JSON. Three reasons. It has no dependency, which keeps
// the module testable and the build free of another third-party library. It
// is human-readable and hand-editable, which matters when a user's favorite
// patch identifier needs correcting. And it is trivially diffable, so a
// user can keep their settings in version control if they want to.
//
// Robustness is the priority over compactness: this file holds work the user
// cannot easily recreate - their catalogue of patches, their edited
// expression maps, their templates. A parser that throws away a whole file
// because one line is malformed would be the worst possible behaviour, so
// unreadable lines are skipped and reported rather than being fatal.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace vcc::composer {

/// Current format version. Bumped when the layout changes in a way a reader
/// has to know about; the version is written into every file so an older
/// file can be migrated rather than rejected.
///
/// 1: the first format, with ticks at 960 to the quarter note.
/// 2: ticks at 2880 to the quarter note, the notation engine's. Every
///    tick-valued key - one whose last name starts with `ticks_` or ends
///    with `_tick` or `_ticks`, which is how every such key is named - is
///    multiplied by three on the way up.
constexpr int kSettingsFormatVersion = 2;

/// A section of key-value pairs.
///
/// Nested structures - a template's ordered tracks, a patch's tag list - are
/// expressed with indexed keys such as `track.0.name`, which keeps the
/// format flat and the parser simple.
class SettingsSection {
public:
    void set(const std::string& key, const std::string& value);
    void set_int(const std::string& key, std::int64_t value);
    void set_double(const std::string& key, double value);
    void set_bool(const std::string& key, bool value);

    /// Reads a value, returning `fallback` when the key is absent or, for
    /// the typed accessors, when the value does not parse. A corrupt value
    /// yielding a documented default is better than a hard failure on a
    /// file the user cannot easily recreate.
    std::string get(const std::string& key, const std::string& fallback = {}) const;
    std::int64_t get_int(const std::string& key, std::int64_t fallback = 0) const;
    double get_double(const std::string& key, double fallback = 0.0) const;
    bool get_bool(const std::string& key, bool fallback = false) const;

    /// What reading one setting found.
    ///
    /// Three outcomes, because the `get_` calls above collapse two of them
    /// and they want opposite treatment. A key that is **not there** is
    /// ordinary - a first run, or a file written before the setting existed
    /// - and the default standing is exactly right, silently. A key that is
    /// **there and says something that is not a number** is the composer's
    /// file disagreeing with this build, and letting the default stand
    /// without a word is how `ticks_before_playhead = 12x` turned into a
    /// setting that looked like it had been applied.
    ///
    /// This file's own rule is that such a value "keeps the default and is
    /// **named**". The naming needs somewhere to learn there was anything to
    /// name, which a fallback cannot provide: the fallback it returns is
    /// indistinguishable from the value having been read.
    enum class Reading {
        Absent,
        Read,
        Unusable,
    };

    /// Reads into `out`, leaving it untouched unless the answer is `Read`.
    Reading read_int(const std::string& key, std::int64_t& out) const;
    Reading read_double(const std::string& key, double& out) const;
    Reading read_bool(const std::string& key, bool& out) const;

    bool has(const std::string& key) const;
    bool remove(const std::string& key);

    /// Keys under an indexed prefix. For `track` this returns the indices
    /// present, so a list can be read back without storing its length.
    std::vector<std::size_t> indices(const std::string& prefix) const;

    const std::map<std::string, std::string>& values() const noexcept { return values_; }
    std::size_t size() const noexcept { return values_.size(); }
    bool empty() const noexcept { return values_.empty(); }

    void clear() { values_.clear(); }

private:
    std::map<std::string, std::string> values_;
};

/// A settings document: an ordered set of named sections plus a version.
class SettingsStore {
public:
    SettingsStore() = default;

    /// The format version this document was read from, or the current
    /// version for a new one.
    int version() const noexcept { return version_; }
    void set_version(int version) noexcept { version_ = version; }

    SettingsSection& section(const std::string& name);
    const SettingsSection* find_section(const std::string& name) const noexcept;
    bool has_section(const std::string& name) const noexcept;
    bool remove_section(const std::string& name);

    /// Section names, in insertion order, so a written file is stable.
    std::vector<std::string> section_names() const;

    std::size_t section_count() const noexcept { return order_.size(); }

    /// Serializes to text.
    std::string to_text() const;

    /// Parses text. Lines that cannot be understood are counted in
    /// `skipped_lines` rather than failing the whole parse - see the note at
    /// the top of this file.
    ///
    /// Returns false only for input that is not a settings document at all:
    /// a missing or unreadable version header.
    bool from_text(const std::string& text, std::size_t* skipped_lines = nullptr);

    void clear();

private:
    int version_ = kSettingsFormatVersion;
    std::map<std::string, SettingsSection> sections_;
    std::vector<std::string> order_;
};

/// Escapes a value so it survives a round trip through the text format:
/// newlines, leading and trailing whitespace, and the escape character
/// itself would otherwise be lost or change the parse.
std::string escape_value(const std::string& value);

/// Reverses `escape_value`.
std::string unescape_value(const std::string& value);

}  // namespace vcc::composer
