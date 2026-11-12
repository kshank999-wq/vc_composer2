// Shortcut profiles, the instrument catalogue, session templates and
// settings persistence (spec 02 sections 8, 10, 11, 12, 13).
//
// Acceptance criteria 7, 8 and 9 are exercised directly:
//
//   7. switching among Finale-style, Sibelius-style and custom profiles
//   8. saving a preferred patch and marking it the default
//   9. a template recalling track names, instrumentation and preferences
#include "vcc/composer/InstrumentCatalog.h"
#include "vcc/composer/SessionTemplate.h"
#include "vcc/composer/SettingsMigration.h"
#include "vcc/composer/SettingsStore.h"
#include "vcc/composer/ShortcutProfile.h"

#include <algorithm>
#include <set>
#include <string>
#include <vector>

#include "vcc_test.h"

using namespace vcc::composer;

// ===========================================================================
// Shortcut profiles
// ===========================================================================

VCC_TEST(three_profiles_ship_and_all_are_distinct)
{
    // Acceptance criterion 7.
    const std::vector<ShortcutProfile> profiles = built_in_profiles();
    VCC_CHECK_EQ(profiles.size(), static_cast<std::size_t>(3));

    std::set<std::string> names;
    for (const ShortcutProfile& profile : profiles) {
        VCC_CHECK(!profile.name().empty());
        VCC_CHECK(profile.is_built_in());
        VCC_CHECK(profile.binding_count() > 20);
        VCC_CHECK_MSG(names.insert(profile.name()).second, "duplicate profile name");
    }
}

VCC_TEST(no_built_in_profile_ships_with_a_conflict)
{
    // A profile with two commands on one key is a bug the user would have
    // to discover by pressing the key and getting the wrong thing.
    for (const ShortcutProfile& profile : built_in_profiles()) {
        const std::vector<KeyStroke> conflicts = profile.conflicts();
        if (!conflicts.empty()) {
            std::string detail = profile.name() + " has conflicts:";
            for (const KeyStroke& stroke : conflicts)
                detail += " " + stroke.to_text();
            VCC_CHECK_MSG(false, detail);
        }
    }
}

VCC_TEST(the_profiles_differ_in_their_duration_entry_convention)
{
    // The whole reason for offering them: Finale users reach for the numeric
    // keypad, Sibelius users for the number row. A profile that did not
    // differ here would serve nobody's muscle memory.
    const ShortcutProfile finale = make_finale_style_profile();
    const ShortcutProfile sibelius = make_sibelius_style_profile();

    const KeyStroke finale_quarter = finale.stroke_for(Command::SelectDurationQuarter);
    const KeyStroke sibelius_quarter = sibelius.stroke_for(Command::SelectDurationQuarter);

    VCC_CHECK(finale_quarter.is_bound());
    VCC_CHECK(sibelius_quarter.is_bound());
    VCC_CHECK(finale_quarter != sibelius_quarter);

    VCC_CHECK_EQ(finale_quarter.key, Key::Keypad5);
    VCC_CHECK_EQ(sibelius_quarter.key, Key::Digit5);
}

VCC_TEST(the_default_profile_needs_no_numeric_keypad)
{
    // Laptops do not have one, so the shipped default must be reachable
    // without it.
    const ShortcutProfile profile = make_vc_composer_profile();

    for (const std::pair<Command, KeyStroke>& binding : profile.bindings()) {
        const auto key = static_cast<std::uint16_t>(binding.second.key);
        const bool is_keypad = key >= static_cast<std::uint16_t>(Key::Keypad0)
                            && key <= static_cast<std::uint16_t>(Key::KeypadDecimal);
        VCC_CHECK_MSG(!is_keypad, std::string("the default profile binds ")
                                      + to_string(binding.first) + " to a keypad key");
    }
}

VCC_TEST(every_profile_covers_the_essential_commands)
{
    // A profile missing note entry or dynamics is not usable, whatever else
    // it binds.
    static const Command kEssential[] = {
        Command::SelectDurationQuarter, Command::SelectDurationEighth,
        Command::ToggleDot,             Command::ToggleRest,
        Command::AccidentalSharp,       Command::AccidentalFlat,
        Command::PitchUpStep,           Command::PitchDownStep,
        Command::MoveCursorLeft,        Command::MoveCursorRight,
        Command::Undo,                  Command::Redo,
        Command::StartCrescendo,        Command::StartDiminuendo,
        Command::SelectVoice1,          Command::SelectVoice2,
        Command::SelectVoice3,          Command::SelectVoice4,
    };

    for (const ShortcutProfile& profile : built_in_profiles()) {
        for (const Command command : kEssential) {
            VCC_CHECK_MSG(profile.is_bound(command),
                          profile.name() + " does not bind " + to_string(command));
        }
    }
}

VCC_TEST(a_built_in_profile_cannot_be_edited_in_place)
{
    // Protecting the reference the user is working from. Spec 11 requires
    // every shortcut be editable, which is served by duplicating.
    ShortcutProfile profile = make_vc_composer_profile();
    const KeyStroke original = profile.stroke_for(Command::SelectDurationQuarter);

    VCC_CHECK(!profile.bind(Command::SelectDurationQuarter, KeyStroke{Key::Z, 0}));
    VCC_CHECK(!profile.unbind(Command::SelectDurationQuarter));
    VCC_CHECK_EQ(profile.rebind(Command::SelectDurationQuarter, KeyStroke{Key::Z, 0}),
                 Command::None);

    profile.clear();
    VCC_CHECK(profile.binding_count() > 0);
    VCC_CHECK(profile.stroke_for(Command::SelectDurationQuarter) == original);
}

VCC_TEST(duplicating_a_profile_gives_an_editable_copy)
{
    // Spec 11: all shortcuts must remain editable.
    const ShortcutProfile built_in = make_sibelius_style_profile();
    ShortcutProfile custom = built_in.duplicate("My layout");

    VCC_CHECK(!custom.is_built_in());
    VCC_CHECK(custom.name() == "My layout");
    VCC_CHECK_EQ(custom.binding_count(), built_in.binding_count());

    // A free key can be bound.
    VCC_CHECK(custom.bind(Command::ShowComposerWindow, KeyStroke{Key::F9, 0}));
    VCC_CHECK_EQ(custom.command_for(KeyStroke{Key::F9, 0}), Command::ShowComposerWindow);

    // And the original is untouched.
    VCC_CHECK(!built_in.is_bound(Command::ShowComposerWindow)
              || built_in.stroke_for(Command::ShowComposerWindow) != KeyStroke{Key::F9, 0});
}

VCC_TEST(binding_refuses_to_steal_a_key_silently)
{
    // Silently taking a key leaves the user with a command that stopped
    // working for no visible reason.
    ShortcutProfile profile("custom");
    VCC_CHECK(profile.bind(Command::SelectDurationQuarter, KeyStroke{Key::Q, 0}));
    VCC_CHECK(!profile.bind(Command::SelectDurationHalf, KeyStroke{Key::Q, 0}));

    VCC_CHECK_EQ(profile.command_for(KeyStroke{Key::Q, 0}), Command::SelectDurationQuarter);
    VCC_CHECK(!profile.is_bound(Command::SelectDurationHalf));
}

VCC_TEST(rebinding_takes_the_key_and_reports_what_lost_it)
{
    // The deliberate version, so the UI can tell the user what changed.
    ShortcutProfile profile("custom");
    profile.bind(Command::SelectDurationQuarter, KeyStroke{Key::Q, 0});

    const Command displaced = profile.rebind(Command::SelectDurationHalf, KeyStroke{Key::Q, 0});
    VCC_CHECK_EQ(displaced, Command::SelectDurationQuarter);
    VCC_CHECK_EQ(profile.command_for(KeyStroke{Key::Q, 0}), Command::SelectDurationHalf);
    VCC_CHECK(!profile.is_bound(Command::SelectDurationQuarter));

    // Rebinding a command to the key it already has displaces nothing.
    VCC_CHECK_EQ(profile.rebind(Command::SelectDurationHalf, KeyStroke{Key::Q, 0}),
                 Command::None);
}

