#include "ui/review/Relink.h"

#include "ui/Dialogs.h"
#include "ui/hw/HwTheme.h"

namespace vcc::desktop::review {

namespace {

juce::Component::SafePointer<dialogs::ToolWindow>& relink_window()
{
    static juce::Component::SafePointer<dialogs::ToolWindow> w;
    return w;
}
RelinkPanel*& relink_slot()
{
    static RelinkPanel* p = nullptr;
    return p;
}

constexpr int kNotFound = 1;
constexpr int kFirstCandidate = 10;
constexpr int kBrowse = 9000;

juce::String file_name(const std::string& stored)
{
    return juce::String(stored).replaceCharacter('\\', '/').fromLastOccurrenceOf("/", false, false);
}

}  // namespace

// --- A missing file ------------------------------------------------------------------------

class RelinkPanel::Row final : public juce::Component {
public:
    Row(RelinkPanel& panel, int index) : panel_(panel), index_(index)
    {
        const auto& a = panel.missing_[static_cast<std::size_t>(index)];
        name_.setText((a.kind == session::package::Asset::Kind::Picture ? "Picture: " : "") + file_name(a.stored),
                      juce::dontSendNotification);
        name_.setFont(hw::fonts::ui(13.0f, true));
        name_.setColour(juce::Label::textColourId, hw::tok::text_bright);
        juce::StringArray users;
        for (const auto& u : a.used_by) users.add(juce::String(u));
        where_.setText("Was " + juce::String(a.stored) + "  -  used by " + users.joinIntoString(", "),
                       juce::dontSendNotification);
        where_.setFont(hw::fonts::ui(11.0f));
        where_.setColour(juce::Label::textColourId, hw::tok::silk);
        where_.setTooltip(where_.getText());
        file_.setTitle("Relink " + file_name(a.stored));
        file_.setComponentID("relink_file_" + juce::String(index));
        file_.onChange = [this] {
            const int id = file_.getSelectedId();
            if (id == kBrowse) {
                browse();
                return;
            }
            const auto* m = match();
            if (id >= kFirstCandidate && m && id - kFirstCandidate < static_cast<int>(m->candidates.size()))
                panel_.chosen_[static_cast<std::size_t>(index_)] =
                    juce::File(juce::String(m->candidates[static_cast<std::size_t>(id - kFirstCandidate)].path))
                        .getFullPathName();
            else if (id == kNotFound)
                panel_.chosen_[static_cast<std::size_t>(index_)].clear();
            panel_.update_status();
        };
        for (auto* c : std::initializer_list<juce::Component*>{&name_, &where_, &file_}) addAndMakeVisible(*c);
        fill();
    }

    void fill()
    {
        file_.clear(juce::dontSendNotification);
        const auto* m = match();
        file_.addItem(!m                       ? juce::String("Not searched yet")
                      : m->candidates.empty() ? juce::String("Not found")
                      : m->ambiguous()        ? "Pick one: " + juce::String(static_cast<int>(m->candidates.size())) +
                                             " files could be this one"
                                              : juce::String("Leave missing"),
                      kNotFound);
        const juce::String chosen = panel_.chosen_[static_cast<std::size_t>(index_)];
        int selected = kNotFound;
        if (m) {
            for (std::size_t i = 0; i < m->candidates.size(); ++i) {
                const auto& c = m->candidates[i];
                const juce::String text = juce::String(c.path) + (c.same_name ? "" : "  (another format)");
                file_.addItem(text, kFirstCandidate + static_cast<int>(i));
                if (juce::File(juce::String(c.path)) == juce::File(chosen)) selected = kFirstCandidate + static_cast<int>(i);
            }
        }
        if (chosen.isNotEmpty() && selected == kNotFound) {
            // Picked by hand: shown as its own entry.
            file_.addItem(chosen, kBrowse - 1);
            selected = kBrowse - 1;
        }
        file_.addSeparator();
        file_.addItem("Choose a file...", kBrowse);
        file_.setSelectedId(selected, juce::dontSendNotification);
        const bool ambiguous = m && m->ambiguous() && chosen.isEmpty();
        file_.setColour(juce::ComboBox::outlineColourId,
                        ambiguous ? hw::tok::meter_amber : chosen.isNotEmpty() ? hw::tok::meter_green : hw::tok::edge);
        file_.setTooltip(ambiguous ? "More than one file could be this one: pick it" : chosen);
    }

