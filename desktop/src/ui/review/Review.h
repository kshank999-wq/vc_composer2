// Director review and project packaging (Main Spec §15, §16).
//
// Director Review: up to three slots, A, B and C, each a whole-project
// snapshot or a version of the selected track. A, B and C (keys or 1-3)
// switch between them while the transport plays on - an audition, not an
// edit: nothing reaches undo. The window says where the slots differ ("B
// differs from A in bars 3-4, 7"), approves (locks) the slot under review,
// and ends with Keep (one undo step) or Back (to where it began; also what
// closing the window does).
//
// Package: File > Collect and Save Package... gathers the project and every
// media file it uses into one folder (and, optionally, a .zip), with a
// report of what the receiving machine needs. File > Open Package... unpacks
// a .zip and opens the project in it.
#pragma once

#include <array>
#include <atomic>
#include <map>
#include <memory>
#include <thread>

#include "ui/AppContext.h"
#include "ui/hw/HwControls.h"
#include "vcc/session/Package.h"

namespace vcc::desktop::review {

class ReviewPanel final : public juce::Component, private juce::Timer {
public:
    explicit ReviewPanel(AppContext& context);
    ~ReviewPanel() override;

    enum class Mode { Snapshots, Track };
    void set_mode(Mode m);
    Mode mode() const noexcept { return mode_; }
    /// What a slot holds: a snapshot id, or a version id of the track.
    void assign(int slot, const std::string& id);
    const std::string& assigned(int slot) const { return slots_[static_cast<std::size_t>(slot)]; }
    /// Plays slot 0 (A), 1 (B) or 2 (C).
    void play_slot(int slot);
    int current() const noexcept { return current_; }
    /// Records the current versions as a new snapshot and puts it in `slot`.
    void capture(int slot);
    void approve_current(bool approved);
    /// "B differs from A in bars 3-4" (or per track, for snapshots).
    juce::String difference() const;
    /// Keeps what is playing (one undo step) or goes back to the start.
    void keep();
    void back();
    bool ended() const noexcept { return ended_; }
    const std::string& track_id() const noexcept { return track_; }

    void resized() override;
    void paint(juce::Graphics& g) override;
    bool keyPressed(const juce::KeyPress& k) override;

private:
    void timerCallback() override;
    void reload();
    std::map<std::string, std::string> versions_of(int slot) const;
    bool slot_approved(int slot) const;
    juce::String slot_text(int slot) const;

    AppContext& context_;
    Mode mode_ = Mode::Snapshots;
    std::string track_;
    std::array<std::string, 3> slots_;
    int current_ = -1;
    std::map<std::string, std::string> original_;
    bool ended_ = false;
    std::string signature_;

    hw::Keycap snapshots_{"Snapshots", true}, track_mode_{"Selected track", true};
    std::array<std::unique_ptr<hw::Keycap>, 3> keys_;
    std::array<juce::ComboBox, 3> choosers_;
    std::array<juce::TextButton, 3> captures_;
    juce::ToggleButton approve_{"Approved (locked)"};
    juce::TextButton keep_{"Keep"}, back_{"Back"};
    juce::Label title_, diff_, help_;
};

/// View > Director Review.
void show_review(AppContext& context);
ReviewPanel* review_panel();

class PackagePanel final : public juce::Component, private juce::Timer {
public:
    explicit PackagePanel(AppContext& context);
    ~PackagePanel() override;

    juce::File& destination() noexcept { return destination_; }
    juce::ToggleButton& include_picture() noexcept { return picture_; }
    juce::ToggleButton& make_zip() noexcept { return zip_; }
    /// Starts packaging in the background (the window's Package key).
    void start();
    bool busy() const noexcept { return worker_ != nullptr; }
    /// The last package's result and zip (empty when none was made).
    const session::package::Result& result() const noexcept { return result_; }
    const juce::File& zip_file() const noexcept { return zip_file_; }
    const juce::String& message() const noexcept { return message_; }

    void resized() override;
    void paint(juce::Graphics& g) override;

private:
    void timerCallback() override;
    void refresh_inventory();
    void finished();

    AppContext& context_;
    juce::File destination_;
    session::package::Inventory inventory_;
    juce::Label title_, folder_, status_;
    juce::TextEditor report_;
    juce::TextButton choose_{"Choose..."}, go_{"Package"}, cancel_{"Cancel"};
    juce::ToggleButton picture_{"Include the picture"}, zip_{"Also make a .zip"};
    double progress_value_ = 0.0;
    juce::ProgressBar progress_{progress_value_};
    std::unique_ptr<juce::FileChooser> chooser_;

    // The worker: a copy of the project, packaged off the message thread.
    std::unique_ptr<std::thread> worker_;
    std::atomic<double> fraction_{0.0};
    std::atomic<bool> cancel_requested_{false};
    std::atomic<bool> done_{false};
    session::package::Result result_;
    juce::File zip_file_;
    juce::String message_;
};

/// File > Collect and Save Package.
void show_package(AppContext& context);
PackagePanel* package_panel();

/// Unpacks a package .zip beside it (a new folder) and returns the project
/// file inside, or an empty File with `error` set.
juce::File unpack(const juce::File& zip, juce::String& error);

}  // namespace vcc::desktop::review
