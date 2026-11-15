#include "ui/NewProjectWizard.h"

#include <set>

#include "ui/Dialogs.h"
#include "ui/Theme.h"
#include "ui/VideoWindow.h"
#include "vcc/session/Package.h"
#include "vcc/theory/Pitch.h"

namespace vcc::desktop {

namespace {

juce::Component::SafePointer<dialogs::ToolWindow>& wizard_window()
{
    static juce::Component::SafePointer<dialogs::ToolWindow> w;
    return w;
}
NewProjectWizard*& wizard_slot()
{
    static NewProjectWizard* p = nullptr;
    return p;
}

const std::pair<const char*, const char*> kCategories[] = {
    {"", "Empty project"}, {"cinematic", "Cinematic"}, {"modern", "Modern"}, {"regional", "Regional"},
    {"era", "Historical era"}, {"composer", "Composer style"}, {"custom", "My templates"}};
const char* kMeters[] = {"4/4", "3/4", "2/4", "6/8", "5/4", "7/8", "9/8", "12/8", "2/2", "5/8", "7/4"};
const int kSampleRates[] = {44100, 48000, 88200, 96000};
const FrameRate kRates[] = {FrameRate::Fps23_976, FrameRate::Fps24, FrameRate::Fps25, FrameRate::Fps29_97_NonDrop,
                            FrameRate::Fps29_97_Drop, FrameRate::Fps30, FrameRate::Fps47_952, FrameRate::Fps48,
                            FrameRate::Fps50, FrameRate::Fps59_94_NonDrop, FrameRate::Fps59_94_Drop, FrameRate::Fps60};

juce::String pretty(const std::string& id)
{
    juce::String s(id);
    s = s.replaceCharacter('_', ' ');
    return s.substring(0, 1).toUpperCase() + s.substring(1);
}

void heading(juce::Label& l, const juce::String& text)
{
    l.setText(text.toUpperCase(), juce::dontSendNotification);
    l.setFont(juce::FontOptions(11.0f, juce::Font::bold));
    l.setColour(juce::Label::textColourId, colours::text_dim);
}

}  // namespace

NewProjectWizard::NewProjectWizard(AppContext& context) : context_(context)
{
    setComponentID("new_project_wizard");
    wizard_slot() = this;
    title_.setText("New Project", juce::dontSendNotification);
    title_.setFont(juce::FontOptions(18.0f, juce::Font::bold));
    title_.setColour(juce::Label::textColourId, colours::text);
    heading(name_label_, "Project");
    heading(scoring_label_, "Scoring");
    heading(music_label_, "Key, meter and tempo");
    heading(picture_label_, "Picture and audio");
    name_.setTitle("Project name");
    name_.setText("Untitled", false);
    name_.onTextChange = [this] { refresh_labels(); };
    folder_label_.setColour(juce::Label::textColourId, colours::text_dim);
    folder_label_.setFont(juce::FontOptions(12.0f));
    choose_folder_.onClick = [this] {
        const juce::Component::SafePointer<NewProjectWizard> safe(this);
        chooser_ = std::make_unique<juce::FileChooser>("Where to keep the project", folder_);
        chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                              [safe](const juce::FileChooser& fc) {
                                  if (safe && fc.getResult().isDirectory()) safe->set_folder(fc.getResult());
                              });
    };
    category_.setTitle("Scoring category");
    for (int i = 0; i < 7; ++i) category_.addItem(kCategories[i].second, i + 1);
    category_.setSelectedId(1, juce::dontSendNotification);
    category_.onChange = [this] {
        fill_moods();
        fill_templates();
    };
    mood_.setTitle("Mood");
    mood_.onChange = [this] { fill_templates(); };
    template_box_.setTitle("Template");
    template_box_.onChange = [this] { template_changed(); };
    about_.setFont(juce::FontOptions(12.0f));
    about_.setColour(juce::Label::textColourId, colours::text_dim);
    about_.setJustificationType(juce::Justification::topLeft);
    tonic_.setTitle("Key");
    mode_.setTitle("Mode");
    dialogs::fill_key_combos(tonic_, mode_, theory::Key::parse("C major").value(), false);
    meter_.setTitle("Time signature");
    meter_.setEditableText(true);
    for (int i = 0; i < 11; ++i) meter_.addItem(kMeters[i], i + 1);
    meter_.setSelectedId(1, juce::dontSendNotification);
    tempo_.setTitle("Tempo");
    tempo_.setRange(20.0, 400.0, 1.0);
    tempo_.setValue(120.0, juce::dontSendNotification);
    tempo_.setTextValueSuffix(" BPM");
    frame_rate_.setTitle("Frame rate");
    for (int i = 0; i < 12; ++i) frame_rate_.addItem(to_string(kRates[i]), i + 1);
    frame_rate_.setSelectedId(2, juce::dontSendNotification);
    start_tc_.setTitle("Start timecode");
    start_tc_.setText("01:00:00:00", false);
    sample_rate_.setTitle("Sample rate");
    for (int i = 0; i < 4; ++i) sample_rate_.addItem(juce::String(kSampleRates[i]) + " Hz", i + 1);
    sample_rate_.setSelectedId(2, juce::dontSendNotification);
    picture_file_.setFont(juce::FontOptions(12.0f));
    picture_file_.setColour(juce::Label::textColourId, colours::text_dim);
    choose_picture_.onClick = [this] {
        const juce::Component::SafePointer<NewProjectWizard> safe(this);
        chooser_ = std::make_unique<juce::FileChooser>("Picture to import", juce::File(), "*.mov;*.mp4;*.m4v;*.mxf;*.avi;*.mkv");
        chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [safe](const juce::FileChooser& fc) {
                                  if (safe && fc.getResult().existsAsFile()) safe->set_picture(fc.getResult());
                              });
    };
    clear_picture_.onClick = [this] { set_picture({}); };
    status_.setFont(juce::FontOptions(12.0f));
    status_.setColour(juce::Label::textColourId, colours::accent_warm);
    create_.setComponentID("wizard_create");
    create_.onClick = [this] {
        if (create().isEmpty())
            if (auto* w = getTopLevelComponent()) w->setVisible(false);
    };
    cancel_.onClick = [this] {
        if (auto* w = getTopLevelComponent()) w->setVisible(false);
    };
    for (auto* c : std::initializer_list<juce::Component*>{
             &title_, &name_label_, &name_, &folder_label_, &choose_folder_, &scoring_label_, &category_, &mood_, &template_box_,
             &about_, &music_label_, &tonic_, &mode_, &meter_, &tempo_, &picture_label_, &frame_rate_, &start_tc_, &sample_rate_,
             &picture_file_, &choose_picture_, &clear_picture_, &status_, &create_, &cancel_})
        addAndMakeVisible(*c);
    set_folder(juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("VC Composer 2"));
    fill_moods();
    fill_templates();
}

