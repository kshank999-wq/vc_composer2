// The Edit window's shortcut table (docs/spec/ui/SPEC.md, "Keyboard
// shortcuts"): Finale/Sibelius numbering on the number row, and nothing
// bound twice.
#include "vcc/composer/ShortcutProfile.h"
#include "vcc_test.h"

using namespace vcc::composer;

namespace {
KeyStroke key(Key k, ModifierMask m = 0) { return KeyStroke{k, m}; }
}  // namespace

TEST_CASE("note values follow Finale and Sibelius: 5 is a quarter, 7 a whole")
{
    for (const auto platform : {ShortcutPlatform::CommandKey, ShortcutPlatform::ControlKey}) {
        const auto p = make_composer2_profile(platform);
        CHECK(p.command_for(key(Key::Digit7)) == Command::SelectDurationWhole);
        CHECK(p.command_for(key(Key::Digit6)) == Command::SelectDurationHalf);
        CHECK(p.command_for(key(Key::Digit5)) == Command::SelectDurationQuarter);
        CHECK(p.command_for(key(Key::Digit4)) == Command::SelectDurationEighth);
        CHECK(p.command_for(key(Key::Digit3)) == Command::SelectDurationSixteenth);
        CHECK(p.command_for(key(Key::Digit2)) == Command::SelectDurationThirtySecond);
        CHECK(p.command_for(key(Key::Period)) == Command::ToggleDot);
        CHECK(p.command_for(key(Key::Slash)) == Command::ToggleTuplet);
        CHECK(p.command_for(key(Key::Digit0)) == Command::ToggleRest);
        CHECK(p.command_for(key(Key::Minus)) == Command::AccidentalFlat);
        CHECK(p.command_for(key(Key::Equals)) == Command::AccidentalSharp);
        CHECK(p.command_for(key(Key::ArrowUp)) == Command::PitchUpStep);
        CHECK(p.command_for(key(Key::ArrowDown)) == Command::PitchDownStep);
        CHECK(p.command_for(key(Key::ArrowUp, to_mask(Modifier::Shift))) == Command::PitchUpOctave);
        CHECK(p.command_for(key(Key::ArrowDown, to_mask(Modifier::Shift))) == Command::PitchDownOctave);
        const auto primary = to_mask(primary_modifier(platform));
        CHECK(p.command_for(key(Key::Digit1, primary)) == Command::SelectVoice1);
        CHECK(p.command_for(key(Key::Digit4, primary)) == Command::SelectVoice4);
        CHECK(p.command_for(key(Key::Z, primary)) == Command::Undo);
    }
}

TEST_CASE("the profile binds no keystroke twice and leaves Enter, R and L to the transport")
{
    const auto p = make_composer2_profile(ShortcutPlatform::CommandKey);
    CHECK(p.conflicts().empty());
    CHECK(p.command_for(key(Key::Enter)) == Command::None);
    CHECK(p.command_for(key(Key::R)) == Command::None);
    CHECK(p.command_for(key(Key::L)) == Command::None);
    CHECK(p.is_built_in());
    CHECK_EQ(p.name(), std::string("Composer 2"));
}
