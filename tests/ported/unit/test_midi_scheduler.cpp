// Sample-accurate scheduling of VC Composer's MIDI into host blocks
// (spec 7: preserve musical timing and avoid jitter).
#include "vcc/engine/MidiScheduler.h"

#include <vector>

#include "vcc_test.h"

using namespace vcc::engine;
using namespace vcc::music;

namespace {

constexpr std::size_t kOutCapacity = 64;

}  // namespace

VCC_TEST(a_new_scheduler_is_empty)
{
    MidiScheduler scheduler(128);
    VCC_CHECK_EQ(scheduler.pending(), static_cast<std::size_t>(0));
    VCC_CHECK_EQ(scheduler.capacity(), static_cast<std::size_t>(128));

    MidiEvent out[kOutCapacity];
    VCC_CHECK_EQ(scheduler.collect(0, 512, out, kOutCapacity), static_cast<std::size_t>(0));
}

VCC_TEST(an_event_is_emitted_in_the_block_that_contains_it)
{
    MidiScheduler scheduler(128);

    // Note at absolute sample 1000. Blocks are 512 samples, so it belongs to
    // the block starting at 512, at offset 488.
    const MidiEvent note = make_note_on(0, 60, 100, 1000);
    VCC_CHECK_EQ(scheduler.enqueue(&note, 1), static_cast<std::size_t>(1));

    MidiEvent out[kOutCapacity];

    // Block 0: too early, nothing emitted, event held.
    VCC_CHECK_EQ(scheduler.collect(0, 512, out, kOutCapacity), static_cast<std::size_t>(0));
    VCC_CHECK_EQ(scheduler.pending(), static_cast<std::size_t>(1));

    // Block 1: due, emitted at the correct offset.
    VCC_CHECK_EQ(scheduler.collect(512, 512, out, kOutCapacity), static_cast<std::size_t>(1));
    VCC_CHECK_EQ(out[0].sample_position, static_cast<std::int64_t>(488));
    VCC_CHECK_EQ(out[0].data1, static_cast<std::uint8_t>(60));
    VCC_CHECK_EQ(scheduler.pending(), static_cast<std::size_t>(0));
    VCC_CHECK_EQ(scheduler.late_events(), static_cast<std::uint64_t>(0));
}

VCC_TEST(an_event_exactly_on_a_block_boundary_belongs_to_the_later_block)
{
    MidiScheduler scheduler(128);
    const MidiEvent note = make_note_on(0, 64, 100, 512);
    scheduler.enqueue(&note, 1);

    MidiEvent out[kOutCapacity];
    // Block [0, 512) is exclusive of 512.
    VCC_CHECK_EQ(scheduler.collect(0, 512, out, kOutCapacity), static_cast<std::size_t>(0));
    VCC_CHECK_EQ(scheduler.collect(512, 512, out, kOutCapacity), static_cast<std::size_t>(1));
    VCC_CHECK_EQ(out[0].sample_position, static_cast<std::int64_t>(0));
    VCC_CHECK_EQ(scheduler.late_events(), static_cast<std::uint64_t>(0));
}

VCC_TEST(events_at_one_position_keep_the_order_they_were_given_in)
{
    // Sorting by position is not enough on its own. A chase puts a dynamics
    // controller, a keyswitch and the note it applies to at the playhead
    // sample, and their order is the whole point: a switch after its note
    // plays the wrong articulation, a level after its note arrives too late
    // to be heard as anything but a jump.
    //
    // So the sort has to be stable, and this is what says so. Everything
    // above here preserves order too; what Pro Tools does with events
    // sharing an offset is assumption 8 in
    // docs/aax/00-avid-developer-prerequisites.md.
    MidiScheduler scheduler(128);

    const MidiEvent events[] = {
        make_control_change(0, 11, 40, 256),
        make_note_on(0, 24, 100, 256),   // A keyswitch.
        make_note_off(0, 24, 0, 256),
        make_note_on(0, 60, 90, 256),    // The note it applies to.
        make_note_on(0, 64, 90, 256),
    };
    VCC_CHECK_EQ(scheduler.enqueue(events, 5), static_cast<std::size_t>(5));

    MidiEvent out[kOutCapacity];
    const std::size_t emitted = scheduler.collect(0, 512, out, kOutCapacity);
    VCC_CHECK_EQ(emitted, static_cast<std::size_t>(5));

    for (std::size_t i = 0; i < emitted; ++i) {
        VCC_CHECK_EQ(out[i].sample_position, static_cast<std::int64_t>(256));
        VCC_CHECK_EQ(out[i].type, events[i].type);
        VCC_CHECK_EQ(out[i].data1, events[i].data1);
        VCC_CHECK_EQ(out[i].data2, events[i].data2);
    }
}

