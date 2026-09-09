#include "sse_shard_plan.h"

#include <cmath>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>

namespace sse_pipeline {
namespace {
const std::uint32_t kFirstChannel = 1U;
const std::uint32_t kLastChannel = 6U;
}

ShardPlan::ShardPlan(std::size_t shard_count)
    : shard_count_(0), valid_(false), counts_(), totals_(), assignments_() {
    // Shard ids are returned as uint32_t.  Refuse a count that cannot be
    // represented by that API, and leave the object inert on allocation
    // failure because the constructor has no error channel.
    if (shard_count == 0 ||
        shard_count > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()))
        return;

    try {
        counts_.assign(kLastChannel,
                       std::vector<std::size_t>(shard_count, static_cast<std::size_t>(0)));
        totals_.assign(shard_count, static_cast<std::size_t>(0));
    } catch (const std::bad_alloc&) {
        counts_.clear();
        totals_.clear();
        return;
    } catch (const std::length_error&) {
        counts_.clear();
        totals_.clear();
        return;
    }
    shard_count_ = shard_count;
    valid_ = true;
}

bool ShardPlan::valid_channel(std::uint32_t channel_no) {
    return channel_no >= kFirstChannel && channel_no <= kLastChannel;
}

bool ShardPlan::valid_symbol(const std::string& symbol) {
    if (symbol.size() != 6U) return false;
    for (std::size_t i = 0; i < symbol.size(); ++i) {
        if (symbol[i] < '0' || symbol[i] > '9') return false;
    }
    return true;
}

bool ShardPlan::valid_shard(std::uint32_t shard) const {
    return valid_ && static_cast<std::size_t>(shard) < shard_count_;
}

bool ShardPlan::fail(std::string* error, const std::string& message) {
    if (error) *error = message;
    return false;
}

std::uint32_t ShardPlan::choose_shard(std::uint32_t channel_no) const {
    const std::vector<std::size_t>& channel = counts_[channel_no - kFirstChannel];
    std::size_t best = 0;
    for (std::size_t shard = 1; shard < shard_count_; ++shard) {
        // Balance within this channel first.  Across equal channel loads,
        // prefer the globally least-used shard and then the lowest id.
        if (channel[shard] < channel[best] ||
            (channel[shard] == channel[best] && totals_[shard] < totals_[best])) {
            best = shard;
        }
    }
    return static_cast<std::uint32_t>(best);
}

void ShardPlan::record(std::uint32_t channel_no, std::uint32_t shard) {
    ++counts_[channel_no - kFirstChannel][shard];
    ++totals_[shard];
}

bool ShardPlan::assign(std::uint32_t channel_no, const std::string& symbol,
                       std::uint32_t* shard, std::string* error) {
    if (error) error->clear();
    if (!valid_) return fail(error, "invalid SSE shard plan");
    if (!shard) return fail(error, "null shard output");
    if (!valid_channel(channel_no))
        return fail(error, "invalid Shanghai channel: expected 1..6");
    if (!valid_symbol(symbol))
        return fail(error, "invalid Shanghai stock symbol: expected six digits");

    const std::map<std::string, Assignment>::const_iterator existing =
        assignments_.find(symbol);
    if (existing != assignments_.end()) {
        if (existing->second.channel_no != channel_no)
            return fail(error, "stock is already assigned to a different Shanghai channel");
        *shard = existing->second.shard;
        return true;
    }

    const std::uint32_t selected = choose_shard(channel_no);
    assignments_.insert(std::make_pair(symbol, Assignment(channel_no, selected)));
    record(channel_no, selected);
    *shard = selected;
    return true;
}

bool ShardPlan::lookup(const std::string& symbol, std::uint32_t* channel_no,
                       std::uint32_t* shard) const {
    if (!valid_ || !channel_no || !shard || !valid_symbol(symbol)) return false;
    const std::map<std::string, Assignment>::const_iterator existing =
        assignments_.find(symbol);
    if (existing == assignments_.end()) return false;
    *channel_no = existing->second.channel_no;
    *shard = existing->second.shard;
    return true;
}

bool ShardPlan::restore(std::uint32_t channel_no, const std::string& symbol,
                        std::uint32_t shard, std::string* error) {
    if (error) error->clear();
    if (!valid_) return fail(error, "invalid SSE shard plan");
    if (!valid_channel(channel_no))
        return fail(error, "invalid Shanghai channel: expected 1..6");
    if (!valid_symbol(symbol))
        return fail(error, "invalid Shanghai stock symbol: expected six digits");
    if (!valid_shard(shard)) return fail(error, "shard is outside this plan");

    const std::map<std::string, Assignment>::const_iterator existing =
        assignments_.find(symbol);
    if (existing != assignments_.end()) {
        if (existing->second.channel_no == channel_no && existing->second.shard == shard)
            return true;
        return fail(error, "restored stock route conflicts with its existing assignment");
    }

    assignments_.insert(std::make_pair(symbol, Assignment(channel_no, shard)));
    record(channel_no, shard);
    return true;
}

