// The composer's tool windows. Each opens non-modally, works on the current
// document through Session, and auditions through the engine's preview
// path, which never writes to the project.
#pragma once

#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "ui/AppContext.h"

namespace vcc::desktop {

/// Template browser (Supplement §3, §11): search, filters, preview, then
/// Create (new project) or Add to Existing (merge).
/// `for_new_project` chooses which action is the default.
void show_template_browser(AppContext& context, bool for_new_project);

/// Chord Explorer / Progression Builder (Supplement §9).
void show_chord_explorer(AppContext& context);

/// Ostinato Creator (Supplement §10).
void show_ostinato_creator(AppContext& context);

/// Pieces shared by the tool windows (implemented in Dialogs.cpp).
namespace dialogs {

/// A non-modal, resizable tool window with a native title bar. It deletes
/// itself when closed; its content must not touch the AppContext in its
/// destructor (the window may outlive the main window at shutdown).
class ToolWindow final : public juce::DocumentWindow, private juce::DeletedAtShutdown {
public:
    ToolWindow(const juce::String& title, juce::Component* content, int width, int height);
    void closeButtonPressed() override;
    /// Called when the close button is pressed, before the window goes.
    std::function<void()> on_close;
};

/// Brings an open tool window to the front, or creates one with `make`.
void show_tool(juce::Component::SafePointer<ToolWindow>& slot, const juce::String& title, int width, int height,
               const std::function<juce::Component*()>& make, std::function<void()> on_close = {});

/// Asks Replace / Merge / New Lane / Cancel for a Send to Track that landed
/// on existing material. `done` is called only for a decision, never on Cancel.
void ask_collision(const juce::String& what, std::function<void(project::CollisionPolicy)> done);

/// Reports a send result in the status bar (and an alert on error).
void report_send(AppContext& context, const project::InsertResult& result, const juce::String& what);

/// Tonic spellings offered in key selectors ("C", "C#", "Db", ...).
const std::vector<std::string>& tonic_names();
/// Fills tonic and mode combos (ids are 1-based indices) and selects `key`.
/// With `heptatonic_only`, only modes that carry Roman numerals are listed.
void fill_key_combos(juce::ComboBox& tonic, juce::ComboBox& mode, const theory::Key& key, bool heptatonic_only);
/// The key the combos describe.
std::optional<theory::Key> key_from_combos(const juce::ComboBox& tonic, const juce::ComboBox& mode);
/// The project's key, or C major when it cannot be read.
theory::Key project_key(const AppContext& context);

/// Placeholder voice for auditioning on a track: family and GM program
/// (-1 for unpitched). Falls back to piano.
std::pair<std::string, int> preview_voice(const AppContext& context, const std::string& track_id);
/// Project tempo at the start, for auditions.
double project_bpm(const AppContext& context);
/// Shifts notes so the earliest possible start (`origin`) is tick 0.
std::vector<NoteEvent> shifted(const std::vector<NoteEvent>& notes, Ticks origin);

/// "C4" style name of a MIDI note.
juce::String note_name(int midi);

/// Draws notes as a small piano roll in `area` spanning [start, end) with
/// bar lines from `meter`.
void draw_mini_roll(juce::Graphics& g, juce::Rectangle<float> area, const std::vector<NoteEvent>& notes, Ticks start,
                    Ticks end, const MeterMap& meter, juce::Colour colour);

/// A small caption label in the dim text colour.
void style_caption(juce::Label& label, const juce::String& text);

/// Target track chooser: the project's music tracks plus a "new track"
/// entry. It re-reads the project each time it opens, so it never offers a
/// track that has gone.
class TrackSelector final : public juce::ComboBox {
public:
    TrackSelector(AppContext& context, juce::String new_track_label);
    /// Re-reads the tracks, keeping the current choice when it still exists
    /// (else the selected track, else the first one).
    void refresh();
    /// The chosen track id; empty when "new track" is chosen.
    std::string track_id() const;
    void select(const std::string& track_id);
    void showPopup() override;

private:
    AppContext& context_;
    juce::String new_label_;
    std::vector<std::string> ids_;
};

}  // namespace dialogs

}  // namespace vcc::desktop
