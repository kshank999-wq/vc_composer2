#include "collab/SharedWindow.h"

#include "licensing/Account.h"
#include "licensing/AccountWindow.h"
#include "ui/Dialogs.h"
#include "ui/Theme.h"

namespace vcc::desktop {

namespace shared = session::shared;
using collab::Controller;
using collab::Outcome;

namespace {

juce::Component::SafePointer<dialogs::ToolWindow>& shared_window()
{
    static juce::Component::SafePointer<dialogs::ToolWindow> w;
    return w;
}
juce::Component::SafePointer<dialogs::ToolWindow>& conflict_window()
{
    static juce::Component::SafePointer<dialogs::ToolWindow> w;
    return w;
}
SharedProjectsPanel*& panel_slot()
{
    static SharedProjectsPanel* p = nullptr;
    return p;
}
ConflictPanel*& conflict_slot()
{
    static ConflictPanel* p = nullptr;
    return p;
}

void style(juce::Label& l, bool title = false)
{
    l.setColour(juce::Label::textColourId, title ? colours::text : colours::text_dim);
    l.setJustificationType(juce::Justification::topLeft);
    if (title) l.setFont(juce::FontOptions(15.0f, juce::Font::bold));
}

juce::String track_name(const AppContext& c, const juce::String& id)
{
    const auto* t = c.session.data().find_track(id.toStdString());
    return t ? juce::String(t->name) : id;
}

}  // namespace

// --- Shared Projects ---------------------------------------------------------------------------

SharedProjectsPanel::SharedProjectsPanel(AppContext& context, std::function<bool(const juce::File&)> open)
    : context_(context), open_(std::move(open))
{
    setComponentID("shared_projects_panel");
    panel_slot() = this;
    title_.setText("Shared projects", juce::dontSendNotification);
    list_title_.setText("Shared with you", juce::dontSendNotification);
    project_title_.setText("This project", juce::dontSendNotification);
    style(title_, true);
    style(list_title_, true);
    style(project_title_, true);
    for (auto* l : {&status_, &project_, &people_}) {
        style(*l);
        l->setMinimumHorizontalScale(1.0f);
    }
    project_.setColour(juce::Label::textColourId, colours::text);
    for (auto* l : {&title_, &status_, &list_title_, &project_title_, &project_, &people_}) addAndMakeVisible(*l);
    list_.setTitle("Shared projects");
    list_.setRowHeight(40);
    list_.setColour(juce::ListBox::backgroundColourId, colours::panel);
    addAndMakeVisible(list_);

    open_button_.onClick = [this] {
        const int row = list_.getSelectedRow();
        if (row < 0) return report({false, "Choose a project in the list first"});
        auto chooser = std::make_shared<juce::FileChooser>("Where to keep \"" + projects_[static_cast<std::size_t>(row)].name + "\"",
                                                           juce::File::getSpecialLocation(juce::File::userDocumentsDirectory));
        chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                             [this, row, chooser, alive = alive_](const juce::FileChooser& fc) {
                                 if (*alive && fc.getResult() != juce::File()) open_row(row, fc.getResult());
                             });
    };
    reload_.onClick = [this] { refresh(); };
    share_.onClick = [this] {
        auto name = juce::String(context_.session.title()).upToLastOccurrenceOf(" *", false, false);
        auto* w = new juce::AlertWindow("Share This Project",
                                        "People you invite on vc-composer.com open it from File > Shared Projects. "
                                        "Only the project is shared; audio and picture files stay on this computer.",
                                        juce::MessageBoxIconType::NoIcon);
        w->addTextEditor("name", name, "Shared project name");
        w->addButton("Share", 1, juce::KeyPress(juce::KeyPress::returnKey));
        w->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
        w->enterModalState(true, juce::ModalCallbackFunction::create([this, w, alive = alive_](int r) {
                               if (*alive && r == 1) share(w->getTextEditorContents("name").trim());
                           }),
                           true);
    };
    sync_.onClick = [this] { sync(); };
    lock_.onClick = [this] { lock_selected(true); };
    unlock_.onClick = [this] { lock_selected(false); };
    team_.onClick = [] {
        const auto site = Account::get().config().site_url;
        juce::URL((site.isEmpty() ? juce::String("https://vc-composer.com") : site) + "/account/collaboration").launchInDefaultBrowser();
    };
    for (auto* b : {&open_button_, &reload_, &share_, &sync_, &lock_, &unlock_, &team_}) addAndMakeVisible(*b);
    open_button_.setTooltip("Downloads the latest revision into a folder you choose and opens it");
    sync_.setTooltip("Brings in what the others pushed (asking only about real conflicts), then pushes your changes");
    lock_.setTooltip("Only you can change the selected part until you unlock it (two hours at most)");