NewProjectWizard::~NewProjectWizard()
{
    if (wizard_slot() == this) wizard_slot() = nullptr;
}

void NewProjectWizard::set_folder(const juce::File& folder)
{
    folder_ = folder;
    refresh_labels();
}

void NewProjectWizard::refresh_labels()
{
    const auto name = juce::String(session::package::safe_name(name_.getText().trim().toStdString()));
    folder_label_.setText(folder_ == juce::File() ? juce::String("Not saved until you choose Save")
                                                  : "Saved as " + folder_.getChildFile(name).getChildFile(name + ".vcc2").getFullPathName(),
                          juce::dontSendNotification);
    picture_file_.setText(picture_ == juce::File() ? juce::String("No picture (import it later from Picture)")
                                                   : picture_.getFileName(),
                          juce::dontSendNotification);
}

void NewProjectWizard::fill_moods()
{
    mood_.clear(juce::dontSendNotification);
    mood_ids_.clear();
    const int cat = category_.getSelectedId() - 1;
    const std::string category = cat >= 0 ? kCategories[cat].first : "";
    mood_.setEnabled(!category.empty());
    mood_.addItem("Any mood", 1);
    mood_ids_.push_back("");
    if (!category.empty()) {
        std::set<std::string> moods;
        for (const auto* t : context_.catalogs.templates->list())
            if ((category == "custom" ? !t->built_in : t->category == category && t->built_in) && t->instantiable())
                moods.insert(t->subcategory);
        for (const auto& m : moods) {
            if (m.empty()) continue;
            mood_.addItem(pretty(m), static_cast<int>(mood_ids_.size()) + 1);
            mood_ids_.push_back(m);
        }
    }
    mood_.setSelectedId(1, juce::dontSendNotification);
}

