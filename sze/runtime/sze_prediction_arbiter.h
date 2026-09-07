#ifndef SZE_PREDICTION_ARBITER_H
#define SZE_PREDICTION_ARBITER_H

#include <cmath>
#include <cstdint>

namespace sze_prediction {

enum Source {
    kFullOrderBook = 1,
    kSnapshot = 2
};

struct Candidate {
    Source source = kFullOrderBook;
    std::uint32_t trading_day = 0;
    std::uint64_t exchange_time_us = 0;
    double turnover = 0.0;
    std::uint64_t sequence = 0;
    bool valid = false;
};

struct Selection {
    Source source = kFullOrderBook;
    std::uint64_t sequence = 0;
    bool ready = false;
};

class Arbiter {
public:
    Arbiter(double turnover_epsilon = 0.01,
            std::uint64_t max_staleness_us = 4000000U)
        : turnover_epsilon_(turnover_epsilon),
          max_staleness_us_(max_staleness_us) {}

    bool update(const Candidate& candidate) {
        if (!candidate.valid || candidate.trading_day < 20000101U ||
            candidate.exchange_time_us == 0U || candidate.sequence == 0U ||
            !std::isfinite(candidate.turnover) || candidate.turnover < 0.0) {
            return false;
        }
        if (trading_day_ == 0U || candidate.trading_day > trading_day_) {
            reset(candidate.trading_day);
        } else if (candidate.trading_day < trading_day_) {
            return false;
        }
        Candidate* slot = candidate.source == kSnapshot ? &snapshot_ : &full_;
        if (slot->valid &&
            candidate.exchange_time_us < slot->exchange_time_us) {
            return false;
        }
        *slot = candidate;
        return true;
    }

    Selection select(bool full_runtime_valid) const {
        Selection result;
        const std::uint64_t latest_time = latest_exchange_time_us();
        const bool full_usable = full_runtime_valid && fresh(full_, latest_time);
        const bool snapshot_usable = fresh(snapshot_, latest_time);
        if (full_usable &&
            (!snapshot_usable ||
             full_.turnover + turnover_epsilon_ >= snapshot_.turnover)) {
            result.source = kFullOrderBook;
            result.sequence = full_.sequence;
            result.ready = true;
        } else if (snapshot_usable) {
            result.source = kSnapshot;
            result.sequence = snapshot_.sequence;
            result.ready = true;
        }
        return result;
    }

    bool should_dispatch(const Selection& selection) const {
        return selection.ready &&
               (!last_dispatched_.ready ||
                selection.source != last_dispatched_.source ||
                selection.sequence != last_dispatched_.sequence);
    }

    void mark_dispatched(const Selection& selection) {
        if (selection.ready) {
            last_dispatched_ = selection;
        }
    }

private:
    void reset(std::uint32_t trading_day) {
        trading_day_ = trading_day;
        full_ = Candidate();
        snapshot_ = Candidate();
        last_dispatched_ = Selection();
    }

    std::uint64_t latest_exchange_time_us() const {
        std::uint64_t latest = 0U;
        if (full_.valid && full_.exchange_time_us > latest) {
            latest = full_.exchange_time_us;
        }
        if (snapshot_.valid && snapshot_.exchange_time_us > latest) {
            latest = snapshot_.exchange_time_us;
        }
        return latest;
    }

    bool fresh(const Candidate& candidate, std::uint64_t latest_time) const {
        return candidate.valid && candidate.trading_day == trading_day_ &&
               candidate.exchange_time_us <= latest_time &&
               latest_time - candidate.exchange_time_us <= max_staleness_us_;
    }

    double turnover_epsilon_;
    std::uint64_t max_staleness_us_;
    std::uint32_t trading_day_ = 0;
    Candidate full_;
    Candidate snapshot_;
    Selection last_dispatched_;
};

}  // namespace sze_prediction

#endif