VCC_TEST(a_custom_profile_has_no_conflicts_by_construction)
{
    ShortcutProfile profile("custom");
    for (const Command command : all_commands()) {
        // Bind everything to the same key; all but the first must be refused.
        profile.bind(command, KeyStroke{Key::A, 0});
    }
    VCC_CHECK_EQ(profile.binding_count(), static_cast<std::size_t>(1));
    VCC_CHECK(profile.conflicts().empty());
}

VCC_TEST(keystroke_text_round_trips)
{
    // The text form is what goes into the settings file.
    const KeyStroke strokes[] = {
        KeyStroke{Key::A, 0},
        KeyStroke{Key::Keypad5, 0},
        KeyStroke{Key::Z, to_mask(Modifier::Control)},
        KeyStroke{Key::Z, combine(to_mask(Modifier::Control), to_mask(Modifier::Shift))},
        KeyStroke{Key::Period, to_mask(Modifier::Alt)},
        KeyStroke{Key::F12, combine(to_mask(Modifier::Command), to_mask(Modifier::Alt))},
        KeyStroke{Key::ArrowLeft, 0},
    };

    for (const KeyStroke& original : strokes) {
        const std::string text = original.to_text();
        VCC_CHECK(!text.empty());

        KeyStroke parsed;
        VCC_CHECK_MSG(KeyStroke::from_text(text, parsed), "could not parse " + text);
        VCC_CHECK_MSG(parsed == original, "round trip changed " + text + " into "
                                              + parsed.to_text());
    }
}

VCC_TEST(malformed_keystroke_text_is_refused)
{
    // Silently binding the wrong key would be worse than refusing to load
    // a corrupt line.
    KeyStroke parsed;
    VCC_CHECK(!KeyStroke::from_text("", parsed));
    VCC_CHECK(!KeyStroke::from_text("NotAKey", parsed));
    VCC_CHECK(!KeyStroke::from_text("Hyper+A", parsed));
    VCC_CHECK(!KeyStroke::from_text("Ctrl+", parsed));
    VCC_CHECK(!KeyStroke::from_text("Ctrl+NotAKey", parsed));
}

VCC_TEST(every_command_has_a_name_and_a_description)
{
    // Both reach the shortcut editor; a blank entry would be a dead row.
    for (const Command command : all_commands()) {
        const std::string name = to_string(command);
        VCC_CHECK(!name.empty());
        VCC_CHECK(name != "None");
        VCC_CHECK(std::string(describe(command)).size() > 0);

        Command parsed = Command::None;
        VCC_CHECK_MSG(command_from_string(name, parsed), "cannot parse command name " + name);
        VCC_CHECK_EQ(parsed, command);
    }
}

VCC_TEST(command_names_are_unique)
{
    // They are the on-disk identifiers, so a duplicate would silently
    // rebind the wrong command on load.
    std::set<std::string> names;
    for (const Command command : all_commands())
        VCC_CHECK_MSG(names.insert(to_string(command)).second,
                      std::string("duplicate command name ") + to_string(command));
}

// ===========================================================================
// Instrument definitions
// ===========================================================================

VCC_TEST(the_standard_instruments_are_all_valid)
{
    const std::vector<InstrumentDefinition>& instruments = standard_instruments();
    VCC_CHECK(instruments.size() > 30);

    for (const InstrumentDefinition& definition : instruments) {
        VCC_CHECK_MSG(definition.is_valid(), "invalid definition: " + definition.name);
        VCC_CHECK(!definition.short_name.empty());
        VCC_CHECK(definition.family != InstrumentFamily::Unknown);
        VCC_CHECK(definition.lowest_note < definition.highest_note);
    }
}

VCC_TEST(instrument_names_are_unique)
{
    std::set<std::string> names;
    for (const InstrumentDefinition& definition : standard_instruments())
        VCC_CHECK_MSG(names.insert(definition.name).second,
                      "duplicate instrument " + definition.name);
}

VCC_TEST(transposing_instruments_have_the_right_transposition)
{
    // Getting these wrong transposes a whole part silently, which is the
    // worst kind of bug in notation software.
    struct Expected { const char* name; int semitones; };
    static const Expected kExpected[] = {
        {"Flute", 0},
        {"Oboe", 0},
        {"Clarinet in Bb", -2},     // Written C sounds Bb.
        {"Clarinet in A", -3},
        {"Bass Clarinet", -14},     // A major ninth below.
        {"Bassoon", 0},
        {"English Horn", -7},       // A perfect fifth below.
        {"Horn in F", -7},
        {"Trumpet in Bb", -2},
        {"Trumpet in C", 0},
        {"Piccolo", 12},            // Sounds an octave above.
        {"Alto Saxophone", -9},     // A major sixth below.
        {"Tenor Saxophone", -14},
        {"Violin I", 0},
        {"Viola", 0},
        {"Cello", 0},
    };

    for (const Expected& expected : kExpected) {
        const InstrumentDefinition* definition = find_standard_instrument(expected.name);
        VCC_CHECK_MSG(definition != nullptr, std::string("missing instrument ") + expected.name);
        if (definition == nullptr)
            continue;
        VCC_CHECK_MSG(definition->transposition_semitones == expected.semitones,
                      std::string(expected.name) + " transposition is "
                          + std::to_string(definition->transposition_semitones) + ", expected "
                          + std::to_string(expected.semitones));
    }
}

VCC_TEST(octave_transposing_instruments_are_marked_with_an_octave_shift)
{
    // Double bass, guitar and vocal tenor all sound an octave below where
    // they are written.
    for (const char* name : {"Double Bass", "Guitar", "Tenor"}) {
        const InstrumentDefinition* definition = find_standard_instrument(name);
        VCC_CHECK_MSG(definition != nullptr, std::string("missing ") + name);
        if (definition == nullptr)
            continue;
        VCC_CHECK_MSG(definition->octave_shift == -1,
                      std::string(name) + " is not marked as sounding an octave lower");
        VCC_CHECK_EQ(definition->total_transposition(), -12);
    }
}

VCC_TEST(clefs_match_the_instrument)
{
    VCC_CHECK_EQ(find_standard_instrument("Viola")->clef, vcc::notation::Clef::Alto);
    VCC_CHECK_EQ(find_standard_instrument("Trombone")->clef, vcc::notation::Clef::Tenor);
    VCC_CHECK_EQ(find_standard_instrument("Cello")->clef, vcc::notation::Clef::Bass);
    VCC_CHECK_EQ(find_standard_instrument("Flute")->clef, vcc::notation::Clef::Treble);
    VCC_CHECK_EQ(find_standard_instrument("Percussion")->clef, vcc::notation::Clef::Percussion);
}

VCC_TEST(keyboard_and_harp_use_a_grand_staff)
{
    // Spec 5.1 requires a grand staff for piano and similar.
    for (const char* name : {"Piano", "Harp", "Celesta"}) {
        const InstrumentDefinition* definition = find_standard_instrument(name);
        VCC_CHECK_MSG(definition != nullptr && definition->uses_grand_staff,
                      std::string(name) + " should use a grand staff");
        VCC_CHECK_EQ(definition->lower_clef, vcc::notation::Clef::Bass);
    }

    VCC_CHECK(!find_standard_instrument("Flute")->uses_grand_staff);
}

VCC_TEST(score_order_follows_convention)
{
    // Woodwinds above brass above percussion above strings, as a score is
    // laid out.
    const auto order_of = [](const char* name) {
        return find_standard_instrument(name)->score_order;
    };

    VCC_CHECK(order_of("Flute") < order_of("Horn in F"));
    VCC_CHECK(order_of("Horn in F") < order_of("Timpani"));
    VCC_CHECK(order_of("Timpani") < order_of("Violin I"));
    VCC_CHECK(order_of("Violin I") < order_of("Viola"));
    VCC_CHECK(order_of("Viola") < order_of("Cello"));
    VCC_CHECK(order_of("Cello") < order_of("Double Bass"));
    VCC_CHECK(order_of("Piccolo") < order_of("Flute"));
}

