#include "ui/review/Review.h"

#include <set>

#include "audio/MasterExport.h"
#include "ui/Dialogs.h"
#include "ui/hw/HwTheme.h"
#include "vcc/picture/Picture.h"

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
    comments_.setTooltip("Review comments pinned to moments (adds at the playhead)");
    comments_.onClick = [this] {
        if (context_.show_comment) context_.show_comment({});
    };
    mixes_.setTooltip("Render A, B and C each to its own WAV, for listening away from the app");
    mixes_.onClick = [this] {
        const juce::Component::SafePointer<ReviewPanel> safe(this);
        const auto& path = context_.session.path();
        const juce::File start = path.empty() ? juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
                                              : juce::File(juce::String(path)).getParentDirectory();
        chooser_ = std::make_unique<juce::FileChooser>("Export review mixes into", start);
        chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                              [safe](const juce::FileChooser& fc) {
                                  if (safe && fc.getResult() != juce::File()) safe->export_mixes(fc.getResult());
                              });
    };
    addAndMakeVisible(comments_);
    addAndMakeVisible(mixes_);
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
    job_.reset();
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

bool ReviewPanel::export_mixes(const juce::File& folder)
{
    if (exporting()) return false;
    export_queue_.clear();
    for (int i = 0; i < 3; ++i)
        if (!slots_[static_cast<std::size_t>(i)].empty()) export_queue_.push_back(i);
    if (export_queue_.empty()) {
        export_message_ = "Nothing to render: put something in A, B or C";
        return false;
    }
    context_.engine.stop();
    before_export_ = context_.session.active_versions();
    if (ended_ || current_ < 0) original_ = before_export_;
    export_folder_ = folder;
    exported_.clear();
    const auto& sel = context_.session.selection();
    export_start_ = sel.has_range() ? sel.from : 0;
    export_end_ = sel.has_range() ? sel.to : context_.session.content_end();
    export_message_ = "Rendering...";
    startTimerHz(30);
    next_mix();
    return true;
}

void ReviewPanel::next_mix()
{
    if (export_queue_.empty()) {
        finish_mixes();
        return;
    }
    export_slot_ = export_queue_.front();
    export_queue_.erase(export_queue_.begin());
    // This slot's versions, from where the review began.
    auto versions = mode_ == Mode::Snapshots ? original_ : before_export_;
    for (const auto& [track, version] : versions_of(export_slot_)) versions[track] = version;
    context_.session.audition_versions(versions);
    const juce::String label = slot_text(export_slot_).fromFirstOccurrenceOf("(", false, false).upToLastOccurrenceOf(")", false, false);
    const juce::String name = juce::String(session::package::safe_name(context_.session.data().name)) + " - Review " +
                              juce::String(kSlotNames[export_slot_]) + " (" +
                              juce::String(session::package::safe_name(label.toStdString())) + ").wav";
    MasterExportRequest request;
    request.file = export_folder_.getChildFile(name);
    request.start = export_start_;
    request.end = export_end_;
    request.tail_seconds = 2.0;
    job_ = std::make_unique<MasterExportJob>(context_, request);
    export_message_ = "Rendering " + juce::String(kSlotNames[export_slot_]) + "...";
    diff_.setText(export_message_, juce::dontSendNotification);
}

void ReviewPanel::finish_mixes()
{
    context_.session.audition_versions(before_export_);
    // What each file holds, and where they differ.
    const auto& d = context_.session.data();
    juce::String notes = "Review mixes of \"" + juce::String(d.name) + "\"\n";
    notes << "Range: bar " << d.meter.bar_at(export_start_) << " to bar " << d.meter.bar_at(std::max<Ticks>(0, export_end_ - 1))
          << " (" << picture::project_timecode(d, d.tempo.seconds_at(export_start_)) << " - "
          << picture::project_timecode(d, d.tempo.seconds_at(export_end_)) << ")\n\n";
    for (const auto& f : exported_) notes << "  " << f << "\n";
    const auto diff = difference();
    if (diff.isNotEmpty()) notes << "\nDifferences: " << diff << "\n";
    int open = 0;
    for (const auto& c : d.comments) {
        if (c.resolved) continue;
        const Ticks at = d.tempo.tick_at_seconds(c.seconds);
        if (at < export_start_ || at > export_end_) continue;
        if (open++ == 0) notes << "\nOpen comments in this range:\n";
        notes << "  " << picture::project_timecode(d, c.seconds) << "  bar " << picture::locate_in_music(d, c.seconds).bar << "  "
              << c.author << ": " << c.text << "\n";
    }
    export_folder_.getChildFile("Review Mixes.txt").replaceWithText(notes);
    export_message_ = "Rendered " + juce::String(exported_.size()) + " review mix" + (exported_.size() == 1 ? "" : "es") +
                      " into " + export_folder_.getFullPathName();
    if (context_.status) context_.status(export_message_);
    startTimerHz(10);
    export_slot_ = -1;
    reload();
    diff_.setText(export_message_, juce::dontSendNotification);
}