    void resized() override
    {
        auto b = getLocalBounds().reduced(4, 3);
        auto top = b.removeFromTop(20);
        name_.setBounds(top);
        where_.setBounds(b.removeFromTop(18));
        file_.setBounds(b.removeFromTop(26));
    }
    void paint(juce::Graphics& g) override
    {
        g.setColour(hw::tok::edge);
        g.drawHorizontalLine(getHeight() - 1, 0.0f, static_cast<float>(getWidth()));
    }

private:
    const session::relink::Match* match() const
    {
        return index_ < static_cast<int>(panel_.matches_.size()) ? &panel_.matches_[static_cast<std::size_t>(index_)]
                                                                 : nullptr;
    }
    void browse()
    {
        const juce::Component::SafePointer<Row> safe(this);
        const auto& a = panel_.missing_[static_cast<std::size_t>(index_)];
        const juce::String name = file_name(a.stored);
        panel_.chooser_ = std::make_unique<juce::FileChooser>("Where is " + name + "?", juce::File(), "*");
        panel_.chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                                     [safe](const juce::FileChooser& fc) {
                                         if (!safe) return;
                                         if (fc.getResult().existsAsFile())
                                             safe->panel_.choose(safe->index_, fc.getResult().getFullPathName());
                                         else
                                             safe->fill();
                                     });
    }

    RelinkPanel& panel_;
    int index_;
    juce::Label name_, where_;
    juce::ComboBox file_;
    friend class RelinkPanel;
};

// --- The panel -----------------------------------------------------------------------------

RelinkPanel::RelinkPanel(AppContext& context) : context_(context)
{
    setComponentID("relink_panel");
    relink_slot() = this;
    title_.setFont(hw::fonts::ui(15.0f, true));
    title_.setColour(juce::Label::textColourId, hw::tok::text_bright);
    folders_label_.setFont(hw::fonts::ui(12.0f));
    folders_label_.setColour(juce::Label::textColourId, hw::tok::text);
    status_.setFont(hw::fonts::ui(12.0f, true));
    status_.setColour(juce::Label::textColourId, hw::tok::text);
    view_.setViewedComponent(&list_, false);
    view_.setScrollBarsShown(true, false);
    for (auto* c : std::initializer_list<juce::Component*>{&title_, &folders_label_, &status_, &add_folder_,
                                                          &clear_folders_, &search_, &cancel_, &relink_, &view_})
        addAndMakeVisible(*c);
    search_.setComponentID("relink_search");
    relink_.setComponentID("relink_apply");
    cancel_.setEnabled(false);
    add_folder_.onClick = [this] {
        const juce::Component::SafePointer<RelinkPanel> safe(this);
        chooser_ = std::make_unique<juce::FileChooser>("Search in", juce::File());
        chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                              [safe](const juce::FileChooser& fc) {
                                  if (safe && fc.getResult().isDirectory()) safe->add_folder(fc.getResult());
                              });
    };
    clear_folders_.onClick = [this] { clear_folders(); };
    search_.onClick = [this] { search(); };
    cancel_.onClick = [this] { cancel(); };
    relink_.onClick = [this] { relink(); };
    // Where moved media usually are: beside the project, or one folder up.
    const auto& path = context_.session.path();
    if (!path.empty()) {
        const juce::File dir = juce::File(juce::String(path)).getParentDirectory();
        add_folder(dir);
        add_folder(dir.getParentDirectory());
    }
    refresh();
    startTimerHz(15);
}

