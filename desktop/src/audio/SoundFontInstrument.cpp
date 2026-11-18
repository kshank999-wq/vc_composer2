#include "audio/SoundFontInstrument.h"

#include "tsf.h"

namespace vcc::desktop {

namespace {
// Levels that sit the pack beside the placeholder and the rest of the mix: a
// three-note chord at velocity 100 peaks around 0.5-0.7 on either bank
// (measured: MuseScore General's orchestral presets run about 10 dB hotter
// than the Salamander grand as published).
constexpr float kPianoGain = 1.2f;
constexpr float kGmGain = 0.6f;
}  // namespace

PackVoice pack_voice_for(int gm_program, bool unpitched)
{
    PackVoice v;
    if (!unpitched && (gm_program == 0 || gm_program == 1)) {
        if (auto piano = sounds::piano()) {
            v.bank = std::move(piano);
            v.name = "Salamander Grand Piano";
            return v;
        }
    }
    auto gm = sounds::gm();
    if (!gm) return v;
    v.bank = gm;
    v.bank_number = unpitched || gm_program < 0 ? 128 : 0;
    v.program = unpitched || gm_program < 0 ? 0 : juce::jlimit(0, 127, gm_program);
    const char* preset = tsf_bank_get_presetname(gm->font, v.bank_number, v.program);
    v.name = "MuseScore General: " + juce::String(preset != nullptr ? preset : unpitched ? "Drums" : "General MIDI");
    return v;
}

SoundFontInstrument::SoundFontInstrument(PackVoice voice, int polyphony)
    : voice_(std::move(voice)), polyphony_(polyphony)
{
    if (voice_.bank && voice_.bank->font) {
        player_ = tsf_copy(voice_.bank->font);
        preset_ = tsf_get_presetindex(player_, voice_.bank_number, voice_.program);
        if (preset_ < 0 && voice_.bank_number == 128) preset_ = tsf_get_presetindex(player_, 128, 0);
        if (preset_ < 0) preset_ = 0;
        gain_ = voice_.name.startsWith("Salamander") ? kPianoGain : kGmGain;
    }
}

SoundFontInstrument::~SoundFontInstrument()
{
    if (player_ != nullptr) tsf_close(player_);
}

void SoundFontInstrument::prepare(double sample_rate, int max_block_size)
{
    if (player_ == nullptr) return;
    tsf_set_output(player_, TSF_STEREO_UNWEAVED, static_cast<int>(sample_rate), 0.0f);
    // Voices and the one MIDI channel are allocated here, never while playing.
    tsf_set_max_voices(player_, polyphony_);
    tsf_channel_set_presetindex(player_, 0, preset_);
    tsf_channel_set_pitchrange(player_, 0, 2.0f);
    scratch_.assign(static_cast<std::size_t>(std::max(64, max_block_size)) * 2, 0.0f);
}

void SoundFontInstrument::handle(const juce::MidiMessage& m) noexcept
{
    if (m.isNoteOn())
        tsf_channel_note_on(player_, 0, m.getNoteNumber(), m.getFloatVelocity());
    else if (m.isNoteOff())
        tsf_channel_note_off(player_, 0, m.getNoteNumber());
    else if (m.isPitchWheel())
        tsf_channel_set_pitchwheel(player_, 0, m.getPitchWheelValue());
    else if (m.isController())
        tsf_channel_midi_control(player_, 0, m.getControllerNumber(), m.getControllerValue());
    else if (m.isAllNotesOff() || m.isAllSoundOff())
        tsf_channel_sounds_off_all(player_, 0);
}

void SoundFontInstrument::render(juce::AudioBuffer<float>& out, int start, int count) noexcept
{
    const int chunk = static_cast<int>(scratch_.size() / 2);
    auto* left = out.getWritePointer(0);
    auto* right = out.getNumChannels() > 1 ? out.getWritePointer(1) : nullptr;
    while (count > 0) {
        const int n = std::min(count, chunk);
        tsf_render_float(player_, scratch_.data(), n, 0);
        const float* l = scratch_.data();
        const float* r = scratch_.data() + n;
        for (int i = 0; i < n; ++i) {
            left[start + i] += l[i] * gain_;
            if (right != nullptr) right[start + i] += r[i] * gain_;
        }
        start += n;
        count -= n;
    }
}

void SoundFontInstrument::process(juce::AudioBuffer<float>& out, juce::MidiBuffer& midi, int num_samples) noexcept
{
    if (player_ == nullptr || scratch_.empty()) return;
    int pos = 0;
    for (const auto meta : midi) {
        const int at = juce::jlimit(0, num_samples, meta.samplePosition);
        if (at > pos) {
            render(out, pos, at - pos);
            pos = at;
        }
        handle(meta.getMessage());
    }
    if (pos < num_samples) render(out, pos, num_samples - pos);
}

void SoundFontInstrument::all_notes_off() noexcept
{
    if (player_ != nullptr) tsf_channel_sounds_off_all(player_, 0);
}

}  // namespace vcc::desktop
