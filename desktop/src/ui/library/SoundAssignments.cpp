#include "ui/library/SoundAssignments.h"

#include <algorithm>
#include <set>

#include "ui/Dialogs.h"
#include "ui/Theme.h"
#include "ui/library/LibraryBrowser.h"

namespace vcc::desktop {

namespace {

juce::Component::SafePointer<dialogs::ToolWindow>& window_slot()
{
    static juce::Component::SafePointer<dialogs::ToolWindow> w;
    return w;
}
SoundAssignmentsPanel*& panel_slot()
{
    static SoundAssignmentsPanel* p = nullptr;
    return p;
}

juce::String articulation_label(const std::string& a)
{
    if (a.empty()) return "Whole instrument (one track)";
    juce::String s(a);
    s = s.replace("_", " ");
    return s.substring(0, 1).toUpperCase() + s.substring(1);
}

}  // namespace

/// One articulation: its name, the sound assigned, Use / Choose / Clear.
class SoundAssignmentsPanel::Row final : public juce::Component {
public:
    Row(SoundAssignmentsPanel& owner, std::string articulation) : owner_(owner), articulation_(std::move(articulation))
    {
        setComponentID("assignment_" + juce::String(articulation_.empty() ? "whole" : articulation_));
        name_.setText(articulation_label(articulation_), juce::dontSendNotification);
        name_.setColour(juce::Label::textColourId, colours::text);
        sound_.setColour(juce::Label::textColourId, colours::text_dim);
        for (auto* l : {&name_, &sound_}) addAndMakeVisible(*l);
        use_.setTooltip("Assign the sound chosen in the Library to this articulation");
        choose_.setTooltip("Pick from your saved sounds and instrument plug-ins");
        clear_.setTooltip("No sound assigned (the built-in sound, or whatever the track has)");
        use_.onClick = [this] { owner_.use_carried(articulation_); };
        choose_.onClick = [this] { owner_.choose_menu(articulation_, &choose_); };
        clear_.onClick = [this] { owner_.clear(articulation_); };
        for (auto* b : {&use_, &choose_, &clear_}) addAndMakeVisible(*b);
        refresh();
    }
    void refresh()
    {
        const auto text = owner_.assigned_text(articulation_);
        sound_.setText(text.isEmpty() ? juce::String("- not assigned -") : text, juce::dontSendNotification);
        sound_.setColour(juce::Label::textColourId, text.isEmpty() ? colours::text_dim : colours::text);
        use_.setEnabled(owner_.carried_.has_value());
        clear_.setEnabled(text.isNotEmpty());
    }
    void resized() override
    {
        auto r = getLocalBounds().reduced(4, 3);
        name_.setBounds(r.removeFromLeft(170));
        clear_.setBounds(r.removeFromRight(56));
        r.removeFromRight(4);
        choose_.setBounds(r.removeFromRight(76));
        r.removeFromRight(4);
        use_.setBounds(r.removeFromRight(56));
        r.removeFromRight(6);
        sound_.setBounds(r);
    }
    void paint(juce::Graphics& g) override
    {
        g.setColour(colours::panel.brighter(0.04f));
        g.fillRoundedRectangle(getLocalBounds().toFloat().reduced(1.0f), 4.0f);
    }

private:
    SoundAssignmentsPanel& owner_;
    std::string articulation_;
    juce::Label name_, sound_;
    juce::TextButton use_{"Use"}, choose_{"Choose..."}, clear_{"Clear"};
};

SoundAssignmentsPanel::SoundAssignmentsPanel(AppContext& context) : context_(context)
{
    setComponentID("sound_assignments");
    panel_slot() = this;
    title_.setText("Sound Assignments", juce::dontSendNotification);
    title_.setFont(juce::FontOptions(16.0f, juce::Font::bold));
    hint_.setText("Choose your sound for each instrument and articulation once: it goes in by itself in every template, "
                  "every new track and every articulation track.",
                  juce::dontSendNotification);
    hint_.setColour(juce::Label::textColourId, colours::text_dim);
    carried_label_.setColour(juce::Label::textColourId, colours::text);
    for (auto* l : {&title_, &hint_, &carried_label_}) {
        l->setJustificationType(juce::Justification::topLeft);
        addAndMakeVisible(*l);
    }
    family_.setTitle("Family");
    family_.addItem("All instruments", 1);
    int id = 2;
    for (const auto& f : templates::family_order()) family_.addItem(templates::family_display(f), id++);
    family_.setSelectedId(1, juce::dontSendNotification);
    family_.onChange = [this] { refilter(); };
    search_.setTitle("Search instruments");
    search_.setTextToShowWhenEmpty("Search instruments...", colours::text_dim);
    search_.onTextChange = [this] { refilter(); };
    instruments_list_.setRowHeight(24);
    instruments_list_.setTitle("Instruments");
    for (auto* c : std::initializer_list<juce::Component*>{&family_, &search_, &instruments_list_, &rows_view_}) addAndMakeVisible(c);
    rows_view_.setViewedComponent(&rows_holder_, false);
    rows_view_.setScrollBarsShown(true, false);
    add_articulation_.onClick = [this] {
        if (instrument_.empty()) return;
        juce::PopupMenu m;
        for (const auto& a : context_.catalogs.articulation_ids) {
            if (std::find(rows_.begin(), rows_.end(), a) != rows_.end()) continue;
            m.addItem(articulation_label(a), [this, a] {
                rows_.push_back(a);
                rebuild_rows();
            });
        }
        m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&add_articulation_));
    };
    apply_.setTooltip("Put these sounds on this project's tracks now, replacing what they have");
    apply_.onClick = [this] {
        const int n = apply_to_project();
        context_.status(n == 0 ? juce::String("No tracks here play an assigned sound")
                               : "Assigned sounds on " + juce::String(n) + (n == 1 ? " track" : " tracks"));
    };
    export_.setTooltip("Save your assignments and the sounds (with their settings) to a file: for safekeeping or another computer");
    import_.setTooltip("Load assignments saved on this or another computer");
    export_.onClick = [this] {
        chooser_ = std::make_unique<juce::FileChooser>("Export sound assignments",
                                                       juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
                                                           .getChildFile("VC Composer Sounds.vccsounds"),
                                                       "*.vccsounds");
        chooser_->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting,
                              [this](const juce::FileChooser& fc) {
                                  if (fc.getResult() != juce::File()) context_.status(export_to(fc.getResult()));
                              });
    };
    import_.onClick = [this] {
        chooser_ = std::make_unique<juce::FileChooser>("Import sound assignments",
                                                       juce::File::getSpecialLocation(juce::File::userDocumentsDirectory),
                                                       "*.vccsounds;*.json");
        chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [this](const juce::FileChooser& fc) {
                                  if (fc.getResult().existsAsFile()) context_.status(import_from(fc.getResult()));
                              });
    };
    for (auto* b : {&add_articulation_, &apply_, &export_, &import_}) addAndMakeVisible(*b);
    refilter();
    setSize(760, 560);
}

