// File > Shared Projects...: the projects shared with you (open one), and
// the open project's sharing - share it, sync it, who is in it, its recent
// revisions, and which parts are locked by whom (lock or unlock the selected
// part). People are invited and seats managed on vc-composer.com.
//
// The conflict dialog a sync opens when the same thing was changed two
// ways: each conflict says what it is and both sides; the composer keeps
// theirs or mine per conflict (or all at once), or cancels the sync.
#pragma once

#include "collab/Collaboration.h"
#include "ui/AppContext.h"

namespace vcc::desktop {

class SharedProjectsPanel final : public juce::Component, private juce::ListBoxModel {
public:
    SharedProjectsPanel(AppContext& context, std::function<bool(const juce::File&)> open);
    ~SharedProjectsPanel() override;

    /// Reads the open project's state and fetches the list again.
    void refresh(std::function<void()> done = {});
    juce::String status_text() const { return status_.getText(); }
    juce::String project_text() const { return project_.getText(); }
    juce::String people_text() const { return people_.getText(); }
    int project_count() const { return static_cast<int>(projects_.size()); }

    // The same as the buttons; `done` runs on the message thread when finished.
    void share(const juce::String& name, std::function<void(collab::Outcome)> done = {});
    void sync(std::function<void(collab::Outcome)> done = {});
    void lock_selected(bool lock, std::function<void(collab::Outcome)> done = {});
    void open_row(int row, const juce::File& folder, std::function<void(collab::Outcome)> done = {});

    void resized() override;
    void paint(juce::Graphics& g) override;

private:
    int getNumRows() override;
    void paintListBoxItem(int row, juce::Graphics& g, int width, int height, bool selected) override;
    void listBoxItemDoubleClicked(int row, const juce::MouseEvent&) override;
    void update();
    void report(const collab::Outcome& o);

    AppContext& context_;
    std::function<bool(const juce::File&)> open_;
    juce::Label title_, status_, list_title_, project_title_, project_, people_;
    juce::ListBox list_{"Shared projects", this};
    juce::TextButton open_button_{"Open Shared Project..."}, reload_{"Refresh"}, share_{"Share This Project..."},
        sync_{"Sync Now"}, lock_{"Lock Selected Part"}, unlock_{"Unlock"}, team_{"People and Seats Online"};
    std::vector<collab::ProjectSummary> projects_;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

/// The conflicts of a sync, decided one by one.
class ConflictPanel final : public juce::Component {
public:
    ConflictPanel(const project::MergeResult& result, std::function<void(std::optional<std::vector<bool>>)> answer);
    int conflict_count() const { return static_cast<int>(rows_.size()); }
    juce::String row_text(int i) const;
    /// What the composer chose for conflict `i` (true: theirs).
    void choose(int i, bool theirs);
    void finish();
    void cancel();

    void resized() override;
    void paint(juce::Graphics& g) override;

private:
    struct Row {
        std::unique_ptr<juce::Label> what;
        std::unique_ptr<juce::ToggleButton> mine, theirs;
    };
    std::vector<Row> rows_;
    juce::Label intro_;
    juce::Viewport viewport_;
    juce::Component list_;
    juce::TextButton all_mine_{"Keep All Mine"}, all_theirs_{"Take All Theirs"}, finish_{"Finish Sync"}, cancel_{"Cancel Sync"};
    std::function<void(std::optional<std::vector<bool>>)> answer_;
    bool answered_ = false;
};

void show_shared_projects(AppContext& context, std::function<bool(const juce::File&)> open);
SharedProjectsPanel* shared_projects_panel();
/// Opens the conflict dialog (the controller's ask_conflicts).
void ask_conflicts(const project::MergeResult& result, std::function<void(std::optional<std::vector<bool>>)> answer);
ConflictPanel* conflict_panel();

}  // namespace vcc::desktop
