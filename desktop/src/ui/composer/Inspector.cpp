#include "ui/composer/Inspector.h"

#include <algorithm>
#include <cmath>

#include "AppResources.h"
#include "ui/composer/KeyLegend.h"
#include "vcc/project/Performance.h"

namespace vcc::desktop::composer_ui {

using composer::Command;

namespace {
juce::String utf8(const char* s) { return juce::String::fromUTF8(s); }
constexpr int kKeyH = 26, kGap = 6, kLabel = 14;
}  // namespace

Inspector::Inspector(AppContext& context) : hw::Panel("Inspector"), context_(context)
{
    const auto& font = music_font();
    const juce::Font artic_font = font.loaded ? font.font.font_for_staff_space(6.0) : juce::Font(juce::FontOptions(16.0f));
    // Articulation, 3 x 2: SMuFL glyphs over the mark's shortcut.
    const std::pair<Command, const char*> artics[] = {
        {Command::ArticulationAccent, "\xee\x92\xa0"},          // U+E4A0 articAccentAbove
        {Command::ArticulationStaccato, "\xee\x92\xa2"},        // U+E4A2 articStaccatoAbove
        {Command::ArticulationTenuto, "\xee\x92\xa4"},          // U+E4A4 articTenutoAbove
        {Command::ArticulationMarcato, "\xee\x92\xac"},         // U+E4AC articMarcatoAbove
        {Command::ArticulationStaccatissimo, "\xee\x92\xa8"},  // U+E4A8 articStaccatissimoWedgeAbove
        {Command::StartSlur, nullptr},
    };
    const char* artic_names[] = {"Accent", "Staccato", "Tenuto", "Marcato", "Staccatissimo", "Slur"};
    int i = 0;
    for (const auto& [command, glyph] : artics) {
        auto k = std::make_unique<hw::Keycap>(glyph ? key_legend(context_.shortcuts, command) : juce::String("Slur"));
        if (glyph && font.loaded) k->set_music_symbol(utf8(glyph), artic_font);
        k->setTooltip(juce::String(artic_names[i]) + " on the selected notes");
        k->setComponentID("inspector_artic_" + juce::String(i));
        k->setWantsKeyboardFocus(false);
        k->onClick = [this, command] {
            if (on_command) on_command(command);
        };
        addAndMakeVisible(*k);
        artic_.push_back(std::move(k));
        artic_cmd_.push_back(command);
        ++i;
    }
    // Dynamics, 4 x 2.
    const std::pair<Command, const char*> dyns[] = {
        {Command::DynamicPianissimo, "pp"}, {Command::DynamicPiano, "p"},     {Command::DynamicMezzoPiano, "mp"},
        {Command::DynamicMezzoForte, "mf"}, {Command::DynamicForte, "f"},     {Command::DynamicFortissimo, "ff"},
        {Command::StartCrescendo, "<"},     {Command::StartDiminuendo, ">"},
    };
    i = 0;
    for (const auto& [command, legend] : dyns) {
        auto k = std::make_unique<hw::Keycap>(legend);
        k->set_dynamic_legend(i < 6);
        k->set_legend_size(i < 6 ? 15.0f : 14.0f);
        k->setTooltip(i < 6 ? juce::String(legend) + " at the selected notes"
                            : juce::String(i == 6 ? "Crescendo" : "Diminuendo") + " over the selected notes");
        k->setComponentID("inspector_dyn_" + juce::String(i));
        k->setWantsKeyboardFocus(false);
        k->onClick = [this, command] {
            if (on_command) on_command(command);
        };
        addAndMakeVisible(*k);
        dyn_.push_back(std::move(k));
        dyn_cmd_.push_back(command);
        ++i;
    }
    // Performance: velocity and humanize write overrides, never the score.
    velocity_.setRange(1.0, 127.0, 1.0);
    velocity_.setValue(80.0, juce::dontSendNotification);
    velocity_.setTitle("Velocity");
    velocity_.setTooltip("The selected notes' performed velocity (a performance override; the score is unchanged)");
    velocity_.onDragStart = [this] { ++gesture_; };
    velocity_.onValueChange = [this] { set_velocity(juce::roundToInt(velocity_.getValue())); };
    humanize_.setRange(0.0, 100.0, 1.0);
    humanize_.setValue(0.0, juce::dontSendNotification);
    humanize_.setTitle("Humanize");
    humanize_.setTooltip("Humanize the selected notes' timing, length and velocity by this much, on release");
    humanize_.onValueChange = [this] { hum_lcd_.set_text(juce::String(juce::roundToInt(humanize_.getValue())) + "%"); };
    humanize_.onDragEnd = [this] { apply_humanize(humanize_.getValue()); };
    for (auto* c : std::initializer_list<juce::Component*>{&velocity_, &humanize_, &vel_lcd_, &hum_lcd_}) addAndMakeVisible(c);
    velocity_.setWantsKeyboardFocus(false);
    humanize_.setWantsKeyboardFocus(false);
    vel_lcd_.set_justification(juce::Justification::centred);
    hum_lcd_.set_justification(juce::Justification::centred);
    // Voice 1-4.
    for (int v = 0; v < 4; ++v) {
        auto k = std::make_unique<hw::Keycap>(juce::String(v + 1), false);
        k->set_led_dot(true);
        k->setTooltip("Voice " + juce::String(v + 1) + " (" + key_legend(context_.shortcuts, static_cast<Command>(
                                                                                                 static_cast<int>(Command::SelectVoice1) + v)) +
                      ")");
        k->setComponentID("inspector_voice_" + juce::String(v));
        k->setWantsKeyboardFocus(false);
        const auto command = static_cast<Command>(static_cast<int>(Command::SelectVoice1) + v);
        k->onClick = [this, command] {
            if (on_command) on_command(command);
        };
        addAndMakeVisible(*k);
        voice_.push_back(std::move(k));
    }
    // Transpose: octave and semitone, spelled in the key.
    const std::pair<const char*, int> steps[] = {{"Oct -", -12}, {"-1", -1}, {"+1", 1}, {"Oct +", 12}};
    for (const auto& [legend, semis] : steps) {
        auto k = std::make_unique<hw::Keycap>(legend);
        k->setTooltip(std::abs(semis) == 12 ? juce::String("Transpose the selected notes an octave ") + (semis > 0 ? "up" : "down")
                                            : juce::String("Transpose the selected notes a semitone ") + (semis > 0 ? "up" : "down"));
        k->setWantsKeyboardFocus(false);
        const int n = semis;
        k->onClick = [this, n] {
            const auto ids = selected();
            if (ids.empty()) {
                if (context_.status) context_.status("Select notes to transpose");
                return;
            }
            context_.session.transpose_notes(context_.session.selection().track, ids, n);
        };
        addAndMakeVisible(*k);
        transpose_.push_back(std::move(k));
    }
    listener_ = context_.session.add_listener([this](session::Change c) {
        if (c == session::Change::Document || c == session::Change::Selection) refresh();
    });
    refresh();
}

Inspector::~Inspector() { context_.session.remove_listener(listener_); }

std::vector<notation::EventId> Inspector::selected() const
{
    const auto& sel = context_.session.selection();
    return sel.notes;
}

std::map<notation::EventId, std::pair<int, int>> Inspector::velocities() const
{
    std::map<notation::EventId, std::pair<int, int>> out;
    const auto& sel = context_.session.selection();
    const auto* t = context_.session.data().find_track(sel.track);
    if (!t || sel.notes.empty()) return out;
    const auto& r = render_.render(*t);
    const auto* perf = context_.session.performance_of(sel.track);
    for (std::size_t i = 0; i < r.notes.size(); ++i) {
        const auto id = r.note_ids[i];
        if (!std::binary_search(sel.notes.begin(), sel.notes.end(), id) || out.count(id)) continue;
        int ov = 0;
        if (perf)
            if (auto it = perf->find(id); it != perf->end()) ov = it->second.velocity;
        out[id] = {r.notes[i].velocity, ov};
    }
    return out;
}

void Inspector::set_velocity(int target)
{
    const auto& sel = context_.session.selection();
    const auto vels = velocities();
    if (vels.empty()) return;
    target = juce::jlimit(1, 127, target);
    std::vector<notation::EventId> ids;
    for (const auto& [id, v] : vels) ids.push_back(id);
    // The override that makes each note sound at `target`: the written
    // velocity is what it plays now less its current override.
    context_.session.edit_performance(sel.track, ids, "Velocity" + std::string(gesture_ % 2 ? " " : ""),
                                      [&vels, target](notation::EventId id, project::NoteOverride& o) {
                                          const auto it = vels.find(id);
                                          if (it == vels.end()) return;
                                          const int written = it->second.first - it->second.second;
                                          o.velocity = target - written;
                                      });
}

void Inspector::apply_humanize(double percent)
{
    const auto ids = selected();
    if (ids.empty() || percent <= 0.0) {
        if (ids.empty() && percent > 0.0 && context_.status) context_.status("Select notes to humanize");
        return;
    }
    // 100% is a loose player: a 32nd early or late, a little longer or
    // shorter, 12 softer or louder. A new seed each time.
    const double f = percent / 100.0;
    project::HumanizeSettings s;
    s.timing = static_cast<Ticks>(std::lround(f * kTicksPerQuarter / 8.0));
    s.length = static_cast<Ticks>(std::lround(f * kTicksPerQuarter / 12.0));
    s.velocity = static_cast<int>(std::lround(f * 12.0));
    s.seed = ++seed_;
    // A velocity set with the Velocity knob is kept: humanize then varies
    // only timing and length (its velocity would replace the one set).
    bool kept = false;
    for (const auto& [id, v] : velocities())
        if (v.second != 0) kept = true;
    if (kept) s.velocity = 0;
    if (context_.session.humanize(context_.session.selection().track, ids, s) && context_.status)
        context_.status("Humanized " + juce::String(static_cast<int>(ids.size())) + " note" + (ids.size() == 1 ? "" : "s") +
                        " by " + juce::String(juce::roundToInt(percent)) + "%" +
                        (kept ? " (timing and length; the velocity set is kept)" : ""));
}

void Inspector::refresh_legends()
{
    for (std::size_t i = 0; i < artic_.size(); ++i)
        if (artic_cmd_[i] != Command::StartSlur) artic_[i]->setButtonText(key_legend(context_.shortcuts, artic_cmd_[i]));
}

void Inspector::set_horizontal(bool horizontal)
{
    if (horizontal_ == horizontal) return;
    horizontal_ = horizontal;
    resized();
    repaint();
}

void Inspector::set_voice(int voice)
{
    for (int v = 0; v < 4; ++v) voice_[static_cast<std::size_t>(v)]->setToggleState(v == voice, juce::dontSendNotification);
}

void Inspector::refresh()
{
    const auto& sel = context_.session.selection();
    const auto* t = context_.session.data().find_track(sel.track);
    const auto* v = t ? t->active_version() : nullptr;
    std::vector<const notation::ScoreNote*> notes;
    if (v)
        for (auto id : sel.notes)
            if (const auto* n = v->score.find_note(id); n && !n->is_rest) notes.push_back(n);
    // An articulation lights when every selected note has it.
    for (std::size_t i = 0; i < artic_.size(); ++i) {
        bool all = !notes.empty();
        notation::Articulation a = notation::Articulation::None;
        switch (artic_cmd_[i]) {
            case Command::ArticulationAccent: a = notation::Articulation::Accent; break;
            case Command::ArticulationStaccato: a = notation::Articulation::Staccato; break;
            case Command::ArticulationTenuto: a = notation::Articulation::Tenuto; break;
            case Command::ArticulationMarcato: a = notation::Articulation::Marcato; break;
            case Command::ArticulationStaccatissimo: a = notation::Articulation::Staccatissimo; break;
            default: all = false; break;
        }
        for (const auto* n : notes)
            if (all && !notation::has_articulation(n->articulations, a)) all = false;
        artic_[i]->setToggleState(all, juce::dontSendNotification);
    }
    // A dynamic lights when it is marked where the selection starts; a
    // hairpin when one starts there.
    const notation::ScoreNote* first = nullptr;
    for (const auto* n : notes)
        if (!first || n->start_tick < first->start_tick) first = n;
    for (std::size_t i = 0; i < dyn_.size(); ++i) {
        bool on = false;
        if (first && v) {
            const auto staff = static_cast<std::uint8_t>(v->score.staff_index_of(first->id));
            if (i < 6) {
                const auto level = static_cast<notation::DynamicLevel>(static_cast<int>(notation::DynamicLevel::Pianissimo) +
                                                                       static_cast<int>(i));
                for (const auto& d : v->score.dynamics())
                    if (d.tick == first->start_tick && d.staff == staff && d.level == level) on = true;
            } else {
                const bool cresc = i == 6;
                for (const auto& h : v->score.hairpins())
                    if (h.start_tick == first->start_tick && h.staff == staff && (h.kind == notation::HairpinKind::Crescendo) == cresc) on = true;
            }
        }
        dyn_[i]->setToggleState(on, juce::dontSendNotification);
    }
    // Velocity: the selection's performed velocity when it is one value.
    const auto vels = velocities();
    int lo = 128, hi = -1;
    for (const auto& [id, pv] : vels) {
        lo = std::min(lo, pv.first);
        hi = std::max(hi, pv.first);
    }
    shown_velocity_ = hi >= 0 && lo == hi ? lo : -1;
    if (hi >= 0) {
        if (!velocity_.isMouseButtonDown()) velocity_.setValue(lo == hi ? lo : (lo + hi) / 2, juce::dontSendNotification);
        vel_lcd_.set_text(lo == hi ? juce::String(lo) : juce::String(lo) + "-" + juce::String(hi));
    } else {
        vel_lcd_.set_text("-");
    }
    velocity_.setEnabled(!vels.empty());
    velocity_.repaint();
    for (auto& k : transpose_) k->setEnabled(!notes.empty());
}

void Inspector::resized()
{
    auto c = content().reduced(4, 0);
    // A block of keys under its label, `cols` across, in `area`.
    auto grid = [](std::vector<std::unique_ptr<hw::Keycap>>& keys, int cols, juce::Rectangle<int>& area) {
        area.removeFromTop(kLabel);
        const int kw = (area.getWidth() - (cols - 1) * kGap) / cols;
        const int rows = (static_cast<int>(keys.size()) + cols - 1) / cols;
        for (int r = 0; r < rows; ++r) {
            auto row = area.removeFromTop(kKeyH);
            for (int col = 0; col < cols; ++col) {
                const auto idx = static_cast<std::size_t>(r * cols + col);
                if (idx >= keys.size()) break;
                keys[idx]->setBounds(row.getX() + col * (kw + kGap), row.getY(), kw, kKeyH);
            }
            area.removeFromTop(kGap);
        }
        area.removeFromTop(4);
    };
    auto knobs = [this](juce::Rectangle<int> area) {
        area.removeFromTop(kLabel);
        auto top = area.removeFromTop(84);
        const int half = area.getWidth() / 2;
        velocity_.setBounds(juce::Rectangle<int>(84, 84).withCentre({top.getX() + half / 2, top.getCentreY()}));
        humanize_.setBounds(juce::Rectangle<int>(84, 84).withCentre({top.getX() + half + half / 2, top.getCentreY()}));
        auto lcds = area.removeFromTop(18);
        vel_lcd_.setBounds(juce::Rectangle<int>(56, 18).withCentre({lcds.getX() + half / 2, lcds.getCentreY()}));
        hum_lcd_.setBounds(juce::Rectangle<int>(56, 18).withCentre({lcds.getX() + half + half / 2, lcds.getCentreY()}));
    };
    if (horizontal_) {
        // Under the edit window: four columns side by side.
        const int w = c.getWidth();
        auto col1 = c.removeFromLeft(w * 22 / 100);
        c.removeFromLeft(12);
        auto col2 = c.removeFromLeft(w * 27 / 100);
        c.removeFromLeft(12);
        auto col3 = c.removeFromLeft(w * 26 / 100);
        c.removeFromLeft(12);
        grid(artic_, 3, col1);
        grid(dyn_, 4, col2);
        knobs(col3);
        grid(voice_, 4, c);
        grid(transpose_, 4, c);
        return;
    }
    grid(artic_, 3, c);
    grid(dyn_, 4, c);
    knobs(c.removeFromTop(kLabel + 84 + 18));
    c.removeFromTop(10);
    grid(voice_, 4, c);
    grid(transpose_, 4, c);
}

void Inspector::paintOverChildren(juce::Graphics& g)
{
    auto label = [&](const juce::String& text, const juce::Component& below) {
        hw::paint::silk(g, text, {static_cast<float>(below.getX()), static_cast<float>(below.getY() - kLabel), 200.0f, 12.0f}, 10.0f);
    };
    if (!artic_.empty()) label("Articulation", *artic_.front());
    if (!dyn_.empty()) label("Dynamics", *dyn_.front());
    hw::paint::silk(g, "Velocity",
                    {static_cast<float>(velocity_.getX()), static_cast<float>(velocity_.getY() - kLabel + 2), 84.0f, 12.0f}, 10.0f,
                    juce::Justification::centred);
    hw::paint::silk(g, "Humanize",
                    {static_cast<float>(humanize_.getX()), static_cast<float>(humanize_.getY() - kLabel + 2), 84.0f, 12.0f}, 10.0f,
                    juce::Justification::centred);
    if (!voice_.empty()) label("Voice", *voice_.front());
    if (!transpose_.empty()) label("Transpose", *transpose_.front());
}

}  // namespace vcc::desktop::composer_ui
