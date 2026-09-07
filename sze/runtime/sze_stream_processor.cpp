#include "sze/runtime/sze_stream_processor.h"

#include "sze/market_data/SZEProtocol.h"
#include "sze/market_data/SZERecoverable.h"
#include "sze/runtime/mix153060_live_adapter.h"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <cstring>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

namespace sze_stream {
namespace {

enum InputMode {
    kInputUnset,
    kInputLive,
    kInputRecovery
};

struct OutputProvenance {
    std::uint64_t ingress_sequence;
    std::uint32_t channel_id;
    std::size_t record_offset;
    std::size_t record_size;
    std::uint64_t recovery_event_id;
    std::uint64_t recovery_feed_sequence;
    std::uint64_t recovery_channel_sequence;
    std::uint16_t recovery_source_id;

    OutputProvenance()
        : ingress_sequence(0), channel_id(0), record_offset(0), record_size(0),
          recovery_event_id(0), recovery_feed_sequence(0),
          recovery_channel_sequence(0), recovery_source_id(0) {}
};

std::string normalize_symbol(const char* value) {
    if (value == 0) {
        return std::string();
    }
    std::string result(value);
    while (!result.empty() && result.back() == ' ') {
        result.pop_back();
    }
    const std::size_t dot = result.find('.');
    if (dot != std::string::npos) {
        result.resize(dot);
    }
    return result;
}

std::string normalize_symbol(const std::string& value) {
    return normalize_symbol(value.c_str());
}

bool valid_shenzhen_symbol(const std::string& value) {
    const std::string key = normalize_symbol(value);
    if (key.size() != 6U) {
        return false;
    }
    for (std::size_t i = 0; i < key.size(); ++i) {
        if (key[i] < '0' || key[i] > '9') {
            return false;
        }
    }
    if (value.size() == 6U) {
        return true;
    }
    return value.size() == 9U && value[6] == '.' &&
           (value[7] == 'S' || value[7] == 's') &&
           (value[8] == 'Z' || value[8] == 'z');
}

const char* sequence_status_name(sze_recovery::SequenceStatus status) {
    switch (status) {
    case sze_recovery::kSequenceFirst: return "first";
    case sze_recovery::kSequenceAccepted: return "accepted";
    case sze_recovery::kSequenceDuplicate: return "duplicate";
    case sze_recovery::kSequenceGap: return "gap";
    case sze_recovery::kSequenceRegression: return "regression";
    case sze_recovery::kSequenceAlreadyInvalid: return "already-invalid";
    }
    return "unknown";
}

bool valid_same_boot_clock(std::uint64_t receive_mono_ns,
                           std::uint64_t reference_mono_ns,
                           std::uint64_t reference_realtime_ns,
                           std::uint32_t trading_day,
                           std::string* reason) {
    if (receive_mono_ns == 0U || reference_mono_ns == 0U ||
        reference_realtime_ns == 0U) {
        if (reason != 0) *reason = "same-boot clock anchor is incomplete";
        return false;
    }
    std::uint64_t estimated_realtime_ns = reference_realtime_ns;
    if (receive_mono_ns >= reference_mono_ns) {
        const std::uint64_t delta = receive_mono_ns - reference_mono_ns;
        if (delta > std::numeric_limits<std::uint64_t>::max() -
                        reference_realtime_ns) {
            if (reason != 0) *reason = "same-boot realtime addition overflow";
            return false;
        }
        estimated_realtime_ns += delta;
    } else {
        const std::uint64_t delta = reference_mono_ns - receive_mono_ns;
        if (delta > reference_realtime_ns) {
            if (reason != 0) *reason = "same-boot realtime subtraction underflow";
            return false;
        }
        estimated_realtime_ns -= delta;
    }
    const std::uint64_t china_offset_ns = 8ULL * 3600ULL * 1000000000ULL;
    if (estimated_realtime_ns >
        std::numeric_limits<std::uint64_t>::max() - china_offset_ns) {
        if (reason != 0) *reason = "same-boot China time addition overflow";
        return false;
    }
    const std::uint64_t china_seconds =
        (estimated_realtime_ns + china_offset_ns) / 1000000000ULL;
    if (china_seconds > static_cast<std::uint64_t>(
            std::numeric_limits<std::time_t>::max())) {
        if (reason != 0) *reason = "same-boot realtime is outside calendar range";
        return false;
    }
    const std::time_t calendar_seconds =
        static_cast<std::time_t>(china_seconds);
    struct tm calendar;
    std::memset(&calendar, 0, sizeof(calendar));
    if (::gmtime_r(&calendar_seconds, &calendar) == 0) {
        if (reason != 0) *reason = "same-boot realtime calendar conversion failed";
        return false;
    }
    const std::uint32_t estimated_day =
        static_cast<std::uint32_t>(calendar.tm_year + 1900) * 10000U +
        static_cast<std::uint32_t>(calendar.tm_mon + 1) * 100U +
        static_cast<std::uint32_t>(calendar.tm_mday);
    if (estimated_day != trading_day) {
        if (reason != 0) *reason = "same-boot realtime trading day mismatch";
        return false;
    }
    return true;
}

}  // namespace

RecoveryTimeContext::RecoveryTimeContext()
    : basis(RecoveryTimeBasis::kAnalysisExchangeTime),
      reference_mono_ns(0), reference_realtime_ns(0) {}

RecoveryTimeContext RecoveryTimeContext::same_boot(
    std::uint64_t reference_mono_ns,
    std::uint64_t reference_realtime_ns) {
    RecoveryTimeContext result;
    result.basis = RecoveryTimeBasis::kSameBootMonotonic;
    result.reference_mono_ns = reference_mono_ns;
    result.reference_realtime_ns = reference_realtime_ns;
    return result;
}

RecoveryTimeContext RecoveryTimeContext::analysis_exchange_time() {
    return RecoveryTimeContext();
}

ProcessedSample::ProcessedSample()
    : sample(),
      prediction(std::numeric_limits<float>::quiet_NaN()),
      prediction_valid(false),
      ingress_sequence(0),
      channel_id(0),
      record_offset(0),
      record_size(0),
      recovery_event_id(0),
      recovery_feed_sequence(0),
      recovery_channel_sequence(0),
      recovery_source_id(0) {}

ProcessorStats::ProcessorStats()
    : datagrams(0), records(0), orders(0), executions(0), heartbeats(0),
      known_non_target(0), duplicates(0), samples(0) {}

struct SzeStreamProcessor::Impl {
    struct Instrument {
        explicit Instrument(const mix153060::StaticInputs& value)
            : inputs(value), runtime(value), model_state() {}

