#include "ui/OstinatoLibrary.h"

#include <algorithm>

#include "AppResources.h"
#include "ui/Theme.h"

namespace vcc::desktop {

namespace {

juce::String utf8(const std::string& s) { return juce::String::fromUTF8(s.c_str()); }

}  // namespace

// --- The library ---------------------------------------------------------------------------------

OstinatoLibrary& OstinatoLibrary::get()
{
    static OstinatoLibrary library = [] {
        OstinatoLibrary l;
        l.file_ = user_data_directory().getChildFile("ostinato_presets.json");
        l.load();
        return l;
    }();
    return library;
}

void OstinatoLibrary::use_file(const juce::File& file)
{
    file_ = file;
    load();
}

void OstinatoLibrary::load()
{
    folders_.clear();
    presets_.clear();
    const auto doc = json::parse(file_.loadFileAsString().toStdString());
    if (!doc || !doc->is_object()) return;
    folders_ = doc->get_string_array("folders");
    if (const auto* list = doc->get("presets"))
        for (const auto& p : list->as_array()) {
            OstinatoUserPreset u;
            u.id = p.get_string("id");
            u.name = p.get_string("name");
            u.folder = p.get_string("folder");
            u.base_preset = p.get_string("base_preset");
            u.tonic = p.get_string("tonic");
            u.mode = p.get_string("mode");
            u.follow = static_cast<int>(p.get_int("follow", 1));
            u.length_bars = static_cast<int>(p.get_int("length_bars", 8));
            u.density = p.get_double("density", 90.0);
            u.swing = p.get_double("swing", 0.0);
            u.evolve = p.get_bool("evolve", false);
            u.call_response = p.get_bool("call_response", false);
            u.seed = p.get_string("seed");
            if (const auto* v = p.get("variant")) u.variant = *v;
            u.saved = p.get_string("saved");
            if (!u.id.empty()) presets_.push_back(std::move(u));
        }
}

void OstinatoLibrary::save() const
{
    json::Array folders, presets;
    for (const auto& f : folders_) folders.push_back(f);
    for (const auto& u : presets_)
        presets.push_back(json::Object{{"id", u.id},
                                       {"name", u.name},
                                       {"folder", u.folder},
                                       {"base_preset", u.base_preset},
                                       {"tonic", u.tonic},
                                       {"mode", u.mode},
                                       {"follow", u.follow},
                                       {"length_bars", u.length_bars},
                                       {"density", u.density},
                                       {"swing", u.swing},
                                       {"evolve", u.evolve},
                                       {"call_response", u.call_response},
                                       {"seed", u.seed},
                                       {"variant", u.variant},
                                       {"saved", u.saved}});
    const json::Value doc = json::Object{{"schema_version", 1}, {"folders", folders}, {"presets", presets}};
    file_.getParentDirectory().createDirectory();
    file_.replaceWithText(utf8(json::write(doc)));
}

const OstinatoUserPreset* OstinatoLibrary::find(const std::string& id) const
{
    for (const auto& p : presets_)
        if (p.id == id) return &p;
    return nullptr;
}

std::string OstinatoLibrary::add(OstinatoUserPreset preset)
{
    preset.id = juce::Uuid().toString().toStdString();
    if (preset.name.empty()) preset.name = "My ostinato";
    preset.saved = juce::Time::getCurrentTime().formatted("%d %b %Y %H:%M").toStdString();
    if (!preset.folder.empty() && std::find(folders_.begin(), folders_.end(), preset.folder) == folders_.end())
        folders_.push_back(preset.folder);
    presets_.push_back(preset);
    save();
    return preset.id;
}

bool OstinatoLibrary::rename(const std::string& id, const std::string& name)
{
    for (auto& p : presets_)
        if (p.id == id && !name.empty()) {
            p.name = name;
            save();
            return true;
        }
    return false;
}

bool OstinatoLibrary::move(const std::string& id, const std::string& folder)
{
    for (auto& p : presets_)
        if (p.id == id) {
            p.folder = folder;
            if (!folder.empty() && std::find(folders_.begin(), folders_.end(), folder) == folders_.end())
                folders_.push_back(folder);
            save();
            return true;
        }
    return false;
}

bool OstinatoLibrary::remove(const std::string& id)
{
    const auto it = std::find_if(presets_.begin(), presets_.end(), [&](const auto& p) { return p.id == id; });
    if (it == presets_.end()) return false;
    presets_.erase(it);
    save();
    return true;
}

bool OstinatoLibrary::add_folder(const std::string& name)
{
    if (name.empty() || std::find(folders_.begin(), folders_.end(), name) != folders_.end()) return false;
    folders_.push_back(name);
    save();
    return true;
}

bool OstinatoLibrary::rename_folder(const std::string& from, const std::string& to)
{
    const auto it = std::find(folders_.begin(), folders_.end(), from);
    if (it == folders_.end() || to.empty() || std::find(folders_.begin(), folders_.end(), to) != folders_.end()) return false;
    *it = to;
    for (auto& p : presets_)
        if (p.folder == from) p.folder = to;
    save();
    return true;
}

bool OstinatoLibrary::remove_folder(const std::string& name)
{
    const auto it = std::find(folders_.begin(), folders_.end(), name);
    if (it == folders_.end()) return false;
    folders_.erase(it);
    for (auto& p : presets_)
        if (p.folder == name) p.folder.clear();
    save();
    return true;
}

// --- The window ----------------------------------------------------------------------------------

namespace {

/// Asks for a name (a folder's or a preset's), then calls `done` with it.
void ask_name(const juce::String& title, const juce::String& prompt, const juce::String& initial,
              std::function<void(const std::string&)> done)
{
    auto* w = new juce::AlertWindow(title, prompt, juce::MessageBoxIconType::NoIcon);
    w->addTextEditor("name", initial);
    w->addButton("OK", 1, juce::KeyPress(juce::KeyPress::returnKey));
    w->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    w->enterModalState(true, juce::ModalCallbackFunction::create([w, done](int r) {
                           const auto name = w->getTextEditorContents("name").trim().toStdString();
                           if (r == 1 && !name.empty()) done(name);
                       }),
                       true);
}

class LibraryPanel final : public juce::Component, private juce::ListBoxModel {
public:
    explicit LibraryPanel(std::function<void(const OstinatoUserPreset&)> load) : load_(std::move(load))
    {
        setComponentID("ostinato_library");
        for (auto* l : {&folders_, &presets_}) {
            addAndMakeVisible(*l);
            l->setRowHeight(30);
            l->setColour(juce::ListBox::backgroundColourId, colours::panel);
            l->setColour(juce::ListBox::outlineColourId, colours::outline);
            l->setOutlineThickness(1);
        }
        folders_.setModel(&folder_model_);
        presets_.setModel(this);
        folders_.setTitle("Folders");
        presets_.setTitle("Presets");
        folder_model_.panel = this;
        for (auto* b : {&new_folder_, &rename_folder_, &delete_folder_, &load_key_, &rename_, &move_, &delete_})
            addAndMakeVisible(*b);
        load_key_.setColour(juce::TextButton::buttonColourId, colours::accent);
        new_folder_.onClick = [this] {
            ask_name("New Folder", "A name for the folder:", "", [this](const std::string& n) {
                OstinatoLibrary::get().add_folder(n);
                refresh();
            });
        };
        rename_folder_.onClick = [this] {
            const auto f = folder();
            if (f.empty()) return;
            ask_name("Rename Folder", "A new name for \"" + utf8(f) + "\":", utf8(f), [this, f](const std::string& n) {
                OstinatoLibrary::get().rename_folder(f, n);
                refresh();
            });
        };
        delete_folder_.onClick = [this] {
            const auto f = folder();
            if (f.empty()) return;
            OstinatoLibrary::get().remove_folder(f);  // its presets stay, out of any folder
            folders_.selectRow(0);
            refresh();
        };
        load_key_.onClick = [this] { load_selected(); };
        rename_.onClick = [this] {
            const auto* p = selected();
            if (!p) return;
            const auto id = p->id;
            ask_name("Rename Preset", "A new name:", utf8(p->name), [this, id](const std::string& n) {
                OstinatoLibrary::get().rename(id, n);
                refresh();
            });
        };
        move_.onClick = [this] {
            const auto* p = selected();
            if (!p) return;
            const auto id = p->id;
            juce::PopupMenu m;
            m.addItem("Not in a folder", [this, id] {
                OstinatoLibrary::get().move(id, {});
                refresh();
            });
            for (const auto& f : OstinatoLibrary::get().folders())
                m.addItem(utf8(f), [this, id, f] {
                    OstinatoLibrary::get().move(id, f);
                    refresh();
                });
            m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&move_));
        };
        delete_.onClick = [this] {
            if (const auto* p = selected()) {
                OstinatoLibrary::get().remove(p->id);
                refresh();
            }
        };
        refresh();
        folders_.selectRow(0);
        setSize(720, 460);
    }
    ~LibraryPanel() override
    {
        folders_.setModel(nullptr);
        presets_.setModel(nullptr);
    }