RelinkPanel::~RelinkPanel()
{
    stopTimer();
    if (worker_) {
        cancel_requested_ = true;
        worker_->join();
    }
    if (relink_slot() == this) relink_slot() = nullptr;
}

void RelinkPanel::refresh()
{
    missing_ = session::relink::missing(context_.session.data(), context_.session.path());
    matches_.clear();
    chosen_.assign(missing_.size(), {});
    message_.clear();
    rebuild_rows();
}

void RelinkPanel::add_folder(const juce::File& folder)
{
    if (folder == juce::File() || !folder.isDirectory()) return;
    // A folder inside one already listed adds nothing; one that holds a
    // listed folder replaces it.
    for (const auto& f : folders_)
        if (folder == juce::File(f) || folder.isAChildOf(juce::File(f))) return;
    for (int i = folders_.size(); --i >= 0;)
        if (juce::File(folders_[i]).isAChildOf(folder)) folders_.remove(i);
    folders_.add(folder.getFullPathName());
    update_status();
}

void RelinkPanel::clear_folders()
{
    folders_.clear();
    update_status();
}

void RelinkPanel::search()
{
    if (worker_ || missing_.empty() || folders_.isEmpty()) return;
    std::vector<std::string> folders;
    for (const auto& f : folders_) folders.push_back(f.toStdString());
    auto missing = missing_;
    scanned_ = 0;
    cancel_requested_ = false;
    done_ = false;
    search_.setEnabled(false);
    relink_.setEnabled(false);
    cancel_.setEnabled(true);
    worker_ = std::make_unique<std::thread>([this, missing = std::move(missing), folders = std::move(folders)] {
        auto r = session::relink::search(missing, folders, [this](int n) {
            scanned_ = n;
            return !cancel_requested_.load();
        });
        result_ = std::move(r);
        done_ = true;
    });
    update_status();
}

void RelinkPanel::finished()
{
    worker_->join();
    worker_.reset();
    search_.setEnabled(true);
    cancel_.setEnabled(false);
    relink_.setEnabled(true);
    matches_ = std::move(result_.matches);
    // Keep what the user already picked; else the clear best.
    for (std::size_t i = 0; i < matches_.size() && i < chosen_.size(); ++i)
        if (chosen_[i].isEmpty() && !matches_[i].chosen.empty())
            chosen_[i] = juce::File(juce::String(matches_[i].chosen)).getFullPathName();  // native separators
    int ambiguous = 0, none = 0;
    for (std::size_t i = 0; i < matches_.size(); ++i) {
        if (matches_[i].candidates.empty()) ++none;
        else if (chosen_[i].isEmpty()) ++ambiguous;
    }
    message_ = (result_.cancelled ? "Stopped after " : "Searched ") + juce::String(result_.files_scanned) + " file(s): " +
               juce::String(chosen_count()) + " of " + juce::String(static_cast<int>(missing_.size())) + " found";
    if (ambiguous > 0) message_ << ", " << ambiguous << " to pick (marked)";
    if (none > 0) message_ << ", " << none << " not found";
    rebuild_rows();
}

juce::String RelinkPanel::chosen(int index) const
{
    return index >= 0 && index < static_cast<int>(chosen_.size()) ? chosen_[static_cast<std::size_t>(index)] : juce::String();
}

void RelinkPanel::choose(int index, const juce::String& file)
{
    if (index < 0 || index >= static_cast<int>(chosen_.size())) return;
    chosen_[static_cast<std::size_t>(index)] = file;
    if (index < static_cast<int>(rows_.size())) rows_[static_cast<std::size_t>(index)]->fill();
    update_status();
}

int RelinkPanel::chosen_count() const
{
    return static_cast<int>(std::count_if(chosen_.begin(), chosen_.end(), [](const juce::String& s) { return s.isNotEmpty(); }));
}