VCC_TEST(events_come_out_sorted_by_position_regardless_of_arrival_order)
{
    // The bridge delivers whenever the I/O thread runs, so arrival order is
    // not musical order. Hosts expect ordered MIDI.
    MidiScheduler scheduler(128);

    const MidiEvent events[] = {
        make_note_on(0, 60, 100, 400),
        make_note_on(0, 61, 100, 100),
        make_note_on(0, 62, 100, 300),
        make_note_on(0, 63, 100, 0),
        make_note_on(0, 64, 100, 200),
    };
    VCC_CHECK_EQ(scheduler.enqueue(events, 5), static_cast<std::size_t>(5));

    MidiEvent out[kOutCapacity];
    const std::size_t emitted = scheduler.collect(0, 512, out, kOutCapacity);
    VCC_CHECK_EQ(emitted, static_cast<std::size_t>(5));

    for (std::size_t i = 1; i < emitted; ++i)
        VCC_CHECK(out[i - 1].sample_position <= out[i].sample_position);

    VCC_CHECK_EQ(out[0].data1, static_cast<std::uint8_t>(63));  // Position 0.
    VCC_CHECK_EQ(out[4].data1, static_cast<std::uint8_t>(60));  // Position 400.
}

VCC_TEST(a_late_event_is_emitted_at_the_block_start_and_counted)
{
    // Dropping a note VC Composer asked for is worse than playing it a
    // block late, but the user needs to be able to see that it happened.
    MidiScheduler scheduler(128);
    const MidiEvent note = make_note_on(0, 60, 100, 100);
    scheduler.enqueue(&note, 1);

    MidiEvent out[kOutCapacity];
    VCC_CHECK_EQ(scheduler.collect(5000, 512, out, kOutCapacity), static_cast<std::size_t>(1));
    VCC_CHECK_EQ(out[0].sample_position, static_cast<std::int64_t>(0));
    VCC_CHECK_EQ(scheduler.late_events(), static_cast<std::uint64_t>(1));
}

VCC_TEST(events_without_a_position_are_emitted_immediately)
{
    // "Play this now" from VC Composer: no timeline position to schedule to.
    MidiScheduler scheduler(128);
    MidiEvent note = make_note_on(0, 60, 100);
    note.sample_position = kUnknownSamplePosition;
    scheduler.enqueue(&note, 1);

    MidiEvent out[kOutCapacity];
    VCC_CHECK_EQ(scheduler.collect(48000, 512, out, kOutCapacity), static_cast<std::size_t>(1));
    VCC_CHECK_EQ(out[0].sample_position, static_cast<std::int64_t>(0));
    VCC_CHECK_EQ(scheduler.late_events(), static_cast<std::uint64_t>(1));
}

VCC_TEST(without_a_host_timeline_everything_pending_is_emitted)
{
    // Some hosts report no position while stopped; there is nothing to
    // schedule against, so holding events back would strand them forever.
    MidiScheduler scheduler(128);

    const MidiEvent events[] = {
        make_note_on(0, 60, 100, 100000),
        make_note_on(0, 61, 100, 200000),
    };
    scheduler.enqueue(events, 2);

    MidiEvent out[kOutCapacity];
    const std::size_t emitted =
        scheduler.collect(kUnknownSamplePosition, 512, out, kOutCapacity);
    VCC_CHECK_EQ(emitted, static_cast<std::size_t>(2));
    VCC_CHECK_EQ(out[0].sample_position, static_cast<std::int64_t>(0));
    VCC_CHECK_EQ(out[1].sample_position, static_cast<std::int64_t>(0));
    VCC_CHECK_EQ(scheduler.pending(), static_cast<std::size_t>(0));
}

