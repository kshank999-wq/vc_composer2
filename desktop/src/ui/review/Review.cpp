#include "ui/review/Review.h"

#include <set>

#include "ui/Dialogs.h"
#include "ui/hw/HwTheme.h"

namespace vcc::desktop::review {

namespace {

const char* const kSlotNames[] = {"A", "B", "C"};

juce::Component::SafePointer<dialogs::ToolWindow>& review_window()
{
    static juce::Component::SafePointer<dialogs::ToolWindow> w;
    return w;
}
ReviewPanel*& review_slot()
{
    static ReviewPanel* p = nullptr;
    return p;
}
juce::Component::SafePointer<dialogs::ToolWindow>& package_window()
{
    static juce::Component::SafePointer<dialogs::ToolWindow> w;
    return w;
}
PackagePanel*& package_slot()
{
    static PackagePanel* p = nullptr;
    return p;
}

void style_label(juce::Label& l, float size, bool bright)
{
    l.setFont(hw::fonts::ui(size, bright));
    l.setColour(juce::Label::textColourId, bright ? hw::tok::text_bright : hw::tok::text);
}

}  // namespace

// --- Director review -----------------------------------------------------------------------

ReviewPanel::ReviewPanel(AppContext& context) : context_(context)
{
    setComponentID("director_review");
    setWantsKeyboardFocus(true);
    review_slot() = this;
    original_ = context_.session.active_versions();
    track_ = context_.session.selected_track();
    style_label(title_, 15.0f, true);
    style_label(diff_, 13.0f, true);
    style_label(help_, 11.0f, false);
    help_.setText("A, B, C (or 1, 2, 3) switch while it plays: listening, not editing - nothing goes into undo. "
                  "Keep makes what is playing the project's choice (one undo step); Back, or closing, returns to "
                  "where the review began.",
                  juce::dontSendNotification);
    for (auto* c : std::initializer_list<juce::Component*>{&title_, &diff_, &help_, &snapshots_, &track_mode_, &approve_,
                                                          &keep_, &back_})
        addAndMakeVisible(*c);
    snapshots_.set_led_dot(true);
    track_mode_.set_led_dot(true);
    snapshots_.onClick = [this] { set_mode(Mode::Snapshots); };
    track_mode_.onClick = [this] { set_mode(Mode::Track); };
    for (int i = 0; i < 3; ++i) {
        keys_[static_cast<std::size_t>(i)] = std::make_unique<hw::Keycap>(kSlotNames[i], true);
        auto& key = *keys_[static_cast<std::size_t>(i)];
        key.set_led_dot(true);
        key.setComponentID(juce::String("review_slot_") + kSlotNames[i]);
        key.onClick = [this, i] { play_slot(i); };
        addAndMakeVisible(key);
        auto& combo = choosers_[static_cast<std::size_t>(i)];
        combo.setTextWhenNothingSelected("(empty)");
        combo.onChange = [this, i] {
            auto& c = choosers_[static_cast<std::size_t>(i)];
            const int idx = c.getSelectedItemIndex();
            if (idx < 0) return;
            const auto& d = context_.session.data();
            std::string id;
            if (mode_ == Mode::Snapshots && idx < static_cast<int>(d.snapshots.size()))
                id = d.snapshots[static_cast<std::size_t>(idx)].id;
            if (mode_ == Mode::Track)
                if (const auto* t = d.find_track(track_); t && idx < static_cast<int>(t->versions.size()))
                    id = t->versions[static_cast<std::size_t>(idx)].id;
            slots_[static_cast<std::size_t>(i)] = id;
            if (current_ == i) play_slot(i);
            repaint();
        };
        addAndMakeVisible(combo);
        auto& cap = captures_[static_cast<std::size_t>(i)];
        cap.setButtonText("Capture");
        cap.setTooltip("Records what plays now as a snapshot in this slot");
        cap.onClick = [this, i] { capture(i); };
        addAndMakeVisible(cap);
    }
    approve_.onClick = [this] { approve_current(approve_.getToggleState()); };
    keep_.onClick = [this] { keep(); };
    back_.onClick = [this] { back(); };
    // Start where it is most useful: two snapshots, or the track's versions.
    const auto& d = context_.session.data();
    const auto* t = d.find_track(track_);
    if (d.snapshots.size() < 2 && t && t->versions.size() >= 2) mode_ = Mode::Track;
    if (mode_ == Mode::Snapshots)
        for (std::size_t i = 0; i < 3 && i < d.snapshots.size(); ++i) slots_[i] = d.snapshots[i].id;
    else
        for (std::size_t i = 0; i < 3 && t && i < t->versions.size(); ++i) slots_[i] = t->versions[i].id;
    reload();
    startTimerHz(10);
}

ReviewPanel::~ReviewPanel()
{
    stopTimer();
    if (review_slot() == this) review_slot() = nullptr;
}

void ReviewPanel::set_mode(Mode m)
{
    if (m == mode_) return;
    mode_ = m;
    slots_ = {};
    current_ = -1;
    track_ = context_.session.selected_track();
    const auto& d = context_.session.data();
    if (mode_ == Mode::Track) {
        if (const auto* t = d.find_track(track_))
            for (std::size_t i = 0; i < 3 && i < t->versions.size(); ++i) slots_[i] = t->versions[i].id;
    } else {
        for (std::size_t i = 0; i < 3 && i < d.snapshots.size(); ++i) slots_[i] = d.snapshots[i].id;
    }
    signature_.clear();
    reload();
}

void ReviewPanel::assign(int slot, const std::string& id)
{
    if (slot < 0 || slot > 2) return;
    slots_[static_cast<std::size_t>(slot)] = id;
    signature_.clear();
    reload();
    if (current_ == slot) play_slot(slot);
}

std::map<std::string, std::string> ReviewPanel::versions_of(int slot) const
{
    const auto& id = slots_[static_cast<std::size_t>(slot)];
    if (id.empty()) return {};
    if (mode_ == Mode::Track) return {{track_, id}};
    for (const auto& s : context_.session.data().snapshots)
        if (s.id == id) return s.track_versions;
    return {};
}

void ReviewPanel::play_slot(int slot)
{
    if (slot < 0 || slot > 2 || slots_[static_cast<std::size_t>(slot)].empty()) {
        if (slot >= 0 && slot <= 2) keys_[static_cast<std::size_t>(slot)]->setToggleState(false, juce::dontSendNotification);
        return;
    }
    // A new audition (first, or after Keep or Back): it begins from what
    // is active now - an undo in between included.
    if (ended_ || current_ < 0) original_ = context_.session.active_versions();
    // Snapshots: from where the review began, so a track not in the
    // snapshot plays as it was, not as the last slot left it.
    auto versions = mode_ == Mode::Snapshots ? original_ : context_.session.active_versions();
    for (const auto& [track, version] : versions_of(slot)) versions[track] = version;
    context_.session.audition_versions(versions);
    current_ = slot;
    ended_ = false;
    reload();
    if (context_.status) context_.status("Review: playing " + slot_text(slot));
}

void ReviewPanel::capture(int slot)
{
    if (mode_ != Mode::Snapshots) set_mode(Mode::Snapshots);
    const std::string id = context_.session.create_snapshot("Review " + std::string(kSlotNames[slot]));
    slots_[static_cast<std::size_t>(slot)] = id;
    current_ = slot;
    signature_.clear();
    reload();
}

bool ReviewPanel::slot_approved(int slot) const
{
    const auto& id = slots_[static_cast<std::size_t>(slot)];
    const auto& d = context_.session.data();
    if (mode_ == Mode::Snapshots) {
        for (const auto& s : d.snapshots)
            if (s.id == id) return s.approved;
        return false;
    }
    if (const auto* t = d.find_track(track_))
        for (const auto& v : t->versions)
            if (v.id == id) return v.approved;
    return false;
}

void ReviewPanel::approve_current(bool approved)
{
    if (current_ < 0) return;
    const auto& id = slots_[static_cast<std::size_t>(current_)];
    if (mode_ == Mode::Snapshots) {
        context_.session.set_snapshot_approved(id, approved);
        // A snapshot's approval pins its versions too.
        for (const auto& [track, version] : versions_of(current_)) context_.session.set_version_approved(track, version, approved);
    } else {
        context_.session.set_version_approved(track_, id, approved);
    }
    signature_.clear();
    reload();
}

juce::String ReviewPanel::slot_text(int slot) const
{
    const auto& id = slots_[static_cast<std::size_t>(slot)];
    const auto& d = context_.session.data();
    juce::String name = "(empty)";
    if (mode_ == Mode::Snapshots) {
        for (const auto& s : d.snapshots)
            if (s.id == id) name = s.name;
    } else if (const auto* t = d.find_track(track_)) {
        for (const auto& v : t->versions)
            if (v.id == id) name = juce::String(t->name) + " " + v.name;
    }
    return juce::String(kSlotNames[slot]) + " (" + name + ")";
}

juce::String ReviewPanel::difference() const
{
    // The slot playing against the other filled slots.
    const int a = current_ >= 0 ? current_ : 0;
    juce::StringArray parts;
    for (int b = 0; b < 3; ++b) {
        if (b == a || slots_[static_cast<std::size_t>(a)].empty() || slots_[static_cast<std::size_t>(b)].empty()) continue;
        const auto va = versions_of(a), vb = versions_of(b);
        juce::StringArray tracks;
        std::set<std::string> ids;
        for (const auto& [t, v] : va) ids.insert(t);
        for (const auto& [t, v] : vb) ids.insert(t);
        for (const auto& t : ids) {
            const auto ia = va.find(t), ib = vb.find(t);
            if (ia == va.end() || ib == vb.end() || ia->second == ib->second) continue;
            const auto diff = context_.session.compare_versions(t, ia->second, ib->second);
            const auto* track = context_.session.data().find_track(t);
            if (mode_ == Mode::Track) tracks.add(diff.describe());
            else if (track) tracks.add(juce::String(track->name) + ": " + diff.describe());
        }
        parts.add(juce::String(kSlotNames[b]) + " vs " + kSlotNames[a] + ": " +
                  (tracks.isEmpty() ? juce::String("same music") : tracks.joinIntoString("; ")));
    }
    return parts.joinIntoString("   |   ");
}

void ReviewPanel::keep()
{
    if (current_ < 0) return;
    const auto chosen = context_.session.active_versions();
    context_.session.choose_versions("Choose " + slot_text(current_).toStdString(), original_, chosen);
    original_ = context_.session.active_versions();
    ended_ = true;
    if (context_.status) context_.status("Kept " + slot_text(current_) + " (undo goes back)");
}

void ReviewPanel::back()
{
    context_.session.audition_versions(original_);
    current_ = -1;
    ended_ = true;
    reload();
}

void ReviewPanel::reload()
{
    const auto& d = context_.session.data();
    snapshots_.setToggleState(mode_ == Mode::Snapshots, juce::dontSendNotification);
    track_mode_.setToggleState(mode_ == Mode::Track, juce::dontSendNotification);
    const auto* t = d.find_track(track_);
    for (int i = 0; i < 3; ++i) {
        auto& combo = choosers_[static_cast<std::size_t>(i)];
        combo.clear(juce::dontSendNotification);
        int selected = -1, n = 0;
        if (mode_ == Mode::Snapshots) {
            for (const auto& s : d.snapshots) {
                combo.addItem(juce::String(s.name) + (s.approved ? "  [approved]" : ""), ++n);
                if (s.id == slots_[static_cast<std::size_t>(i)]) selected = n;
            }
        } else if (t) {
            for (const auto& v : t->versions) {
                combo.addItem(juce::String(v.name) + (v.approved ? "  [approved]" : ""), ++n);
                if (v.id == slots_[static_cast<std::size_t>(i)]) selected = n;
            }
        }
        combo.setSelectedId(selected > 0 ? selected : 0, juce::dontSendNotification);
        captures_[static_cast<std::size_t>(i)].setVisible(mode_ == Mode::Snapshots);
        keys_[static_cast<std::size_t>(i)]->setToggleState(current_ == i, juce::dontSendNotification);
        keys_[static_cast<std::size_t>(i)]->setEnabled(!slots_[static_cast<std::size_t>(i)].empty());
    }
    approve_.setEnabled(current_ >= 0);
    approve_.setToggleState(current_ >= 0 && slot_approved(current_), juce::dontSendNotification);
    title_.setText(mode_ == Mode::Snapshots ? juce::String("Whole project: snapshots")
                                            : "Track: " + juce::String(t ? t->name : std::string("(none selected)")),
                   juce::dontSendNotification);
    const auto diff = difference();
    diff_.setText(current_ >= 0 ? "Playing " + slot_text(current_) + (diff.isNotEmpty() ? "   -   " + diff : juce::String())
                                : diff.isNotEmpty() ? diff : juce::String("Choose what goes in A, B and C"),
                  juce::dontSendNotification);
    repaint();
}

void ReviewPanel::timerCallback()
{
    // Snapshots and versions added, renamed or approved elsewhere.
    const auto& d = context_.session.data();
    std::string sig = track_ + "|";
    for (const auto& s : d.snapshots) sig += s.id + s.name + (s.approved ? "*" : "") + ";";
    if (const auto* t = d.find_track(track_))
        for (const auto& v : t->versions) sig += v.id + v.name + (v.approved ? "*" : "") + ";";
    if (sig != signature_) {
        signature_ = sig;
        reload();
    }
}

bool ReviewPanel::keyPressed(const juce::KeyPress& k)
{
    if (k.getModifiers().isCommandDown() || k.getModifiers().isCtrlDown() || k.getModifiers().isAltDown()) return false;
    const auto text = juce::CharacterFunctions::toUpperCase(k.getTextCharacter());
    const auto code = juce::CharacterFunctions::toUpperCase(static_cast<juce::juce_wchar>(k.getKeyCode()));
    for (int i = 0; i < 3; ++i)
        if (text == static_cast<juce::juce_wchar>('A' + i) || text == static_cast<juce::juce_wchar>('1' + i) ||
            code == static_cast<juce::juce_wchar>('A' + i) || code == static_cast<juce::juce_wchar>('1' + i)) {
            play_slot(i);
            return true;
        }
    if (k == juce::KeyPress::spaceKey) {
        context_.engine.toggle_play();
        return true;
    }
    return false;
}

void ReviewPanel::paint(juce::Graphics& g)
{
    g.fillAll(hw::tok::panel_bottom);
    // Approved slots: a green bar over the key.
    for (int i = 0; i < 3; ++i) {
        if (slots_[static_cast<std::size_t>(i)].empty() || !slot_approved(i)) continue;
        const auto r = keys_[static_cast<std::size_t>(i)]->getBounds().toFloat();
        g.setColour(juce::Colour(0xff58c46a));
        g.fillRoundedRectangle(r.withY(r.getBottom() + 4).withHeight(4), 2.0f);
        g.setFont(hw::fonts::ui(10.0f, true));
        g.drawText("APPROVED", r.withY(r.getBottom() + 10).withHeight(14), juce::Justification::centred, false);
    }
}

void ReviewPanel::resized()
{
    auto b = getLocalBounds().reduced(14);
    auto top = b.removeFromTop(30);
    snapshots_.setBounds(top.removeFromLeft(120));
    top.removeFromLeft(8);
    track_mode_.setBounds(top.removeFromLeft(140));
    b.removeFromTop(10);
    title_.setBounds(b.removeFromTop(24));
    b.removeFromTop(8);
    auto keys = b.removeFromTop(86);
    const int w = keys.getWidth() / 3;
    for (int i = 0; i < 3; ++i) {
        auto col = keys.removeFromLeft(w).reduced(8, 0);
        keys_[static_cast<std::size_t>(i)]->setBounds(col.removeFromTop(56).withSizeKeepingCentre(std::min(120, col.getWidth()), 56));
    }
    b.removeFromTop(14);
    auto row = b.removeFromTop(28);
    for (int i = 0; i < 3; ++i) {
        auto col = row.removeFromLeft(w).reduced(8, 0);
        captures_[static_cast<std::size_t>(i)].setBounds(col.removeFromRight(70));
        col.removeFromRight(4);
        choosers_[static_cast<std::size_t>(i)].setBounds(col);
    }
    b.removeFromTop(12);
    diff_.setBounds(b.removeFromTop(40));
    b.removeFromTop(6);
    auto actions = b.removeFromTop(30);
    approve_.setBounds(actions.removeFromLeft(180));
    back_.setBounds(actions.removeFromRight(90));
    actions.removeFromRight(8);
    keep_.setBounds(actions.removeFromRight(90));
    b.removeFromTop(10);
    help_.setBounds(b.removeFromTop(40));
}

void show_review(AppContext& context)
{
    dialogs::show_tool(review_window(), "Director Review", 760, 400, [&context] { return new ReviewPanel(context); },
                       [] {
                           // Closing without Keep: back to where the review began.
                           if (auto* p = review_slot(); p && !p->ended()) p->back();
                       });
}

ReviewPanel* review_panel() { return review_slot(); }

// --- Package -----------------------------------------------------------------------------

PackagePanel::PackagePanel(AppContext& context) : context_(context)
{
    setComponentID("package_panel");
    package_slot() = this;
    style_label(title_, 15.0f, true);
    style_label(folder_, 12.0f, false);
    style_label(status_, 12.0f, true);
    report_.setMultiLine(true);
    report_.setReadOnly(true);
    report_.setFont(juce::FontOptions(juce::Font::getDefaultMonospacedFontName(), 12.0f, juce::Font::plain));
    picture_.setToggleState(true, juce::dontSendNotification);
    picture_.onClick = [this] { refresh_inventory(); };
    for (auto* c : std::initializer_list<juce::Component*>{&title_, &folder_, &status_, &report_, &choose_, &go_, &cancel_,
                                                          &picture_, &zip_, &progress_})
        addAndMakeVisible(*c);
    cancel_.setEnabled(false);
    // Beside the project by default: "<Name> Package".
    const auto& path = context_.session.path();
    const juce::File base = path.empty() ? juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
                                         : juce::File(juce::String(path)).getParentDirectory().getParentDirectory();
    destination_ = base.getChildFile(juce::String(session::package::safe_name(context_.session.data().name)) + " Package")
                       .getNonexistentSibling(false);
    choose_.onClick = [this] {
        chooser_ = std::make_unique<juce::FileChooser>("Package into", destination_.getParentDirectory());
        chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                              [safe = juce::Component::SafePointer<PackagePanel>(this)](const juce::FileChooser& fc) {
                                  if (!safe || fc.getResult() == juce::File()) return;
                                  safe->destination_ =
                                      fc.getResult()
                                          .getChildFile(juce::String(session::package::safe_name(
                                                            safe->context_.session.data().name)) +
                                                        " Package")
                                          .getNonexistentSibling(false);
                                  safe->refresh_inventory();
                              });
    };
    go_.onClick = [this] { start(); };
    cancel_.onClick = [this] { cancel_requested_ = true; };
    refresh_inventory();
    startTimerHz(15);
}

