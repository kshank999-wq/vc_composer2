// Template Browser (Templates Supplement §3, §8 Preview, §11): search and
// filter the template library, preview a template's tracks and notes,
// audition a short copyright-safe MIDI sketch, then Create or Add.
#pragma once

#include <memory>
#include <vector>

#include "ui/AppContext.h"
#include "vcc/templates/Shelf.h"

namespace vcc::desktop {

/// The composer's favorite and recent templates (saved in the user folder).
templates::TemplateShelf& template_shelf();
void save_template_shelf();

/// Writes the templates (and, when asked, the saved patches that play them)
/// as a .vcctemplates package; what happened.
juce::String export_template_package_file(AppContext& context, const std::vector<std::string>& template_ids,
                                          const juce::File& file, bool with_patches);
/// Imports a package: its templates become the composer's (saved), its
/// patches join the library. What happened.
juce::String import_template_package_file(AppContext& context, const juce::File& file);
/// Removes a custom template and its file.
bool delete_custom_template(AppContext& context, const std::string& template_id);
/// The open browser, if any.
class TemplateBrowser;
TemplateBrowser* template_browser();

class TemplateBrowser final : public juce::Component, private juce::ListBoxModel {
public:
    TemplateBrowser(AppContext& context, bool for_new_project);
    ~TemplateBrowser() override;

    void set_for_new_project(bool for_new_project);
    /// Lists again (after templates were imported or removed).
    void reload();
    /// Selects a template by id (the desktop check).
    bool select(const std::string& template_id);
    /// "Favorites" / "Recent" / "Custom" / "All categories"...
    void show_category(const juce::String& name);
    int shown_count() const noexcept { return static_cast<int>(shown_.size()); }
    void toggle_favorite();
    void create();
    void add();
    /// One track per instrument, or a track per articulation as well
    /// (Violins I, then Violins I - Pizzicato, - Tremolo...).
    void set_layout(templates::TrackLayout layout);
    templates::TrackLayout layout() const;

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    class Preview;

    int getNumRows() override;
    void paintListBoxItem(int row, juce::Graphics&, int width, int height, bool selected) override;
    void selectedRowsChanged(int last_row) override;
    void listBoxItemDoubleClicked(int row, const juce::MouseEvent&) override;
    void returnKeyPressed(int last_row) override;

    void filter();
    void show_selected();
    const templates::Template* chosen() const;
    void audition();
    void update_buttons();
    void share_menu();

    AppContext& context_;
    bool for_new_project_;
    juce::TextEditor search_;
    juce::ComboBox category_, variant_, size_, layout_;
    juce::Label layout_label_;
    juce::Label count_;
    juce::ListBox list_;
    std::unique_ptr<Preview> preview_;
    juce::Viewport preview_view_;
    juce::TextButton audition_{"Audition"}, stop_{"Stop"}, create_{"Create Project"}, add_{"Add to Project"},
        favorite_{"Favorite"}, share_{"Share..."};
    std::vector<const templates::Template*> shown_;
    std::vector<std::string> variants_;
};

}  // namespace vcc::desktop