VCC_TEST(without_a_host_timeline_events_beyond_capacity_are_kept_in_order)
{
    MidiScheduler scheduler(128);

    MidiEvent events[10];
    for (std::size_t i = 0; i < 10; ++i)
        events[i] = make_note_on(0, static_cast<std::uint8_t>(60 + i), 100, 0);
    scheduler.enqueue(events, 10);

    MidiEvent out[4];
    VCC_CHECK_EQ(scheduler.collect(kUnknownSamplePosition, 512, out, 4),
                 static_cast<std::size_t>(4));
    VCC_CHECK_EQ(out[0].data1, static_cast<std::uint8_t>(60));
    VCC_CHECK_EQ(scheduler.pending(), static_cast<std::size_t>(6));

    VCC_CHECK_EQ(scheduler.collect(kUnknownSamplePosition, 512, out, 4),
                 static_cast<std::size_t>(4));
    VCC_CHECK_EQ(out[0].data1, static_cast<std::uint8_t>(64));
}

VCC_TEST(future_events_stay_pending_across_many_blocks)
{
    MidiScheduler scheduler(1024);

    // One note per block for 100 blocks, all enqueued up front.
    for (std::int64_t block = 0; block < 100; ++block) {
        const MidiEvent note =
            make_note_on(0, static_cast<std::uint8_t>(60 + (block % 12)), 100, block * 512 + 10);
        scheduler.enqueue(&note, 1);
    }
    VCC_CHECK_EQ(scheduler.pending(), static_cast<std::size_t>(100));

    MidiEvent out[kOutCapacity];
    for (std::int64_t block = 0; block < 100; ++block) {
        const std::size_t emitted = scheduler.collect(block * 512, 512, out, kOutCapacity);
        VCC_CHECK_MSG(emitted == 1, "block " + std::to_string(block) + " emitted "
                                        + std::to_string(emitted) + " events");
        VCC_CHECK_EQ(out[0].sample_position, static_cast<std::int64_t>(10));
    }
    VCC_CHECK_EQ(scheduler.pending(), static_cast<std::size_t>(0));
    VCC_CHECK_EQ(scheduler.late_events(), static_cast<std::uint64_t>(0));
}

VCC_TEST(overflowing_the_output_defers_rather_than_drops)
{
    MidiScheduler scheduler(128);

    MidiEvent events[10];
    for (std::size_t i = 0; i < 10; ++i)
        events[i] = make_note_on(0, static_cast<std::uint8_t>(60 + i), 100,
                                 static_cast<std::int64_t>(i * 10));
    scheduler.enqueue(events, 10);

    MidiEvent out[4];
    VCC_CHECK_EQ(scheduler.collect(0, 512, out, 4), static_cast<std::size_t>(4));
    VCC_CHECK_EQ(scheduler.pending(), static_cast<std::size_t>(6));
    VCC_CHECK_EQ(scheduler.dropped_events(), static_cast<std::uint64_t>(0));

    // The deferred events come out next block, late but intact.
    const std::size_t emitted = scheduler.collect(512, 512, out, 4);
    VCC_CHECK_EQ(emitted, static_cast<std::size_t>(4));
    VCC_CHECK_EQ(scheduler.late_events(), static_cast<std::uint64_t>(4));
}

VCC_TEST(a_full_scheduler_drops_and_counts_instead_of_growing)
{
    MidiScheduler scheduler(8);

    MidiEvent events[20];
    for (std::size_t i = 0; i < 20; ++i)
        events[i] = make_note_on(0, 60, 100, static_cast<std::int64_t>(1000000 + i));

    const std::size_t accepted = scheduler.enqueue(events, 20);
    VCC_CHECK_EQ(accepted, static_cast<std::size_t>(8));
    VCC_CHECK_EQ(scheduler.pending(), static_cast<std::size_t>(8));
    VCC_CHECK_EQ(scheduler.dropped_events(), static_cast<std::uint64_t>(12));
}

VCC_TEST(malformed_events_are_dropped_before_reaching_the_host)
{
    MidiScheduler scheduler(64);

    MidiEvent bad = make_note_on(0, 60, 100, 0);
    bad.type = static_cast<MidiEventType>(0x0F);  // Not a channel-voice type.
    const MidiEvent good = make_note_on(0, 61, 100, 0);

    const MidiEvent batch[] = {bad, good};
    VCC_CHECK_EQ(scheduler.enqueue(batch, 2), static_cast<std::size_t>(1));
    VCC_CHECK_EQ(scheduler.dropped_events(), static_cast<std::uint64_t>(1));

    MidiEvent out[kOutCapacity];
    VCC_CHECK_EQ(scheduler.collect(0, 512, out, kOutCapacity), static_cast<std::size_t>(1));
    VCC_CHECK_EQ(out[0].data1, static_cast<std::uint8_t>(61));
}

