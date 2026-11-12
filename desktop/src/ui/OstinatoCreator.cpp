#include "ui/OstinatoCreator.h"

#include <algorithm>
#include <string>

#include "ui/Theme.h"

namespace vcc::desktop {

namespace {

constexpr int kTopPanel = 124;
constexpr int kBottomBar = 52;

juce::String utf8(const std::string& s) { return juce::String::fromUTF8(s.c_str()); }

const juce::Colour kCardColours[3] = {juce::Colour(0xff4f9de8), juce::Colour(0xffe8a33d), juce::Colour(0xff4fbf73)};

juce::String pretty(const std::string& id)
{
    juce::String s = juce::String(id).replaceCharacter('_', ' ');
    return s.substring(0, 1).toUpperCase() + s.substring(1);
}

}  // namespace

// --- Variant card -----------------------------------------------------------------------------

class OstinatoCreator::Card final : public juce::Component {
public:
    Card(OstinatoCreator& owner, int index) : owner_(owner), index_(index)
    {
        for (auto* b : {&audition_, &vary_, &send_}) addAndMakeVisible(*b);
        addAndMakeVisible(lock_);
        audition_.setTooltip("Loop this variant (nothing is written)");
        vary_.setTooltip("Re-roll one aspect of this variant");
        lock_.setTooltip("Keep this variant's rhythm when varying it");
        send_.setTooltip("Write this variant to the target track (one undo step)");
        send_.setColour(juce::TextButton::buttonColourId, colours::accent);
        audition_.onClick = [this] { owner_.audition(index_); };
        send_.onClick = [this] { owner_.send(index_); };
        vary_.onClick = [this] {
            juce::PopupMenu m;
            m.addSectionHeader("Vary one aspect");
            juce::Component::SafePointer<Card> self(this);
            auto item = [&m, self](const juce::String& name, ostinato::Aspect aspect) {
                m.addItem(name, [self, aspect] {
                    if (self != nullptr) self->owner_.vary(self->index_, aspect);
                });
            };
            item("Rhythm", ostinato::Aspect::Rhythm);
            item("Pitch", ostinato::Aspect::Pitch);
            item("Accents", ostinato::Aspect::Accents);
            item("Register", ostinato::Aspect::Register);
            m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&vary_));
        };
    }

    bool lock_rhythm() const { return lock_.getToggleState(); }
    juce::String note;  ///< "varied pitch" etc.

    void paint(juce::Graphics& g) override
    {
        const auto colour = kCardColours[index_];
        const bool playing = owner_.auditioning_ == index_;
        auto r = getLocalBounds().toFloat().reduced(1.0f);
        g.setColour(colours::panel);
        g.fillRoundedRectangle(r, 8.0f);
        g.setColour(playing ? colour : colours::outline);
        g.drawRoundedRectangle(r.reduced(0.5f), 8.0f, playing ? 2.0f : 1.0f);

        const ostinato::Variant* v = index_ < static_cast<int>(owner_.variants_.size())
                                         ? &owner_.variants_[static_cast<std::size_t>(index_)]
                                         : nullptr;
        // Badge and title.
        auto badge = juce::Rectangle<float>(14.0f, 12.0f, 34.0f, 34.0f);
        g.setColour(colour);
        g.fillEllipse(badge);
        g.setColour(colours::background);
        g.setFont(juce::FontOptions(18.0f, juce::Font::bold));
        g.drawText(juce::String::charToString(static_cast<juce::juce_wchar>('A' + index_)), badge, juce::Justification::centred);
        g.setColour(colours::text);
        g.setFont(juce::FontOptions(16.0f, juce::Font::bold));
        g.drawText("Variant " + (v ? utf8(v->id) : juce::String("-")), 58, 10, getWidth() - 70, 20,
                   juce::Justification::centredLeft, true);
        g.setColour(colours::text_dim);
        g.setFont(juce::FontOptions(12.0f));
        juce::String sub;
        if (v) {
            int lo = 127, hi = 0;
            for (const auto& n : v->notes) {
                lo = std::min(lo, n.pitch);
                hi = std::max(hi, n.pitch);
            }
            sub << static_cast<int>(v->notes.size()) << " notes";
            if (!v->notes.empty() && !owner_.options_.percussion)
                sub << "  -  " << dialogs::note_name(lo) << juce::String::fromUTF8("–") << dialogs::note_name(hi);
            if (owner_.options_.percussion) sub << "  -  drum map";
            if (note.isNotEmpty()) sub << "  -  " << note;
        }
        g.drawText(sub, 58, 30, getWidth() - 70, 18, juce::Justification::centredLeft, true);

        if (v)
            dialogs::draw_mini_roll(g, roll_.toFloat(), v->notes, v->start, v->end, owner_.options_.meter, colour);
        g.setColour(colours::text_dim.withAlpha(0.7f));
        g.setFont(juce::FontOptions(11.0f));
        if (v)
            g.drawText("seed " + juce::String(static_cast<juce::int64>(v->seed % 1000000007ULL)) +
                           (v->locks.rhythm ? juce::String("  -  rhythm locked") : juce::String()),
                       roll_.getX(), roll_.getBottom() + 2, roll_.getWidth(), 16, juce::Justification::centredRight);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced(14, 12);
        r.removeFromTop(44);
        auto buttons = r.removeFromBottom(28);
        r.removeFromBottom(26);
        roll_ = r;
        auto lock_row = getLocalBounds().reduced(14, 12).removeFromBottom(28 + 26).removeFromTop(24);
        lock_.setBounds(lock_row.removeFromLeft(120));
        const int w = (buttons.getWidth() - 12) / 3;
        audition_.setBounds(buttons.removeFromLeft(w));
        buttons.removeFromLeft(6);
        vary_.setBounds(buttons.removeFromLeft(w));
        buttons.removeFromLeft(6);
        send_.setBounds(buttons);
    }