        mix153060::StaticInputs inputs;
        mix153060::Runtime runtime;
        mix153060::State model_state;
    };

    std::vector<Instrument> instruments;
    std::unordered_map<std::string, std::size_t> instrument_index;
    std::vector<sze_recovery::FeedSequenceTracker> trackers;
    const mix153060::Model* model;
    SampleCallback callback;
    ProcessorStats stats;
    bool available;
    std::string error;
    InputMode input_mode;
    std::uint16_t expected_source_id;
    std::uint64_t last_recovery_event_id;
    bool recovery_time_locked;
    RecoveryTimeContext recovery_time_context;
    std::uint64_t last_recovery_receive_mono_ns;

    Impl(const std::vector<mix153060::StaticInputs>& inputs,
         const mix153060::Model* supplied_model,
         std::size_t channel_count,
         const SampleCallback& supplied_callback,
         std::uint16_t supplied_source_id)
        : instruments(), instrument_index(), trackers(channel_count),
          model(supplied_model), callback(supplied_callback), stats(),
          available(true), error(), input_mode(kInputUnset),
          expected_source_id(supplied_source_id), last_recovery_event_id(0),
          recovery_time_locked(false), recovery_time_context(),
          last_recovery_receive_mono_ns(0) {
        if (inputs.empty()) {
            throw std::runtime_error("SZE stream processor requires instruments");
        }
        if (channel_count == 0U) {
            throw std::runtime_error("SZE stream processor requires channels");
        }
        if (model != 0 && !model->loaded()) {
            throw std::runtime_error("SZE stream processor model is not loaded");
        }
        instruments.reserve(inputs.size());
        for (std::size_t i = 0; i < inputs.size(); ++i) {
            if (!inputs[i].valid() ||
                !std::isfinite(inputs[i].free_share) || inputs[i].free_share <= 0.0 ||
                !std::isfinite(inputs[i].history_volatility_20d) || inputs[i].history_volatility_20d < 0.0 ||
                !valid_shenzhen_symbol(inputs[i].instrument) ||
                inputs[i].trading_date != inputs[0].trading_date) {
                throw std::runtime_error("invalid SZE stream processor static inputs");
            }
            const std::string key = normalize_symbol(inputs[i].instrument);
            if (key.empty() || instrument_index.find(key) != instrument_index.end()) {
                throw std::runtime_error("duplicate or empty SZE processor instrument");
            }
            instruments.push_back(Instrument(inputs[i]));
            instrument_index[key] = i;
        }
        for (std::size_t i = 0; i < trackers.size(); ++i) {
            trackers[i].reset(static_cast<std::uint32_t>(inputs[0].trading_date));
        }
    }