VCC_TEST(reset_discards_pending_events)
{
    // Transport stop, session reload and reconnect all reset: stale future
    // events must not fire against a new timeline.
    MidiScheduler scheduler(64);
    const MidiEvent note = make_note_on(0, 60, 100, 1000000);
    scheduler.enqueue(&note, 1);
    VCC_CHECK_EQ(scheduler.pending(), static_cast<std::size_t>(1));

    scheduler.reset();
    VCC_CHECK_EQ(scheduler.pending(), static_cast<std::size_t>(0));

    MidiEvent out[kOutCapacity];
    VCC_CHECK_EQ(scheduler.collect(1000000, 512, out, kOutCapacity),
                 static_cast<std::size_t>(0));
}

VCC_TEST(scheduler_tolerates_degenerate_arguments)
{
    MidiScheduler scheduler(16);
    const MidiEvent note = make_note_on(0, 60, 100, 0);
    scheduler.enqueue(&note, 1);

    MidiEvent out[4];
    VCC_CHECK_EQ(scheduler.enqueue(nullptr, 5), static_cast<std::size_t>(0));
    VCC_CHECK_EQ(scheduler.enqueue(&note, 0), static_cast<std::size_t>(0));
    VCC_CHECK_EQ(scheduler.collect(0, 512, nullptr, 4), static_cast<std::size_t>(0));
    VCC_CHECK_EQ(scheduler.collect(0, 512, out, 0), static_cast<std::size_t>(0));

    // A zero-length block still emits anything already due at its start.
    VCC_CHECK_EQ(scheduler.collect(0, 0, out, 4), static_cast<std::size_t>(1));
}

// --- Musical time -------------------------------------------------------------

namespace {

/// The host at the start of a block: `sample` on its timeline, `ppq`
/// quarters in, at `tempo`.
MusicalPosition host_at(std::int64_t sample, double ppq, double tempo)
{
    MusicalPosition position;
    position.sample_position = sample;
    position.sample_position_valid = true;
    position.ppq_position = ppq;
    position.ppq_position_valid = true;
    position.tempo_bpm = tempo;
    position.tempo_valid = true;
    position.transport = TransportState::Playing;
    return position;
}

MidiEvent note_at_tick(std::int64_t tick, std::uint8_t note = 60)
{
    MidiEvent event = make_note_on(0, note, 100, tick);
    event.flags = kMidiEventMusicalTime;
    return event;
}

constexpr double kRate = 48000.0;

}  // namespace

VCC_TEST(a_tick_is_placed_at_the_tempo_of_the_block_it_plays_in)
{
    // One quarter in, 120 BPM at 48 kHz: sample 24000, which is block 46
    // (23552..24064) at offset 448.
    MidiEvent out[kOutCapacity];
    std::int64_t sample = -1;
    VCC_CHECK(place_in_block(note_at_tick(960), host_at(0, 0.0, 120.0), kRate, sample));
    VCC_CHECK_EQ(sample, std::int64_t{24000});

    // And from an anchor part-way through: the block at sample 23552 is
    // 0.4906 quarters in, and the event is the rest of the way.
    VCC_CHECK(place_in_block(note_at_tick(960), host_at(23552, 23552.0 / 24000.0, 120.0), kRate,
                             sample));
    VCC_CHECK_EQ(sample, std::int64_t{24000});

    MidiScheduler scheduler(128);
    const MidiEvent note = note_at_tick(960);
    scheduler.enqueue(&note, 1);

    // Blocks before it: held.
    for (std::int64_t block = 0; block < 46; ++block) {
        const std::int64_t start = block * 512;
        VCC_CHECK_EQ(scheduler.collect(start, 512,
                                       host_at(start, static_cast<double>(start) / 24000.0, 120.0),
                                       kRate, out, kOutCapacity),
                     std::size_t{0});
    }
    VCC_CHECK_EQ(scheduler.pending(), std::size_t{1});

    // Its own block: emitted at the offset, in samples, with the flag gone.
    VCC_CHECK_EQ(scheduler.collect(23552, 512, host_at(23552, 23552 / 24000.0, 120.0), kRate,
                                   out, kOutCapacity),
                 std::size_t{1});
    VCC_CHECK_EQ(out[0].sample_position, std::int64_t{448});
    VCC_CHECK(!is_in_musical_time(out[0]));
    VCC_CHECK_EQ(scheduler.late_events(), std::uint64_t{0});
}