private:
    OstinatoCreator& owner_;
    int index_;
    juce::Rectangle<int> roll_;
    juce::TextButton audition_{"Audition"}, vary_{"Vary..."}, send_{"Send to Track"};
    juce::ToggleButton lock_{"Lock rhythm"};
};

// --- Creator ----------------------------------------------------------------------------------

OstinatoCreator::OstinatoCreator(AppContext& c) : context_(c), target_(c, "New Ostinato Track")
{
    dialogs::style_caption(track_caption_, "Track");
    dialogs::style_caption(preset_caption_, "Preset");
    dialogs::style_caption(key_caption_, "Key");
    dialogs::style_caption(follow_caption_, "Follow");
    dialogs::style_caption(length_caption_, "Length");
    dialogs::style_caption(start_caption_, "Start bar");
    dialogs::style_caption(meter_caption_, "Meter");
    dialogs::style_caption(seed_caption_, "Seed");
    dialogs::style_caption(density_caption_, "Density");
    dialogs::style_caption(swing_caption_, "Swing");
    for (auto* l : {&track_caption_, &preset_caption_, &key_caption_, &follow_caption_, &length_caption_, &start_caption_,
                    &meter_caption_, &seed_caption_, &density_caption_, &swing_caption_})
        addAndMakeVisible(*l);

    addAndMakeVisible(target_);
    target_.setTooltip("Track the ostinato is for: its range limits the notes");
    target_.onChange = [this] { track_changed(); };

    const auto& presets = c.session.catalogs().ostinato_presets;
    preset_ids_ = presets.ids();
    for (std::size_t i = 0; i < preset_ids_.size(); ++i) {
        const auto* p = presets.find(preset_ids_[i]);
        preset_.addItem(p ? utf8(p->get_string("name", preset_ids_[i])) : pretty(preset_ids_[i]), static_cast<int>(i) + 1);
    }
    // A sensible first preset for the target track's family.
    std::string wanted;
    if (const auto* t = c.session.data().find_track(target_.track_id()))
        if (const auto* def = c.session.instrument_for(*t)) {
            if (def->unpitched) wanted = "syncopated_percussion";
            else if (def->family == "strings") wanted = "urgent_string_spiccato";
            else if (def->family == "keys") wanted = "rolling_piano_arpeggio";
            else if (def->family == "brass") wanted = "low_brass_ostinato";
            else if (def->family == "synth") wanted = "pulsing_synth";
            else if (def->family == "percussion") wanted = "mallet_pattern";
        }
    auto it = std::find(preset_ids_.begin(), preset_ids_.end(), wanted);
    preset_.setSelectedId(it != preset_ids_.end() ? static_cast<int>(it - preset_ids_.begin()) + 1 : 1,
                          juce::dontSendNotification);
    addAndMakeVisible(preset_);
    preset_.onChange = [this] { preset_changed(); };

    dialogs::fill_key_combos(tonic_, mode_, dialogs::project_key(c), false);
    tonic_.setTooltip("Key for the ostinato (the project key is not changed)");
    follow_.addItem("None (key only)", 1);
    const auto& progs = c.session.catalogs().progressions.all();
    for (std::size_t i = 0; i < progs.size(); ++i) follow_.addItem(utf8(progs[i].name), static_cast<int>(i) + 2);
    follow_.setSelectedId(1, juce::dontSendNotification);
    follow_.setTooltip("Follow a progression's chords (repeated over the length)");
    for (auto* cb : {&tonic_, &mode_, &follow_}) {
        addAndMakeVisible(*cb);
        cb->onChange = [this] { generate(); };
    }

    for (auto* s : {&length_, &start_}) {
        s->setSliderStyle(juce::Slider::IncDecButtons);
        s->setTextBoxStyle(juce::Slider::TextBoxLeft, false, 44, 24);
        s->setIncDecButtonsMode(juce::Slider::incDecButtonsNotDraggable);
        addAndMakeVisible(*s);
    }
    length_.setRange(1.0, 64.0, 1.0);
    length_.setValue(16.0, juce::dontSendNotification);
    length_.setTooltip("Length in bars");
    start_.setRange(1.0, 999.0, 1.0);
    start_.setValue(1.0, juce::dontSendNotification);
    start_.setTooltip("Bar the ostinato starts at");
    length_.onValueChange = [this] { generate(); };
    start_.onValueChange = [this] { generate(); };

    addAndMakeVisible(meter_);
    meter_.setFont(juce::FontOptions(14.0f, juce::Font::bold));
    meter_.setTooltip("The project's meter at the start bar");

    addAndMakeVisible(seed_);
    seed_.setInputRestrictions(12, "0123456789");
    seed_.setText("1", false);
    seed_.setIndents(8, 5);
    seed_.setTooltip("The same seed and settings always give the same variants");
    seed_.onReturnKey = [this] { generate(); };
    seed_.onFocusLost = [this] { generate(); };
    addAndMakeVisible(new_seed_);
    new_seed_.onClick = [this] { new_seed(); };

    for (auto* s : {&density_, &swing_}) {
        s->setSliderStyle(juce::Slider::LinearHorizontal);
        s->setTextBoxStyle(juce::Slider::TextBoxRight, false, 48, 22);
        s->setTextValueSuffix("%");
        s->setColour(juce::Slider::textBoxOutlineColourId, colours::outline);
        addAndMakeVisible(*s);
        s->onDragEnd = [this] { generate(); };
        s->onValueChange = [this, s] {
            if (!s->isMouseButtonDown()) generate();
        };
    }
    density_.setRange(5.0, 100.0, 1.0);
    swing_.setRange(0.0, 50.0, 1.0);
    density_.setTooltip("How many grid steps sound");
    swing_.setTooltip("Delay of every other grid step");

    for (auto* t : {&evolve_, &call_response_}) {
        addAndMakeVisible(*t);
        t->onClick = [this] { generate(); };
    }
    evolve_.setTooltip("Mutate one aspect every 4 bars");
    call_response_.setTooltip("Alternate bars answer with an inverted contour");

    addAndMakeVisible(generate_);
    generate_.setColour(juce::TextButton::buttonColourId, colours::accent);
    generate_.setTooltip("Generate variants A, B and C from the seed");
    generate_.onClick = [this] { generate(); };
    addAndMakeVisible(stop_);
    stop_.onClick = [this] { stop(); };

    addAndMakeVisible(hint_);
    hint_.setFont(juce::FontOptions(12.0f));
    hint_.setColour(juce::Label::textColourId, colours::text_dim);
    hint_.setText("Auditions loop on the placeholder voice and never touch the project. Send to Track writes one undoable region; "
                  "if notes are already there you choose Replace, Merge or New Lane.",
                  juce::dontSendNotification);
    addAndMakeVisible(info_);
    info_.setFont(juce::FontOptions(12.5f));
    info_.setColour(juce::Label::textColourId, colours::text_dim);

    for (int i = 0; i < 3; ++i) {
        cards_[static_cast<std::size_t>(i)] = std::make_unique<Card>(*this, i);
        addAndMakeVisible(*cards_[static_cast<std::size_t>(i)]);
    }

    preset_changed();  // sets density/swing from the preset and generates
    setSize(1080, 680);
}