InferenceShardPlan::InferenceShardPlan(
    std::size_t shard_count,
    const std::map<std::string, double>& frequency_weights)
    : shard_count_(0), valid_(false), frequency_weights_(), counts_(),
      weighted_counts_(), totals_(), weighted_totals_(), assignments_() {
    if (shard_count == 0 ||
        shard_count > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()))
        return;
    for (std::map<std::string, double>::const_iterator it = frequency_weights.begin();
         it != frequency_weights.end(); ++it) {
        if (!valid_symbol(it->first) || !std::isfinite(it->second) || it->second <= 0.0)
            return;
    }
    try {
        counts_.assign(kLastChannel,
                       std::vector<std::size_t>(shard_count, static_cast<std::size_t>(0)));
        weighted_counts_.assign(kLastChannel,
                                std::vector<double>(shard_count, 0.0));
        totals_.assign(shard_count, static_cast<std::size_t>(0));
        weighted_totals_.assign(shard_count, 0.0);
        frequency_weights_ = frequency_weights;
    } catch (const std::bad_alloc&) {
        counts_.clear();
        weighted_counts_.clear();
        totals_.clear();
        weighted_totals_.clear();
        return;
    } catch (const std::length_error&) {
        counts_.clear();
        weighted_counts_.clear();
        totals_.clear();
        weighted_totals_.clear();
        return;
    }
    shard_count_ = shard_count;
    valid_ = true;
}

bool InferenceShardPlan::valid_channel(std::uint32_t channel_no) {
    return channel_no >= kFirstChannel && channel_no <= kLastChannel;
}

bool InferenceShardPlan::valid_symbol(const std::string& symbol) {
    if (symbol.size() != 6U) return false;
    for (std::size_t i = 0; i < symbol.size(); ++i)
        if (symbol[i] < '0' || symbol[i] > '9') return false;
    return true;
}

bool InferenceShardPlan::valid_shard(std::uint32_t shard) const {
    return valid_ && static_cast<std::size_t>(shard) < shard_count_;
}

bool InferenceShardPlan::fail(std::string* error, const std::string& message) {
    if (error) *error = message;
    return false;
}

double InferenceShardPlan::weight_for(const std::string& symbol) const {
    const std::map<std::string, double>::const_iterator found =
        frequency_weights_.find(symbol);
    return found == frequency_weights_.end() ? 1.0 : found->second;
}

void InferenceShardPlan::add_weight(double* value, double amount) {
    const double maximum = (std::numeric_limits<double>::max)();
    if (amount >= maximum - *value) *value = maximum;
    else *value += amount;
}

std::uint32_t InferenceShardPlan::choose_channel_shard(
    std::uint32_t channel_no, double /*weight*/) const {
    const std::vector<std::size_t>& channel = counts_[channel_no - kFirstChannel];
    const std::vector<double>& weighted = weighted_counts_[channel_no - kFirstChannel];
    std::size_t best = 0;
    for (std::size_t shard = 1; shard < shard_count_; ++shard) {
        // Expected-frequency load is the primary balance signal.  Count and
        // global load keep ties deterministic and spread sparse channels.
        if (weighted[shard] < weighted[best] ||
            (weighted[shard] == weighted[best] &&
             (channel[shard] < channel[best] ||
              (channel[shard] == channel[best] &&
               (weighted_totals_[shard] < weighted_totals_[best] ||
                (weighted_totals_[shard] == weighted_totals_[best] &&
                 totals_[shard] < totals_[best])))))) {
            best = shard;
        }
    }
    return static_cast<std::uint32_t>(best);
}

std::uint32_t InferenceShardPlan::choose_unknown_shard(double /*weight*/) const {
    std::size_t best = 0;
    for (std::size_t shard = 1; shard < shard_count_; ++shard) {
        if (weighted_totals_[shard] < weighted_totals_[best] ||
            (weighted_totals_[shard] == weighted_totals_[best] &&
             totals_[shard] < totals_[best])) {
            best = shard;
        }
    }
    return static_cast<std::uint32_t>(best);
}

void InferenceShardPlan::record_channel(std::uint32_t channel_no,
                                        std::uint32_t shard, double weight) {
    ++counts_[channel_no - kFirstChannel][shard];
    add_weight(&weighted_counts_[channel_no - kFirstChannel][shard], weight);
}

void InferenceShardPlan::record_unknown(std::uint32_t shard, double weight) {
    ++totals_[shard];
    add_weight(&weighted_totals_[shard], weight);
}

