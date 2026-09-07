#include "sze/runtime/sze_recovery_driver.h"

#include "sze/market_data/SZERecoverable.h"

#include <sstream>
#include <stdexcept>
#include <chrono>
#include <thread>
#include <vector>

namespace sze_stream {
namespace {

void set_error(bool* available, std::string* destination,
               const std::string& value) {
    if (*available) {
        *available = false;
        *destination = value;
    }
}

bool open_reader(const SzeRecoveryJournalConfig& config,
                 sze_recovery::JournalReader* reader,
                 sze_recovery::JournalOpenResult* opened,
                 std::uint64_t* generation,
                 bool reject_unclean,
                 bool* available,
                 std::string* error) {
    sze_recovery::JournalConfig journal;
    journal.directory = config.directory;
    journal.prefix = config.prefix;
    journal.trading_day = config.trading_day;
    journal.source_id = config.source_id;
    journal.segment_bytes = config.segment_bytes;
    journal.max_payload_bytes = config.max_payload_bytes;
    journal.generation = config.expected_generation;
    *opened = reader->open(journal);
    if (opened->status != sze_recovery::kJournalOk) {
        std::ostringstream message;
        message << "SZE recovery journal open failed status="
                << static_cast<int>(opened->status);
        set_error(available, error, message.str());
        return false;
    }
    if (!opened->existing || (reject_unclean && opened->unclean_restart) ||
        opened->corrupt_tail ||
        opened->continuity_state == sze_recovery::kContinuityInvalid) {
        set_error(available, error,
                  "SZE recovery journal is not a clean valid recording");
        reader->close();
        return false;
    }
    *generation = reader->generation();
    if (config.expected_generation != 0U &&
        *generation != config.expected_generation) {
        set_error(available, error, "SZE recovery journal generation mismatch");
        reader->close();
        return false;
    }
    return true;
}

bool read_one(sze_recovery::JournalReader* reader,
              sze_recovery::CanonicalEvent* event,
              std::vector<unsigned char>* payload,
              std::uint64_t generation,
              std::uint64_t* previous_event_id,
              std::uint64_t* previous_receive_mono_ns,
              SzeStreamProcessor* processor,
              bool invoke_processor,
              const std::function<void(const sze_recovery::CanonicalEvent&)>& observer,
              const RecoveryTimeContext& time_context,
              std::uint64_t* payload_bytes,
              bool* available,
              std::string* error) {
    const sze_recovery::JournalStatus status = reader->next(
        event, payload->data(), payload->size());
    if (status == sze_recovery::kJournalEnd) {
        if (reader->generation() != generation) {
            set_error(available, error,
                      "SZE recovery journal generation changed at end");
        }
        return false;
    }
    if (status != sze_recovery::kJournalOk) {
        std::ostringstream message;
        message << "SZE recovery journal read failed status="
                << static_cast<int>(status);
        set_error(available, error, message.str());
        return false;
    }
    if (reader->generation() != generation) {
        set_error(available, error,
                  "SZE recovery journal generation changed between segments");
        return false;
    }
    if (*previous_event_id != 0U &&
        event->event_id != *previous_event_id + 1U) {
        std::ostringstream message;
        message << "SZE recovery journal event id gap expected="
                << (*previous_event_id + 1U)
                << " actual=" << event->event_id;
        set_error(available, error, message.str());
        return false;
    }
    std::string validation_error;
    if (!processor->validate_recovery_event(
            *event, payload->data(), event->payload_size,
            &validation_error)) {
        set_error(available, error,
                  "SZE recovery preflight rejected record: " +
                  validation_error);
        return false;
    }
    if (!processor->validate_recovery_time(
            *event, time_context, &validation_error)) {
        set_error(available, error,
                  "SZE recovery time preflight rejected record: " +
                  validation_error);
        return false;
    }
    if (time_context.basis == RecoveryTimeBasis::kSameBootMonotonic &&
        *previous_receive_mono_ns != 0U &&
        event->receive_mono_ns < *previous_receive_mono_ns) {
        set_error(available, error,
                  "SZE recovery receive monotonic time regressed");
        return false;
    }
    *previous_event_id = event->event_id;
    if (time_context.basis == RecoveryTimeBasis::kSameBootMonotonic) {
        *previous_receive_mono_ns = event->receive_mono_ns;
    }
    *payload_bytes += event->payload_size;
    if (invoke_processor) {
        try {
            if (observer) observer(*event);
            processor->on_recovery(*event, payload->data(), event->payload_size,
                                   time_context);
        } catch (const std::exception& exception) {
            set_error(available, error,
                      std::string("SZE recovery processing failed: ") +
                      exception.what());
            return false;
        } catch (...) {
            set_error(available, error, "SZE recovery processing failed");
            return false;
        }
    }
    return true;
}

bool preflight_active_journal(const SzeRecoveryJournalConfig& config,
                              SzeStreamProcessor* processor,
                              const RecoveryTimeContext& time_context,
                              const std::atomic<bool>& stop_requested,
                              bool* available,
                              std::string* error) {
    sze_recovery::JournalReader reader;
    sze_recovery::JournalOpenResult opened;
    std::uint64_t generation = 0U;
    if (!open_reader(config, &reader, &opened, &generation, false,
                     available, error)) {
        return false;
    }
    std::vector<unsigned char> payload(config.max_payload_bytes);
    sze_recovery::CanonicalEvent event;
    std::uint64_t previous_event_id = 0U;
    std::uint64_t previous_receive_mono_ns = 0U;
    for (;;) {
        if (stop_requested.load()) {
            set_error(available, error, "SZE recovery stopped during preflight");
            return false;
        }
        const sze_recovery::JournalStatus status = reader.next(
            &event, payload.data(), payload.size());
        if (status == sze_recovery::kJournalEnd ||
            status == sze_recovery::kJournalWouldBlock) {
            break;
        }
        if (status != sze_recovery::kJournalOk) {
            std::ostringstream message;
            message << "SZE handoff journal preflight failed status="
                    << static_cast<int>(status);
            set_error(available, error, message.str());
            reader.close();
            return false;
        }
        if (reader.generation() != generation) {
            set_error(available, error,
                      "SZE handoff journal generation changed between segments");
            reader.close();
            return false;
        }
        if (previous_event_id != 0U &&
            event.event_id != previous_event_id + 1U) {
            set_error(available, error,
                      "SZE handoff journal event id gap");
            reader.close();
            return false;
        }
        std::string validation_error;
        if (!processor->validate_recovery_event(
                event, payload.data(), event.payload_size, &validation_error)) {
            set_error(available, error,
                      "SZE handoff preflight rejected record: " +
                      validation_error);
            reader.close();
            return false;
        }
        if (!processor->validate_recovery_time(
                event, time_context, &validation_error)) {
            set_error(available, error,
                      "SZE handoff time preflight rejected record: " +
                      validation_error);
            reader.close();
            return false;
        }
        if (time_context.basis == RecoveryTimeBasis::kSameBootMonotonic &&
            previous_receive_mono_ns != 0U &&
            event.receive_mono_ns < previous_receive_mono_ns) {
            set_error(available, error,
                      "SZE handoff receive monotonic time regressed");
            reader.close();
            return false;
        }
        previous_event_id = event.event_id;
        if (time_context.basis == RecoveryTimeBasis::kSameBootMonotonic) {
            previous_receive_mono_ns = event.receive_mono_ns;
        }
    }
    const bool generation_ok = reader.generation() == generation;
    reader.close();
    if (!generation_ok) {
        set_error(available, error,
                  "SZE handoff journal generation changed at preflight end");
        return false;
    }
    return *available;
}

}  // namespace

SzeRecoveryJournalConfig::SzeRecoveryJournalConfig()
    : directory(), prefix("sze"), trading_day(0U), source_id(88U),
      segment_bytes(1ULL << 30U), max_payload_bytes(256U),
      expected_generation(0U), shm_path(), handoff_timeout_ms(1000U) {}

SzeRecoveryDriverStats::SzeRecoveryDriverStats()
    : records(0U), payload_bytes(0U), handoff_events(0U) {}

SzeRecoveryDriver::SzeRecoveryDriver(SzeStreamProcessor* processor)
    : processor_(processor), stats_(), available_(true), error_(),
      handoff_consumer_(), handoff_payload_(), handoff_time_context_(),
      handoff_generation_(0U), handoff_first_event_id_(0U),
      handoff_previous_event_id_(0U),
      handoff_trading_day_(0U), handoff_source_id_(0U),
      handoff_active_(false), live_ready_(false), stop_requested_(false) {
    if (processor_ == 0) {
        available_ = false;
        error_ = "SZE recovery driver requires a processor";
    }
}

SzeRecoveryDriver::~SzeRecoveryDriver() {
    if (handoff_consumer_) {
        handoff_consumer_->close();
    }
}

bool SzeRecoveryDriver::replay(const SzeRecoveryJournalConfig& config,
                               const RecoveryTimeContext& time_context) {
    if (!available_ || processor_ == 0) {
        return false;
    }
    if (config.directory.empty() || config.trading_day < 20000101U ||
        config.trading_day > 99991231U || config.source_id == 0U ||
        config.max_payload_bytes == 0U) {
        set_error(&available_, &error_, "invalid SZE recovery journal config");
        return false;
    }

    stats_ = SzeRecoveryDriverStats();
    sze_recovery::JournalReader reader;
    sze_recovery::JournalOpenResult opened;
    std::uint64_t generation = 0U;
    if (!open_reader(config, &reader, &opened, &generation, true,
                     &available_, &error_)) {
        return false;
    }
    std::vector<unsigned char> payload(config.max_payload_bytes);
    sze_recovery::CanonicalEvent event;
    std::uint64_t previous_event_id = 0U;
    std::uint64_t previous_receive_mono_ns = 0U;
    std::uint64_t preflight_records = 0U;
    std::uint64_t preflight_bytes = 0U;
    for (;;) {
        if (stop_requested_.load()) { set_error(&available_, &error_, "SZE recovery stopped"); break; }
        if (!read_one(&reader, &event, &payload, generation,
                      &previous_event_id, &previous_receive_mono_ns,
                      processor_, false, event_observer_, time_context,
                      &preflight_bytes, &available_, &error_)) {
            break;
        }
        ++preflight_records;
    }
    reader.close();
    if (!available_) {
        return false;
    }

    const std::uint64_t preflight_generation = generation;
    if (!open_reader(config, &reader, &opened, &generation, true,
                     &available_, &error_)) {
        return false;
    }
    if (generation != preflight_generation) {
        set_error(&available_, &error_,
                  "SZE recovery journal generation changed between passes");
        reader.close();
        return false;
    }
    previous_event_id = 0U;
    previous_receive_mono_ns = 0U;
    std::uint64_t processed_bytes = 0U;
    std::uint64_t processed_records = 0U;
    for (;;) {
        if (stop_requested_.load()) { set_error(&available_, &error_, "SZE recovery stopped"); break; }
        if (!read_one(&reader, &event, &payload, generation,
                      &previous_event_id, &previous_receive_mono_ns,
                      processor_, true, event_observer_, time_context,
                      &processed_bytes, &available_, &error_)) {
            break;
        }
        ++processed_records;
    }
    reader.close();
    if (!available_) {
        return false;
    }
    if (processed_records != preflight_records ||
        processed_bytes != preflight_bytes) {
        set_error(&available_, &error_,
                  "SZE recovery journal changed between preflight and replay");
        return false;
    }
    stats_.records = processed_records;
    stats_.payload_bytes = processed_bytes;
    return true;
}

bool SzeRecoveryDriver::replay_handoff(
    const SzeRecoveryJournalConfig& config,
    const RecoveryTimeContext& time_context) {
    if (!available_ || processor_ == 0 || handoff_active_ || live_ready_) {
        return false;
    }
    if (config.directory.empty() || config.shm_path.empty() ||
        config.trading_day < 20000101U || config.trading_day > 99991231U ||
        config.source_id == 0U || config.max_payload_bytes == 0U ||
        config.expected_generation == 0U ||
        time_context.basis != RecoveryTimeBasis::kSameBootMonotonic ||
        time_context.reference_mono_ns == 0U ||
        time_context.reference_realtime_ns == 0U) {
        set_error(&available_, &error_, "invalid SZE handoff configuration");
        return false;
    }
    if (!preflight_active_journal(config, processor_, time_context, stop_requested_,
                                  &available_, &error_)) {
        return false;
    }

    std::unique_ptr<sze_recovery::ReplayHandoffConsumer> consumer(
        new sze_recovery::ReplayHandoffConsumer());
    sze_recovery::JournalConfig journal;
    journal.directory = config.directory;
    journal.prefix = config.prefix;
    journal.trading_day = config.trading_day;
    journal.source_id = config.source_id;
    journal.segment_bytes = config.segment_bytes;
    journal.max_payload_bytes = config.max_payload_bytes;
    journal.generation = config.expected_generation;
    if (!consumer->open(journal, config.shm_path, false)) {
        std::ostringstream message;
        message << "SZE handoff open failed status="
                << static_cast<int>(consumer->last_open_status());
        set_error(&available_, &error_, message.str());
        return false;
    }
    if (consumer->ring_trading_day() != config.trading_day ||
        consumer->ring_source_id() != config.source_id ||
        consumer->generation() == 0U ||
        consumer->ring_generation() != consumer->generation() ||
        (config.expected_generation != 0U &&
         consumer->generation() != config.expected_generation) ||
        !consumer->producer_alive()) {
        set_error(&available_, &error_,
                  "SZE handoff source liveness or epoch validation failed");
        consumer->close();
        return false;
    }

    handoff_consumer_ = std::move(consumer);
    handoff_payload_.assign(config.max_payload_bytes, 0U);
    handoff_time_context_ = time_context;
    handoff_generation_ = handoff_consumer_->generation();
    handoff_first_event_id_ = handoff_consumer_->next_event_id();
    handoff_previous_event_id_ = 0U;
    handoff_trading_day_ = config.trading_day;
    handoff_source_id_ = config.source_id;
    handoff_active_ = true;
    live_ready_ = false;
    const std::uint64_t timeout_ms = config.handoff_timeout_ms;
    const std::chrono::steady_clock::time_point start =
        std::chrono::steady_clock::now();
    for (;;) {
        if (stop_requested_.load()) {
            error_ = "SZE recovery stopped before live cutover";
            return false;
        }
        (void)poll_handoff();
        if (live_ready_) {
            return true;
        }
        if (!available_) {
            return false;
        }
        const std::chrono::steady_clock::time_point now =
            std::chrono::steady_clock::now();
        const std::uint64_t elapsed = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                now - start).count());
        if (elapsed >= timeout_ms) {
            set_error(&available_, &error_,
                      "SZE handoff did not reach a live cutover before timeout");
            return false;
        }
        std::this_thread::yield();
    }
}

