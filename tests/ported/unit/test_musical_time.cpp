// Musical-time normalization and the constant-tempo conversions the bridge
// uses for within-block offsets (spec 7).
#include "vcc/music/MusicalTime.h"

#include <limits>

#include "vcc_test.h"

using namespace vcc::music;

VCC_TEST(time_signature_accepts_only_musical_denominators)
{
    VCC_CHECK(TimeSignature{4, 4}.is_valid());
    VCC_CHECK(TimeSignature{7, 8}.is_valid());
    VCC_CHECK(TimeSignature{12, 16}.is_valid());
    VCC_CHECK(TimeSignature{1, 1}.is_valid());

    VCC_CHECK(!TimeSignature{0, 4}.is_valid());     // No numerator.
    VCC_CHECK(!TimeSignature{4, 0}.is_valid());     // No denominator.
    VCC_CHECK(!TimeSignature{4, 3}.is_valid());     // Not a power of two.
    VCC_CHECK(!TimeSignature{4, 256}.is_valid());   // Out of range.
    VCC_CHECK(!TimeSignature{200, 4}.is_valid());
}

VCC_TEST(position_defaults_report_nothing_as_known)
{
    // The whole point of the *_valid flags: an unpopulated position must not
    // look like a real one reporting 0 BPM at bar 1.
    const MusicalPosition position;
    VCC_CHECK(!position.tempo_valid);
    VCC_CHECK(!position.ppq_position_valid);
    VCC_CHECK(!position.sample_position_valid);
    VCC_CHECK(!position.time_signature_valid);
    VCC_CHECK_EQ(position.transport, TransportState::Unknown);
    VCC_CHECK(!position.is_playing());
}

VCC_TEST(recording_counts_as_playing)
{
    MusicalPosition position;
    position.transport = TransportState::Recording;
    VCC_CHECK(position.is_playing());

    position.transport = TransportState::Stopped;
    VCC_CHECK(!position.is_playing());
}

VCC_TEST(sample_to_ppq_round_trips_at_fixed_tempo)
{
    // 120 BPM at 48 kHz: one quarter note is 24000 samples.
    VCC_CHECK_NEAR(samples_to_ppq(24000, 48000.0, 120.0), 1.0, 1e-9);
    VCC_CHECK_NEAR(samples_to_ppq(96000, 48000.0, 120.0), 4.0, 1e-9);
    VCC_CHECK_EQ(ppq_to_samples(1.0, 48000.0, 120.0), static_cast<std::int64_t>(24000));

    // A non-round tempo must survive the round trip within a sample.
    const std::int64_t samples = ppq_to_samples(3.5, 44100.0, 137.3);
    VCC_CHECK_NEAR(samples_to_ppq(samples, 44100.0, 137.3), 3.5, 1e-4);
}

VCC_TEST(conversions_refuse_nonsense_input_instead_of_producing_infinities)
{
    VCC_CHECK_EQ(samples_to_ppq(48000, 0.0, 120.0), 0.0);
    VCC_CHECK_EQ(samples_to_ppq(48000, 48000.0, 0.0), 0.0);
    VCC_CHECK_EQ(samples_to_ppq(48000, -48000.0, 120.0), 0.0);
    VCC_CHECK_EQ(ppq_to_samples(1.0, 48000.0, 0.0), static_cast<std::int64_t>(0));
    VCC_CHECK_EQ(ppq_to_samples(std::numeric_limits<double>::quiet_NaN(), 48000.0, 120.0),
                 static_cast<std::int64_t>(0));
    VCC_CHECK_EQ(ppq_to_samples(std::numeric_limits<double>::infinity(), 48000.0, 120.0),
                 static_cast<std::int64_t>(0));
}

VCC_TEST_MAIN("music/MusicalTime")
