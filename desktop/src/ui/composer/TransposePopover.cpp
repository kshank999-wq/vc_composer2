#include "ui/composer/TransposePopover.h"

#include "ui/hw/HwTheme.h"
#include "vcc/theory/Pitch.h"

namespace vcc::desktop::composer_ui {

namespace {
TransposePopover*& open_one()
{
    static TransposePopover* p = nullptr;
    return p;
}
const char* const kKeys[] = {"C", "Db", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B"};
}  // namespace

TransposePopover::TransposePopover(AppContext& context, std::string track_id)
    : context_(context), track_(std::move(track_id))
{
    setComponentID("transpose_popover");
    open_one() = this;
    for (auto* k : {&down_, &up_, &oct_down_, &oct_up_, &reset_, &bypass_, &notation_, &playback_, &apply_}) {
        k->setWantsKeyboardFocus(false);
        addAndMakeVisible(*k);
    }
    down_.setComponentID("trans_down");
    up_.setComponentID("trans_up");
    down_.setTooltip("Down a semitone");
    up_.setTooltip("Up a semitone");
    oct_down_.setTooltip("Down an octave");
    oct_up_.setTooltip("Up an octave");
    down_.onClick = [this] { nudge(-1); };
    up_.onClick = [this] { nudge(1); };
    oct_down_.onClick = [this] { nudge(-12); };
    oct_up_.onClick = [this] { nudge(12); };
    reset_.onClick = [this] { reset(); };
    bypass_.set_led_dot(true);
    bypass_.setComponentID("trans_bypass");
    bypass_.setTooltip("A/B: hear and see the track without its shift (the shift is kept)");
    bypass_.onClick = [this] { toggle_bypass(); };
    notation_.set_led_dot(true);
    playback_.set_led_dot(true);
    notation_.setTooltip("The shift is heard and shown on the staff");
    playback_.setTooltip("The shift is heard; the staff keeps the notes as written");
    notation_.onClick = [this] { set_playback_only(false); };
    playback_.onClick = [this] { set_playback_only(true); };
    apply_.setComponentID("trans_apply");
    apply_.setTooltip("Write the shift into the notes and set the shift back to 0 (asks first; one undo step)");
    apply_.onClick = [this] { apply_to_notes(); };
    target_.setTextWhenNothingSelected("To key...");
    for (int i = 0; i < 12; ++i) target_.addItem("To " + juce::String(kKeys[i]), i + 1);
    target_.setTooltip("The shift that moves the music from the project's key to this one");
    target_.onChange = [this] {
        if (target_.getSelectedId() > 0) transpose_to_key(target_.getSelectedId() - 1);
        target_.setSelectedId(0, juce::dontSendNotification);
    };
    addAndMakeVisible(target_);
    setSize(330, 196);
    refresh();
    startTimer(250);
}

TransposePopover::~TransposePopover()
{
    stopTimer();
    if (open_one() == this) open_one() = nullptr;
}

TransposePopover* TransposePopover::current() { return open_one(); }

TransposePopover* TransposePopover::show(AppContext& context, const std::string& track_id, juce::Rectangle<int> anchor)
{
    auto content = std::make_unique<TransposePopover>(context, track_id);
    auto* raw = content.get();
    juce::CallOutBox::launchAsynchronously(std::move(content), anchor, nullptr);
    return raw;
}

void TransposePopover::nudge(int semitones)
{
    if (const auto* t = context_.session.data().find_track(track_)) set(t->transpose + semitones);
}

void TransposePopover::set(int semitones)
{
    std::string why;
    const auto* t = context_.session.data().find_track(track_);
    if (!t) return;
    if (!context_.session.can_transpose(*t, &why)) {
        context_.status(juce::String(why));
        return;
    }
    context_.session.set_track_transpose(track_, semitones);
    refresh();
}

void TransposePopover::reset() { set(0); }

void TransposePopover::toggle_bypass()
{
    if (const auto* t = context_.session.data().find_track(track_)) context_.session.set_transpose_bypass(track_, !t->transpose_bypass);
    refresh();
}

void TransposePopover::set_playback_only(bool on)
{
    context_.session.set_transpose_mode(track_, on ? "playback" : "notation");
    refresh();
}

void TransposePopover::transpose_to_key(int pitch_class)
{
    const auto key = theory::Key::parse(context_.session.data().key);
    const int from = key ? key->tonic.pitch_class() : 0;
    int shift = ((pitch_class - from) % 12 + 12) % 12;
    if (shift > 6) shift -= 12;  // the nearer way
    set(shift);
}

void TransposePopover::apply_to_notes()
{
    const auto* t = context_.session.data().find_track(track_);
    if (!t || t->transpose == 0) {
        context_.status("No shift to write into the notes");
        return;
    }
    auto* w = new juce::AlertWindow("Apply to notes",
                                    "Write " + juce::String(session::Session::interval_name(t->transpose)) + " into " +
                                        juce::String(t->name) + "'s notes and set its Trans back to 0? One undo step.",
                                    juce::MessageBoxIconType::QuestionIcon);
    w->setComponentID("trans_apply_question");
    w->addButton("Apply", 1, juce::KeyPress(juce::KeyPress::returnKey));
    w->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    juce::Component::SafePointer<TransposePopover> self(this);
    auto& ctx = context_;
    const std::string id = track_;
    w->enterModalState(true, juce::ModalCallbackFunction::create([self, &ctx, id](int r) {
        if (r != 1) return;
        if (ctx.session.commit_track_transpose(id)) ctx.status("The notes are written transposed; Trans is back to 0");
        if (self) self->refresh();
    }), true);
}

void TransposePopover::refresh()
{
    const auto* t = context_.session.data().find_track(track_);
    if (!t) return;
    const int s = t->transpose;
    readout_ = (s > 0 ? "+" : "") + juce::String(s) + " st";
    interval_ = juce::String(session::Session::interval_name(s));
    if (t->transpose_bypass && s != 0) interval_ << " (bypassed)";
    bypass_.setToggleState(t->transpose_bypass, juce::dontSendNotification);
    notation_.setToggleState(t->transpose_mode == "notation", juce::dontSendNotification);
    playback_.setToggleState(t->transpose_mode == "playback", juce::dontSendNotification);
    apply_.setEnabled(s != 0);
    reset_.setEnabled(s != 0);
    // Range after the shift: said, never fixed silently.
    const auto issues = context_.session.range_issues(*t);
    const auto limits = context_.session.range_limits(*t);
    warning_.clear();
    if (!issues.empty())
        warning_ << static_cast<int>(issues.size()) << (issues.size() == 1 ? " note is" : " notes are") << " outside the "
                 << juce::String(limits.instrument) << "'s range now (ringed)";
    std::string why;
    const bool ok = context_.session.can_transpose(*t, &why);
    for (auto* k : {&down_, &up_, &oct_down_, &oct_up_}) k->setEnabled(ok);
    target_.setEnabled(ok);
    if (!ok) warning_ = juce::String(why);
    repaint();
}

void TransposePopover::paint(juce::Graphics& g)
{
    g.fillAll(hw::tok::panel_bottom);
    auto r = getLocalBounds().reduced(10, 8);
    const auto* t = context_.session.data().find_track(track_);
    hw::paint::silk(g, "Trans  " + juce::String(t ? t->name : std::string()),
                    r.removeFromTop(14).toFloat(), 10.0f);
    auto lcd = r.removeFromTop(34).toFloat().withWidth(120.0f);
    hw::paint::lcd_window(g, lcd);
    hw::paint::lcd_text(g, readout_, lcd.reduced(8, 2), 18.0f, juce::Justification::centredLeft);
    g.setColour(hw::tok::text_bright);
    g.setFont(hw::fonts::ui(12.0f, false));
    g.drawText(interval_, juce::Rectangle<float>(lcd.getRight() + 10, lcd.getY(), 190, lcd.getHeight()),
               juce::Justification::centredLeft, true);
    if (warning_.isNotEmpty()) {
        g.setColour(juce::Colour(0xffe0a040));
        g.setFont(hw::fonts::ui(11.0f, false));
        g.drawText(warning_, getLocalBounds().reduced(10, 0).removeFromBottom(20).toFloat(), juce::Justification::centredLeft, true);
    }
}

void TransposePopover::resized()
{
    auto r = getLocalBounds().reduced(10, 8);
    r.removeFromTop(14 + 34 + 8);
    auto row = r.removeFromTop(26);
    for (auto* k : {&oct_down_, &down_, &up_, &oct_up_}) {
        k->setBounds(row.removeFromLeft(56));
        row.removeFromLeft(4);
    }
    reset_.setBounds(row);
    r.removeFromTop(6);
    row = r.removeFromTop(26);
    notation_.setBounds(row.removeFromLeft(150));
    row.removeFromLeft(4);
    playback_.setBounds(row);
    r.removeFromTop(6);
    row = r.removeFromTop(26);
    bypass_.setBounds(row.removeFromLeft(96));
    row.removeFromLeft(4);
    target_.setBounds(row.removeFromLeft(84).reduced(0, 2));
    row.removeFromLeft(4);
    apply_.setBounds(row);
}

}  // namespace vcc::desktop::composer_ui