void NewProjectWizard::fill_templates()
{
    template_box_.clear(juce::dontSendNotification);
    template_ids_.clear();
    const int cat = category_.getSelectedId() - 1;
    const std::string category = cat >= 0 ? kCategories[cat].first : "";
    const int mood_index = mood_.getSelectedId() - 1;
    const std::string mood = mood_index >= 0 && mood_index < static_cast<int>(mood_ids_.size())
                                 ? mood_ids_[static_cast<std::size_t>(mood_index)]
                                 : "";
    template_box_.setEnabled(!category.empty());
    if (category.empty()) {
        template_box_.addItem("(no template: an empty project)", 1);
        template_ids_.push_back("");
    } else {
        for (const auto* t : context_.catalogs.templates->list()) {
            const bool in = category == "custom" ? !t->built_in : (t->category == category && t->built_in);
            if (!in || !t->instantiable() || (!mood.empty() && t->subcategory != mood)) continue;
            template_box_.addItem(t->display_name, static_cast<int>(template_ids_.size()) + 1);
            template_ids_.push_back(t->template_id);
        }
        if (template_ids_.empty()) {
            template_box_.addItem("(none in this category)", 1);
            template_ids_.push_back("");
        }
    }
    template_box_.setSelectedId(1, juce::dontSendNotification);
    template_changed();
}

void NewProjectWizard::template_changed()
{
    const int i = template_box_.getSelectedId() - 1;
    template_ = i >= 0 && i < static_cast<int>(template_ids_.size()) ? template_ids_[static_cast<std::size_t>(i)] : "";
    const auto* t = template_.empty() ? nullptr : context_.catalogs.templates->get(template_);
    if (!t) {
        about_.setText("An empty project: add tracks yourself, or a template later (Track > Add Template).",
                       juce::dontSendNotification);
        return;
    }
    juce::String about = juce::String(t->description);
    about << "\n" << static_cast<int>(t->tracks.size()) << " tracks";
    if (!t->ensemble_size.empty()) about << ", " << t->ensemble_size << " ensemble";
    about << " - " << t->tempo_low << "-" << t->tempo_high << " BPM suggested";
    about_.setText(about, juce::dontSendNotification);
    // The template's recommendations, to keep or change.
    for (const auto& km : t->recommended_key_modes)
        if (km.mode != "custom") {
            set_key(km.tonic + " " + km.mode);
            break;
        }
    if (!t->meter_options.empty()) set_meter(t->meter_options.front());
    set_tempo(std::round((t->tempo_low + t->tempo_high) / 2.0));
    if (name_.getText().trim() == "Untitled" || name_.getText().trim().isEmpty()) name_.setText(t->display_name, false);
    refresh_labels();
}

void NewProjectWizard::choose(const std::string& category, const std::string& mood, const std::string& template_id)
{
    for (int i = 0; i < 7; ++i)
        if (category == kCategories[i].first) category_.setSelectedId(i + 1, juce::dontSendNotification);
    fill_moods();
    for (std::size_t i = 0; i < mood_ids_.size(); ++i)
        if (mood_ids_[i] == mood) mood_.setSelectedId(static_cast<int>(i) + 1, juce::dontSendNotification);
    fill_templates();
    for (std::size_t i = 0; i < template_ids_.size(); ++i)
        if (template_ids_[i] == template_id) template_box_.setSelectedId(static_cast<int>(i) + 1, juce::dontSendNotification);
    template_changed();
}

void NewProjectWizard::set_key(const std::string& key)
{
    if (const auto k = theory::Key::parse(key)) dialogs::fill_key_combos(tonic_, mode_, *k, false);
}

void NewProjectWizard::set_meter(const juce::String& meter) { meter_.setText(meter, juce::dontSendNotification); }
void NewProjectWizard::set_tempo(double bpm) { tempo_.setValue(bpm, juce::dontSendNotification); }

void NewProjectWizard::set_frame_rate(FrameRate rate)
{
    for (int i = 0; i < 12; ++i)
        if (kRates[i] == rate) frame_rate_.setSelectedId(i + 1, juce::dontSendNotification);
}

void NewProjectWizard::set_start_timecode(const juce::String& tc) { start_tc_.setText(tc, false); }

void NewProjectWizard::set_sample_rate(int rate)
{
    for (int i = 0; i < 4; ++i)
        if (kSampleRates[i] == rate) sample_rate_.setSelectedId(i + 1, juce::dontSendNotification);
}

