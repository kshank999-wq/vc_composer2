// The shortcut profiles a composer keeps (spec 02 sections 11, 13).
//
// What is checked here is what the library decides: which profiles it
// holds and which it refuses, what goes into the settings file and comes
// back, what a profile written on the other platform reads as, and what a
// binding that will not read costs - which is that binding and nothing
// else.
#include "vcc/composer/ShortcutProfileLibrary.h"

#include <string>
#include <vector>

#include "vcc_test.h"
#include "vcc_test_files.h"

using namespace vcc::composer;
using vcc::composer::Command;
using vcc::composer::Key;
using vcc::composer::KeyStroke;
using vcc::composer::Modifier;
using vcc::composer::ShortcutPlatform;
using vcc::composer::ShortcutProfile;

namespace {

/// A composer's profile: the default with the undo key moved and a key
/// bound that no built-in binds, so a round trip that quietly dropped
/// either shows up.
ShortcutProfile my_layout()
{
    ShortcutProfile profile = vcc::composer::make_vc_composer_profile(ShortcutPlatform::ControlKey)
                                  .duplicate("My layout");
    profile.rebind(Command::Undo, KeyStroke{Key::F1, 0});
    profile.rebind(Command::ShowComposerWindow, KeyStroke{Key::F9, 0});
    return profile;
}

bool same(const ShortcutProfile& a, const ShortcutProfile& b)
{
    if (a.name() != b.name() || a.binding_count() != b.binding_count())
        return false;
    for (const std::pair<Command, KeyStroke>& binding : a.bindings()) {
        if (b.stroke_for(binding.first) != binding.second)
            return false;
    }
    return true;
}

bool mentions(const std::string& text, const std::string& fragment)
{
    return text.find(fragment) != std::string::npos;
}

}  // namespace

VCC_TEST(the_library_starts_with_the_three_built_in_profiles_and_they_cannot_be_replaced)
{
    ShortcutProfileLibrary library = built_in_shortcut_profiles(ShortcutPlatform::ControlKey);
    VCC_CHECK_EQ(library.size(), static_cast<std::size_t>(3));
    VCC_CHECK(library.find(kVcComposerProfileName) != nullptr);
    VCC_CHECK(library.find(kFinaleStyleProfileName) != nullptr);
    VCC_CHECK(library.find(kSibeliusStyleProfileName) != nullptr);
    VCC_CHECK(library.is_built_in(kVcComposerProfileName));
    VCC_CHECK(!library.is_built_in("My layout"));
    VCC_CHECK_EQ(library.default_profile().name(), std::string(kVcComposerProfileName));

    // A composer's profile under a built-in's name is refused, as is
    // removing a built-in: the reference a composer started from is always
    // there.
    ShortcutProfile impostor = my_layout();
    impostor.set_name(kVcComposerProfileName);
    VCC_CHECK(!library.store(impostor));
    VCC_CHECK(!library.remove(kVcComposerProfileName));
    VCC_CHECK(library.default_profile().stroke_for(Command::Undo) != KeyStroke{Key::F1, 0});

    // A blank name is refused, and so is a built-in profile itself: the
    // library's built-ins are the ones it was made with.
    ShortcutProfile blank = my_layout();
    blank.set_name({});
    VCC_CHECK(!library.store(blank));
    VCC_CHECK(!library.store(vcc::composer::make_finale_style_profile()));
    VCC_CHECK_EQ(library.size(), static_cast<std::size_t>(3));
}

VCC_TEST(a_composers_profile_is_stored_replaced_by_name_and_removed)
{
    ShortcutProfileLibrary library = built_in_shortcut_profiles(ShortcutPlatform::ControlKey);
    VCC_CHECK(library.store(my_layout()));
    VCC_CHECK_EQ(library.size(), static_cast<std::size_t>(4));

    ShortcutProfile changed = my_layout();
    changed.rebind(Command::Undo, KeyStroke{Key::F2, 0});
    VCC_CHECK(library.store(changed));
    VCC_CHECK_EQ(library.size(), static_cast<std::size_t>(4));
    VCC_CHECK(library.find("My layout")->stroke_for(Command::Undo) == KeyStroke{Key::F2, 0});

    // The stored copy is the composer's to edit in place.
    VCC_CHECK(library.find("My layout")->rebind(Command::Undo, KeyStroke{Key::F3, 0})
              == Command::None);
    VCC_CHECK(library.find("My layout")->stroke_for(Command::Undo) == KeyStroke{Key::F3, 0});

    VCC_CHECK(library.remove("My layout"));
    VCC_CHECK(library.find("My layout") == nullptr);
    VCC_CHECK(!library.remove("My layout"));
}