    /// The chosen folder: empty for "All presets" (row 0).
    std::string folder() const
    {
        const int r = folders_.getSelectedRow();
        const auto& f = OstinatoLibrary::get().folders();
        return r >= 1 && r - 1 < static_cast<int>(f.size()) ? f[static_cast<std::size_t>(r - 1)] : std::string();
    }
    void refresh()
    {
        shown_.clear();
        const auto f = folder();
        for (const auto& p : OstinatoLibrary::get().presets())
            if (f.empty() || p.folder == f) shown_.push_back(p.id);
        folders_.updateContent();
        presets_.updateContent();
        folders_.repaint();
        presets_.repaint();
        const bool in_folder = !f.empty();
        rename_folder_.setEnabled(in_folder);
        delete_folder_.setEnabled(in_folder);
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(colours::background);
        g.setColour(colours::text_dim);
        g.setFont(juce::FontOptions(11.5f, juce::Font::bold));
        g.drawText("FOLDERS", 12, 8, 200, 18, juce::Justification::centredLeft);
        g.drawText("YOUR OSTINATO PRESETS  -  double-click to load", 236, 8, 400, 18, juce::Justification::centredLeft);
    }
    void resized() override
    {
        auto r = getLocalBounds().reduced(12);
        r.removeFromTop(22);
        auto left = r.removeFromLeft(212);
        auto fb = left.removeFromBottom(28);
        new_folder_.setBounds(fb.removeFromLeft(70));
        fb.removeFromLeft(4);
        rename_folder_.setBounds(fb.removeFromLeft(66));
        fb.removeFromLeft(4);
        delete_folder_.setBounds(fb);
        left.removeFromBottom(6);
        folders_.setBounds(left);
        r.removeFromLeft(12);
        auto pb = r.removeFromBottom(28);
        load_key_.setBounds(pb.removeFromRight(100));
        pb.removeFromRight(8);
        delete_.setBounds(pb.removeFromLeft(70));
        pb.removeFromLeft(6);
        move_.setBounds(pb.removeFromLeft(110));
        pb.removeFromLeft(6);
        rename_.setBounds(pb.removeFromLeft(80));
        r.removeFromBottom(6);
        presets_.setBounds(r);
    }

private:
    // The presets list.
    int getNumRows() override { return static_cast<int>(shown_.size()); }
    void paintListBoxItem(int row, juce::Graphics& g, int w, int h, bool selected_row) override
    {
        if (row < 0 || row >= getNumRows()) return;
        const auto* p = OstinatoLibrary::get().find(shown_[static_cast<std::size_t>(row)]);
        if (!p) return;
        g.fillAll(selected_row ? colours::accent.withAlpha(0.28f) : (row % 2 ? colours::panel : colours::panel.brighter(0.025f)));
        g.setColour(colours::text);
        g.setFont(juce::FontOptions(13.5f));
        g.drawText(utf8(p->name), 10, 0, w / 2, h, juce::Justification::centredLeft, true);
        g.setColour(colours::text_dim);
        g.setFont(juce::FontOptions(11.0f));
        g.drawText(utf8(p->folder.empty() ? p->base_preset : p->folder + "  -  " + p->base_preset) + "  -  " + utf8(p->tonic) +
                       " " + utf8(p->mode) + "  -  " + utf8(p->saved),
                   w / 2, 0, w / 2 - 8, h, juce::Justification::centredRight, true);
    }
    void listBoxItemDoubleClicked(int, const juce::MouseEvent&) override { load_selected(); }

