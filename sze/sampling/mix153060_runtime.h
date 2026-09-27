#ifndef T0_PREDICTOR_MIX153060_RUNTIME_H
#define T0_PREDICTOR_MIX153060_RUNTIME_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "common/model/mix153060/mix153060_model.h"

namespace mix153060 {

static const std::size_t kTopLevels = 10;
static const std::size_t kModelDepth = 5;

enum class OrderKind {
    kLimit,
    kMarket,
    kSelfBest
};

enum class TradeKind {
    kFill,
    kCancel
};

struct StaticInputs {
    std::string instrument;
    int32_t trading_date;
    double average_amount;
    double turnover_threshold;
    double free_share;
    double pre_close;
    double upper_limit;
    double lower_limit;
    double history_volatility_20d;
    // V06 baseline sampling contract. False preserves the deployed A3 stream.
    bool v06_baseline;

    StaticInputs();
    bool valid() const;
};

struct OrderEvent {
    int64_t app_sequence;
    int64_t exchange_time_us;
    int64_t local_time_us;
    double price;
    int64_t volume;
    bool buy;
    OrderKind kind;

    OrderEvent();
};

struct TradeEvent {
    int64_t app_sequence;
    int64_t exchange_time_us;
    int64_t local_time_us;
    double price;
    int64_t volume;
    int64_t buy_order_id;
    int64_t sell_order_id;
    TradeKind kind;

    TradeEvent();
};

struct Sample {
    std::array<float, kFeatureCount> factors;
    std::array<double, 10> bid_price;
    std::array<double, 10> ask_price;
    std::array<double, 10> bid_volume;
    std::array<double, 10> ask_volume;
    std::string instrument;
    int64_t exchange_time_us;
    int64_t local_time_us;
    int64_t app_sequence;
    int64_t cut_index;
    int64_t row_in_stock_day;
    int64_t window_start_exchange_time_us;
    int64_t window_start_app_sequence;
    int64_t window_start_cut_index;
    double last_price;
    double mid_price;
    double turnover;
    double volume;
    bool amount_trigger;
    bool time_trigger;
    bool change_trigger;

    Sample();
};

struct SampleBuffer {
    std::array<Sample, 2> values;
    std::size_t count;

    SampleBuffer();
    void clear();
};

// Optional per-callback timing. A null pointer keeps the production hot path
// free of clock reads; the mutation interval surrounds only book.add/book.fill.
struct EventTiming {
    std::uint64_t book_mutation_ns;
    std::uint64_t sample_work_ns;
    std::uint64_t total_runtime_ns;
    EventTiming();
    void clear();
};

// Parse a feed clock such as HH:MM:SS.mmm or HH:MM:SS.ffffff. When a trading
// date is supplied, the result uses the same UTC-like epoch convention as the
// research handoff (the exchange-local clock is not timezone-shifted).
bool parse_exchange_time_us(const char* text,
                            int32_t trading_date,
                            int64_t* result);

// Convert a framework receive timestamp to the same epoch convention. The
// backtest timestamp is HHMMSSmmmuuunnn; live callbacks normally use ns/us.
int64_t normalize_receive_time_us(int64_t receive_time,
                                  int32_t trading_date,
                                  int64_t exchange_time_us);

// Recover a capture timestamp recorded with CLOCK_MONOTONIC into the same
// exchange-local, UTC-like epoch used by parse_exchange_time_us(). The two
// reference clocks must come from the same host boot as receive_mono_ns.
// Historical/offline replay without that guarantee must use exchange_time_us.
int64_t recover_monotonic_receive_time_us(std::uint64_t receive_mono_ns,
                                          std::uint64_t reference_mono_ns,
                                          std::uint64_t reference_realtime_ns,
                                          int32_t trading_date,
                                          int64_t exchange_time_us);

class Runtime {
public:
    explicit Runtime(const StaticInputs& inputs = StaticInputs());
    ~Runtime();

    Runtime(Runtime&& other) noexcept;
    Runtime& operator=(Runtime&& other) noexcept;

    bool configured() const;
    const StaticInputs& inputs() const;

    // Orders are held until the next event so same-time fills can be grouped
    // into one minimatch frame. This affects only feature/model timing; book
    // mutation remains outside the legacy snapshot path.
    void on_order(const OrderEvent& event,
                 SampleBuffer* output,
                 EventTiming* timing = 0);
    void on_trade(const TradeEvent& event,
                  SampleBuffer* output,
                  EventTiming* timing = 0);
    void flush(SampleBuffer* output);
    // Returns a reconstructed price for one raw zero-price SZE market order.
    // The result is consumed so callers can write a separate audit stream
    // without changing the canonical raw event record.
    bool take_resolved_market_order(OrderEvent* event, bool* from_linked_fill);
    void reset();
    void invalidate();

    std::size_t frame_count() const;
    std::size_t sample_count() const;
    bool available() const;
    int64_t failure_sequence() const;
    const std::string& failure_reason() const;

private:
    Runtime(const Runtime&);
    Runtime& operator=(const Runtime&);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace mix153060

#endif  // T0_PREDICTOR_MIX153060_RUNTIME_H
