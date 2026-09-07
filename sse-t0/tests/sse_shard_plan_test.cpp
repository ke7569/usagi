#include "../market_data/sse_shard_plan.h"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

std::string symbol(unsigned int value) {
    std::string result = std::to_string(value);
    return std::string(6U - result.size(), '0') + result;
}

void check_balanced(const sse_pipeline::ShardPlan& plan,
                   std::size_t channel, std::size_t expected) {
    const std::vector<std::vector<std::size_t> >& counts = plan.counts();
    check(counts.size() == 6U, "expected six Shanghai channels");
    check(counts[channel].size() == 4U, "unexpected shard count");
    std::size_t minimum = counts[channel][0];
    std::size_t maximum = minimum;
    std::size_t total = 0;
    for (std::size_t shard = 0; shard < counts[channel].size(); ++shard) {
        if (counts[channel][shard] < minimum) minimum = counts[channel][shard];
        if (counts[channel][shard] > maximum) maximum = counts[channel][shard];
        total += counts[channel][shard];
    }
    check(total == expected && maximum - minimum <= 1U,
          "channel stock counts are not balanced");
}
}  // namespace

int main() {
    try {
        sse_pipeline::ShardPlan plan(4);
        std::string error;
        std::uint32_t shard = 99;

        for (std::uint32_t channel = 1; channel <= 6; ++channel) {
            for (unsigned int i = 0; i < channel + 3U; ++i)
                check(plan.assign(channel, symbol(channel * 100U + i), &shard, &error),
                      "channel assignment failed");
            check_balanced(plan, channel - 1U, channel + 3U);
        }

        // A skewed channel still starts with its globally least-loaded shard
        // when all of its own shard counts are tied.
        sse_pipeline::ShardPlan skewed(3);
        for (unsigned int i = 0; i < 4U; ++i)
            check(skewed.assign(1, symbol(700U + i), &shard, &error),
                  "skew setup failed");
        check(skewed.assign(2, "800001", &shard, &error) && shard == 1U,
              "global count tie-break was not deterministic");
        check(skewed.counts()[0][0] == 2U && skewed.counts()[0][1] == 1U &&
                  skewed.counts()[0][2] == 1U && skewed.counts()[1][1] == 1U,
              "skewed channel counts changed unexpectedly");

        // A repeated assignment is stable and does not change counts.
        const std::vector<std::vector<std::size_t> > before = plan.counts();
        std::uint32_t first_shard = 0;
        check(plan.lookup("900001", 0, &first_shard) == false,
              "lookup must require both output pointers");
        check(plan.assign(1, "900001", &first_shard, &error),
              "stable assignment setup failed");
        std::uint32_t assigned_channel = 0;
        std::uint32_t assigned_shard = 0;
        check(plan.lookup("900001", &assigned_channel, &assigned_shard) &&
                  assigned_channel == 1U && assigned_shard == first_shard,
              "assigned route lookup failed");
        const std::vector<std::vector<std::size_t> > after_first = plan.counts();
        std::uint32_t repeated_shard = 0;
        check(plan.assign(1, "900001", &repeated_shard, &error) &&
                  repeated_shard == first_shard && plan.counts() == after_first,
              "repeated assignment moved or recounted stock");
        check(!plan.assign(2, "900001", &repeated_shard, &error),
              "changed channel was accepted");
        check(plan.counts() == after_first, "rejected channel changed counts");
        check(before != after_first, "stable assignment setup did not count stock");

        // Restore is idempotent for the exact mapping and rejects conflicts.
        check(plan.restore(3, "654321", 2, &error), "initial restore failed");
        check(plan.restore(3, "654321", 2, &error), "same restore was not idempotent");
        check(!plan.restore(3, "654321", 1, &error), "conflicting restore accepted");
        check(!plan.restore(4, "654321", 2, &error), "channel-conflicting restore accepted");
        check(!plan.restore(3, "654322", 4, &error), "out-of-range restore accepted");

        // Invalid plan and input are rejected without creating routes.
        sse_pipeline::ShardPlan zero(0);
        check(!zero.assign(1, "000001", &shard, &error), "zero-shard plan accepted");
        check(!plan.assign(0, "000002", &shard, &error), "channel zero accepted");
        check(!plan.assign(7, "000002", &shard, &error), "channel seven accepted");
        check(!plan.assign(1, "12345", &shard, &error), "short symbol accepted");
        check(!plan.assign(1, "600000.SH", &shard, &error), "suffixed symbol accepted");
        check(!plan.restore(1, "abcdef", 0, &error), "non-numeric symbol restored");
        check(!plan.restore(1, "000003", 4, &error), "out-of-range shard accepted");

        std::uint32_t restored_channel = 0;
        std::uint32_t restored_shard = 0;
        check(plan.lookup("654321", &restored_channel, &restored_shard) &&
                  restored_channel == 3U && restored_shard == 2U,
              "restored route lookup failed");
        check(!plan.lookup("999999", &restored_channel, &restored_shard),
              "unknown route lookup succeeded");

        std::cout << "sse_shard_plan_test: PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
