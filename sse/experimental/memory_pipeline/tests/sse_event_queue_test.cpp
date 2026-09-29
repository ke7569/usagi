#include "../market_data/sse_event_queue.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <thread>

namespace {

void check(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

sse_pipeline::Event make_event(std::uint64_t sequence) {
    sse_pipeline::Event event = {};
    event.event_id = 0x100000000ULL + sequence;
    event.receive_realtime_ns = 1700000000000000000ULL + sequence;
    event.receive_mono_ns = 9000000000000000000ULL - sequence;
    event.exchange_time_us = 1700000000000ULL + sequence;
    event.sequence = sequence;
    event.trading_day = 20260907U;
    event.shard_id = static_cast<std::uint32_t>(sequence % 17U);
    event.channel_no = static_cast<std::uint16_t>(sequence % 31U);
    event.payload_size = static_cast<std::uint16_t>(sizeof(event.payload));
    event.kind = static_cast<std::uint8_t>((sequence % 2U) + 1U);
    event.flags = static_cast<std::uint8_t>(sequence % 251U);
    event.version = static_cast<std::uint16_t>(sse_pipeline::kEventVersion);
    for (std::size_t i = 0; i < sizeof(event.security_id); ++i) {
        event.security_id[i] = static_cast<char>('A' + ((sequence + i) % 26U));
    }
    for (std::size_t i = 0; i < sizeof(event.payload); ++i) {
        event.payload[i] = static_cast<unsigned char>((sequence * 13U + i * 7U) & 0xffU);
    }
    return event;
}

void check_event(const sse_pipeline::Event& actual,
                 const sse_pipeline::Event& expected) {
    check(actual.event_id == expected.event_id, "event_id changed");
    check(actual.receive_realtime_ns == expected.receive_realtime_ns,
          "receive_realtime_ns changed");
    check(actual.receive_mono_ns == expected.receive_mono_ns,
          "receive_mono_ns changed");
    check(actual.exchange_time_us == expected.exchange_time_us,
          "exchange_time_us changed");
    check(actual.sequence == expected.sequence, "sequence changed");
    check(actual.trading_day == expected.trading_day, "trading_day changed");
    check(actual.shard_id == expected.shard_id, "shard_id changed");
    check(actual.channel_no == expected.channel_no, "channel_no changed");
    check(actual.payload_size == expected.payload_size, "payload_size changed");
    check(actual.kind == expected.kind, "kind changed");
    check(actual.flags == expected.flags, "flags changed");
    check(actual.version == expected.version, "version changed");
    check(std::memcmp(actual.security_id, expected.security_id,
                      sizeof(actual.security_id)) == 0,
          "security_id changed");
    check(std::memcmp(actual.payload, expected.payload, sizeof(actual.payload)) == 0,
          "payload changed");
}

void test_invalid_capacity() {
    bool rejected = false;
    try {
        sse_pipeline::EventQueue queue(0);
        static_cast<void>(queue);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    check(rejected, "zero capacity accepted");

    rejected = false;
    try {
        sse_pipeline::EventQueue queue((std::numeric_limits<std::size_t>::max)());
        static_cast<void>(queue);
    } catch (const std::length_error&) {
        rejected = true;
    }
    check(rejected, "overflowing capacity accepted");
}

void test_capacity_and_empty_full() {
    sse_pipeline::EventQueue queue(2);
    sse_pipeline::Event output = {};
    const sse_pipeline::Event first = make_event(1);
    const sse_pipeline::Event second = make_event(2);

    check(queue.capacity() == 2, "wrong capacity");
    check(queue.size() == 0, "new queue is not empty");
    check(queue.high_water() == 0, "new queue has nonzero high water");
    check(!queue.pop(&output), "empty queue pop succeeded");
    check(!queue.pop(0), "null output pop succeeded");
    check(queue.push(first), "first push failed");
    check(queue.push(second), "second push failed");
    check(!queue.push(first), "full queue push succeeded");
    check(queue.size() == 2, "full queue size is wrong");
    check(queue.high_water() == 2, "high water did not reach capacity");
    check(queue.pop(&output), "first pop failed");
    check_event(output, first);
    check(queue.pop(&output), "second pop failed");
    check_event(output, second);
    check(!queue.pop(&output), "queue not empty after two pops");
    check(queue.size() == 0, "empty queue size is wrong");
}

void test_wraparound() {
    sse_pipeline::EventQueue queue(3);
    sse_pipeline::Event output = {};
    for (std::uint64_t sequence = 0; sequence < 1000; sequence += 3) {
        for (std::uint64_t offset = 0; offset < 3; ++offset) {
            check(queue.push(make_event(sequence + offset)),
                  "wraparound push failed");
        }
        for (std::uint64_t offset = 0; offset < 3; ++offset) {
            const sse_pipeline::Event expected = make_event(sequence + offset);
            check(queue.pop(&output), "wraparound pop failed");
            check_event(output, expected);
        }
    }
    check(queue.size() == 0, "wraparound queue is not empty");
    check(queue.high_water() == 3, "wraparound high water is wrong");
}

void test_heap_allocation() {
    // EventQueue is intentionally safe as a normally aligned subobject (for
    // example, inside a Worker allocated with ordinary C++11 new).
    const std::unique_ptr<sse_pipeline::EventQueue> heap_queue(
        new sse_pipeline::EventQueue(1));
    const sse_pipeline::Event expected = make_event(77);
    sse_pipeline::Event actual = {};
    check(heap_queue->push(expected), "heap queue push failed");
    check(heap_queue->pop(&actual), "heap queue pop failed");
    check_event(actual, expected);
}

void test_concurrent_sequence_and_payload() {
    const std::size_t event_count = 100000;
    sse_pipeline::EventQueue queue(257);
    std::atomic<bool> failed(false);

    std::thread producer([&queue, event_count]() {
        for (std::uint64_t sequence = 0; sequence < event_count; ++sequence) {
            const sse_pipeline::Event event = make_event(sequence);
            while (!queue.push(event)) {
                std::this_thread::yield();
            }
        }
    });

    std::thread consumer([&queue, &failed, event_count]() {
        std::size_t received = 0;
        while (received < event_count) {
            sse_pipeline::Event event = {};
            if (!queue.pop(&event)) {
                std::this_thread::yield();
                continue;
            }
            const sse_pipeline::Event expected = make_event(received);
            if (event.sequence != received ||
                event.event_id != expected.event_id ||
                std::memcmp(event.payload, expected.payload, sizeof(event.payload)) != 0) {
                failed.store(true, std::memory_order_release);
            }
            ++received;
        }
    });

    producer.join();
    consumer.join();
    check(!failed.load(std::memory_order_acquire),
          "concurrent queue sequence or payload mismatch");
    check(queue.size() == 0, "concurrent queue is not empty");
    check(queue.high_water() > 0 && queue.high_water() <= queue.capacity(),
          "concurrent high water is out of bounds");
}

}  // namespace

int main() {
    try {
        test_invalid_capacity();
        test_capacity_and_empty_full();
        test_wraparound();
        test_heap_allocation();
        test_concurrent_sequence_and_payload();
        std::cout << "sse_event_queue_test: PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
