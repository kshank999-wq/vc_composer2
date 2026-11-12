// The expression maps a composer keeps (spec 02 sections 7, 12, 13).
//
// What is checked here is what the library decides: which maps it holds
// and which it refuses, what goes into the settings file and comes back,
// and what a map that will not read costs - which is that map and nothing
// else.
#include "vcc/composer/ExpressionMapLibrary.h"

#include <string>
#include <vector>

#include "vcc_test.h"
#include "vcc_test_files.h"

using namespace vcc::composer;
using vcc::notation::Articulation;
using vcc::notation::ArticulationAction;
using vcc::notation::ArticulationRule;
using vcc::notation::DynamicLevel;
using vcc::notation::DynamicTarget;
using vcc::notation::ExpressionMap;

namespace {

/// A map that differs from the defaults in every field a file carries, so
/// a round trip that quietly dropped one shows up.
ExpressionMap everything_changed()
{
    ExpressionMap map;
    map.name = "Solo Strings";
    map.library = "A library";
    map.dynamic_target = DynamicTarget::VelocityAndControlChange;
    map.dynamics_cc = 1;
    for (std::size_t index = 0; index < vcc::notation::kDynamicLevelCount; ++index) {
        map.dynamic_velocity[index] = static_cast<std::uint8_t>(20 + index * 10);
        map.dynamic_cc_value[index] = static_cast<std::uint8_t>(10 + index * 15);
    }
    map.default_hairpin_shape = vcc::notation::CurveShape::SCurve;
    map.hairpin_resolution_ticks = 60;
    map.default_length_scale = 0.9;
    map.minimum_note_ticks = 45;

    ArticulationRule staccato;
    staccato.articulation = Articulation::Staccato;
    staccato.action = ArticulationAction::Keyswitch;
    staccato.keyswitch_note = 24;
    staccato.keyswitch_velocity = 90;
    map.set_rule(staccato);

    ArticulationRule accent;
    accent.articulation = Articulation::Accent;
    accent.action = ArticulationAction::ScaleVelocityAndLength;
    accent.velocity_scale = 1.3;
    accent.length_scale = 0.8;
    map.set_rule(accent);

    ArticulationRule mute;
    mute.articulation = Articulation::Mute;
    mute.action = ArticulationAction::ControlChange;
    mute.cc_number = 32;
    mute.cc_value = 64;
    map.set_rule(mute);
    return map;
}

bool same(const ExpressionMap& a, const ExpressionMap& b)
{
    if (a.name != b.name || a.library != b.library || a.dynamic_target != b.dynamic_target
        || a.dynamics_cc != b.dynamics_cc || a.default_hairpin_shape != b.default_hairpin_shape
        || a.hairpin_resolution_ticks != b.hairpin_resolution_ticks
        || a.minimum_note_ticks != b.minimum_note_ticks
        || std::abs(a.default_length_scale - b.default_length_scale) > 1e-9)
        return false;
    for (std::size_t index = 0; index < vcc::notation::kDynamicLevelCount; ++index) {
        if (a.dynamic_velocity[index] != b.dynamic_velocity[index]
            || a.dynamic_cc_value[index] != b.dynamic_cc_value[index])
            return false;
    }
    if (a.articulations.size() != b.articulations.size())
        return false;
    for (const ArticulationRule& rule : a.articulations) {
        const ArticulationRule* other = b.rule_for(rule.articulation);
        if (other == nullptr || other->action != rule.action
            || other->keyswitch_note != rule.keyswitch_note
            || other->keyswitch_velocity != rule.keyswitch_velocity
            || other->cc_number != rule.cc_number || other->cc_value != rule.cc_value
            || std::abs(other->velocity_scale - rule.velocity_scale) > 1e-9
            || std::abs(other->length_scale - rule.length_scale) > 1e-9)
            return false;
    }
    return true;
}

bool mentions(const std::string& text, const std::string& fragment)
{
    return text.find(fragment) != std::string::npos;
}

}  // namespace