OstinatoCreator::~OstinatoCreator() = default;

void OstinatoCreator::paint(juce::Graphics& g)
{
    g.fillAll(colours::background);
    g.setColour(colours::panel);
    g.fillRect(0, 0, getWidth(), kTopPanel);
    g.fillRect(0, getHeight() - kBottomBar, getWidth(), kBottomBar);
    g.setColour(colours::outline);
    g.drawHorizontalLine(kTopPanel - 1, 0.0f, static_cast<float>(getWidth()));
    g.drawHorizontalLine(getHeight() - kBottomBar, 0.0f, static_cast<float>(getWidth()));
}

void OstinatoCreator::resized()
{
    auto top = getLocalBounds().removeFromTop(kTopPanel).reduced(12, 12);
    auto row_of = [&top] {
        auto row = top.removeFromTop(26);
        top.removeFromTop(10);
        return row;
    };
    auto place = [](juce::Rectangle<int>& row, juce::Label& caption, int caption_w, juce::Component& c, int w) {
        caption.setBounds(row.removeFromLeft(caption_w));
        row.removeFromLeft(6);
        c.setBounds(row.removeFromLeft(w));
        row.removeFromLeft(16);
    };

    auto row1 = row_of();
    place(row1, track_caption_, 52, target_, 220);
    place(row1, preset_caption_, 46, preset_, 210);
    key_caption_.setBounds(row1.removeFromLeft(30));
    row1.removeFromLeft(6);
    tonic_.setBounds(row1.removeFromLeft(62));
    row1.removeFromLeft(4);
    mode_.setBounds(row1.removeFromLeft(128));
    row1.removeFromLeft(16);
    place(row1, follow_caption_, 46, follow_, std::max(120, row1.getWidth() - 52));

    auto row2 = row_of();
    place(row2, length_caption_, 52, length_, 96);
    place(row2, start_caption_, 58, start_, 96);
    place(row2, meter_caption_, 42, meter_, 44);
    seed_caption_.setBounds(row2.removeFromLeft(34));
    row2.removeFromLeft(6);
    seed_.setBounds(row2.removeFromLeft(90));
    row2.removeFromLeft(6);
    new_seed_.setBounds(row2.removeFromLeft(84));
    generate_.setBounds(row2.removeFromRight(110));

    auto row3 = row_of();
    place(row3, density_caption_, 52, density_, 220);
    place(row3, swing_caption_, 44, swing_, 200);
    evolve_.setBounds(row3.removeFromLeft(170));
    row3.removeFromLeft(8);
    call_response_.setBounds(row3.removeFromLeft(160));

    auto r = getLocalBounds().withTrimmedTop(kTopPanel);
    auto bottom = r.removeFromBottom(kBottomBar).reduced(12, 12);
    stop_.setBounds(bottom.removeFromLeft(80));
    bottom.removeFromLeft(14);
    hint_.setBounds(bottom);
    r = r.reduced(12, 8);
    info_.setBounds(r.removeFromTop(24));
    r.removeFromTop(6);
    const int gap = 12;
    const int w = (r.getWidth() - 2 * gap) / 3;
    for (auto& card : cards_) {
        card->setBounds(r.removeFromLeft(w));
        r.removeFromLeft(gap);
    }
}

