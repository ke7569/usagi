#ifndef SSE_PIPELINE_EVENT_QUEUE_H
#define SSE_PIPELINE_EVENT_QUEUE_H

#include "sse_event.h"

#include <atomic>
#include <cstddef>
#include <limits>
#include <memory>
#include <stdexcept>

namespace sse_pipeline {

// A bounded single-producer/single-consumer queue for fixed-size events.
//
// Exactly one thread may call push and exactly one thread may call pop.  The
// queue owns a preallocated ring and never allocates or waits in either
// operation.  size() is a point-in-time estimate when another thread is
// modifying the queue; capacity() and high_water() are stable observations.
// Published counters can wrap as size_t wraps, while each owner keeps its ring
// index in [0, capacity), so arbitrary capacities do not depend on the counter
// modulus being divisible by capacity.  The optional origin timestamp is
// transient queue metadata and is never part of the Event payload.
class EventQueue {
private:
    struct Slot {
        Event event;
        std::uint64_t origin_ns;
    };

public:
    explicit EventQueue(std::size_t capacity)
        : storage_(), capacity_(capacity), producer_pos_(), consumer_pos_(),
          producer_index_(), consumer_index_(), high_water_() {
        if (capacity == 0) {
            throw std::invalid_argument("EventQueue capacity must be greater than zero");
        }
        if (capacity > (std::numeric_limits<std::size_t>::max)() / sizeof(Slot)) {
            throw std::length_error("EventQueue capacity overflows allocation size");
        }
        storage_.reset(new Slot[capacity]);
    }

    EventQueue(const EventQueue&) = delete;
    EventQueue& operator=(const EventQueue&) = delete;

    // Returns false immediately when all usable slots are occupied.
    bool push(const Event& event, std::uint64_t origin_ns = 0U) {
        const std::size_t producer = producer_pos_.value.load(std::memory_order_relaxed);
        const std::size_t consumer = consumer_pos_.value.load(std::memory_order_acquire);
        if (producer - consumer >= capacity_) {
            return false;
        }

        storage_[producer_index_.value].event = event;
        storage_[producer_index_.value].origin_ns = origin_ns;
        if (producer_index_.value + 1 == capacity_) {
            producer_index_.value = 0;
        } else {
            ++producer_index_.value;
        }
        producer_pos_.value.store(producer + 1, std::memory_order_release);

        const std::size_t occupancy = (producer + 1) - consumer;
        // Only the producer writes this counter, so no retry loop is needed.
        const std::size_t previous = high_water_.value.load(std::memory_order_relaxed);
        if (occupancy > previous) {
            high_water_.value.store(occupancy, std::memory_order_release);
        }
        return true;
    }

    // Returns false when the queue is empty or output is null.
    bool pop(Event* event, std::uint64_t* origin_ns = 0) {
        if (event == 0) {
            return false;
        }

        const std::size_t consumer = consumer_pos_.value.load(std::memory_order_relaxed);
        const std::size_t producer = producer_pos_.value.load(std::memory_order_acquire);
        if (consumer == producer) {
            return false;
        }

        *event = storage_[consumer_index_.value].event;
        if (origin_ns != 0) {
            *origin_ns = storage_[consumer_index_.value].origin_ns;
        }
        if (consumer_index_.value + 1 == capacity_) {
            consumer_index_.value = 0;
        } else {
            ++consumer_index_.value;
        }
        consumer_pos_.value.store(consumer + 1, std::memory_order_release);
        return true;
    }

    // The result can be stale or reflect counters sampled at different times
    // while the producer and consumer are active.  It is always bounded by
    // capacity for a valid SPSC queue state.
    std::size_t size() const {
        const std::size_t producer = producer_pos_.value.load(std::memory_order_acquire);
        const std::size_t consumer = consumer_pos_.value.load(std::memory_order_acquire);
        const std::size_t count = producer - consumer;
        return count > capacity_ ? capacity_ : count;
    }

    std::size_t capacity() const { return capacity_; }

    std::size_t high_water() const {
        return high_water_.value.load(std::memory_order_acquire);
    }

private:
    // Keep each frequently-written field at least a cache line apart without
    // increasing EventQueue's alignment.  The latter matters because queues
    // are embedded in heap-allocated workers on pre-C++17 toolchains, whose
    // ordinary operator new does not guarantee alignas(64) for subobjects.
    struct PaddedCounter {
        PaddedCounter() : value(0) {}
        std::atomic<std::size_t> value;
        unsigned char padding[120];
    };

    struct PaddedIndex {
        PaddedIndex() : value(0) {}
        std::size_t value;
        unsigned char padding[120];
    };

    struct PaddedHighWater {
        PaddedHighWater() : value(0) {}
        std::atomic<std::size_t> value;
        unsigned char padding[120];
    };

    static_assert(sizeof(PaddedCounter) >= 128,
                  "counter must occupy two cache lines");
    static_assert(sizeof(PaddedIndex) >= 128, "index must occupy two cache lines");
    static_assert(sizeof(PaddedHighWater) >= 128,
                  "high-water state must occupy two cache lines");

    std::unique_ptr<Slot[]> storage_;
    const std::size_t capacity_;
    PaddedCounter producer_pos_;
    PaddedCounter consumer_pos_;
    PaddedIndex producer_index_;
    PaddedIndex consumer_index_;
    PaddedHighWater high_water_;
};

}  // namespace sse_pipeline

#endif
