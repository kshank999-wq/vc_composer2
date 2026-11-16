// The instrument usage panel (Memory Management spec §6.1, §6.3): every
// instrument in the project, with what it costs - its share of the audio
// engine's real-time budget and its memory, as far as it can be measured -
// and the controls to bring that down: Freeze & Unload, Suspend, Reload,
// Purge (where the plug-in format offers it), Locate; and batch Freeze &
// Unload by selection, articulation group or instrument family, with
// progress and Cancel. It suggests; it never unloads anything by itself.
//
// Memory is what the operating system reports per process: an instrument
// alone in its plug-in host process is measured exactly; instruments sharing
// a process get an estimate (marked ~) with the process's total; inside the
// app it is "Not reported".
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <optional>
#include <string>
#include <vector>

#include "ui/AppContext.h"

namespace vcc::desktop {

class InstrumentUsagePanel final : public juce::Component, private juce::TableListBoxModel, private juce::Timer {
public:
    struct Row {
        std::string track_id;
        juce::String track, family, instrument, state;
        juce::String memory;                       ///< "1.2 GB", "~400 MB (1 of 3 · 1.2 GB)", "Not reported".
        std::optional<std::uint64_t> memory_bytes;  ///< Measured or estimated; nullopt: not reported.
        bool memory_exact = false;
        double load = 0.0;   ///< % of real time.
        bool load_measured = false;
        bool plugin = false, loaded = true, suspended = false, frozen = false;
    };
    enum class Sort { Track = 1, Instrument, State, Memory, Load };
    enum class Action { FreezeUnload, Suspend, Reload, Purge, Locate };

    explicit InstrumentUsagePanel(AppContext& context);
    ~InstrumentUsagePanel() override;

    /// Reads everything again now (the timer does every second).
    void refresh();
    const std::vector<Row>& rows() const noexcept { return rows_; }
    void sort_by(Sort column, bool forwards);
    juce::String summary() const { return summary_.getText(); }
    juce::String suggestion() const { return suggestion_.getText(); }
    juce::String progress() const { return progress_.getText(); }

    void select_tracks(const std::vector<std::string>& ids);
    std::vector<std::string> selected_tracks() const;
    /// The action on the selected rows; what it did, for the status line.
    juce::String run(Action action);
    /// Batch Freeze & Unload targets: (label, tracks), as the Batch menu lists them.
    std::vector<std::pair<juce::String, std::vector<std::string>>> batch_targets() const;
    juce::String freeze_batch(const std::vector<std::string>& tracks);
    void cancel_batch();

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    int getNumRows() override { return static_cast<int>(rows_.size()); }
    void paintRowBackground(juce::Graphics& g, int row, int width, int height, bool selected) override;
    void paintCell(juce::Graphics& g, int row, int column, int width, int height, bool selected) override;
    void sortOrderChanged(int column, bool forwards) override;
    void selectedRowsChanged(int) override { update_buttons(); }
    void cellDoubleClicked(int row, int, const juce::MouseEvent&) override;
    void timerCallback() override;

    void apply_sort();
    void update_buttons();
    void show_batch_menu();

    AppContext& context_;
    std::vector<Row> rows_;
    Sort sort_ = Sort::Memory;
    bool forwards_ = false;  ///< Largest first.
    int ticks_ = 0;
    juce::Label title_, summary_, suggestion_, progress_;
    juce::TableListBox table_{"instruments"};
    juce::TextButton freeze_{"Freeze & Unload"}, suspend_{"Suspend"}, reload_{"Reload"}, purge_{"Purge"},
        locate_{"Locate"}, batch_{"Batch Freeze & Unload..."}, cancel_{"Cancel"};
};

/// Opens (or brings forward) the panel.
void show_instrument_usage(AppContext& context);
/// The open panel, or null (the checks).
InstrumentUsagePanel* instrument_usage_panel();

}  // namespace vcc::desktop
