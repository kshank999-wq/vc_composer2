#include "audio/PlaceholderSynth.h"

#include <cmath>

namespace vcc::desktop {

PlaceholderVoice placeholder_voice_for(const std::string& family, int gm, bool unpitched)
{
    if (unpitched || gm < 0) return PlaceholderVoice::Drums;
    if (gm >= 8 && gm <= 15) return PlaceholderVoice::Mallet;                    // celesta .. dulcimer
    if (gm >= 16 && gm <= 23) return PlaceholderVoice::Organ;                    // organs, accordion, harmonica
    if (gm >= 24 && gm <= 31) return PlaceholderVoice::Plucked;                  // guitars
    if (gm >= 32 && gm <= 39) return gm >= 38 ? PlaceholderVoice::Lead : PlaceholderVoice::Plucked;  // basses
    if (gm == 45 || gm == 46 || (gm >= 104 && gm <= 108)) return PlaceholderVoice::Plucked;  // pizz, harp, sitar..kalimba
    if (gm == 47 || (gm >= 112 && gm <= 119)) return PlaceholderVoice::Mallet;   // timpani, bells, steel drums
    if (gm >= 52 && gm <= 54) return PlaceholderVoice::Voice;
    if (gm >= 80 && gm <= 87) return PlaceholderVoice::Lead;
    if (gm >= 88 && gm <= 103) return PlaceholderVoice::Pad;
    if (gm >= 120) return PlaceholderVoice::Pad;
    if (gm >= 0 && gm <= 7) return PlaceholderVoice::Keys;
    if (family == "strings" || (gm >= 40 && gm <= 51) || gm == 110) return PlaceholderVoice::Bowed;
    if (family == "brass" || (gm >= 56 && gm <= 63)) return PlaceholderVoice::Brass;
    if (gm >= 72 && gm <= 79) return PlaceholderVoice::Flute;
    if (family == "woodwinds" || (gm >= 64 && gm <= 71) || gm == 109 || gm == 111) return PlaceholderVoice::Reed;
    if (family == "keys") return PlaceholderVoice::Keys;
    if (family == "synth") return PlaceholderVoice::Pad;
    if (family == "vocals") return PlaceholderVoice::Voice;
    if (family == "percussion") return PlaceholderVoice::Mallet;
    if (family == "rhythm" || family == "traditional") return PlaceholderVoice::Plucked;
    return PlaceholderVoice::Keys;
}

const char* to_string(PlaceholderVoice v) noexcept
{
    switch (v) {
        case PlaceholderVoice::Bowed: return "bowed";
        case PlaceholderVoice::Brass: return "brass";
        case PlaceholderVoice::Reed: return "reed";
        case PlaceholderVoice::Flute: return "flute";
        case PlaceholderVoice::Keys: return "keys";
        case PlaceholderVoice::Plucked: return "plucked";
        case PlaceholderVoice::Mallet: return "mallet";
        case PlaceholderVoice::Organ: return "organ";
        case PlaceholderVoice::Pad: return "pad";
        case PlaceholderVoice::Lead: return "lead";
        case PlaceholderVoice::Voice: return "voice";
        case PlaceholderVoice::Drums: return "drums";
    }
    return "keys";
}

namespace {

struct Sound final : juce::SynthesiserSound {
    bool appliesToNote(int) override { return true; }
    bool appliesToChannel(int) override { return true; }
};

/// Parameters of a tonal placeholder voice.
struct Patch {
    float saw = 0, square = 0, triangle = 0, sine = 0;
    float detune_cents = 0;  ///< second saw for width
    float attack = 0.01f, decay = 0.2f, sustain = 0.8f, release = 0.2f;
    float cutoff_hz = 4000, cutoff_velocity = 0.5f, key_tracking = 0.5f;
    float vibrato_depth = 0, vibrato_delay = 0.4f;
    float breath = 0;  ///< noise amount
    float level = 0.18f;
};

Patch patch_for(PlaceholderVoice v)
{
    Patch p;
    switch (v) {
        case PlaceholderVoice::Bowed:
            p.saw = 0.8f; p.triangle = 0.3f; p.detune_cents = 6; p.attack = 0.09f; p.sustain = 0.9f; p.release = 0.3f;
            p.cutoff_hz = 2600; p.vibrato_depth = 0.004f; p.level = 0.13f; break;
        case PlaceholderVoice::Brass:
            p.saw = 1.0f; p.attack = 0.05f; p.decay = 0.3f; p.sustain = 0.85f; p.release = 0.15f;
            p.cutoff_hz = 1800; p.cutoff_velocity = 1.2f; p.level = 0.13f; break;
        case PlaceholderVoice::Reed:
            p.square = 0.6f; p.triangle = 0.5f; p.attack = 0.03f; p.sustain = 0.9f; p.release = 0.12f;
            p.cutoff_hz = 2800; p.vibrato_depth = 0.003f; p.level = 0.14f; break;
        case PlaceholderVoice::Flute:
            p.triangle = 0.8f; p.sine = 0.4f; p.breath = 0.04f; p.attack = 0.04f; p.sustain = 0.9f; p.release = 0.12f;
            p.cutoff_hz = 5000; p.vibrato_depth = 0.004f; p.level = 0.17f; break;
        case PlaceholderVoice::Keys:
            p.triangle = 0.7f; p.sine = 0.6f; p.saw = 0.1f; p.attack = 0.002f; p.decay = 1.4f; p.sustain = 0.0f;
            p.release = 0.3f; p.cutoff_hz = 5000; p.cutoff_velocity = 1.0f; p.level = 0.2f; break;
        case PlaceholderVoice::Plucked:
            p.saw = 0.4f; p.triangle = 0.6f; p.attack = 0.002f; p.decay = 0.9f; p.sustain = 0.0f; p.release = 0.15f;
            p.cutoff_hz = 3200; p.cutoff_velocity = 1.0f; p.level = 0.2f; break;
        case PlaceholderVoice::Mallet:
            p.sine = 1.0f; p.triangle = 0.2f; p.attack = 0.001f; p.decay = 0.8f; p.sustain = 0.0f; p.release = 0.4f;
            p.cutoff_hz = 8000; p.level = 0.22f; break;
        case PlaceholderVoice::Organ:
            p.sine = 0.7f; p.square = 0.3f; p.attack = 0.01f; p.sustain = 1.0f; p.release = 0.08f; p.cutoff_hz = 4500;
            p.level = 0.12f; break;
        case PlaceholderVoice::Pad:
            p.saw = 0.7f; p.detune_cents = 12; p.triangle = 0.3f; p.attack = 0.6f; p.sustain = 0.9f; p.release = 1.2f;
            p.cutoff_hz = 1500; p.cutoff_velocity = 0.3f; p.level = 0.11f; break;
        case PlaceholderVoice::Lead:
            p.saw = 1.0f; p.square = 0.3f; p.attack = 0.005f; p.decay = 0.25f; p.sustain = 0.7f; p.release = 0.1f;
            p.cutoff_hz = 2200; p.cutoff_velocity = 1.0f; p.level = 0.14f; break;
        case PlaceholderVoice::Voice:
            p.triangle = 0.8f; p.saw = 0.2f; p.detune_cents = 8; p.attack = 0.15f; p.sustain = 0.9f; p.release = 0.35f;
            p.cutoff_hz = 1600; p.vibrato_depth = 0.006f; p.level = 0.15f; break;
        case PlaceholderVoice::Drums: break;
    }
    return p;
}

/// Deterministic white noise with no allocation or locking.
struct Noise {
    std::uint32_t state = 0x9E3779B9u;
    float next() noexcept
    {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return static_cast<float>(state) * (2.0f / 4294967295.0f) - 1.0f;
    }
};

class TonalVoice final : public juce::SynthesiserVoice {
public:
    using juce::SynthesiserVoice::renderNextBlock;
    explicit TonalVoice(Patch patch) : patch_(patch) {}

