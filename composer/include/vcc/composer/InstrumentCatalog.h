// Copyright (c) VC Composer. All rights reserved.
//
// Instrument definitions and the sample-library catalogue (spec 02
// sections 8, 10, 12; acceptance criterion 8).
//
// Two separate things live here, and keeping them separate is the point:
//
//   * An InstrumentDefinition is musical knowledge - a viola reads alto
//     clef and sounds where it is written, a B-flat clarinet is written a
//     major second high. This is true regardless of what samples anyone
//     owns, so it ships with the application.
//   * A LibraryPatch is a specific sound the user owns - a particular
//     violin pizzicato in a particular library, loaded by a particular
//     plug-in. This is personal, and it is what spec 10 asks VC Composer to
//     remember so "the composer's favorite patches become the default
//     choices when VC Composer builds a session".
//
// Spec 13 requires third-party integrations stay modular and that no
// library's behaviour be hard-coded. Accordingly a patch is described by
// data the user supplies or confirms, never by assumptions about a product.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "vcc/notation/ExpressionMap.h"
#include "vcc/notation/Pitch.h"

namespace vcc::composer {

/// Orchestral families, used for grouping and for template generation.
enum class InstrumentFamily : std::uint8_t {
    Unknown = 0,
    Woodwind,
    Brass,
    Percussion,
    UnpitchedPercussion,
    Keyboard,
    Harp,
    Guitar,
    Strings,
    Voice,
    Synth,
};

const char* to_string(InstrumentFamily family) noexcept;
bool family_from_string(const std::string& text, InstrumentFamily& out) noexcept;

/// Musical facts about an instrument (spec 12: InstrumentDefinition).
struct InstrumentDefinition {
    std::string name;              ///< "Violin", "Clarinet in Bb".
    std::string short_name;        ///< "Vln.", "Cl."
    InstrumentFamily family = InstrumentFamily::Unknown;

    notation::Clef clef = notation::Clef::Treble;

    /// A second staff, for piano, harp and organ (spec 5.1).
    bool uses_grand_staff = false;
    notation::Clef lower_clef = notation::Clef::Bass;

    /// Written-to-sounding transposition in semitones. A B-flat clarinet is
    /// -2: written C sounds B-flat.
    std::int8_t transposition_semitones = 0;

    /// Octave displacement from an octave clef, in octaves.
    std::int8_t octave_shift = 0;

    /// Playable range in MIDI note numbers, sounding pitch. Used to warn
    /// that a note is outside what the instrument can play - a real
    /// compositional error, not a technical one.
    std::uint8_t lowest_note = 0;
    std::uint8_t highest_note = 127;

    /// Conventional score order, low numbers first. Woodwinds above brass
    /// above percussion above strings, as a score is laid out.
    std::int32_t score_order = 1000;

    bool is_valid() const noexcept;

    /// Total written-to-sounding shift in semitones, combining the
    /// transposition and any octave displacement. Mirrors
    /// notation::Staff::total_transposition, which is what this populates.
    int total_transposition() const noexcept
    {
        return transposition_semitones + octave_shift * 12;
    }

    /// True when a sounding note is inside the instrument's range.
    bool can_play(int sounding_midi_note) const noexcept;
};

/// The instrument definitions that ship with the application.
///
/// Standard orchestral knowledge - clefs, transpositions, ranges, score
/// order - not anything specific to a library or a product.
const std::vector<InstrumentDefinition>& standard_instruments();

/// Finds a standard instrument by name, case-sensitively. Returns nullptr
/// when absent rather than inventing a definition, since a guessed
/// transposition would silently transpose a part wrongly.
const InstrumentDefinition* find_standard_instrument(const std::string& name) noexcept;

/// A specific sound the user owns (spec 10, spec 12: LibraryPatch).
struct LibraryPatch {
    /// Stable identifier, assigned by the catalogue.
    std::uint64_t id = 0;

    /// Who makes the library.
    std::string provider;