    void invalidate(const std::string& reason) {
        if (available) {
            available = false;
            error = reason;
        }
        throw std::runtime_error(error.empty() ? reason : error);
    }

    void callback_failed(const std::string& reason) {
        if (available) {
            available = false;
            error = reason;
        }
    }

    void latch_mode(InputMode requested) {
        if (input_mode == kInputUnset) {
            input_mode = requested;
        } else if (input_mode != requested) {
            invalidate("SZE stream processor cannot mix live and recovery input");
        }
    }

    std::size_t find_instrument(const char* symbol) const {
        const std::string key = normalize_symbol(symbol);
        const std::unordered_map<std::string, std::size_t>::const_iterator it =
            instrument_index.find(key);
        return it == instrument_index.end() ? instruments.size() : it->second;
    }

    void emit(const mix153060::Sample& sample,
              const OutputProvenance& provenance,
              Instrument* instrument) {
        ProcessedSample output;
        output.sample = sample;
        output.ingress_sequence = provenance.ingress_sequence;
        output.channel_id = provenance.channel_id;
        output.record_offset = provenance.record_offset;
        output.record_size = provenance.record_size;
        output.recovery_event_id = provenance.recovery_event_id;
        output.recovery_feed_sequence = provenance.recovery_feed_sequence;
        output.recovery_channel_sequence = provenance.recovery_channel_sequence;
        output.recovery_source_id = provenance.recovery_source_id;
        if (model != 0) {
            float prediction = std::numeric_limits<float>::quiet_NaN();
            if (!model->predict(sample.factors, &instrument->model_state, &prediction)) {
                invalidate("SZE stream processor model rejected sample");
            }
            output.prediction = prediction;
            output.prediction_valid = true;
        }
        ++stats.samples;
        if (callback) {
            try {
                callback(output);
            } catch (const std::exception& exception) {
                callback_failed("SZE stream sample callback failed: " +
                                std::string(exception.what()));
                throw;
            } catch (...) {
                callback_failed("SZE stream sample callback failed");
                throw;
            }
        }
    }

    void process_order(const LFL2OrderField& source,
                       std::int64_t receive_time,
                       const OutputProvenance& provenance) {
        const std::size_t index = find_instrument(source.InstrumentID);
        if (index == instruments.size()) {
            return;
        }
        Instrument& instrument = instruments[index];
        mix153060::OrderEvent event;
        std::string reason;
        if (!mix153060::normalize_order_event(
            source, instrument.inputs.trading_date,
                receive_time,
                &event, &reason)) {
            invalidate("SZE order normalization failed: " + reason);
        }
        mix153060::SampleBuffer samples;
        instrument.runtime.on_order(event, &samples);
        if (!instrument.runtime.available()) {
            invalidate("SZE order book rejected event: " +
                       instrument.runtime.failure_reason());
        }
        for (std::size_t i = 0; i < samples.count; ++i) {
            emit(samples.values[i], provenance, &instrument);
        }
    }

    void process_trade(const LFL2TradeField& source,
                       std::int64_t receive_time,
                       const OutputProvenance& provenance) {
        const std::size_t index = find_instrument(source.InstrumentID);
        if (index == instruments.size()) {
            return;
        }
        Instrument& instrument = instruments[index];
        mix153060::TradeEvent event;
        std::string reason;
        if (!mix153060::normalize_trade_event(
            source, instrument.inputs.trading_date,
                receive_time,
                &event, &reason)) {
            invalidate("SZE trade normalization failed: " + reason);
        }
        mix153060::SampleBuffer samples;
        instrument.runtime.on_trade(event, &samples);
        if (!instrument.runtime.available()) {
            invalidate("SZE trade book rejected event: " +
                       instrument.runtime.failure_reason());
        }
        for (std::size_t i = 0; i < samples.count; ++i) {
            emit(samples.values[i], provenance, &instrument);
        }
    }