    bool canPlaySound(juce::SynthesiserSound*) override { return true; }

    void startNote(int note, float velocity, juce::SynthesiserSound*, int) override
    {
        const double sr = getSampleRate();
        frequency_ = juce::MidiMessage::getMidiNoteInHertz(note);
        phase_a_ = phase_b_ = 0.0;
        velocity_ = velocity;
        age_ = 0.0;
        juce::ADSR::Parameters ap{patch_.attack, patch_.decay, patch_.sustain, patch_.release};
        // Struck/plucked sounds decay faster up high, as real strings do.
        if (patch_.sustain <= 0.0f) ap.decay = static_cast<float>(patch_.decay * std::pow(2.0, (60 - note) / 24.0));
        env_.setSampleRate(sr);
        env_.setParameters(ap);
        env_.noteOn();
        const double key = std::pow(2.0, (note - 60) / 12.0 * patch_.key_tracking);
        const double cutoff = juce::jlimit(80.0, sr * 0.45,
                                           patch_.cutoff_hz * key * (1.0 + patch_.cutoff_velocity * (velocity - 0.5)));
        lp_coeff_ = static_cast<float>(1.0 - std::exp(-2.0 * juce::MathConstants<double>::pi * cutoff / sr));
        lp_state_ = 0.0f;
    }

