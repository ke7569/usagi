#ifndef SZE_V06_ACCOUNT_STATE_H
#define SZE_V06_ACCOUNT_STATE_H
#include "V06StrategyRules.h"
#include <cmath>
#include <map>
#include <mutex>
#include <set>
#include <string>

// One account-wide state shared by the strategy instances. Its recursive
// mutex also serializes synchronous gateway callbacks with order submission.
class V06AccountState {
public:
    mutable std::recursive_mutex mutex;
    struct Instrument {
        long long base = 0, initial_total = 0;
        double open = 0;
        bool synced = false;
    };
    void register_instrument(const std::string& code, long long base) {
        std::lock_guard<std::recursive_mutex> lock(mutex);
        instruments_[code].base = base;
    }
    void sync_position(const std::string& code, long long total) {
        std::lock_guard<std::recursive_mutex> lock(mutex);
        Instrument& p = instruments_[code];
        if (!initial_locked_) { p.initial_total = total; p.synced = true; }
    }
    bool observe_open(const std::string& code, double price) {
        std::lock_guard<std::recursive_mutex> lock(mutex);
        const auto it = instruments_.find(code);
        if (it != instruments_.end() && it->second.open == 0 &&
            std::isfinite(price) && price > 0) {
            it->second.open = price;
            return true;
        }
        return false;
    }
    bool observe_execution_open(const std::string& code, double price,
                                std::uint32_t time_of_day_ms,
                                std::uint32_t event_day, std::uint32_t expected_day) {
        // The capture journal is replayed from before the opening auction.
        // No auction match means the first continuous-session match is open.
        const std::uint32_t begin = (9U * 3600U + 25U * 60U) * 1000U;
        const std::uint32_t close = 15U * 3600U * 1000U;
        if (!expected_day || event_day != expected_day || time_of_day_ms < begin ||
            time_of_day_ms > close) return false;
        return observe_open(code, price);
    }
    void observe_cumulative(const std::string& order_key, long long cumulative) {
        std::lock_guard<std::recursive_mutex> lock(mutex);
        if (cumulative < 0) { healthy_ = false; return; }
        auto& coverage = quantities_[order_key];
        coverage.first = std::max(coverage.first, cumulative);
    }
    void fill(const std::string& key, bool buy, long long quantity, double price,
              const std::string& order_key = std::string()) {
        std::lock_guard<std::recursive_mutex> lock(mutex);
        if (quantity <= 0 || !std::isfinite(price) || price <= 0 || key.empty()) {
            healthy_ = false; return;
        }
        const double amount = (buy ? 1.0 : -1.0) * quantity * price;
        auto inserted = fills_.insert(std::make_pair(key, amount));
        if (inserted.second) {
            traded_ += amount;
            if (!order_key.empty()) quantities_[order_key].second += quantity;
        }
        else if (inserted.first->second != amount) healthy_ = false;
    }
    bool ready() const {
        std::lock_guard<std::recursive_mutex> lock(mutex);
        if (!healthy_ || instruments_.empty()) return false;
        for (const auto& item : quantities_)
            if (item.second.first != item.second.second) return false;
        for (const auto& item : instruments_) {
            const Instrument& p = item.second;
            if (!p.synced || p.base < 0) return false;
            if ((p.base != 0 || p.initial_total != 0) && p.open <= 0) return false;
        }
        return true;
    }
    bool needs_market_data(const std::string& code) const {
        std::lock_guard<std::recursive_mutex> lock(mutex);
        const auto it = instruments_.find(code);
        return it != instruments_.end() &&
            (!it->second.synced || it->second.base > 0 || it->second.initial_total > 0);
    }
    v06_strategy::DirectionalSkew skew(long long exchange_us,
                                      const v06_strategy::Config& config) {
        std::lock_guard<std::recursive_mutex> lock(mutex);
        if (!ready()) return v06_strategy::DirectionalSkew();
        initial_locked_ = true;
        bool has_base = false;
        for (const auto& item : instruments_) has_base = has_base || item.second.base > 0;
        if (!has_base) {
            // All instruments are sell-only. No target-capital denominator
            // exists; volume clamps still enforce actual sellable inventory.
            v06_strategy::DirectionalSkew neutral;
            neutral.valid = true;
            return neutral;
        }
        const long long boundary = exchange_us / 10000000LL;
        if (boundary != boundary_) {
            v06_strategy::GlobalExposureInput input;
            for (const auto& item : instruments_) {
                const Instrument& p = item.second;
                if (p.base == 0 && p.initial_total == 0) continue;
                v06_strategy::GlobalExposureInstrument i;
                i.static_position_after_adjustment = p.base;
                i.position_after_adjustment = p.initial_total;
                i.open_price = p.open;
                input.instruments.push_back(i);
            }
            input.net_traded_amount = traded_;
            skew_ = v06_strategy::directionalSkew(v06_strategy::globalExposure(input), config);
            boundary_ = boundary;
        }
        return skew_;
    }
    void invalidate() { std::lock_guard<std::recursive_mutex> lock(mutex); healthy_ = false; }
private:
    std::map<std::string, Instrument> instruments_;
    std::map<std::string, double> fills_;
    std::map<std::string, std::pair<long long, long long> > quantities_;
    double traded_ = 0;
    bool healthy_ = true;
    bool initial_locked_ = false;
    long long boundary_ = -1;
    v06_strategy::DirectionalSkew skew_;
};
#endif