std::string OstinatoCreator::preset_id() const
{
    const int i = preset_.getSelectedId() - 1;
    return i >= 0 && i < static_cast<int>(preset_ids_.size()) ? preset_ids_[static_cast<std::size_t>(i)] : std::string();
}

ostinato::OstinatoOptions OstinatoCreator::build_options()
{
    ostinato::OstinatoOptions o;
    if (const auto* p = context_.session.catalogs().ostinato_presets.find(preset_id())) o.apply_preset(*p);
    if (auto k = dialogs::key_from_combos(tonic_, mode_)) o.key = *k;
    o.meter = context_.session.data().meter;
    o.start_bar = static_cast<int>(start_.getValue());
    o.length_bars = static_cast<int>(length_.getValue());
    if (const auto* t = context_.session.data().find_track(target_.track_id())) {
        const auto [low, high] = context_.session.range_for(*t);
        o.range_low = low;
        o.range_high = high;
    }
    o.density = density_.getValue() / 100.0;
    o.swing = swing_.getValue() / 100.0;
    o.evolve = evolve_.getToggleState();
    o.call_response = call_response_.getToggleState();

    follow_note_.clear();
    const int f = follow_.getSelectedId() - 2;
    const auto& progs = context_.session.catalogs().progressions.all();
    if (f >= 0 && f < static_cast<int>(progs.size())) {
        const auto& prog = progs[static_cast<std::size_t>(f)];
        theory::ProgressionRenderOptions ro;
        ro.key = o.key;
        ro.start_bar = o.start_bar;
        ro.length_bars = o.length_bars;
        ro.expand = theory::ExpandMode::Repeat;
        std::string error;
        if (auto render = theory::render_progression(prog, o.meter, ro, &error)) {
            for (const auto& rc : render->chords) o.harmony.push_back({rc.start, rc.start + rc.duration, rc.chord});
            follow_note_ = "following " + utf8(prog.name) + " (" + utf8(prog.roman_text()) + ")";
        } else {
            follow_note_ = "cannot follow " + utf8(prog.name) + ": " + utf8(error);
        }
    }
    return o;
}

