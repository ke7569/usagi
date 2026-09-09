#ifndef SSE_SHARD_PLAN_H
#define SSE_SHARD_PLAN_H

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace sse_pipeline {

// Stable routing for Shanghai stocks.  The outer counts vector is indexed by
// ChannelNo - 1 and each inner vector is indexed by shard id.
class ShardPlan {
public:
    explicit ShardPlan(std::size_t shard_count);

    // Assign a new stock to a shard, or return the existing assignment.  A
    // stock is never moved after its first successful assignment.
    bool assign(std::uint32_t channel_no, const std::string& symbol,
                std::uint32_t* shard, std::string* error);

    // Resolve a stock for snapshot routing.  Snapshots do not carry a
    // ChannelNo, so both the remembered channel and shard are returned.
    bool lookup(const std::string& symbol, std::uint32_t* channel_no,
                std::uint32_t* shard) const;

    // Install a previously persisted route.  Reinstalling the exact route is
    // idempotent; a route that disagrees with the existing one is rejected.
    bool restore(std::uint32_t channel_no, const std::string& symbol,
                 std::uint32_t shard, std::string* error);

    // Counts are indexed as counts()[channel_no - 1][shard].
    const std::vector<std::vector<std::size_t> >& counts() const {
        return counts_;
    }

    std::size_t shard_count() const { return shard_count_; }
    bool valid() const { return valid_; }

private:
    struct Assignment {
        std::uint32_t channel_no;
        std::uint32_t shard;
        Assignment() : channel_no(0), shard(0) {}
        Assignment(std::uint32_t channel, std::uint32_t shard_id)
            : channel_no(channel), shard(shard_id) {}
    };

    static bool valid_channel(std::uint32_t channel_no);
    static bool valid_symbol(const std::string& symbol);
    bool valid_shard(std::uint32_t shard) const;
    static bool fail(std::string* error, const std::string& message);
    std::uint32_t choose_shard(std::uint32_t channel_no) const;
    void record(std::uint32_t channel_no, std::uint32_t shard);

    std::size_t shard_count_;
    bool valid_;
    std::vector<std::vector<std::size_t> > counts_;
    std::vector<std::size_t> totals_;
    std::map<std::string, Assignment> assignments_;
};

// Stable routing for Shanghai inference owners.  Unlike the book plan this
// plan can reserve an owner before a stock's first tick supplies ChannelNo:
// snapshot-only work calls assign(symbol), and the later tick calls
// register_channel().  Registration only records the channel; it never moves
// an existing stock route.
class InferenceShardPlan {
public:
    explicit InferenceShardPlan(
        std::size_t shard_count,
        const std::map<std::string, double>& frequency_weights =
            std::map<std::string, double>());

    // Assign with a known ChannelNo.  Repeated calls are idempotent and a
    // stock remains on its first selected owner.
    bool assign(std::uint32_t channel_no, const std::string& symbol,
                std::uint32_t* shard, std::string* error);

    // Assign a stock whose ChannelNo is not known yet (snapshot path).
    bool assign(const std::string& symbol, std::uint32_t* shard,
                std::string* error);

    // Register ChannelNo after a no-channel assignment.  The returned owner
    // is always the pre-existing owner when one was already selected.
    bool register_channel(std::uint32_t channel_no, const std::string& symbol,
                          std::uint32_t* shard, std::string* error);

    bool lookup(const std::string& symbol, std::uint32_t* channel_no,
                std::uint32_t* shard) const;

    bool restore(std::uint32_t channel_no, const std::string& symbol,
                 std::uint32_t shard, std::string* error);

    const std::vector<std::vector<std::size_t> >& counts() const {
        return counts_;
    }
    const std::vector<std::vector<double> >& weighted_counts() const {
        return weighted_counts_;
    }
    std::size_t shard_count() const { return shard_count_; }
    bool valid() const { return valid_; }

private:
    struct Assignment {
        std::uint32_t channel_no;
        std::uint32_t shard;
        Assignment() : channel_no(0), shard(0) {}
        Assignment(std::uint32_t channel, std::uint32_t shard_id)
            : channel_no(channel), shard(shard_id) {}
    };

    static bool valid_channel(std::uint32_t channel_no);
    static bool valid_symbol(const std::string& symbol);
    bool valid_shard(std::uint32_t shard) const;
    static bool fail(std::string* error, const std::string& message);
    double weight_for(const std::string& symbol) const;
    std::uint32_t choose_channel_shard(std::uint32_t channel_no,
                                       double weight) const;
    std::uint32_t choose_unknown_shard(double weight) const;
    void record_channel(std::uint32_t channel_no, std::uint32_t shard,
                        double weight);
    void record_unknown(std::uint32_t shard, double weight);
    void add_weight(double* value, double amount);

    std::size_t shard_count_;
    bool valid_;
    std::map<std::string, double> frequency_weights_;
    std::vector<std::vector<std::size_t> > counts_;
    std::vector<std::vector<double> > weighted_counts_;
    std::vector<std::size_t> totals_;
    std::vector<double> weighted_totals_;
    std::map<std::string, Assignment> assignments_;
};

}  // namespace sse_pipeline

#endif
