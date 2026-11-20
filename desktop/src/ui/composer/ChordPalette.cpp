#include "ui/composer/ChordPalette.h"

#include <algorithm>

#include "ui/hw/HwTheme.h"

namespace vcc::desktop::composer_ui {

namespace {

const char* const kLetters = "CDEFGAB";
const char* const kAlterations[] = {"b5", "#5", "b9", "#9", "#11", "b13", "no3", "no5"};
const char* const kSlashNames[] = {"C", "C#", "Db", "D", "D#", "Eb", "E", "F", "F#", "Gb", "G", "G#", "Ab", "A", "A#", "Bb", "B"};

juce::String quality_label(const theory::ChordQuality& q)
{
    static const std::map<std::string, const char*> names = {
        {"maj", "Major"}, {"m", "Minor"}, {"dim", "Diminished"}, {"aug", "Augmented"}, {"sus2", "Suspended 2"},
        {"sus4", "Suspended 4"}, {"5", "Power chord (5)"}, {"6", "Added 6"}, {"m6", "Minor added 6"}, {"69", "6/9"},
        {"7", "Dominant 7"}, {"maj7", "Major 7"}, {"m7", "Minor 7"}, {"m7b5", "Half-diminished 7"},
        {"dim7", "Diminished 7"}, {"mmaj7", "Minor-major 7"}, {"aug7", "Augmented 7"}, {"augmaj7", "Augmented major 7"},
        {"7sus4", "7 suspended 4"}, {"add9", "Add 9 (no 7th)"}, {"madd9", "Minor add 9 (no 7th)"},
        {"9", "Dominant 9 (stacked)"}, {"maj9", "Major 9"}, {"m9", "Minor 9"}, {"7b9", "7 flat 9"}, {"7#9", "7 sharp 9"},
        {"11", "Dominant 11"}, {"m11", "Minor 11"}, {"maj7#11", "Major 7 sharp 11"}, {"13", "Dominant 13"},
        {"maj13", "Major 13"}, {"m13", "Minor 13"}, {"7alt", "Altered dominant"},
    };
    const auto it = names.find(q.id);
    const juce::String name = it != names.end() ? juce::String(it->second) : juce::String(q.id);
    return q.symbol.empty() ? name : name + "   (" + juce::String(q.symbol) + ")";
}

void style_combo(juce::ComboBox& c, const juce::String& id)
{
    c.setComponentID(id);
    c.setColour(juce::ComboBox::backgroundColourId, hw::tok::panel_bottom);
    c.setColour(juce::ComboBox::outlineColourId, hw::tok::edge);
    c.setColour(juce::ComboBox::textColourId, hw::tok::text_bright);
    c.setColour(juce::ComboBox::arrowColourId, hw::tok::text);
}

int steps_for(int semitones)
{
    static const int kSteps[12] = {0, 1, 1, 2, 2, 3, 3, 4, 5, 5, 6, 6};
    const int a = std::abs(semitones);
    const int s = kSteps[a % 12] + 7 * (a / 12);
    return semitones < 0 ? -s : s;
}

}  // namespace

/// The chord's notes on a staff: written pitch for a transposing one, each
/// keeping the chord's spelling.
std::vector<notation::Pitch> written_chord(const std::vector<theory::VoicedTone>& voiced, const notation::Staff& staff,
                                          int trans)
{
    // Written = sounding less the instrument's transposition and the track's Trans.
    const int t = staff.transposition_semitones + trans;
    const int total = staff.total_transposition() + trans;
    std::vector<notation::Pitch> out;
    for (const auto& v : voiced) {
        theory::VoicedTone w{t != 0 ? v.pitch.transposed(steps_for(-t), -t) : v.pitch, v.midi - total};
        notation::Pitch p;
        p.step = static_cast<notation::Step>(((w.pitch.letter % 7) + 7) % 7);
        p.alteration = static_cast<std::int8_t>(w.pitch.alteration);
        p.octave = static_cast<std::int8_t>(w.octave());
        out.push_back(p);
    }
    return out;
}


std::optional<theory::Chord> chord_for(const ChordModeState& s, std::string* problem)
{
    auto fail = [&](const std::string& why) -> std::optional<theory::Chord> {
        if (problem) *problem = why;
        return std::nullopt;
    };
    // The type picks the quality: the key's own chord on that degree, or an outright one.
    std::string quality_id = s.quality;
    if (s.type == "triad" || s.type == "seventh") {
        const auto chords = theory::diatonic_chords(s.key, s.type == "seventh" ? 4 : 3);
        const int degree = ((s.letter - s.key.tonic.letter) % 7 + 7) % 7;
        if (degree < static_cast<int>(chords.size())) quality_id = chords[static_cast<std::size_t>(degree)].quality.id;
        else quality_id = s.type == "seventh" ? "7" : "maj";
    } else if (s.type != "custom" && !s.type.empty()) {
        quality_id = s.type;
    }
    const auto* q = theory::find_quality(quality_id);
    if (!q) return fail("Choose a chord quality");
    std::string text(1, kLetters[std::clamp(s.letter, 0, 6)]);
    if (s.accidental < 0) text += "b";
    if (s.accidental > 0) text += "#";
    text += q->symbol;
    if (!s.alterations.empty()) {
        std::string mods;
        for (const char* a : kAlterations)
            if (s.alterations.count(a)) mods += (mods.empty() ? "" : ",") + std::string(a);
        text += "(" + mods + ")";
    }
    auto chord = theory::parse_chord_symbol(text);
    if (!chord) return fail("Those alterations contradict the chord (" + text + ")");
    if (!s.slash_bass.empty()) {
        const auto bass = theory::SpelledPitch::parse(s.slash_bass);
        if (!bass) return fail("Choose a bass note");
        const auto tones = chord->tones();
        const auto pos = std::find(tones.begin(), tones.end(), *bass);
        if (pos != tones.end() && pos - tones.begin() <= 3) chord->inversion = static_cast<int>(pos - tones.begin());
        else chord->bass = *bass;
    } else {
        std::size_t core = 0;
        for (const auto& t : chord->intervals()) core += t.degree <= 7 ? 1 : 0;
        if (s.inversion >= static_cast<int>(core)) return fail("This chord has no such inversion");
        chord->inversion = s.inversion;
    }
    if (problem) problem->clear();
    return chord;
}

const std::vector<std::pair<std::string, const char*>>& chord_types()
{
    static const std::vector<std::pair<std::string, const char*>> list{
        {"triad", "Triad"}, {"seventh", "7th"}, {"maj7", "Maj7"}, {"add9", "Add9"},
        {"sus2", "Sus2"},   {"sus4", "Sus4"},   {"dim", "Dim"},   {"aug", "Aug"}};
    return list;
}

ChordForStaves voice_chord_for_staves(const theory::Chord& chord, const ChordModeState& state, bool grand_staff)
{
    const auto style = state.open ? theory::VoicingStyle::Open : theory::VoicingStyle::Closed;
    ChordForStaves out;
    auto doubled_up = [](std::vector<theory::VoicedTone> tones) {
        // Over two octaves: the same chord again an octave higher.
        const auto n = tones.size();
        for (std::size_t i = 0; i < n; ++i) {
            auto t = tones[i];
            t.midi += 12;
            if (t.midi <= 127) tones.push_back(t);
        }
        std::sort(tones.begin(), tones.end(), [](const auto& a, const auto& b) { return a.midi < b.midi; });
        tones.erase(std::unique(tones.begin(), tones.end(), [](const auto& a, const auto& b) { return a.midi == b.midi; }),
                    tones.end());
        return tones;
    };
    if (!grand_staff) {
        auto tones = theory::voice_for_entry(chord, state.octave, style);
        // An open voicing already reaches across two octaves; a close one is doubled up.
        out.upper = state.span >= 2 && !state.open ? doubled_up(tones) : tones;
        return out;
    }
    // Two hands: the right hand the chord (with its inversion) from middle C
    // up; the left hand the root below - with its fifth and octave over two octaves.
    auto right = theory::voice_for_entry(chord, std::max(4, state.octave), style);
    out.upper = state.span >= 2 && !state.open ? doubled_up(right) : right;
    // The left hand plays the chord's bass: the root, or the inverted tone
    // or slash bass when there is one.
    const bool root_position = chord.inversion == 0 && !chord.bass.has_value();
    const auto rooted = theory::voice_for_entry(chord, state.span >= 2 ? 2 : 3, theory::VoicingStyle::Closed);
    if (!rooted.empty()) {
        out.lower.push_back(rooted.front());  // the bass
        if (state.span >= 2) {
            if (root_position)
                for (const auto& t : rooted)
                    if (((t.midi - rooted.front().midi) % 12 + 12) % 12 == 7) {
                        out.lower.push_back(t);  // the fifth
                        break;
                    }
            auto octave = rooted.front();
            octave.midi += 12;
            out.lower.push_back(octave);
        }
    }
    // Nothing of the left hand above middle C, nothing of the right below it.
    out.lower.erase(std::remove_if(out.lower.begin(), out.lower.end(), [](const auto& t) { return t.midi >= 60; }),
                    out.lower.end());
    return out;
}

ChordPalette::ChordPalette(ChordModeState& state) : hw::Panel("Chord mode", false), state_(state)
{
    setComponentID("chord_palette");
    for (int i = 0; i < 7; ++i) {
        roots_.push_back(std::make_unique<hw::Keycap>(juce::String::charToString(kLetters[i]), false));
        auto& k = *roots_.back();
        k.setComponentID("chord_root_" + juce::String::charToString(kLetters[i]));
        k.onClick = [this, i] {
            // A note of the key's scale: F# in D major, Bb in F.
            state_.letter = i;
            state_.accidental = scale_accidental_[static_cast<std::size_t>(i)];
            changed();
        };
        addAndMakeVisible(k);
    }
    borrow_from_.setComponentID("chord_borrow_from");
    borrow_from_.setTitle("Borrow from");
    borrow_from_.setTooltip("Where the borrowed chords come from: the parallel key (with the Neapolitan), or another mode on "
                            "the same tonic");
    borrow_from_.onChange = [this] {
        if (refreshing_) return;
        state_.borrow_source = std::max(0, borrow_from_.getSelectedItemIndex());
        changed();
    };
    addAndMakeVisible(borrow_from_);
    const char* acc[] = {"\xE2\x99\xAD", "\xE2\x99\xAE", "\xE2\x99\xAF"};
    for (int i = 0; i < 3; ++i) {
        accidentals_.push_back(std::make_unique<hw::Keycap>(juce::String::fromUTF8(acc[i]), false));
        auto& k = *accidentals_.back();
        k.setComponentID("chord_accidental_" + juce::String(i - 1));
        k.onClick = [this, i] {
            state_.accidental = i - 1;
            changed();
        };
        addAndMakeVisible(k);
    }
    for (const char* a : kAlterations) {
        juce::String legend = juce::String(a).replace("b", juce::String::fromUTF8("\xE2\x99\xAD")).replace("#", juce::String::fromUTF8("\xE2\x99\xAF"));
        if (juce::String(a).startsWith("no")) legend = juce::String(a);
        alterations_.push_back(std::make_unique<hw::Keycap>(legend, false));
        auto& k = *alterations_.back();
        k.setComponentID("chord_alt_" + juce::String(a));
        k.set_led_dot(true);
        const std::string name = a;
        k.onClick = [this, name] {
            if (state_.alterations.count(name)) state_.alterations.erase(name);
            else state_.alterations.insert(name);
            changed();
        };
        addAndMakeVisible(k);
    }
    style_combo(quality_, "chord_quality");
    int qid = 1;
    for (const auto& q : theory::chord_qualities()) quality_.addItem(quality_label(q), qid++);
    quality_.onChange = [this] {
        if (refreshing_) return;
        const int i = quality_.getSelectedItemIndex();
        const auto& all = theory::chord_qualities();
        if (i >= 0 && i < static_cast<int>(all.size())) state_.quality = all[static_cast<std::size_t>(i)].id;
        state_.type = "custom";
        changed();
    };
    style_combo(bass_, "chord_bass");
    bass_.onChange = [this] {
        if (refreshing_) return;
        const int id = bass_.getSelectedId();
        if (id >= 1 && id <= 4) {
            state_.inversion = id - 1;
            state_.slash_bass.clear();
        } else if (id >= 10) {
            state_.inversion = 0;
            state_.slash_bass = kSlashNames[id - 10];
        }
        changed();
    };
    style_combo(voicing_, "chord_voicing");
    voicing_.addItem("Close position", 1);
    voicing_.addItem("Open position", 2);
    voicing_.addItem("Drop 2", 3);
    voicing_.addItem("Spread (bass an octave down)", 4);
    voicing_.onChange = [this] {
        if (refreshing_) return;
        static const theory::VoicingStyle styles[] = {theory::VoicingStyle::Closed, theory::VoicingStyle::Open,
                                                      theory::VoicingStyle::Drop2, theory::VoicingStyle::Spread};
        const int i = voicing_.getSelectedItemIndex();
        if (i >= 0 && i < 4) state_.style = styles[i];
        changed();
    };
    style_combo(then_, "chord_then");
    then_.addItem("After chord", 1);
    then_.addItem("Next beat", 2);
    then_.addItem("Next bar", 3);
    then_.setTooltip("Where the cursor goes after an entry");
    then_.onChange = [this] {
        if (refreshing_) return;
        state_.then = std::max(0, then_.getSelectedItemIndex());
        changed();
    };
    // Send to: this track, or the chord spread over a section, a note per player.
    style_combo(send_, "chord_send_to");
    send_.addItem("Send to: this track", 1);
    send_.addItem("Send to: Strings (open, 5 parts)", 2);
    send_.addItem("Send to: Woodwinds (4 parts)", 3);
    send_.addItem("Send to: Brass (4 parts)", 4);
    send_.addItem("Send to: Choir (SATB)", 5);
    send_.setTooltip("Where Insert writes the chord: on this track, or spread over a section - a note each, top down "
                     "(single-line instruments never get chords)");
    send_.onChange = [this] {
        if (refreshing_) return;
        state_.send_to = std::max(0, send_.getSelectedItemIndex());
        changed();
    };
    addAndMakeVisible(send_);
    // The quality, bass and voicing lists stay behind the keys (scripts and
    // the checks still set them); Then stays a list.
    for (auto* c : {&quality_, &bass_, &voicing_}) addChildComponent(*c);
    addAndMakeVisible(then_);

    // Type: the key's triad or seventh on the root, or a chord outright.
    for (const auto& [id, legend] : chord_types()) {
        types_.push_back(std::make_unique<hw::Keycap>(legend, false));
        auto& k = *types_.back();
        k.setComponentID("chord_type_" + juce::String(id));
        const std::string type = id;
        k.setTooltip(type == "triad"     ? juce::String("The key's own triad on the root (major, minor or diminished)")
                     : type == "seventh" ? juce::String("The key's own seventh chord on the root")
                                         : "A " + juce::String(legend) + " chord on the root");
        k.onClick = [this, type] {
            state_.type = type;
            changed();
        };
        addAndMakeVisible(k);
    }
    // Inversion: root position, 1st, 2nd, 3rd.
    const char* inv[] = {"Root", "1st", "2nd", "3rd"};
    for (int i = 0; i < 4; ++i) {
        inversions_.push_back(std::make_unique<hw::Keycap>(inv[i], false));
        auto& k = *inversions_.back();
        k.setComponentID("chord_inversion_" + juce::String(i));
        k.onClick = [this, i] {
            state_.inversion = i;
            state_.slash_bass.clear();
            changed();
        };
        addAndMakeVisible(k);
    }
    // Voicing: close or open; spread over one octave or two.
    close_voicing_.setComponentID("chord_voicing_close");
    open_.setComponentID("chord_voicing_open");
    span1_.setComponentID("chord_span_1");
    span2_.setComponentID("chord_span_2");
    close_voicing_.setTooltip("Close: the chord's notes as near together as they go");
    open_.setTooltip("Open: alternate notes raised an octave - a wider, orchestral sound");
    span1_.setTooltip("Within about an octave");
    span2_.setTooltip("Spread over two octaves (on piano: both hands, the left hand in the bass clef)");
    close_voicing_.onClick = [this] {
        state_.open = false;
        state_.style = theory::VoicingStyle::Closed;
        changed();
    };
    open_.onClick = [this] {
        state_.open = true;
        state_.style = theory::VoicingStyle::Open;
        changed();
    };
    span1_.onClick = [this] {
        state_.span = 1;
        changed();
    };
    span2_.onClick = [this] {
        state_.span = 2;
        changed();
    };
    for (auto* k : {&close_voicing_, &open_, &span1_, &span2_}) addAndMakeVisible(*k);

    octave_down_.setComponentID("chord_octave_down");
    octave_up_.setComponentID("chord_octave_up");
    octave_down_.onClick = [this] {
        state_.octave = std::max(0, state_.octave - 1);
        changed();
    };
    octave_up_.onClick = [this] {
        state_.octave = std::min(8, state_.octave + 1);
        changed();
    };
    symbol_.set_led_dot(true);
    symbol_.setComponentID("chord_symbol");
    symbol_.setTooltip("Also write the chord symbol on the ruler's Chords row");
    symbol_.onClick = [this] {
        state_.write_symbol = symbol_.getToggleState();
        changed();
    };
    close_.setComponentID("chord_close");
    close_.setTooltip("Close Chord Mode");
    close_.onClick = [this] {
        if (on_close) on_close();
    };
    insert_.setComponentID("chord_insert");
    insert_.setTooltip("Enter the chord at the cursor (or click a staff while Chord Mode is on)");
    insert_.onClick = [this] {
        if (on_insert) on_insert(false);
    };
    replace_.setComponentID("chord_replace");
    replace_.setTooltip("Replace the selected notes with the chord, at their place and value");
    replace_.onClick = [this] {
        if (on_insert) on_insert(true);
    };
    audition_.setComponentID("chord_audition");
    audition_.setTooltip("Hear the chord on the selected track");
    audition_.onClick = [this] {
        if (on_audition) on_audition();
    };
    for (auto* c : std::initializer_list<juce::Component*>{&octave_down_, &octave_up_, &symbol_, &close_, &insert_, &replace_,
                                                            &audition_, &octave_lcd_, &name_lcd_})
        addAndMakeVisible(*c);
    name_lcd_.setComponentID("chord_name");
    octave_lcd_.set_justification(juce::Justification::centred);
    constrainer_.setMinimumOnscreenAmounts(40, 80, 40, 80);
    setSize(kWidth, kHeight);
    refresh();
}

hw::Keycap* ChordPalette::type_key(const std::string& type)
{
    for (std::size_t i = 0; i < types_.size(); ++i)
        if (chord_types()[i].first == type) return types_[i].get();
    return nullptr;
}

hw::Keycap* ChordPalette::alteration_key(const std::string& name)
{
    for (std::size_t i = 0; i < alterations_.size(); ++i)
        if (name == kAlterations[i]) return alterations_[i].get();
    return nullptr;
}

void ChordPalette::fill_bass_box()
{
    bass_.clear(juce::dontSendNotification);
    std::string problem;
    ChordModeState plain = state_;
    plain.slash_bass.clear();
    plain.inversion = 0;
    const auto chord = chord_for(plain);
    const auto tones = chord ? chord->tones() : std::vector<theory::SpelledPitch>{};
    std::size_t core = 0;
    if (chord)
        for (const auto& t : chord->intervals()) core += t.degree <= 7 ? 1 : 0;
    static const char* inv[] = {"Root position", "1st inversion", "2nd inversion", "3rd inversion"};
    for (std::size_t i = 0; i < 4; ++i) {
        juce::String label = inv[i];
        if (i < tones.size()) label << " (" << juce::String::fromUTF8(tones[i].display_name().c_str()) << ")";
        bass_.addItem(label, static_cast<int>(i) + 1);
        bass_.setItemEnabled(static_cast<int>(i) + 1, i < std::max<std::size_t>(core, 1));
    }
    bass_.addSeparator();
    for (int i = 0; i < 17; ++i) bass_.addItem("Over " + juce::String(kSlashNames[i]), 10 + i);
    int id = state_.inversion + 1;
    for (int i = 0; i < 17; ++i)
        if (state_.slash_bass == kSlashNames[i]) id = 10 + i;
    bass_.setSelectedId(id, juce::dontSendNotification);
}

void ChordPalette::fill_scale()
{
    const auto key = state_.key;
    const int size = state_.type == "seventh" ? 4 : 3;
    const bool same = shown_source_ == state_.borrow_source && shown_size_ == size && shown_key_.tonic == key.tonic &&
                      shown_key_.mode == key.mode;
    if (same) return;
    shown_key_ = key;
    shown_source_ = state_.borrow_source;
    shown_size_ = size;
    // The scale's notes on the root keys, its numerals over them.
    const auto chords = theory::diatonic_chords(key, size);
    const auto scale = key.scale();
    for (int l = 0; l < 7; ++l) {
        scale_accidental_[static_cast<std::size_t>(l)] = 0;
        numerals_[static_cast<std::size_t>(l)].clear();
    }
    for (std::size_t d = 0; d < scale.size() && d < 7; ++d) {
        const auto& p = scale[d];
        const int l = p.letter;
        if (l < 0 || l > 6) continue;
        scale_accidental_[static_cast<std::size_t>(l)] = p.alteration;
        roots_[static_cast<std::size_t>(l)]->setButtonText(juce::String::fromUTF8(p.display_name().c_str()));
        if (d < chords.size())
            numerals_[static_cast<std::size_t>(l)] =
                juce::String::fromUTF8(theory::roman_label(static_cast<int>(d) + 1, chords[d].quality).c_str());
        roots_[static_cast<std::size_t>(l)]->setTooltip(numerals_[static_cast<std::size_t>(l)] + " in " +
                                                       juce::String::fromUTF8(key.name().c_str()));
    }
    // Borrow from: as in the Circle of Fifths and the Chord Explorer.
    const auto ref = theory::key_reference(key);
    const bool minor_home = theory::is_minor_family(ref.key.mode);
    borrow_from_.clear(juce::dontSendNotification);
    int n = 0;
    for (auto src : theory::borrow_sources())
        borrow_from_.addItem("Borrow from " + juce::String(theory::borrow_source_name(src, minor_home)) +
                                 (src == theory::BorrowSource::Parallel ? " + Neapolitan" : ""),
                             ++n);
    const auto& sources = theory::borrow_sources();
    const int src = std::clamp(state_.borrow_source, 0, static_cast<int>(sources.size()) - 1);
    borrow_from_.setSelectedItemIndex(src, juce::dontSendNotification);
    for (auto& k : borrowed_keys_) removeChildComponent(k.get());
    borrowed_keys_.clear();
    borrowed_.clear();
    for (const auto& b : theory::borrowed_chords(ref, sources[static_cast<std::size_t>(src)], size > 3)) {
        borrowed_.push_back(b.chord);
        auto k = std::make_unique<hw::Keycap>(juce::String::fromUTF8(b.roman.c_str()), false);
        k->set_legend_size(10.5f);
        k->setComponentID("chord_borrowed_" + juce::String(static_cast<int>(borrowed_keys_.size())));
        k->setTooltip(juce::String::fromUTF8(b.chord.display_name().c_str()) + ", borrowed from " +
                      juce::String::fromUTF8(b.source.c_str()) + (b.name.empty() ? juce::String() : " (" + juce::String(b.name) + ")"));
        const auto chord = b.chord;
        k->onClick = [this, chord] {
            // The borrowed chord outright: its root and its quality.
            state_.letter = chord.root.letter;
            state_.accidental = chord.root.alteration;
            state_.type = "custom";
            state_.quality = chord.quality.id;
            changed();
        };
        addAndMakeVisible(*k);
        borrowed_keys_.push_back(std::move(k));
    }
    resized();
}

void ChordPalette::refresh()
{
    refreshing_ = true;
    fill_scale();
    for (int i = 0; i < 7; ++i)
        roots_[static_cast<std::size_t>(i)]->setToggleState(
            state_.letter == i && state_.accidental == scale_accidental_[static_cast<std::size_t>(i)], juce::dontSendNotification);
    for (std::size_t i = 0; i < borrowed_keys_.size() && i < borrowed_.size(); ++i)
        borrowed_keys_[i]->setToggleState(state_.type == "custom" && borrowed_[i].root.letter == state_.letter &&
                                              borrowed_[i].root.alteration == state_.accidental &&
                                              borrowed_[i].quality.id == state_.quality,
                                          juce::dontSendNotification);
    for (int i = 0; i < 3; ++i)
        accidentals_[static_cast<std::size_t>(i)]->setToggleState(state_.accidental == i - 1, juce::dontSendNotification);
    for (std::size_t i = 0; i < alterations_.size(); ++i)
        alterations_[i]->setToggleState(state_.alterations.count(kAlterations[i]) > 0, juce::dontSendNotification);
    const auto& all = theory::chord_qualities();
    for (std::size_t i = 0; i < all.size(); ++i)
        if (all[i].id == state_.quality) quality_.setSelectedItemIndex(static_cast<int>(i), juce::dontSendNotification);
    fill_bass_box();
    static const theory::VoicingStyle styles[] = {theory::VoicingStyle::Closed, theory::VoicingStyle::Open,
                                                  theory::VoicingStyle::Drop2, theory::VoicingStyle::Spread};
    for (int i = 0; i < 4; ++i)
        if (styles[i] == state_.style) voicing_.setSelectedItemIndex(i, juce::dontSendNotification);
    then_.setSelectedItemIndex(std::clamp(state_.then, 0, 2), juce::dontSendNotification);
    send_.setSelectedItemIndex(std::clamp(state_.send_to, 0, 4), juce::dontSendNotification);
    for (std::size_t i = 0; i < types_.size(); ++i)
        types_[i]->setToggleState(chord_types()[i].first == state_.type, juce::dontSendNotification);
    {
        ChordModeState plain = state_;
        plain.inversion = 0;
        plain.slash_bass.clear();
        std::size_t core = 0;
        if (const auto c = chord_for(plain))
            for (const auto& t : c->intervals()) core += t.degree <= 7 ? 1 : 0;
        for (std::size_t i = 0; i < inversions_.size(); ++i) {
            inversions_[i]->setToggleState(state_.slash_bass.empty() && state_.inversion == static_cast<int>(i),
                                           juce::dontSendNotification);
            inversions_[i]->setEnabled(i < std::max<std::size_t>(core, 1));
        }
    }
    close_voicing_.setToggleState(!state_.open, juce::dontSendNotification);
    open_.setToggleState(state_.open, juce::dontSendNotification);
    span1_.setToggleState(state_.span < 2, juce::dontSendNotification);
    span2_.setToggleState(state_.span >= 2, juce::dontSendNotification);
    symbol_.setToggleState(state_.write_symbol, juce::dontSendNotification);
    octave_lcd_.set_text("BASS OCT " + juce::String(state_.octave));
    std::string problem;
    const auto chord = chord_for(state_, &problem);
    name_lcd_.set_text(chord ? juce::String::fromUTF8(chord->display_name().c_str()) : juce::String("--"));
    insert_.setEnabled(chord.has_value());
    replace_.setEnabled(chord.has_value());
    audition_.setEnabled(chord.has_value());
    if (!chord) {
        notes_.clear();
        warning_ = problem;
    }
    refreshing_ = false;
    // The owner previews the notes on the staff being written.
    if (chord && on_changed) on_changed();
    repaint();
}

void ChordPalette::changed() { refresh(); }

void ChordPalette::set_preview(const juce::String& notes, const juce::String& warning)
{
    std::string problem;
    if (!chord_for(state_, &problem)) return;  // the problem stays shown
    notes_ = notes;
    warning_ = warning;
    repaint();
}

void ChordPalette::paint(juce::Graphics& g)
{
    hw::Panel::paint(g);
    auto r = content().reduced(10, 4);
    auto silk = [&](const juce::String& t, int y) {
        hw::paint::silk(g, t, juce::Rectangle<float>(static_cast<float>(r.getX()), static_cast<float>(y), 300.0f, 12.0f), 9.5f);
    };
    silk("Root: the key's scale (or click a staff)", r.getY());
    // Each scale note's numeral over its key.
    for (int l = 0; l < 7; ++l) {
        const auto kb = roots_[static_cast<std::size_t>(l)]->getBounds();
        hw::paint::silk(g, numerals_[static_cast<std::size_t>(l)],
                        juce::Rectangle<float>(static_cast<float>(kb.getX()), static_cast<float>(kb.getY() - 12),
                                               static_cast<float>(kb.getWidth()), 11.0f),
                        9.0f, juce::Justification::centred);
    }
    silk("Borrowed chords", r.getY() + 54);
    silk("Type", r.getY() + 124);
    silk("Alter / omit", r.getY() + 170);
    silk("Inversion", r.getY() + 216);
    silk("Voicing and spread", r.getY() + 262);
    // The notes and any warning, under the controls.
    const auto preview = juce::Rectangle<float>(static_cast<float>(r.getX()), static_cast<float>(getHeight() - 82),
                                                static_cast<float>(r.getWidth()), 34.0f);
    g.setFont(hw::fonts::ui(11.5f, false));
    g.setColour(hw::tok::text_bright);
    g.drawText(notes_, preview.withHeight(17.0f), juce::Justification::centredLeft, true);
    g.setColour(juce::Colour(0xffe0a040));
    g.drawText(warning_, preview.withTrimmedTop(17.0f), juce::Justification::centredLeft, true);
}

void ChordPalette::resized()
{
    close_.setBounds(getWidth() - 34, 6, 24, 20);
    auto r = content().reduced(10, 4);
    auto line = [&](int y, int h) { return juce::Rectangle<int>(r.getX(), r.getY() + y, r.getWidth(), h); };
    // Root: the key's seven notes in scale order from the tonic, then the
    // three accidentals for a chromatic root.
    auto row = line(24, 26);
    {
        auto acc = row.removeFromRight(96);
        for (auto& k : accidentals_) k->setBounds(acc.removeFromLeft(32).withTrimmedRight(3));
        row.removeFromRight(8);
        const int kw = row.getWidth() / 7;
        const int tonic = std::clamp(shown_key_.tonic.letter, 0, 6);
        for (int d = 0; d < 7; ++d) roots_[static_cast<std::size_t>((tonic + d) % 7)]->setBounds(row.removeFromLeft(kw).withTrimmedRight(3));
    }
    // Borrowed chords and where they come from.
    row = line(68, 24);
    borrow_from_.setBounds(row.removeFromRight(220));
    row = line(94, 24);
    if (!borrowed_keys_.empty()) {
        const int bw = std::min(48, row.getWidth() / static_cast<int>(borrowed_keys_.size()));
        for (auto& k : borrowed_keys_) k->setBounds(row.removeFromLeft(bw).withTrimmedRight(3));
    }
    row = line(138, 26);
    {
        const int tw = row.getWidth() / static_cast<int>(types_.size());
        for (auto& k : types_) k->setBounds(row.removeFromLeft(tw).withTrimmedRight(3));
    }
    row = line(184, 26);
    const int w = r.getWidth() / static_cast<int>(alterations_.size());
    for (auto& k : alterations_) k->setBounds(row.removeFromLeft(w).withTrimmedRight(3));
    row = line(230, 26);
    for (auto& k : inversions_) k->setBounds(row.removeFromLeft(54).withTrimmedRight(3));
    row.removeFromLeft(8);
    name_lcd_.setBounds(row);
    row = line(276, 26);
    close_voicing_.setBounds(row.removeFromLeft(62).withTrimmedRight(3));
    open_.setBounds(row.removeFromLeft(62).withTrimmedRight(3));
    row.removeFromLeft(12);
    span1_.setBounds(row.removeFromLeft(62).withTrimmedRight(3));
    span2_.setBounds(row.removeFromLeft(62).withTrimmedRight(3));
    row = line(312, 24);
    octave_down_.setBounds(row.removeFromLeft(28));
    row.removeFromLeft(3);
    octave_lcd_.setBounds(row.removeFromLeft(86));
    row.removeFromLeft(3);
    octave_up_.setBounds(row.removeFromLeft(28));
    row.removeFromLeft(8);
    symbol_.setBounds(row.removeFromLeft(76));
    row.removeFromLeft(6);
    then_.setBounds(row);
    send_.setBounds(line(344, 24));
    row = juce::Rectangle<int>(r.getX(), getHeight() - 42, r.getWidth(), 30);
    insert_.setBounds(row.removeFromLeft(110));
    row.removeFromLeft(6);
    replace_.setBounds(row.removeFromLeft(120));
    row.removeFromLeft(6);
    audition_.setBounds(row);
}

juce::Component& ChordPalette::drag_target()
{
    // In its own window the header moves the window (to any screen), not
    // the palette inside it.
    auto* top = getTopLevelComponent();
    return top != nullptr && top != this && top->isOnDesktop() ? *top : *this;
}

void ChordPalette::mouseDown(const juce::MouseEvent& e)
{
    if (e.y >= 30) return;
    auto& target = drag_target();
    if (&target == this) {
        dragger_.startDraggingComponent(this, e);
        return;
    }
    // Moved in screen coordinates: the window goes anywhere on the desktop.
    drag_from_ = e.getScreenPosition();
    window_from_ = target.getScreenPosition();
}

void ChordPalette::mouseDrag(const juce::MouseEvent& e)
{
    if (e.mouseDownPosition.y >= 30) return;
    auto& target = drag_target();
    if (&target == this) {
        dragger_.dragComponent(this, e, &constrainer_);  // docked: stays in its parent
        return;
    }
    target.setTopLeftPosition(window_from_ + (e.getScreenPosition() - drag_from_));
}

}  // namespace vcc::desktop::composer_ui
