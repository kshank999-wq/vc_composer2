#include "vcc/composer/ShortcutProfile.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace vcc::composer {
namespace {

struct KeyName { Key key; const char* name; };

/// Every key's stable text form. Used for the settings file, so these names
/// are part of the on-disk format and must not change.
const KeyName kKeyNames[] = {
    {Key::A, "A"}, {Key::B, "B"}, {Key::C, "C"}, {Key::D, "D"}, {Key::E, "E"},
    {Key::F, "F"}, {Key::G, "G"}, {Key::H, "H"}, {Key::I, "I"}, {Key::J, "J"},
    {Key::K, "K"}, {Key::L, "L"}, {Key::M, "M"}, {Key::N, "N"}, {Key::O, "O"},
    {Key::P, "P"}, {Key::Q, "Q"}, {Key::R, "R"}, {Key::S, "S"}, {Key::T, "T"},
    {Key::U, "U"}, {Key::V, "V"}, {Key::W, "W"}, {Key::X, "X"}, {Key::Y, "Y"},
    {Key::Z, "Z"},

    {Key::Digit0, "0"}, {Key::Digit1, "1"}, {Key::Digit2, "2"}, {Key::Digit3, "3"},
    {Key::Digit4, "4"}, {Key::Digit5, "5"}, {Key::Digit6, "6"}, {Key::Digit7, "7"},
    {Key::Digit8, "8"}, {Key::Digit9, "9"},

    {Key::Keypad0, "Keypad0"}, {Key::Keypad1, "Keypad1"}, {Key::Keypad2, "Keypad2"},
    {Key::Keypad3, "Keypad3"}, {Key::Keypad4, "Keypad4"}, {Key::Keypad5, "Keypad5"},
    {Key::Keypad6, "Keypad6"}, {Key::Keypad7, "Keypad7"}, {Key::Keypad8, "Keypad8"},
    {Key::Keypad9, "Keypad9"},
    {Key::KeypadPlus, "KeypadPlus"}, {Key::KeypadMinus, "KeypadMinus"},
    {Key::KeypadMultiply, "KeypadMultiply"}, {Key::KeypadDivide, "KeypadDivide"},
    {Key::KeypadEnter, "KeypadEnter"}, {Key::KeypadDecimal, "KeypadDecimal"},

    {Key::Space, "Space"}, {Key::Enter, "Enter"}, {Key::Escape, "Escape"},
    {Key::Tab, "Tab"}, {Key::Backspace, "Backspace"}, {Key::Delete, "Delete"},
    {Key::ArrowLeft, "Left"}, {Key::ArrowRight, "Right"},
    {Key::ArrowUp, "Up"}, {Key::ArrowDown, "Down"},
    {Key::Home, "Home"}, {Key::End, "End"},
    {Key::PageUp, "PageUp"}, {Key::PageDown, "PageDown"},

    {Key::Period, "Period"}, {Key::Comma, "Comma"}, {Key::Slash, "Slash"},
    {Key::Backslash, "Backslash"}, {Key::Semicolon, "Semicolon"}, {Key::Quote, "Quote"},
    {Key::LeftBracket, "LeftBracket"}, {Key::RightBracket, "RightBracket"},
    {Key::Minus, "Minus"}, {Key::Equals, "Equals"}, {Key::Grave, "Grave"},

    {Key::F1, "F1"}, {Key::F2, "F2"}, {Key::F3, "F3"}, {Key::F4, "F4"},
    {Key::F5, "F5"}, {Key::F6, "F6"}, {Key::F7, "F7"}, {Key::F8, "F8"},
    {Key::F9, "F9"}, {Key::F10, "F10"}, {Key::F11, "F11"}, {Key::F12, "F12"},
};

struct CommandName { Command command; const char* name; const char* description; };

/// Command names are part of the on-disk format and must not change.
const CommandName kCommandNames[] = {
    {Command::SelectDurationBreve, "SelectDurationBreve", "Select breve (double whole note)"},
    {Command::SelectDurationWhole, "SelectDurationWhole", "Select whole note"},
    {Command::SelectDurationHalf, "SelectDurationHalf", "Select half note"},
    {Command::SelectDurationQuarter, "SelectDurationQuarter", "Select quarter note"},
    {Command::SelectDurationEighth, "SelectDurationEighth", "Select eighth note"},
    {Command::SelectDurationSixteenth, "SelectDurationSixteenth", "Select 16th note"},
    {Command::SelectDurationThirtySecond, "SelectDurationThirtySecond", "Select 32nd note"},
    {Command::SelectDurationSixtyFourth, "SelectDurationSixtyFourth", "Select 64th note"},

    {Command::ToggleDot, "ToggleDot", "Add or remove a dot"},
    {Command::ToggleDoubleDot, "ToggleDoubleDot", "Add or remove a second dot"},
    {Command::ToggleTuplet, "ToggleTuplet", "Start or end a tuplet"},
    {Command::ToggleTie, "ToggleTie", "Tie to the next note"},
    {Command::ToggleRest, "ToggleRest", "Enter a rest instead of a note"},
    {Command::ToggleGraceEntry, "ToggleGraceEntry", "Enter grace notes"},

    {Command::AccidentalSharp, "AccidentalSharp", "Sharp"},
    {Command::AccidentalFlat, "AccidentalFlat", "Flat"},
    {Command::AccidentalNatural, "AccidentalNatural", "Natural"},
    {Command::AccidentalDoubleSharp, "AccidentalDoubleSharp", "Double sharp"},
    {Command::AccidentalDoubleFlat, "AccidentalDoubleFlat", "Double flat"},
    {Command::CycleEnharmonicSpelling, "CycleEnharmonicSpelling", "Respell enharmonically"},

    {Command::PitchUpStep, "PitchUpStep", "Raise pitch one step"},
    {Command::PitchDownStep, "PitchDownStep", "Lower pitch one step"},
    {Command::PitchUpOctave, "PitchUpOctave", "Raise pitch an octave"},
    {Command::PitchDownOctave, "PitchDownOctave", "Lower pitch an octave"},
    {Command::MoveCursorLeft, "MoveCursorLeft", "Move left"},
    {Command::MoveCursorRight, "MoveCursorRight", "Move right"},
    {Command::MoveCursorMeasureLeft, "MoveCursorMeasureLeft", "Previous measure"},
    {Command::MoveCursorMeasureRight, "MoveCursorMeasureRight", "Next measure"},
    {Command::MoveCursorStaffUp, "MoveCursorStaffUp", "Staff above"},
    {Command::MoveCursorStaffDown, "MoveCursorStaffDown", "Staff below"},

    {Command::ArticulationAccent, "ArticulationAccent", "Accent"},
    {Command::ArticulationStaccato, "ArticulationStaccato", "Staccato"},
    {Command::ArticulationTenuto, "ArticulationTenuto", "Tenuto"},
    {Command::ArticulationMarcato, "ArticulationMarcato", "Marcato"},
    {Command::ArticulationStaccatissimo, "ArticulationStaccatissimo", "Staccatissimo"},

    {Command::DynamicPianississimo, "DynamicPianississimo", "ppp"},
    {Command::DynamicPianissimo, "DynamicPianissimo", "pp"},
    {Command::DynamicPiano, "DynamicPiano", "p"},
    {Command::DynamicMezzoPiano, "DynamicMezzoPiano", "mp"},
    {Command::DynamicMezzoForte, "DynamicMezzoForte", "mf"},
    {Command::DynamicForte, "DynamicForte", "f"},
    {Command::DynamicFortissimo, "DynamicFortissimo", "ff"},
    {Command::DynamicFortississimo, "DynamicFortississimo", "fff"},

    {Command::StartCrescendo, "StartCrescendo", "Crescendo hairpin"},
    {Command::StartDiminuendo, "StartDiminuendo", "Diminuendo hairpin"},
    {Command::StartSlur, "StartSlur", "Slur"},
    {Command::ExtendSpanRight, "ExtendSpanRight", "Extend the span right"},
    {Command::ShrinkSpanLeft, "ShrinkSpanLeft", "Shrink the span from the right"},

    {Command::EditKeySignature, "EditKeySignature", "Key signature"},
    {Command::EditTimeSignature, "EditTimeSignature", "Time signature"},
    {Command::EditClef, "EditClef", "Clef"},
    {Command::EditTempo, "EditTempo", "Tempo marking"},
    {Command::AddText, "AddText", "Text or playing technique"},

    {Command::Undo, "Undo", "Undo"},
    {Command::Redo, "Redo", "Redo"},
    {Command::Copy, "Copy", "Copy"},
    {Command::Cut, "Cut", "Cut"},
    {Command::Paste, "Paste", "Paste"},
    {Command::DeleteSelection, "DeleteSelection", "Delete the selection"},
    {Command::SelectAll, "SelectAll", "Select all"},

    {Command::ToggleKeyboardEntryMode, "ToggleKeyboardEntryMode", "Keyboard-only entry mode"},
    {Command::PlayFromCursor, "PlayFromCursor", "Play from the cursor"},
    {Command::StopPlayback, "StopPlayback", "Stop playback"},
    {Command::ResyncWithHost, "ResyncWithHost", "Re-sync with the host timeline"},
    {Command::ShowComposerWindow, "ShowComposerWindow", "Bring VC Composer to the front"},

    {Command::ExportMidiFile, "ExportMidiFile", "Export as a MIDI file"},
    {Command::ImportMidiFile, "ImportMidiFile", "Import a MIDI file"},
    {Command::CycleImportGrid, "CycleImportGrid", "Change the grid an import snaps to"},
    {Command::ToggleImportTriplets, "ToggleImportTriplets", "Snap an import to triplets too"},
    {Command::ToggleImportLengths, "ToggleImportLengths", "Snap imported note lengths"},
    {Command::CycleImportMinimumLength, "CycleImportMinimumLength",
     "Change the shortest note an import keeps"},
    {Command::ToggleImportClefs, "ToggleImportClefs", "Guess a clef for each imported part"},
    {Command::CycleImportKey, "CycleImportKey", "Change the key an import spells against"},
    {Command::SendToProTools, "SendToProTools", "Send to Pro Tools"},

    {Command::SaveDocument, "SaveDocument", "Save this part"},
    {Command::SaveDocumentAs, "SaveDocumentAs", "Save this part as"},
    {Command::OpenDocument, "OpenDocument", "Open a saved part"},
    {Command::CloseDocument, "CloseDocument", "Close this part"},
    {Command::CloseAllDocuments, "CloseAllDocuments", "Close every part"},
    {Command::NewDocument, "NewDocument", "Start a new part"},
    {Command::RenameDocument, "RenameDocument", "Rename this part"},
    {Command::DuplicateDocument, "DuplicateDocument", "Duplicate this part"},
    {Command::MoveDocumentUp, "MoveDocumentUp", "Move this part up the list"},
    {Command::MoveDocumentDown, "MoveDocumentDown", "Move this part down the list"},
    {Command::ZoomNotationIn, "ZoomNotationIn", "Draw the notation larger"},
    {Command::ZoomNotationOut, "ZoomNotationOut", "Draw the notation smaller"},
    {Command::ResetNotationZoom, "ResetNotationZoom", "Back to the default size"},
    {Command::ScrollToPlayhead, "ScrollToPlayhead", "Show the playhead"},
    {Command::ToggleFollowPlayhead, "ToggleFollowPlayhead", "Follow the playhead"},
    {Command::ToggleClick, "ToggleClick", "Sound a click"},
    {Command::ClickTempoSlower, "ClickTempoSlower", "Click a mark slower"},
    {Command::ClickTempoFaster, "ClickTempoFaster", "Click a mark faster"},
    {Command::ClickFollowsThePart, "ClickFollowsThePart", "Click at the part's tempo"},

    {Command::SelectVoice1, "SelectVoice1", "Write in voice 1"},
    {Command::SelectVoice2, "SelectVoice2", "Write in voice 2"},
    {Command::SelectVoice3, "SelectVoice3", "Write in voice 3"},
    {Command::SelectVoice4, "SelectVoice4", "Write in voice 4"},

    {Command::OpenRecentDocument, "OpenRecentDocument", "Reopen a recent part"},
    {Command::ImportMidiFileIntoPart, "ImportMidiFileIntoPart",
     "Import a MIDI file into this part at the cursor"},
    {Command::ToggleImportDynamics, "ToggleImportDynamics",
     "Read an import's velocities as dynamics"},

    {Command::CycleExpressionMap, "CycleExpressionMap", "Next expression map for this part"},
    {Command::CopyExpressionMap, "CopyExpressionMap", "Copy this part's expression map"},
    {Command::CycleExpressionMapDynamics, "CycleExpressionMapDynamics",
     "How this part's map sends dynamics"},
    {Command::SetExpressionMapDynamicsController, "SetExpressionMapDynamicsController",
     "Which controller this part's map sends dynamics on"},
    {Command::SetExpressionMapKeyswitch, "SetExpressionMapKeyswitch",
     "The keyswitch an articulation sends in this part's map"},

    {Command::CycleShortcutProfile, "CycleShortcutProfile", "Next shortcut profile"},
    {Command::CopyShortcutProfile, "CopyShortcutProfile", "Copy the shortcut profile in use"},
    {Command::SetShortcutBinding, "SetShortcutBinding",
     "Bind a command to a key in the shortcut profile in use"},

    {Command::ChooseInstrument, "ChooseInstrument", "Which instrument this part is written for"},
    {Command::SetPartTransposition, "SetPartTransposition",
     "How far this part sounds from where it is written"},

    {Command::ToggleImportMarkers, "ToggleImportMarkers",
     "Read an import's markers as text"},
    {Command::CycleImportSplit, "CycleImportSplit",
     "Change where an imported keyboard track is split into two staves"},

    {Command::PrintPart, "PrintPart", "Print this part as pages of SVG"},
    {Command::CrossStaffUp, "CrossStaffUp", "Draw the notes on the staff above"},
    {Command::CrossStaffDown, "CrossStaffDown", "Draw the notes on the staff below"},
    {Command::ToggleGraceSlash, "ToggleGraceSlash", "Slash or unslash the grace notes"},
    {Command::ToggleImportVoices, "ToggleImportVoices",
     "Tell an import's overlapping lines apart as voices"},
    {Command::ToggleNestedTuplet, "ToggleNestedTuplet",
     "Start or end a tuplet inside the tuplet being written"},
};

}  // namespace

