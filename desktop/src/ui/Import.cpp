#include "ui/Import.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include "ui/WaveformCache.h"

namespace vcc::desktop {

bool is_audio_file(const juce::File& f)
{
    return f.hasFileExtension("wav;wave;aif;aiff;flac;mp3");
}

bool is_midi_file(const juce::File& f) { return f.hasFileExtension("mid;midi;smf"); }
bool is_musicxml_file(const juce::File& f) { return f.hasFileExtension("musicxml;xml;mxl"); }
bool is_project_file(const juce::File& f) { return f.hasFileExtension("vcc2"); }

ImportResult import_audio_files(AppContext& context, const juce::Array<juce::File>& files, const std::string& track_id,
                                double at_seconds, bool allow_selected)
{
    ImportResult result;
    auto& s = context.session;
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    struct Item {
        juce::File file;
        WaveformCache::FileInfo info;
    };
    std::vector<Item> items;
    for (const auto& f : files) {
        Item item{f, {}};
        if (WaveformCache::read_info(formats, f, item.info)) items.push_back(item);
        else result.unreadable.add(f.getFileName());
    }
    if (items.empty()) return result;

    auto audio_track = [&](const std::string& id) {
        const auto* t = s.data().find_track(id);
        return t && (t->kind == project::TrackKind::Audio || t->kind == project::TrackKind::Reference);
    };
    // A track it was dropped on: an audio or reference track, or an
    // instrument or MIDI track's own audio lane.
    auto droppable = [&](const std::string& id) {
        const auto* t = s.data().find_track(id);
        return t && (audio_track(id) || t->kind == project::TrackKind::Instrument || t->kind == project::TrackKind::MidiOnly);
    };
    std::string target = droppable(track_id) ? track_id : std::string();
    if (target.empty() && allow_selected && audio_track(s.selected_track())) target = s.selected_track();
    if (target.empty())
        target = s.add_audio_track(items.front().file.getFileNameWithoutExtension().toStdString(), items.front().info.channels > 1);
    if (target.empty()) return result;

    // Inside a saved project the files are copied into "Audio Files" and
    // stored relative to the project; otherwise they are referenced where they are.
    const std::string folder = s.audio_folder();
    const juce::File audio_dir = folder.empty() ? juce::File() : juce::File(juce::String::fromUTF8(folder.c_str()));
    if (audio_dir != juce::File()) audio_dir.createDirectory();
    int copied = 0;
    std::vector<std::pair<std::string, project::AudioClip>> clips;
    double at = std::max(0.0, at_seconds);
    for (const auto& item : items) {
        std::string stored = item.file.getFullPathName().toStdString();
        if (audio_dir.isDirectory()) {
            juce::File dest = item.file.isAChildOf(audio_dir) ? item.file : audio_dir.getChildFile(item.file.getFileName());
            if (dest != item.file && dest.exists())
                dest = audio_dir.getNonexistentChildFile(item.file.getFileNameWithoutExtension(), item.file.getFileExtension(), false);
            if (dest == item.file || item.file.copyFileTo(dest)) {
                stored = (audio_dir.getFileName() + "/" + dest.getRelativePathFrom(audio_dir)).toStdString();
                copied += dest == item.file ? 0 : 1;
            }
        }
        project::AudioClip c;
        c.name = item.file.getFileNameWithoutExtension().toStdString();
        c.file = stored;
        c.start_seconds = at;
        c.length = item.info.length_seconds;
        c.channels = item.info.channels;
        c.file_sample_rate = item.info.sample_rate;
        clips.emplace_back(target, c);
        at += c.length;  // several files play one after another
    }
    result.ids = s.add_audio_clips(clips, clips.size() == 1 ? "Import " + clips.front().second.name : "Import audio", false);
    if (result.ids.empty()) return result;
    result.tracks.push_back(target);
    s.select_track(target);
    result.message = "Imported " + juce::String(static_cast<int>(result.ids.size())) +
                     (result.ids.size() == 1 ? " audio file" : " audio files");
    if (copied > 0) result.message << " (copied into " << audio_dir.getFileName() << ")";
    else if (folder.empty()) result.message << " - referenced in place; save the project to keep copies with it";
    return result;
}

ImportResult import_midi_files(AppContext& context, const juce::Array<juce::File>& files, const std::string& track_id,
                               Ticks at)
{
    ImportResult result;
    int parts = 0, notes = 0;
    bool tempo = false;
    for (const auto& f : files) {
        const auto r = context.session.import_midi_file(f.getFullPathName().toStdString(), track_id, at);
        if (!r.error.empty()) {
            result.unreadable.add(f.getFileName() + " (" + juce::String(r.error) + ")");
            continue;
        }
        parts += r.parts;
        notes += r.notes;
        tempo = tempo || r.took_tempo;
        result.tracks.insert(result.tracks.end(), r.track_ids.begin(), r.track_ids.end());
    }
    if (parts > 0) {
        result.message = "Imported " + juce::String(notes) + " notes in " + juce::String(parts) + (parts == 1 ? " region" : " regions");
        if (tempo) result.message << ", with the file's tempo and meter";
    }
    return result;
}

}  // namespace vcc::desktop
