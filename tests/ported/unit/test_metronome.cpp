// The click a composer writes to.
//
// The engine is framework-free, so all of it is checked here: where the
// clicks fall, which of them is the accent, that it does not drift, and
// that a block boundary is not a reason to cut a click in half.
#include "vcc/audio/Metronome.h"

#include <cmath>
#include <string>
#include <vector>

using namespace vcc::audio;
using vcc::music::TimeSignature;

#include "vcc_test.h"

namespace {

TimeSignature meter_of(std::uint16_t numerator, std::uint16_t denominator)
{
    TimeSignature meter;
    meter.numerator = numerator;
    meter.denominator = denominator;
    return meter;
}

/// Every click in the next `frames`, as the engine would place them.
std::vector<MetronomeClick> clicks(const Metronome& metronome, std::size_t frames)
{
    std::vector<MetronomeClick> found(64);
    const std::size_t count = metronome.clicks_in(frames, found.data(), found.size());
    found.resize(count);
    return found;
}

/// The loudest sample in a block, which is how a test hears a click.
float peak(const std::vector<float>& block)
{
    float loudest = 0.0f;
    for (const float sample : block)
        loudest = std::max(loudest, std::abs(sample));
    return loudest;
}

}  // namespace

VCC_TEST(a_metronome_that_has_not_been_started_is_silent)
{
    Metronome metronome;
    metronome.prepare(48000.0);
    VCC_CHECK(!metronome.is_running());

    std::vector<float> block(512 * 2, 1.0f);
    metronome.render(block.data(), 512);

    // Replaced rather than added to: a caller mixing this in has its own
    // add, and one that does not would hear whatever was in the buffer.
    VCC_CHECK_MSG(peak(block) == 0.0f, "a stopped metronome made a sound");
    VCC_CHECK_EQ(clicks(metronome, 512).size(), std::size_t{0});
}

VCC_TEST(the_first_click_is_a_downbeat_at_the_very_start)
{
    Metronome metronome;
    metronome.prepare(48000.0);
    VCC_CHECK(metronome.set_tempo(120.0));
    VCC_CHECK(metronome.set_meter(meter_of(4, 4)));
    metronome.start();

    const std::vector<MetronomeClick> found = clicks(metronome, 48000);
    VCC_CHECK_MSG(!found.empty(), "a started metronome clicked nothing");
    if (found.empty())
        return;

    VCC_CHECK_EQ(found[0].frame, std::size_t{0});
    VCC_CHECK_MSG(found[0].is_downbeat(),
                  "the first click a composer hears is not the top of a bar");
}

VCC_TEST(starting_again_begins_a_new_bar_rather_than_carrying_on)
{
    // The first test only proves this for a metronome that has never run -
    // its position is zero anyway. What matters is a *re*start: a composer
    // who stops and starts again counts from the downbeat, not from
    // wherever the last one left off.
    Metronome metronome;
    metronome.prepare(48000.0);
    VCC_CHECK(metronome.set_tempo(120.0));
    VCC_CHECK(metronome.set_meter(meter_of(4, 4)));
    metronome.start();

    // Run it to somewhere that is not a downbeat and not a beat at all.
    std::vector<float> block(1024 * 2, 0.0f);
    for (int i = 0; i < 13; ++i)
        metronome.render(block.data(), 1024);
    VCC_CHECK(metronome.position() > 0);
    VCC_CHECK_MSG(metronome.position() % 24000 != 0, "it stopped on a beat by accident");

    metronome.stop();
    metronome.start();

    VCC_CHECK_MSG(metronome.position() == 0, "starting again carried on from where it was");

    const std::vector<MetronomeClick> found = clicks(metronome, 1024);
    VCC_CHECK_MSG(!found.empty(), "the restarted metronome clicked nothing");
    if (found.empty())
        return;
    VCC_CHECK_MSG(found[0].frame == std::size_t{0}, "and not at once");
    VCC_CHECK_MSG(found[0].is_downbeat(),
                  "a composer who starts again counts from the wrong place");
}

VCC_TEST(clicks_fall_on_the_beat_at_the_tempo_and_the_accent_on_the_bar)
{
    Metronome metronome;
    metronome.prepare(48000.0);
    VCC_CHECK(metronome.set_tempo(120.0));
    VCC_CHECK(metronome.set_meter(meter_of(3, 4)));
    metronome.start();

    // 120 a minute at 48 kHz is half a second, 24000 samples, to the beat.
    VCC_CHECK_EQ(metronome.samples_per_beat(), 24000.0);

    const std::vector<MetronomeClick> found = clicks(metronome, 48000 * 3);
    VCC_CHECK_MSG(found.size() == 6, "three seconds at 120 is six beats");
    if (found.size() != 6)
        return;

    for (std::size_t i = 0; i < found.size(); ++i) {
        VCC_CHECK_EQ(found[i].frame, i * 24000);
        VCC_CHECK_EQ(found[i].beat_in_bar, static_cast<std::uint32_t>(i % 3));
        VCC_CHECK_MSG(found[i].is_downbeat() == (i % 3 == 0),
                      "the accent is not where the bar starts");
    }
}