VCC_TEST(stepping_through_the_profiles_wraps_and_a_copy_gets_a_free_name)
{
    ShortcutProfileLibrary library = built_in_shortcut_profiles(ShortcutPlatform::ControlKey);
    VCC_CHECK(library.store(my_layout()));

    VCC_CHECK_EQ(library.next_after(kVcComposerProfileName), std::string(kFinaleStyleProfileName));
    VCC_CHECK_EQ(library.next_after(kFinaleStyleProfileName),
                 std::string(kSibeliusStyleProfileName));
    VCC_CHECK_EQ(library.next_after(kSibeliusStyleProfileName), std::string("My layout"));
    VCC_CHECK_EQ(library.next_after("My layout"), std::string(kVcComposerProfileName));
    // A name that is not there steps to the first.
    VCC_CHECK_EQ(library.next_after("nothing"), std::string(kVcComposerProfileName));

    VCC_CHECK_EQ(library.copy_name_for("VC Composer"), std::string("VC Composer copy"));
    ShortcutProfile copy = my_layout();
    copy.set_name("VC Composer copy");
    VCC_CHECK(library.store(copy));
    VCC_CHECK_EQ(library.copy_name_for("VC Composer"), std::string("VC Composer copy 2"));
}

VCC_TEST(a_profile_goes_into_the_settings_and_comes_back_whole)
{
    vcc::composer::SettingsSection section;
    shortcut_profile_to_settings(my_layout(), section, ShortcutPlatform::ControlKey);

    // By name, not by number, so the file reads as what it says.
    VCC_CHECK_EQ(section.get("name"), std::string("My layout"));
    VCC_CHECK_EQ(section.get("platform"), std::string("Control"));
    VCC_CHECK_EQ(section.get("bind.Undo"), std::string("F1"));
    VCC_CHECK_EQ(section.get("bind.ShowComposerWindow"), std::string("F9"));
    VCC_CHECK_EQ(section.get("bind.Redo"), std::string("Ctrl+Shift+Z"));

    ShortcutProfile read;
    std::vector<std::string> warnings;
    VCC_CHECK(shortcut_profile_from_settings(section, read, warnings, ShortcutPlatform::ControlKey));
    VCC_CHECK_MSG(warnings.empty(), warnings.empty() ? "" : warnings.front());
    VCC_CHECK(same(read, my_layout()));
    VCC_CHECK(!read.is_built_in());
}

VCC_TEST(a_profile_written_on_the_other_platform_is_remapped_for_this_one)
{
    // Written on a Mac: Cmd+Z. Read on Windows, that key is dead, so it
    // comes back as Ctrl+Z - and a Mac reading a Windows file gets the
    // reverse. A key that uses neither is left exactly as it was.
    vcc::composer::SettingsSection section;
    section.set("name", "From the Mac");
    section.set("platform", "Command");
    section.set("bind.Undo", "Cmd+Z");
    section.set("bind.SelectDurationQuarter", "5");

    ShortcutProfile read;
    std::vector<std::string> warnings;
    VCC_CHECK(shortcut_profile_from_settings(section, read, warnings, ShortcutPlatform::ControlKey));
    VCC_CHECK(warnings.empty());
    VCC_CHECK(read.stroke_for(Command::Undo)
              == KeyStroke{Key::Z, vcc::composer::to_mask(Modifier::Control)});
    VCC_CHECK(read.stroke_for(Command::SelectDurationQuarter) == KeyStroke{Key::Digit5, 0});

    ShortcutProfile on_mac;
    VCC_CHECK(shortcut_profile_from_settings(section, on_mac, warnings, ShortcutPlatform::CommandKey));
    VCC_CHECK(on_mac.stroke_for(Command::Undo)
              == KeyStroke{Key::Z, vcc::composer::to_mask(Modifier::Command)});

    // No platform at all - a file written by hand - is this one. A word
    // that is neither is said, and the keys are taken as written.
    section.remove("platform");
    VCC_CHECK(shortcut_profile_from_settings(section, read, warnings, ShortcutPlatform::ControlKey));
    VCC_CHECK(warnings.empty());
    VCC_CHECK(read.stroke_for(Command::Undo)
              == KeyStroke{Key::Z, vcc::composer::to_mask(Modifier::Command)});

    section.set("platform", "Amiga");
    VCC_CHECK(shortcut_profile_from_settings(section, read, warnings, ShortcutPlatform::ControlKey));
    VCC_CHECK_EQ(warnings.size(), static_cast<std::size_t>(1));
    VCC_CHECK_MSG(mentions(warnings.front(), "platform"), warnings.front());
}

