#include "vcc/composer/ShortcutProfileLibrary.h"

#include <utility>

namespace vcc::composer {
namespace {

constexpr const char* kNameKey = "name";
constexpr const char* kPlatformKey = "platform";
constexpr const char* kBindingPrefix = "bind.";

std::string section_name(std::size_t index)
{
    return std::string(kShortcutProfileSectionPrefix) + "." + std::to_string(index);
}

bool is_profile_section(const std::string& name)
{
    const std::string prefix = std::string(kShortcutProfileSectionPrefix) + ".";
    if (name.compare(0, prefix.size(), prefix) != 0 || name.size() == prefix.size())
        return false;
    for (std::size_t i = prefix.size(); i < name.size(); ++i) {
        if (name[i] < '0' || name[i] > '9')
            return false;
    }
    return true;
}

bool platform_from_string(const std::string& text, composer::ShortcutPlatform& out)
{
    if (text == composer::to_string(composer::ShortcutPlatform::ControlKey)) {
        out = composer::ShortcutPlatform::ControlKey;
        return true;
    }
    if (text == composer::to_string(composer::ShortcutPlatform::CommandKey)) {
        out = composer::ShortcutPlatform::CommandKey;
        return true;
    }
    return false;
}

}  // namespace

const composer::ShortcutProfile* ShortcutProfileLibrary::find(
    const std::string& name) const noexcept
{
    for (const composer::ShortcutProfile& profile : profiles) {
        if (profile.name() == name)
            return &profile;
    }
    return nullptr;
}

composer::ShortcutProfile* ShortcutProfileLibrary::find(const std::string& name) noexcept
{
    for (composer::ShortcutProfile& profile : profiles) {
        if (profile.name() == name)
            return &profile;
    }
    return nullptr;
}

std::size_t ShortcutProfileLibrary::index_of(const std::string& name) const noexcept
{
    for (std::size_t index = 0; index < profiles.size(); ++index) {
        if (profiles[index].name() == name)
            return index;
    }
    return profiles.size();
}

bool ShortcutProfileLibrary::is_built_in(const std::string& name) const noexcept
{
    const composer::ShortcutProfile* profile = find(name);
    return profile != nullptr && profile->is_built_in();
}

const composer::ShortcutProfile& ShortcutProfileLibrary::default_profile() const noexcept
{
    if (const composer::ShortcutProfile* profile = find(kVcComposerProfileName))
        return *profile;
    // A library is always built from the built-ins, so this is a library
    // somebody emptied by hand; the application's own profile is the answer.
    static const composer::ShortcutProfile fallback = composer::make_vc_composer_profile();
    return fallback;
}

bool ShortcutProfileLibrary::store(const composer::ShortcutProfile& profile)
{
    if (profile.name().empty() || profile.is_built_in() || is_built_in(profile.name()))
        return false;

    const std::size_t index = index_of(profile.name());
    if (index < profiles.size())
        profiles[index] = profile;
    else
        profiles.push_back(profile);
    return true;
}

bool ShortcutProfileLibrary::remove(const std::string& name)
{
    if (is_built_in(name))
        return false;
    const std::size_t index = index_of(name);
    if (index >= profiles.size())
        return false;
    profiles.erase(profiles.begin() + static_cast<std::ptrdiff_t>(index));
    return true;
}

std::string ShortcutProfileLibrary::next_after(const std::string& name) const
{
    if (profiles.empty())
        return {};
    const std::size_t index = index_of(name);
    if (index >= profiles.size())
        return profiles.front().name();
    return profiles[(index + 1) % profiles.size()].name();
}

std::string ShortcutProfileLibrary::copy_name_for(const std::string& name) const
{
    const std::string base = name + " copy";
    if (find(base) == nullptr)
        return base;
    for (int number = 2;; ++number) {
        const std::string candidate = base + " " + std::to_string(number);
        if (find(candidate) == nullptr)
            return candidate;
    }
}

ShortcutProfileLibrary built_in_shortcut_profiles(composer::ShortcutPlatform platform)
{
    ShortcutProfileLibrary library;
    library.profiles = composer::built_in_profiles(platform);
    return library;
}

void shortcut_profile_to_settings(const composer::ShortcutProfile& profile,
                                  composer::SettingsSection& out,
                                  composer::ShortcutPlatform platform)
{
    out.clear();
    out.set(kNameKey, profile.name());
    out.set(kPlatformKey, composer::to_string(platform));
    for (const std::pair<composer::Command, composer::KeyStroke>& binding : profile.bindings()) {
        if (!binding.second.is_bound())
            continue;
        out.set(std::string(kBindingPrefix) + composer::to_string(binding.first),
                binding.second.to_text());
    }
}

bool shortcut_profile_from_settings(const composer::SettingsSection& section,
                                    composer::ShortcutProfile& out,
                                    std::vector<std::string>& warnings,
                                    composer::ShortcutPlatform platform)
{
    const std::string name = section.get(kNameKey);
    if (name.empty()) {
        warnings.push_back("a shortcut profile in the settings has no name, so it was left out");
        return false;
    }

    // The platform the bindings were written for. Absent - a file written
    // by hand - means this one; a word that is neither is said, and the
    // bindings are taken as written rather than guessed at.
    composer::ShortcutPlatform written_for = platform;
    if (section.has(kPlatformKey) && !platform_from_string(section.get(kPlatformKey), written_for)) {
        warnings.push_back(name + ": the platform was not Control or Command, so the keys are "
                                  "taken as written");
        written_for = platform;
    }

    composer::ShortcutProfile profile(name);
    const std::string prefix = kBindingPrefix;
    for (const auto& entry : section.values()) {
        if (entry.first.compare(0, prefix.size(), prefix) != 0)
            continue;
        const std::string command_name = entry.first.substr(prefix.size());

        composer::Command command = composer::Command::None;
        if (!composer::command_from_string(command_name, command)
            || command == composer::Command::None) {
            warnings.push_back(name + ": " + command_name
                               + " is not a command this build has, so its key was left out");
            continue;
        }

        composer::KeyStroke stroke;
        if (!composer::KeyStroke::from_text(entry.second, stroke)) {
            warnings.push_back(name + ": the key for " + command_name
                               + " was not one this build can read, so it was left out");
            continue;
        }

        // Refused rather than stolen: two commands on one key is a profile
        // where one of them has silently stopped working, and which one is
        // the file's order rather than anybody's choice.
        if (!profile.bind(command, stroke)) {
            warnings.push_back(name + ": " + stroke.to_text() + " is bound twice, so "
                               + command_name + " was left without it");
        }
    }

    out = written_for == platform ? std::move(profile) : profile.remapped_for(platform);
    return true;
}

void shortcut_profiles_to_store(const ShortcutProfileLibrary& library,
                                composer::SettingsStore& store,
                                composer::ShortcutPlatform platform)
{
    // Whole rather than merged: a profile removed today must not come back
    // from a longer list tomorrow.
    for (const std::string& name : store.section_names()) {
        if (is_profile_section(name))
            store.remove_section(name);
    }

    std::size_t written = 0;
    for (const composer::ShortcutProfile& profile : library.profiles) {
        if (profile.is_built_in())
            continue;
        shortcut_profile_to_settings(profile, store.section(section_name(written)), platform);
        ++written;
    }
}

std::vector<std::string> shortcut_profiles_from_store(const composer::SettingsStore& store,
                                                      ShortcutProfileLibrary& out,
                                                      composer::ShortcutPlatform platform)
{
    std::vector<std::string> warnings;
    for (const std::string& name : store.section_names()) {
        if (!is_profile_section(name))
            continue;
        const composer::SettingsSection* section = store.find_section(name);
        if (section == nullptr)
            continue;

        composer::ShortcutProfile profile;
        if (!shortcut_profile_from_settings(*section, profile, warnings, platform))
            continue;
        if (out.is_built_in(profile.name())) {
            warnings.push_back("a shortcut profile in the settings is called " + profile.name()
                               + ", which is built in, so it was left out");
            continue;
        }
        out.store(profile);
    }
    return warnings;
}

}  // namespace vcc::composer
