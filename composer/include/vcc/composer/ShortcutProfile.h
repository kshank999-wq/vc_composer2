// Copyright (c) VC Composer. All rights reserved.
//
// Keyboard workflow profiles (spec 02 section 11, acceptance criterion 7).
//
// Spec 11 asks for Finale-style, Sibelius-style, VC Composer default and
// custom profiles, all editable, with the explicit constraint: "Shortcut
// profiles should emulate familiar workflow concepts without copying
// proprietary code or protected UI assets. All shortcuts must remain
// editable because host/application conflicts may occur."
//
// What that means in practice, and what this module does:
//
//   * Commands are named by what they do, never by another product's menu
//     structure or terminology.
//   * The profiles encode the *workflow concept* each application is known
//     for - Finale's numeric-keypad duration entry, Sibelius's number-row
//     duration entry - because those conventions are the muscle memory a
//     composer actually has. They are not transcriptions of any product's
//     shortcut table, and where a real product's binding is not known with
//     confidence it is simply left unbound rather than guessed at.
//   * Every binding is editable and every profile can be copied, because a
//     host will take some of these keys for itself.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace vcc::composer {

/// Modifier keys, as a bitmask so combinations are representable.
///
/// `Command` is the macOS command key and `Control` the control key on both
/// platforms. They are kept separate rather than folded into one "primary
/// modifier" so a profile can be written once and still respect the
/// platform convention the user expects.
enum class Modifier : std::uint8_t {
    None    = 0,
    Shift   = 1u << 0,
    Control = 1u << 1,
    Alt     = 1u << 2,   ///< Option on macOS.
    Command = 1u << 3,   ///< macOS only.
};

using ModifierMask = std::uint8_t;

/// Which modifier a platform uses for application shortcuts.
///
/// Every desktop platform has one key that means "this is a command, not a
/// character": Command on macOS, Control on Windows and Linux. Binding the
/// wrong one is not a cosmetic difference. Cmd+Z is what a Mac user's hands
/// do without thinking, and Ctrl+Z on macOS is bound to nothing at all, so a
/// profile built for the wrong platform has a dead undo key.
///
/// It is a value rather than a compile-time switch so that a profile can be
/// built for either platform, compared against the other, and tested on a
/// machine that is neither.
enum class ShortcutPlatform : std::uint8_t {
    /// Windows and Linux: Control.
    ControlKey = 0,
    /// macOS: Command.
    CommandKey,
};

const char* to_string(ShortcutPlatform platform) noexcept;

/// The platform this build targets.
ShortcutPlatform host_shortcut_platform() noexcept;

/// The modifier that platform uses for application shortcuts.
Modifier primary_modifier(ShortcutPlatform platform) noexcept;

ModifierMask to_mask(Modifier modifier) noexcept;
ModifierMask operator|(Modifier a, Modifier b) noexcept;
bool has_modifier(ModifierMask mask, Modifier modifier) noexcept;

/// Combines two masks.
///
/// Needed because `a | b` on an 8-bit type promotes to int, which then
/// narrows when used to initialize a KeyStroke - a warning worth keeping
/// rather than silencing with a cast at every call site.
constexpr ModifierMask combine(ModifierMask a, ModifierMask b) noexcept
{
    return static_cast<ModifierMask>(a | b);
}

/// Physical keys a binding can use.
///
/// The numeric keypad is distinguished from the number row because that
/// distinction is the whole difference between two of the workflow
/// conventions this module exists to support.
enum class Key : std::uint16_t {
    None = 0,

    // Letters.
    A = 1, B, C, D, E, F, G, H, I, J, K, L, M,
    N, O, P, Q, R, S, T, U, V, W, X, Y, Z,

    // Number row.
    Digit0 = 40, Digit1, Digit2, Digit3, Digit4,
    Digit5, Digit6, Digit7, Digit8, Digit9,

    // Numeric keypad, distinct from the number row.
    Keypad0 = 60, Keypad1, Keypad2, Keypad3, Keypad4,
    Keypad5, Keypad6, Keypad7, Keypad8, Keypad9,
    KeypadPlus, KeypadMinus, KeypadMultiply, KeypadDivide,
    KeypadEnter, KeypadDecimal,

    // Navigation and editing.
    Space = 90, Enter, Escape, Tab, Backspace, Delete,
    ArrowLeft, ArrowRight, ArrowUp, ArrowDown,
    Home, End, PageUp, PageDown,

    // Punctuation used by notation conventions.
    Period = 120, Comma, Slash, Backslash, Semicolon, Quote,
    LeftBracket, RightBracket, Minus, Equals, Grave,

    // Function keys.
    F1 = 140, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
};

const char* to_string(Key key) noexcept;