    Controller::get().on_change = [this] { update(); };
    update();
    setSize(760, 540);
    refresh();
}

SharedProjectsPanel::~SharedProjectsPanel()
{
    *alive_ = false;
    Controller::get().on_change = nullptr;
    if (panel_slot() == this) panel_slot() = nullptr;
}

void SharedProjectsPanel::report(const Outcome& o)
{
    status_.setText(o.message, juce::dontSendNotification);
    status_.setColour(juce::Label::textColourId, o.ok ? colours::text_dim : colours::accent_warm);
    if (o.message.isNotEmpty()) context_.status(o.message);
}

void SharedProjectsPanel::update()
{
    auto& c = Controller::get();
    const auto link = c.link();
    const auto& d = c.detail();
    project_.setText(c.summary(), juce::dontSendNotification);
    juce::String people;
    if (link && !d.id.isEmpty()) {
        people << "People: ";
        for (std::size_t i = 0; i < d.members.size(); ++i) {
            const auto& m = d.members[i];
            people << (i ? ", " : "") << m.name << " (" << juce::String(shared::role_label(m.role.toStdString()))
                   << (m.accepted ? "" : ", invited") << ")";
        }
        people << "\n\nLocked parts: ";
        if (d.locks.empty()) people << "none";
        for (std::size_t i = 0; i < d.locks.size(); ++i) {
            const auto& l = d.locks[i];
            people << (i ? "; " : "") << track_name(context_, l.track_id) << " - "
                   << (l.user_id == d.me ? juce::String("you") : l.holder) << " until " << l.until;
        }
        people << "\n\nRecent revisions:";
        for (std::size_t i = 0; i < d.revisions.size() && i < 6; ++i) {
            const auto& r = d.revisions[i];
            people << "\n  " << r.number << "  " << r.author << "  " << r.when
                   << (r.message.isNotEmpty() ? "  - " + r.message : juce::String());
        }
    }
    people_.setText(people, juce::dontSendNotification);
    const bool signed_in = Account::get().signed_in();
    const bool writes = link && shared::can_push(link->role);
    share_.setEnabled(signed_in && !link && !context_.session.path().empty() && !c.busy());
    sync_.setEnabled(signed_in && link.has_value() && !c.busy());
    lock_.setEnabled(signed_in && writes && !context_.session.selected_track().empty());
    unlock_.setEnabled(lock_.isEnabled());
    open_button_.setEnabled(signed_in && list_.getSelectedRow() >= 0 && !c.busy());
    reload_.setEnabled(signed_in);
    if (!signed_in) report({false, "Sign in to use shared projects (Help > Account and Licence...)"});
    repaint();
}

void SharedProjectsPanel::refresh(std::function<void()> done)
{
    update();
    if (!Account::get().signed_in()) {
        if (done) done();
        return;
    }
    if (Controller::get().link()) Controller::get().refresh();
    Controller::get().list([this, alive = alive_, done = std::move(done)](std::vector<collab::ProjectSummary> list, Outcome o) {
        if (*alive) {
            projects_ = std::move(list);
            list_.updateContent();
            list_.repaint();
            report(o);
            update();
        }
        if (done) done();
    });
}

void SharedProjectsPanel::share(const juce::String& name, std::function<void(Outcome)> done)
{
    status_.setText("Sharing...", juce::dontSendNotification);
    Controller::get().share(name.isEmpty() ? juce::String("Untitled") : name, [this, alive = alive_, done = std::move(done)](Outcome o) {
        if (*alive) {
            report(o);
            refresh();
        }
        if (done) done(o);
    });
    update();
}

void SharedProjectsPanel::sync(std::function<void(Outcome)> done)
{
    // A sync saves the project with its new base.
    if (!may_save(context_)) {
        if (done) done({false, "Saving is off, so syncing is too"});
        return;
    }
    status_.setText("Syncing...", juce::dontSendNotification);
    Controller::get().sync([this, alive = alive_, done = std::move(done)](Outcome o) {
        if (*alive) report(o);
        if (done) done(o);
    });
    update();
}

void SharedProjectsPanel::lock_selected(bool lock, std::function<void(Outcome)> done)
{
    Controller::get().set_lock(context_.session.selected_track(), lock, [this, alive = alive_, done = std::move(done)](Outcome o) {
        if (*alive) report(o);
        if (done) done(o);
    });
}

void SharedProjectsPanel::open_row(int row, const juce::File& folder, std::function<void(Outcome)> done)
{
    if (row < 0 || row >= static_cast<int>(projects_.size())) return;
    status_.setText("Downloading...", juce::dontSendNotification);
    Controller::get().open_shared(projects_[static_cast<std::size_t>(row)], folder, open_,
                                  [this, alive = alive_, done = std::move(done)](Outcome o) {
                                      if (*alive) report(o);
                                      if (done) done(o);
                                  });
}