ModifierMask to_mask(Modifier modifier) noexcept
{
    return static_cast<ModifierMask>(modifier);
}

ModifierMask operator|(Modifier a, Modifier b) noexcept
{
    return static_cast<ModifierMask>(static_cast<ModifierMask>(a) | static_cast<ModifierMask>(b));
}

bool has_modifier(ModifierMask mask, Modifier modifier) noexcept
{
    const auto bit = static_cast<ModifierMask>(modifier);
    return bit != 0 && (mask & bit) == bit;
}

const char* to_string(Key key) noexcept
{
    for (const KeyName& entry : kKeyNames) {
        if (entry.key == key)
            return entry.name;
    }
    return "None";
}

bool key_from_string(const std::string& text, Key& out) noexcept
{
    for (const KeyName& entry : kKeyNames) {
        if (text == entry.name) {
            out = entry.key;
            return true;
        }
    }
    return false;
}

const char* to_string(ShortcutPlatform platform) noexcept
{
    switch (platform) {
        case ShortcutPlatform::ControlKey: return "Control";
        case ShortcutPlatform::CommandKey: break;
    }
    return "Command";
}

ShortcutPlatform host_shortcut_platform() noexcept
{
#if defined(__APPLE__)
    return ShortcutPlatform::CommandKey;
#else
    return ShortcutPlatform::ControlKey;
#endif
}