VCC_TEST(a_binding_that_will_not_read_is_left_out_and_named_and_the_rest_stand)
{
    vcc::composer::SettingsSection section;
    shortcut_profile_to_settings(my_layout(), section, ShortcutPlatform::ControlKey);
    section.set("bind.FlyToTheMoon", "F12");       // No such command.
    section.set("bind.Copy", "Ctrl+Hyper+C");      // No such modifier.
    section.set("bind.Cut", "F1");                 // Already Undo's key.

    ShortcutProfile read;
    std::vector<std::string> warnings;
    VCC_CHECK(shortcut_profile_from_settings(section, read, warnings, ShortcutPlatform::ControlKey));

    // The profile still reads - the rest of it is the composer's work -
    // and each bad binding is named.
    VCC_CHECK_EQ(warnings.size(), static_cast<std::size_t>(3));
    for (const std::string& warning : warnings)
        VCC_CHECK_MSG(mentions(warning, "My layout"), warning);
    VCC_CHECK(!read.is_bound(Command::Copy));
    VCC_CHECK(read.command_for(KeyStroke{Key::F12, 0}) == Command::None);

    // One key, one command: whichever the file gave it first keeps it, and
    // the other is said to have lost it. The order is the file's, which is
    // alphabetical by command name, so Cut comes before Undo here.
    VCC_CHECK(read.command_for(KeyStroke{Key::F1, 0}) == Command::Cut);
    VCC_CHECK(!read.is_bound(Command::Undo));
    VCC_CHECK(read.conflicts().empty());

    // A profile with no name is nothing to read.
    vcc::composer::SettingsSection nameless;
    nameless.set("bind.Undo", "F1");
    ShortcutProfile none;
    std::vector<std::string> said;
    VCC_CHECK(!shortcut_profile_from_settings(nameless, none, said, ShortcutPlatform::ControlKey));
    VCC_CHECK_EQ(said.size(), static_cast<std::size_t>(1));
}

VCC_TEST(the_composers_profiles_go_into_the_store_whole_and_the_built_ins_do_not)
{
    ShortcutProfileLibrary library = built_in_shortcut_profiles(ShortcutPlatform::ControlKey);
    VCC_CHECK(library.store(my_layout()));
    ShortcutProfile second = my_layout();
    second.set_name("Laptop");
    VCC_CHECK(library.store(second));

    vcc::composer::SettingsStore store;
    shortcut_profiles_to_store(library, store, ShortcutPlatform::ControlKey);
    VCC_CHECK(store.has_section("shortcut_profile.0"));
    VCC_CHECK(store.has_section("shortcut_profile.1"));
    VCC_CHECK(!store.has_section("shortcut_profile.2"));

    // Read back onto a fresh set of built-ins: five profiles, same as before.
    ShortcutProfileLibrary read = built_in_shortcut_profiles(ShortcutPlatform::ControlKey);
    VCC_CHECK(shortcut_profiles_from_store(store, read, ShortcutPlatform::ControlKey).empty());
    VCC_CHECK_EQ(read.size(), static_cast<std::size_t>(5));
    VCC_CHECK(same(*read.find("My layout"), my_layout()));

    // Written whole: a profile removed today does not come back from a
    // longer list tomorrow.
    VCC_CHECK(library.remove("My layout"));
    shortcut_profiles_to_store(library, store, ShortcutPlatform::ControlKey);
    VCC_CHECK(!store.has_section("shortcut_profile.1"));
    ShortcutProfileLibrary again = built_in_shortcut_profiles(ShortcutPlatform::ControlKey);
    shortcut_profiles_from_store(store, again, ShortcutPlatform::ControlKey);
    VCC_CHECK_EQ(again.size(), static_cast<std::size_t>(4));
    VCC_CHECK(again.find("Laptop") != nullptr);
}

VCC_TEST(a_profile_in_the_file_named_like_a_built_in_is_left_out_and_said)
{
    vcc::composer::SettingsStore store;
    ShortcutProfile impostor = my_layout();
    impostor.set_name(kVcComposerProfileName);
    shortcut_profile_to_settings(impostor, store.section("shortcut_profile.0"),
                                 ShortcutPlatform::ControlKey);

    ShortcutProfileLibrary read = built_in_shortcut_profiles(ShortcutPlatform::ControlKey);
    const std::vector<std::string> warnings =
        shortcut_profiles_from_store(store, read, ShortcutPlatform::ControlKey);
    VCC_CHECK_EQ(read.size(), static_cast<std::size_t>(3));
    VCC_CHECK(read.default_profile().stroke_for(Command::Undo) != KeyStroke{Key::F1, 0});
    VCC_CHECK_EQ(warnings.size(), static_cast<std::size_t>(1));
    VCC_CHECK_MSG(mentions(warnings.front(), "built in"), warnings.front());
}

// The case that carried this library through the companion application's
// settings file was not ported: that file belonged to the Pro Tools
// companion. VC Composer 2 persists these through its own settings layer.

VCC_TEST_MAIN("composer/ShortcutProfileLibrary")