    void stopNote(float, bool allow_tail_off) override
    {
        if (allow_tail_off) {
            env_.noteOff();
        } else {
            env_.reset();
            clearCurrentNote();
        }
    }

    void pitchWheelMoved(int) override {}
    void controllerMoved(int, int) override {}

    void renderNextBlock(juce::AudioBuffer<float>& out, int start, int count) override
    {
        if (!isVoiceActive()) return;
        const double sr = getSampleRate();
        const float gain = patch_.level * (0.25f + 0.75f * velocity_ * velocity_);
        const double detune = std::pow(2.0, patch_.detune_cents / 1200.0);
        for (int i = 0; i < count; ++i) {
            age_ += 1.0 / sr;
            double f = frequency_;
            if (patch_.vibrato_depth > 0 && age_ > patch_.vibrato_delay)
                f *= 1.0 + patch_.vibrato_depth * std::sin(2.0 * juce::MathConstants<double>::pi * 5.2 * age_);
            phase_a_ += f / sr;
            phase_a_ -= std::floor(phase_a_);
            phase_b_ += f * detune / sr;
            phase_b_ -= std::floor(phase_b_);
            const float a = static_cast<float>(phase_a_);
            float s = 0.0f;
            if (patch_.saw > 0) {
                float saw = 2.0f * a - 1.0f;
                if (patch_.detune_cents > 0) saw = 0.5f * (saw + 2.0f * static_cast<float>(phase_b_) - 1.0f);
                s += patch_.saw * saw;
            }
            if (patch_.square > 0) s += patch_.square * (a < 0.5f ? 0.7f : -0.7f);
            if (patch_.triangle > 0) s += patch_.triangle * (1.0f - 4.0f * std::abs(a - 0.5f));
            if (patch_.sine > 0) s += patch_.sine * std::sin(2.0f * juce::MathConstants<float>::pi * a);
            if (patch_.breath > 0) s += patch_.breath * noise_.next();
            lp_state_ += lp_coeff_ * (s - lp_state_);
            const float v = lp_state_ * env_.getNextSample() * gain;
            out.addSample(0, start + i, v);
            if (out.getNumChannels() > 1) out.addSample(1, start + i, v);
        }
        if (!env_.isActive()) clearCurrentNote();
    }

private:
    Patch patch_;
    juce::ADSR env_;
    double frequency_ = 440.0, phase_a_ = 0.0, phase_b_ = 0.0, age_ = 0.0;
    float velocity_ = 0.8f, lp_coeff_ = 0.5f, lp_state_ = 0.0f;
    Noise noise_;
};

/// A synthesised kit on the General MIDI drum map.
class DrumVoice final : public juce::SynthesiserVoice {
public:
    using juce::SynthesiserVoice::renderNextBlock;
    bool canPlaySound(juce::SynthesiserSound*) override { return true; }

    void startNote(int note, float velocity, juce::SynthesiserSound*, int) override
    {
        note_ = note;
        velocity_ = velocity;
        t_ = 0.0;
        phase_ = 0.0;
        hp_state_ = hp_prev_ = 0.0f;
        // Kick, snare/clap, hats, cymbals, toms, everything else.
        if (note == 35 || note == 36) { kind_ = Kind::Kick; decay_ = 0.45; }
        else if (note == 38 || note == 40 || note == 39) { kind_ = Kind::Snare; decay_ = 0.22; }
        else if (note == 42 || note == 44) { kind_ = Kind::Hat; decay_ = 0.05; }
        else if (note == 46) { kind_ = Kind::Hat; decay_ = 0.3; }
        else if (note == 49 || note == 51 || note == 52 || note == 55 || note == 57 || note == 59) { kind_ = Kind::Cymbal; decay_ = 1.4; }
        else if (note >= 41 && note <= 50) { kind_ = Kind::Tom; decay_ = 0.35; }
        else { kind_ = Kind::Perc; decay_ = 0.12; }
    }