/// Parses a key name as `to_string` writes it. Returns false when
/// unrecognized, so a corrupt or hand-edited profile does not silently bind
/// the wrong key.
bool key_from_string(const std::string& text, Key& out) noexcept;

/// One key combination.
struct KeyStroke {
    Key key = Key::None;
    ModifierMask modifiers = 0;

    bool is_bound() const noexcept { return key != Key::None; }

    /// Human-readable form, for the UI and for the settings file:
    /// "Ctrl+Shift+N", "Keypad5", "Period".
    std::string to_text() const;

    /// Parses what `to_text` produces.
    static bool from_text(const std::string& text, KeyStroke& out);
};

bool operator==(const KeyStroke& a, const KeyStroke& b) noexcept;
bool operator!=(const KeyStroke& a, const KeyStroke& b) noexcept;

/// The commands a profile can bind.
///
/// Named for what they do rather than after any application's menus. New
/// commands are appended so that a stored profile keeps working.
enum class Command : std::uint16_t {
    None = 0,

    // --- Note duration selection -------------------------------------------
    SelectDurationBreve = 1,
    SelectDurationWhole,
    SelectDurationHalf,
    SelectDurationQuarter,
    SelectDurationEighth,
    SelectDurationSixteenth,
    SelectDurationThirtySecond,
    SelectDurationSixtyFourth,

    // --- Rhythmic modifiers -------------------------------------------------
    ToggleDot = 20,
    ToggleDoubleDot,
    ToggleTuplet,
    ToggleTie,
    ToggleRest,
    ToggleGraceEntry,

    // --- Accidentals --------------------------------------------------------
    AccidentalSharp = 40,
    AccidentalFlat,
    AccidentalNatural,
    AccidentalDoubleSharp,
    AccidentalDoubleFlat,
    CycleEnharmonicSpelling,

    // --- Pitch entry and movement -------------------------------------------
    PitchUpStep = 60,
    PitchDownStep,
    PitchUpOctave,
    PitchDownOctave,
    MoveCursorLeft,
    MoveCursorRight,
    MoveCursorMeasureLeft,
    MoveCursorMeasureRight,
    MoveCursorStaffUp,
    MoveCursorStaffDown,

    // --- Articulations ------------------------------------------------------
    ArticulationAccent = 90,
    ArticulationStaccato,
    ArticulationTenuto,
    ArticulationMarcato,
    ArticulationStaccatissimo,

    // --- Dynamics -----------------------------------------------------------
    DynamicPianississimo = 110,
    DynamicPianissimo,
    DynamicPiano,
    DynamicMezzoPiano,
    DynamicMezzoForte,
    DynamicForte,
    DynamicFortissimo,
    DynamicFortississimo,

    // --- Spans --------------------------------------------------------------
    StartCrescendo = 140,
    StartDiminuendo,
    StartSlur,
    ExtendSpanRight,
    ShrinkSpanLeft,

    // --- Structure ----------------------------------------------------------
    EditKeySignature = 160,
    EditTimeSignature,
    EditClef,
    EditTempo,
    AddText,

    // --- Editing ------------------------------------------------------------
    Undo = 180,
    Redo,
    Copy,
    Cut,
    Paste,
    DeleteSelection,
    SelectAll,

    // --- Workflow -----------------------------------------------------------
    ToggleKeyboardEntryMode = 200,
    PlayFromCursor,
    StopPlayback,
    ResyncWithHost,
    ShowComposerWindow,

    // --- File ---------------------------------------------------------------
    /// Write the score out as a standard MIDI file, for importing into Pro
    /// Tools or dragging onto a track (spec 01 section 7).
    ExportMidiFile = 220,

    /// Read a standard MIDI file back in as notation (spec 02 section 16).
    ///
    /// Placed next to the export because it is its inverse, and because a
    /// composer looking for one looks where the other is.
    ImportMidiFile,

    /// Step the grid an imported MIDI file is quantized to through the
    /// values the interface offers.
    ///
    /// One command rather than one per value: the grid is set rarely, and a
    /// row of note values next to the duration buttons would read as a
    /// second way to choose a duration.
    CycleImportGrid,

    /// Consider the triplet subdivision of that grid as well.
    ToggleImportTriplets,

    /// Snap imported note lengths to that grid, or keep them as they were
    /// played.
    ToggleImportLengths,

    /// Step the shortest note an import keeps through the values the
    /// interface offers, the first of which is "keep everything".
    CycleImportMinimumLength,

    /// Guess a clef for each imported part from where it sits, or write
    /// every one on a treble staff.
    ToggleImportClefs,

    /// Step the key an import spells against when the file names none,
    /// through the keys the interface offers.
    CycleImportKey,

    /// Place the score onto its bound Pro Tools track over the scripting
    /// channel: the same file as the export, imported rather than dragged.
    SendToProTools,

