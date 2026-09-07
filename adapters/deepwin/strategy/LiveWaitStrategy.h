#ifndef LIVE_WAIT_STRATEGY_H
#define LIVE_WAIT_STRATEGY_H

#include "bse_shard_types.h"

#include <chrono>
#include <cstdint>
#include <sstream>
#include <string>
#include <thread>

namespace live_wait_detail {
inline void cpu_relax() {
#if defined(__x86_64__) || defined(__i386__)
    __asm__ __volatile__("pause" ::: "memory");
#else
    std::this_thread::yield();
#endif
}
}

class LiveWaitStrategy {
public:
    explicit LiveWaitStrategy(const LiveWaitConfig& config) : config_(config) {}

    void reset() {
        idle_rounds_ = 0;
    }

    void on_idle() {
        ++idle_rounds_;
        switch (config_.mode) {
            case LiveWaitMode::Spin:
                live_wait_detail::cpu_relax();
                return;
            case LiveWaitMode::Yield:
                ++yield_count_;
                std::this_thread::yield();
                return;
            case LiveWaitMode::SpinPauseThenYieldSleep:
                if (config_.spin_rounds_before_yield > 0 &&
                    idle_rounds_ <= config_.spin_rounds_before_yield) {
                    live_wait_detail::cpu_relax();
                    return;
                }
                if (config_.yield_rounds_before_sleep > 0 &&
                    idle_rounds_ > (config_.spin_rounds_before_yield + config_.yield_rounds_before_sleep) &&
                    config_.sleep_us > 0) {
                    ++sleep_count_;
                    std::this_thread::sleep_for(std::chrono::microseconds(config_.sleep_us));
                    return;
                }
                ++yield_count_;
                std::this_thread::yield();
                return;
            case LiveWaitMode::SpinPauseThenYield:
            default:
                if (config_.spin_rounds_before_yield > 0 &&
                    idle_rounds_ <= config_.spin_rounds_before_yield) {
                    live_wait_detail::cpu_relax();
                    return;
                }
                ++yield_count_;
                std::this_thread::yield();
                return;
        }
    }

    uint64_t idle_rounds() const {
        return idle_rounds_;
    }

    uint64_t yield_count() const {
        return yield_count_;
    }

    uint64_t sleep_count() const {
        return sleep_count_;
    }

    std::string describe() const {
        std::ostringstream oss;
        oss << "mode=" << mode_name(config_.mode)
            << " spin_rounds_before_yield=" << config_.spin_rounds_before_yield
            << " yield_rounds_before_sleep=" << config_.yield_rounds_before_sleep
            << " sleep_us=" << config_.sleep_us;
        return oss.str();
    }

    static const char* mode_name(LiveWaitMode mode) {
        switch (mode) {
            case LiveWaitMode::Spin:
                return "spin";
            case LiveWaitMode::Yield:
                return "yield";
            case LiveWaitMode::SpinPauseThenYield:
                return "spin_pause_then_yield";
            case LiveWaitMode::SpinPauseThenYieldSleep:
                return "spin_pause_then_yield_sleep";
            default:
                return "unknown";
        }
    }

private:
    LiveWaitConfig config_;
    uint64_t idle_rounds_ = 0;
    uint64_t yield_count_ = 0;
    uint64_t sleep_count_ = 0;
};

#endif // LIVE_WAIT_STRATEGY_H