int RelinkPanel::relink()
{
    std::map<std::string, std::string> relinks;
    for (std::size_t i = 0; i < missing_.size() && i < chosen_.size(); ++i)
        if (chosen_[i].isNotEmpty()) relinks[missing_[i].stored] = chosen_[i].toStdString();
    if (relinks.empty()) {
        message_ = "Nothing chosen to relink";
        update_status();
        return 0;
    }
    const int files = static_cast<int>(relinks.size());
    const int changed = context_.session.relink_media(relinks);
    refresh();
    message_ = changed > 0 ? "Relinked " + juce::String(files) + " file(s) (" + juce::String(changed) +
                                 " clip(s) and cut(s)); Edit > Undo puts them back"
                           : juce::String("Nothing was relinked");
    if (!missing_.empty()) message_ << " - " << static_cast<int>(missing_.size()) << " still missing";
    update_status();
    if (context_.status) context_.status(message_);
    return changed;
}

void RelinkPanel::rebuild_rows()
{
    rows_.clear();
    for (int i = 0; i < static_cast<int>(missing_.size()); ++i) {
        rows_.push_back(std::make_unique<Row>(*this, i));
        list_.addAndMakeVisible(*rows_.back());
    }
    title_.setText(missing_.empty() ? juce::String("Every media file is where the project expects it")
                                    : juce::String(static_cast<int>(missing_.size())) + " media file(s) missing",
                   juce::dontSendNotification);
    update_status();
    resized();
}

void RelinkPanel::update_status()
{
    folders_label_.setText(folders_.isEmpty() ? juce::String("Search in: (add a folder)")
                                              : "Search in: " + folders_.joinIntoString(";  "),
                           juce::dontSendNotification);
    folders_label_.setTooltip(folders_.joinIntoString("\n"));
    juce::String s;
    if (worker_) s = "Searching... " + juce::String(scanned_.load()) + " file(s)";
    else if (message_.isNotEmpty()) s = message_;
    else if (missing_.empty()) s = "Nothing to relink";
    else s = "Search the folders, check the matches, then Relink";
    status_.setText(s, juce::dontSendNotification);
    search_.setEnabled(!worker_ && !missing_.empty() && !folders_.isEmpty());
    relink_.setEnabled(!worker_ && chosen_count() > 0);
    relink_.setButtonText(chosen_count() > 0 ? "Relink " + juce::String(chosen_count()) : juce::String("Relink"));
}

void RelinkPanel::timerCallback()
{
    if (worker_ && done_) finished();
    else if (worker_) update_status();
}

void RelinkPanel::paint(juce::Graphics& g) { g.fillAll(hw::tok::panel_bottom); }

void RelinkPanel::resized()
{
    auto b = getLocalBounds().reduced(14);
    title_.setBounds(b.removeFromTop(26));
    auto row = b.removeFromTop(26);
    clear_folders_.setBounds(row.removeFromRight(64));
    row.removeFromRight(6);
    add_folder_.setBounds(row.removeFromRight(110));
    row.removeFromRight(8);
    folders_label_.setBounds(row);
    b.removeFromTop(8);
    auto bottom = b.removeFromBottom(30);
    relink_.setBounds(bottom.removeFromRight(110));
    bottom.removeFromRight(8);
    cancel_.setBounds(bottom.removeFromRight(80));
    bottom.removeFromRight(8);
    search_.setBounds(bottom.removeFromRight(90));
    bottom.removeFromRight(12);
    status_.setBounds(bottom);
    b.removeFromBottom(8);
    view_.setBounds(b);
    const int w = std::max(100, b.getWidth() - view_.getScrollBarThickness());
    int y = 0;
    for (auto& r : rows_) {
        r->setBounds(0, y, w, 72);
        y += 72;
    }
    list_.setSize(w, std::max(y, 10));
}

void show_relink(AppContext& context)
{
    dialogs::show_tool(relink_window(), "Relink Missing Media", 760, 520, [&context] { return new RelinkPanel(context); });
    if (auto* p = relink_slot()) p->refresh();
}

RelinkPanel* relink_panel() { return relink_slot(); }

}  // namespace vcc::desktop::review