VCC_TEST(range_checks_reflect_the_instrument)
{
    const InstrumentDefinition* violin = find_standard_instrument("Violin I");
    VCC_CHECK(violin->can_play(55));    // Open G, the lowest string.
    VCC_CHECK(!violin->can_play(48));   // Below the instrument.
    VCC_CHECK(violin->can_play(96));

    const InstrumentDefinition* unknown = find_standard_instrument("No Such Instrument");
    VCC_CHECK(unknown == nullptr);      // Never invented.
}

// ===========================================================================
// Library catalogue and favorites
// ===========================================================================

namespace {

LibraryPatch make_patch(const char* patch_name, const char* instrument,
                        const char* player = "Kontakt", const char* identifier = "")
{
    LibraryPatch patch;
    patch.patch_name = patch_name;
    patch.instrument_name = instrument;
    patch.provider = "Example Provider";
    patch.player = player;
    patch.library = "Example Library";
    patch.patch_identifier = identifier;
    patch.family = InstrumentFamily::Strings;
    patch.expression_map = vcc::notation::make_orchestral_cc_expression_map();
    return patch;
}

}  // namespace

VCC_TEST(a_patch_can_be_saved_and_found)
{
    // Acceptance criterion 8, first half.
    InstrumentCatalog catalog;
    const std::uint64_t id = catalog.add_patch(make_patch("Violin Pizzicato", "Violin I"));

    VCC_CHECK(id != 0);
    VCC_CHECK_EQ(catalog.patch_count(), static_cast<std::size_t>(1));

    const LibraryPatch* stored = catalog.find_patch(id);
    VCC_CHECK(stored != nullptr);
    VCC_CHECK(stored->patch_name == "Violin Pizzicato");
    VCC_CHECK(stored->display_name().find("Violin Pizzicato") != std::string::npos);
    VCC_CHECK(stored->display_name().find("Kontakt") != std::string::npos);
}

VCC_TEST(an_invalid_patch_is_refused)
{
    InstrumentCatalog catalog;

    LibraryPatch nameless = make_patch("", "Violin I");
    VCC_CHECK_EQ(catalog.add_patch(nameless), static_cast<std::uint64_t>(0));

    LibraryPatch over_rated = make_patch("Violin", "Violin I");
    over_rated.rating = 9;
    VCC_CHECK_EQ(catalog.add_patch(over_rated), static_cast<std::uint64_t>(0));

    // An unusable expression map would render silence or nonsense.
    LibraryPatch bad_map = make_patch("Violin", "Violin I");
    bad_map.expression_map.dynamic_velocity[0] = 0;
    VCC_CHECK_EQ(catalog.add_patch(bad_map), static_cast<std::uint64_t>(0));

    VCC_CHECK_EQ(catalog.patch_count(), static_cast<std::size_t>(0));
}

VCC_TEST(marking_a_favorite_replaces_the_previous_one_for_that_instrument)
{
    // One favorite per instrument is what makes "recall the favorite"
    // unambiguous (spec 10.1).
    InstrumentCatalog catalog;
    const std::uint64_t first = catalog.add_patch(make_patch("Violin Legato", "Violin I"));
    const std::uint64_t second = catalog.add_patch(make_patch("Violin Pizzicato", "Violin I"));

    VCC_CHECK(catalog.set_favorite(first));
    VCC_CHECK(catalog.find_patch(first)->is_favorite);

    VCC_CHECK(catalog.set_favorite(second));
    VCC_CHECK(catalog.find_patch(second)->is_favorite);
    VCC_CHECK_MSG(!catalog.find_patch(first)->is_favorite,
                  "two patches are favorite for the same instrument");

    const LibraryPatch* favorite = catalog.favorite_for_instrument("Violin I");
    VCC_CHECK(favorite != nullptr);
    VCC_CHECK_EQ(favorite->id, second);
}

VCC_TEST(favorites_for_different_instruments_coexist)
{
    InstrumentCatalog catalog;
    const std::uint64_t violin = catalog.add_patch(make_patch("Violin Pizz", "Violin I"));
    const std::uint64_t oboe = catalog.add_patch(make_patch("Oboe Solo", "Oboe"));

    VCC_CHECK(catalog.set_favorite(violin));
    VCC_CHECK(catalog.set_favorite(oboe));

    VCC_CHECK(catalog.find_patch(violin)->is_favorite);
    VCC_CHECK(catalog.find_patch(oboe)->is_favorite);
    VCC_CHECK_EQ(catalog.favorites().size(), static_cast<std::size_t>(2));
}

VCC_TEST(a_patch_added_as_favorite_does_not_create_a_second_favorite)
{
    InstrumentCatalog catalog;
    LibraryPatch first = make_patch("Violin A", "Violin I");
    first.is_favorite = true;
    const std::uint64_t first_id = catalog.add_patch(first);

    LibraryPatch second = make_patch("Violin B", "Violin I");
    second.is_favorite = true;
    const std::uint64_t second_id = catalog.add_patch(second);

    VCC_CHECK(!catalog.find_patch(first_id)->is_favorite);
    VCC_CHECK(catalog.find_patch(second_id)->is_favorite);
    VCC_CHECK_EQ(catalog.favorites().size(), static_cast<std::size_t>(1));
}

VCC_TEST(automatic_loading_is_only_claimed_when_it_is_possible)
{
    // Spec 10.1: where automatic loading is unavailable, identify the patch
    // and offer a guided workflow rather than substituting a sound.
    InstrumentCatalog catalog;

    const std::uint64_t loadable =
        catalog.add_patch(make_patch("Violin Pizz", "Violin I", "Kontakt", "Library/Violin.nki"));
    VCC_CHECK(catalog.find_patch(loadable)->can_attempt_automatic_load());

    const std::uint64_t manual =
        catalog.add_patch(make_patch("Oboe Solo", "Oboe", "Kontakt", ""));
    VCC_CHECK(!catalog.find_patch(manual)->can_attempt_automatic_load());

    LibraryPatch no_player = make_patch("Horn", "Horn in F", "", "something");
    const std::uint64_t unplayable = catalog.add_patch(no_player);
    VCC_CHECK(!catalog.find_patch(unplayable)->can_attempt_automatic_load());
}

VCC_TEST(the_catalogue_can_be_searched_and_filtered)
{
    InstrumentCatalog catalog;

    LibraryPatch pizz = make_patch("Violin Pizzicato", "Violin I");
    pizz.tags = {"pizzicato", "short"};
    catalog.add_patch(pizz);

    LibraryPatch legato = make_patch("Violin Legato", "Violin I");
    legato.tags = {"warm", "long"};
    catalog.add_patch(legato);

    LibraryPatch oboe = make_patch("Oboe Solo", "Oboe", "Opus");
    oboe.family = InstrumentFamily::Woodwind;
    oboe.tags = {"solo", "intimate"};
    catalog.add_patch(oboe);

    VCC_CHECK_EQ(catalog.patches_for_instrument("Violin I").size(), static_cast<std::size_t>(2));
    VCC_CHECK_EQ(catalog.patches_for_family(InstrumentFamily::Woodwind).size(),
                 static_cast<std::size_t>(1));
    VCC_CHECK_EQ(catalog.patches_with_tag("pizzicato").size(), static_cast<std::size_t>(1));

    // Search is case-insensitive and covers names, libraries and tags.
    VCC_CHECK_EQ(catalog.search("violin").size(), static_cast<std::size_t>(2));
    VCC_CHECK_EQ(catalog.search("PIZZICATO").size(), static_cast<std::size_t>(1));
    VCC_CHECK_EQ(catalog.search("opus").size(), static_cast<std::size_t>(1));
    VCC_CHECK_EQ(catalog.search("").size(), static_cast<std::size_t>(3));
    VCC_CHECK_EQ(catalog.search("nothing here").size(), static_cast<std::size_t>(0));

    const std::vector<std::string> players = catalog.players();
    VCC_CHECK_EQ(players.size(), static_cast<std::size_t>(2));
}