    /// Write the part to the file it came from, asking for one when it has
    /// none yet (spec 02 section 4.1).
    SaveDocument,

    /// Write the part to a file the composer chooses, whether or not it
    /// already has one.
    SaveDocumentAs,

    /// Open a saved part.
    OpenDocument,

    /// Close the part on screen, asking first when it has unsaved changes.
    CloseDocument,
    CloseAllDocuments,

    /// Start a new, empty part.
    NewDocument,

    /// Give the part on screen a different name.
    RenameDocument,

    /// Copy the part on screen into a new part beside it.
    DuplicateDocument,

    /// Move the part on screen one place up or down the window's list.
    MoveDocumentUp,
    MoveDocumentDown,

    /// Draw the notation larger or smaller, and back to its default size.
    ZoomNotationIn,
    ZoomNotationOut,
    ResetNotationZoom,

    /// Show the host playhead now, and whether to keep showing it.
    ScrollToPlayhead,
    ToggleFollowPlayhead,

    /// Sound a click to write to, when the host is not playing one.
    ToggleClick,

    /// Count the click at a tempo of the composer's rather than the part's.
    ClickTempoSlower,
    ClickTempoFaster,
    ClickFollowsThePart,

    // --- Voices -------------------------------------------------------------
    /// Write into one of the four voices a staff holds (spec 02 section
    /// 5.1: a keyboard part is two or more lines on one staff). Everything
    /// goes into the first until another is chosen. With notes selected,
    /// moves them into that voice instead of changing where entry goes.
    SelectVoice1 = 260,
    SelectVoice2,
    SelectVoice3,
    SelectVoice4,

    // --- File, continued ----------------------------------------------------
    /// Reopen one of the parts opened or saved lately, from a list, with no
    /// file dialog (`app/RecentParts.h`).
    OpenRecentDocument = 280,

    /// Read a MIDI file into the part on screen, at the cursor, rather than
    /// as a part of its own (spec 02 section 16).
    ImportMidiFileIntoPart,

    /// Read the velocities of an imported file as dynamics, or leave the
    /// dynamics for the composer to write.
    ToggleImportDynamics,

    // --- Expression maps ----------------------------------------------------
    /// Step the part on screen through the expression maps this
    /// installation has (spec 02 section 7).
    CycleExpressionMap = 300,

    /// Copy the part's map under a new name, as the composer's own to edit.
    CopyExpressionMap,

    /// Step how the part's map sends dynamics: velocity, a controller, or
    /// both.
    CycleExpressionMapDynamics,

    /// Set the controller the part's map sends dynamics on.
    SetExpressionMapDynamicsController,

    /// Set the keyswitch note an articulation sends, in the part's map.
    SetExpressionMapKeyswitch,

    // --- Shortcut profiles --------------------------------------------------
    /// Step the window through the shortcut profiles this installation has
    /// (spec 02 section 11). Not bound by any built-in profile: a key that
    /// changes what every key does is a key a composer presses by accident.
    CycleShortcutProfile = 320,

    /// Copy the profile in use under a new name, as the composer's own to
    /// edit.
    CopyShortcutProfile,

    /// Bind a command to a key in the profile in use, from a line the
    /// composer typed: "Undo Ctrl+Z", or "Undo none" to take the key away.
    SetShortcutBinding,

    // --- Instruments --------------------------------------------------------
    /// Choose which instrument the part on screen is written for, from the
    /// catalogue: its clef, its transposition, a grand staff when it reads
    /// one (spec 02 sections 5.1, 8).
    ChooseInstrument = 340,

    /// Set how far the part on screen sounds from where it is written, in
    /// semitones, for an instrument the catalogue does not have.
    SetPartTransposition,

    // --- Import, continued --------------------------------------------------
    /// Read an imported file's markers and text events as text markings,
    /// or leave them in the file.
    ToggleImportMarkers = 360,

    /// Step the point a keyboard track is split at on import - off, C3,
    /// C4, C5 - so a piano sketch lands on two staves.
    CycleImportSplit,

    // --- Printing -----------------------------------------------------------
    /// Write the part on screen as pages of SVG, with the music font
    /// embedded, for a player to print (spec 02 section 5.3).
    PrintPart = 380,

    // --- Cross-staff ---------------------------------------------------------
    /// Draws the selected notes on the staff above or below the one they
    /// belong to, as a piano part crosses its hands; pressed again, puts
    /// them back.
    CrossStaffUp = 400,
    CrossStaffDown,

    // --- Grace notes, continued ---------------------------------------------
    /// Puts the acciaccatura's slash on the selected grace notes or takes
    /// it off, which is what makes an appoggiatura.
    ToggleGraceSlash = 420,

    // --- Import, continued --------------------------------------------------
    /// Put the notes an imported file forces apart - a note begun while
    /// another still sounds - into voices of their own, or leave
    /// everything in the first voice.
    ToggleImportVoices = 440,