void OstinatoCreator::preset_changed()
{
    ostinato::OstinatoOptions o;
    if (const auto* p = context_.session.catalogs().ostinato_presets.find(preset_id())) o.apply_preset(*p);
    density_.setValue(juce::jlimit(5.0, 100.0, o.density * 100.0), juce::dontSendNotification);
    swing_.setValue(juce::jlimit(0.0, 50.0, o.swing * 100.0), juce::dontSendNotification);
    generate();
}

void OstinatoCreator::track_changed() { generate(); }

void OstinatoCreator::generate()
{
    options_ = build_options();
    std::uint64_t seed = 1;
    try {
        if (seed_.getText().isNotEmpty()) seed = std::stoull(seed_.getText().toStdString());
    } catch (...) {
        seed = 1;
    }
    ostinato::Generator generator(options_);
    variants_ = generator.generate_variants(seed, 3);
    for (std::size_t i = 0; i < variants_.size() && i < cards_.size(); ++i) {
        variants_[i].locks.rhythm = cards_[i]->lock_rhythm();
        cards_[i]->note.clear();
    }
    vary_count_ = {};
    update_info();
    for (auto& c : cards_) c->repaint();
    if (auditioning_ >= 0) audition(auditioning_);  // keep the loop going with the new notes
}

void OstinatoCreator::new_seed()
{
    seed_.setText(juce::String(juce::Random::getSystemRandom().nextInt(999999) + 1), false);
    generate();
}