VCC_TEST(updating_a_patch_keeps_its_id_and_its_favorite_status)
{
    InstrumentCatalog catalog;
    const std::uint64_t id = catalog.add_patch(make_patch("Violin Legato", "Violin I"));
    catalog.set_favorite(id);

    LibraryPatch revised = *catalog.find_patch(id);
    revised.patch_name = "Violin Legato (edited)";
    revised.rating = 5;
    VCC_CHECK(catalog.update_patch(id, revised));

    const LibraryPatch* stored = catalog.find_patch(id);
    VCC_CHECK_EQ(stored->id, id);
    VCC_CHECK(stored->patch_name == "Violin Legato (edited)");
    VCC_CHECK_EQ(stored->rating, static_cast<std::uint8_t>(5));
    VCC_CHECK_MSG(stored->is_favorite, "an edit silently cleared the favorite flag");
}

VCC_TEST(patch_ids_are_not_reused_after_clearing)
{
    // A stored template referring to a patch id must not resolve to a
    // different patch later.
    InstrumentCatalog catalog;
    const std::uint64_t before = catalog.add_patch(make_patch("A", "Violin I"));
    catalog.clear();
    const std::uint64_t after = catalog.add_patch(make_patch("B", "Violin I"));
    VCC_CHECK(after != before);
}

// ===========================================================================
// Session templates
// ===========================================================================

VCC_TEST(every_ensemble_generates_a_plausible_template)
{
    static const InstrumentationRequest::Ensemble kEnsembles[] = {
        InstrumentationRequest::Ensemble::SmallOrchestra,
        InstrumentationRequest::Ensemble::FullOrchestra,
        InstrumentationRequest::Ensemble::StringQuartet,
        InstrumentationRequest::Ensemble::WindQuintet,
        InstrumentationRequest::Ensemble::SoloPiano,
        InstrumentationRequest::Ensemble::PianoTrio,
        InstrumentationRequest::Ensemble::StringOrchestra,
        InstrumentationRequest::Ensemble::ConcertBand,
        InstrumentationRequest::Ensemble::HybridRhythmSection,
    };

    const InstrumentCatalog empty_catalog;

    for (const InstrumentationRequest::Ensemble ensemble : kEnsembles) {
        InstrumentationRequest request;
        request.ensemble = ensemble;

        const SessionTemplate generated = generate_template(request, empty_catalog);
        VCC_CHECK_MSG(generated.is_valid(),
                      std::string("invalid template for ") + to_string(ensemble));
        VCC_CHECK_MSG(!generated.tracks.empty(),
                      std::string("no tracks for ") + to_string(ensemble));

        // Every track must carry usable notation defaults, since spec 8
        // requires the staff be right before a note is entered.
        for (const TemplateTrack& track : generated.tracks) {
            VCC_CHECK(!track.track_name.empty());
            VCC_CHECK(!track.instrument_name.empty());
            VCC_CHECK(!track.patch_summary.empty());
        }
    }
}

VCC_TEST(a_generated_template_is_in_score_order)
{
    // A template that reads out of score order is immediately wrong to a
    // composer's eye.
    InstrumentationRequest request;
    request.ensemble = InstrumentationRequest::Ensemble::FullOrchestra;

    const SessionTemplate generated = generate_template(request, InstrumentCatalog{});

    for (std::size_t index = 1; index < generated.tracks.size(); ++index) {
        VCC_CHECK_MSG(generated.tracks[index].score_order
                          >= generated.tracks[index - 1].score_order,
                      "tracks are out of score order at " + std::to_string(index));
    }

    // Woodwinds first, strings last.
    VCC_CHECK_EQ(generated.tracks.front().family, InstrumentFamily::Woodwind);
    VCC_CHECK_EQ(generated.tracks.back().family, InstrumentFamily::Strings);
}

VCC_TEST(notation_defaults_come_from_the_instrument)
{
    // Spec 8: "Assign staff type, clef, transposition and notation defaults
    // based on instrument."
    InstrumentationRequest request;
    request.ensemble = InstrumentationRequest::Ensemble::SmallOrchestra;

    const SessionTemplate generated = generate_template(request, InstrumentCatalog{});

    const auto find_track = [&generated](const std::string& name) -> const TemplateTrack* {
        for (const TemplateTrack& track : generated.tracks) {
            if (track.instrument_name == name)
                return &track;
        }
        return nullptr;
    };

    const TemplateTrack* viola = find_track("Viola");
    VCC_CHECK(viola != nullptr);
    VCC_CHECK_EQ(viola->clef, vcc::notation::Clef::Alto);

    const TemplateTrack* clarinet = find_track("Clarinet in Bb");
    VCC_CHECK(clarinet != nullptr);
    VCC_CHECK_EQ(clarinet->transposition_semitones, -2);

    const TemplateTrack* bass = find_track("Double Bass");
    VCC_CHECK(bass != nullptr);
    VCC_CHECK_EQ(bass->octave_shift, -1);
}

VCC_TEST(solo_piano_generates_a_grand_staff_track)
{
    InstrumentationRequest request;
    request.ensemble = InstrumentationRequest::Ensemble::SoloPiano;

    const SessionTemplate generated = generate_template(request, InstrumentCatalog{});
    VCC_CHECK_EQ(generated.tracks.size(), static_cast<std::size_t>(1));
    VCC_CHECK(generated.tracks[0].uses_grand_staff);
    VCC_CHECK_EQ(generated.tracks[0].clef, vcc::notation::Clef::Treble);
    VCC_CHECK_EQ(generated.tracks[0].lower_clef, vcc::notation::Clef::Bass);
}

VCC_TEST(a_template_recalls_saved_favorites)
{
    // Acceptance criteria 8 and 9 together: a saved preferred patch becomes
    // the default choice when a session is built.
    InstrumentCatalog catalog;
    const std::uint64_t pizz = catalog.add_patch(
        make_patch("Violin Pizzicato", "Violin I", "Kontakt", "Library/Pizz.nki"));
    catalog.set_favorite(pizz);

    const std::uint64_t oboe = catalog.add_patch(make_patch("Oboe Solo", "Oboe", "Opus", ""));
    catalog.set_favorite(oboe);

    InstrumentationRequest request;
    request.ensemble = InstrumentationRequest::Ensemble::SmallOrchestra;
    const SessionTemplate generated = generate_template(request, catalog);

    bool found_violin = false;
    bool found_oboe = false;
    for (const TemplateTrack& track : generated.tracks) {
        if (track.instrument_name == "Violin I") {
            found_violin = true;
            VCC_CHECK_EQ(track.patch_id, pizz);
            VCC_CHECK(track.patch_summary.find("Violin Pizzicato") != std::string::npos);
            VCC_CHECK(track.patch_loads_automatically);
        }
        if (track.instrument_name == "Oboe") {
            found_oboe = true;
            VCC_CHECK_EQ(track.patch_id, oboe);
            // No identifier, so loading is guided rather than automatic.
            VCC_CHECK(!track.patch_loads_automatically);
            VCC_CHECK(track.patch_summary.find("manually") != std::string::npos);
        }
    }
    VCC_CHECK(found_violin);
    VCC_CHECK(found_oboe);

    // The tracks needing attention are reported, so the UI can prompt.
    const std::vector<std::string> guided = generated.tracks_needing_guided_loading();
    VCC_CHECK_EQ(guided.size(), static_cast<std::size_t>(1));
}

VCC_TEST(a_track_with_no_saved_preference_is_flagged_not_filled)
{
    // Spec 10.1 forbids silently substituting another sound.
    InstrumentationRequest request;
    request.ensemble = InstrumentationRequest::Ensemble::StringQuartet;

    const SessionTemplate generated = generate_template(request, InstrumentCatalog{});

    VCC_CHECK_EQ(generated.tracks_without_a_saved_patch().size(), generated.tracks.size());
    for (const TemplateTrack& track : generated.tracks) {
        VCC_CHECK_EQ(track.patch_id, static_cast<std::uint64_t>(0));
        VCC_CHECK(track.patch_summary.find("No saved preference") != std::string::npos);
    }
}