Modifier primary_modifier(ShortcutPlatform platform) noexcept
{
    return platform == ShortcutPlatform::CommandKey ? Modifier::Command : Modifier::Control;
}

std::string KeyStroke::to_text() const
{
    if (!is_bound())
        return "";

    std::string text;
    // A fixed modifier order, so the text form of a stroke is stable and
    // can be compared as a string.
    if (has_modifier(modifiers, Modifier::Control))
        text += "Ctrl+";
    if (has_modifier(modifiers, Modifier::Command))
        text += "Cmd+";
    if (has_modifier(modifiers, Modifier::Alt))
        text += "Alt+";
    if (has_modifier(modifiers, Modifier::Shift))
        text += "Shift+";

    text += to_string(key);
    return text;
}

bool KeyStroke::from_text(const std::string& text, KeyStroke& out)
{
    if (text.empty())
        return false;

    KeyStroke result;
    std::size_t position = 0;

    for (;;) {
        const std::size_t plus = text.find('+', position);
        if (plus == std::string::npos)
            break;

        const std::string token = text.substr(position, plus - position);
        if (token == "Ctrl")
            result.modifiers |= to_mask(Modifier::Control);
        else if (token == "Cmd")
            result.modifiers |= to_mask(Modifier::Command);
        else if (token == "Alt")
            result.modifiers |= to_mask(Modifier::Alt);
        else if (token == "Shift")
            result.modifiers |= to_mask(Modifier::Shift);
        else
            return false;  // An unknown modifier must not be ignored.

        position = plus + 1;
    }

    if (!key_from_string(text.substr(position), result.key))
        return false;

    out = result;
    return true;
}

