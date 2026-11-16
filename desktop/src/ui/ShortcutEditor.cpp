#include "ui/ShortcutEditor.h"

#include <algorithm>

#include "AppResources.h"
#include "ui/Dialogs.h"
#include "ui/Theme.h"
#include "vcc/core/Json.h"
#include "vcc/ui/KeyMapping.h"

namespace vcc::desktop {

namespace {

juce::Component::SafePointer<dialogs::ToolWindow>& editor_window()
{
    static juce::Component::SafePointer<dialogs::ToolWindow> w;
    return w;
}
ShortcutEditor*& editor_slot()
{
    static ShortcutEditor* p = nullptr;
    return p;
}

juce::File user_folder() { return user_data_directory().getChildFile("Shortcuts"); }

juce::File file_for(const std::string& name)
{
    return user_folder().getChildFile(juce::File::createLegalFileName(juce::String(name)) + ".vccshortcuts");
}

}  // namespace

std::string shortcut_profile_to_text(const composer::ShortcutProfile& profile)
{
    json::Value o = json::Value::make_object();
    o.set("vcc_shortcuts", 1);
    o.set("name", profile.name());
    json::Value b = json::Value::make_array();
    for (const auto& [command, stroke] : profile.bindings()) {
        json::Value x = json::Value::make_object();
        x.set("command", composer::to_string(command));
        x.set("key", stroke.to_text());
        b.push_back(std::move(x));
    }
    o.set("bindings", std::move(b));
    return o.to_text(2);
}

std::optional<composer::ShortcutProfile> shortcut_profile_from_text(const std::string& text, std::vector<std::string>* problems)
{
    const auto doc = json::parse(text);
    if (!doc || doc->get_int("vcc_shortcuts", 0) < 1 || doc->get_string("name").empty()) return std::nullopt;
    composer::ShortcutProfile p(doc->get_string("name"));
    for (const auto& b : (*doc)["bindings"].as_array()) {
        composer::Command command;
        composer::KeyStroke stroke;
        if (!composer::command_from_string(b.get_string("command"), command) ||
            !composer::KeyStroke::from_text(b.get_string("key"), stroke)) {
            if (problems) problems->push_back(b.get_string("command") + " = " + b.get_string("key") + " not understood");
            continue;
        }
        if (!p.bind(command, stroke) && problems)
            problems->push_back(b.get_string("key") + " is bound twice; kept the first");
    }
    return p;
}

std::vector<composer::ShortcutProfile> all_shortcut_profiles()
{
    std::vector<composer::ShortcutProfile> out{composer::make_composer2_profile()};
    for (auto& p : composer::built_in_profiles()) out.push_back(std::move(p));
    for (const auto& f : user_folder().findChildFiles(juce::File::findFiles, false, "*.vccshortcuts")) {
        auto p = shortcut_profile_from_text(f.loadFileAsString().toStdString());
        if (!p) continue;
        if (std::none_of(out.begin(), out.end(), [&](const composer::ShortcutProfile& x) { return x.name() == p->name(); }))
            out.push_back(std::move(*p));
    }
    return out;
}

bool save_user_shortcut_profile(const composer::ShortcutProfile& profile)
{
    if (profile.is_built_in() || profile.name().empty()) return false;
    user_folder().createDirectory();
    return file_for(profile.name()).replaceWithText(juce::String(shortcut_profile_to_text(profile)));
}

bool delete_user_shortcut_profile(const std::string& name) { return file_for(name).deleteFile(); }

// --- Editor ---------------------------------------------------------------------------------

ShortcutEditor::ShortcutEditor(AppContext& context, std::function<void(const std::string&)> apply)
    : context_(context), apply_(std::move(apply)), commands_(composer::all_commands())
{
    setComponentID("shortcut_editor");
    editor_slot() = this;
    setWantsKeyboardFocus(true);
    profiles_.setTitle("Shortcut profile");
    profiles_.onChange = [this] { choose_profile(profiles_.getText().toStdString()); };
    list_.setTitle("Commands and keys");
    list_.setModel(this);
    list_.setRowHeight(24);
    status_.setFont(juce::FontOptions(12.0f));
    status_.setColour(juce::Label::textColourId, colours::text);
    conflicts_.setFont(juce::FontOptions(12.0f));
    set_.onClick = [this] { start_capture(list_.getSelectedRow()); };
    clear_.onClick = [this] {
        const int row = list_.getSelectedRow();
        if (row < 0 || profile_.is_built_in()) return;
        profile_.unbind(commands_[static_cast<std::size_t>(row)]);
        persist();
    };
    duplicate_.onClick = [this] { duplicate(); };
    use_.onClick = [this] {
        if (apply_) apply_(profile_.name());
        status_.setText("In use: " + juce::String(profile_.name()), juce::dontSendNotification);
    };
    delete_.onClick = [this] {
        if (profile_.is_built_in()) return;
        delete_user_shortcut_profile(profile_.name());
        refresh();
        choose_profile("Composer 2");
    };
    export_.onClick = [this] {
        const juce::Component::SafePointer<ShortcutEditor> safe(this);
        chooser_ = std::make_unique<juce::FileChooser>(
            "Export shortcuts",
            juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
                .getChildFile(juce::File::createLegalFileName(juce::String(profile_.name())) + ".vccshortcuts"),
            "*.vccshortcuts");
        chooser_->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting,
                              [safe](const juce::FileChooser& fc) {
                                  if (!safe || fc.getResult() == juce::File()) return;
                                  fc.getResult().withFileExtension("vccshortcuts").replaceWithText(
                                      juce::String(shortcut_profile_to_text(safe->profile_)));
                                  safe->status_.setText("Exported " + fc.getResult().getFileName(), juce::dontSendNotification);
                              });
    };
    import_.onClick = [this] {
        const juce::Component::SafePointer<ShortcutEditor> safe(this);
        chooser_ = std::make_unique<juce::FileChooser>("Import shortcuts", juce::File(), "*.vccshortcuts;*.json");
        chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [safe](const juce::FileChooser& fc) {
                                  if (!safe || !fc.getResult().existsAsFile()) return;
                                  std::vector<std::string> problems;
                                  auto p = shortcut_profile_from_text(fc.getResult().loadFileAsString().toStdString(), &problems);
                                  if (!p) {
                                      safe->status_.setText("Not a shortcut profile", juce::dontSendNotification);
                                      return;
                                  }
                                  for (const auto& x : all_shortcut_profiles())
                                      if (x.name() == p->name() && x.is_built_in()) p->set_name(p->name() + " (imported)");
                                  save_user_shortcut_profile(*p);
                                  const auto name = p->name();
                                  safe->refresh();
                                  safe->choose_profile(name);
                                  safe->status_.setText("Imported \"" + juce::String(name) + "\"" +
                                                            (problems.empty() ? juce::String() : " - " + juce::String(problems.front())),
                                                        juce::dontSendNotification);
                              });
    };
    for (auto* c : std::initializer_list<juce::Component*>{&profiles_, &list_, &status_, &conflicts_, &set_, &clear_, &duplicate_,
                                                          &import_, &export_, &delete_, &use_})
        addAndMakeVisible(*c);
    refresh();
    choose_profile(context_.shortcuts.name());
}