    void process_datagram(const deepwin_market_data::StreamEvent& stream_event) {
        latch_mode(kInputLive);
        if (stream_event.channel_id >= trackers.size()) {
            invalidate("SZE stream event channel is out of range");
        }
        if (stream_event.data == 0 || stream_event.size == 0U) {
            invalidate("SZE stream datagram is empty");
        }
        ++stats.datagrams;
        std::size_t offset = 0U;
        while (offset < stream_event.size) {
            const std::size_t remaining = stream_event.size - offset;
            if (remaining < 9U) {
                std::ostringstream message;
                message << "SZE stream record truncated at offset " << offset;
                invalidate(message.str());
            }
            const unsigned char* record = stream_event.data + offset;
            const std::uint8_t message_type = record[8];
            const std::size_t record_size = sze_md::wire_record_size(message_type);
            if (record_size == 0U) {
                std::ostringstream message;
                message << "SZE stream unknown message type "
                        << static_cast<unsigned>(message_type)
                        << " at offset " << offset;
                invalidate(message.str());
            }
            if (record_size > remaining) {
                std::ostringstream message;
                message << "SZE stream truncated message type "
                        << static_cast<unsigned>(message_type)
                        << " at offset " << offset;
                invalidate(message.str());
            }

            std::uint32_t raw_sequence = 0U;
            std::memcpy(&raw_sequence, record, sizeof(raw_sequence));
            const sze_recovery::SequenceResult sequence =
                trackers[stream_event.channel_id].observe(raw_sequence);
            if (sequence.status == sze_recovery::kSequenceGap ||
                sequence.status == sze_recovery::kSequenceRegression ||
                sequence.status == sze_recovery::kSequenceAlreadyInvalid) {
                std::ostringstream message;
                message << "SZE stream sequence "
                        << sequence_status_name(sequence.status)
                        << " channel=" << stream_event.channel_id
                        << " raw=" << raw_sequence
                        << " expected=" << sequence.expected;
                invalidate(message.str());
            }
            const bool duplicate = sequence.status == sze_recovery::kSequenceDuplicate;
            if (duplicate) {
                ++stats.duplicates;
            }

            LFL2OrderField order;
            LFL2TradeField trade;
            std::memset(&order, 0, sizeof(order));
            std::memset(&trade, 0, sizeof(trade));
            sze_md::DecodeFailureReason failure =
                sze_md::DecodeFailureReason::kNone;
            const sze_md::DecodeStatus status = sze_md::decode_record(
                record, record_size, &order, &trade, &failure);
            ++stats.records;
            if (status == sze_md::DecodeStatus::kHeartbeat) {
                ++stats.heartbeats;
            } else if (status == sze_md::DecodeStatus::kKnownNonTarget) {
                ++stats.known_non_target;
            } else if (status == sze_md::DecodeStatus::kOrder) {
                if (!duplicate) {
                    ++stats.orders;
                    OutputProvenance provenance;
                    provenance.ingress_sequence = stream_event.sequence;
                    provenance.channel_id = stream_event.channel_id;
                    provenance.record_offset = offset;
                    provenance.record_size = record_size;
                    process_order(order,
                                  static_cast<std::int64_t>(stream_event.realtime_ns),
                                  provenance);
                }
            } else if (status == sze_md::DecodeStatus::kExecution) {
                if (!duplicate) {
                    ++stats.executions;
                    OutputProvenance provenance;
                    provenance.ingress_sequence = stream_event.sequence;
                    provenance.channel_id = stream_event.channel_id;
                    provenance.record_offset = offset;
                    provenance.record_size = record_size;
                    process_trade(trade,
                                  static_cast<std::int64_t>(stream_event.realtime_ns),
                                  provenance);
                }
            } else {
                std::ostringstream message;
                message << "SZE stream decode failed at offset " << offset
                        << ": " << sze_md::decode_failure_reason_name(failure);
                invalidate(message.str());
            }
            offset += record_size;
        }
    }