bool operator==(const KeyStroke& a, const KeyStroke& b) noexcept
{
    return a.key == b.key && a.modifiers == b.modifiers;
}

bool operator!=(const KeyStroke& a, const KeyStroke& b) noexcept
{
    return !(a == b);
}

const char* to_string(Command command) noexcept
{
    for (const CommandName& entry : kCommandNames) {
        if (entry.command == command)
            return entry.name;
    }
    return "None";
}

bool command_from_string(const std::string& text, Command& out) noexcept
{
    for (const CommandName& entry : kCommandNames) {
        if (text == entry.name) {
            out = entry.command;
            return true;
        }
    }
    return false;
}

const char* describe(Command command) noexcept
{
    for (const CommandName& entry : kCommandNames) {
        if (entry.command == command)
            return entry.description;
    }
    return "";
}

const std::vector<Command>& all_commands()
{
    static const std::vector<Command> commands = [] {
        std::vector<Command> result;
        result.reserve(sizeof(kCommandNames) / sizeof(kCommandNames[0]));
        for (const CommandName& entry : kCommandNames)
            result.push_back(entry.command);
        return result;
    }();
    return commands;
}

void ShortcutProfile::bind_unchecked(Command command, const KeyStroke& stroke)
{
    const auto existing =
        std::find_if(bindings_.begin(), bindings_.end(),
                     [command](const std::pair<Command, KeyStroke>& entry) {
                         return entry.first == command;
                     });
    if (existing != bindings_.end())
        existing->second = stroke;
    else
        bindings_.emplace_back(command, stroke);
}

bool ShortcutProfile::bind(Command command, const KeyStroke& stroke)
{
    if (built_in_ || command == Command::None || !stroke.is_bound())
        return false;

    // Refuse to steal a stroke silently: the user would be left with a
    // command that stopped working for no visible reason.
    const Command holder = command_for(stroke);
    if (holder != Command::None && holder != command)
        return false;

    bind_unchecked(command, stroke);
    return true;
}

Command ShortcutProfile::rebind(Command command, const KeyStroke& stroke)
{
    if (built_in_ || command == Command::None || !stroke.is_bound())
        return Command::None;

    const Command displaced = command_for(stroke);
    if (displaced != Command::None && displaced != command)
        unbind(displaced);

    bind_unchecked(command, stroke);
    return displaced == command ? Command::None : displaced;
}

bool ShortcutProfile::unbind(Command command)
{
    if (built_in_)
        return false;

    const auto found =
        std::find_if(bindings_.begin(), bindings_.end(),
                     [command](const std::pair<Command, KeyStroke>& entry) {
                         return entry.first == command;
                     });
    if (found == bindings_.end())
        return false;

    bindings_.erase(found);
    return true;
}

KeyStroke ShortcutProfile::stroke_for(Command command) const noexcept
{
    for (const std::pair<Command, KeyStroke>& entry : bindings_) {
        if (entry.first == command)
            return entry.second;
    }
    return KeyStroke{};
}

Command ShortcutProfile::command_for(const KeyStroke& stroke) const noexcept
{
    if (!stroke.is_bound())
        return Command::None;
    for (const std::pair<Command, KeyStroke>& entry : bindings_) {
        if (entry.second == stroke)
            return entry.first;
    }
    return Command::None;
}

bool ShortcutProfile::is_bound(Command command) const noexcept
{
    return stroke_for(command).is_bound();
}

std::vector<std::pair<Command, KeyStroke>> ShortcutProfile::bindings() const
{
    return bindings_;
}

ShortcutProfile ShortcutProfile::remapped_for(ShortcutPlatform platform) const
{
    const ModifierMask control = to_mask(Modifier::Control);
    const ModifierMask command = to_mask(Modifier::Command);
    const ModifierMask wanted = to_mask(primary_modifier(platform));
    const ModifierMask unwanted = wanted == control ? command : control;

    ShortcutProfile remapped = *this;
    for (std::pair<Command, KeyStroke>& binding : remapped.bindings_) {
        ModifierMask& modifiers = binding.second.modifiers;
        if ((modifiers & unwanted) == 0)
            continue;

        // Swap the one bit. Shift and Alt mean the same thing on every
        // platform, and a binding using neither key is not a platform
        // convention at all.
        modifiers = static_cast<ModifierMask>(modifiers & ~unwanted);
        modifiers = combine(modifiers, wanted);
    }
    return remapped;
}

ShortcutProfile ShortcutProfile::duplicate(std::string new_name) const
{
    ShortcutProfile copy;
    copy.name_ = std::move(new_name);
    copy.bindings_ = bindings_;
    // The copy is editable even when the original is not: this is how a
    // user customizes a built-in profile (spec 11).
    copy.built_in_ = false;
    return copy;
}

std::vector<KeyStroke> ShortcutProfile::conflicts() const
{
    std::vector<KeyStroke> duplicates;

    for (std::size_t i = 0; i < bindings_.size(); ++i) {
        for (std::size_t j = i + 1; j < bindings_.size(); ++j) {
            if (bindings_[i].second != bindings_[j].second)
                continue;
            if (std::find(duplicates.begin(), duplicates.end(), bindings_[i].second)
                == duplicates.end()) {
                duplicates.push_back(bindings_[i].second);
            }
        }
    }
    return duplicates;
}

void ShortcutProfile::clear()
{
    if (built_in_)
        return;
    bindings_.clear();
}

// ---------------------------------------------------------------------------
// Built-in profiles
// ---------------------------------------------------------------------------

