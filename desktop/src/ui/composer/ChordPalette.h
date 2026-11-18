// Chord Mode's floating palette (Add-On Features spec §3): root, quality,
// alterations and omissions, bass or inversion, voicing, register, and what
// happens after an entry. The Composer view owns it, shows it while Chord
// Mode is on, and enters the chord it describes - at the cursor, over the
// selection, or wherever a staff is clicked - as one undo step.
#pragma once

#include <functional>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "ui/AppContext.h"
#include "ui/hw/HwControls.h"
#include "vcc/notation/Score.h"
#include "vcc/theory/Voicing.h"

namespace vcc::desktop::composer_ui {

/// What the palette is set to. Kept by the Composer view, so it survives
/// the palette closing.
struct ChordModeState {
    int letter = 0;                      ///< 0 C .. 6 B
    int accidental = 0;                  ///< -1 flat, 0 natural, +1 sharp
    std::string quality = "maj";         ///< theory quality id
    std::set<std::string> alterations;   ///< "b5", "#5", "b9", "#9", "#11", "b13", "no3", "no5"
    int inversion = 0;                   ///< 0 root position, 1-3
    std::string slash_bass;              ///< "E", "Bb"; empty: none (overrides inversion)
    theory::VoicingStyle style = theory::VoicingStyle::Closed;
    int octave = 4;                      ///< The bass note's octave (C4 is middle C).
    int then = 0;                        ///< 0 after the chord, 1 next beat, 2 next bar
    bool write_symbol = true;            ///< Also set the chord symbol (the ruler's Chords row).
    /// What kind of chord, built in the key on the chosen (or clicked) root:
    /// "triad" and "seventh" are the key's own chords on that degree; "maj7",
    /// "add9", "sus2", "sus4", "dim", "aug" are those chords outright;
    /// "custom" uses `quality` as chosen.
    std::string type = "triad";
    theory::Key key{};                   ///< The project's key, for "triad" and "seventh".
    bool open = false;                   ///< Open voicing (else close).
    int span = 1;                        ///< Spread over 1 or 2 octaves.
};

/// The chord types the palette offers, as (type id, key legend).
const std::vector<std::pair<std::string, const char*>>& chord_types();

/// The palette's chord as written notes, per staff: a single staff gets
/// them all; a grand staff (piano) gets them split for two hands - the left
/// hand the root (and, over two octaves, its fifth and octave) below
/// middle C, the right hand the chord above it.
struct ChordForStaves {
    std::vector<theory::VoicedTone> upper;  ///< The staff written on (or the right hand).
    std::vector<theory::VoicedTone> lower;  ///< The left hand, on a grand staff.
};
ChordForStaves voice_chord_for_staves(const theory::Chord& chord, const ChordModeState& state, bool grand_staff);

/// The chord a state describes, or nullopt with the reason in `problem`.
std::optional<theory::Chord> chord_for(const ChordModeState& state, std::string* problem = nullptr);

class ChordPalette final : public hw::Panel {
public:
    static constexpr int kWidth = 420;
    static constexpr int kHeight = 404;

    explicit ChordPalette(ChordModeState& state);

    /// Re-reads the state into the controls.
    void refresh();
    /// The line under the controls: the chord's notes, and any warning.
    void set_preview(const juce::String& notes, const juce::String& warning);

    std::function<void(bool replace_selection)> on_insert;
    std::function<void()> on_audition;
    std::function<void()> on_close;
    /// Something changed: the owner recomputes the preview.
    std::function<void()> on_changed;

    // For the checks.
    hw::Keycap& root_key(int letter) { return *roots_[static_cast<std::size_t>(letter)]; }
    hw::Keycap& accidental_key(int accidental) { return *accidentals_[static_cast<std::size_t>(accidental + 1)]; }
    hw::Keycap* alteration_key(const std::string& name);
    juce::ComboBox& quality_box() { return quality_; }
    juce::ComboBox& bass_box() { return bass_; }
    juce::ComboBox& voicing_box() { return voicing_; }
    juce::ComboBox& then_box() { return then_; }
    hw::Keycap& insert_key() { return insert_; }
    hw::Keycap& replace_key() { return replace_; }
    hw::Keycap& audition_key() { return audition_; }
    hw::Keycap* type_key(const std::string& type);
    hw::Keycap& inversion_key(int inversion) { return *inversions_[static_cast<std::size_t>(std::clamp(inversion, 0, 3))]; }
    hw::Keycap& open_key() { return open_; }
    hw::Keycap& close_key() { return close_voicing_; }
    hw::Keycap& span_key(int octaves) { return octaves >= 2 ? span2_ : span1_; }
    hw::Keycap& symbol_key() { return symbol_; }
    const juce::String& preview_notes() const noexcept { return notes_; }
    const juce::String& preview_warning() const noexcept { return warning_; }

    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;

private:
    void changed();
    void fill_bass_box();
    juce::Component& drag_target();

    ChordModeState& state_;
    std::vector<std::unique_ptr<hw::Keycap>> roots_, accidentals_, alterations_, types_, inversions_;
    hw::Keycap close_voicing_{"Close", true}, open_{"Open", true}, span1_{"1 Oct", true}, span2_{"2 Oct", true};
    juce::ComboBox quality_, bass_, voicing_, then_;
    hw::Keycap octave_down_{"-"}, octave_up_{"+"}, symbol_{"Symbol", true}, close_{"x"};
    hw::Keycap insert_{"Insert"}, replace_{"Replace sel."}, audition_{"Audition"};
    hw::Lcd octave_lcd_{"", 12.0f}, name_lcd_{"", 15.0f};
    juce::String notes_, warning_;
    juce::ComponentDragger dragger_;
    juce::Point<int> drag_from_, window_from_;
    juce::ComponentBoundsConstrainer constrainer_;
    bool refreshing_ = false;
};

/// A chord's notes on a staff: the written pitch for a transposing one (and
/// a track's Trans), each keeping the chord's spelling.
std::vector<notation::Pitch> written_chord(const std::vector<theory::VoicedTone>& voiced, const notation::Staff& staff,
                                          int trans = 0);

}  // namespace vcc::desktop::composer_ui
