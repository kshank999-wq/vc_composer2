// Collaboration on the desktop (Main Spec §15): what a copy of a shared
// project keeps beside its file, and the sync itself apart from the network.
//
//   <Name>.vcc2               the project, as always
//   <Name>.vcc2.shared.json   which shared project it is, the composer's
//                             role in it, and the revision it last synced with
//   <Name>.vcc2.shared-base   that revision's document: the common ancestor a
//                             three-way merge needs
//
// A sync is pull-then-push. Pull: when the server's head is newer than the
// base, merge(base, this copy, head) - separate changes combine, a real
// conflict is asked about - and the head becomes the base. Push: when this
// copy differs from the base, upload it naming the base; the server refuses
// if somebody pushed meanwhile (then pull again). The network half lives in
// the desktop app; everything here is testable without one.
//
// Audio and picture files travel too: SharedMedia.h.
#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "vcc/core/Json.h"
#include "vcc/project/Merge.h"
#include "vcc/session/SharedMedia.h"

namespace vcc::session::shared {

struct Link {
    std::string project_id;  ///< The server's id for the shared project.
    std::string name;
    std::string role;        ///< owner, editor, arranger, reviewer, viewer.
    int base = 0;            ///< The revision this copy last synced with (0: none yet).
    std::uint32_t site = 0;  ///< This installation's id site in it.
    Manifest base_media;     ///< The base revision's media (what changed here since).

    json::Value to_json() const;
    static std::optional<Link> from_json(const json::Value& doc);
};

std::string link_path(const std::string& project_file);
std::string base_path(const std::string& project_file);

std::optional<Link> read_link(const std::string& project_file);
bool write_link(const std::string& project_file, const Link& link, std::string* error = nullptr);
/// The base revision's document; nullopt before the first sync.
std::optional<json::Value> read_base(const std::string& project_file);
bool write_base(const std::string& project_file, const std::string& document_text, std::string* error = nullptr);
/// Forgets the sharing (the project file stays, unshared).
void unlink(const std::string& project_file);

/// Owners, editors and arrangers write; reviewers and viewers only open.
bool can_push(const std::string& role);
/// "Editor / composer"... for the window.
std::string role_label(const std::string& role);

/// A site from any random number: 1..0xffffff, never 0 (unshared).
std::uint32_t site_from(std::uint64_t random);

/// The document as it is shared: what this copy pushes and compares.
std::string document_text(const json::Value& doc);

/// Whether `ours` has changes the base does not (nothing to push otherwise).
bool changed_since(const json::Value& base, const json::Value& ours);

/// The pull: theirs merged into ours. No base yet (the first sync of a copy
/// that was opened from the server) means theirs wins outright.
project::MergeResult pull(const std::optional<json::Value>& base, const json::Value& ours, const json::Value& theirs);

}  // namespace vcc::session::shared
