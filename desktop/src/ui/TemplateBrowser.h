// Template Browser (Templates Supplement §3, §8 Preview, §11): search and
// filter the template library, preview a template's tracks and notes,
// audition a short copyright-safe MIDI sketch, then Create or Add.
#pragma once

#include <memory>
#include <vector>

#include "ui/AppContext.h"

namespace vcc::desktop {

class TemplateBrowser final : public juce::Component, private juce::ListBoxModel {
public:
    TemplateBrowser(AppContext& context, bool for_new_project);
    ~TemplateBrowser() override;

    void set_for_new_project(bool for_new_project);

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
    void create();
    void add();
    void update_buttons();

    AppContext& context_;
    bool for_new_project_;
    juce::TextEditor search_;
    juce::ComboBox category_, variant_, size_;
    juce::Label count_;
    juce::ListBox list_;
    std::unique_ptr<Preview> preview_;
    juce::Viewport preview_view_;
    juce::TextButton audition_{"Audition"}, stop_{"Stop"}, create_{"Create Project"}, add_{"Add to Project"};
    std::vector<const templates::Template*> shown_;
    std::vector<std::string> variants_;
};

}  // namespace vcc::desktop