VCC_TEST(custom_instrumentation_is_honoured)
{
    InstrumentationRequest request;
    request.ensemble = InstrumentationRequest::Ensemble::Custom;
    request.additional_instruments = {"Harp", "Celesta", "Violin I"};

    const SessionTemplate generated = generate_template(request, InstrumentCatalog{});
    VCC_CHECK_EQ(generated.tracks.size(), static_cast<std::size_t>(3));
}

VCC_TEST(an_unknown_instrument_gets_a_track_but_no_invented_transposition)
{
    // A guessed transposition would silently transpose the part.
    InstrumentationRequest request;
    request.ensemble = InstrumentationRequest::Ensemble::Custom;
    request.additional_instruments = {"Theremin"};

    const SessionTemplate generated = generate_template(request, InstrumentCatalog{});
    VCC_CHECK_EQ(generated.tracks.size(), static_cast<std::size_t>(1));
    VCC_CHECK_EQ(generated.tracks[0].transposition_semitones, 0);
    VCC_CHECK_EQ(generated.tracks[0].octave_shift, 0);
    VCC_CHECK(generated.tracks[0].patch_summary.find("Unknown instrument") != std::string::npos);
}

VCC_TEST(divided_strings_produce_two_desks_per_section)
{
    InstrumentationRequest request;
    request.ensemble = InstrumentationRequest::Ensemble::StringQuartet;
    request.divide_strings = true;

    const SessionTemplate generated = generate_template(request, InstrumentCatalog{});
    // Four sections, each divided in two.
    VCC_CHECK_EQ(generated.tracks.size(), static_cast<std::size_t>(8));

    // Each desk keeps its parent instrument's notation defaults.
    for (const TemplateTrack& track : generated.tracks) {
        VCC_CHECK(track.track_name.find("div.") != std::string::npos);
        VCC_CHECK(track.family == InstrumentFamily::Strings);
    }
}

VCC_TEST(midi_channels_are_assigned_and_wrap)
{
    InstrumentationRequest request;
    request.ensemble = InstrumentationRequest::Ensemble::FullOrchestra;
    request.assign_channels = true;

    const SessionTemplate generated = generate_template(request, InstrumentCatalog{});
    for (const TemplateTrack& track : generated.tracks)
        VCC_CHECK(track.midi_channel < 16);
}

VCC_TEST(templates_can_be_saved_and_recalled)
{
    // Acceptance criterion 9.
    TemplateLibrary library;

    InstrumentationRequest request;
    request.ensemble = InstrumentationRequest::Ensemble::StringQuartet;
    SessionTemplate quartet = generate_template(request, InstrumentCatalog{});
    quartet.name = "My quartet";

    VCC_CHECK(library.save(quartet));
    VCC_CHECK_EQ(library.size(), static_cast<std::size_t>(1));

    const SessionTemplate* recalled = library.find("My quartet");
    VCC_CHECK(recalled != nullptr);
    VCC_CHECK_EQ(recalled->tracks.size(), quartet.tracks.size());
    VCC_CHECK(recalled->tracks[0].track_name == quartet.tracks[0].track_name);

    // Saving the same name replaces rather than duplicating.
    VCC_CHECK(library.save(quartet));
    VCC_CHECK_EQ(library.size(), static_cast<std::size_t>(1));

    VCC_CHECK(library.remove("My quartet"));
    VCC_CHECK(library.find("My quartet") == nullptr);
    VCC_CHECK(!library.remove("My quartet"));
}

VCC_TEST(an_invalid_template_is_refused)
{
    TemplateLibrary library;

    SessionTemplate nameless;
    VCC_CHECK(!library.save(nameless));

    SessionTemplate bad_tempo;
    bad_tempo.name = "Bad";
    bad_tempo.tempo_bpm = 0.0;
    VCC_CHECK(!library.save(bad_tempo));

    SessionTemplate bad_meter;
    bad_meter.name = "Bad meter";
    bad_meter.time_signature = vcc::music::TimeSignature{4, 3};
    VCC_CHECK(!library.save(bad_meter));

    VCC_CHECK_EQ(library.size(), static_cast<std::size_t>(0));
}

// ===========================================================================
// Settings persistence
// ===========================================================================

VCC_TEST(settings_round_trip_through_text)
{
    SettingsStore store;
    SettingsSection& general = store.section("general");
    general.set("profile", "Sibelius style");
    general.set_int("recent_tempo", 144);
    general.set_double("gain", 0.7071067811865476);
    general.set_bool("follow_host", true);

    SettingsSection& patches = store.section("patches");
    patches.set("0.name", "Violin Pizzicato");
    patches.set("0.player", "Kontakt");
    patches.set("1.name", "Oboe Solo");

    const std::string text = store.to_text();
    VCC_CHECK(text.find("[general]") != std::string::npos);
    VCC_CHECK(text.find("version = " + std::to_string(kSettingsFormatVersion)) != std::string::npos);

    SettingsStore reloaded;
    std::size_t skipped = 99;
    VCC_CHECK(reloaded.from_text(text, &skipped));
    VCC_CHECK_EQ(skipped, static_cast<std::size_t>(0));
    VCC_CHECK_EQ(reloaded.version(), kSettingsFormatVersion);

    const SettingsSection* general_back = reloaded.find_section("general");
    VCC_CHECK(general_back != nullptr);
    VCC_CHECK(general_back->get("profile") == "Sibelius style");
    VCC_CHECK_EQ(general_back->get_int("recent_tempo"), static_cast<std::int64_t>(144));
    VCC_CHECK_NEAR(general_back->get_double("gain"), 0.7071067811865476, 1e-15);
    VCC_CHECK(general_back->get_bool("follow_host"));

    const SettingsSection* patches_back = reloaded.find_section("patches");
    VCC_CHECK(patches_back != nullptr);
    VCC_CHECK_EQ(patches_back->indices("0").size(), static_cast<std::size_t>(0));
    VCC_CHECK(patches_back->get("0.name") == "Violin Pizzicato");
    VCC_CHECK(patches_back->get("1.name") == "Oboe Solo");
}

VCC_TEST(indexed_keys_enumerate_their_list)
{
    // How nested structures are stored without recording a length.
    SettingsStore store;
    SettingsSection& section = store.section("template");
    section.set("track.0.name", "Violin I");
    section.set("track.0.clef", "treble");
    section.set("track.1.name", "Viola");
    section.set("track.7.name", "Cello");
    section.set("other", "ignored");

    const std::vector<std::size_t> indices = section.indices("track");
    VCC_CHECK_EQ(indices.size(), static_cast<std::size_t>(3));
    VCC_CHECK_EQ(indices[0], static_cast<std::size_t>(0));
    VCC_CHECK_EQ(indices[1], static_cast<std::size_t>(1));
    VCC_CHECK_EQ(indices[2], static_cast<std::size_t>(7));
}

VCC_TEST(values_needing_escapes_survive_a_round_trip)
{
    // A patch path with a backslash, a note with a newline, a name with
    // padding: all things a user can produce.
    const std::string awkward[] = {
        "C:\\Libraries\\Violin\\Pizz.nki",
        "line one\nline two",
        "  leading and trailing  ",
        "tab\there",
        "equals = inside",
        "[brackets]",
        "# not a comment",
        "",
    };

    SettingsStore store;
    SettingsSection& section = store.section("awkward");
    for (std::size_t index = 0; index < sizeof(awkward) / sizeof(awkward[0]); ++index)
        section.set("key" + std::to_string(index), awkward[index]);

    SettingsStore reloaded;
    VCC_CHECK(reloaded.from_text(store.to_text()));

    const SettingsSection* back = reloaded.find_section("awkward");
    VCC_CHECK(back != nullptr);
    for (std::size_t index = 0; index < sizeof(awkward) / sizeof(awkward[0]); ++index) {
        const std::string key = "key" + std::to_string(index);
        VCC_CHECK_MSG(back->get(key) == awkward[index],
                      "value " + std::to_string(index) + " round-tripped to '"
                          + back->get(key) + "' instead of '" + awkward[index] + "'");
    }
}