    bool validate_recovery_event(const sze_recovery::CanonicalEvent& event,
                                 const void* payload,
                                 std::size_t payload_size,
                                 std::string* reason) const {
        if (reason != 0) {
            reason->clear();
        }
        if (event.event_id == 0U) {
            if (reason != 0) *reason = "recovery event id is zero";
            return false;
        }
        if (payload == 0 || payload_size == 0U ||
            event.payload_size != payload_size) {
            if (reason != 0) *reason = "recovery payload size is invalid";
            return false;
        }
        if (event.source_id != expected_source_id) {
            if (reason != 0) *reason = "recovery source id mismatch";
            return false;
        }
        if (event.trading_day !=
            static_cast<std::uint32_t>(instruments[0].inputs.trading_date)) {
            if (reason != 0) *reason = "recovery trading day mismatch";
            return false;
        }
        if (event.record_kind != sze_recovery::kRecordMarketData) {
            if (reason != 0) *reason = "recovery record kind is not market data";
            return false;
        }
        LFL2OrderField order;
        LFL2TradeField trade;
        std::memset(&order, 0, sizeof(order));
        std::memset(&trade, 0, sizeof(trade));
        sze_md::DecodeFailureReason failure =
            sze_md::DecodeFailureReason::kNone;
        const sze_md::DecodeStatus status = sze_md::decode_recovery_record(
            event, payload, payload_size, &order, &trade, &failure);
        if (status != sze_md::DecodeStatus::kOrder &&
            status != sze_md::DecodeStatus::kExecution) {
            if (reason != 0) {
                *reason = std::string("recovery decode failed: ") +
                    sze_md::decode_failure_reason_name(failure);
            }
            return false;
        }
        return true;
    }

    bool validate_recovery_time(const sze_recovery::CanonicalEvent& event,
                                const RecoveryTimeContext& time_context,
                                std::string* reason) const {
        if (reason != 0) {
            reason->clear();
        }
        if (time_context.basis == RecoveryTimeBasis::kAnalysisExchangeTime) {
            return true;
        }
        if (time_context.basis != RecoveryTimeBasis::kSameBootMonotonic) {
            if (reason != 0) *reason = "recovery time basis is invalid";
            return false;
        }
        return valid_same_boot_clock(
            event.receive_mono_ns, time_context.reference_mono_ns,
            time_context.reference_realtime_ns,
            static_cast<std::uint32_t>(instruments[0].inputs.trading_date),
            reason);
    }

