#include "vcc/composer/SessionTemplate.h"

#include <algorithm>

namespace vcc::composer {

bool SessionTemplate::is_valid() const noexcept
{
    if (name.empty())
        return false;
    if (!time_signature.is_valid() || !key.is_valid())
        return false;
    if (!(tempo_bpm > 0.0) || tempo_bpm > 1000.0)
        return false;

    for (const TemplateTrack& track : tracks) {
        if (track.track_name.empty())
            return false;
    }
    return true;
}

std::vector<std::string> SessionTemplate::tracks_without_a_saved_patch() const
{
    std::vector<std::string> result;
    for (const TemplateTrack& track : tracks) {
        if (track.patch_id == 0)
            result.push_back(track.track_name);
    }
    return result;
}

std::vector<std::string> SessionTemplate::tracks_needing_guided_loading() const
{
    std::vector<std::string> result;
    for (const TemplateTrack& track : tracks) {
        if (track.patch_id != 0 && !track.patch_loads_automatically)
            result.push_back(track.track_name);
    }
    return result;
}

const char* to_string(InstrumentationRequest::Ensemble ensemble) noexcept
{
    switch (ensemble) {
        case InstrumentationRequest::Ensemble::SmallOrchestra:      return "Small orchestra";
        case InstrumentationRequest::Ensemble::FullOrchestra:       return "Full orchestra";
        case InstrumentationRequest::Ensemble::StringQuartet:       return "String quartet";
        case InstrumentationRequest::Ensemble::WindQuintet:         return "Wind quintet";
        case InstrumentationRequest::Ensemble::SoloPiano:           return "Solo piano";
        case InstrumentationRequest::Ensemble::PianoTrio:           return "Piano trio";
        case InstrumentationRequest::Ensemble::StringOrchestra:     return "String orchestra";
        case InstrumentationRequest::Ensemble::ConcertBand:         return "Concert band";
        case InstrumentationRequest::Ensemble::HybridRhythmSection: return "Hybrid / rhythm section";
        case InstrumentationRequest::Ensemble::Custom:              break;
    }
    return "Custom";
}

std::vector<std::string> ensemble_instruments(InstrumentationRequest::Ensemble ensemble)
{
    // Conventional instrumentation for each ensemble. Ordinary orchestration
    // knowledge, in score order; the score_order field on each definition
    // does the final sorting.
    switch (ensemble) {
        case InstrumentationRequest::Ensemble::SmallOrchestra:
            return {"Flute", "Oboe", "Clarinet in Bb", "Bassoon",
                    "Horn in F", "Trumpet in Bb", "Timpani",
                    "Violin I", "Violin II", "Viola", "Cello", "Double Bass"};

        case InstrumentationRequest::Ensemble::FullOrchestra:
            return {"Piccolo", "Flute", "Oboe", "English Horn", "Clarinet in Bb",
                    "Bass Clarinet", "Bassoon", "Contrabassoon",
                    "Horn in F", "Trumpet in Bb", "Trombone", "Bass Trombone", "Tuba",
                    "Timpani", "Percussion", "Glockenspiel", "Harp",
                    "Violin I", "Violin II", "Viola", "Cello", "Double Bass"};

        case InstrumentationRequest::Ensemble::StringQuartet:
            return {"Violin I", "Violin II", "Viola", "Cello"};

        case InstrumentationRequest::Ensemble::WindQuintet:
            return {"Flute", "Oboe", "Clarinet in Bb", "Horn in F", "Bassoon"};

        case InstrumentationRequest::Ensemble::SoloPiano:
            return {"Piano"};

        case InstrumentationRequest::Ensemble::PianoTrio:
            return {"Piano", "Violin I", "Cello"};

        case InstrumentationRequest::Ensemble::StringOrchestra:
            return {"Violin I", "Violin II", "Viola", "Cello", "Double Bass"};

        case InstrumentationRequest::Ensemble::ConcertBand:
            return {"Piccolo", "Flute", "Oboe", "Clarinet in Bb", "Bass Clarinet",
                    "Alto Saxophone", "Tenor Saxophone", "Bassoon",
                    "Horn in F", "Trumpet in Bb", "Trombone", "Tuba",
                    "Timpani", "Percussion"};

        case InstrumentationRequest::Ensemble::HybridRhythmSection:
            return {"Piano", "Guitar", "Percussion",
                    "Violin I", "Violin II", "Viola", "Cello", "Double Bass"};

        case InstrumentationRequest::Ensemble::Custom:
            break;
    }
    return {};
}

SessionTemplate generate_template(const InstrumentationRequest& request,
                                  const InstrumentCatalog& catalog)
{
    SessionTemplate result;
    result.name = to_string(request.ensemble);
    result.description = "Generated instrumentation";
    result.time_signature = request.time_signature;
    result.key = request.key;
    result.tempo_bpm = request.tempo_bpm;

    std::vector<std::string> instrument_names = ensemble_instruments(request.ensemble);
    for (const std::string& extra : request.additional_instruments) {
        if (std::find(instrument_names.begin(), instrument_names.end(), extra)
            == instrument_names.end()) {
            instrument_names.push_back(extra);
        }
    }

    // Divided strings: each section becomes two desks, as a larger score
    // would be laid out.
    if (request.divide_strings) {
        std::vector<std::string> divided;
        for (const std::string& name : instrument_names) {
            const InstrumentDefinition* definition = find_standard_instrument(name);
            if (definition != nullptr && definition->family == InstrumentFamily::Strings
                && name != "Double Bass") {
                divided.push_back(name + " (div. 1)");
                divided.push_back(name + " (div. 2)");
            } else {
                divided.push_back(name);
            }
        }
        instrument_names = divided;
    }

    std::uint8_t channel = 0;

    for (const std::string& track_label : instrument_names) {
        // A divided desk shares its parent instrument's definition.
        std::string definition_name = track_label;
        const std::size_t divider = track_label.find(" (div. ");
        if (divider != std::string::npos)
            definition_name = track_label.substr(0, divider);

        const InstrumentDefinition* definition = find_standard_instrument(definition_name);

        TemplateTrack track;
        track.track_name = track_label;
        track.instrument_name = definition_name;

        if (definition != nullptr) {
            // Notation defaults come from the instrument, so the staff is
            // correct before a note is entered (spec 8).
            track.family = definition->family;
            track.clef = definition->clef;
            track.uses_grand_staff = definition->uses_grand_staff;
            track.lower_clef = definition->lower_clef;
            track.transposition_semitones = definition->transposition_semitones;
            track.octave_shift = definition->octave_shift;
            track.score_order = definition->score_order;
        } else {
            // An instrument this build has no definition for still gets a
            // track, with neutral defaults and no invented transposition -
            // a guessed transposition would silently transpose the part.
            track.score_order = 9000;
            track.patch_summary = "Unknown instrument: check clef and transposition";
        }

        // The user's saved preference for this instrument (spec 10.1,
        // acceptance criteria 8 and 9).
        if (const LibraryPatch* favorite = catalog.favorite_for_instrument(definition_name)) {
            track.patch_id = favorite->id;
            track.patch_summary = favorite->display_name();
            track.patch_loads_automatically = favorite->can_attempt_automatic_load();
            if (!track.patch_loads_automatically) {
                track.patch_summary += " - load manually";
            }
        } else if (track.patch_summary.empty()) {
            // No favorite saved. Say so rather than choosing a substitute,
            // which spec 10.1 forbids.
            track.patch_summary = "No saved preference";
        }

        if (request.assign_channels) {
            track.midi_channel = channel;
            channel = static_cast<std::uint8_t>((channel + 1) % 16);
        }

        result.tracks.push_back(track);
    }

    // Score order, with the original order preserved among equals so
    // divided desks stay in the order they were generated.
    std::stable_sort(result.tracks.begin(), result.tracks.end(),
                     [](const TemplateTrack& a, const TemplateTrack& b) {
                         return a.score_order < b.score_order;
                     });

    return result;
}

bool TemplateLibrary::save(const SessionTemplate& session_template)
{
    if (!session_template.is_valid())
        return false;

    const auto existing =
        std::find_if(templates_.begin(), templates_.end(),
                     [&session_template](const SessionTemplate& other) {
                         return other.name == session_template.name;
                     });

    if (existing != templates_.end())
        *existing = session_template;
    else
        templates_.push_back(session_template);

    return true;
}

bool TemplateLibrary::remove(const std::string& name)
{
    const auto found = std::find_if(templates_.begin(), templates_.end(),
                                    [&name](const SessionTemplate& other) {
                                        return other.name == name;
                                    });
    if (found == templates_.end())
        return false;
    templates_.erase(found);
    return true;
}

const SessionTemplate* TemplateLibrary::find(const std::string& name) const noexcept
{
    for (const SessionTemplate& session_template : templates_) {
        if (session_template.name == name)
            return &session_template;
    }
    return nullptr;
}

}  // namespace vcc::composer