VCC_TEST(escaping_is_reversible_for_arbitrary_bytes)
{
    for (int byte = 1; byte < 128; ++byte) {
        const std::string original(3, static_cast<char>(byte));
        VCC_CHECK_MSG(unescape_value(escape_value(original)) == original,
                      "escaping is not reversible for byte " + std::to_string(byte));
    }
}

VCC_TEST(a_malformed_line_is_skipped_rather_than_failing_the_file)
{
    // This file holds work the user cannot easily recreate. Losing all of
    // it because one line is corrupt would be the worst behaviour.
    const std::string text =
        "version = 1\n"
        "[good]\n"
        "key = value\n"
        "this line has no equals sign\n"
        "[unterminated\n"
        "= no key\n"
        "another = fine\n";

    SettingsStore store;
    std::size_t skipped = 0;
    VCC_CHECK(store.from_text(text, &skipped));
    VCC_CHECK_EQ(skipped, static_cast<std::size_t>(3));

    const SettingsSection* good = store.find_section("good");
    VCC_CHECK(good != nullptr);
    VCC_CHECK(good->get("key") == "value");
    VCC_CHECK(good->get("another") == "fine");
}

VCC_TEST(a_document_with_no_version_is_refused)
{
    // Without a version the layout is unknown, and guessing risks
    // misreading the user's data.
    SettingsStore store;
    VCC_CHECK(!store.from_text("[section]\nkey = value\n"));
    VCC_CHECK(!store.from_text(""));
    VCC_CHECK(!store.from_text("version = not a number\n"));
}

VCC_TEST(a_future_version_is_reported_rather_than_assumed)
{
    // A newer file is readable; the caller decides whether to migrate.
    SettingsStore store;
    VCC_CHECK(store.from_text("version = 99\n[section]\nkey = value\n"));
    VCC_CHECK_EQ(store.version(), 99);
    VCC_CHECK(store.find_section("section")->get("key") == "value");
}

namespace {

// Synthetic steps, since there is one real version: what a rename and a
// move look like, which is what the first real step will be.
void rename_zoom(SettingsStore& store, std::vector<std::string>&)
{
    SettingsSection& workspace = store.section("workspace");
    if (workspace.has("zoom")) {
        workspace.set("notation.zoom", workspace.get("zoom"));
        workspace.remove("zoom");
    }
}

void move_font_dir(SettingsStore& store, std::vector<std::string>& notes)
{
    SettingsSection& import = store.section("import");
    if (import.has("font_dir")) {
        store.section("notation").set("font_dir", import.get("font_dir"));
        import.remove("font_dir");
    }
    if (import.has("swing")) {
        import.remove("swing");
        notes.push_back("the swing setting has no place in format 3 and was dropped");
    }
}

const std::vector<SettingsMigrationStep> kTwoSteps = {
    {1, "the zoom moved under notation", &rename_zoom},
    {2, "the font directory moved to the notation section", &move_font_dir},
};

}  // namespace

VCC_TEST(an_older_settings_file_is_brought_up_to_date_one_step_at_a_time)
{
    SettingsStore store;
    VCC_CHECK(store.from_text("version = 1\n[workspace]\nzoom = 150\n[import]\nfont_dir = fonts\n"
                              "swing = 0.6\ngrid = eighth\n"));

    const SettingsMigrationResult result = migrate_settings(store, kTwoSteps, 3);
    VCC_CHECK(result.migrated());
    VCC_CHECK(!result.newer);
    VCC_CHECK(!result.stranded);
    VCC_CHECK_EQ(result.from, 1);
    VCC_CHECK_EQ(result.to, 3);
    VCC_CHECK_EQ(store.version(), 3);

    // Each step's one line, in order, and what a step said for itself.
    VCC_CHECK_EQ(result.notes.size(), std::size_t{3});
    if (result.notes.size() == 3) {
        VCC_CHECK_MSG(result.notes[0].find("format 1 to 2") != std::string::npos, result.notes[0]);
        VCC_CHECK_MSG(result.notes[0].find("zoom") != std::string::npos, result.notes[0]);
        VCC_CHECK_MSG(result.notes[1].find("format 2 to 3") != std::string::npos, result.notes[1]);
        VCC_CHECK_MSG(result.notes[2].find("swing") != std::string::npos, result.notes[2]);
    }

    // The content followed: moved, renamed, dropped, and the rest untouched.
    VCC_CHECK(store.find_section("workspace")->get("notation.zoom") == "150");
    VCC_CHECK(!store.find_section("workspace")->has("zoom"));
    VCC_CHECK(store.find_section("notation")->get("font_dir") == "fonts");
    VCC_CHECK(!store.find_section("import")->has("font_dir"));
    VCC_CHECK(!store.find_section("import")->has("swing"));
    VCC_CHECK(store.find_section("import")->get("grid") == "eighth");

    // And the written file declares the version it now is.
    VCC_CHECK(store.to_text().find("version = 3") != std::string::npos);

    // A file already at the target is untouched, notes and all.
    const SettingsMigrationResult again = migrate_settings(store, kTwoSteps, 3);
    VCC_CHECK(!again.migrated());
    VCC_CHECK(again.notes.empty());
    VCC_CHECK_EQ(store.version(), 3);

    // Starting part-way takes only the steps from there.
    SettingsStore later;
    VCC_CHECK(later.from_text("version = 2\n[import]\nfont_dir = f\n"));
    const SettingsMigrationResult from_two = migrate_settings(later, kTwoSteps, 3);
    VCC_CHECK_EQ(from_two.notes.size(), std::size_t{1});
    VCC_CHECK(later.find_section("notation") != nullptr
              && later.find_section("notation")->get("font_dir") == "f");
}

VCC_TEST(a_newer_settings_file_is_left_alone_and_a_stranded_one_goes_as_far_as_it_can)
{
    // Newer: nothing touched, version kept, so writing it back does not
    // claim it is older than it is.
    SettingsStore newer;
    VCC_CHECK(newer.from_text("version = 9\n[workspace]\nzoom = 150\n"));
    const SettingsMigrationResult left = migrate_settings(newer, kTwoSteps, 3);
    VCC_CHECK(left.newer);
    VCC_CHECK(!left.migrated());
    VCC_CHECK(!left.stranded);
    VCC_CHECK_EQ(newer.version(), 9);
    VCC_CHECK(newer.find_section("workspace")->get("zoom") == "150");

    // Stranded: the steps reach 2 and no further, and the store says 2 -
    // the version its content is, which is the only honest header.
    const std::vector<SettingsMigrationStep> one_step = {kTwoSteps.front()};
    SettingsStore old;
    VCC_CHECK(old.from_text("version = 1\n[workspace]\nzoom = 150\n"));
    const SettingsMigrationResult stuck = migrate_settings(old, one_step, 3);
    VCC_CHECK(stuck.stranded);
    VCC_CHECK(stuck.migrated());
    VCC_CHECK_EQ(stuck.to, 2);
    VCC_CHECK_EQ(old.version(), 2);
    VCC_CHECK(old.find_section("workspace")->get("notation.zoom") == "150");

    // A version before any this build knows - a hand-edited header - has
    // no step from it at all.
    SettingsStore zero;
    VCC_CHECK(zero.from_text("version = 0\n[workspace]\nzoom = 150\n"));
    const SettingsMigrationResult nowhere = migrate_settings(zero, kTwoSteps, 3);
    VCC_CHECK(nowhere.stranded);
    VCC_CHECK(!nowhere.migrated());
    VCC_CHECK_EQ(zero.version(), 0);

    VCC_CHECK(steps_lead_to(kTwoSteps, 3));
    VCC_CHECK(!steps_lead_to(one_step, 3));
    VCC_CHECK(steps_lead_to(one_step, 2));
}