namespace {

/// Bindings shared by all three profiles: platform editing conventions and
/// navigation that no notation application would rebind.
void add_common_bindings(ShortcutProfile& profile,
                         void (*bind)(ShortcutProfile&, Command, const KeyStroke&),
                         ShortcutPlatform platform)
{
    // Command on macOS, Control elsewhere - for the editing conventions and
    // for the navigation that follows them. Notation applications use the
    // same key for both on each platform, so a Mac user gets Cmd+Z and
    // Cmd+left-arrow together rather than a mixture.
    const ModifierMask primary = to_mask(primary_modifier(platform));

    bind(profile, Command::Undo, KeyStroke{Key::Z, primary});
    bind(profile, Command::Redo, KeyStroke{Key::Z, combine(primary, to_mask(Modifier::Shift))});
    bind(profile, Command::Copy, KeyStroke{Key::C, primary});
    bind(profile, Command::Cut, KeyStroke{Key::X, primary});
    bind(profile, Command::Paste, KeyStroke{Key::V, primary});
    bind(profile, Command::SelectAll, KeyStroke{Key::A, primary});
    bind(profile, Command::ExportMidiFile, KeyStroke{Key::E, primary});
    bind(profile, Command::PrintPart, KeyStroke{Key::P, primary});
    bind(profile, Command::SendToProTools,
         KeyStroke{Key::E, combine(primary, to_mask(Modifier::Shift))});

    // Import, not italic: there is no italic in a notation toolbar, and
    // every application that reads a file in puts it here.
    bind(profile, Command::ImportMidiFile, KeyStroke{Key::I, primary});
    // Shifted, beside it: the same file, placed into the part on screen
    // rather than given one of its own.
    bind(profile, Command::ImportMidiFileIntoPart,
         KeyStroke{Key::I, combine(primary, to_mask(Modifier::Shift))});

    // The conventions every application on both platforms shares. A
    // notation program that moved Save would be the only one.
    bind(profile, Command::SaveDocument, KeyStroke{Key::S, primary});
    bind(profile, Command::SaveDocumentAs,
         KeyStroke{Key::S, combine(primary, to_mask(Modifier::Shift))});
    bind(profile, Command::OpenDocument, KeyStroke{Key::O, primary});
    // Beside Open, shifted: the list is a quicker Open, and the stroke
    // says so.
    bind(profile, Command::OpenRecentDocument,
         KeyStroke{Key::O, combine(primary, to_mask(Modifier::Shift))});
    bind(profile, Command::CloseDocument, KeyStroke{Key::W, primary});

    // Shift+W beside W, which is what every application with a "close all"
    // uses. It closes things rather than transposing them, so it is not in
    // the territory the shifted arrows had to stay out of.
    bind(profile, Command::CloseAllDocuments,
         KeyStroke{Key::W, combine(primary, to_mask(Modifier::Shift))});
    bind(profile, Command::NewDocument, KeyStroke{Key::N, primary});
    bind(profile, Command::RenameDocument, KeyStroke{Key::R, primary});
    bind(profile, Command::DuplicateDocument, KeyStroke{Key::D, primary});

    // Not the bare or primary arrows: those transpose by a step and by an
    // octave, and a composer reaching for a pitch must never reorder their
    // parts instead.
    bind(profile, Command::MoveDocumentUp,
         KeyStroke{Key::ArrowUp, combine(primary, to_mask(Modifier::Shift))});
    bind(profile, Command::MoveDocumentDown,
         KeyStroke{Key::ArrowDown, combine(primary, to_mask(Modifier::Shift))});

    // Cross-staff on the remaining arrow pair: the note moves to the other
    // staff on the page and nowhere else.
    bind(profile, Command::CrossStaffUp, KeyStroke{Key::ArrowUp, to_mask(Modifier::Alt)});
    bind(profile, Command::CrossStaffDown, KeyStroke{Key::ArrowDown, to_mask(Modifier::Alt)});

    // The conventional trio, and free here: the notation view reads bare
    // digits and letters, not these.
    bind(profile, Command::ZoomNotationIn, KeyStroke{Key::Equals, primary});
    bind(profile, Command::ZoomNotationOut, KeyStroke{Key::Minus, primary});
    bind(profile, Command::ResetNotationZoom, KeyStroke{Key::Digit0, primary});

    // L for locate, which is the word a DAW uses for it.
    bind(profile, Command::ScrollToPlayhead, KeyStroke{Key::L, primary});
    bind(profile, Command::ToggleFollowPlayhead,
         KeyStroke{Key::L, combine(primary, to_mask(Modifier::Shift))});

    // K for klick, which is what a DAW calls it - and Cmd/Ctrl+M is a
    // window command on macOS.
    bind(profile, Command::ToggleClick, KeyStroke{Key::K, primary});

    // Only the way back is bound. A practice tempo is chosen once and then
    // written to, so the steppers are buttons; the keyboard is for what a
    // composer does *while* writing, and getting back to the part's own
    // tempo is the one of these that belongs there.
    bind(profile, Command::ClickFollowsThePart,
         KeyStroke{Key::K, combine(primary, to_mask(Modifier::Shift))});
    bind(profile, Command::DeleteSelection, KeyStroke{Key::Delete, 0});

    bind(profile, Command::MoveCursorLeft, KeyStroke{Key::ArrowLeft, 0});
    bind(profile, Command::MoveCursorRight, KeyStroke{Key::ArrowRight, 0});
    bind(profile, Command::PitchUpStep, KeyStroke{Key::ArrowUp, 0});
    bind(profile, Command::PitchDownStep, KeyStroke{Key::ArrowDown, 0});
    bind(profile, Command::PitchUpOctave, KeyStroke{Key::ArrowUp, primary});
    bind(profile, Command::PitchDownOctave, KeyStroke{Key::ArrowDown, primary});
    bind(profile, Command::MoveCursorMeasureLeft, KeyStroke{Key::ArrowLeft, primary});
    bind(profile, Command::MoveCursorMeasureRight, KeyStroke{Key::ArrowRight, primary});
    bind(profile, Command::MoveCursorStaffUp, KeyStroke{Key::PageUp, 0});
    bind(profile, Command::MoveCursorStaffDown, KeyStroke{Key::PageDown, 0});

    bind(profile, Command::PlayFromCursor, KeyStroke{Key::Space, 0});
    bind(profile, Command::StopPlayback, KeyStroke{Key::Escape, 0});

    // Voices on Alt and the number row. The bare digits are durations in
    // two of the profiles and Shift+digit is a dynamic in two, so Alt is
    // the modifier that is free for a number in every one of them - and
    // "voice 2" being a 2 is the convention worth keeping whatever the rest
    // of the profile does.
    bind(profile, Command::SelectVoice1, KeyStroke{Key::Digit1, to_mask(Modifier::Alt)});
    bind(profile, Command::SelectVoice2, KeyStroke{Key::Digit2, to_mask(Modifier::Alt)});
    bind(profile, Command::SelectVoice3, KeyStroke{Key::Digit3, to_mask(Modifier::Alt)});
    bind(profile, Command::SelectVoice4, KeyStroke{Key::Digit4, to_mask(Modifier::Alt)});
}

}  // namespace

