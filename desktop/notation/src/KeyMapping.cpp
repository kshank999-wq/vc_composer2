#include "vcc/ui/KeyMapping.h"

namespace vcc::ui {
namespace {

/// True only where the platform has a Command key distinct from Ctrl.
constexpr bool kCommandIsDistinct =
    static_cast<int>(juce::ModifierKeys::commandModifier)
    != static_cast<int>(juce::ModifierKeys::ctrlModifier);

/// Keys JUCE identifies by a named constant rather than by a character.
struct NamedKey {
    int code;
    composer::Key key;
};

const NamedKey kNamedKeys[] = {
    {juce::KeyPress::spaceKey,      composer::Key::Space},
    {juce::KeyPress::returnKey,     composer::Key::Enter},
    {juce::KeyPress::escapeKey,     composer::Key::Escape},
    {juce::KeyPress::tabKey,        composer::Key::Tab},
    {juce::KeyPress::backspaceKey,  composer::Key::Backspace},
    {juce::KeyPress::deleteKey,     composer::Key::Delete},
    {juce::KeyPress::leftKey,       composer::Key::ArrowLeft},
    {juce::KeyPress::rightKey,      composer::Key::ArrowRight},
    {juce::KeyPress::upKey,         composer::Key::ArrowUp},
    {juce::KeyPress::downKey,       composer::Key::ArrowDown},
    {juce::KeyPress::homeKey,       composer::Key::Home},
    {juce::KeyPress::endKey,        composer::Key::End},
    {juce::KeyPress::pageUpKey,     composer::Key::PageUp},
    {juce::KeyPress::pageDownKey,   composer::Key::PageDown},

    {juce::KeyPress::numberPad0,        composer::Key::Keypad0},
    {juce::KeyPress::numberPad1,        composer::Key::Keypad1},
    {juce::KeyPress::numberPad2,        composer::Key::Keypad2},
    {juce::KeyPress::numberPad3,        composer::Key::Keypad3},
    {juce::KeyPress::numberPad4,        composer::Key::Keypad4},
    {juce::KeyPress::numberPad5,        composer::Key::Keypad5},
    {juce::KeyPress::numberPad6,        composer::Key::Keypad6},
    {juce::KeyPress::numberPad7,        composer::Key::Keypad7},
    {juce::KeyPress::numberPad8,        composer::Key::Keypad8},
    {juce::KeyPress::numberPad9,        composer::Key::Keypad9},
    {juce::KeyPress::numberPadAdd,      composer::Key::KeypadPlus},
    {juce::KeyPress::numberPadSubtract, composer::Key::KeypadMinus},
    {juce::KeyPress::numberPadMultiply, composer::Key::KeypadMultiply},
    {juce::KeyPress::numberPadDivide,   composer::Key::KeypadDivide},
    {juce::KeyPress::numberPadSeparator, composer::Key::KeypadEnter},
    {juce::KeyPress::numberPadDecimalPoint, composer::Key::KeypadDecimal},

    {juce::KeyPress::F1Key,  composer::Key::F1},
    {juce::KeyPress::F2Key,  composer::Key::F2},
    {juce::KeyPress::F3Key,  composer::Key::F3},
    {juce::KeyPress::F4Key,  composer::Key::F4},
    {juce::KeyPress::F5Key,  composer::Key::F5},
    {juce::KeyPress::F6Key,  composer::Key::F6},
    {juce::KeyPress::F7Key,  composer::Key::F7},
    {juce::KeyPress::F8Key,  composer::Key::F8},
    {juce::KeyPress::F9Key,  composer::Key::F9},
    {juce::KeyPress::F10Key, composer::Key::F10},
    {juce::KeyPress::F11Key, composer::Key::F11},
    {juce::KeyPress::F12Key, composer::Key::F12},
};

/// Punctuation the notation conventions use, by the character it produces.
struct PunctuationKey {
    int character;
    composer::Key key;
};

const PunctuationKey kPunctuation[] = {
    {'.',  composer::Key::Period},
    {',',  composer::Key::Comma},
    {'/',  composer::Key::Slash},
    {'\\', composer::Key::Backslash},
    {';',  composer::Key::Semicolon},
    {'\'', composer::Key::Quote},
    {'[',  composer::Key::LeftBracket},
    {']',  composer::Key::RightBracket},
    {'-',  composer::Key::Minus},
    {'=',  composer::Key::Equals},
    {'`',  composer::Key::Grave},
};

}  // namespace

composer::ModifierMask modifiers_from(const juce::ModifierKeys& modifiers)
{
    composer::ModifierMask mask = 0;
    if (modifiers.isShiftDown())
        mask = composer::combine(mask, composer::to_mask(composer::Modifier::Shift));
    if (modifiers.isCtrlDown())
        mask = composer::combine(mask, composer::to_mask(composer::Modifier::Control));
    if (modifiers.isAltDown())
        mask = composer::combine(mask, composer::to_mask(composer::Modifier::Alt));

    // On Windows and Linux JUCE's "command" modifier *is* Ctrl - it names
    // the platform's usual shortcut key, not a physical one. Reading both
    // would set two bits for one key held, and a binding recorded as Ctrl
    // would then never match. The shortcut model's Command is the macOS
    // key, so it is only read where it is a key of its own.
    if (kCommandIsDistinct && modifiers.isCommandDown())
        mask = composer::combine(mask, composer::to_mask(composer::Modifier::Command));

    return mask;
}

bool key_stroke_from(const juce::KeyPress& press, composer::KeyStroke& out)
{
    if (!press.isValid())
        return false;

    const int code = press.getKeyCode();
    composer::Key key = composer::Key::None;

    for (const NamedKey& named : kNamedKeys) {
        if (named.code == code) {
            key = named.key;
            break;
        }
    }

    if (key == composer::Key::None) {
        // **Either case.** The comment here used to say JUCE reports letter
        // keys in uppercase whatever the shift state, and the code took it
        // at its word. Running the application says otherwise: on this
        // platform Ctrl+L arrives as 108, the lowercase 'l', and only
        // Ctrl+Shift+L arrives as 76. Every Cmd/Ctrl+letter shortcut - Save,
        // Open, New, Close, Rename, Duplicate, Export, Import - was
        // therefore dropped before it reached a command, silently, while
        // the same strokes worked from a test that built the KeyPress by
        // hand with an uppercase code.
        //
        // Shift is carried as a modifier rather than as a different key, so
        // the letter is the same key whichever case it arrives in.
        if (code >= 'A' && code <= 'Z') {
            key = static_cast<composer::Key>(static_cast<std::uint16_t>(composer::Key::A)
                                             + static_cast<std::uint16_t>(code - 'A'));
        } else if (code >= 'a' && code <= 'z') {
            key = static_cast<composer::Key>(static_cast<std::uint16_t>(composer::Key::A)
                                             + static_cast<std::uint16_t>(code - 'a'));
        } else if (code >= '0' && code <= '9') {
            key = static_cast<composer::Key>(static_cast<std::uint16_t>(composer::Key::Digit0)
                                             + static_cast<std::uint16_t>(code - '0'));
        } else {
            for (const PunctuationKey& punctuation : kPunctuation) {
                if (punctuation.character == code) {
                    key = punctuation.key;
                    break;
                }
            }
        }
    }

    if (key == composer::Key::None)
        return false;

    out.key = key;
    out.modifiers = modifiers_from(press.getModifiers());
    return true;
}

bool step_from_key(const composer::KeyStroke& stroke, notation::Step& out)
{
    // A through G name pitches, as every notation program's entry works.
    // The order here is the model's: C is 0.
    switch (stroke.key) {
        case composer::Key::A: out = notation::Step::A; return true;
        case composer::Key::B: out = notation::Step::B; return true;
        case composer::Key::C: out = notation::Step::C; return true;
        case composer::Key::D: out = notation::Step::D; return true;
        case composer::Key::E: out = notation::Step::E; return true;
        case composer::Key::F: out = notation::Step::F; return true;
        case composer::Key::G: out = notation::Step::G; return true;
        default: break;
    }
    return false;
}

}  // namespace vcc::ui
