// Edit > Keyboard Shortcuts > Edit Shortcuts... (Main Spec §7): the factory
// profiles (Composer 2, VC Composer, Finale-style, Sibelius-style) and the
// composer's own, every command with its key; Set Key takes the next key
// pressed and says what already uses it (Replace takes it from there,
// nothing is stolen silently); conflicts are listed; Duplicate makes an
// editable copy of a factory profile; Import / Export as .vccshortcuts
// files to share. User profiles live in the app's data folder.
#pragma once

#include <vector>

#include "ui/AppContext.h"
#include "vcc/composer/ShortcutProfile.h"

namespace vcc::desktop {

/// Factory profiles, then the composer's own.
std::vector<composer::ShortcutProfile> all_shortcut_profiles();
/// Saves a user profile (never a factory one). False when it is built in.
bool save_user_shortcut_profile(const composer::ShortcutProfile& profile);
bool delete_user_shortcut_profile(const std::string& name);
/// A profile as a document, and back (nullopt when it is not one).
std::string shortcut_profile_to_text(const composer::ShortcutProfile& profile);
std::optional<composer::ShortcutProfile> shortcut_profile_from_text(const std::string& text, std::vector<std::string>* problems = nullptr);

class ShortcutEditor final : public juce::Component, private juce::ListBoxModel {
public:
    /// `apply` makes a profile the one in use (the main window's).
    ShortcutEditor(AppContext& context, std::function<void(const std::string&)> apply);
    ~ShortcutEditor() override;

    void choose_profile(const std::string& name);
    const composer::ShortcutProfile& profile() const noexcept { return profile_; }
    /// Duplicates the current profile as an editable one; returns its name.
    std::string duplicate();
    /// Waits for the next key for the selected command.
    void start_capture(int row);
    bool capturing() const noexcept { return capture_row_ >= 0; }
    /// What a capture does with a stroke (the key handler and checks call it).
    /// Returns the message shown.
    juce::String take_stroke(const composer::KeyStroke& stroke, bool replace_if_taken);
    juce::String conflicts_text() const;
    int rows() const;

    bool keyPressed(const juce::KeyPress& key) override;
    void resized() override;
    void paint(juce::Graphics& g) override;

private:
    int getNumRows() override;
    void paintListBoxItem(int row, juce::Graphics& g, int w, int h, bool selected) override;
    void listBoxItemDoubleClicked(int row, const juce::MouseEvent&) override { start_capture(row); }
    void refresh();
    void persist();

    AppContext& context_;
    std::function<void(const std::string&)> apply_;
    composer::ShortcutProfile profile_;
    std::vector<composer::Command> commands_;
    juce::ComboBox profiles_;
    juce::ListBox list_;
    juce::Label status_, conflicts_;
    juce::TextButton set_{"Set key..."}, clear_{"Clear"}, duplicate_{"Duplicate"}, import_{"Import..."},
        export_{"Export..."}, delete_{"Delete"}, use_{"Use this profile"};
    std::unique_ptr<juce::FileChooser> chooser_;
    int capture_row_ = -1;
    composer::KeyStroke pending_;
};

void show_shortcut_editor(AppContext& context, std::function<void(const std::string&)> apply);
ShortcutEditor* shortcut_editor();

}  // namespace vcc::desktop