void OstinatoCreator::vary(int index, ostinato::Aspect aspect)
{
    if (index < 0 || index >= static_cast<int>(variants_.size())) return;
    auto base = variants_[static_cast<std::size_t>(index)];
    base.locks.rhythm = cards_[static_cast<std::size_t>(index)]->lock_rhythm();
    const int n = ++vary_count_[static_cast<std::size_t>(index)];
    const std::string id = std::string(1, static_cast<char>('A' + index)) + std::to_string(n + 1);
    ostinato::Generator generator(options_);
    variants_[static_cast<std::size_t>(index)] = generator.vary(base, aspect, id);
    static const char* names[] = {"rhythm", "pitch", "accents", "register"};
    cards_[static_cast<std::size_t>(index)]->note = juce::String("varied ") + names[static_cast<int>(aspect)];
    cards_[static_cast<std::size_t>(index)]->repaint();
    if (auditioning_ == index) audition(index);
}

void OstinatoCreator::audition(int index)
{
    if (index < 0 || index >= static_cast<int>(variants_.size())) return;
    const auto& v = variants_[static_cast<std::size_t>(index)];
    auto [family, program] = dialogs::preview_voice(context_, target_.track_id());
    auto notes = dialogs::shifted(v.notes, v.start);
    if (options_.percussion) {
        family = "percussion";
        program = -1;
        for (auto& n : notes) n.channel = 9;
    }
    context_.engine.preview(notes, family, program, dialogs::project_bpm(context_), true, v.end - v.start);
    auditioning_ = index;
    for (auto& c : cards_) c->repaint();
    context_.status("Looping variant " + utf8(v.id) + " - press Stop to end the audition");
}

void OstinatoCreator::stop()
{
    context_.engine.stop_preview();
    auditioning_ = -1;
    for (auto& c : cards_) c->repaint();
}

void OstinatoCreator::send(int index)
{
    if (index < 0 || index >= static_cast<int>(variants_.size())) return;
    std::string track = target_.track_id();
    if (track.empty()) {
        std::string instrument = "keys.piano";
        if (options_.percussion)
            for (const auto& i : context_.session.catalogs().instruments.all())
                if (i.family == "percussion" && i.unpitched) {
                    instrument = i.id;
                    break;
                }
        track = context_.session.add_track("Ostinato", instrument);
        if (track.empty()) {
            context_.status("Could not create an ostinato track");
            return;
        }
        target_.refresh();
        target_.select(track);
    }
    const auto& v = variants_[static_cast<std::size_t>(index)];
    const juce::String what = "Ostinato " + utf8(v.id) + " (" + juce::String(options_.length_bars) + " bars from bar " +
                              juce::String(options_.start_bar) + ")";
    send_with(track, v, preset_id(), what, project::CollisionPolicy::Ask);
}

void OstinatoCreator::send_with(const std::string& track_id, const ostinato::Variant& variant, const std::string& preset,
                                const juce::String& what, project::CollisionPolicy policy)
{
    const auto result = context_.session.send_ostinato(track_id, variant, preset, policy);
    if (result.needs_decision) {
        juce::Component::SafePointer<OstinatoCreator> self(this);
        dialogs::ask_collision(what, [self, track_id, variant, preset, what](project::CollisionPolicy decided) {
            if (self != nullptr) self->send_with(track_id, variant, preset, what, decided);
        });
        return;
    }
    dialogs::report_send(context_, result, what);
}

void OstinatoCreator::update_info()
{
    const auto sig = options_.meter.at_bar(options_.start_bar);
    meter_.setText(juce::String(sig.numerator) + "/" + juce::String(sig.denominator), juce::dontSendNotification);
    juce::String s;
    if (options_.percussion) s << "Drum mode (unpitched drum map)";
    else
        s << "Range " << dialogs::note_name(options_.range_low) << juce::String::fromUTF8("–")
          << dialogs::note_name(options_.range_high) << "  -  " << utf8(options_.key.tonic.display_name()) << " "
          << juce::String(theory::display_name(options_.key.mode)).toLowerCase();
    s << "  -  " << options_.length_bars << " bars from bar " << options_.start_bar;
    if (!options_.articulation.empty()) s << "  -  " << utf8(options_.articulation);
    if (follow_note_.isNotEmpty()) s << "  -  " << follow_note_;
    info_.setText(s, juce::dontSendNotification);
    info_.setColour(juce::Label::textColourId,
                    follow_note_.startsWith("cannot") ? colours::accent_warm : colours::text_dim);
}

}  // namespace vcc::desktop