SoundAssignmentsPanel::~SoundAssignmentsPanel()
{
    if (panel_slot() == this) panel_slot() = nullptr;
}

void SoundAssignmentsPanel::set_carried(std::optional<library::Entry> entry)
{
    carried_ = std::move(entry);
    carried_label_.setText(carried_ ? "Sound to assign: " + juce::String(carried_->display) +
                                          " - press Use on each articulation it plays"
                                    : juce::String("Choose a sound in the Library and press Assign, or use Choose... on a row"),
                           juce::dontSendNotification);
    if (carried_ && !carried_->instrument_id.empty()) show_instrument(carried_->instrument_id);
    for (auto& r : row_comps_) r->refresh();
}

void SoundAssignmentsPanel::refilter()
{
    instruments_.clear();
    const auto q = search_.getText().toLowerCase();
    const int fam = family_.getSelectedId();
    const auto& order = templates::family_order();
    for (const auto& def : context_.catalogs.instruments.all()) {
        if (fam > 1 && static_cast<std::size_t>(fam - 2) < order.size() && def.family != order[static_cast<std::size_t>(fam - 2)])
            continue;
        if (q.isNotEmpty() && !juce::String(def.name).toLowerCase().contains(q)) continue;
        instruments_.push_back(&def);
    }
    instruments_list_.updateContent();
    instruments_list_.repaint();
}

int SoundAssignmentsPanel::getNumRows() { return static_cast<int>(instruments_.size()); }

void SoundAssignmentsPanel::paintListBoxItem(int row, juce::Graphics& g, int width, int height, bool selected)
{
    if (row < 0 || row >= static_cast<int>(instruments_.size())) return;
    const auto* def = instruments_[static_cast<std::size_t>(row)];
    if (selected || def->id == instrument_) g.fillAll(colours::accent.withAlpha(0.3f));
    // A dot: it has sounds assigned.
    bool any = false;
    if (context_.library)
        for (const auto& a : context_.library->lib().assignments()) any = any || a.instrument_id == def->id;
    g.setColour(any ? colours::accent : colours::text);
    g.setFont(13.0f);
    g.drawText((any ? juce::String::fromUTF8("\xe2\x97\x8f ") : juce::String("   ")) + juce::String(def->name), 6, 0, width - 8,
               height, juce::Justification::centredLeft, true);
}

