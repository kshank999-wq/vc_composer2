#include "vcc/composer/InstrumentCatalog.h"

#include <algorithm>
#include <cctype>

namespace vcc::composer {
namespace {

std::string to_lower(const std::string& text)
{
    std::string result = text;
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                   });
    return result;
}

bool contains_ignoring_case(const std::string& haystack, const std::string& needle)
{
    if (needle.empty())
        return true;
    return to_lower(haystack).find(to_lower(needle)) != std::string::npos;
}

}  // namespace

const char* to_string(InstrumentFamily family) noexcept
{
    switch (family) {
        case InstrumentFamily::Woodwind:            return "Woodwind";
        case InstrumentFamily::Brass:               return "Brass";
        case InstrumentFamily::Percussion:          return "Percussion";
        case InstrumentFamily::UnpitchedPercussion: return "Unpitched percussion";
        case InstrumentFamily::Keyboard:            return "Keyboard";
        case InstrumentFamily::Harp:                return "Harp";
        case InstrumentFamily::Guitar:              return "Guitar";
        case InstrumentFamily::Strings:             return "Strings";
        case InstrumentFamily::Voice:               return "Voice";
        case InstrumentFamily::Synth:               return "Synth";
        case InstrumentFamily::Unknown:             break;
    }
    return "Unknown";
}

bool family_from_string(const std::string& text, InstrumentFamily& out) noexcept
{
    static const InstrumentFamily kAll[] = {
        InstrumentFamily::Unknown,   InstrumentFamily::Woodwind,
        InstrumentFamily::Brass,     InstrumentFamily::Percussion,
        InstrumentFamily::UnpitchedPercussion, InstrumentFamily::Keyboard,
        InstrumentFamily::Harp,      InstrumentFamily::Guitar,
        InstrumentFamily::Strings,   InstrumentFamily::Voice,
        InstrumentFamily::Synth,
    };
    for (const InstrumentFamily family : kAll) {
        if (text == to_string(family)) {
            out = family;
            return true;
        }
    }
    return false;
}

bool InstrumentDefinition::is_valid() const noexcept
{
    if (name.empty())
        return false;
    if (lowest_note > highest_note)
        return false;
    if (transposition_semitones < -24 || transposition_semitones > 24)
        return false;
    if (octave_shift < -3 || octave_shift > 3)
        return false;
    return true;
}

bool InstrumentDefinition::can_play(int sounding_midi_note) const noexcept
{
    return sounding_midi_note >= static_cast<int>(lowest_note)
        && sounding_midi_note <= static_cast<int>(highest_note);
}