VCC_TEST(the_library_starts_with_the_two_built_in_maps_and_they_cannot_be_replaced)
{
    ExpressionMapLibrary library = built_in_expression_maps();
    VCC_CHECK_EQ(library.size(), static_cast<std::size_t>(2));
    VCC_CHECK(library.find(kDefaultExpressionMapName) != nullptr);
    VCC_CHECK(library.find(kOrchestralExpressionMapName) != nullptr);
    VCC_CHECK(library.is_built_in(kDefaultExpressionMapName));
    VCC_CHECK(!library.is_built_in("Solo Strings"));
    VCC_CHECK_EQ(library.default_map().name, std::string(kDefaultExpressionMapName));

    // A composer's map under a built-in's name is refused, as is removing a
    // built-in: the reference a composer started from is always there.
    ExpressionMap impostor = everything_changed();
    impostor.name = kDefaultExpressionMapName;
    VCC_CHECK(!library.store(impostor));
    VCC_CHECK(!library.remove(kDefaultExpressionMapName));
    VCC_CHECK_EQ(library.default_map().dynamic_target, DynamicTarget::Velocity);

    // A blank name and an invalid map are refused too.
    ExpressionMap blank = everything_changed();
    blank.name.clear();
    VCC_CHECK(!library.store(blank));
    ExpressionMap broken = everything_changed();
    broken.dynamics_cc = 200;
    VCC_CHECK(!library.store(broken));
    VCC_CHECK_EQ(library.size(), static_cast<std::size_t>(2));
}

VCC_TEST(a_composers_map_is_stored_replaced_by_name_and_removed)
{
    ExpressionMapLibrary library = built_in_expression_maps();
    VCC_CHECK(library.store(everything_changed()));
    VCC_CHECK_EQ(library.size(), static_cast<std::size_t>(3));

    ExpressionMap changed = everything_changed();
    changed.dynamics_cc = 11;
    VCC_CHECK(library.store(changed));
    VCC_CHECK_EQ(library.size(), static_cast<std::size_t>(3));
    VCC_CHECK_EQ(library.find("Solo Strings")->dynamics_cc, static_cast<std::uint8_t>(11));

    VCC_CHECK(library.remove("Solo Strings"));
    VCC_CHECK(library.find("Solo Strings") == nullptr);
    VCC_CHECK(!library.remove("Solo Strings"));
}

VCC_TEST(stepping_through_the_maps_wraps_and_a_copy_gets_a_free_name)
{
    ExpressionMapLibrary library = built_in_expression_maps();
    VCC_CHECK(library.store(everything_changed()));

    VCC_CHECK_EQ(library.next_after(kDefaultExpressionMapName),
                 std::string(kOrchestralExpressionMapName));
    VCC_CHECK_EQ(library.next_after(kOrchestralExpressionMapName), std::string("Solo Strings"));
    VCC_CHECK_EQ(library.next_after("Solo Strings"), std::string(kDefaultExpressionMapName));
    // A name that is not there steps to the first.
    VCC_CHECK_EQ(library.next_after("nothing"), std::string(kDefaultExpressionMapName));

    VCC_CHECK_EQ(library.copy_name_for("Default"), std::string("Default copy"));
    ExpressionMap copy = everything_changed();
    copy.name = "Default copy";
    VCC_CHECK(library.store(copy));
    VCC_CHECK_EQ(library.copy_name_for("Default"), std::string("Default copy 2"));
}

VCC_TEST(a_map_goes_into_the_settings_and_comes_back_whole)
{
    vcc::composer::SettingsSection section;
    expression_map_to_settings(everything_changed(), section);

    // By name, not by number, so the file reads as what it says.
    VCC_CHECK_EQ(section.get("dynamics.target"), std::string("velocity + control change"));
    VCC_CHECK_EQ(section.get("rule.0.articulation"), std::string("staccato"));
    VCC_CHECK_EQ(section.get("rule.0.action"), std::string("keyswitch"));
    VCC_CHECK(section.has("dynamics.velocity.mf"));

    ExpressionMap read;
    std::vector<std::string> warnings;
    VCC_CHECK(expression_map_from_settings(section, read, warnings));
    VCC_CHECK_MSG(warnings.empty(), warnings.empty() ? "" : warnings.front());
    VCC_CHECK(same(read, everything_changed()));
}