VCC_TEST(this_builds_steps_lead_from_every_older_version_to_its_own)
{
    // The table may be empty only while there is one version: the first
    // format change has to come with its step, or every composer's file is
    // stranded at the old one.
    VCC_CHECK(steps_lead_to(settings_migration_steps(), kSettingsFormatVersion));
    VCC_CHECK_EQ(settings_migration_steps().size(),
                 static_cast<std::size_t>(kSettingsFormatVersion - 1));

    SettingsStore current;
    VCC_CHECK(current.from_text("version = " + std::to_string(kSettingsFormatVersion)
                                + "\n[import]\ngrid = eighth\n"));
    VCC_CHECK(!migrate_settings(current).migrated());
}

VCC_TEST(the_step_from_format_1_triples_every_tick_valued_key)
{
    // Format 1 counted 960 ticks to the quarter; format 2 counts 2880. A
    // key that holds ticks is named so - `ticks_` first, or `_tick` or
    // `_ticks` last - wherever it sits, and every one is multiplied by
    // three; anything else is left exactly as it was.
    SettingsStore store;
    VCC_CHECK(store.from_text("version = 1\n"
                              "[import]\nminimum_ticks = 240\ngrid = eighth\n"
                              "[notation]\nticks_before_playhead = 960\nticks_after_playhead = 11520\n"
                              "ticks_between_layouts = 1920\nzoom = 150\n"
                              "[workspace]\npart.0.path = a\npart.0.cursor_tick = 480\n"
                              "part.0.view_tick = 3840\npart.0.cursor_voice = 2\n"
                              "[expression_map.0]\nname = Strings\nhairpin.resolution_ticks = 120\n"
                              "length.minimum_ticks = 30\nlength.default_scale = 0.9\n"
                              "[odd]\nsome_ticks = soon\n"));

    const SettingsMigrationResult brought = migrate_settings(store);
    VCC_CHECK(brought.migrated());
    VCC_CHECK_EQ(brought.to, kSettingsFormatVersion);
    VCC_CHECK_EQ(store.version(), kSettingsFormatVersion);

    const auto value = [&](const char* section, const char* key) {
        const SettingsSection* found = store.find_section(section);
        return found == nullptr ? std::string("<none>") : found->get(key);
    };
    VCC_CHECK(value("import", "minimum_ticks") == "720");
    VCC_CHECK(value("import", "grid") == "eighth");
    VCC_CHECK(value("notation", "ticks_before_playhead") == "2880");
    VCC_CHECK(value("notation", "ticks_after_playhead") == "34560");
    VCC_CHECK(value("notation", "ticks_between_layouts") == "5760");
    VCC_CHECK(value("notation", "zoom") == "150");
    VCC_CHECK(value("workspace", "part.0.cursor_tick") == "1440");
    VCC_CHECK(value("workspace", "part.0.view_tick") == "11520");
    VCC_CHECK(value("workspace", "part.0.cursor_voice") == "2");
    VCC_CHECK(value("workspace", "part.0.path") == "a");
    VCC_CHECK(value("expression_map.0", "hairpin.resolution_ticks") == "360");
    VCC_CHECK(value("expression_map.0", "length.minimum_ticks") == "90");
    VCC_CHECK(value("expression_map.0", "length.default_scale") == "0.9");

    // One that is not a number is left as it was, and named.
    VCC_CHECK(value("odd", "some_ticks") == "soon");
    bool named = false;
    for (const std::string& note : brought.notes)
        named = named || note.find("some_ticks") != std::string::npos;
    VCC_CHECK(named);
}

VCC_TEST(typed_reads_fall_back_rather_than_throwing)
{
    SettingsStore store;
    SettingsSection& section = store.section("s");
    section.set("number", "not a number");
    section.set("real", "also not");
    section.set("flag", "perhaps");

    VCC_CHECK_EQ(section.get_int("number", -1), static_cast<std::int64_t>(-1));
    VCC_CHECK_NEAR(section.get_double("real", 2.5), 2.5, 1e-12);
    VCC_CHECK(section.get_bool("flag", true));
    VCC_CHECK(!section.get_bool("flag", false));

    // Absent keys use the fallback too.
    VCC_CHECK_EQ(section.get_int("absent", 7), static_cast<std::int64_t>(7));
    VCC_CHECK(section.get("absent", "default") == "default");
}

VCC_TEST(bool_accepts_the_forms_a_hand_editor_would_write)
{
    SettingsStore store;
    SettingsSection& section = store.section("s");
    section.set("a", "true");
    section.set("b", "1");
    section.set("c", "false");
    section.set("d", "0");

    VCC_CHECK(section.get_bool("a"));
    VCC_CHECK(section.get_bool("b"));
    VCC_CHECK(!section.get_bool("c", true));
    VCC_CHECK(!section.get_bool("d", true));
}

VCC_TEST(sections_keep_their_creation_order_so_files_are_stable)
{
    // A file that reorders itself on every save is unusable in version
    // control.
    SettingsStore store;
    store.section("zebra").set("k", "v");
    store.section("apple").set("k", "v");
    store.section("middle").set("k", "v");

    const std::vector<std::string> names = store.section_names();
    VCC_CHECK_EQ(names.size(), static_cast<std::size_t>(3));
    VCC_CHECK(names[0] == "zebra");
    VCC_CHECK(names[1] == "apple");
    VCC_CHECK(names[2] == "middle");

    // And writing twice produces the same bytes.
    VCC_CHECK(store.to_text() == store.to_text());
}

VCC_TEST(sections_can_be_removed)
{
    SettingsStore store;
    store.section("a").set("k", "v");
    store.section("b").set("k", "v");

    VCC_CHECK(store.remove_section("a"));
    VCC_CHECK(!store.has_section("a"));
    VCC_CHECK(store.has_section("b"));
    VCC_CHECK_EQ(store.section_names().size(), static_cast<std::size_t>(1));
    VCC_CHECK(!store.remove_section("a"));
}

VCC_TEST(a_shortcut_profile_survives_persistence)
{
    // The end-to-end requirement from spec 13: profiles persist.
    const ShortcutProfile original = make_sibelius_style_profile().duplicate("Mine");

    SettingsStore store;
    SettingsSection& section = store.section("shortcuts.Mine");
    section.set("name", original.name());
    for (const std::pair<Command, KeyStroke>& binding : original.bindings())
        section.set(to_string(binding.first), binding.second.to_text());

    SettingsStore reloaded;
    VCC_CHECK(reloaded.from_text(store.to_text()));

    const SettingsSection* back = reloaded.find_section("shortcuts.Mine");
    VCC_CHECK(back != nullptr);

    ShortcutProfile restored(back->get("name"));
    std::size_t restored_count = 0;
    for (const auto& entry : back->values()) {
        if (entry.first == "name")
            continue;
        Command command = Command::None;
        KeyStroke stroke;
        if (command_from_string(entry.first, command)
            && KeyStroke::from_text(entry.second, stroke)) {
            restored.rebind(command, stroke);
            ++restored_count;
        }
    }

    VCC_CHECK(restored.name() == "Mine");
    VCC_CHECK_EQ(restored_count, original.binding_count());
    VCC_CHECK_EQ(restored.binding_count(), original.binding_count());

    for (const std::pair<Command, KeyStroke>& binding : original.bindings()) {
        VCC_CHECK_MSG(restored.stroke_for(binding.first) == binding.second,
                      std::string("binding for ") + to_string(binding.first)
                          + " did not survive persistence");
    }
}

// --- Platform shortcut conventions (spec 11) --------------------------------

VCC_TEST(the_editing_shortcuts_use_the_key_the_platform_uses)
{
    // Ctrl+Z on macOS is bound to nothing: a Mac user's undo is Cmd+Z. So
    // the built-in profiles are built for a platform rather than assuming
    // one.
    const ShortcutProfile mac = make_vc_composer_profile(ShortcutPlatform::CommandKey);
    const ShortcutProfile other = make_vc_composer_profile(ShortcutPlatform::ControlKey);

    const KeyStroke mac_undo = mac.stroke_for(Command::Undo);
    VCC_CHECK(has_modifier(mac_undo.modifiers, Modifier::Command));
    VCC_CHECK(!has_modifier(mac_undo.modifiers, Modifier::Control));
    VCC_CHECK_EQ(mac_undo.to_text(), std::string("Cmd+Z"));

    const KeyStroke other_undo = other.stroke_for(Command::Undo);
    VCC_CHECK(has_modifier(other_undo.modifiers, Modifier::Control));
    VCC_CHECK(!has_modifier(other_undo.modifiers, Modifier::Command));
    VCC_CHECK_EQ(other_undo.to_text(), std::string("Ctrl+Z"));

    // And the command still resolves from the stroke the user will type.
    VCC_CHECK_EQ(mac.command_for(KeyStroke{Key::Z, to_mask(Modifier::Command)}), Command::Undo);
    VCC_CHECK_EQ(mac.command_for(KeyStroke{Key::Z, to_mask(Modifier::Control)}), Command::None);
}

