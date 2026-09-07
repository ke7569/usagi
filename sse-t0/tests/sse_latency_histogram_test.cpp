#include "../market_data/sse_latency_histogram.h"

#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {

void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void test_empty() {
    const sse_pipeline::LatencyHistogram histogram;
    check(histogram.count() == 0U, "empty count is not zero");
    check(histogram.min_ns() == 0U, "empty minimum is not zero");
    check(histogram.max_ns() == 0U, "empty maximum is not zero");
    check(histogram.sum_ns() == 0U, "empty sum is not zero");
    check(histogram.mean_ns() == 0.0, "empty mean is not zero");
    check(histogram.percentile_ns(0.0) == 0U &&
              histogram.percentile_ns(50.0) == 0U &&
              histogram.percentile_ns(100.0) == 0U,
          "empty percentile is not zero");
    check(histogram.percentile_ns(-1.0) == 0U &&
              histogram.percentile_ns(101.0) == 0U,
          "invalid percentile was accepted");
}

void test_ordering_and_statistics() {
    sse_pipeline::LatencyHistogram histogram;
    histogram.observe(1000U);
    histogram.observe(1U);
    histogram.observe(100U);
    histogram.observe(10U);

    check(histogram.count() == 4U, "wrong observation count");
    check(histogram.min_ns() == 1U && histogram.max_ns() == 1000U,
          "wrong extrema");
    check(histogram.sum_ns() == 1111U, "wrong sum");
    check(histogram.mean_ns() > 277.74 && histogram.mean_ns() < 277.76,
          "wrong mean");
    check(histogram.percentile_ns(0.0) <= histogram.percentile_ns(50.0) &&
              histogram.percentile_ns(50.0) <= histogram.percentile_ns(95.0) &&
              histogram.percentile_ns(95.0) <= histogram.percentile_ns(99.0) &&
              histogram.percentile_ns(99.0) <= histogram.percentile_ns(100.0),
          "percentiles are not ordered");
    check(histogram.percentile_ns(100.0) == 1023U,
          "p100 did not return the known bucket upper bound");
}

void test_known_bucket_bounds() {
    sse_pipeline::LatencyHistogram histogram;
    histogram.observe(512U);
    histogram.observe(543U);  // Same e=9, subdivision=0 bucket.
    histogram.observe(544U);
    histogram.observe(575U);  // Same e=9, subdivision=1 bucket.

    check(histogram.percentile_ns(25.0) == 543U,
          "first known bucket upper bound changed");
    check(histogram.percentile_ns(50.0) == 543U,
          "second known bucket upper bound changed");
    check(histogram.percentile_ns(75.0) == 575U &&
              histogram.percentile_ns(100.0) == 575U,
          "second known bucket endpoint changed");
}

void test_zero_and_outlier() {
    sse_pipeline::LatencyHistogram histogram;
    const std::uint64_t maximum = (std::numeric_limits<std::uint64_t>::max)();
    histogram.observe(0U);
    histogram.observe(maximum);

    check(histogram.count() == 2U && histogram.min_ns() == 0U &&
              histogram.max_ns() == maximum,
          "zero/outlier statistics changed");
    check(histogram.sum_ns() == maximum, "outlier sum overflowed");
    check(histogram.percentile_ns(50.0) == 0U,
          "zero was not retained as the first rank");
    check(histogram.percentile_ns(95.0) == maximum &&
              histogram.percentile_ns(99.9) == maximum,
          "outlier percentile was not retained");
}

void test_merge() {
    sse_pipeline::LatencyHistogram first;
    sse_pipeline::LatencyHistogram second;
    first.observe(0U);
    first.observe(10U);
    second.observe(20U);
    second.observe(1000U);
    first.merge(second);

    check(first.count() == 4U && first.min_ns() == 0U &&
              first.max_ns() == 1000U && first.sum_ns() == 1030U,
          "merged statistics changed");
    check(first.percentile_ns(25.0) == 0U,
          "merged zero rank changed");
    check(first.percentile_ns(100.0) == 1023U,
          "merged high bucket changed");

    // A self-merge is useful for offline aggregation and must not lose the
    // source buckets while they are being added.
    first.merge(first);
    check(first.count() == 8U && first.sum_ns() == 2060U,
          "self merge changed statistics incorrectly");
}

void test_saturation() {
    sse_pipeline::LatencyHistogram histogram;
    const std::uint64_t maximum = (std::numeric_limits<std::uint64_t>::max)();
    histogram.observe(maximum);
    histogram.observe(1U);
    check(histogram.sum_ns() == maximum,
          "sum did not remain saturated after an outlier");
    check(histogram.mean_ns() > 9.22e18 && histogram.mean_ns() < 9.23e18,
          "saturated mean is not finite and bounded");
}

}  // namespace

int main() {
    try {
        test_empty();
        test_ordering_and_statistics();
        test_known_bucket_bounds();
        test_zero_and_outlier();
        test_merge();
        test_saturation();
        std::cout << "sse_latency_histogram_test: PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