VCC_TEST(the_beat_the_click_counts_is_the_one_the_meter_names)
{
    // A click counting quarters in 6/8 would be counting something the
    // music is not written in.
    Metronome metronome;
    metronome.prepare(48000.0);
    VCC_CHECK(metronome.set_tempo(120.0));

    VCC_CHECK(metronome.set_meter(meter_of(4, 4)));
    VCC_CHECK_EQ(metronome.samples_per_beat(), 24000.0);

    VCC_CHECK(metronome.set_meter(meter_of(6, 8)));
    VCC_CHECK_MSG(metronome.samples_per_beat() == 12000.0,
                  "an eighth-note beat is half the length of a quarter one");

    VCC_CHECK(metronome.set_meter(meter_of(2, 2)));
    VCC_CHECK_EQ(metronome.samples_per_beat(), 48000.0);
}

VCC_TEST(a_metronome_does_not_drift_over_a_long_stretch)
{
    // The point of computing each click from beat zero rather than adding
    // an interval each time. At a tempo whose beat is not a whole number of
    // samples, adding would be wrong by a sample every few beats.
    Metronome metronome;
    metronome.prepare(44100.0);
    VCC_CHECK(metronome.set_tempo(137.0));
    VCC_CHECK(metronome.set_meter(meter_of(4, 4)));
    metronome.start();

    const double spb = metronome.samples_per_beat();
    VCC_CHECK(spb > 0.0);

    // Ten minutes of 64-sample blocks, which is where a drift would show.
    const std::size_t block = 64;
    const std::size_t blocks = static_cast<std::size_t>(44100.0 * 600.0) / block;

    std::vector<float> buffer(block * 2, 0.0f);
    std::uint64_t counted = 0;
    std::uint64_t last_at = 0;

    for (std::size_t i = 0; i < blocks; ++i) {
        const std::uint64_t start = metronome.position();
        for (const MetronomeClick& click : clicks(metronome, block)) {
            last_at = start + click.frame;
            VCC_CHECK_MSG(
                std::abs(static_cast<double>(last_at) - static_cast<double>(counted) * spb)
                    <= 1.0,
                "a click landed away from where its beat belongs");
            ++counted;
        }
        metronome.render(buffer.data(), block);
    }

    VCC_CHECK_MSG(counted > 1300, "ten minutes at 137 is about 1370 beats");

    // And the last one is still where it should be, not a thousand
    // roundings away from it.
    const double expected = static_cast<double>(counted - 1) * spb;
    VCC_CHECK_MSG(std::abs(static_cast<double>(last_at) - expected) <= 1.0,
                  "the last click of ten minutes had drifted");
}

VCC_TEST(a_click_that_starts_in_one_block_finishes_in_the_next)
{
    // A block boundary is not a reason to cut a click in half.
    Metronome metronome;
    metronome.prepare(48000.0);
    VCC_CHECK(metronome.set_tempo(120.0));
    VCC_CHECK(metronome.set_meter(meter_of(4, 4)));
    metronome.start();

    // The click is 30ms, which at 48 kHz is 1440 samples - longer than
    // these blocks, so it has to carry across several of them.
    const std::size_t block = 256;
    std::vector<float> buffer(block * 2, 0.0f);

    metronome.render(buffer.data(), block);
    VCC_CHECK_MSG(peak(buffer) > 0.0f, "the first block has the click in it");

    metronome.render(buffer.data(), block);
    VCC_CHECK_MSG(peak(buffer) > 0.0f, "and the next block has the rest of it");

    // The click is 1440 samples, which is five and a bit of these blocks:
    // count past it rather than guessing, then check the block after.
    while (metronome.position() < 1440 + block)
        metronome.render(buffer.data(), block);

    metronome.render(buffer.data(), block);
    VCC_CHECK_MSG(peak(buffer) == 0.0f,
                  "between clicks it is silent, at sample "
                      + std::to_string(metronome.position()));
    VCC_CHECK_MSG(metronome.position() < 24000, "and still before the next beat");
}

