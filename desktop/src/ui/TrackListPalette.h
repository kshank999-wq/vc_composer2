// View > Track List: a floating list of the project's tracks, grouped by
// section, with a search box. Clicking one selects it and takes the edit
// window to it (the Composer lanes or the Producer tracks). It floats over
// VC Composer, is listed under Window, and collapses to a slim bar (its
// arrow, or a double-click on the bar) when it is not needed.
#pragma once

#include <functional>

#include "ui/AppContext.h"

namespace vcc::desktop {

class TrackListPanel final : public juce::Component, private juce::ListBoxModel {
public:
    TrackListPanel(AppContext& context, std::function<void(const std::string&)> go_to);
    ~TrackListPanel() override;

    /// The rows shown now: (track id or "" for a section heading, text).
    std::vector<std::pair<std::string, juce::String>> rows() const { return rows_; }
    void set_filter(const juce::String& text);
    /// Picks the row for a track (as a click does).
    bool pick(const std::string& track_id);
    void set_collapsed(bool collapsed);
    bool collapsed() const noexcept { return collapsed_; }

    void resized() override;
    void paint(juce::Graphics& g) override;

private:
    int getNumRows() override { return static_cast<int>(rows_.size()); }
    void paintListBoxItem(int row, juce::Graphics& g, int width, int height, bool selected) override;
    void listBoxItemClicked(int row, const juce::MouseEvent&) override;
    void rebuild();

    AppContext& context_;
    std::function<void(const std::string&)> go_to_;
    juce::TextEditor search_;
    juce::TextButton collapse_{"-"};
    juce::Label title_;
    juce::ListBox list_;
    std::vector<std::pair<std::string, juce::String>> rows_;
    int listener_ = -1;
    bool collapsed_ = false;
    int expanded_height_ = 520;
};

/// Shows (or brings forward) the floating track list.
void show_track_list(AppContext& context, std::function<void(const std::string&)> go_to);
/// Closes it; whether it is open.
void close_track_list();
bool track_list_open();
TrackListPanel* track_list_panel();

}  // namespace vcc::desktop
