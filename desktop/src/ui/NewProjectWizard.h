// File > New Project... (Main Spec §5): name and location; the scoring
// category, mood and starting template (or an empty project); key, meter and
// tempo, prefilled from the template's recommendations and free to change;
// frame rate, start timecode and sample rate; and an optional picture to
// import. Create makes the project in a folder of its own, with no undo
// history, and opens it.
#pragma once

#include <memory>

#include "ui/AppContext.h"
#include "vcc/session/Session.h"

namespace vcc::desktop {

class NewProjectWizard final : public juce::Component {
public:
    explicit NewProjectWizard(AppContext& context);
    ~NewProjectWizard() override;

    juce::TextEditor& name() noexcept { return name_; }
    void set_folder(const juce::File& folder);
    const juce::File& folder() const noexcept { return folder_; }
    /// Picks a category, then a mood (subcategory, empty for all), then a
    /// template (empty: an empty project); the music fields follow the
    /// template's recommendations.
    void choose(const std::string& category, const std::string& mood, const std::string& template_id);
    const std::string& template_id() const noexcept { return template_; }
    /// One track per instrument, or a track per articulation as well.
    juce::ComboBox& layout_box() noexcept { return layout_; }
    void set_key(const std::string& key);
    void set_meter(const juce::String& meter);
    void set_tempo(double bpm);
    void set_frame_rate(FrameRate rate);
    void set_start_timecode(const juce::String& tc);
    void set_sample_rate(int rate);
    void set_picture(const juce::File& file) { picture_ = file; refresh_labels(); }
    /// What Create would make.
    session::ProjectSetup setup() const;
    /// Makes the project (and imports the picture). Empty on success, else why.
    juce::String create();
    const juce::String& message() const noexcept { return message_; }

    void resized() override;
    void paint(juce::Graphics& g) override;

private:
    void fill_moods();
    void fill_templates();
    void template_changed();
    void refresh_labels();

    AppContext& context_;
    juce::File folder_, picture_;
    std::string template_;
    std::vector<std::string> mood_ids_, template_ids_;
    juce::Label title_, name_label_, folder_label_, scoring_label_, about_, music_label_, picture_label_, picture_file_,
        status_;
    juce::TextEditor name_, start_tc_;
    juce::TextButton choose_folder_{"Choose..."}, choose_picture_{"Choose picture..."}, clear_picture_{"None"},
        create_{"Create"}, cancel_{"Cancel"};
    juce::ComboBox category_, mood_, template_box_, layout_, tonic_, mode_, meter_, frame_rate_, sample_rate_;
    juce::Slider tempo_{juce::Slider::IncDecButtons, juce::Slider::TextBoxLeft};
    std::unique_ptr<juce::FileChooser> chooser_;
    juce::String message_;
};

/// Opens the wizard (one at a time).
void show_new_project_wizard(AppContext& context);
NewProjectWizard* new_project_wizard();

}  // namespace vcc::desktop