void ReviewPanel::timerCallback()
{
    if (job_) {
        if (job_->step(30.0)) return;
        const auto r = job_->result();
        const auto file = job_->request().file;
        job_.reset();
        if (!r.ok) {
            export_queue_.clear();
            export_message_ = "Review mixes stopped: " + r.message;
            context_.session.audition_versions(before_export_);
            startTimerHz(10);
            diff_.setText(export_message_, juce::dontSendNotification);
            return;
        }
        exported_.add(file.getFileName() + juce::String::formatted("  (%.1f LUFS, %.1f dBTP)", r.integrated, r.true_peak));
        next_mix();
        return;
    }
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
    b.removeFromTop(8);
    auto more = b.removeFromTop(28);
    comments_.setBounds(more.removeFromLeft(130));
    more.removeFromLeft(8);
    mixes_.setBounds(more.removeFromLeft(190));
    b.removeFromTop(8);
    help_.setBounds(b.removeFromTop(40));
}

void show_review(AppContext& context)
{
    dialogs::show_tool(review_window(), "Director Review", 760, 440, [&context] { return new ReviewPanel(context); },
                       [] {
                           // Closing without Keep: back to where the review began.
                           if (auto* p = review_slot(); p && !p->ended()) p->back();
                       });
}

ReviewPanel* review_panel() { return review_slot(); }

// --- Comments ----------------------------------------------------------------------------

namespace {
juce::Component::SafePointer<dialogs::ToolWindow>& comments_window()
{
    static juce::Component::SafePointer<dialogs::ToolWindow> w;
    return w;
}
CommentsPanel*& comments_slot()
{
    static CommentsPanel* p = nullptr;
    return p;
}
juce::String when_text(const std::string& iso)
{
    // "2026-11-15T18:42:10Z" -> "2026-11-15 18:42"
    juce::String s(iso);
    return s.length() >= 16 ? s.substring(0, 10) + " " + s.substring(11, 16) : s;
}
}  // namespace

class CommentsPanel::Row final : public juce::Component {
public:
    Row(AppContext& context, const project::Comment& c) : context_(context), id_(c.id)
    {
        const auto& d = context.session.data();
        juce::String head = juce::String(picture::project_timecode(d, c.seconds)) + "  -  bar " +
                            juce::String(picture::locate_in_music(d, c.seconds).bar);
        if (const auto* t = d.find_track(c.track_id)) head << "  -  " << t->name;
        head << "  -  " << c.author << ", " << when_text(c.created);
        head_ = head;
        text_ = juce::String(c.text);
        for (const auto& r : c.replies) replies_.add(juce::String(r.author) + ": " + juce::String(r.text));
        resolved_ = c.resolved;
        resolve_.setButtonText(c.resolved ? "Reopen" : "Resolve");
        resolve_.setComponentID("comment_resolve_" + juce::String(c.id));
        reply_.setComponentID("comment_reply_" + juce::String(c.id));
        resolve_.onClick = [this] {
            if (const auto* cm = context_.session.find_comment(id_)) context_.session.set_comment_resolved(id_, !cm->resolved);
        };
        reply_.onClick = [this] {
            auto* panel = comments_slot();
            const auto text = panel ? panel->entry().getText().trim() : juce::String();
            if (text.isEmpty()) {
                if (context_.status) context_.status("Type the reply in the box at the top, then press Reply");
                return;
            }
            context_.session.reply_to_comment(id_, panel->author().getText().toStdString(), text.toStdString());
            panel->entry().clear();
        };
        remove_.onClick = [this] { context_.session.remove_comment(id_); };
        for (auto* b : {&resolve_, &reply_, &remove_}) addAndMakeVisible(*b);
    }

    const std::string& id() const noexcept { return id_; }
    juce::Button& resolve() noexcept { return resolve_; }
    juce::Button& reply() noexcept { return reply_; }
    bool highlighted = false;

