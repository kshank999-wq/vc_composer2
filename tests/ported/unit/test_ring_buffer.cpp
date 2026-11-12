// The lock-free primitives every audio-thread boundary depends on (spec 8).
#include "vcc/audio/SpscRingBuffer.h"

#include <atomic>
#include <cstring>
#include <thread>
#include <vector>

#include "vcc/music/MidiEvent.h"
#include "vcc_test.h"

using namespace vcc::audio;
using vcc::music::MidiEvent;

VCC_TEST(queue_reports_usable_capacity)
{
    SpscQueue<int> queue(10);
    // Rounded up to a power of two with one slot reserved to tell full from
    // empty, so the usable count is at least what was asked for.
    VCC_CHECK(queue.capacity() >= 10);
    VCC_CHECK(queue.empty());
    VCC_CHECK_EQ(queue.size(), static_cast<std::size_t>(0));
}

VCC_TEST(queue_preserves_order)
{
    SpscQueue<int> queue(64);
    for (int i = 0; i < 32; ++i)
        VCC_CHECK(queue.push(i));

    VCC_CHECK_EQ(queue.size(), static_cast<std::size_t>(32));

    for (int i = 0; i < 32; ++i) {
        int value = -1;
        VCC_CHECK(queue.pop(value));
        VCC_CHECK_EQ(value, i);
    }
    VCC_CHECK(queue.empty());
}

VCC_TEST(queue_drops_instead_of_blocking_when_full)
{
    SpscQueue<int> queue(4);
    std::size_t pushed = 0;
    while (queue.push(static_cast<int>(pushed)))
        ++pushed;

    VCC_CHECK_EQ(pushed, queue.capacity());
    VCC_CHECK_EQ(queue.dropped_count(), static_cast<std::uint64_t>(1));

    // Every further push is a counted drop, never a block.
    VCC_CHECK(!queue.push(99));
    VCC_CHECK_EQ(queue.dropped_count(), static_cast<std::uint64_t>(2));

    int value = 0;
    VCC_CHECK(queue.pop(value));
    VCC_CHECK(queue.push(1234));  // Space freed by the pop.
}

VCC_TEST(bulk_operations_report_partial_progress)
{
    SpscQueue<int> queue(8);
    std::vector<int> input(static_cast<std::size_t>(queue.capacity() + 4));
    for (std::size_t i = 0; i < input.size(); ++i)
        input[i] = static_cast<int>(i);

    const std::size_t accepted = queue.push_bulk(input.data(), input.size());
    VCC_CHECK_EQ(accepted, queue.capacity());
    VCC_CHECK(queue.dropped_count() > 0);

    std::vector<int> output(input.size(), -1);
    const std::size_t read = queue.pop_bulk(output.data(), output.size());
    VCC_CHECK_EQ(read, accepted);
    for (std::size_t i = 0; i < read; ++i)
        VCC_CHECK_EQ(output[i], static_cast<int>(i));
}

VCC_TEST(clear_discards_everything_readable)
{
    SpscQueue<int> queue(16);
    for (int i = 0; i < 8; ++i)
        queue.push(i);

    queue.clear();
    VCC_CHECK(queue.empty());

    int value = 0;
    VCC_CHECK(!queue.pop(value));
}

VCC_TEST(queue_carries_midi_events_losslessly)
{
    SpscQueue<MidiEvent> queue(128);
    const MidiEvent original = vcc::music::make_pitch_bend(5, 9001, 12345);
    VCC_CHECK(queue.push(original));

    MidiEvent received;
    VCC_CHECK(queue.pop(received));
    VCC_CHECK_EQ(received.type, original.type);
    VCC_CHECK_EQ(received.channel, original.channel);
    VCC_CHECK_EQ(received.bend, original.bend);
    VCC_CHECK_EQ(received.sample_position, original.sample_position);
}

VCC_TEST(concurrent_producer_and_consumer_lose_nothing)
{
    // The property that matters in production: with one writer and one
    // reader, every accepted item comes out exactly once and in order.
    constexpr int kCount = 200000;
    SpscQueue<int> queue(1024);

    std::atomic<bool> producer_done{false};
    std::atomic<int> accepted{0};

    std::thread producer([&] {
        for (int i = 0; i < kCount; ++i) {
            while (!queue.push(i)) {
                // Spin rather than sleep: this mirrors the real drop policy
                // being replaced by back-pressure for the purposes of the test.
                std::this_thread::yield();
            }
            accepted.fetch_add(1, std::memory_order_relaxed);
        }
        producer_done.store(true, std::memory_order_release);
    });

    int expected = 0;
    int received = 0;
    while (received < kCount) {
        int value = -1;
        if (queue.pop(value)) {
            VCC_CHECK_EQ(value, expected);
            ++expected;
            ++received;
        } else if (producer_done.load(std::memory_order_acquire) && queue.empty()) {
            break;
        }
    }

    producer.join();
    // Nothing lost and nothing reordered is the property under test. The
    // drop counter is expected to be non-zero here: the retry loop above
    // deliberately re-pushes after a full queue, and each rejection counts.
    VCC_CHECK_EQ(received, kCount);
    VCC_CHECK_EQ(accepted.load(), kCount);
    VCC_CHECK(queue.empty());
}

VCC_TEST_MAIN("audio/SpscQueue")