bool InferenceShardPlan::assign(std::uint32_t channel_no, const std::string& symbol,
                                std::uint32_t* shard, std::string* error) {
    if (error) error->clear();
    // Zero is the wire-level "ChannelNo not known yet" value on snapshots.
    if (channel_no == 0U) return assign(symbol, shard, error);
    if (!valid_) return fail(error, "invalid SSE inference shard plan");
    if (!shard) return fail(error, "null shard output");
    if (!valid_channel(channel_no))
        return fail(error, "invalid Shanghai channel: expected 1..6");
    if (!valid_symbol(symbol))
        return fail(error, "invalid Shanghai stock symbol: expected six digits");

    const std::map<std::string, Assignment>::iterator existing =
        assignments_.find(symbol);
    if (existing != assignments_.end()) {
        if (existing->second.channel_no != 0U &&
            existing->second.channel_no != channel_no)
            return fail(error, "stock is already assigned to a different Shanghai channel");
        if (existing->second.channel_no == 0U) {
            existing->second.channel_no = channel_no;
            record_channel(channel_no, existing->second.shard, weight_for(symbol));
        }
        *shard = existing->second.shard;
        return true;
    }

    const double weight = weight_for(symbol);
    const std::uint32_t selected = choose_channel_shard(channel_no, weight);
    assignments_.insert(std::make_pair(symbol, Assignment(channel_no, selected)));
    record_channel(channel_no, selected, weight);
    ++totals_[selected];
    add_weight(&weighted_totals_[selected], weight);
    *shard = selected;
    return true;
}

bool InferenceShardPlan::assign(const std::string& symbol, std::uint32_t* shard,
                                std::string* error) {
    if (error) error->clear();
    if (!valid_) return fail(error, "invalid SSE inference shard plan");
    if (!shard) return fail(error, "null shard output");
    if (!valid_symbol(symbol))
        return fail(error, "invalid Shanghai stock symbol: expected six digits");
    const std::map<std::string, Assignment>::const_iterator existing =
        assignments_.find(symbol);
    if (existing != assignments_.end()) {
        *shard = existing->second.shard;
        return true;
    }

    const double weight = weight_for(symbol);
    const std::uint32_t selected = choose_unknown_shard(weight);
    assignments_.insert(std::make_pair(symbol, Assignment(0U, selected)));
    record_unknown(selected, weight);
    *shard = selected;
    return true;
}

bool InferenceShardPlan::register_channel(std::uint32_t channel_no,
                                          const std::string& symbol,
                                          std::uint32_t* shard,
                                          std::string* error) {
    if (error) error->clear();
    if (!valid_) return fail(error, "invalid SSE inference shard plan");
    if (!shard) return fail(error, "null shard output");
    if (!valid_channel(channel_no))
        return fail(error, "invalid Shanghai channel: expected 1..6");
    if (!valid_symbol(symbol))
        return fail(error, "invalid Shanghai stock symbol: expected six digits");
    const std::map<std::string, Assignment>::iterator existing =
        assignments_.find(symbol);
    if (existing == assignments_.end()) return assign(channel_no, symbol, shard, error);
    if (existing->second.channel_no != 0U &&
        existing->second.channel_no != channel_no)
        return fail(error, "stock is already assigned to a different Shanghai channel");
    if (existing->second.channel_no == 0U) {
        existing->second.channel_no = channel_no;
        record_channel(channel_no, existing->second.shard, weight_for(symbol));
    }
    *shard = existing->second.shard;
    return true;
}

bool InferenceShardPlan::lookup(const std::string& symbol,
                                std::uint32_t* channel_no,
                                std::uint32_t* shard) const {
    if (!valid_ || !channel_no || !shard || !valid_symbol(symbol)) return false;
    const std::map<std::string, Assignment>::const_iterator existing =
        assignments_.find(symbol);
    if (existing == assignments_.end()) return false;
    *channel_no = existing->second.channel_no;
    *shard = existing->second.shard;
    return true;
}

bool InferenceShardPlan::restore(std::uint32_t channel_no, const std::string& symbol,
                                 std::uint32_t shard, std::string* error) {
    if (error) error->clear();
    if (!valid_) return fail(error, "invalid SSE inference shard plan");
    if (!valid_channel(channel_no))
        return fail(error, "invalid Shanghai channel: expected 1..6");
    if (!valid_symbol(symbol))
        return fail(error, "invalid Shanghai stock symbol: expected six digits");
    if (!valid_shard(shard)) return fail(error, "shard is outside this plan");
    const std::map<std::string, Assignment>::iterator existing =
        assignments_.find(symbol);
    if (existing != assignments_.end()) {
        if (existing->second.shard != shard)
            return fail(error, "restored stock route conflicts with its existing assignment");
        if (existing->second.channel_no == 0U) {
            existing->second.channel_no = channel_no;
            record_channel(channel_no, shard, weight_for(symbol));
            return true;
        }
        if (existing->second.channel_no == channel_no) return true;
        return fail(error, "restored stock route conflicts with its existing channel");
    }
    assignments_.insert(std::make_pair(symbol, Assignment(channel_no, shard)));
    record_channel(channel_no, shard, weight_for(symbol));
    ++totals_[shard];
    add_weight(&weighted_totals_[shard], weight_for(symbol));
    return true;
}

}  // namespace sse_pipeline