const std::vector<InstrumentDefinition>& standard_instruments()
{
    // Standard orchestral knowledge: clef, transposition, sounding range and
    // conventional score order. Nothing here is specific to a library.
    //
    // Ranges are the practical written-for ranges a composer would use, not
    // the extremes a virtuoso can reach; they exist to catch a note that is
    // obviously unplayable, so being conservative is the useful direction.
    static const std::vector<InstrumentDefinition> instruments = [] {
        std::vector<InstrumentDefinition> list;

        const auto add = [&list](const char* name, const char* short_name,
                                 InstrumentFamily family, notation::Clef clef,
                                 std::int8_t transposition, std::uint8_t lowest,
                                 std::uint8_t highest, std::int32_t order) {
            InstrumentDefinition definition;
            definition.name = name;
            definition.short_name = short_name;
            definition.family = family;
            definition.clef = clef;
            definition.transposition_semitones = transposition;
            definition.lowest_note = lowest;
            definition.highest_note = highest;
            definition.score_order = order;
            list.push_back(definition);
        };

        // --- Woodwinds, in score order ------------------------------------
        add("Piccolo", "Picc.", InstrumentFamily::Woodwind, notation::Clef::Treble, 12, 74, 108, 100);
        add("Flute", "Fl.", InstrumentFamily::Woodwind, notation::Clef::Treble, 0, 59, 96, 110);
        add("Alto Flute", "A. Fl.", InstrumentFamily::Woodwind, notation::Clef::Treble, -5, 55, 91, 115);
        add("Oboe", "Ob.", InstrumentFamily::Woodwind, notation::Clef::Treble, 0, 58, 91, 120);
        add("English Horn", "E. Hn.", InstrumentFamily::Woodwind, notation::Clef::Treble, -7, 52, 84, 125);
        add("Clarinet in Bb", "Cl.", InstrumentFamily::Woodwind, notation::Clef::Treble, -2, 50, 94, 130);
        add("Clarinet in A", "Cl.", InstrumentFamily::Woodwind, notation::Clef::Treble, -3, 49, 93, 131);
        add("Bass Clarinet", "B. Cl.", InstrumentFamily::Woodwind, notation::Clef::Treble, -14, 38, 82, 135);
        add("Bassoon", "Bsn.", InstrumentFamily::Woodwind, notation::Clef::Bass, 0, 34, 75, 140);
        add("Contrabassoon", "Cbsn.", InstrumentFamily::Woodwind, notation::Clef::Bass, -12, 22, 63, 145);
        add("Alto Saxophone", "A. Sax.", InstrumentFamily::Woodwind, notation::Clef::Treble, -9, 49, 85, 150);
        add("Tenor Saxophone", "T. Sax.", InstrumentFamily::Woodwind, notation::Clef::Treble, -14, 44, 80, 152);

        // --- Brass ----------------------------------------------------------
        add("Horn in F", "Hn.", InstrumentFamily::Brass, notation::Clef::Treble, -7, 34, 77, 200);
        add("Trumpet in Bb", "Tpt.", InstrumentFamily::Brass, notation::Clef::Treble, -2, 52, 82, 210);
        add("Trumpet in C", "Tpt.", InstrumentFamily::Brass, notation::Clef::Treble, 0, 54, 84, 211);
        add("Trombone", "Tbn.", InstrumentFamily::Brass, notation::Clef::Tenor, 0, 40, 72, 220);
        add("Bass Trombone", "B. Tbn.", InstrumentFamily::Brass, notation::Clef::Bass, 0, 34, 67, 225);
        add("Tuba", "Tba.", InstrumentFamily::Brass, notation::Clef::Bass, 0, 28, 65, 230);

        // --- Percussion -----------------------------------------------------
        add("Timpani", "Timp.", InstrumentFamily::Percussion, notation::Clef::Bass, 0, 36, 60, 300);
        add("Glockenspiel", "Glock.", InstrumentFamily::Percussion, notation::Clef::Treble, 24, 79, 108, 310);
        add("Vibraphone", "Vib.", InstrumentFamily::Percussion, notation::Clef::Treble, 0, 53, 89, 315);
        add("Marimba", "Mar.", InstrumentFamily::Percussion, notation::Clef::Treble, 0, 45, 96, 320);
        add("Xylophone", "Xyl.", InstrumentFamily::Percussion, notation::Clef::Treble, 12, 65, 108, 325);
        add("Tubular Bells", "Tub. B.", InstrumentFamily::Percussion, notation::Clef::Treble, 0, 60, 89, 330);
        add("Percussion", "Perc.", InstrumentFamily::UnpitchedPercussion, notation::Clef::Percussion, 0, 0, 127, 340);

        // --- Keyboard and harp ----------------------------------------------
        InstrumentDefinition piano;
        piano.name = "Piano";
        piano.short_name = "Pno.";
        piano.family = InstrumentFamily::Keyboard;
        piano.clef = notation::Clef::Treble;
        piano.uses_grand_staff = true;
        piano.lower_clef = notation::Clef::Bass;
        piano.lowest_note = 21;
        piano.highest_note = 108;
        piano.score_order = 400;
        list.push_back(piano);

        InstrumentDefinition celesta = piano;
        celesta.name = "Celesta";
        celesta.short_name = "Cel.";
        celesta.lowest_note = 60;
        celesta.highest_note = 108;
        celesta.score_order = 405;
        list.push_back(celesta);

        InstrumentDefinition harp;
        harp.name = "Harp";
        harp.short_name = "Hp.";
        harp.family = InstrumentFamily::Harp;
        harp.clef = notation::Clef::Treble;
        harp.uses_grand_staff = true;
        harp.lower_clef = notation::Clef::Bass;
        harp.lowest_note = 24;
        harp.highest_note = 104;
        harp.score_order = 410;
        list.push_back(harp);

        // Guitar sounds an octave below where it is written.
        InstrumentDefinition guitar;
        guitar.name = "Guitar";
        guitar.short_name = "Gtr.";
        guitar.family = InstrumentFamily::Guitar;
        guitar.clef = notation::Clef::TrebleOctaveDown;
        guitar.octave_shift = -1;
        guitar.lowest_note = 40;
        guitar.highest_note = 84;
        guitar.score_order = 420;
        list.push_back(guitar);

        // --- Voices ---------------------------------------------------------
        add("Soprano", "S.", InstrumentFamily::Voice, notation::Clef::Treble, 0, 60, 81, 500);
        add("Alto", "A.", InstrumentFamily::Voice, notation::Clef::Treble, 0, 53, 74, 510);
        InstrumentDefinition tenor_voice;
        tenor_voice.name = "Tenor";
        tenor_voice.short_name = "T.";
        tenor_voice.family = InstrumentFamily::Voice;
        // Vocal tenor reads treble clef but sounds an octave lower.
        tenor_voice.clef = notation::Clef::TrebleOctaveDown;
        tenor_voice.octave_shift = -1;
        tenor_voice.lowest_note = 48;
        tenor_voice.highest_note = 69;
        tenor_voice.score_order = 520;
        list.push_back(tenor_voice);
        add("Bass", "B.", InstrumentFamily::Voice, notation::Clef::Bass, 0, 40, 64, 530);

        // --- Strings, last in score order -----------------------------------
        add("Violin I", "Vln. I", InstrumentFamily::Strings, notation::Clef::Treble, 0, 55, 103, 600);
        add("Violin II", "Vln. II", InstrumentFamily::Strings, notation::Clef::Treble, 0, 55, 103, 610);
        add("Viola", "Vla.", InstrumentFamily::Strings, notation::Clef::Alto, 0, 48, 91, 620);
        add("Cello", "Vc.", InstrumentFamily::Strings, notation::Clef::Bass, 0, 36, 84, 630);
        // Double bass sounds an octave below where it is written.
        InstrumentDefinition bass;
        bass.name = "Double Bass";
        bass.short_name = "D.B.";
        bass.family = InstrumentFamily::Strings;
        bass.clef = notation::Clef::Bass;
        bass.octave_shift = -1;
        bass.lowest_note = 28;
        bass.highest_note = 67;
        bass.score_order = 640;
        list.push_back(bass);

        return list;
    }();

    return instruments;
}