    int height_for(int width) const
    {
        juce::AttributedString a;
        a.append(text_, hw::fonts::ui(13.0f, false));
        juce::TextLayout layout;
        layout.createLayout(a, static_cast<float>(width - 24));
        return 26 + static_cast<int>(std::ceil(layout.getHeight())) + 16 * replies_.size() + 34;
    }

    void paint(juce::Graphics& g) override
    {
        auto r = getLocalBounds().reduced(4, 3).toFloat();
        g.setColour(highlighted ? hw::Theme::get().lcd().withAlpha(0.18f) : juce::Colour(0xff23272c));
        g.fillRoundedRectangle(r, 5.0f);
        g.setColour(resolved_ ? juce::Colour(0xff6b737c) : juce::Colour(0xffe0a040));
        g.fillRoundedRectangle(r.removeFromLeft(4.0f), 2.0f);
        r.removeFromLeft(8.0f);
        g.setColour(hw::tok::text);
        g.setFont(hw::fonts::ui(11.0f, true));
        g.drawText(head_ + (resolved_ ? "  -  RESOLVED" : ""), r.removeFromTop(20.0f), juce::Justification::centredLeft, true);
        juce::AttributedString a;
        a.append(text_, hw::fonts::ui(13.0f, false), resolved_ ? hw::tok::text : hw::tok::text_bright);
        juce::TextLayout layout;
        layout.createLayout(a, r.getWidth() - 8.0f);
        layout.draw(g, r.removeFromTop(layout.getHeight() + 4.0f));
        g.setFont(hw::fonts::ui(11.5f, false));
        g.setColour(hw::tok::text);
        for (const auto& reply : replies_) g.drawText("    " + reply, r.removeFromTop(16.0f), juce::Justification::centredLeft, true);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced(10, 6).removeFromBottom(26);
        remove_.setBounds(r.removeFromRight(70));
        r.removeFromRight(6);
        reply_.setBounds(r.removeFromRight(70));
        r.removeFromRight(6);
        resolve_.setBounds(r.removeFromRight(80));
    }

    void mouseUp(const juce::MouseEvent&) override
    {
        if (const auto* c = context_.session.find_comment(id_)) {
            context_.engine.locate(context_.session.data().tempo.tick_at_seconds(c->seconds));
            if (auto* p = comments_slot()) p->focus(id_);
        }
    }

private:
    AppContext& context_;
    std::string id_;
    juce::String head_, text_;
    juce::StringArray replies_;
    bool resolved_ = false;
    juce::TextButton resolve_{"Resolve"}, reply_{"Reply"}, remove_{"Delete"};
};

CommentsPanel::CommentsPanel(AppContext& context) : context_(context)
{
    setComponentID("comments_panel");
    comments_slot() = this;
    entry_.setTextToShowWhenEmpty("Comment at the playhead (or a reply)...", hw::tok::text.withAlpha(0.5f));
    entry_.setMultiLine(false);
    entry_.onReturnKey = [this] { add_at_playhead(); };
    auto saved = hw::ui_settings().getValue("review_author");
    if (saved.isEmpty()) saved = juce::SystemStats::getFullUserName();
    if (saved.isEmpty()) saved = juce::SystemStats::getLogonName();
    author_.setText(saved, false);
    author_.setTooltip("Your name on comments and replies");
    author_.onTextChange = [this] { hw::ui_settings().setValue("review_author", author_.getText()); };
    add_.onClick = [this] { add_at_playhead(); };
    resolved_.setToggleState(hw::ui_settings().getBoolValue("comments_show_resolved", true), juce::dontSendNotification);
    resolved_.onClick = [this] { set_show_resolved(resolved_.getToggleState()); };
    style_label(count_, 12.0f, true);
    view_.setViewedComponent(&list_, false);
    view_.setScrollBarsShown(true, false);
    for (auto* c : std::initializer_list<juce::Component*>{&entry_, &author_, &add_, &on_track_, &resolved_, &count_, &view_})
        addAndMakeVisible(*c);
    rebuild();
    startTimerHz(8);
}

CommentsPanel::~CommentsPanel()
{
    stopTimer();
    if (comments_slot() == this) comments_slot() = nullptr;
}

std::string CommentsPanel::add_at_playhead()
{
    const auto text = entry_.getText().trim();
    if (text.isEmpty()) return {};
    const std::string track = on_track_.getToggleState() ? context_.session.selected_track() : std::string();
    const auto id = context_.session.add_comment(context_.engine.position_seconds(), text.toStdString(),
                                                 author_.getText().trim().toStdString(), track);
    if (!id.empty()) {
        entry_.clear();
        focused_ = id;
        rebuild();
        if (context_.status) context_.status("Comment added at " + juce::String(picture::project_timecode(context_.session.data(), context_.engine.position_seconds())));
    }
    return id;
}