ShortcutProfile make_vc_composer_profile(ShortcutPlatform platform)
{
    ShortcutProfile profile("VC Composer");

    const auto bind = [](ShortcutProfile& target, Command command, const KeyStroke& stroke) {
        target.bind_unchecked(command, stroke);
    };

    // Durations on the number row, reachable on a laptop: no numeric keypad
    // required. Q is the shortest so the common values sit under the
    // strongest fingers.
    bind(profile, Command::SelectDurationWhole, KeyStroke{Key::Digit1, 0});
    bind(profile, Command::SelectDurationHalf, KeyStroke{Key::Digit2, 0});
    bind(profile, Command::SelectDurationQuarter, KeyStroke{Key::Digit3, 0});
    bind(profile, Command::SelectDurationEighth, KeyStroke{Key::Digit4, 0});
    bind(profile, Command::SelectDurationSixteenth, KeyStroke{Key::Digit5, 0});
    bind(profile, Command::SelectDurationThirtySecond, KeyStroke{Key::Digit6, 0});
    bind(profile, Command::SelectDurationSixtyFourth, KeyStroke{Key::Digit7, 0});
    bind(profile, Command::SelectDurationBreve, KeyStroke{Key::Digit8, 0});

    bind(profile, Command::ToggleDot, KeyStroke{Key::Period, 0});
    bind(profile, Command::ToggleDoubleDot, KeyStroke{Key::Period, to_mask(Modifier::Shift)});
    bind(profile, Command::ToggleTuplet, KeyStroke{Key::T, 0});
    bind(profile, Command::ToggleNestedTuplet, KeyStroke{Key::T, to_mask(Modifier::Shift)});
    bind(profile, Command::ToggleTie, KeyStroke{Key::Y, 0});
    bind(profile, Command::ToggleRest, KeyStroke{Key::R, 0});
    bind(profile, Command::ToggleGraceEntry, KeyStroke{Key::G, to_mask(Modifier::Alt)});
    bind(profile, Command::ToggleGraceSlash,
         KeyStroke{Key::G, combine(to_mask(Modifier::Alt), to_mask(Modifier::Shift))});

    bind(profile, Command::AccidentalSharp, KeyStroke{Key::Equals, 0});
    bind(profile, Command::AccidentalFlat, KeyStroke{Key::Minus, 0});
    bind(profile, Command::AccidentalNatural, KeyStroke{Key::Digit0, 0});
    bind(profile, Command::AccidentalDoubleSharp,
         KeyStroke{Key::Equals, to_mask(Modifier::Shift)});
    bind(profile, Command::AccidentalDoubleFlat,
         KeyStroke{Key::Minus, to_mask(Modifier::Shift)});
    bind(profile, Command::CycleEnharmonicSpelling, KeyStroke{Key::E, 0});

    bind(profile, Command::ArticulationAccent, KeyStroke{Key::A, to_mask(Modifier::Alt)});
    bind(profile, Command::ArticulationStaccato, KeyStroke{Key::S, to_mask(Modifier::Alt)});
    bind(profile, Command::ArticulationTenuto, KeyStroke{Key::T, to_mask(Modifier::Alt)});
    bind(profile, Command::ArticulationMarcato, KeyStroke{Key::M, to_mask(Modifier::Alt)});
    bind(profile, Command::ArticulationStaccatissimo,
         KeyStroke{Key::S, combine(to_mask(Modifier::Alt), to_mask(Modifier::Shift))});

    bind(profile, Command::DynamicPianississimo,
         KeyStroke{Key::Digit1, to_mask(Modifier::Shift)});
    bind(profile, Command::DynamicPianissimo, KeyStroke{Key::Digit2, to_mask(Modifier::Shift)});
    bind(profile, Command::DynamicPiano, KeyStroke{Key::Digit3, to_mask(Modifier::Shift)});
    bind(profile, Command::DynamicMezzoPiano, KeyStroke{Key::Digit4, to_mask(Modifier::Shift)});
    bind(profile, Command::DynamicMezzoForte, KeyStroke{Key::Digit5, to_mask(Modifier::Shift)});
    bind(profile, Command::DynamicForte, KeyStroke{Key::Digit6, to_mask(Modifier::Shift)});
    bind(profile, Command::DynamicFortissimo, KeyStroke{Key::Digit7, to_mask(Modifier::Shift)});
    bind(profile, Command::DynamicFortississimo,
         KeyStroke{Key::Digit8, to_mask(Modifier::Shift)});

    bind(profile, Command::StartCrescendo, KeyStroke{Key::Comma, 0});
    bind(profile, Command::StartDiminuendo, KeyStroke{Key::Slash, 0});
    bind(profile, Command::StartSlur, KeyStroke{Key::S, 0});
    bind(profile, Command::ExtendSpanRight, KeyStroke{Key::RightBracket, 0});
    bind(profile, Command::ShrinkSpanLeft, KeyStroke{Key::LeftBracket, 0});

    bind(profile, Command::EditKeySignature, KeyStroke{Key::K, 0});
    bind(profile, Command::EditTimeSignature, KeyStroke{Key::M, 0});
    bind(profile, Command::EditClef, KeyStroke{Key::C, 0});
    bind(profile, Command::EditTempo, KeyStroke{Key::Q, 0});
    bind(profile, Command::AddText, KeyStroke{Key::X, 0});

    bind(profile, Command::ToggleKeyboardEntryMode, KeyStroke{Key::N, 0});
    bind(profile, Command::ResyncWithHost, KeyStroke{Key::F5, 0});
    bind(profile, Command::ShowComposerWindow, KeyStroke{Key::F6, 0});

    add_common_bindings(profile, bind, platform);

    profile.built_in_ = true;
    return profile;
}