int SharedProjectsPanel::getNumRows() { return static_cast<int>(projects_.size()); }

void SharedProjectsPanel::paintListBoxItem(int row, juce::Graphics& g, int width, int height, bool selected)
{
    if (row < 0 || row >= getNumRows()) return;
    const auto& p = projects_[static_cast<std::size_t>(row)];
    if (selected) g.fillAll(colours::selection);
    g.setColour(colours::text);
    g.setFont(juce::FontOptions(14.0f, juce::Font::bold));
    g.drawText(p.name, 10, 3, width - 20, 18, juce::Justification::centredLeft);
    g.setColour(colours::text_dim);
    g.setFont(juce::FontOptions(12.0f));
    g.drawText(juce::String(shared::role_label(p.role.toStdString())) + "  ·  " + p.owner + "  ·  " +
                   (p.head > 0 ? "revision " + juce::String(p.head) : juce::String("nothing pushed yet")),
               10, 21, width - 20, 16, juce::Justification::centredLeft);
    g.setColour(colours::outline);
    g.drawHorizontalLine(height - 1, 0.0f, static_cast<float>(width));
    juce::ignoreUnused(open_button_);
}

void SharedProjectsPanel::listBoxItemDoubleClicked(int, const juce::MouseEvent&) { open_button_.triggerClick(); }

void SharedProjectsPanel::paint(juce::Graphics& g) { g.fillAll(colours::background); }

void SharedProjectsPanel::resized()
{
    auto r = getLocalBounds().reduced(16);
    title_.setBounds(r.removeFromTop(24));
    status_.setBounds(r.removeFromBottom(22));
    r.removeFromBottom(6);
    auto left = r.removeFromLeft(300);
    r.removeFromLeft(16);
    list_title_.setBounds(left.removeFromTop(22));
    auto lb = left.removeFromBottom(30);
    open_button_.setBounds(lb.removeFromLeft(190).reduced(0, 2));
    lb.removeFromLeft(8);
    reload_.setBounds(lb.reduced(0, 2));
    left.removeFromBottom(6);
    list_.setBounds(left);

    project_title_.setBounds(r.removeFromTop(22));
    project_.setBounds(r.removeFromTop(54));
    auto b1 = r.removeFromTop(30);
    share_.setBounds(b1.removeFromLeft(170).reduced(0, 2));
    b1.removeFromLeft(8);
    sync_.setBounds(b1.removeFromLeft(110).reduced(0, 2));
    r.removeFromTop(4);
    auto b2 = r.removeFromTop(30);
    lock_.setBounds(b2.removeFromLeft(170).reduced(0, 2));
    b2.removeFromLeft(8);
    unlock_.setBounds(b2.removeFromLeft(110).reduced(0, 2));
    team_.setBounds(r.removeFromBottom(30).removeFromLeft(200).reduced(0, 2));
    r.removeFromBottom(6);
    people_.setBounds(r.reduced(0, 6));
}

// --- Conflicts --------------------------------------------------------------------------------------

ConflictPanel::ConflictPanel(const project::MergeResult& result, std::function<void(std::optional<std::vector<bool>>)> answer)
    : answer_(std::move(answer))
{
    setComponentID("conflict_panel");
    conflict_slot() = this;
    const int n = static_cast<int>(result.conflicts.size());
    juce::String combined;
    if (result.from_theirs + result.from_ours > 0)
        combined = " (" + juce::String(result.from_theirs) + " of their changes, " + juce::String(result.from_ours) + " of yours)";
    intro_.setText(juce::String(n) + (n == 1 ? " thing was" : " things were") +
                       " changed both here and by someone else since you last synced. Choose which to keep; "
                       "everything else combined by itself" + combined + ".",
                   juce::dontSendNotification);
    style(intro_);
    intro_.setMinimumHorizontalScale(1.0f);  // wrap rather than squeeze
    intro_.setColour(juce::Label::textColourId, colours::text);
    addAndMakeVisible(intro_);
    for (const auto& c : result.conflicts) {
        Row row;
        row.what = std::make_unique<juce::Label>();
        row.what->setText(juce::String(c.description), juce::dontSendNotification);
        style(*row.what);
        row.what->setColour(juce::Label::textColourId, colours::text);
        row.mine = std::make_unique<juce::ToggleButton>("Mine: " + collab::describe_value(c.ours));
        row.theirs = std::make_unique<juce::ToggleButton>("Theirs: " + collab::describe_value(c.theirs));
        const int group = 9100 + static_cast<int>(rows_.size());
        row.mine->setRadioGroupId(group);
        row.theirs->setRadioGroupId(group);
        row.mine->setToggleState(true, juce::dontSendNotification);
        for (auto* b : {row.mine.get(), row.theirs.get()}) {
            b->setColour(juce::ToggleButton::textColourId, colours::text);
            list_.addAndMakeVisible(*b);
        }
        list_.addAndMakeVisible(*row.what);
        rows_.push_back(std::move(row));
    }
    viewport_.setViewedComponent(&list_, false);
    viewport_.setScrollBarsShown(true, false);
    addAndMakeVisible(viewport_);
    all_mine_.onClick = [this] {
        for (int i = 0; i < conflict_count(); ++i) choose(i, false);
    };
    all_theirs_.onClick = [this] {
        for (int i = 0; i < conflict_count(); ++i) choose(i, true);
    };
    finish_.onClick = [this] { finish(); };
    cancel_.onClick = [this] { cancel(); };
    for (auto* b : {&all_mine_, &all_theirs_, &finish_, &cancel_}) addAndMakeVisible(*b);
    setSize(640, 420);
}