PackagePanel::~PackagePanel()
{
    stopTimer();
    if (worker_) {
        cancel_requested_ = true;
        worker_->join();
    }
    if (package_slot() == this) package_slot() = nullptr;
}

void PackagePanel::refresh_inventory()
{
    const auto& d = context_.session.data();
    inventory_ = session::package::inventory(d, context_.session.path());
    session::package::Options o;
    o.include_picture = picture_.getToggleState();
    title_.setText("Collect \"" + juce::String(d.name) + "\" into one folder", juce::dontSendNotification);
    folder_.setText("Into: " + destination_.getFullPathName(), juce::dontSendNotification);
    report_.setText(session::package::report_text(d, inventory_, o), juce::dontSendNotification);
    if (message_.isEmpty())
        status_.setText(context_.session.path().empty() ? juce::String("Unsaved project: save it first so media beside it are found")
                        : inventory_.missing() > 0 ? juce::String(inventory_.missing()) + " missing file(s): the package lists them"
                                                   : juce::String("Ready"),
                        juce::dontSendNotification);
}

void PackagePanel::start()
{
    if (worker_) return;
    // Plug-in states first, so the package carries the sound as it is now.
    if (context_.capture_plugin_states) context_.capture_plugin_states();
    auto project = std::make_shared<project::Project>(context_.session.project());
    const std::string path = context_.session.path();
    const std::string folder = destination_.getFullPathName().toStdString();
    session::package::Options o;
    o.include_picture = picture_.getToggleState();
    const bool zip = zip_.getToggleState();
    fraction_ = 0.0;
    cancel_requested_ = false;
    done_ = false;
    message_.clear();
    zip_file_ = juce::File();
    go_.setEnabled(false);
    cancel_.setEnabled(true);
    status_.setText("Packaging...", juce::dontSendNotification);
    worker_ = std::make_unique<std::thread>([this, project, path, folder, o, zip] {
        auto r = session::package::collect(*project, path, folder, o, [this](double f) {
            fraction_ = f * (0.9);
            return !cancel_requested_.load();
        });
        if (r.ok && zip) {
            const juce::File dir(juce::String(r.folder));
            const juce::File out = dir.getSiblingFile(dir.getFileName() + ".zip").getNonexistentSibling(false);
            juce::ZipFile::Builder builder;
            for (const auto& f : dir.findChildFiles(juce::File::findFiles, true))
                builder.addFile(f, 6, dir.getFileName() + "/" + f.getRelativePathFrom(dir).replaceCharacter('\\', '/'));
            juce::FileOutputStream os(out);
            if (os.openedOk() && builder.writeToStream(os, nullptr)) {
                os.flush();
                zip_file_ = out;
            } else {
                r.error = "the package was made, but the .zip could not be written";
            }
        }
        result_ = std::move(r);
        fraction_ = 1.0;
        done_ = true;
    });
}