ShortcutProfile make_finale_style_profile(ShortcutPlatform platform)
{
    // The workflow concept: durations on the numeric keypad, which is the
    // muscle memory this profile exists to serve. Not a transcription of any
    // product's shortcut table (spec 11).
    ShortcutProfile profile("Finale style");

    const auto bind = [](ShortcutProfile& target, Command command, const KeyStroke& stroke) {
        target.bind_unchecked(command, stroke);
    };

    bind(profile, Command::SelectDurationSixtyFourth, KeyStroke{Key::Keypad1, 0});
    bind(profile, Command::SelectDurationThirtySecond, KeyStroke{Key::Keypad2, 0});
    bind(profile, Command::SelectDurationSixteenth, KeyStroke{Key::Keypad3, 0});
    bind(profile, Command::SelectDurationEighth, KeyStroke{Key::Keypad4, 0});
    bind(profile, Command::SelectDurationQuarter, KeyStroke{Key::Keypad5, 0});
    bind(profile, Command::SelectDurationHalf, KeyStroke{Key::Keypad6, 0});
    bind(profile, Command::SelectDurationWhole, KeyStroke{Key::Keypad7, 0});
    bind(profile, Command::SelectDurationBreve, KeyStroke{Key::Keypad8, 0});

    bind(profile, Command::ToggleDot, KeyStroke{Key::KeypadDecimal, 0});
    bind(profile, Command::ToggleTie, KeyStroke{Key::KeypadEnter, 0});
    bind(profile, Command::ToggleRest, KeyStroke{Key::Keypad0, 0});
    bind(profile, Command::ToggleGraceEntry, KeyStroke{Key::G, to_mask(Modifier::Alt)});
    bind(profile, Command::ToggleGraceSlash,
         KeyStroke{Key::G, combine(to_mask(Modifier::Alt), to_mask(Modifier::Shift))});
    bind(profile, Command::ToggleTuplet, KeyStroke{Key::Keypad9, 0});
    bind(profile, Command::ToggleNestedTuplet, KeyStroke{Key::Keypad9, to_mask(Modifier::Shift)});

    bind(profile, Command::AccidentalSharp, KeyStroke{Key::KeypadPlus, 0});
    bind(profile, Command::AccidentalFlat, KeyStroke{Key::KeypadMinus, 0});
    bind(profile, Command::AccidentalNatural, KeyStroke{Key::KeypadMultiply, 0});
    bind(profile, Command::CycleEnharmonicSpelling, KeyStroke{Key::KeypadDivide, 0});

    // Off the keypad, the same letter-key spirit as the default profile.
    bind(profile, Command::ArticulationAccent, KeyStroke{Key::A, to_mask(Modifier::Alt)});
    bind(profile, Command::ArticulationStaccato, KeyStroke{Key::S, to_mask(Modifier::Alt)});
    bind(profile, Command::ArticulationTenuto, KeyStroke{Key::T, to_mask(Modifier::Alt)});
    bind(profile, Command::ArticulationMarcato, KeyStroke{Key::M, to_mask(Modifier::Alt)});

    bind(profile, Command::DynamicPiano, KeyStroke{Key::P, 0});
    bind(profile, Command::DynamicForte, KeyStroke{Key::F, 0});
    bind(profile, Command::DynamicMezzoForte, KeyStroke{Key::M, 0});
    bind(profile, Command::DynamicMezzoPiano, KeyStroke{Key::N, 0});

    bind(profile, Command::StartCrescendo, KeyStroke{Key::Comma, 0});
    bind(profile, Command::StartDiminuendo, KeyStroke{Key::Slash, 0});
    bind(profile, Command::StartSlur, KeyStroke{Key::S, 0});

    bind(profile, Command::EditKeySignature, KeyStroke{Key::K, 0});
    bind(profile, Command::EditTimeSignature, KeyStroke{Key::T, 0});
    bind(profile, Command::EditClef, KeyStroke{Key::C, 0});
    bind(profile, Command::EditTempo, KeyStroke{Key::Q, 0});
    bind(profile, Command::AddText, KeyStroke{Key::X, 0});

    bind(profile, Command::ToggleKeyboardEntryMode, KeyStroke{Key::Grave, 0});
    bind(profile, Command::ResyncWithHost, KeyStroke{Key::F5, 0});
    bind(profile, Command::ShowComposerWindow, KeyStroke{Key::F6, 0});

    add_common_bindings(profile, bind, platform);

    profile.built_in_ = true;
    return profile;
}