    // --- Tuplets, continued -------------------------------------------------
    /// Nest the tuplet being written inside one of its own ratio - a
    /// triplet inside a triplet - or take the nesting off again.
    ToggleNestedTuplet = 460,
};

/// Stable name for the settings file and the shortcut editor.
const char* to_string(Command command) noexcept;

/// Parses a command name. Returns false for anything unrecognized, so a
/// profile written by a newer build loses only the bindings this build does
/// not understand.
bool command_from_string(const std::string& text, Command& out) noexcept;

/// Short description for the shortcut editor.
const char* describe(Command command) noexcept;

/// Every command, for building an editor UI.
const std::vector<Command>& all_commands();

/// A named set of bindings (spec 12: ShortcutProfile).
class ShortcutProfile {
public:
    ShortcutProfile() = default;
    explicit ShortcutProfile(std::string name) : name_(std::move(name)) {}

    const std::string& name() const noexcept { return name_; }
    void set_name(std::string name) { name_ = std::move(name); }

    /// True for the three profiles that ship with the application. A
    /// built-in profile can be read and copied but not edited in place, so
    /// a user cannot lose the reference they were working from - spec 11
    /// requires every shortcut be editable, which is served by copying to a
    /// custom profile.
    bool is_built_in() const noexcept { return built_in_; }

    /// Binds a stroke to a command.
    ///
    /// Returns false when the profile is built in, or when the stroke is
    /// already bound to a different command - a silent steal would leave the
    /// user with a command that mysteriously stopped working. Rebind
    /// deliberately with `rebind`.
    bool bind(Command command, const KeyStroke& stroke);

    /// Binds a stroke, taking it from whatever held it. Returns the command
    /// that lost it, or `Command::None`.
    Command rebind(Command command, const KeyStroke& stroke);

    /// Removes a command's binding.
    bool unbind(Command command);

    /// The stroke bound to a command, or an unbound stroke.
    KeyStroke stroke_for(Command command) const noexcept;

    /// The command a stroke triggers, or `Command::None`.
    Command command_for(const KeyStroke& stroke) const noexcept;

    bool is_bound(Command command) const noexcept;

    /// Every binding, for serialization and for the editor.
    std::vector<std::pair<Command, KeyStroke>> bindings() const;

    std::size_t binding_count() const noexcept { return bindings_.size(); }

    /// A copy with every Control binding turned into a Command binding, or
    /// the reverse, so a profile written on one platform can be used on
    /// another.
    ///
    /// Only the application-shortcut modifier is swapped. Shift and Alt mean
    /// the same thing everywhere, and a binding using neither Control nor
    /// Command is left exactly as it is - a duration on the number row is
    /// not a platform convention.
    ShortcutProfile remapped_for(ShortcutPlatform platform) const;

    /// An editable copy, named `new_name`. This is how a user customizes a
    /// built-in profile (spec 11).
    ShortcutProfile duplicate(std::string new_name) const;

    /// Strokes bound to more than one command. Always empty for the
    /// built-in profiles, and checked by their tests - a profile shipping
    /// with a conflict would be a bug the user has to discover.
    std::vector<KeyStroke> conflicts() const;

    void clear();

private:
    friend ShortcutProfile make_vc_composer_profile(ShortcutPlatform);
    friend ShortcutProfile make_finale_style_profile(ShortcutPlatform);
    friend ShortcutProfile make_sibelius_style_profile(ShortcutPlatform);

    void bind_unchecked(Command command, const KeyStroke& stroke);

    std::string name_;
    bool built_in_ = false;
    std::vector<std::pair<Command, KeyStroke>> bindings_;
};

/// The application's own profile. Chosen to avoid the keys a DAW usually
/// claims, and to be reachable without a numeric keypad - laptops do not
/// have one.
ShortcutProfile make_vc_composer_profile(
    ShortcutPlatform platform = host_shortcut_platform());

/// A profile following the numeric-keypad duration-entry convention that
/// Finale users have in their fingers.
///
/// Emulates the workflow concept, not a product: the keypad selects
/// durations, and everything else follows the same spirit. It is not a
/// transcription of any shortcut table, and bindings that cannot be stated
/// with confidence are left unbound rather than guessed (spec 11).
ShortcutProfile make_finale_style_profile(
    ShortcutPlatform platform = host_shortcut_platform());

/// A profile following the number-row duration-entry convention that
/// Sibelius users have in their fingers, with the same caveats as above.
ShortcutProfile make_sibelius_style_profile(
    ShortcutPlatform platform = host_shortcut_platform());

/// The three built-in profiles.
std::vector<ShortcutProfile> built_in_profiles(
    ShortcutPlatform platform = host_shortcut_platform());

}  // namespace vcc::composer
