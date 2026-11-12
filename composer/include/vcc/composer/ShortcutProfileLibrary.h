// Copyright (c) VC Composer. All rights reserved.
//
// The shortcut profiles a composer has, and how they are kept (spec 02
// sections 11, 13).
//
// Spec 11 asks for Finale-style, Sibelius-style and default profiles, "all
// editable", and `composer/ShortcutProfile.h` has long been those: three
// built-ins, each copyable into an editable profile of the composer's own.
// Spec 13 asks that shortcut profiles be persisted in the settings file.
// What was missing between the two was any *collection* the application
// holds, any way to keep one between runs, and any way for the composer to
// say which one the window uses. This is that collection, shaped like the
// expression-map library and for the same reasons.
//
// The built-ins cannot be edited in place or removed: a composer must always
// have the reference they started from. Editing one copies it first. The
// composer's own profiles go in the settings file as a section per profile
// (`[shortcut_profile.0]`, `[shortcut_profile.1]`, ...), written whole each
// time, with one key per binding in the command's stable name and the
// stroke's own text - `bind.Undo = Ctrl+Z` - so a file a composer reads is
// one they can correct. A profile records the platform it was written on,
// and is remapped when it is read on the other, so Cmd+Z on a Mac is Ctrl+Z
// on Windows rather than a dead key.
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "vcc/composer/SettingsStore.h"
#include "vcc/composer/ShortcutProfile.h"

namespace vcc::composer {

/// What a profile's section is called, with its index after a dot.
constexpr const char* kShortcutProfileSectionPrefix = "shortcut_profile";

/// The section that says which profile the window uses, and its one key.
constexpr const char* kShortcutsSettingsSection = "shortcuts";
constexpr const char* kChosenShortcutProfileKey = "profile";

/// The built-in profiles' names, as `composer/ShortcutProfile.cpp` gives
/// them. The first is the one the window uses until a composer chooses.
constexpr const char* kVcComposerProfileName = "VC Composer";
constexpr const char* kFinaleStyleProfileName = "Finale style";
constexpr const char* kSibeliusStyleProfileName = "Sibelius style";

struct ShortcutProfileLibrary {
    /// The built-in profiles first, in a fixed order, then the composer's in
    /// the order they were made. Names are unique.
    std::vector<composer::ShortcutProfile> profiles;

    const composer::ShortcutProfile* find(const std::string& name) const noexcept;
    composer::ShortcutProfile* find(const std::string& name) noexcept;
    std::size_t index_of(const std::string& name) const noexcept;

    /// True for a name one of the built-in profiles has.
    bool is_built_in(const std::string& name) const noexcept;

    /// The profile the window uses when nothing has been chosen, or when
    /// what was chosen is not here.
    const composer::ShortcutProfile& default_profile() const noexcept;

    /// Adds a profile, or replaces the composer's profile of that name.
    /// Refuses a built-in name, an empty name, and a built-in profile - the
    /// library's built-ins are the ones it was made with.
    bool store(const composer::ShortcutProfile& profile);

    /// Removes a composer's profile. Refuses a built-in.
    bool remove(const std::string& name);

    /// The name after `name`, wrapping at the end, for a control that steps
    /// through them. A name that is not in the library steps to the first.
    std::string next_after(const std::string& name) const;

    /// A name no profile has yet, for a copy of `name`: "VC Composer copy",
    /// then "VC Composer copy 2", and so on.
    std::string copy_name_for(const std::string& name) const;

    std::size_t size() const noexcept { return profiles.size(); }
};

/// A library holding only the built-in profiles, built for `platform`.
ShortcutProfileLibrary built_in_shortcut_profiles(
    composer::ShortcutPlatform platform = composer::host_shortcut_platform());

/// Writes one profile into a section: its name, the platform whose
/// shortcut key its bindings use, and one key per binding.
void shortcut_profile_to_settings(
    const composer::ShortcutProfile& profile, composer::SettingsSection& out,
    composer::ShortcutPlatform platform = composer::host_shortcut_platform());

/// Reads one profile back, remapped for `platform` when the file says it
/// was written for the other. Returns false when nothing usable could be
/// read - no name - and names in `warnings` every binding that was there
/// and could not be used: a command this build does not have, a stroke
/// that will not parse, a stroke already bound in the same profile. Each
/// such binding is left out and the rest of the profile stands.
bool shortcut_profile_from_settings(
    const composer::SettingsSection& section, composer::ShortcutProfile& out,
    std::vector<std::string>& warnings,
    composer::ShortcutPlatform platform = composer::host_shortcut_platform());

/// Writes the composer's profiles into the store, one section each,
/// replacing whatever profile sections it held. The built-ins are not
/// written: they are this build's, and a file that carried them would pin
/// an older version's bindings over a newer one's.
void shortcut_profiles_to_store(
    const ShortcutProfileLibrary& library, composer::SettingsStore& store,
    composer::ShortcutPlatform platform = composer::host_shortcut_platform());

/// Reads the composer's profiles from the store into a library that already
/// holds the built-ins. A profile that cannot be read is named and skipped;
/// one that reads but shares a built-in's name is named and skipped too,
/// since a composer's "VC Composer" that silently replaced the real one
/// would change what every key does without a word.
std::vector<std::string> shortcut_profiles_from_store(
    const composer::SettingsStore& store, ShortcutProfileLibrary& out,
    composer::ShortcutPlatform platform = composer::host_shortcut_platform());

}  // namespace vcc::composer
