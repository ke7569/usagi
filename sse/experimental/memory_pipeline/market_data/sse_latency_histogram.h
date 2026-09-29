#ifndef SSE_PIPELINE_LATENCY_HISTOGRAM_H
#define SSE_PIPELINE_LATENCY_HISTOGRAM_H

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace sse_pipeline {

// Fixed-size, single-owner latency histogram.  It is intended to be kept
// per receive/compute thread and merged after the worker stops; observe() and
// merge() use no locks, atomics, or dynamic allocation.
//
// Positive values are grouped by floor(log2(ns)) and then 16 subdivisions of
// that power-of-two interval (64 exponents x 16 buckets).  percentile_ns()
// uses nearest-rank selection and returns the inclusive upper bound of the
// selected bucket.  Consequently the result is an upper-bound estimate; for
// values >= 16 ns its relative quantization error is less than 1/16.  Values
// below 16 ns have the corresponding integer-safe low-range quantization, and
// zero is tracked exactly.  Invalid percentiles and empty histograms return 0.
//
// sum_ns() and all counters saturate at uint64_t max rather than wrapping.
// This keeps outliers and offline merges well-defined, but means a saturated
// sum no longer represents an exact arithmetic total.
class LatencyHistogram {
public:
    static const std::size_t kExponentCount = 64U;
    static const std::size_t kSubdivisions = 16U;
    static const std::size_t kBucketCount = kExponentCount * kSubdivisions;

    LatencyHistogram()
        : count_(0U), min_ns_(0U), max_ns_(0U), sum_ns_(0U), zero_count_(0U),
          has_values_(false) {
        for (std::size_t index = 0U; index < kBucketCount; ++index)
            buckets_[index] = 0U;
    }

    void observe(std::uint64_t value_ns) {
        count_ = saturated_add(count_, 1U);
        sum_ns_ = saturated_add(sum_ns_, value_ns);
        if (!has_values_) {
            min_ns_ = value_ns;
            max_ns_ = value_ns;
            has_values_ = true;
        } else {
            if (value_ns < min_ns_) min_ns_ = value_ns;
            if (value_ns > max_ns_) max_ns_ = value_ns;
        }

        if (value_ns == 0U) {
            zero_count_ = saturated_add(zero_count_, 1U);
            return;
        }
        const std::size_t index = bucket_index(value_ns);
        buckets_[index] = saturated_add(buckets_[index], 1U);
    }

    std::uint64_t count() const { return count_; }
    std::uint64_t min_ns() const { return has_values_ ? min_ns_ : 0U; }
    std::uint64_t max_ns() const { return has_values_ ? max_ns_ : 0U; }
    std::uint64_t sum_ns() const { return sum_ns_; }

    double mean_ns() const {
        if (count_ == 0U) return 0.0;
        return static_cast<double>(sum_ns_) / static_cast<double>(count_);
    }

    // percentile is expressed as a percentage in [0, 100].  This is a
    // nearest-rank percentile over the histogram's quantized observations.
    std::uint64_t percentile_ns(double percentile) const {
        if (count_ == 0U || !(percentile >= 0.0) || percentile > 100.0)
            return 0U;

        std::uint64_t rank = percentile_rank(percentile);
        if (rank <= zero_count_) return 0U;
        rank -= zero_count_;
        for (std::size_t index = 0U; index < kBucketCount; ++index) {
            if (rank <= buckets_[index]) return bucket_upper(index);
            rank -= buckets_[index];
        }
        // This is reachable only after counter saturation has made the
        // independently tracked total larger than the bucket sum.
        return max_ns_;
    }

    // Add another stopped histogram without allocating.  This is deliberately
    // offline-only: concurrent mutation of either operand is unsupported.
    void merge(const LatencyHistogram& other) {
        if (!other.has_values_) return;
        if (!has_values_) {
            min_ns_ = other.min_ns_;
            max_ns_ = other.max_ns_;
            has_values_ = true;
        } else {
            if (other.min_ns_ < min_ns_) min_ns_ = other.min_ns_;
            if (other.max_ns_ > max_ns_) max_ns_ = other.max_ns_;
        }
        count_ = saturated_add(count_, other.count_);
        sum_ns_ = saturated_add(sum_ns_, other.sum_ns_);
        zero_count_ = saturated_add(zero_count_, other.zero_count_);
        for (std::size_t index = 0U; index < kBucketCount; ++index)
            buckets_[index] = saturated_add(buckets_[index], other.buckets_[index]);
    }

private:
    static std::uint64_t saturated_add(std::uint64_t left,
                                       std::uint64_t right) {
        const std::uint64_t maximum =
            (std::numeric_limits<std::uint64_t>::max)();
        return right > maximum - left ? maximum : left + right;
    }

    static std::size_t floor_log2(std::uint64_t value) {
        std::size_t exponent = 0U;
        while (value >>= 1U) ++exponent;
        return exponent;
    }

    static std::size_t bucket_index(std::uint64_t value) {
        const std::size_t exponent = floor_log2(value);
        std::size_t subdivision = 0U;
        const std::uint64_t base = static_cast<std::uint64_t>(1U) << exponent;
        if (exponent >= 4U) {
            subdivision = static_cast<std::size_t>(
                (value >> (exponent - 4U)) & 0x0fU);
        } else {
            // At the small end, integer values cannot fill all 16 intervals;
            // this formula preserves the same normalized bucket boundaries.
            subdivision = static_cast<std::size_t>(
                ((value - base) << 4U) / base);
        }
        return exponent * kSubdivisions + subdivision;
    }

    static std::uint64_t bucket_upper(std::size_t index) {
        const std::size_t exponent = index / kSubdivisions;
        const std::size_t subdivision = index % kSubdivisions;
        const std::uint64_t base = static_cast<std::uint64_t>(1U) << exponent;
        if (exponent < 4U) {
            const std::uint64_t scaled =
                static_cast<std::uint64_t>(subdivision + 1U) * base;
            const std::uint64_t width = (scaled + 15U) / 16U;
            return base + width - 1U;
        }

        const std::uint64_t width =
            static_cast<std::uint64_t>(1U) << (exponent - 4U);
        // Written as (base - 1) + ... so the exponent-63 upper endpoint does
        // not perform an intermediate 2^64 overflow.
        return base - 1U +
               static_cast<std::uint64_t>(subdivision + 1U) * width;
    }

    std::uint64_t percentile_rank(double percentile) const {
        if (percentile <= 0.0) return 1U;
        if (percentile >= 100.0) return count_;

        const long double scaled =
            static_cast<long double>(count_) *
            static_cast<long double>(percentile) / 100.0L;
        const long double rounded = std::ceil(scaled);
        if (rounded <= 1.0L) return 1U;
        const long double maximum = static_cast<long double>(count_);
        if (rounded >= maximum) return count_;
        return static_cast<std::uint64_t>(rounded);
    }

    std::uint64_t count_;
    std::uint64_t min_ns_;
    std::uint64_t max_ns_;
    std::uint64_t sum_ns_;
    std::uint64_t zero_count_;
    std::uint64_t buckets_[kBucketCount];
    bool has_values_;
};

}  // namespace sse_pipeline

#endif