VCC_TEST(a_value_that_will_not_read_keeps_its_default_and_is_named_and_a_rule_is_left_out)
{
    vcc::composer::SettingsSection section;
    expression_map_to_settings(everything_changed(), section);
    section.set("dynamics.cc", "mod wheel");
    section.set("dynamics.velocity.ff", "300");
    section.set("hairpin.shape", "wobbly");
    section.set("rule.0.keyswitch_note", "C0");  // The staccato rule.
    section.set("rule.1.action", "wave hands");  // The accent rule.

    ExpressionMap read;
    std::vector<std::string> warnings;
    VCC_CHECK(expression_map_from_settings(section, read, warnings));

    // The map still reads - the rest of it is the composer's work - and
    // each bad value is named.
    VCC_CHECK_EQ(warnings.size(), static_cast<std::size_t>(5));
    VCC_CHECK_EQ(read.dynamics_cc, static_cast<std::uint8_t>(11));  // The engine's default.
    VCC_CHECK_EQ(read.velocity_for(DynamicLevel::Fortissimo), static_cast<std::uint8_t>(112));
    VCC_CHECK_EQ(read.velocity_for(DynamicLevel::Forte), static_cast<std::uint8_t>(70));
    VCC_CHECK_EQ(read.default_hairpin_shape, vcc::notation::CurveShape::Linear);

    // The two bad rules are gone and the good one is there.
    VCC_CHECK(read.rule_for(Articulation::Staccato) == nullptr);
    VCC_CHECK(read.rule_for(Articulation::Accent) == nullptr);
    VCC_CHECK(read.rule_for(Articulation::Mute) != nullptr);
    for (const std::string& warning : warnings)
        VCC_CHECK_MSG(mentions(warning, "Solo Strings") || mentions(warning, "the "), warning);

    // A map with no name is nothing to read.
    vcc::composer::SettingsSection nameless;
    nameless.set("dynamics.cc", "1");
    ExpressionMap none;
    std::vector<std::string> said;
    VCC_CHECK(!expression_map_from_settings(nameless, none, said));
    VCC_CHECK_EQ(said.size(), static_cast<std::size_t>(1));
}

VCC_TEST(the_composers_maps_go_into_the_store_whole_and_the_built_ins_do_not)
{
    ExpressionMapLibrary library = built_in_expression_maps();
    VCC_CHECK(library.store(everything_changed()));
    ExpressionMap second = everything_changed();
    second.name = "Brass";
    VCC_CHECK(library.store(second));

    vcc::composer::SettingsStore store;
    expression_maps_to_store(library, store);
    VCC_CHECK(store.has_section("expression_map.0"));
    VCC_CHECK(store.has_section("expression_map.1"));
    VCC_CHECK(!store.has_section("expression_map.2"));

    // Read back onto a fresh set of built-ins: three maps, same as before.
    ExpressionMapLibrary read = built_in_expression_maps();
    VCC_CHECK(expression_maps_from_store(store, read).empty());
    VCC_CHECK_EQ(read.size(), static_cast<std::size_t>(4));
    VCC_CHECK(same(*read.find("Solo Strings"), everything_changed()));

    // Written whole: a map removed today does not come back from a longer
    // list tomorrow.
    VCC_CHECK(library.remove("Solo Strings"));
    expression_maps_to_store(library, store);
    VCC_CHECK(!store.has_section("expression_map.1"));
    ExpressionMapLibrary again = built_in_expression_maps();
    expression_maps_from_store(store, again);
    VCC_CHECK_EQ(again.size(), static_cast<std::size_t>(3));
    VCC_CHECK(again.find("Brass") != nullptr);
}

VCC_TEST(a_map_in_the_file_named_like_a_built_in_is_left_out_and_said)
{
    vcc::composer::SettingsStore store;
    ExpressionMap impostor = everything_changed();
    impostor.name = kDefaultExpressionMapName;
    expression_map_to_settings(impostor, store.section("expression_map.0"));

    ExpressionMapLibrary read = built_in_expression_maps();
    const std::vector<std::string> warnings = expression_maps_from_store(store, read);
    VCC_CHECK_EQ(read.size(), static_cast<std::size_t>(2));
    VCC_CHECK_EQ(read.default_map().dynamic_target, DynamicTarget::Velocity);
    VCC_CHECK_EQ(warnings.size(), static_cast<std::size_t>(1));
    VCC_CHECK_MSG(mentions(warnings.front(), "built in"), warnings.front());
}

// The case that carried this library through the companion application's
// settings file was not ported: that file belonged to the Pro Tools
// companion. VC Composer 2 persists these through its own settings layer.

VCC_TEST_MAIN("composer/ExpressionMapLibrary")