session::ProjectSetup NewProjectWizard::setup() const
{
    session::ProjectSetup s;
    s.name = name_.getText().trim().toStdString();
    if (s.name.empty()) s.name = "Untitled";
    s.folder = folder_ == juce::File() ? std::string() : folder_.getFullPathName().toStdString();
    s.template_id = template_;
    if (const auto k = dialogs::key_from_combos(tonic_, mode_)) s.key = k->name();
    s.meter = TimeSignature::parse(meter_.getText().trim().toStdString());
    s.tempo = tempo_.getValue();
    const int fr = frame_rate_.getSelectedId() - 1;
    s.frame_rate = fr >= 0 && fr < 12 ? kRates[fr] : FrameRate::Fps24;
    s.start_timecode = start_tc_.getText().trim().toStdString();
    const int sr = sample_rate_.getSelectedId() - 1;
    s.sample_rate = sr >= 0 && sr < 4 ? kSampleRates[sr] : 48000;
    return s;
}

juce::String NewProjectWizard::create()
{
    auto s = setup();
    if (!TimeSignature::parse(meter_.getText().trim().toStdString())) {
        message_ = "\"" + meter_.getText() + "\" is not a time signature (try 4/4 or 7/8)";
        status_.setText(message_, juce::dontSendNotification);
        return message_;
    }
    if (s.folder.size()) juce::File(juce::String(s.folder)).createDirectory();
    context_.engine.stop();
    const auto r = context_.session.create_project(s);
    if (!r.ok) {
        message_ = "Not created: " + juce::String(r.error);
        status_.setText(message_, juce::dontSendNotification);
        return message_;
    }
    context_.engine.locate(0);
    message_ = "Created " + juce::String(s.name);
    if (!r.warnings.empty())
        message_ << " - " << static_cast<int>(r.warnings.size()) << " track(s) use placeholders until you assign sounds (Library)";
    if (picture_ != juce::File()) {
        juce::String error;
        if (!import_picture_file(context_, picture_, error)) message_ << " - the picture was not imported: " << error;
    }
    if (context_.status) context_.status(message_);
    return {};
}

void NewProjectWizard::paint(juce::Graphics& g) { g.fillAll(colours::panel); }

void NewProjectWizard::resized()
{
    auto b = getLocalBounds().reduced(16);
    title_.setBounds(b.removeFromTop(28));
    b.removeFromTop(4);
    auto line = [&](int h) {
        auto r = b.removeFromTop(h);
        b.removeFromTop(6);
        return r;
    };
    name_label_.setBounds(line(16));
    auto r = line(26);
    choose_folder_.setBounds(r.removeFromRight(100));
    r.removeFromRight(8);
    name_.setBounds(r);
    folder_label_.setBounds(line(18));
    scoring_label_.setBounds(line(16));
    r = line(26);
    const int third = (r.getWidth() - 16) / 3;
    category_.setBounds(r.removeFromLeft(third));
    r.removeFromLeft(8);
    mood_.setBounds(r.removeFromLeft(third));
    r.removeFromLeft(8);
    template_box_.setBounds(r);
    about_.setBounds(line(48));
    music_label_.setBounds(line(16));
    r = line(26);
    tonic_.setBounds(r.removeFromLeft(90));
    r.removeFromLeft(6);
    mode_.setBounds(r.removeFromLeft(150));
    r.removeFromLeft(12);
    meter_.setBounds(r.removeFromLeft(90));
    r.removeFromLeft(12);
    tempo_.setBounds(r.removeFromLeft(170));
    picture_label_.setBounds(line(16));
    r = line(26);
    frame_rate_.setBounds(r.removeFromLeft(160));
    r.removeFromLeft(8);
    start_tc_.setBounds(r.removeFromLeft(120));
    r.removeFromLeft(8);
    sample_rate_.setBounds(r.removeFromLeft(120));
    r = line(26);
    clear_picture_.setBounds(r.removeFromRight(60));
    r.removeFromRight(6);
    choose_picture_.setBounds(r.removeFromRight(140));
    r.removeFromRight(8);
    picture_file_.setBounds(r);
    auto bottom = b.removeFromBottom(30);
    create_.setBounds(bottom.removeFromRight(110));
    bottom.removeFromRight(8);
    cancel_.setBounds(bottom.removeFromRight(90));
    bottom.removeFromRight(8);
    status_.setBounds(bottom);
}

void show_new_project_wizard(AppContext& context)
{
    dialogs::show_tool(wizard_window(), "New Project", 720, 560, [&context] { return new NewProjectWizard(context); });
}

NewProjectWizard* new_project_wizard() { return wizard_slot(); }

}  // namespace vcc::desktop
