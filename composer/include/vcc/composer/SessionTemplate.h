// Copyright (c) VC Composer. All rights reserved.
//
// Instrumentation and session template generation (spec 02 sections 8, 12;
// acceptance criterion 9).
//
// Spec 8 asks VC Composer to "help establish a score before writing begins":
// suggest instrumentation, propose track names, assign staff type, clef,
// transposition and notation defaults per instrument, and associate each
// track with the user's preferred patch where one has been saved.
//
// Two things this module deliberately does not do. It does not create tracks
// in Pro Tools - that requires host capabilities which spec 14 says must be
// verified against Avid's documentation first, and which are not available
// here. And it does not choose a sound when the user has no favorite: spec
// 10.1 requires identifying the preferred patch and offering a guided load
// rather than "silently substituting another sound".
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "vcc/composer/InstrumentCatalog.h"
#include "vcc/music/MusicalTime.h"
#include "vcc/notation/Pitch.h"

namespace vcc::composer {

/// One proposed track in a template (spec 12: SessionTemplate holds ordered
/// tracks, names, instruments, patches and defaults).
struct TemplateTrack {
    /// Track name as it should appear in the host: "Violin I", "Horn 1".
    std::string track_name;

    /// The instrument definition this track is for, by name.
    std::string instrument_name;
    InstrumentFamily family = InstrumentFamily::Unknown;

    // Notation defaults, taken from the instrument definition so the staff
    // is right before a note is entered (spec 8).
    notation::Clef clef = notation::Clef::Treble;
    bool uses_grand_staff = false;
    notation::Clef lower_clef = notation::Clef::Bass;
    std::int8_t transposition_semitones = 0;
    std::int8_t octave_shift = 0;

    /// The patch to load, when the user has a favorite for this instrument.
    /// 0 means none was saved.
    std::uint64_t patch_id = 0;

    /// What to show the user about the sound. When `patch_id` is 0 this says
    /// no preference is saved, rather than naming a substitute.
    std::string patch_summary;

    /// True when the patch can be loaded automatically, false when spec
    /// 10.1's guided workflow is needed instead.
    bool patch_loads_automatically = false;

    /// Conventional score order, so a generated template reads like a score.
    std::int32_t score_order = 1000;

    /// MIDI channel suggestion, 0-15.
    std::uint8_t midi_channel = 0;
};

/// A reusable session template (spec 8: "Save reusable orchestral, chamber,
/// hybrid, band, piano, custom and project-specific templates").
struct SessionTemplate {
    std::string name;
    std::string description;

    /// Tracks in score order.
    std::vector<TemplateTrack> tracks;

    // Score defaults applied to every track's notation.
    music::TimeSignature time_signature{4, 4};
    notation::KeySignature key{};
    double tempo_bpm = 120.0;

    bool is_valid() const noexcept;

    /// Tracks whose instrument has no saved favorite. These are what the UI
    /// must prompt about, because VC Composer will not pick for the user
    /// (spec 10.1).
    std::vector<std::string> tracks_without_a_saved_patch() const;

    /// Tracks whose saved patch cannot be loaded automatically and so need
    /// the guided one-step workflow.
    std::vector<std::string> tracks_needing_guided_loading() const;
};

/// What the user asks for when requesting instrumentation.
struct InstrumentationRequest {
    /// A starting ensemble, or `Custom` to supply instruments directly.
    enum class Ensemble : std::uint8_t {
        Custom = 0,
        /// Flute, oboe, clarinet, bassoon, horn, trumpet, timpani, strings.
        SmallOrchestra,
        /// Full woodwind, brass, percussion and string sections.
        FullOrchestra,
        /// String quartet.
        StringQuartet,
        /// Wind quintet.
        WindQuintet,
        /// Piano alone, on a grand staff.
        SoloPiano,
        /// Piano trio.
        PianoTrio,
        /// Strings only, five sections.
        StringOrchestra,
        /// Concert band.
        ConcertBand,
        /// Strings, piano, guitar and drums.
        HybridRhythmSection,
    };

    Ensemble ensemble = Ensemble::Custom;

    /// Instrument definition names, used when `ensemble` is `Custom` and
    /// appended to the ensemble otherwise.
    std::vector<std::string> additional_instruments;

    /// Divide string sections into desks, for a larger score.
    bool divide_strings = false;

    /// Assign each track its own MIDI channel, cycling after 16.
    bool assign_channels = true;

    music::TimeSignature time_signature{4, 4};
    notation::KeySignature key{};
    double tempo_bpm = 120.0;
};

const char* to_string(InstrumentationRequest::Ensemble ensemble) noexcept;

/// The instrument names an ensemble is made of, in score order.
std::vector<std::string> ensemble_instruments(InstrumentationRequest::Ensemble ensemble);

/// Builds a proposed template (spec 8).
///
/// The catalogue is consulted for each instrument's favorite patch, so a
/// user who has saved a preferred violin pizzicato gets it proposed
/// automatically (acceptance criteria 8 and 9). Where no favorite exists the
/// track is still created with correct notation defaults and is flagged as
/// needing a choice - never filled with a substitute.
///
/// Nothing is created in the host: spec 8 says "create or guide creation of
/// the required MIDI/instrument tracks using only supported Pro Tools
/// interfaces", and which of those two it is depends on host capabilities
/// that must be verified against Avid's documentation first (spec 14).
SessionTemplate generate_template(const InstrumentationRequest& request,
                                  const InstrumentCatalog& catalog);

/// A library of saved templates.
class TemplateLibrary {
public:
    /// Adds or replaces a template by name. Returns false if invalid.
    bool save(const SessionTemplate& session_template);

    bool remove(const std::string& name);

    const SessionTemplate* find(const std::string& name) const noexcept;

    const std::vector<SessionTemplate>& templates() const noexcept { return templates_; }
    std::size_t size() const noexcept { return templates_.size(); }

    void clear() { templates_.clear(); }

private:
    std::vector<SessionTemplate> templates_;
};

}  // namespace vcc::composer
