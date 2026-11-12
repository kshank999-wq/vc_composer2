// Copyright (c) VC Composer. All rights reserved.
//
// Bringing an older settings file up to this build's format (spec 02
// section 13: "a versioned local settings database/file").
//
// There is one format version so far, so the table of steps is empty. The
// machinery is here anyway, because the first format change is the
// expensive moment to find out there is none: every composer's file is
// then an older one. A step takes a file at one version to the next, and
// a file is walked up one step at a time, so a change only ever has to
// know about the version just before it.
//
// What a step may do: rename or move keys and sections, split or join
// values, drop what the new format no longer holds. What it must not do is
// guess at a composer's intent - a value it cannot carry across is dropped
// and named in the notes rather than invented.
//
// A file from a *newer* build is left exactly as it is, version and all:
// this build reads what it understands of it, and writing into it keeps the
// version it declared, because claiming the file is older than it is would
// be a lie to whichever build wrote it (`app/SettingsFile`).
#pragma once

#include <string>
#include <vector>

#include "vcc/composer/SettingsStore.h"

namespace vcc::composer {

/// One step: a document at version `from` becomes one at `from + 1`.
struct SettingsMigrationStep {
    int from = 0;

    /// One line saying what changed, for the notes.
    const char* what = "";

    /// Rewrites the store in place. May add notes of its own - what it
    /// dropped, and why.
    void (*apply)(SettingsStore& store, std::vector<std::string>& notes) = nullptr;
};

struct SettingsMigrationResult {
    /// The version the document declared, and the one it ended at.
    int from = 0;
    int to = 0;

    /// What each step did, and anything a step said.
    std::vector<std::string> notes;

    /// The document is from a later build than this one; it was left alone.
    bool newer = false;

    /// The document is older, and no step leads on from `to`: it was
    /// brought as far as the steps go and no further.
    bool stranded = false;

    bool migrated() const noexcept { return to != from; }
};

/// The steps this build knows, in order. One for each version older than
/// `kSettingsFormatVersion`, so every older file has a way up; a test holds
/// the table to that.
const std::vector<SettingsMigrationStep>& settings_migration_steps();

/// True when `steps` lead from every version from 1 up to `target - 1` by
/// single steps: what a table has to be for no older file to be stranded.
bool steps_lead_to(const std::vector<SettingsMigrationStep>& steps, int target);

/// Walks `store` up from the version it declares to `target`, one step at
/// a time. A store already at `target` is untouched; one past it is left
/// alone and reported `newer`; one that runs out of steps is reported
/// `stranded` at the version it reached. A migrated store declares
/// `target` afterwards, so writing it writes this build's format.
SettingsMigrationResult migrate_settings(SettingsStore& store,
                                         const std::vector<SettingsMigrationStep>& steps,
                                         int target);

/// With this build's own steps and version.
SettingsMigrationResult migrate_settings(SettingsStore& store);

}  // namespace vcc::composer
