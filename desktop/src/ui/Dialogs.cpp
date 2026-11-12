#include "ui/Dialogs.h"

#include <algorithm>
#include <cmath>

#include "ui/ChordExplorer.h"
#include "ui/OstinatoCreator.h"
#include "ui/TemplateBrowser.h"
#include "ui/Theme.h"

namespace vcc::desktop {

namespace {
juce::Component::SafePointer<dialogs::ToolWindow> template_window, chord_window, ostinato_window;
}  // namespace

void show_template_browser(AppContext& context, bool for_new_project)
{
    // An open browser switches to the requested default action.
    if (template_window != nullptr)
        if (auto* browser = dynamic_cast<TemplateBrowser*>(template_window->getContentComponent()))
            browser->set_for_new_project(for_new_project);
    dialogs::show_tool(
        template_window, "Templates", 1040, 660,
        [&context, for_new_project] { return new TemplateBrowser(context, for_new_project); },
        [&context] { context.engine.stop_preview(); });
}

void show_chord_explorer(AppContext& context)
{
    dialogs::show_tool(
        chord_window, "Chord Explorer", 1060, 680, [&context] { return new ChordExplorer(context); },
        [&context] { context.engine.stop_preview(); });
}

void show_ostinato_creator(AppContext& context)
{
    dialogs::show_tool(
        ostinato_window, "Ostinato Creator", 1080, 680, [&context] { return new OstinatoCreator(context); },
        [&context] { context.engine.stop_preview(); });
}

namespace dialogs {

// --- Windows ------------------------------------------------------------------------------

ToolWindow::ToolWindow(const juce::String& title, juce::Component* content, int width, int height)
    : juce::DocumentWindow(title, colours::background, juce::DocumentWindow::closeButton | juce::DocumentWindow::minimiseButton)
{
    setUsingNativeTitleBar(true);
    setContentOwned(content, false);
    setResizable(true, false);
    setResizeLimits(760, 480, 4000, 3000);
    centreWithSize(width, height);
}

void ToolWindow::closeButtonPressed()
{
    if (on_close) on_close();
    setVisible(false);
    juce::Component::SafePointer<ToolWindow> self(this);
    juce::MessageManager::callAsync([self] {
        if (self != nullptr) delete self.getComponent();
    });
}

void show_tool(juce::Component::SafePointer<ToolWindow>& slot, const juce::String& title, int width, int height,
               const std::function<juce::Component*()>& make, std::function<void()> on_close)
{
    if (slot != nullptr && slot->isVisible()) {
        slot->setMinimised(false);
        slot->toFront(true);
        return;
    }
    auto* window = new ToolWindow(title, make(), width, height);
    window->on_close = std::move(on_close);
    window->setVisible(true);
    window->toFront(true);
    slot = window;
}

// --- Send to Track --------------------------------------------------------------------------

void ask_collision(const juce::String& what, std::function<void(project::CollisionPolicy)> done)
{
    auto* alert = new juce::AlertWindow("Material already there",
                                        what + " overlaps notes already on the track.\n"
                                               "Replace them, merge with them, or put the new notes on a new lane?",
                                        juce::MessageBoxIconType::QuestionIcon);
    alert->addButton("Replace", 1);
    alert->addButton("Merge", 2);
    alert->addButton("New Lane", 3, juce::KeyPress(juce::KeyPress::returnKey));
    alert->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    alert->enterModalState(true, juce::ModalCallbackFunction::create([done = std::move(done)](int result) {
                               switch (result) {
                                   case 1: done(project::CollisionPolicy::Replace); break;
                                   case 2: done(project::CollisionPolicy::Merge); break;
                                   case 3: done(project::CollisionPolicy::NewLane); break;
                                   default: break;
                               }
                           }),
                           true);
}

void report_send(AppContext& context, const project::InsertResult& result, const juce::String& what)
{
    if (result.ok) {
        context.status(what + " sent to the track (Undo removes it in one step)");
        return;
    }
    const juce::String why = result.error.empty() ? juce::String("The notes could not be inserted.")
                                                  : juce::String(result.error);
    juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Send to Track", why);
    context.status("Send to Track failed: " + why);
}

// --- Keys -----------------------------------------------------------------------------------

const std::vector<std::string>& tonic_names()
{
    static const std::vector<std::string> names = {"C",  "C#", "Db", "D",  "D#", "Eb", "E",  "F", "F#",
                                                   "Gb", "G",  "G#", "Ab", "A",  "A#", "Bb", "B"};
    return names;
}

namespace {
const std::vector<theory::Mode>& all_modes()
{
    using M = theory::Mode;
    static const std::vector<M> modes = {M::Major,  M::Minor,      M::HarmonicMinor,    M::MelodicMinor,   M::Dorian,
                                         M::Phrygian, M::Lydian,   M::Mixolydian,       M::Aeolian,        M::Locrian,
                                         M::PhrygianDominant, M::DoubleHarmonic, M::WholeTone, M::Octatonic,
                                         M::MajorPentatonic, M::MinorPentatonic, M::Blues};
    return modes;
}
}  // namespace

void fill_key_combos(juce::ComboBox& tonic, juce::ComboBox& mode, const theory::Key& key, bool heptatonic_only)
{
    tonic.clear(juce::dontSendNotification);
    std::vector<std::string> names = tonic_names();
    const std::string wanted = key.tonic.name();
    if (std::find(names.begin(), names.end(), wanted) == names.end()) names.push_back(wanted);
    for (std::size_t i = 0; i < names.size(); ++i) {
        auto p = theory::SpelledPitch::parse(names[i]);
        tonic.addItem(p ? juce::String::fromUTF8(p->display_name().c_str()) : juce::String(names[i]),
                      static_cast<int>(i) + 1);
        if (names[i] == wanted) tonic.setSelectedId(static_cast<int>(i) + 1, juce::dontSendNotification);
    }
    mode.clear(juce::dontSendNotification);
    int selected = 0;
    for (std::size_t i = 0; i < all_modes().size(); ++i) {
        const auto m = all_modes()[i];
        if (heptatonic_only && !theory::is_heptatonic(m)) continue;
        mode.addItem(theory::display_name(m), static_cast<int>(i) + 1);
        if (m == key.mode) selected = static_cast<int>(i) + 1;
    }
    if (selected == 0) selected = key.mode == theory::Mode::Major || !theory::is_minor_family(key.mode) ? 1 : 2;
    mode.setSelectedId(selected, juce::dontSendNotification);
}

std::optional<theory::Key> key_from_combos(const juce::ComboBox& tonic, const juce::ComboBox& mode)
{
    const int t = tonic.getSelectedId(), m = mode.getSelectedId();
    if (t <= 0 || m <= 0 || m > static_cast<int>(all_modes().size())) return std::nullopt;
    std::string name = static_cast<std::size_t>(t) <= tonic_names().size()
                           ? tonic_names()[static_cast<std::size_t>(t - 1)]
                           : std::string();
    if (name.empty()) {
        // An extra spelling appended for the project key: parse it back from the item text.
        name = tonic.getItemText(tonic.getNumItems() - 1).replace(juce::String::fromUTF8("♯"), "#")
                   .replace(juce::String::fromUTF8("♭"), "b").toStdString();
    }
    auto p = theory::SpelledPitch::parse(name);
    if (!p) return std::nullopt;
    return theory::Key{*p, all_modes()[static_cast<std::size_t>(m - 1)]};
}

theory::Key project_key(const AppContext& context)
{
    if (auto k = theory::Key::parse(context.session.data().key)) return *k;
    return theory::Key{theory::SpelledPitch{0, 0}, theory::Mode::Major};
}

// --- Audition -------------------------------------------------------------------------------

std::pair<std::string, int> preview_voice(const AppContext& context, const std::string& track_id)
{
    if (const auto* t = context.session.data().find_track(track_id))
        if (const auto* def = context.session.instrument_for(*t)) return {def->family, def->unpitched ? -1 : def->gm_program};
    return {"keys", 0};
}

double project_bpm(const AppContext& context)
{
    const double bpm = context.session.data().tempo.bpm_at(0);
    return bpm > 0 ? bpm : 100.0;
}

std::vector<NoteEvent> shifted(const std::vector<NoteEvent>& notes, Ticks origin)
{
    std::vector<NoteEvent> out = notes;
    for (auto& n : out) n.start = std::max<Ticks>(0, n.start - origin);
    return out;
}

juce::String note_name(int midi)
{
    return juce::MidiMessage::getMidiNoteName(midi, true, true, 4);
}

// --- Drawing --------------------------------------------------------------------------------

void draw_mini_roll(juce::Graphics& g, juce::Rectangle<float> area, const std::vector<NoteEvent>& notes, Ticks start,
                    Ticks end, const MeterMap& meter, juce::Colour colour)
{
    g.setColour(colours::background.darker(0.25f));
    g.fillRoundedRectangle(area, 4.0f);
    if (end <= start) return;
    const auto inner = area.reduced(4.0f, 5.0f);
    const auto span = static_cast<double>(end - start);
    auto x_of = [&](Ticks t) {
        return inner.getX() + static_cast<float>(static_cast<double>(t - start) / span) * inner.getWidth();
    };

    // Bar lines (beats faintly when there is room).
    const int first = meter.bar_at(start), last = meter.bar_at(end - 1);
    for (int bar = first; bar <= last + 1; ++bar) {
        const Ticks bt = meter.bar_start(bar);
        if (bt < start || bt > end) continue;
        g.setColour(colours::outline.withAlpha(bar == first ? 0.0f : 0.9f));
        g.drawVerticalLine(juce::roundToInt(x_of(bt)), inner.getY(), inner.getBottom());
        const auto sig = meter.at_bar(bar);
        if ((last - first) <= 8)
            for (int b = 1; b < sig.numerator; ++b) {
                const Ticks t = bt + b * sig.beat_ticks();
                if (t >= end) break;
                g.setColour(colours::outline.withAlpha(0.35f));
                g.drawVerticalLine(juce::roundToInt(x_of(t)), inner.getY(), inner.getBottom());
            }
    }
    if (notes.empty()) {
        g.setColour(colours::text_dim);
        g.setFont(juce::FontOptions(12.0f));
        g.drawText("No notes", area, juce::Justification::centred);
        return;
    }
    int lo = 127, hi = 0;
    for (const auto& n : notes) {
        lo = std::min(lo, n.pitch);
        hi = std::max(hi, n.pitch);
    }
    lo -= 1;
    hi += 1;
    // Rows are capped so a narrow figure sits in the middle rather than in
    // a few huge stripes.
    const float row = std::min(9.0f, inner.getHeight() / static_cast<float>(hi - lo + 1));
    const float top = inner.getCentreY() - row * static_cast<float>(hi - lo + 1) / 2.0f;
    g.setColour(colours::outline.withAlpha(0.18f));
    for (int p = lo; p <= hi; ++p)
        if (juce::MidiMessage::isMidiNoteBlack(p))
            g.fillRect(inner.getX(), top + static_cast<float>(hi - p) * row, inner.getWidth(), row);
    for (const auto& n : notes) {
        const float x = x_of(n.start);
        const float w = std::max(2.0f, x_of(n.end()) - x - 0.5f);
        const float y = top + static_cast<float>(hi - n.pitch) * row;
        const float alpha = 0.45f + 0.55f * static_cast<float>(juce::jlimit(1, 127, n.velocity)) / 127.0f;
        g.setColour(colour.withAlpha(alpha));
        g.fillRoundedRectangle(x, y, w, std::max(2.0f, row - 0.5f), std::min(2.0f, row * 0.4f));
    }
}

void style_caption(juce::Label& label, const juce::String& text)
{
    label.setText(text, juce::dontSendNotification);
    label.setFont(juce::FontOptions(12.0f));
    label.setColour(juce::Label::textColourId, colours::text_dim);
    label.setJustificationType(juce::Justification::centredRight);
    label.setMinimumHorizontalScale(0.8f);
}

// --- Track selector -------------------------------------------------------------------------

TrackSelector::TrackSelector(AppContext& context, juce::String new_track_label)
    : context_(context), new_label_(std::move(new_track_label))
{
    refresh();
}

void TrackSelector::refresh()
{
    std::string current = getSelectedId() > 1 ? track_id() : std::string();
    const bool was_new = getSelectedId() == 1;
    if (getSelectedId() == 0) current = context_.session.selected_track();
    clear(juce::dontSendNotification);
    ids_.clear();
    addItem(new_label_, 1);
    addSeparator();
    int select_id = 0;
    for (const auto* t : context_.session.music_tracks()) {
        if (t->kind != project::TrackKind::Instrument) continue;
        ids_.push_back(t->id);
        const int id = static_cast<int>(ids_.size()) + 1;
        addItem(juce::String::fromUTF8(t->name.c_str()), id);
        if (t->id == current) select_id = id;
    }
    if (was_new) select_id = 1;
    if (select_id == 0) select_id = ids_.empty() ? 1 : 2;
    setSelectedId(select_id, juce::dontSendNotification);
}

std::string TrackSelector::track_id() const
{
    const int id = getSelectedId();
    return id >= 2 && id - 2 < static_cast<int>(ids_.size()) ? ids_[static_cast<std::size_t>(id - 2)] : std::string();
}

void TrackSelector::select(const std::string& track_id)
{
    for (std::size_t i = 0; i < ids_.size(); ++i)
        if (ids_[i] == track_id) setSelectedId(static_cast<int>(i) + 2, juce::dontSendNotification);
}

void TrackSelector::showPopup()
{
    refresh();
    juce::ComboBox::showPopup();
}

}  // namespace dialogs

}  // namespace vcc::desktop
