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
};

/// The chord a state describes, or nullopt with the reason in `problem`.
std::optional<theory::Chord> chord_for(const ChordModeState& state, std::string* problem = nullptr);

class ChordPalette final : public hw::Panel {
public:
    static constexpr int kWidth = 372;
    static constexpr int kHeight = 352;

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

    ChordModeState& state_;
    std::vector<std::unique_ptr<hw::Keycap>> roots_, accidentals_, alterations_;
    juce::ComboBox quality_, bass_, voicing_, then_;
    hw::Keycap octave_down_{"-"}, octave_up_{"+"}, symbol_{"Symbol", true}, close_{"x"};
    hw::Keycap insert_{"Insert"}, replace_{"Replace sel."}, audition_{"Audition"};
    hw::Lcd octave_lcd_{"", 12.0f}, name_lcd_{"", 15.0f};
    juce::String notes_, warning_;
    juce::ComponentDragger dragger_;
    juce::ComponentBoundsConstrainer constrainer_;
    bool refreshing_ = false;
};

/// A chord's notes on a staff: the written pitch for a transposing one (and
/// a track's Trans), each keeping the chord's spelling.
std::vector<notation::Pitch> written_chord(const std::vector<theory::VoicedTone>& voiced, const notation::Staff& staff,
                                          int trans = 0);

}  // namespace vcc::desktop::composer_ui