ShortcutEditor::~ShortcutEditor()
{
    if (editor_slot() == this) editor_slot() = nullptr;
}

void ShortcutEditor::refresh()
{
    profiles_.clear(juce::dontSendNotification);
    int id = 1;
    for (const auto& p : all_shortcut_profiles()) profiles_.addItem(juce::String(p.name()), id++);
}

void ShortcutEditor::choose_profile(const std::string& name)
{
    for (auto& p : all_shortcut_profiles())
        if (p.name() == name) profile_ = std::move(p);
    profiles_.setText(juce::String(profile_.name()), juce::dontSendNotification);
    const bool built_in = profile_.is_built_in();
    for (auto* b : {&set_, &clear_, &delete_}) b->setEnabled(!built_in);
    status_.setText(built_in ? "A factory profile: Duplicate it to change keys" : "Your profile: double-click a command to set its key",
                    juce::dontSendNotification);
    capture_row_ = -1;
    conflicts_.setText(conflicts_text(), juce::dontSendNotification);
    conflicts_.setColour(juce::Label::textColourId, profile_.conflicts().empty() ? colours::text_dim : colours::accent_warm);
    list_.updateContent();
    list_.repaint();
}

std::string ShortcutEditor::duplicate()
{
    const auto existing = all_shortcut_profiles();
    auto taken = [&](const std::string& n) {
        return std::any_of(existing.begin(), existing.end(), [&](const composer::ShortcutProfile& p) { return p.name() == n; });
    };
    std::string name = profile_.name() + " (mine)";
    for (int n = 2; taken(name); ++n) name = profile_.name() + " (mine " + std::to_string(n) + ")";
    auto copy = profile_.duplicate(name);
    save_user_shortcut_profile(copy);
    refresh();
    choose_profile(name);
    return name;
}

void ShortcutEditor::start_capture(int row)
{
    if (row < 0 || row >= rows() || profile_.is_built_in()) return;
    capture_row_ = row;
    list_.selectRow(row);
    status_.setText("Press the new key for \"" + juce::String(composer::describe(commands_[static_cast<std::size_t>(row)])) +
                        "\" (Escape cancels)",
                    juce::dontSendNotification);
    grabKeyboardFocus();
}

