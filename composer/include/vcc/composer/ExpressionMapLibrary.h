// Copyright (c) VC Composer. All rights reserved.
//
// The expression maps a composer has, and how they are kept (spec 02
// sections 7, 12, 13).
//
// Spec 7 requires that a written mezzo-forte not be hard-coded to one MIDI
// behaviour, and the engine's `ExpressionMap` is that: a per-patch mapping
// from dynamics and articulations to velocities, controllers and
// keyswitches. Spec 13 requires the mappings be persisted in the versioned
// settings file. What was missing between the two was any *collection* of
// maps the application holds, any way to keep one, and any way for a part
// to say which it plays through. This is that collection.
//
// Two maps are built in - a general one and an orchestral one - and cannot
// be edited in place, for the reason the built-in shortcut profiles cannot:
// a composer must always have the reference they started from. Editing a
// built-in map copies it first. Everything else in the library is the
// composer's, and goes in the settings file as a section per map
// (`[expression_map.0]`, `[expression_map.1]`, ...), written whole each
// time so a map deleted today does not come back from a longer list
// tomorrow.
//
// The keys are the map's own vocabulary - "scale length", "control change",
// an articulation by name - because a settings file a composer can read is
// one they can correct, and because a number that meant "keyswitch" would
// stop meaning it the moment somebody inserted a value into the enum.
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "vcc/composer/SettingsStore.h"
#include "vcc/notation/ExpressionMap.h"

namespace vcc::composer {

/// What a map's section is called, with its index after a dot.
constexpr const char* kExpressionMapSectionPrefix = "expression_map";

/// The two maps every installation has.
constexpr const char* kDefaultExpressionMapName = "Default";
constexpr const char* kOrchestralExpressionMapName = "Orchestral (CC dynamics)";

struct ExpressionMapLibrary {
    /// The built-in maps first, in a fixed order, then the composer's in the
    /// order they were made. Names are unique.
    std::vector<notation::ExpressionMap> maps;

    const notation::ExpressionMap* find(const std::string& name) const noexcept;
    std::size_t index_of(const std::string& name) const noexcept;
    bool is_built_in(const std::string& name) const noexcept;

    /// The map a part with no map of its own plays through.
    const notation::ExpressionMap& default_map() const noexcept;

    /// Adds a map, or replaces the composer's map of that name. Refuses a
    /// built-in name, an empty name, and an invalid map.
    bool store(const notation::ExpressionMap& map);

    /// Removes a composer's map. Refuses a built-in.
    bool remove(const std::string& name);

    /// The name after `name`, wrapping at the end, for a control that steps
    /// through them. A name that is not in the library steps to the first.
    std::string next_after(const std::string& name) const;

    /// A name no map has yet, for a copy of `name`: "Default copy", then
    /// "Default copy 2", and so on.
    std::string copy_name_for(const std::string& name) const;

    std::size_t size() const noexcept { return maps.size(); }
};

/// A library holding only the built-in maps.
ExpressionMapLibrary built_in_expression_maps();

void expression_map_to_settings(const notation::ExpressionMap& map,
                                composer::SettingsSection& out);

/// Reads one map back. Returns false when nothing usable could be read - no
/// name, or a map the engine would refuse - and names in `warnings` every
/// value that was there and could not be used, each of which keeps its
/// default. A rule that will not read is left out and named.
bool expression_map_from_settings(const composer::SettingsSection& section,
                                  notation::ExpressionMap& out,
                                  std::vector<std::string>& warnings);

/// Writes the composer's maps into the store, one section each, replacing
/// whatever map sections it held. The built-ins are not written: they are
/// this build's, and a file that carried them would pin an older version's
/// defaults over a newer one's.
void expression_maps_to_store(const ExpressionMapLibrary& library, composer::SettingsStore& store);

/// Reads the composer's maps from the store into a library that already
/// holds the built-ins. A map that cannot be read is named and skipped; one
/// that reads but shares a built-in's name is named and skipped too, since
/// a composer's "Default" that silently replaced the real one would make
/// every part play differently without a word.
std::vector<std::string> expression_maps_from_store(const composer::SettingsStore& store,
                                                    ExpressionMapLibrary& out);

}  // namespace vcc::composer