    void stopNote(float, bool allow_tail_off) override
    {
        if (!allow_tail_off) clearCurrentNote();  // drums ring out on note-off
    }
    void pitchWheelMoved(int) override {}
    void controllerMoved(int, int) override {}

    void renderNextBlock(juce::AudioBuffer<float>& out, int start, int count) override
    {
        if (!isVoiceActive()) return;
        const double sr = getSampleRate();
        const float gain = 0.35f * (0.3f + 0.7f * velocity_);
        for (int i = 0; i < count; ++i) {
            const double env = std::exp(-t_ / (decay_ * 0.35));
            float s = 0.0f;
            switch (kind_) {
                case Kind::Kick: {
                    const double f = 50.0 + 110.0 * std::exp(-t_ * 30.0);
                    phase_ += f / sr;
                    s = static_cast<float>(std::sin(2.0 * juce::MathConstants<double>::pi * phase_));
                    break;
                }
                case Kind::Snare: {
                    phase_ += 185.0 / sr;
                    s = 0.45f * static_cast<float>(std::sin(2.0 * juce::MathConstants<double>::pi * phase_)) + 0.6f * noise_.next();
                    break;
                }
                case Kind::Hat:
                case Kind::Cymbal: {
                    const float n = noise_.next();
                    hp_state_ = 0.9f * (hp_state_ + n - hp_prev_);  // crude high-pass
                    hp_prev_ = n;
                    s = hp_state_ * (kind_ == Kind::Cymbal ? 0.5f : 0.7f);
                    break;
                }
                case Kind::Tom: {
                    const double base = 80.0 + (note_ - 41) * 18.0;
                    phase_ += (base + base * 0.6 * std::exp(-t_ * 20.0)) / sr;
                    s = static_cast<float>(std::sin(2.0 * juce::MathConstants<double>::pi * phase_));
                    break;
                }
                case Kind::Perc: {
                    phase_ += (300.0 + note_ * 12.0) / sr;
                    s = 0.5f * static_cast<float>(std::sin(2.0 * juce::MathConstants<double>::pi * phase_)) + 0.3f * noise_.next();
                    break;
                }
            }
            const float v = s * static_cast<float>(env) * gain;
            out.addSample(0, start + i, v);
            if (out.getNumChannels() > 1) out.addSample(1, start + i, v);
            t_ += 1.0 / sr;
        }
        if (t_ > decay_ * 3.0) clearCurrentNote();
    }

private:
    enum class Kind { Kick, Snare, Hat, Cymbal, Tom, Perc } kind_ = Kind::Perc;
    int note_ = 36;
    float velocity_ = 0.8f, hp_state_ = 0.0f, hp_prev_ = 0.0f;
    double t_ = 0.0, phase_ = 0.0, decay_ = 0.2;
    Noise noise_;
};

}  // namespace

PlaceholderSynth::PlaceholderSynth(PlaceholderVoice voice, int polyphony) : voice_(voice)
{
    for (int i = 0; i < polyphony; ++i) {
        if (voice == PlaceholderVoice::Drums) synth_.addVoice(new DrumVoice());
        else synth_.addVoice(new TonalVoice(patch_for(voice)));
    }
    synth_.addSound(new Sound());
    synth_.setNoteStealingEnabled(true);
}

PlaceholderSynth::~PlaceholderSynth() = default;

void PlaceholderSynth::prepare(double sample_rate, int max_block_size)
{
    synth_.setCurrentPlaybackSampleRate(sample_rate);
    scratch_.setSize(2, juce::jmax(1, max_block_size));
}

void PlaceholderSynth::process(juce::AudioBuffer<float>& out, juce::MidiBuffer& midi, int num_samples) noexcept
{
    synth_.renderNextBlock(out, midi, 0, num_samples);
}

void PlaceholderSynth::all_notes_off() noexcept
{
    for (int ch = 1; ch <= 16; ++ch) synth_.allNotesOff(ch, false);
}

juce::String PlaceholderSynth::display_name() const
{
    return juce::String("Placeholder (") + to_string(voice_) + ")";
}

}  // namespace vcc::desktop