    /// The plug-in needed to load it - "Kontakt", "Opus". Recorded because
    /// spec 10 requires knowing "virtual-instrument plug-in required to
    /// load the patch".
    std::string player;

    /// The library within that player.
    std::string library;

    /// The patch or preset, as the user would see it: "Violin Pizzicato",
    /// "Oboe Solo Legato".
    std::string patch_name;

    /// Whatever the user or the library can give us to find the patch
    /// again: a preset path, an identifier, a snapshot name. Spec 10 is
    /// careful about this - "subject to the host/library's supported APIs" -
    /// so it is free-form and may be empty, in which case loading is a
    /// guided manual step rather than an automatic one.
    std::string patch_identifier;

    /// The instrument this patch is a sound for, by definition name.
    std::string instrument_name;
    InstrumentFamily family = InstrumentFamily::Unknown;

    /// The expression map for this patch, which is where per-library
    /// controller and keyswitch differences live (spec 7, 10).
    notation::ExpressionMap expression_map;

    /// The default choice for its instrument when a template is generated
    /// (spec 10.1). Only one patch per instrument can hold this.
    bool is_favorite = false;

    /// Free-form tags: "warm", "aggressive", "pizzicato", "solo" (spec 10).
    std::vector<std::string> tags;

    std::string notes;
    /// 0 means unrated; 1-5 otherwise.
    std::uint8_t rating = 0;

    bool is_valid() const noexcept;
    bool has_tag(const std::string& tag) const noexcept;

    /// How it should read in a list: "Violin Pizzicato (Kontakt / Library)".
    std::string display_name() const;

    /// True when enough is known to attempt automatic loading. When false,
    /// spec 10.1 requires a guided one-step workflow rather than silently
    /// substituting another sound.
    bool can_attempt_automatic_load() const noexcept;
};

/// The user's personal catalogue of owned sounds (spec 10).
///
/// Nothing here scans a disk or talks to a plug-in: it is the record of what
/// the user told us they own, which keeps it testable and keeps library
/// integration modular as spec 13 requires.
class InstrumentCatalog {
public:
    // --- Patches ------------------------------------------------------------

    /// Adds a patch and returns its assigned id, or 0 if rejected.
    std::uint64_t add_patch(const LibraryPatch& patch);

    bool remove_patch(std::uint64_t id);

    /// Replaces a patch, keeping its id.
    bool update_patch(std::uint64_t id, const LibraryPatch& patch);

    const LibraryPatch* find_patch(std::uint64_t id) const noexcept;

    const std::vector<LibraryPatch>& patches() const noexcept { return patches_; }
    std::size_t patch_count() const noexcept { return patches_.size(); }

    // --- Favorites ----------------------------------------------------------

    /// Marks a patch as the default for its instrument, clearing the flag
    /// from any other patch for the same instrument.
    ///
    /// Enforcing one favorite per instrument is what makes "recall the
    /// favorite" unambiguous (spec 10.1, acceptance criterion 8).
    bool set_favorite(std::uint64_t id);

    bool clear_favorite(std::uint64_t id);

    /// The favorite patch for an instrument, or nullptr when none is set.
    const LibraryPatch* favorite_for_instrument(const std::string& instrument_name) const noexcept;

    std::vector<LibraryPatch> favorites() const;

    // --- Queries ------------------------------------------------------------

    std::vector<LibraryPatch> patches_for_instrument(const std::string& instrument_name) const;
    std::vector<LibraryPatch> patches_for_family(InstrumentFamily family) const;
    std::vector<LibraryPatch> patches_with_tag(const std::string& tag) const;

    /// Case-insensitive substring search over provider, library, patch and
    /// instrument names, plus tags.
    std::vector<LibraryPatch> search(const std::string& text) const;

    /// Every distinct player named by a patch, so the UI can group by the
    /// plug-in that has to be loaded.
    std::vector<std::string> players() const;

    void clear();

private:
    std::uint64_t next_id() noexcept { return ++last_id_; }

    std::vector<LibraryPatch> patches_;
    std::uint64_t last_id_ = 0;
};

}  // namespace vcc::composer
