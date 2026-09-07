#ifndef T0_PREDICTOR_MIX153060_CAPTURE_H
#define T0_PREDICTOR_MIX153060_CAPTURE_H

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "sze/sampling/mix153060_runtime.h"

namespace mix153060 {

class AsyncPredictionLog;

struct CaptureConfig {
    bool enabled;
    std::string directory;
    std::string prefix;
    std::vector<std::string> instruments;
    std::string output_format;
    std::vector<std::string> detail_instruments;
    bool record_events;
    bool record_samples;
    bool capture_only;
    std::size_t flush_rows;
    std::uint32_t flush_interval_ms;
    std::size_t log_batch_bytes;
    std::size_t log_queue_bytes;

    CaptureConfig();
};

// Buffered, opt-in diagnostics for a small instrument allowlist. This class is
// intentionally independent of the strategy and JSON layers so it can also be
// used by deterministic native replay tests.
class Capture {
public:
    explicit Capture(const CaptureConfig& config);
    ~Capture();

    bool ready() const;
    bool enabled_for(const std::string& instrument) const;
    bool detail_enabled_for(const std::string& instrument) const;
    const std::string& error() const;
    const std::string& events_path() const;
    const std::string& samples_path() const;
    const std::string& market_resolutions_path() const;

    void record_order(const std::string& instrument,
                      const OrderEvent& event,
                      short source,
                      std::int64_t framework_receive_time,
                      const EventTiming& timing,
                      bool accepted,
                      std::size_t samples_emitted,
                      bool source_continuity_valid = true);
    void record_trade(const std::string& instrument,
                      const TradeEvent& event,
                      short source,
                      std::int64_t framework_receive_time,
                      const EventTiming& timing,
                      bool accepted,
                      std::size_t samples_emitted,
                      bool source_continuity_valid = true);
    void record_market_resolution(const std::string& instrument,
                                  const OrderEvent& event,
                                  bool from_linked_fill,
                                  short source,
                                  std::int64_t framework_receive_time,
                                  bool source_continuity_valid = true);
    void record_sample(const std::string& instrument,
                       const Sample& sample,
                       short source,
                       std::int64_t framework_receive_time,
                       float raw_prediction,
                       std::uint64_t model_latency_ns,
                       bool source_continuity_valid = true);
    void flush();

private:
    Capture(const Capture&);
    Capture& operator=(const Capture&);

    void record_event(const std::string& instrument,
                      const char* event_type,
                      const char* event_kind,
                      std::int64_t app_sequence,
                      std::int64_t exchange_time_us,
                      std::int64_t local_time_us,
                      double price,
                      std::int64_t volume,
                      std::int64_t buy_order_id,
                      std::int64_t sell_order_id,
                      short source,
                      std::int64_t framework_receive_time,
                      const EventTiming& timing,
                      bool accepted,
                      std::size_t samples_emitted,
                      bool source_continuity_valid);
    void maybe_flush();
    void fail(const std::string& message);

    bool enabled_;
    bool ready_;
    bool record_events_;
    bool record_samples_;
    std::size_t flush_rows_;
    std::uint32_t flush_interval_ms_;
    std::size_t pending_rows_;
    std::uint64_t last_flush_mono_ns_;
    std::set<std::string> instruments_;
    std::set<std::string> detail_instruments_;
    bool binary_log_;
    std::string error_;
    std::string events_path_;
    std::string samples_path_;
    std::string market_resolutions_path_;
    std::string events_buffer_;
    std::string samples_buffer_;
    std::string market_resolutions_buffer_;
    std::ofstream events_file_;
    std::ofstream samples_file_;
    std::ofstream market_resolutions_file_;
    std::unique_ptr<AsyncPredictionLog> prediction_log_;
};

}  // namespace mix153060

#endif  // T0_PREDICTOR_MIX153060_CAPTURE_H