VCC_TEST(a_downbeat_and_a_beat_do_not_sound_the_same)
{
    Metronome metronome;
    metronome.prepare(48000.0);
    VCC_CHECK(metronome.set_tempo(120.0));
    VCC_CHECK(metronome.set_meter(meter_of(2, 4)));
    metronome.set_level(1.0f);
    metronome.start();

    // Half a beat of each click, which is plenty of the 30ms it lasts.
    const std::size_t window = 512;

    const auto sign_changes = [](const std::vector<float>& block) {
        // The left channel only, and only while the click is sounding:
        // counting through the silence after it would count nothing and
        // make every click look alike.
        int changes = 0;
        bool previous_positive = false;
        bool have_previous = false;
        for (std::size_t frame = 0; frame < block.size() / 2; ++frame) {
            const float sample = block[frame * 2];
            if (sample == 0.0f)
                continue;
            const bool positive = sample > 0.0f;
            if (have_previous && positive != previous_positive)
                ++changes;
            previous_positive = positive;
            have_previous = true;
        }
        return changes;
    };

    std::vector<float> downbeat(window * 2, 0.0f);
    metronome.render(downbeat.data(), window);

    // On to the second beat of the bar, which at 120 is 24000 samples in.
    std::vector<float> skip(window * 2, 0.0f);
    while (metronome.position() + window < 24000)
        metronome.render(skip.data(), window);
    metronome.render(skip.data(), static_cast<std::size_t>(24000 - metronome.position()));

    std::vector<float> beat(window * 2, 0.0f);
    metronome.render(beat.data(), window);

    VCC_CHECK_MSG(peak(downbeat) > 0.0f, "the downbeat sounded");
    VCC_CHECK_MSG(peak(beat) > 0.0f, "and so did the beat after it");

    // The two pitches are a fifth apart, so the higher one crosses zero
    // half again as often over the same stretch. Counted rather than
    // eyeballed: the test that was here compared two numbers that were
    // equal whatever the pitches were, and a mutation making both clicks
    // the same note survived it.
    const int downbeat_changes = sign_changes(downbeat);
    const int beat_changes = sign_changes(beat);
    VCC_CHECK_MSG(downbeat_changes > 0 && beat_changes > 0,
                  "one of the clicks was silent where it was measured");
    VCC_CHECK_MSG(downbeat_changes > beat_changes,
                  "the accent sounds like the beat, so nobody can hear the bar: "
                      + std::to_string(downbeat_changes) + " against "
                      + std::to_string(beat_changes));
}

VCC_TEST(a_tempo_or_a_meter_the_music_does_not_have_is_refused)
{
    Metronome metronome;
    metronome.prepare(48000.0);
    VCC_CHECK(metronome.set_tempo(96.0));

    // Refused rather than clamped: 1000 a minute is not a metronome, and
    // silently making it 300 would be answering a question nobody asked.
    VCC_CHECK(!metronome.set_tempo(1000.0));
    VCC_CHECK(!metronome.set_tempo(0.0));
    VCC_CHECK(!metronome.set_tempo(-96.0));
    VCC_CHECK_EQ(metronome.tempo(), 96.0);

    VCC_CHECK(!metronome.set_meter(meter_of(0, 4)));
    VCC_CHECK(!metronome.set_meter(meter_of(4, 0)));
    VCC_CHECK_EQ(metronome.meter().numerator, 4);

    VCC_CHECK(metronome.set_tempo(kSlowestMetronomeTempo));
    VCC_CHECK(metronome.set_tempo(kFastestMetronomeTempo));
}

VCC_TEST(a_metronome_with_no_sample_rate_makes_no_sound_and_no_nonsense)
{
    Metronome metronome;
    VCC_CHECK_EQ(metronome.sample_rate(), 0.0);
    VCC_CHECK_EQ(metronome.samples_per_beat(), 0.0);

    metronome.start();
    std::vector<float> block(128 * 2, 1.0f);
    metronome.render(block.data(), 128);
    VCC_CHECK_MSG(peak(block) == 0.0f, "it made a sound with no device to make it on");
    VCC_CHECK_EQ(clicks(metronome, 128).size(), std::size_t{0});
}

VCC_TEST(preparing_again_silences_it_rather_than_counting_on)
{
    Metronome metronome;
    metronome.prepare(48000.0);
    metronome.start();

    std::vector<float> block(512 * 2, 0.0f);
    metronome.render(block.data(), 512);
    VCC_CHECK(metronome.position() > 0);

    // A position in samples means nothing once the samples are a different
    // length.
    metronome.prepare(44100.0);
    VCC_CHECK_MSG(!metronome.is_running(), "it kept clicking across a change of device");
    VCC_CHECK_EQ(metronome.position(), std::uint64_t{0});
}

VCC_TEST(the_level_is_what_the_clicks_come_out_at)
{
    Metronome metronome;
    metronome.prepare(48000.0);
    VCC_CHECK(metronome.set_tempo(120.0));
    metronome.start();

    std::vector<float> loud(256 * 2, 0.0f);
    metronome.set_level(1.0f);
    metronome.render(loud.data(), 256);

    metronome.start();
    std::vector<float> quiet(256 * 2, 0.0f);
    metronome.set_level(0.1f);
    metronome.render(quiet.data(), 256);

    VCC_CHECK(peak(loud) > peak(quiet));
    VCC_CHECK(peak(loud) <= 1.0f);

    // Silence is a level, not a refusal.
    metronome.start();
    std::vector<float> off(256 * 2, 0.0f);
    metronome.set_level(0.0f);
    metronome.render(off.data(), 256);
    VCC_CHECK_EQ(peak(off), 0.0f);

    metronome.set_level(5.0f);
    VCC_CHECK_EQ(metronome.level(), 1.0f);
    metronome.set_level(-1.0f);
    VCC_CHECK_EQ(metronome.level(), 0.0f);
}

VCC_TEST_MAIN("audio/Metronome")