bool SzeRecoveryDriver::poll_handoff() {
    if (stop_requested_.load()) {
        live_ready_ = false;
        return false;
    }
    if (!available_ || !handoff_active_ || !handoff_consumer_) {
        return false;
    }
    const auto health_ok = [this]() {
        return handoff_consumer_->generation() == handoff_generation_ &&
            handoff_consumer_->ring_generation() == handoff_generation_ &&
            handoff_consumer_->ring_trading_day() == handoff_trading_day_ &&
            handoff_consumer_->ring_source_id() == handoff_source_id_ &&
            handoff_consumer_->continuity_state() !=
                sze_recovery::kContinuityInvalid &&
            handoff_consumer_->producer_alive();
    };
    if (!health_ok()) {
        set_error(&available_, &error_,
                  "SZE handoff transport health changed before read");
        handoff_active_ = false;
        live_ready_ = false;
        return false;
    }
    sze_recovery::CanonicalEvent event;
    const sze_recovery::ReplayReadStatus status = handoff_consumer_->next(
        &event, handoff_payload_.data(), handoff_payload_.size());
    if (status == sze_recovery::kReplayReadWouldBlock) {
        if (!health_ok()) {
            set_error(&available_, &error_,
                      "SZE handoff transport health changed while waiting");
            handoff_active_ = false;
            live_ready_ = false;
        }
        return false;
    }
    if (status != sze_recovery::kReplayReadEvent) {
        set_error(&available_, &error_,
                  "SZE handoff consumer became invalid");
        handoff_active_ = false;
        live_ready_ = false;
        return false;
    }
    if (handoff_consumer_->generation() != handoff_generation_ ||
        handoff_consumer_->ring_generation() != handoff_generation_ ||
        handoff_consumer_->ring_trading_day() != handoff_trading_day_ ||
        handoff_consumer_->ring_source_id() != handoff_source_id_ ||
        !handoff_consumer_->producer_alive()) {
        set_error(&available_, &error_,
                  "SZE handoff epoch or producer liveness changed");
        handoff_active_ = false;
        live_ready_ = false;
        return false;
    }
    if ((handoff_previous_event_id_ == 0U &&
         event.event_id != handoff_first_event_id_) ||
        (handoff_previous_event_id_ != 0U &&
         event.event_id != handoff_previous_event_id_ + 1U)) {
        set_error(&available_, &error_,
                  "SZE handoff event id is not contiguous");
        handoff_active_ = false;
        live_ready_ = false;
        return false;
    }
    std::string validation_error;
    if (!processor_->validate_recovery_event(
            event, handoff_payload_.data(), event.payload_size,
            &validation_error)) {
        set_error(&available_, &error_,
                  "SZE handoff record validation failed: " + validation_error);
        handoff_active_ = false;
        live_ready_ = false;
        return false;
    }
    try {
        if (event_observer_) event_observer_(event);
        processor_->on_recovery(event, handoff_payload_.data(),
                                event.payload_size, handoff_time_context_);
    } catch (const std::exception& exception) {
        set_error(&available_, &error_,
                  std::string("SZE handoff processing failed: ") +
                  exception.what());
        handoff_active_ = false;
        live_ready_ = false;
        return false;
    } catch (...) {
        set_error(&available_, &error_, "SZE handoff processing failed");
        handoff_active_ = false;
        live_ready_ = false;
        return false;
    }
    handoff_previous_event_id_ = event.event_id;
    ++stats_.records;
    stats_.payload_bytes += event.payload_size;
    if (handoff_consumer_->mode() == sze_recovery::kReplayLive) {
        if (handoff_consumer_->continuity_state() !=
                sze_recovery::kContinuityValid ||
            !handoff_consumer_->producer_alive() ||
            handoff_consumer_->ring_generation() != handoff_generation_) {
            set_error(&available_, &error_,
                      "SZE handoff live readiness proof failed");
            handoff_active_ = false;
            live_ready_ = false;
            return false;
        }
        live_ready_ = true;
        ++stats_.handoff_events;
    }
    return true;
}