void PackagePanel::finished()
{
    worker_->join();
    worker_.reset();
    go_.setEnabled(true);
    cancel_.setEnabled(false);
    if (result_.cancelled) message_ = "Cancelled: nothing was left behind";
    else if (!result_.ok) message_ = "Not packaged: " + juce::String(result_.error);
    else {
        message_ = "Packaged " + juce::String(result_.copied) + " file(s) into " + juce::String(result_.folder) +
                   (zip_file_.existsAsFile() ? " and " + zip_file_.getFileName() : juce::String()) +
                   (result_.inventory.missing() > 0 ? " - " + juce::String(result_.inventory.missing()) + " missing, listed in the report"
                                                    : juce::String());
        if (result_.error.size()) message_ << " (" << result_.error << ")";
        session::package::Options o;
        o.include_picture = picture_.getToggleState();
        report_.setText(session::package::report_text(context_.session.data(), result_.inventory, o), juce::dontSendNotification);
        destination_ = destination_.getNonexistentSibling(false);
    }
    status_.setText(message_, juce::dontSendNotification);
    folder_.setText("Into: " + destination_.getFullPathName(), juce::dontSendNotification);
    if (context_.status) context_.status(message_);
}

void PackagePanel::timerCallback()
{
    progress_value_ = fraction_.load();
    if (worker_ && done_) finished();
}

