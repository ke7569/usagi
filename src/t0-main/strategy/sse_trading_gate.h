#ifndef SSE_T0_TRADING_GATE_H
#define SSE_T0_TRADING_GATE_H

// SSE trading catch-up gate.
//
// Compares the exchange event time (MarketTime, HHMMSSmmm, e.g. 93100000 ==
// 09:31:00.000) against the local wall-clock time of day.  While the gap is
// larger than the configured threshold (default 1 second) the gate blocks
// order submission; it re-opens automatically once the gap returns inside the
// threshold.  State transitions are reported through an injectable sink so
// production can log "blocked"/"resumed" and tests can assert them.

#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <functional>
#include <string>
#include <sys/time.h>

namespace sse_trading {

static const std::uint64_t kDefaultCatchUpThresholdUs = 1000000ULL;  // 1s

// MarketTime (HHMMSSmmm) -> microseconds since midnight.
inline std::uint64_t market_time_to_micros(double market_time) {
    const long long v = static_cast<long long>(market_time + 0.5);
    const long long mmm = v % 1000LL;
    const long long rest = v / 1000LL;
    const long long ss = rest % 100LL;
    const long long mm = (rest / 100LL) % 100LL;
    const long long hh = rest / 10000LL;
    return static_cast<std::uint64_t>(
        ((hh * 3600LL + mm * 60LL + ss) * 1000000LL) + mmm * 1000LL);
}

inline std::uint64_t local_time_of_day_micros() {
    struct timeval tv;
    gettimeofday(&tv, 0);
    const std::time_t seconds = tv.tv_sec;
    struct tm value;
    localtime_r(&seconds, &value);
    return static_cast<std::uint64_t>(
        (value.tm_hour * 3600 + value.tm_min * 60 + value.tm_sec)) * 1000000ULL +
        static_cast<std::uint64_t>(tv.tv_usec);
}

inline std::int64_t gate_lag_us(double market_time, std::uint64_t local_us) {
    const std::uint64_t exchange_us = market_time_to_micros(market_time);
    const std::int64_t diff =
        static_cast<std::int64_t>(local_us) - static_cast<std::int64_t>(exchange_us);
    return diff < 0 ? -diff : diff;
}

class TradingGate {
public:
    TradingGate()
        : enabled_(false), threshold_us_(kDefaultCatchUpThresholdUs),
          blocked_(false), sink_() {}

    void configure(bool enabled, std::uint64_t threshold_us) {
        enabled_ = enabled;
        threshold_us_ = threshold_us == 0 ? kDefaultCatchUpThresholdUs
                                          : threshold_us;
    }

    void set_sink(const std::function<void(const std::string&)>& sink) {
        sink_ = sink;
    }

    bool blocked() const { return blocked_; }

    // Returns true when an order may be submitted.  market_time is the exchange
    // event time (HHMMSSmmm); local_us is the local time-of-day in microseconds.
    // When the state changes, *transition receives "blocked" or "resumed".
    bool permit(double market_time, std::uint64_t local_us,
                std::string* transition = 0) {
        if (!enabled_) {
            if (blocked_) {
                blocked_ = false;
                emit("resumed", transition);
            }
            return true;
        }
        if (market_time <= 0.0) return false;
        const bool block = gate_lag_us(market_time, local_us) >
                           static_cast<std::int64_t>(threshold_us_);
        if (block != blocked_) {
            blocked_ = block;
            emit(block ? "blocked" : "resumed", transition);
        }
        return !block;
    }

private:
    void emit(const char* state, std::string* transition) {
        const std::string message(state);
        if (transition) *transition = message;
        if (sink_) sink_(message);
    }

    bool enabled_;
    std::uint64_t threshold_us_;
    bool blocked_;
    std::function<void(const std::string&)> sink_;
};

// One gate per process, shared by every per-instrument strategy instance.
inline TradingGate& global_trading_gate() {
    static TradingGate gate;
    return gate;
}

}  // namespace sse_trading

#endif  // SSE_T0_TRADING_GATE_H