    void process_recovery(const sze_recovery::CanonicalEvent& event,
                          const void* payload,
                          std::size_t payload_size,
                          const RecoveryTimeContext& time_context) {
        latch_mode(kInputRecovery);
        std::string reason;
        if (!validate_recovery_event(event, payload, payload_size, &reason)) {
            invalidate("SZE recovery validation failed: " + reason);
        }
        if (!recovery_time_locked) {
            recovery_time_context = time_context;
            recovery_time_locked = true;
        } else if (time_context.basis != recovery_time_context.basis ||
                   time_context.reference_mono_ns !=
                       recovery_time_context.reference_mono_ns ||
                   time_context.reference_realtime_ns !=
                       recovery_time_context.reference_realtime_ns) {
            invalidate("SZE recovery time context changed after first record");
        }
        if (last_recovery_event_id != 0U &&
            event.event_id != last_recovery_event_id + 1U) {
            std::ostringstream message;
            message << "SZE recovery event id gap expected="
                    << (last_recovery_event_id + 1U)
                    << " actual=" << event.event_id;
            invalidate(message.str());
        }
        if (time_context.basis == RecoveryTimeBasis::kSameBootMonotonic &&
            (time_context.reference_mono_ns == 0U ||
             time_context.reference_realtime_ns == 0U ||
             event.receive_mono_ns == 0U)) {
            invalidate("SZE recovery same-boot clock anchor is incomplete");
        }
        if (time_context.basis == RecoveryTimeBasis::kSameBootMonotonic &&
            last_recovery_receive_mono_ns != 0U &&
            event.receive_mono_ns < last_recovery_receive_mono_ns) {
            invalidate("SZE recovery receive monotonic time regressed");
        }

        LFL2OrderField order;
        LFL2TradeField trade;
        std::memset(&order, 0, sizeof(order));
        std::memset(&trade, 0, sizeof(trade));
        sze_md::DecodeFailureReason failure =
            sze_md::DecodeFailureReason::kNone;
        const sze_md::DecodeStatus status = sze_md::decode_recovery_record(
            event, payload, payload_size, &order, &trade, &failure);
        if (status != sze_md::DecodeStatus::kOrder &&
            status != sze_md::DecodeStatus::kExecution) {
            invalidate(std::string("SZE recovery decode failed: ") +
                       sze_md::decode_failure_reason_name(failure));
        }

        const char* source_time = status == sze_md::DecodeStatus::kOrder
            ? order.OrderTime : trade.TradeTime;
        std::int64_t exchange_time_us = 0;
        if (!mix153060::parse_exchange_time_us(
                source_time, instruments[0].inputs.trading_date,
                &exchange_time_us)) {
            invalidate("SZE recovery exchange time normalization failed");
        }
        std::int64_t receive_time = exchange_time_us;
        if (time_context.basis == RecoveryTimeBasis::kSameBootMonotonic) {
            if (!valid_same_boot_clock(
                    event.receive_mono_ns, time_context.reference_mono_ns,
                    time_context.reference_realtime_ns,
                    static_cast<std::uint32_t>(
                        instruments[0].inputs.trading_date), &reason)) {
                invalidate("SZE recovery clock validation failed: " + reason);
            }
            receive_time = mix153060::recover_monotonic_receive_time_us(
                event.receive_mono_ns, time_context.reference_mono_ns,
                time_context.reference_realtime_ns,
                instruments[0].inputs.trading_date, exchange_time_us);
        } else if (time_context.basis !=
                   RecoveryTimeBasis::kAnalysisExchangeTime) {
            invalidate("SZE recovery time basis is invalid");
        }
        if (time_context.basis == RecoveryTimeBasis::kSameBootMonotonic) {
            last_recovery_receive_mono_ns = event.receive_mono_ns;
        }

        OutputProvenance provenance;
        provenance.ingress_sequence = event.event_id;
        provenance.channel_id = event.channel_number;
        provenance.record_size = payload_size;
        provenance.recovery_event_id = event.event_id;
        provenance.recovery_feed_sequence = event.feed_sequence;
        provenance.recovery_channel_sequence = event.channel_sequence;
        provenance.recovery_source_id = event.source_id;
        ++stats.records;
        if (status == sze_md::DecodeStatus::kOrder) {
            ++stats.orders;
            process_order(order, receive_time, provenance);
        } else {
            ++stats.executions;
            process_trade(trade, receive_time, provenance);
        }
        last_recovery_event_id = event.event_id;
    }
};

SzeStreamProcessor::SzeStreamProcessor(
    const std::vector<mix153060::StaticInputs>& inputs,
    const mix153060::Model* model,
    std::size_t channel_count,
    const SampleCallback& callback,
    std::uint16_t expected_source_id)
    : impl_(new Impl(inputs, model, channel_count, callback,
                     expected_source_id)) {}

SzeStreamProcessor::~SzeStreamProcessor() {}

void SzeStreamProcessor::on_event(
    const deepwin_market_data::StreamEvent& event) {
    if (!impl_->available) {
        throw std::runtime_error(impl_->error);
    }
    if (event.kind == deepwin_market_data::kIdleEvent) {
        if (event.data != 0 || event.size != 0U) {
            impl_->invalidate("SZE idle event has a payload");
        }
        return;
    }
    if (event.kind != deepwin_market_data::kDatagramEvent) {
        impl_->invalidate("SZE stream event kind is invalid");
    }
    impl_->process_datagram(event);
}

void SzeStreamProcessor::on_recovery(
    const sze_recovery::CanonicalEvent& event,
    const void* payload,
    std::size_t payload_size,
    const RecoveryTimeContext& time_context) {
    if (!impl_->available) {
        throw std::runtime_error(impl_->error);
    }
    impl_->process_recovery(event, payload, payload_size, time_context);
}

bool SzeStreamProcessor::validate_recovery_event(
    const sze_recovery::CanonicalEvent& event,
    const void* payload,
    std::size_t payload_size,
    std::string* error) const {
    return impl_->validate_recovery_event(event, payload, payload_size, error);
}

bool SzeStreamProcessor::validate_recovery_time(
    const sze_recovery::CanonicalEvent& event,
    const RecoveryTimeContext& time_context,
    std::string* error) const {
    return impl_->validate_recovery_time(event, time_context, error);
}

void SzeStreamProcessor::set_callback(const SampleCallback& callback) {
    impl_->callback = callback;
}

bool SzeStreamProcessor::available() const {
    return impl_->available;
}

const std::string& SzeStreamProcessor::error() const {
    return impl_->error;
}

const ProcessorStats& SzeStreamProcessor::stats() const {
    return impl_->stats;
}

}  // namespace sze_stream