void SoundAssignmentsPanel::selectedRowsChanged(int row)
{
    if (row >= 0 && row < static_cast<int>(instruments_.size())) show_instrument(instruments_[static_cast<std::size_t>(row)]->id);
}

std::vector<std::string> SoundAssignmentsPanel::articulations_for(const std::string& instrument_id) const
{
    std::set<std::string> found;
    if (context_.catalogs.templates)
        for (const auto* t : context_.catalogs.templates->list())
            for (const auto& tt : t->tracks)
                if (tt.instrument_standard_id == instrument_id) found.insert(tt.articulation_map.begin(), tt.articulation_map.end());
    if (context_.library) {
        for (const auto& lib : context_.library->lib().libraries())
            for (const auto& p : lib.patches)
                if (p.instrument_id == instrument_id) found.insert(p.articulations.begin(), p.articulations.end());
        for (const auto& a : context_.library->lib().assignments())
            if (a.instrument_id == instrument_id && !a.articulation.empty()) found.insert(a.articulation);
    }
    if (found.empty()) found = {"sustain", "legato", "staccato"};
    // In the vocabulary's order (sustain, legato, staccato...), then any others.
    std::vector<std::string> out{""};
    for (const auto& a : context_.catalogs.articulation_ids)
        if (found.erase(a)) out.push_back(a);
    out.insert(out.end(), found.begin(), found.end());
    return out;
}

void SoundAssignmentsPanel::show_instrument(const std::string& instrument_id)
{
    instrument_ = instrument_id;
    rows_ = articulations_for(instrument_id);
    for (std::size_t i = 0; i < instruments_.size(); ++i)
        if (instruments_[i]->id == instrument_id) {
            instruments_list_.selectRow(static_cast<int>(i), false, true);
            instruments_list_.scrollToEnsureRowIsOnscreen(static_cast<int>(i));
        }
    rebuild_rows();
}

void SoundAssignmentsPanel::rebuild_rows()
{
    row_comps_.clear();
    int y = 0;
    for (const auto& a : rows_) {
        row_comps_.push_back(std::make_unique<Row>(*this, a));
        rows_holder_.addAndMakeVisible(*row_comps_.back());
        row_comps_.back()->setBounds(0, y, std::max(300, rows_view_.getWidth() - 12), 32);
        y += 34;
    }
    rows_holder_.setSize(std::max(300, rows_view_.getWidth() - 12), y);
    instruments_list_.repaint();
    repaint();
}

juce::String SoundAssignmentsPanel::assigned_text(const std::string& articulation) const
{
    if (!context_.library || instrument_.empty()) return {};
    const auto* p = context_.library->lib().assigned(instrument_, articulation);
    return p ? juce::String(p->display) : juce::String();
}

void SoundAssignmentsPanel::assign(const std::string& articulation, const std::string& patch_id)
{
    if (!context_.library) return;
    context_.library->lib().set_assignment(instrument_, articulation, patch_id);
    context_.library->changed();  // saved, and the session told
    for (auto& r : row_comps_) r->refresh();
    instruments_list_.repaint();
}

juce::String SoundAssignmentsPanel::use_carried(const std::string& articulation)
{
    if (!carried_ || !context_.library || instrument_.empty()) return "Choose a sound in the Library first";
    const auto id = context_.library->saved_sound_for(*carried_, instrument_, articulation);
    if (id.empty()) return "Nothing here can play " + juce::String(carried_->display) + " (its player is not installed)";
    assign(articulation, id);
    const auto* def = context_.catalogs.instruments.find(instrument_);
    const auto msg = juce::String(def ? def->name : instrument_) + " - " + articulation_label(articulation) + ": " +
                     juce::String(carried_->display);
    context_.status(msg);
    return msg;
}

void SoundAssignmentsPanel::clear(const std::string& articulation) { assign(articulation, {}); }