ShortcutProfile make_sibelius_style_profile(ShortcutPlatform platform)
{
    // The workflow concept: durations on the number row, and letter keys for
    // the common markings. Same caveat as the Finale-style profile.
    ShortcutProfile profile("Sibelius style");

    const auto bind = [](ShortcutProfile& target, Command command, const KeyStroke& stroke) {
        target.bind_unchecked(command, stroke);
    };

    bind(profile, Command::SelectDurationSixtyFourth, KeyStroke{Key::Digit1, 0});
    bind(profile, Command::SelectDurationThirtySecond, KeyStroke{Key::Digit2, 0});
    bind(profile, Command::SelectDurationSixteenth, KeyStroke{Key::Digit3, 0});
    bind(profile, Command::SelectDurationEighth, KeyStroke{Key::Digit4, 0});
    bind(profile, Command::SelectDurationQuarter, KeyStroke{Key::Digit5, 0});
    bind(profile, Command::SelectDurationHalf, KeyStroke{Key::Digit6, 0});
    bind(profile, Command::SelectDurationWhole, KeyStroke{Key::Digit7, 0});
    bind(profile, Command::SelectDurationBreve, KeyStroke{Key::Digit8, 0});

    bind(profile, Command::ToggleDot, KeyStroke{Key::Period, 0});
    bind(profile, Command::ToggleTie, KeyStroke{Key::Enter, 0});
    bind(profile, Command::ToggleRest, KeyStroke{Key::Digit0, 0});
    bind(profile, Command::ToggleGraceEntry, KeyStroke{Key::G, to_mask(Modifier::Alt)});
    bind(profile, Command::ToggleGraceSlash,
         KeyStroke{Key::G, combine(to_mask(Modifier::Alt), to_mask(Modifier::Shift))});
    bind(profile, Command::ToggleTuplet,
         KeyStroke{Key::Digit3, to_mask(primary_modifier(platform))});
    bind(profile, Command::ToggleNestedTuplet,
         KeyStroke{Key::Digit3, combine(to_mask(primary_modifier(platform)),
                                        to_mask(Modifier::Shift))});

    bind(profile, Command::AccidentalSharp, KeyStroke{Key::Equals, 0});
    bind(profile, Command::AccidentalFlat, KeyStroke{Key::Minus, 0});
    bind(profile, Command::AccidentalNatural, KeyStroke{Key::Digit9, 0});
    bind(profile, Command::CycleEnharmonicSpelling, KeyStroke{Key::E, 0});

    bind(profile, Command::ArticulationAccent, KeyStroke{Key::A, to_mask(Modifier::Alt)});
    bind(profile, Command::ArticulationStaccato, KeyStroke{Key::S, to_mask(Modifier::Alt)});
    bind(profile, Command::ArticulationTenuto, KeyStroke{Key::T, to_mask(Modifier::Alt)});
    bind(profile, Command::ArticulationMarcato, KeyStroke{Key::M, to_mask(Modifier::Alt)});

    // Dynamics on shifted digits rather than letter keys. In this workflow
    // N is note-input mode, which is far stronger muscle memory than any
    // single-letter dynamic, so the letters are left to the commands that
    // need them.
    bind(profile, Command::DynamicPianississimo,
         KeyStroke{Key::Digit1, to_mask(Modifier::Shift)});
    bind(profile, Command::DynamicPianissimo, KeyStroke{Key::Digit2, to_mask(Modifier::Shift)});
    bind(profile, Command::DynamicPiano, KeyStroke{Key::Digit3, to_mask(Modifier::Shift)});
    bind(profile, Command::DynamicMezzoPiano, KeyStroke{Key::Digit4, to_mask(Modifier::Shift)});
    bind(profile, Command::DynamicMezzoForte, KeyStroke{Key::Digit5, to_mask(Modifier::Shift)});
    bind(profile, Command::DynamicForte, KeyStroke{Key::Digit6, to_mask(Modifier::Shift)});
    bind(profile, Command::DynamicFortissimo, KeyStroke{Key::Digit7, to_mask(Modifier::Shift)});
    bind(profile, Command::DynamicFortississimo,
         KeyStroke{Key::Digit8, to_mask(Modifier::Shift)});

    bind(profile, Command::StartCrescendo, KeyStroke{Key::H, 0});
    bind(profile, Command::StartDiminuendo, KeyStroke{Key::H, to_mask(Modifier::Shift)});
    bind(profile, Command::StartSlur, KeyStroke{Key::S, 0});

    bind(profile, Command::EditKeySignature, KeyStroke{Key::K, 0});
    bind(profile, Command::EditTimeSignature, KeyStroke{Key::T, 0});
    bind(profile, Command::EditClef, KeyStroke{Key::Q, 0});
    // Alt+T is tenuto in this profile, so tempo takes the shifted form.
    bind(profile, Command::EditTempo,
         KeyStroke{Key::T, combine(to_mask(Modifier::Alt), to_mask(Modifier::Shift))});
    bind(profile, Command::AddText, KeyStroke{Key::X, 0});

    bind(profile, Command::ToggleKeyboardEntryMode, KeyStroke{Key::N, 0});
    bind(profile, Command::ResyncWithHost, KeyStroke{Key::F5, 0});
    bind(profile, Command::ShowComposerWindow, KeyStroke{Key::F6, 0});

    add_common_bindings(profile, bind, platform);

    profile.built_in_ = true;
    return profile;
}

std::vector<ShortcutProfile> built_in_profiles(ShortcutPlatform platform)
{
    return {make_vc_composer_profile(platform), make_finale_style_profile(platform),
            make_sibelius_style_profile(platform)};
}

}  // namespace vcc::composer