juce::String ShortcutEditor::take_stroke(const composer::KeyStroke& stroke, bool replace_if_taken)
{
    if (capture_row_ < 0) return {};
    const auto command = commands_[static_cast<std::size_t>(capture_row_)];
    const auto holder = profile_.command_for(stroke);
    juce::String message;
    if (holder != composer::Command::None && holder != command && !replace_if_taken) {
        pending_ = stroke;
        message = juce::String(stroke.to_text()) + " is used by \"" + composer::describe(holder) +
                  "\": press it again to move it here, or another key";
        status_.setText(message, juce::dontSendNotification);
        return message;
    }
    const auto lost = profile_.rebind(command, stroke);
    message = juce::String(stroke.to_text()) + " now " + composer::describe(command) +
              (lost != composer::Command::None && lost != command ? juce::String(" (taken from \"") + composer::describe(lost) + "\")"
                                                                   : juce::String());
    capture_row_ = -1;
    persist();
    status_.setText(message, juce::dontSendNotification);
    return message;
}

void ShortcutEditor::persist()
{
    save_user_shortcut_profile(profile_);
    if (context_.shortcuts.name() == profile_.name() && apply_) apply_(profile_.name());
    conflicts_.setText(conflicts_text(), juce::dontSendNotification);
    list_.updateContent();
    list_.repaint();
}

juce::String ShortcutEditor::conflicts_text() const
{
    const auto c = profile_.conflicts();
    if (c.empty()) return "No conflicts";
    juce::StringArray keys;
    for (const auto& k : c) keys.add(juce::String(k.to_text()));
    return "Bound twice: " + keys.joinIntoString(", ");
}

int ShortcutEditor::rows() const { return static_cast<int>(commands_.size()); }
int ShortcutEditor::getNumRows() { return rows(); }

void ShortcutEditor::paintListBoxItem(int row, juce::Graphics& g, int w, int h, bool selected)
{
    if (row < 0 || row >= rows()) return;
    const auto command = commands_[static_cast<std::size_t>(row)];
    g.fillAll(row == capture_row_ ? colours::accent.withAlpha(0.35f) : selected ? colours::selection : colours::panel);
    g.setColour(colours::text);
    g.setFont(juce::FontOptions(13.0f));
    g.drawText(composer::describe(command), 8, 0, w - 180, h, juce::Justification::centredLeft, true);
    const auto stroke = profile_.stroke_for(command);
    g.setColour(stroke.is_bound() ? colours::text : colours::text_dim);
    g.drawText(row == capture_row_ ? juce::String("press a key...") : stroke.is_bound() ? juce::String(stroke.to_text()) : juce::String("-"),
               w - 170, 0, 162, h, juce::Justification::centredRight, true);
}

bool ShortcutEditor::keyPressed(const juce::KeyPress& key)
{
    if (capture_row_ < 0) return false;
    if (key == juce::KeyPress::escapeKey) {
        capture_row_ = -1;
        status_.setText("Cancelled", juce::dontSendNotification);
        list_.repaint();
        return true;
    }
    composer::KeyStroke stroke;
    if (!ui::key_stroke_from(key, stroke)) return true;
    take_stroke(stroke, stroke == pending_);
    return true;
}

void ShortcutEditor::paint(juce::Graphics& g) { g.fillAll(colours::panel); }

void ShortcutEditor::resized()
{
    auto b = getLocalBounds().reduced(12);
    auto top = b.removeFromTop(26);
    use_.setBounds(top.removeFromRight(140));
    top.removeFromRight(8);
    duplicate_.setBounds(top.removeFromRight(96));
    top.removeFromRight(8);
    profiles_.setBounds(top);
    b.removeFromTop(8);
    auto bottom = b.removeFromBottom(28);
    for (auto* x : {&set_, &clear_, &import_, &export_, &delete_}) {
        x->setBounds(bottom.removeFromLeft(96));
        bottom.removeFromLeft(6);
    }
    b.removeFromBottom(6);
    conflicts_.setBounds(b.removeFromBottom(20));
    status_.setBounds(b.removeFromBottom(20));
    b.removeFromBottom(4);
    list_.setBounds(b);
}

void show_shortcut_editor(AppContext& context, std::function<void(const std::string&)> apply)
{
    dialogs::show_tool(editor_window(), "Keyboard Shortcuts", 640, 600,
                       [&context, apply] { return new ShortcutEditor(context, apply); });
}

ShortcutEditor* shortcut_editor() { return editor_slot(); }

}  // namespace vcc::desktop