VCC_TEST(the_platform_changes_the_modifier_and_nothing_else)
{
    // The two builds must differ in exactly one way. Anything else would
    // mean a Mac user and a Windows user were using different applications.
    for (const ShortcutPlatform platform :
         {ShortcutPlatform::CommandKey, ShortcutPlatform::ControlKey}) {
        const std::vector<ShortcutProfile> built = built_in_profiles(platform);
        const std::vector<ShortcutProfile> reference =
            built_in_profiles(platform == ShortcutPlatform::CommandKey
                                  ? ShortcutPlatform::ControlKey
                                  : ShortcutPlatform::CommandKey);

        VCC_CHECK_EQ(built.size(), reference.size());
        for (std::size_t index = 0; index < built.size(); ++index) {
            VCC_CHECK_EQ(built[index].name(), reference[index].name());
            VCC_CHECK_EQ(built[index].binding_count(), reference[index].binding_count());

            // Remapping one to the other's platform makes them identical.
            const ShortcutProfile remapped = reference[index].remapped_for(platform);
            VCC_CHECK_EQ(remapped.binding_count(), built[index].binding_count());

            for (const auto& binding : built[index].bindings()) {
                const KeyStroke expected = remapped.stroke_for(binding.first);
                VCC_CHECK_MSG(expected.key == binding.second.key,
                              to_string(binding.first));
                VCC_CHECK_MSG(expected.modifiers == binding.second.modifiers,
                              std::string(to_string(binding.first)) + ": "
                                  + expected.to_text() + " vs " + binding.second.to_text());
            }
        }
    }
}

VCC_TEST(no_built_in_binding_asks_for_both_control_and_command)
{
    // A stroke needing both is untypeable on Windows and Linux, where the
    // two are the same physical key.
    for (const ShortcutPlatform platform :
         {ShortcutPlatform::CommandKey, ShortcutPlatform::ControlKey}) {
        for (const ShortcutProfile& profile : built_in_profiles(platform)) {
            for (const auto& binding : profile.bindings()) {
                const bool control = has_modifier(binding.second.modifiers, Modifier::Control);
                const bool command = has_modifier(binding.second.modifiers, Modifier::Command);
                VCC_CHECK_MSG(!(control && command),
                              profile.name() + ": " + binding.second.to_text());

                // And on macOS nothing is left on Control, which is where a
                // binding the platform change missed would show up.
                if (platform == ShortcutPlatform::CommandKey)
                    VCC_CHECK_MSG(!control, profile.name() + ": " + binding.second.to_text());
            }
        }
    }
}

VCC_TEST(remapping_a_profile_is_reversible_and_leaves_other_keys_alone)
{
    ShortcutProfile profile = make_sibelius_style_profile(ShortcutPlatform::ControlKey);

    const ShortcutProfile to_mac = profile.remapped_for(ShortcutPlatform::CommandKey);
    const ShortcutProfile back = to_mac.remapped_for(ShortcutPlatform::ControlKey);

    for (const auto& binding : profile.bindings()) {
        const KeyStroke original = binding.second;
        const KeyStroke round_tripped = back.stroke_for(binding.first);
        VCC_CHECK_MSG(round_tripped.key == original.key, to_string(binding.first));
        VCC_CHECK_MSG(round_tripped.modifiers == original.modifiers,
                      to_string(binding.first));

        // Bindings with neither modifier are untouched by either direction.
        if (!has_modifier(original.modifiers, Modifier::Control)) {
            const KeyStroke on_mac = to_mac.stroke_for(binding.first);
            VCC_CHECK_MSG(on_mac.modifiers == original.modifiers, to_string(binding.first));
        }
    }

    // A duration on the number row is not a platform convention.
    VCC_CHECK_EQ(to_mac.stroke_for(Command::SelectDurationQuarter).modifiers,
                 profile.stroke_for(Command::SelectDurationQuarter).modifiers);
}

VCC_TEST(a_remapped_profile_has_no_conflicts)
{
    // Swapping a modifier could in principle collide two bindings onto one
    // stroke, which would silently lose one of them.
    for (const ShortcutProfile& profile : built_in_profiles(ShortcutPlatform::ControlKey)) {
        const ShortcutProfile mac = profile.remapped_for(ShortcutPlatform::CommandKey);
        VCC_CHECK_MSG(mac.conflicts().empty(), mac.name());
        VCC_CHECK_EQ(mac.binding_count(), profile.binding_count());
    }
}

VCC_TEST(the_host_platform_is_the_one_this_build_targets)
{
    // Whatever this build is, the default matches it - so a caller that
    // does not care gets the right thing.
    const ShortcutProfile defaulted = make_vc_composer_profile();
    const ShortcutProfile explicit_host = make_vc_composer_profile(host_shortcut_platform());
    VCC_CHECK_EQ(defaulted.stroke_for(Command::Undo).to_text(),
                 explicit_host.stroke_for(Command::Undo).to_text());

#if defined(__APPLE__)
    VCC_CHECK_EQ(host_shortcut_platform(), ShortcutPlatform::CommandKey);
    VCC_CHECK_EQ(defaulted.stroke_for(Command::Undo).to_text(), std::string("Cmd+Z"));
#else
    VCC_CHECK_EQ(host_shortcut_platform(), ShortcutPlatform::ControlKey);
    VCC_CHECK_EQ(defaulted.stroke_for(Command::Undo).to_text(), std::string("Ctrl+Z"));
#endif

    VCC_CHECK_EQ(std::string(to_string(ShortcutPlatform::CommandKey)), std::string("Command"));
    VCC_CHECK_EQ(std::string(to_string(ShortcutPlatform::ControlKey)), std::string("Control"));
}

VCC_TEST(a_profile_written_on_one_platform_survives_the_move_to_another)
{
    // The file keeps what was written; remapping is what makes it usable on
    // the other platform, and it is the caller's choice rather than
    // something persistence does behind their back.
    const ShortcutProfile mac =
        make_vc_composer_profile(ShortcutPlatform::CommandKey).duplicate("Mine");

    SettingsStore store;
    SettingsSection& section = store.section("shortcuts.Mine");
    for (const std::pair<Command, KeyStroke>& binding : mac.bindings())
        section.set(to_string(binding.first), binding.second.to_text());

    const std::string text = store.to_text();
    VCC_CHECK(text.find("Cmd+Z") != std::string::npos);

    SettingsStore reloaded;
    VCC_CHECK(reloaded.from_text(text));
    const SettingsSection* back = reloaded.find_section("shortcuts.Mine");
    VCC_CHECK(back != nullptr);

    ShortcutProfile restored("Mine");
    for (const auto& entry : back->values()) {
        Command command = Command::None;
        KeyStroke stroke;
        if (command_from_string(entry.first, command)
            && KeyStroke::from_text(entry.second, stroke))
            restored.rebind(command, stroke);
    }

    // What was written is what comes back, Command modifier and all.
    VCC_CHECK_EQ(restored.stroke_for(Command::Undo).to_text(), std::string("Cmd+Z"));

    // And moving it to a Windows machine is one call, not a re-edit.
    const ShortcutProfile moved = restored.remapped_for(ShortcutPlatform::ControlKey);
    VCC_CHECK_EQ(moved.stroke_for(Command::Undo).to_text(), std::string("Ctrl+Z"));
    VCC_CHECK_EQ(moved.binding_count(), restored.binding_count());
}

VCC_TEST_MAIN("composer/workspace")