void SoundAssignmentsPanel::choose_menu(const std::string& articulation, juce::Component* target)
{
    if (!context_.library) return;
    juce::PopupMenu m;
    auto& lib = context_.library->lib();
    // Your saved sounds for this instrument first, then every saved sound, then plug-ins.
    juce::PopupMenu mine, others, plugins;
    for (const auto& p : lib.patches()) {
        const std::string id = p.id;
        (p.instrument_id == instrument_ ? mine : others).addItem(juce::String(p.display), [this, articulation, id] { assign(articulation, id); });
    }
    for (const auto& e : lib.entries()) {
        if (e.kind != library::EntryKind::Plugin || e.plugin_identifier.empty()) continue;
        const auto entry = e;
        plugins.addItem(juce::String(e.display), [this, articulation, entry] {
            if (!context_.library) return;
            const auto id = context_.library->saved_sound_for(entry, instrument_, articulation);
            if (!id.empty()) assign(articulation, id);
        });
    }
    if (mine.getNumItems() > 0) m.addSectionHeader("Your sounds for this instrument"), m.addSubMenu("Saved for this instrument", mine);
    if (others.getNumItems() > 0) m.addSubMenu("Other saved sounds", others);
    if (plugins.getNumItems() > 0) m.addSubMenu("Instrument plug-ins", plugins);
    if (m.getNumItems() == 0)
        m.addItem("No saved sounds or plug-ins yet: scan for plug-ins, or save a track's sound in the Library", false, false, [] {});
    m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(target));
}

int SoundAssignmentsPanel::apply_to_project()
{
    std::vector<std::string> ids;
    for (const auto* t : context_.session.music_tracks())
        if (context_.session.assigned_sound(*t)) ids.push_back(t->id);
    if (ids.empty()) return 0;
    // Replacing what they have: cleared first, then the assigned sound goes in (one undo step).
    context_.session.edit("Apply assigned sounds", [&](project::ProjectData& d) {
        for (const auto& id : ids)
            if (auto* t = d.find_track(id)) t->instrument_plugin = {};
        return true;
    });
    return context_.session.apply_assigned_sounds(ids);
}

juce::String SoundAssignmentsPanel::export_to(const juce::File& file)
{
    if (!context_.library) return "No library";
    auto& lib = context_.library->lib();
    std::vector<std::string> ids;
    for (const auto& a : lib.assignments()) ids.push_back(a.patch_id);
    if (ids.empty()) return "No sounds assigned yet";
    const auto pack = lib.export_pack("Sound assignments", ids, true);
    if (!file.replaceWithText(juce::String(pack.to_text(2)))) return "Could not write " + file.getFullPathName();
    return "Exported " + juce::String(static_cast<int>(lib.assignments().size())) + " assignments to " + file.getFileName();
}

juce::String SoundAssignmentsPanel::import_from(const juce::File& file)
{
    if (!context_.library) return "No library";
    const auto doc = json::parse(file.loadFileAsString().toStdString());
    if (!doc) return file.getFileName() + " is not a sound assignments file";
    std::vector<std::string> warnings;
    const int n = context_.library->lib().import_pack(*doc, &warnings);
    context_.library->changed();
    if (!instrument_.empty()) show_instrument(instrument_);
    return "Imported " + juce::String(n) + (n == 1 ? " sound" : " sounds") + " with their assignments" +
           (warnings.empty() ? juce::String() : " (" + juce::String(static_cast<int>(warnings.size())) + " skipped)");
}

void SoundAssignmentsPanel::paint(juce::Graphics& g) { g.fillAll(colours::background); }

void SoundAssignmentsPanel::resized()
{
    auto r = getLocalBounds().reduced(12);
    title_.setBounds(r.removeFromTop(24));
    hint_.setBounds(r.removeFromTop(34));
    carried_label_.setBounds(r.removeFromTop(22));
    r.removeFromTop(6);
    auto bottom = r.removeFromBottom(30);
    add_articulation_.setBounds(bottom.removeFromLeft(150).reduced(0, 2));
    bottom.removeFromLeft(8);
    apply_.setBounds(bottom.removeFromLeft(160).reduced(0, 2));
    import_.setBounds(bottom.removeFromRight(90).reduced(0, 2));
    bottom.removeFromRight(6);
    export_.setBounds(bottom.removeFromRight(90).reduced(0, 2));
    r.removeFromBottom(8);
    auto left = r.removeFromLeft(220);
    family_.setBounds(left.removeFromTop(26));
    left.removeFromTop(4);
    search_.setBounds(left.removeFromTop(26));
    left.removeFromTop(4);
    instruments_list_.setBounds(left);
    r.removeFromLeft(10);
    rows_view_.setBounds(r);
    rebuild_rows();
}

void show_sound_assignments(AppContext& context, std::optional<library::Entry> carried)
{
    dialogs::show_tool(window_slot(), "Sound Assignments", 760, 560, [&context] { return new SoundAssignmentsPanel(context); });
    if (auto* p = panel_slot()) p->set_carried(std::move(carried));
}

SoundAssignmentsPanel* sound_assignments_panel() { return panel_slot(); }

}  // namespace vcc::desktop