    const OstinatoUserPreset* selected() const
    {
        const int r = presets_.getSelectedRow();
        return r >= 0 && r < static_cast<int>(shown_.size()) ? OstinatoLibrary::get().find(shown_[static_cast<std::size_t>(r)])
                                                               : nullptr;
    }
    void load_selected()
    {
        if (const auto* p = selected(); p && load_) load_(*p);
    }

    // The folders list: "All presets", then the folders.
    struct FolderModel final : juce::ListBoxModel {
        LibraryPanel* panel = nullptr;
        int getNumRows() override { return 1 + static_cast<int>(OstinatoLibrary::get().folders().size()); }
        void paintListBoxItem(int row, juce::Graphics& g, int w, int h, bool selected_row) override
        {
            g.fillAll(selected_row ? colours::accent.withAlpha(0.28f) : colours::panel);
            g.setColour(row == 0 ? colours::text_dim : colours::text);
            g.setFont(juce::FontOptions(13.0f));
            const auto& f = OstinatoLibrary::get().folders();
            const juce::String name = row == 0 ? juce::String("All presets")
                                               : juce::String::fromUTF8("\xf0\x9f\x93\x81 ") +
                                                     utf8(f[static_cast<std::size_t>(row - 1)]);
            g.drawText(name, 10, 0, w - 16, h, juce::Justification::centredLeft, true);
        }
        void selectedRowsChanged(int) override
        {
            if (panel) panel->refresh();
        }
    } folder_model_;

    std::function<void(const OstinatoUserPreset&)> load_;
    juce::ListBox folders_, presets_;
    juce::TextButton new_folder_{"New"}, rename_folder_{"Rename"}, delete_folder_{"Delete"};
    juce::TextButton load_key_{"Load"}, rename_{"Rename"}, move_{"Move to Folder"}, delete_{"Delete"};
    std::vector<std::string> shown_;
};

juce::Component::SafePointer<juce::DialogWindow> library_window;

}  // namespace

void show_ostinato_library(std::function<void(const OstinatoUserPreset&)> load)
{
    if (library_window != nullptr) {
        library_window->toFront(true);
        return;
    }
    juce::DialogWindow::LaunchOptions o;
    o.content.setOwned(new LibraryPanel(std::move(load)));
    o.dialogTitle = "Custom Ostinato Presets";
    o.dialogBackgroundColour = colours::background;
    o.escapeKeyTriggersCloseButton = true;
    o.useNativeTitleBar = false;
    o.resizable = true;
    library_window = o.launchAsync();
}

}  // namespace vcc::desktop