void CommentsPanel::set_show_resolved(bool on)
{
    resolved_.setToggleState(on, juce::dontSendNotification);
    hw::ui_settings().setValue("comments_show_resolved", on);
    signature_.clear();
    rebuild();
}

void CommentsPanel::focus(const std::string& comment_id)
{
    focused_ = comment_id;
    if (const auto* c = context_.session.find_comment(comment_id); c && c->resolved && !resolved_.getToggleState())
        set_show_resolved(true);
    for (auto& r : rows_) {
        r->highlighted = r->id() == comment_id;
        r->repaint();
        if (r->highlighted) view_.setViewPosition(0, std::max(0, r->getY() - 8));
    }
}

std::vector<std::string> CommentsPanel::listed() const
{
    std::vector<std::string> out;
    for (const auto& r : rows_) out.push_back(r->id());
    return out;
}

juce::Component* CommentsPanel::row(const std::string& comment_id)
{
    for (auto& r : rows_)
        if (r->id() == comment_id) return r.get();
    return nullptr;
}

juce::Button* CommentsPanel::resolve_button(const std::string& comment_id)
{
    for (auto& r : rows_)
        if (r->id() == comment_id) return &r->resolve();
    return nullptr;
}

juce::Button* CommentsPanel::reply_button(const std::string& comment_id)
{
    for (auto& r : rows_)
        if (r->id() == comment_id) return &r->reply();
    return nullptr;
}

void CommentsPanel::rebuild()
{
    const auto& d = context_.session.data();
    rows_.clear();
    int open = 0;
    for (const auto& c : d.comments) {
        if (!c.resolved) ++open;
        if (c.resolved && !resolved_.getToggleState()) continue;
        rows_.push_back(std::make_unique<Row>(context_, c));
        rows_.back()->highlighted = c.id == focused_;
        list_.addAndMakeVisible(*rows_.back());
    }
    count_.setText(juce::String(open) + " open, " + juce::String(static_cast<int>(d.comments.size()) - open) + " resolved",
                   juce::dontSendNotification);
    resized();
}

void CommentsPanel::timerCallback()
{
    const auto& d = context_.session.data();
    std::string sig;
    for (const auto& c : d.comments)
        sig += c.id + "|" + std::to_string(c.seconds) + c.text + (c.resolved ? "R" : "") + std::to_string(c.replies.size()) + ";";
    if (sig != signature_) {
        signature_ = sig;
        rebuild();
    }
}

void CommentsPanel::paint(juce::Graphics& g) { g.fillAll(hw::tok::panel_bottom); }

void CommentsPanel::resized()
{
    auto b = getLocalBounds().reduced(12);
    auto top = b.removeFromTop(28);
    add_.setBounds(top.removeFromRight(130));
    top.removeFromRight(6);
    author_.setBounds(top.removeFromRight(140));
    top.removeFromRight(6);
    entry_.setBounds(top);
    b.removeFromTop(6);
    auto opts = b.removeFromTop(24);
    on_track_.setBounds(opts.removeFromLeft(190));
    resolved_.setBounds(opts.removeFromLeft(150));
    count_.setBounds(opts);
    b.removeFromTop(6);
    view_.setBounds(b);
    const int w = std::max(100, b.getWidth() - view_.getScrollBarThickness());
    int y = 0;
    for (auto& r : rows_) {
        const int h = r->height_for(w);
        r->setBounds(0, y, w, h);
        y += h;
    }
    list_.setSize(w, std::max(y, 10));
}

void show_comments(AppContext& context, const std::string& focus)
{
    dialogs::show_tool(comments_window(), "Comments", 640, 560, [&context] { return new CommentsPanel(context); });
    if (!focus.empty())
        if (auto* p = comments_slot()) p->focus(focus);
}

CommentsPanel* comments_panel() { return comments_slot(); }

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
        // (MSVC: build the pointer first; an init-capture of `this` in a
        // nested lambda does not compile there.)
        const juce::Component::SafePointer<PackagePanel> safe(this);
        chooser_ = std::make_unique<juce::FileChooser>("Package into", destination_.getParentDirectory());
        chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                              [safe](const juce::FileChooser& fc) {
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