const InstrumentDefinition* find_standard_instrument(const std::string& name) noexcept
{
    for (const InstrumentDefinition& definition : standard_instruments()) {
        if (definition.name == name)
            return &definition;
    }
    return nullptr;
}

bool LibraryPatch::is_valid() const noexcept
{
    // A patch with no name cannot be presented to the user or recalled, so
    // it is not a usable record.
    if (patch_name.empty())
        return false;
    if (rating > 5)
        return false;
    if (!expression_map.is_valid())
        return false;
    return true;
}

bool LibraryPatch::has_tag(const std::string& tag) const noexcept
{
    return std::find(tags.begin(), tags.end(), tag) != tags.end();
}

std::string LibraryPatch::display_name() const
{
    std::string result = patch_name;

    std::string origin;
    if (!player.empty())
        origin = player;
    if (!library.empty())
        origin += origin.empty() ? library : " / " + library;

    if (!origin.empty())
        result += " (" + origin + ")";
    return result;
}

bool LibraryPatch::can_attempt_automatic_load() const noexcept
{
    // Without a player to load and something to identify the patch by,
    // automatic loading is not possible and spec 10.1 requires the guided
    // workflow instead - never a silent substitution.
    return !player.empty() && !patch_identifier.empty();
}

std::uint64_t InstrumentCatalog::add_patch(const LibraryPatch& patch)
{
    if (!patch.is_valid())
        return 0;

    LibraryPatch stored = patch;
    stored.id = next_id();

    // A patch arriving already marked favorite must not create a second
    // favorite for the same instrument.
    const bool wants_favorite = stored.is_favorite;
    stored.is_favorite = false;
    patches_.push_back(stored);

    if (wants_favorite)
        set_favorite(stored.id);

    return stored.id;
}

bool InstrumentCatalog::remove_patch(std::uint64_t id)
{
    const auto found = std::find_if(patches_.begin(), patches_.end(),
                                    [id](const LibraryPatch& patch) { return patch.id == id; });
    if (found == patches_.end())
        return false;
    patches_.erase(found);
    return true;
}