void PackagePanel::paint(juce::Graphics& g) { g.fillAll(hw::tok::panel_bottom); }

void PackagePanel::resized()
{
    auto b = getLocalBounds().reduced(14);
    title_.setBounds(b.removeFromTop(26));
    auto row = b.removeFromTop(26);
    choose_.setBounds(row.removeFromRight(90));
    folder_.setBounds(row);
    b.removeFromTop(6);
    auto opts = b.removeFromTop(26);
    picture_.setBounds(opts.removeFromLeft(200));
    zip_.setBounds(opts.removeFromLeft(200));
    b.removeFromTop(8);
    auto bottom = b.removeFromBottom(30);
    go_.setBounds(bottom.removeFromRight(110));
    bottom.removeFromRight(8);
    cancel_.setBounds(bottom.removeFromRight(90));
    bottom.removeFromRight(12);
    progress_.setBounds(bottom);
    b.removeFromBottom(8);
    status_.setBounds(b.removeFromBottom(40));
    b.removeFromBottom(6);
    report_.setBounds(b);
}

void show_package(AppContext& context)
{
    dialogs::show_tool(package_window(), "Collect and Save Package", 720, 620, [&context] { return new PackagePanel(context); });
}

PackagePanel* package_panel() { return package_slot(); }

juce::File unpack(const juce::File& zip, juce::String& error)
{
    juce::ZipFile archive(zip);
    if (archive.getNumEntries() == 0) {
        error = zip.getFileName() + " is not a package (.zip)";
        return {};
    }
    const juce::File into = zip.getParentDirectory().getChildFile(zip.getFileNameWithoutExtension() + " (unpacked)")
                                .getNonexistentSibling(false);
    if (!into.createDirectory()) {
        error = "could not create " + into.getFullPathName();
        return {};
    }
    const auto r = archive.uncompressTo(into, false);
    if (r.failed()) {
        error = r.getErrorMessage();
        return {};
    }
    const auto projects = into.findChildFiles(juce::File::findFiles, true, "*.vcc2");
    if (projects.isEmpty()) {
        error = zip.getFileName() + " holds no VC Composer 2 project";
        return {};
    }
    return projects.getFirst();
}

}  // namespace vcc::desktop::review