VCC_TEST(a_tempo_change_after_the_event_was_sent_moves_where_it_lands)
{
    // The whole point. The note is sent while the host is at 120 BPM, and
    // then the host slows to 60 before the note is due. In samples it would
    // have been placed at 24000 when it was sent and played there; in ticks
    // it is placed every block, and lands where 60 BPM puts it.
    MidiScheduler scheduler(128);
    const MidiEvent note = note_at_tick(960);
    scheduler.enqueue(&note, 1);

    MidiEvent out[kOutCapacity];
    std::int64_t sample = 0;
    double ppq = 0.0;
    double tempo = 120.0;

    // Ten blocks at 120, then the ritardando.
    std::int64_t landed = -1;
    for (int block = 0; block < 200 && landed < 0; ++block) {
        if (block == 10)
            tempo = 60.0;
        const std::size_t emitted =
            scheduler.collect(sample, 512, host_at(sample, ppq, tempo), kRate, out, kOutCapacity);
        if (emitted > 0)
            landed = sample + out[0].sample_position;
        sample += 512;
        ppq += 512.0 * tempo / (60.0 * kRate);
    }

    // 10 blocks (5120 samples) covered 0.2133 quarters at 120; the remaining
    // 0.7867 quarters at 60 are 37760 samples: 42880, not 24000.
    VCC_CHECK_EQ(landed, std::int64_t{42880});
    VCC_CHECK_EQ(scheduler.late_events(), std::uint64_t{0});
}

VCC_TEST(a_tick_the_host_cannot_place_is_played_now)
{
    // No musical position from the host: nothing to hold the event against,
    // and a note-off held forever is a note that never ends. Same rule as
    // an event with no position at all.
    MidiScheduler scheduler(128);
    const MidiEvent note = note_at_tick(960);
    scheduler.enqueue(&note, 1);

    MidiEvent out[kOutCapacity];
    MusicalPosition no_ppq = host_at(0, 0.0, 120.0);
    no_ppq.ppq_position_valid = false;
    VCC_CHECK_EQ(scheduler.collect(0, 512, no_ppq, kRate, out, kOutCapacity), std::size_t{1});
    VCC_CHECK_EQ(out[0].sample_position, std::int64_t{0});
    VCC_CHECK(!is_in_musical_time(out[0]));

    // No tempo, no sample rate, no sample position: the same.
    std::int64_t sample = 0;
    MusicalPosition no_tempo = host_at(0, 0.0, 120.0);
    no_tempo.tempo_valid = false;
    VCC_CHECK(!place_in_block(note_at_tick(0), no_tempo, kRate, sample));
    VCC_CHECK(!place_in_block(note_at_tick(0), host_at(0, 0.0, 120.0), 0.0, sample));
    MusicalPosition no_sample = host_at(0, 0.0, 120.0);
    no_sample.sample_position_valid = false;
    VCC_CHECK(!place_in_block(note_at_tick(0), no_sample, kRate, sample));

    // And the four-argument collect is the no-position case by definition.
    const MidiEvent another = note_at_tick(960 * 100);
    scheduler.enqueue(&another, 1);
    VCC_CHECK_EQ(scheduler.collect(0, 512, out, kOutCapacity), std::size_t{1});
    VCC_CHECK_EQ(out[0].sample_position, std::int64_t{0});
}

VCC_TEST(a_tick_already_passed_is_late_like_a_sample_already_passed)
{
    MidiScheduler scheduler(128);
    const MidiEvent note = note_at_tick(960);
    scheduler.enqueue(&note, 1);

    // The host is two quarters in already.
    MidiEvent out[kOutCapacity];
    VCC_CHECK_EQ(scheduler.collect(48000, 512, host_at(48000, 2.0, 120.0), kRate, out,
                                   kOutCapacity),
                 std::size_t{1});
    VCC_CHECK_EQ(out[0].sample_position, std::int64_t{0});
    VCC_CHECK_EQ(scheduler.late_events(), std::uint64_t{1});
}