juce::String ConflictPanel::row_text(int i) const
{
    const auto& r = rows_[static_cast<std::size_t>(i)];
    return r.what->getText() + " | " + r.mine->getButtonText() + " | " + r.theirs->getButtonText();
}

void ConflictPanel::choose(int i, bool theirs)
{
    auto& r = rows_[static_cast<std::size_t>(i)];
    (theirs ? r.theirs : r.mine)->setToggleState(true, juce::sendNotification);
}

void ConflictPanel::finish()
{
    if (answered_) return;
    answered_ = true;
    std::vector<bool> take;
    for (const auto& r : rows_) take.push_back(r.theirs->getToggleState());
    auto answer = std::move(answer_);
    if (conflict_slot() == this) conflict_slot() = nullptr;
    if (auto* w = conflict_window().getComponent()) w->setVisible(false);
    answer(std::move(take));
}

void ConflictPanel::cancel()
{
    if (answered_) return;
    answered_ = true;
    auto answer = std::move(answer_);
    if (conflict_slot() == this) conflict_slot() = nullptr;
    if (auto* w = conflict_window().getComponent()) w->setVisible(false);
    answer(std::nullopt);
}

void ConflictPanel::paint(juce::Graphics& g) { g.fillAll(colours::background); }

void ConflictPanel::resized()
{
    auto r = getLocalBounds().reduced(16);
    intro_.setBounds(r.removeFromTop(54));
    auto buttons = r.removeFromBottom(32);
    all_mine_.setBounds(buttons.removeFromLeft(130).reduced(0, 2));
    buttons.removeFromLeft(8);
    all_theirs_.setBounds(buttons.removeFromLeft(130).reduced(0, 2));
    cancel_.setBounds(buttons.removeFromRight(110).reduced(0, 2));
    buttons.removeFromRight(8);
    finish_.setBounds(buttons.removeFromRight(110).reduced(0, 2));
    r.removeFromBottom(8);
    viewport_.setBounds(r);
    const int w = r.getWidth() - viewport_.getScrollBarThickness();
    int y = 0;
    for (auto& row : rows_) {
        row.what->setBounds(0, y, w, 22);
        row.mine->setBounds(8, y + 22, w / 2 - 12, 24);
        row.theirs->setBounds(w / 2, y + 22, w / 2 - 4, 24);
        y += 56;
    }
    list_.setSize(w, std::max(y, 1));
}

// --- Windows ------------------------------------------------------------------------------------

void show_shared_projects(AppContext& context, std::function<bool(const juce::File&)> open)
{
    dialogs::show_tool(shared_window(), "Shared Projects", 760, 540,
                       [&context, open] { return new SharedProjectsPanel(context, open); });
    if (auto* p = panel_slot()) p->refresh();
}

SharedProjectsPanel* shared_projects_panel() { return panel_slot(); }

void ask_conflicts(const project::MergeResult& result, std::function<void(std::optional<std::vector<bool>>)> answer)
{
    // A fresh dialog per sync (the window holds the last one's panel otherwise).
    if (auto* p = conflict_slot()) p->cancel();
    if (auto* w = conflict_window().getComponent()) delete w;
    dialogs::show_tool(conflict_window(), "Sync: Choose What to Keep", 640, 420,
                       [&result, answer] { return new ConflictPanel(result, answer); },
                       [] {
                           if (auto* p = conflict_slot()) p->cancel();
                       });
}

ConflictPanel* conflict_panel() { return conflict_slot(); }

}  // namespace vcc::desktop