bool SzeRecoveryDriver::available() const {
    return available_ && processor_ != 0 && processor_->available();
}

bool SzeRecoveryDriver::live_ready() const {
    return !stop_requested_.load() && live_ready_ && available_ && handoff_active_ &&
        processor_ != 0 && processor_->available() &&
        handoff_consumer_ != 0 &&
        handoff_consumer_->mode() == sze_recovery::kReplayLive &&
        handoff_consumer_->continuity_state() == sze_recovery::kContinuityValid &&
        handoff_consumer_->ring_trading_day() == handoff_trading_day_ &&
        handoff_consumer_->ring_source_id() == handoff_source_id_ &&
        handoff_consumer_->ring_generation() == handoff_generation_ &&
        handoff_consumer_->producer_alive();
}

const std::string& SzeRecoveryDriver::error() const {
    return error_.empty() && processor_ != 0 ? processor_->error() : error_;
}

void SzeRecoveryDriver::request_stop() { stop_requested_.store(true); }

void SzeRecoveryDriver::set_event_observer(
    const std::function<void(const sze_recovery::CanonicalEvent&)>& observer) {
    event_observer_ = observer;
}

const SzeRecoveryDriverStats& SzeRecoveryDriver::stats() const {
    return stats_;
}

}  // namespace sze_stream
