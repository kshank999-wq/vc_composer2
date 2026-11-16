// Collaboration on the desktop (Main Spec §15): share the open project, open
// one somebody shared with you, sync (pull, merge, push), and lock parts for
// writing. The server is vc-composer.com (web/src/app/api/collab); the
// sharing files beside the project and the merge are vcc/session/Shared.h.
//
// Network calls run off the message thread; merging, asking about
// conflicts and changing the document run on it. One operation at a time.
//
// A sync always saves the project with its new base: a project file older
// than its base would read as "I deleted what the others wrote".
//
// Audio and picture files travel with it (vcc/session/SharedMedia.h): media
// outside the project folder is gathered into it first, files are uploaded
// once each by content, and a pull downloads what this computer lacks,
// checking every file's SHA-256.
#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "ui/AppContext.h"
#include "vcc/project/Merge.h"
#include "vcc/session/Shared.h"

namespace vcc::desktop::collab {

struct ProjectSummary {
    juce::String id, name, role, owner;
    int head = 0;
};

struct Member {
    juce::String name, email, role;
    bool accepted = false;
};

struct Revision {
    int number = 0;
    juce::String message, author, when;
};

struct Lock {
    juce::String track_id, holder, user_id, until;
};

/// A shared project as the server sees it now.
struct Detail {
    juce::String id, name, role, me;
    int head = 0;
    std::vector<Member> members;
    std::vector<Revision> revisions;
    std::vector<Lock> locks;
};

/// What a finished operation says (shown in the status bar and the window).
struct Outcome {
    bool ok = false;
    juce::String message;
};
using Done = std::function<void(Outcome)>;

/// Asks the composer about merge conflicts; answers take_theirs per
/// conflict, or nullopt to cancel the sync (nothing changes).
using AskConflicts = std::function<void(const project::MergeResult&, std::function<void(std::optional<std::vector<bool>>)>)>;

class Controller {
public:
    static Controller& get();

    /// Follows the session: a shared project being opened gets this
    /// computer's id site and its locks. The main window calls it once.
    void attach(AppContext& context);
    void detach();

    /// The open project's sharing, if it is shared and saved.
    std::optional<session::shared::Link> link() const;
    bool busy() const noexcept { return busy_; }
    /// The last detail fetched for the open project (empty before one).
    const Detail& detail() const noexcept { return detail_; }
    /// What the window shows under its title ("Synced with revision 4 of 6").
    juce::String summary() const;

    /// This installation's id site (made once, kept in the user data folder).
    static std::uint32_t site();

    // Each runs in the background and calls `done` on the message thread.
    void list(std::function<void(std::vector<ProjectSummary>, Outcome)> done);
    /// Shares the open, saved project: creates it on the server and pushes revision 1.
    void share(const juce::String& name, Done done);
    /// Downloads a shared project's latest revision into `folder`/<name>/
    /// and opens it there (through `open`, which may ask about unsaved work).
    void open_shared(const ProjectSummary& project, const juce::File& folder,
                     std::function<bool(const juce::File&)> open, Done done);
    /// Pull (merging, asking about conflicts), then push what is new here.
    void sync(Done done);
    /// Locks (or unlocks) a part for writing.
    void set_lock(const std::string& track_id, bool lock, Done done);
    /// Fetches the detail again and applies the locks.
    void refresh(Done done = {});

    /// The conflict question (the window sets it; the desktop check drives it).
    AskConflicts ask_conflicts;
    /// Told whenever the detail or the link changes (the window repaints).
    std::function<void()> on_change;

private:
    Controller() = default;
    void opened();
    void apply_locks();
    void finish(Done& done, Outcome outcome);
    // The sync, step by step (each step's slow part runs in the background).
    void step_pull(Done done, int attempt);
    void step_fetch(Done done, int attempt, juce::String pulled);
    void step_push(Done done, int attempt, juce::String pulled);
    /// Tells the status bar from any thread (downloads and uploads of large files).
    void progress(const juce::String& text);

    AppContext* context_ = nullptr;
    int listener_ = 0;
    std::uint64_t generation_ = 0;
    std::string path_;
    Detail detail_;
    bool busy_ = false;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

/// Parses the server's answers (public for the desktop check).
std::optional<Detail> parse_detail(const json::Value& body);
std::vector<ProjectSummary> parse_list(const json::Value& body);
/// A conflict's side, in a few words ("deleted", "\"Violins (Ben)\"", "C5 crotchet"...).
juce::String describe_value(const std::optional<json::Value>& v);

}  // namespace vcc::desktop::collab