bool InstrumentCatalog::update_patch(std::uint64_t id, const LibraryPatch& patch)
{
    if (!patch.is_valid())
        return false;

    const auto found = std::find_if(patches_.begin(), patches_.end(),
                                    [id](const LibraryPatch& existing) {
                                        return existing.id == id;
                                    });
    if (found == patches_.end())
        return false;

    const bool was_favorite = found->is_favorite;
    LibraryPatch updated = patch;
    updated.id = id;
    updated.is_favorite = was_favorite;
    *found = updated;

    // Changing a favorite's instrument must not leave two favorites for the
    // old instrument or none for the new one.
    if (was_favorite)
        set_favorite(id);

    return true;
}

const LibraryPatch* InstrumentCatalog::find_patch(std::uint64_t id) const noexcept
{
    for (const LibraryPatch& patch : patches_) {
        if (patch.id == id)
            return &patch;
    }
    return nullptr;
}

bool InstrumentCatalog::set_favorite(std::uint64_t id)
{
    const auto target = std::find_if(patches_.begin(), patches_.end(),
                                     [id](const LibraryPatch& patch) { return patch.id == id; });
    if (target == patches_.end())
        return false;

    // One favorite per instrument, so "recall the favorite" is unambiguous.
    const std::string instrument = target->instrument_name;
    for (LibraryPatch& patch : patches_) {
        if (patch.instrument_name == instrument)
            patch.is_favorite = false;
    }
    target->is_favorite = true;
    return true;
}

bool InstrumentCatalog::clear_favorite(std::uint64_t id)
{
    const auto target = std::find_if(patches_.begin(), patches_.end(),
                                     [id](const LibraryPatch& patch) { return patch.id == id; });
    if (target == patches_.end())
        return false;
    target->is_favorite = false;
    return true;
}

const LibraryPatch* InstrumentCatalog::favorite_for_instrument(
    const std::string& instrument_name) const noexcept
{
    for (const LibraryPatch& patch : patches_) {
        if (patch.is_favorite && patch.instrument_name == instrument_name)
            return &patch;
    }
    return nullptr;
}

std::vector<LibraryPatch> InstrumentCatalog::favorites() const
{
    std::vector<LibraryPatch> result;
    for (const LibraryPatch& patch : patches_) {
        if (patch.is_favorite)
            result.push_back(patch);
    }
    return result;
}

std::vector<LibraryPatch> InstrumentCatalog::patches_for_instrument(
    const std::string& instrument_name) const
{
    std::vector<LibraryPatch> result;
    for (const LibraryPatch& patch : patches_) {
        if (patch.instrument_name == instrument_name)
            result.push_back(patch);
    }
    return result;
}

std::vector<LibraryPatch> InstrumentCatalog::patches_for_family(InstrumentFamily family) const
{
    std::vector<LibraryPatch> result;
    for (const LibraryPatch& patch : patches_) {
        if (patch.family == family)
            result.push_back(patch);
    }
    return result;
}

std::vector<LibraryPatch> InstrumentCatalog::patches_with_tag(const std::string& tag) const
{
    std::vector<LibraryPatch> result;
    for (const LibraryPatch& patch : patches_) {
        if (patch.has_tag(tag))
            result.push_back(patch);
    }
    return result;
}

std::vector<LibraryPatch> InstrumentCatalog::search(const std::string& text) const
{
    std::vector<LibraryPatch> result;
    if (text.empty())
        return patches_;

    for (const LibraryPatch& patch : patches_) {
        bool matched = contains_ignoring_case(patch.patch_name, text)
                    || contains_ignoring_case(patch.library, text)
                    || contains_ignoring_case(patch.provider, text)
                    || contains_ignoring_case(patch.player, text)
                    || contains_ignoring_case(patch.instrument_name, text);

        if (!matched) {
            for (const std::string& tag : patch.tags) {
                if (contains_ignoring_case(tag, text)) {
                    matched = true;
                    break;
                }
            }
        }

        if (matched)
            result.push_back(patch);
    }
    return result;
}

std::vector<std::string> InstrumentCatalog::players() const
{
    std::vector<std::string> result;
    for (const LibraryPatch& patch : patches_) {
        if (patch.player.empty())
            continue;
        if (std::find(result.begin(), result.end(), patch.player) == result.end())
            result.push_back(patch.player);
    }
    std::sort(result.begin(), result.end());
    return result;
}

void InstrumentCatalog::clear()
{
    patches_.clear();
    // Ids are deliberately not reset: a stored template referring to a
    // patch id must not silently resolve to a different patch later.
}

}  // namespace vcc::composer
