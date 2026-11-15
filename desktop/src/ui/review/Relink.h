// File > Relink Missing Media (Main Spec §16): lists the audio and picture
// the project cannot find, searches folders for them (in the background,
// one search for every file), proposes the best match for each and relinks
// them all as one undo step. Ambiguous or converted matches are offered in
// the row's list for the user to pick; any row can also be pointed at a file
// by hand.
#pragma once

#include <atomic>
#include <memory>
#include <thread>

#include "ui/AppContext.h"
#include "vcc/session/Relink.h"

namespace vcc::desktop::review {

class RelinkPanel final : public juce::Component, private juce::Timer {
public:
    explicit RelinkPanel(AppContext& context);
    ~RelinkPanel() override;

    /// Re-reads what is missing (clears the choices).
    void refresh();
    const std::vector<session::package::Asset>& missing() const noexcept { return missing_; }
    /// The folders searched: the project's folder and its parent to start.
    const juce::StringArray& folders() const noexcept { return folders_; }
    void add_folder(const juce::File& folder);
    void clear_folders();
    /// Searches in the background; the rows fill when it finishes.
    void search();
    bool searching() const noexcept { return worker_ != nullptr; }
    void cancel() { cancel_requested_ = true; }
    /// The file chosen for missing file `index` (empty: none).
    juce::String chosen(int index) const;
    void choose(int index, const juce::String& file);
    /// The choices made and the rows still without one.
    int chosen_count() const;
    /// Relinks every chosen file (one undo step). Returns the references changed.
    int relink();
    const juce::String& message() const noexcept { return message_; }

    void resized() override;
    void paint(juce::Graphics& g) override;

private:
    class Row;
    void timerCallback() override;
    void rebuild_rows();
    void finished();
    void update_status();

    AppContext& context_;
    std::vector<session::package::Asset> missing_;
    std::vector<session::relink::Match> matches_;  ///< One per missing file once searched.
    std::vector<juce::String> chosen_;
    juce::StringArray folders_;

    juce::Label title_, folders_label_, status_;
    juce::TextButton add_folder_{"Add folder..."}, clear_folders_{"Clear"}, search_{"Search"}, cancel_{"Cancel"},
        relink_{"Relink"};
    juce::Viewport view_;
    juce::Component list_;
    std::vector<std::unique_ptr<Row>> rows_;
    std::unique_ptr<juce::FileChooser> chooser_;
    juce::String message_;
    std::string signature_;

    std::unique_ptr<std::thread> worker_;
    std::atomic<int> scanned_{0};
    std::atomic<bool> cancel_requested_{false};
    std::atomic<bool> done_{false};
    session::relink::SearchResult result_;
};

void show_relink(AppContext& context);
RelinkPanel* relink_panel();

}  // namespace vcc::desktop::review