VCC_TEST(ticks_and_samples_in_one_block_come_out_in_position_order)
{
    // A window in ticks can share a scheduler with host MIDI in samples.
    // Whatever went in, what comes out is block offsets in order, and events
    // placed at one offset keep the order they were given in.
    MidiScheduler scheduler(128);
    const MidiEvent events[] = {
        note_at_tick(960, 60),          // Sample 24000: offset 448 in block 46.
        make_note_on(0, 61, 100, 23600), // Offset 48.
        note_at_tick(960, 62),          // Same offset as the first: after it.
        make_note_on(0, 63, 100, 24000), // Same offset again: after both.
    };
    scheduler.enqueue(events, 4);

    MidiEvent out[kOutCapacity];
    VCC_CHECK_EQ(scheduler.collect(23552, 512, host_at(23552, 23552 / 24000.0, 120.0), kRate,
                                   out, kOutCapacity),
                 std::size_t{4});
    VCC_CHECK_EQ(out[0].data1, std::uint8_t{61});
    VCC_CHECK_EQ(out[0].sample_position, std::int64_t{48});
    VCC_CHECK_EQ(out[1].data1, std::uint8_t{60});
    VCC_CHECK_EQ(out[2].data1, std::uint8_t{62});
    VCC_CHECK_EQ(out[3].data1, std::uint8_t{63});
    VCC_CHECK_EQ(out[3].sample_position, std::int64_t{448});
    for (const MidiEvent& event : out)
        VCC_CHECK(!is_in_musical_time(event));
}

VCC_TEST(ticks_land_exactly_across_a_long_sequence_at_one_tempo)
{
    // The sample-domain property, restated for ticks: with the host's
    // position advancing consistently, every tick lands on the sample the
    // tempo puts it at - to the sample, not to the block.
    MidiScheduler scheduler(4096);

    std::vector<std::int64_t> expected;
    for (std::int64_t i = 0; i < 300; ++i) {
        const std::int64_t tick = i * 240 + 17;  // Sixteenths, off the grid.
        // 120 BPM at 48 kHz: 25 samples per tick.
        expected.push_back(tick * 25);
        const MidiEvent note = note_at_tick(tick);
        scheduler.enqueue(&note, 1);
    }

    std::vector<std::int64_t> observed;
    MidiEvent out[kOutCapacity];
    const std::int32_t block_size = 128;
    for (std::int64_t block = 0; block < 20000; ++block) {
        const std::int64_t start = block * block_size;
        const std::size_t emitted = scheduler.collect(
            start, block_size, host_at(start, static_cast<double>(start) / 24000.0, 120.0), kRate,
            out, kOutCapacity);
        for (std::size_t i = 0; i < emitted; ++i) {
            VCC_CHECK(out[i].sample_position >= 0);
            VCC_CHECK(out[i].sample_position < block_size);
            observed.push_back(start + out[i].sample_position);
        }
    }

    VCC_CHECK_EQ(observed.size(), expected.size());
    for (std::size_t i = 0; i < observed.size(); ++i) {
        VCC_CHECK_MSG(observed[i] == expected[i],
                      "tick event " + std::to_string(i) + " expected at "
                          + std::to_string(expected[i]) + " but landed at "
                          + std::to_string(observed[i]));
    }
    VCC_CHECK_EQ(scheduler.late_events(), std::uint64_t{0});
}

VCC_TEST(timing_is_exact_across_a_long_sequence)
{
    // The property the spec calls "repeatable timing": every event lands at
    // exactly the absolute sample it was scheduled for, reassembled from
    // block start plus emitted offset.
    MidiScheduler scheduler(4096);

    std::vector<std::int64_t> expected;
    for (std::int64_t i = 0; i < 500; ++i) {
        const std::int64_t position = i * 137 + 7;  // Deliberately not block-aligned.
        expected.push_back(position);
        const MidiEvent note = make_note_on(0, 60, 100, position);
        scheduler.enqueue(&note, 1);
    }

    std::vector<std::int64_t> observed;
    MidiEvent out[kOutCapacity];
    const std::int32_t block_size = 128;

    for (std::int64_t block = 0; block < 1000; ++block) {
        const std::int64_t block_start = block * block_size;
        const std::size_t emitted = scheduler.collect(block_start, block_size, out, kOutCapacity);
        for (std::size_t i = 0; i < emitted; ++i) {
            VCC_CHECK(out[i].sample_position >= 0);
            VCC_CHECK(out[i].sample_position < block_size);
            observed.push_back(block_start + out[i].sample_position);
        }
    }

    VCC_CHECK_EQ(observed.size(), expected.size());
    for (std::size_t i = 0; i < observed.size(); ++i) {
        VCC_CHECK_MSG(observed[i] == expected[i],
                      "event " + std::to_string(i) + " expected at "
                          + std::to_string(expected[i]) + " but landed at "
                          + std::to_string(observed[i]));
    }
    VCC_CHECK_EQ(scheduler.late_events(), static_cast<std::uint64_t>(0));
}

VCC_TEST_MAIN("plugin/MidiScheduler")
