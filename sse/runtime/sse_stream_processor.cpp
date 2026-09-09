#include "sse/runtime/sse_stream_processor.h"

#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace sse_stream {
namespace {

const std::uint64_t kContinuousTradingStartMicros = 34200000000ULL;
const std::uint64_t kDayEndMicros = 86400000000ULL;
const std::uint64_t kHardwareBatchGapNanoseconds = 5000ULL;

std::string error_text(const char* prefix, const std::string& detail) {
    return std::string(prefix) + (detail.empty() ? std::string() : ": " + detail);
}

std::vector<std::string> instrument_ids(const sse_tick::DailyStaticMetadataMap& metadata) {
    std::vector<std::string> ids;
    ids.reserve(metadata.size());
    for (const auto& entry : metadata) {
        std::string id;
        if (!sse_tick::normalize_sse_security_id(entry.first, &id))
            throw std::runtime_error("invalid SSE instrument: " + entry.first);
        ids.push_back(id);
    }
    return ids;
}

}  // namespace

Provenance::Provenance()
    : stream_kind(deepwin_market_data::kDatagramEvent), stream_sequence(0ULL),
      monotonic_ns(0ULL), realtime_ns(0ULL), receive_batch(0ULL), batch_index(0U),
      batch_size(0U), stream_channel_id(0U), wire_channel_no(0U),
      wire_sequence(0ULL), record_offset(0U), source_ipv4(0U), source_port(0U),
      timestamp_flags(0U), batch_id(0ULL),
      batch_emitted_ns(0ULL),
      batch_close_reason(sse_live_sampling::kBatchNotClosed) {}

TickOutput::TickOutput()
    : event(), factors(), bid_levels(), ask_levels(), last_trade_price(0.0),
      total_trade_volume(0.0), total_trade_turnover(0.0), prediction(),
      prediction_valid(false), sample_decision(), provenance() {}

SnapshotOutput::SnapshotOutput()
    : snapshot(), snapshot36(), auction59(), prediction(), prediction_valid(false),
      provenance() {}

BatchEndOutput::BatchEndOutput()
    : batch_id(0ULL), last_hardware_ns(0ULL), emitted_monotonic_ns(0ULL),
      packet_count(0U), candidate_count(0U), prediction_count(0U) {}

Output::Output() : kind(kTickOutput), tick(), snapshot(), batch_end() {}

SseStreamProcessor::InstrumentState::InstrumentState(
    const std::string& code, const sse_tick::DailyStaticMetadata& metadata,
    const sse_auction59::StaticMetadata& auction_metadata, bool ipo_known)
    : book(code), factors(), sample_gate(), model_state(), previous_snapshot(),
      have_previous_snapshot(false), last_snapshot_time(0ULL), pending_tick(),
      pending_provenance(), have_pending_tick(false),
      auction(new sse_auction59::Session(code, auction_metadata, ipo_known)),
      auction_decided(false), snapshot_model_enabled(false),
      auction_failure_logged(false), static_failure_logged(false) {
    factors.set_static_metadata(metadata);
}

SseStreamProcessor::SseStreamProcessor(
    const sse_tick::DailyStaticMetadataMap& metadata,
    const sse_hybrid_model::Model* model,
    bool factors_only,
    const OutputCallback& callback,
    const Auction59Provider& auction59_provider,
    const sse_auction59::StaticMetadataMap& auction_metadata,
    bool auction_enabled)
    : states_(), channel_sequences_(), batch_sampler_(instrument_ids(metadata)),
      closed_batches_(),
      hardware_batch_mode_(false), next_hardware_batch_id_(1ULL),
      hardware_batches_(), model_(model), factors_only_(factors_only),
      callback_(callback), auction59_provider_(auction59_provider),
      auction59_inputs_(), auction_arrival_index_(0ULL),
      invalid_(false), invalid_reason_() {
    if (!callback_) throw std::runtime_error("SSE stream processor callback required");
    if (factors_only_ && model_ != 0)
        throw std::runtime_error("SSE factors-only processor cannot take a model");
    if (!factors_only_ && (model_ == 0 || !model_->loaded()))
        throw std::runtime_error("SSE processor requires a loaded model or factors-only mode");
    std::uint32_t metadata_date = 0U;
    for (sse_tick::DailyStaticMetadataMap::const_iterator it = metadata.begin();
         it != metadata.end(); ++it) {
        const sse_tick::DailyStaticMetadata& row = it->second;
        if (!row.complete() || !std::isfinite(row.prev_turnover) ||
            !std::isfinite(row.avg_amount) || !std::isfinite(row.turnover_threshold) ||
            !std::isfinite(row.free_share) || !std::isfinite(row.pre_close) ||
            !std::isfinite(row.limit_price) || !std::isfinite(row.stop_price))
            throw std::runtime_error("incomplete or non-finite SSE static metadata: " + it->first);
        if (metadata_date == 0U) metadata_date = row.date;
        if (row.date != metadata_date)
            throw std::runtime_error("mixed trading dates in SSE static metadata");
        std::string code;
        if (!sse_tick::normalize_sse_security_id(it->first, &code))
            throw std::runtime_error("invalid SSE static metadata security: " + it->first);
        if (states_.find(code) != states_.end())
            throw std::runtime_error("duplicate SSE static metadata security: " + code);
        sse_auction59::StaticMetadata auction_row;
        auction_row.date = row.date;
        auction_row.pre_close = row.pre_close;
        auction_row.upper_limit = row.limit_price;
        auction_row.lower_limit = row.stop_price;
        auction_row.limits_valid = true;
        auction_row.source = row.source;
        auction_row.quality = row.quality;
        sse_auction59::StaticMetadataMap::const_iterator known = auction_metadata.find(code);
        if (known == auction_metadata.end()) known = auction_metadata.find(it->first);
        const bool ipo_known = known != auction_metadata.end();
        // Daily prices remain authoritative; optional metadata supplies only
        // the IPO facts which cannot be inferred from a level-2 snapshot.
        if (ipo_known) {
            auction_row.listing_date = known->second.listing_date;
            auction_row.is_ipo_first_day = known->second.is_ipo_first_day;
        }
        StateMap::iterator inserted = states_.insert(std::make_pair(
            code, InstrumentState(code, row, auction_row, ipo_known))).first;
        if (!auction_enabled) {
            inserted->second.auction->disable("auction_disabled");
            inserted->second.auction_decided = true;
            report_auction_state(code, inserted->second);
        }
    }
    if (states_.empty()) throw std::runtime_error("SSE processor requires static metadata");
    closed_batches_.reserve(states_.size());
}

void SseStreamProcessor::fail(const std::string& reason) {
    if (!invalid_) {
        invalid_ = true;
        invalid_reason_ = reason.empty() ? "SSE stream processor invalid" : reason;
    }
    throw std::runtime_error(invalid_reason_);
}

SseStreamProcessor::InstrumentState* SseStreamProcessor::state_for(
    const std::string& code) {
    StateMap::iterator it = states_.find(code);
    return it == states_.end() ? 0 : &it->second;
}

bool SseStreamProcessor::instrument_static_valid(const std::string& code) const {
    const StateMap::const_iterator it = states_.find(code);
    return it != states_.end() && it->second.auction->shared_static_valid();
}

Provenance SseStreamProcessor::provenance(
    const deepwin_market_data::StreamEvent& event,
    std::uint32_t wire_channel, std::uint64_t wire_sequence,
    std::size_t record_offset) const {
    Provenance value;
    value.stream_kind = event.kind;
    value.stream_sequence = event.sequence;
    value.monotonic_ns = event.monotonic_ns;
    value.realtime_ns = event.realtime_ns;
    value.receive_batch = event.receive_batch;
    value.batch_index = event.batch_index;
    value.batch_size = event.batch_size;
    value.stream_channel_id = event.channel_id;
    value.wire_channel_no = wire_channel;
    value.wire_sequence = wire_sequence;
    value.record_offset = record_offset;
    value.source_ipv4 = event.source_ipv4;
    value.source_port = event.source_port;
    value.timestamp_flags = event.timestamp_flags;
    return value;
}

bool SseStreamProcessor::valid_tick_sequence(const sse_live::TickEvent& tick) {
    if (tick.channel_no == 0U || tick.tick_index == 0ULL) return false;
    ChannelSequence& sequence = channel_sequences_[tick.channel_no];
    if (!sequence.have_tick) {
        sequence.have_tick = true;
        sequence.last_tick = tick.tick_index;
        return true;
    }
    if (tick.tick_index <= sequence.last_tick) return false;  // duplicate/replay
    if (tick.tick_index - sequence.last_tick != 1ULL) {
        batch_sampler_.mark_sequence_gap();
        fail("SSE tick sequence gap on wire channel");
    }
    sequence.last_tick = tick.tick_index;
    return true;
}

void SseStreamProcessor::on_event(const deepwin_market_data::StreamEvent& event) {
    if (invalid_) throw std::runtime_error(invalid_reason_);
    try {
        if (event.kind == deepwin_market_data::kIdleEvent) {
            on_idle(event);
        } else if (event.kind == deepwin_market_data::kDatagramEvent) {
            if (event.hardware_ns != 0ULL) hardware_batch_mode_ = true;
            if (hardware_batch_mode_) {
                advance_hardware_batch(event);
            } else {
                std::string error;
                if (!batch_sampler_.advance_to_event(event.monotonic_ns, &closed_batches_, &error))
                    fail(error_text("SSE sampler advance failure", error));
                for (const auto& batch : closed_batches_) process_closed_batch(batch);
            }
            on_datagram(event);
        } else {
            fail("unknown SSE stream event kind");
        }
    } catch (const std::runtime_error& exception) {
        if (!invalid_) fail(exception.what());
        throw;
    } catch (const std::exception& exception) {
        fail(exception.what());
    } catch (...) {
        fail("unknown SSE stream processor exception");
    }
}

void SseStreamProcessor::on_idle(const deepwin_market_data::StreamEvent& event) {
    if (event.data != 0 || event.size != 0U)
        fail("SSE idle event carries payload");
    if (hardware_batch_mode_) {
        std::vector<std::uint32_t> channels;
        for (HardwareBatchMap::const_iterator it = hardware_batches_.begin();
             it != hardware_batches_.end(); ++it)
            if (it->second.open) channels.push_back(it->first);
        for (std::vector<std::uint32_t>::const_iterator it = channels.begin();
             it != channels.end(); ++it)
            close_hardware_batch(*it, event.monotonic_ns,
                                 sse_live_sampling::kBatchClosedByTimer);
    } else {
        std::string error;
        if (!batch_sampler_.on_timer(event.monotonic_ns, &closed_batches_, &error))
            fail(error_text("SSE idle sampler failure", error));
        for (const auto& batch : closed_batches_) process_closed_batch(batch);
    }
}

void SseStreamProcessor::advance_hardware_batch(
    const deepwin_market_data::StreamEvent& event) {
    const std::uint64_t timestamp = event.hardware_ns != 0ULL
        ? event.hardware_ns : event.monotonic_ns;
    HardwareBatchState& state = hardware_batches_[event.channel_id];
    if (state.have_hardware_timestamp && timestamp < state.last_hardware_ns)
        fail("SSE hardware receive timestamp moved backwards");
    if (!state.open) {
        state.open = true;
        state.batch_id = next_hardware_batch_id_++;
        state.packet_count = 0U;
        state.candidates.clear();
    } else {
        if (timestamp - state.last_hardware_ns >= kHardwareBatchGapNanoseconds) {
            close_hardware_batch(event.channel_id, event.monotonic_ns,
                                 sse_live_sampling::kBatchClosedByNextEvent);
            state.open = true;
            state.batch_id = next_hardware_batch_id_++;
            state.packet_count = 0U;
            state.candidates.clear();
        }
    }
    state.last_hardware_ns = timestamp;
    state.have_hardware_timestamp = true;
    state.last_hardware_monotonic_ns = event.monotonic_ns;
    ++state.packet_count;
}

void SseStreamProcessor::close_hardware_batch(
    std::uint32_t channel_id, std::uint64_t emitted_monotonic_ns,
    sse_live_sampling::BatchCloseReason reason) {
    HardwareBatchMap::iterator state_it = hardware_batches_.find(channel_id);
    if (state_it == hardware_batches_.end() || !state_it->second.open) return;
    HardwareBatchState& state = state_it->second;
    sse_live_sampling::BatchEnd batch;
    batch.batch_id = state.batch_id;
    batch.last_activity_ns = state.last_hardware_ns;
    batch.emitted_ns = emitted_monotonic_ns;
    batch.packet_count = state.packet_count;
    batch.reason = reason;
    for (std::map<std::string, sse_live_sampling::Candidate>::const_iterator it =
             state.candidates.begin(); it != state.candidates.end(); ++it)
        batch.candidates.push_back(it->second);
    state.open = false;
    state.packet_count = 0U;
    state.candidates.clear();
    process_closed_batch(batch);
}

void SseStreamProcessor::commit_hardware_candidate(
    std::uint32_t channel_id,
    const sse_live_sampling::Candidate& candidate) {
    HardwareBatchMap::iterator state_it = hardware_batches_.find(channel_id);
    if (state_it == hardware_batches_.end() || !state_it->second.open)
        fail("SSE hardware batch state missing");
    state_it->second.candidates[candidate.instrument_id] = candidate;
}

void SseStreamProcessor::on_datagram(
    const deepwin_market_data::StreamEvent& event) {
    if (event.data == 0 || event.size == 0U)
        fail("SSE datagram is empty or has no borrowed payload");
    if (sse_live::is_primary_heartbeat(event.data, event.size)) return;
    std::size_t offset = 0U;
    while (offset < event.size) {
        const std::size_t remaining = event.size - offset;
        sse_live::TickEvent tick;
        std::string tick_error;
        if (remaining >= 72U && sse_live::decode_primary_tick(
                event.data + offset, 72U, &tick, &tick_error, false)) {
            process_tick(tick, event, offset);
            offset += 72U;
            continue;
        }
        sse_live::Snapshot snapshot;
        std::string snapshot_error;
        if (remaining >= 440U && sse_live::decode_primary_snapshot(
                event.data + offset, 440U, &snapshot, &snapshot_error, false)) {
            process_snapshot(snapshot, event, offset);
            offset += 440U;
            continue;
        }
        fail(error_text("unknown or truncated SSE wire record",
                        tick_error.empty() ? snapshot_error : tick_error));
    }
}

void SseStreamProcessor::process_tick(
    const sse_live::TickEvent& tick,
    const deepwin_market_data::StreamEvent& event,
    std::size_t record_offset) {
    if (tick.channel_no == 0U || tick.tick_index == 0ULL)
        fail("SSE tick has invalid wire channel or sequence");
    if (!valid_tick_sequence(tick)) return;
    InstrumentState* state = state_for(tick.security_id);
    if (state == 0) return;
    // Every accepted event reaches the auction accumulator, independently of
    // tick sampling. Quiet batches and pre-open rows must not discard history.
    state->auction->on_tick(tick, event.realtime_ns, ++auction_arrival_index_);
    if (!factors_only_ && !state->auction_decided &&
        tick.time_of_day_micros >= sse_hybrid_model::kSnapshotToTickSwitchMicros) {
        // If no opening snapshot ever arrived, the tick handover is the last
        // useful decision boundary. Release the unused auction history now.
        state->auction->disable("no_opening_snapshot_before_tick_handover");
        state->auction_decided = true;
    }
    report_auction_state(tick.security_id, *state);
    std::string error;

    const sse_tick::ApplyResult applied = state->book.apply(tick);
    if (!applied.accepted && tick.event_type != sse_tick::kStatus)
        fail(std::string("SSE order-book rejected ") + applied.reason);
    if (applied.accepted && applied.book_changed) initialize_window(*state, tick);
    sse_live_sampling::Candidate candidate;
    const sse_live_sampling::Candidate* candidate_ptr = 0;
    if (applied.accepted && applied.book_changed) {
        candidate.instrument_id = tick.security_id;
        candidate.cut_index = tick.tick_index;
        candidate.source_sequence = tick.provider_sequence;
        candidate.exchange_time_of_day_micros = tick.time_of_day_micros;
        candidate.local_receive_ns = event.monotonic_ns;
        candidate_ptr = &candidate;
        state->pending_tick = tick;
        state->pending_provenance = provenance(event, tick.channel_no, tick.tick_index,
                                               record_offset);
        state->have_pending_tick = true;
    }
    if (hardware_batch_mode_) {
        if (candidate_ptr) commit_hardware_candidate(event.channel_id, *candidate_ptr);
    } else if (!batch_sampler_.commit_applied_event(tick.security_id, candidate_ptr,
                                                   applied.sequence_healthy, &error))
        fail(error_text("SSE batch sampler commit failure", error));
}

sse_live_sampling::TickCut SseStreamProcessor::book_cut(
    const InstrumentState& state, const sse_live::TickEvent& tick) const {
    sse_tick::Level bids[1] = {}, asks[1] = {};
    state.book.snapshot(bids, asks, 1);
    sse_live_sampling::TickCut cut;
    cut.cut_index = tick.tick_index;
    cut.exchange_time_of_day_micros = tick.time_of_day_micros;
    cut.valid_book = bids[0].price_raw > 0 && asks[0].price_raw > 0 && bids[0].quantity > 0 && asks[0].quantity > 0;
    cut.mid_price = cut.valid_book ? (static_cast<double>(bids[0].price_raw) + asks[0].price_raw) / 2000.0
                                   : state.book.last_trade_price();
    cut.cumulative_turnover = state.book.total_trade_turnover();
    cut.cumulative_volume = static_cast<std::int64_t>(state.book.total_trade_qty() / 1000ULL);
    cut.continuous_trading = tick.time_of_day_micros >= kContinuousTradingStartMicros && tick.time_of_day_micros < kDayEndMicros;
    return cut;
}

void SseStreamProcessor::initialize_window(InstrumentState& state, const sse_live::TickEvent& tick) {
    if (state.sample_gate.initialized()) return;
    const sse_live_sampling::TickCut cut = book_cut(state, tick);
    sse_live_sampling::SampleDecision decision;
    std::string error;
    if (!state.sample_gate.observe_batch_end(cut, state.factors.turnover_threshold(), &decision, &error))
        fail(error_text("SSE opening window failed", error));
    if (!state.sample_gate.initialized()) return;
    state.factors.seed_window(cut.mid_price, cut.exchange_time_of_day_micros,
                              static_cast<double>(state.book.total_trade_qty()), cut.cumulative_turnover);
    state.book.take_flow_window();
}

void SseStreamProcessor::process_closed_batch(
    const sse_live_sampling::BatchEnd& batch) {
    std::uint32_t prediction_count = 0U;
    for (std::vector<sse_live_sampling::Candidate>::const_iterator it =
             batch.candidates.begin(); it != batch.candidates.end(); ++it) {
        InstrumentState* state = state_for(it->instrument_id);
        if (state == 0) fail("SSE batch candidate security is not configured");
        sse_tick::Level bids[10] = {}, asks[10] = {};
        if (!state->book.snapshot(bids, asks, 10U)) fail("SSE order-book snapshot failed");
        if (!state->have_pending_tick || state->pending_tick.tick_index != it->cut_index)
            fail("SSE candidate/book provenance mismatch");
        const sse_live_sampling::TickCut cut = book_cut(*state, state->pending_tick);
        sse_live_sampling::SampleDecision decision;
        std::string error;
        if (!state->sample_gate.observe_batch_end(
                cut, state->factors.turnover_threshold(), &decision, &error))
            fail(error_text("SSE tick sample gate failure", error));
        if (!state->sample_gate.initialized()) continue;
        if (!decision.accepted) continue;

        const sse_tick::FactorRow row = state->factors.build(
            state->book, cut.exchange_time_of_day_micros);
        Output output;
        output.kind = kTickOutput;
        if (!state->have_pending_tick)
            fail("SSE batch candidate provenance is missing");
        output.tick.event = state->pending_tick;
        output.tick.factors = row;
        output.tick.sample_decision = decision;
        output.tick.bid_levels.assign(bids, bids + 10U);
        output.tick.ask_levels.assign(asks, asks + 10U);
        output.tick.last_trade_price = state->book.last_trade_price();
        output.tick.total_trade_volume = state->book.total_trade_qty() / 1000.0;
        output.tick.total_trade_turnover = state->book.total_trade_turnover();
        output.tick.provenance = state->pending_provenance;
        output.tick.provenance.wire_sequence = it->cut_index;
        output.tick.provenance.batch_id = batch.batch_id;
        output.tick.provenance.batch_emitted_ns = batch.emitted_ns;
        output.tick.provenance.batch_close_reason = batch.reason;
        if (!factors_only_) {
            std::string model_error;
            output.tick.prediction_valid = model_->on_tick(
                row.values, "sse", cut.exchange_time_of_day_micros,
                &state->model_state, &output.tick.prediction, &model_error);
            if (!output.tick.prediction_valid)
                fail(error_text("SSE tick model rejected factors", model_error));
            if (!row.validity.complete || !state->auction->shared_static_valid()) {
                output.tick.prediction.selected = false;
                output.tick.prediction.selected_source = sse_hybrid_model::kNoSource;
                output.tick.prediction.selected_pred = 0.0f;
            }
            if (output.tick.prediction_valid) ++prediction_count;
        }
        callback_(output);
    }
    if (hardware_batch_mode_) {
        Output marker;
        marker.kind = kBatchEndOutput;
        marker.batch_end.batch_id = batch.batch_id;
        marker.batch_end.last_hardware_ns = batch.last_activity_ns;
        marker.batch_end.emitted_monotonic_ns = batch.emitted_ns;
        marker.batch_end.packet_count = batch.packet_count;
        marker.batch_end.candidate_count = static_cast<std::uint32_t>(batch.candidates.size());
        marker.batch_end.prediction_count = prediction_count;
        callback_(marker);
    }
}

bool SseStreamProcessor::valid_auction59(const std::vector<float>& factors) {
    if (factors.size() != 59U) return false;
    for (std::size_t i = 0; i < factors.size(); ++i)
        if (!std::isfinite(factors[i])) return false;
    return true;
}

void SseStreamProcessor::report_auction_state(
    const std::string& code, InstrumentState& state) {
    if (!state.auction->shared_static_valid() && !state.static_failure_logged) {
        std::cerr << "sse_prediction_gate code=" << code
                  << " mode=blocked reason=" << state.auction->reason() << "\n";
        state.static_failure_logged = true;
    }
    if (state.auction->failed() && state.auction->shared_static_valid() &&
        !state.auction_failure_logged) {
        std::cerr << "sse_auction59 code=" << code
                  << " mode=tick_only reason=" << state.auction->reason() << "\n";
        state.auction_failure_logged = true;
    }
}

void SseStreamProcessor::select_snapshot_mode(
    const std::string& code, InstrumentState& state) {
    if (state.auction_decided) return;
    state.auction_decided = true;
    if (auction59_provider_ && state.auction->shared_static_valid()) {
        // Retain explicit injection for model/replay tests. The production
        // entry points use the in-memory session, without a CSV provider.
        std::vector<float> factors;
        std::string error;
        bool ok = false;
        try {
            ok = auction59_provider_(code, kSnapshotGenerateStartMicros, &factors, &error);
        } catch (const std::exception& exception) {
            error = exception.what();
        }
        if (ok && valid_auction59(factors)) {
            auction59_inputs_[code] = factors;
            state.snapshot_model_enabled = true;
        } else {
            state.auction->disable(error_text("auction_provider_rejected", error));
        }
    } else if (state.auction->ready()) {
        auction59_inputs_[code] = state.auction->factors();
        state.snapshot_model_enabled = true;
    } else {
        state.auction->seal_missing();
    }
    report_auction_state(code, state);
    if (state.snapshot_model_enabled)
        std::cerr << "sse_auction59 code=" << code << " mode=snapshot_and_tick reason=ready\n";
}

void SseStreamProcessor::process_snapshot(
    const sse_live::Snapshot& snapshot,
    const deepwin_market_data::StreamEvent& event,
    std::size_t record_offset) {
    if (snapshot.sequence == 0ULL)
        fail("SSE snapshot has invalid wire sequence");
    InstrumentState* state = state_for(snapshot.security_id);
    if (state == 0) return;
    // Opening snapshots may be one-sided. Static/opening reconciliation does
    // not require Snapshot36's two-sided quote, and must precede its filters.
    state->auction->on_snapshot(snapshot);
    report_auction_state(snapshot.security_id, *state);
    if (!factors_only_ && snapshot.time_of_day_micros >= kSnapshotGenerateStartMicros)
        select_snapshot_mode(snapshot.security_id, *state);
    // A stock that has already fallen back does not run Snapshot36. Keep
    // checking common metadata above, but stale/invalid Snapshot totals must
    // not invalidate its independent tick path or the other stocks.
    if (!factors_only_ && state->auction_decided &&
        (!state->snapshot_model_enabled || !state->auction->shared_static_valid() ||
         (!auction59_provider_ && !state->auction->ready()))) return;
    // A one-sided quote is not a corrupt feed. It simply cannot form a
    // Snapshot36 row, just as in the existing live snapshot path.
    if (!sse_snapshot36::valid(snapshot)) return;
    if (state->have_previous_snapshot &&
        snapshot.time_of_day_micros <= state->last_snapshot_time) return;
    if (!state->have_previous_snapshot) {
        state->previous_snapshot = snapshot;
        state->last_snapshot_time = snapshot.time_of_day_micros;
        state->have_previous_snapshot = true;
        return;
    }
    const sse_live::Snapshot previous = state->previous_snapshot;
    if (snapshot.volume < previous.volume || snapshot.turnover < previous.turnover)
        fail("SSE snapshot cumulative values regressed within trading day");
    state->previous_snapshot = snapshot;
    state->last_snapshot_time = snapshot.time_of_day_micros;
    // Keep valid pre-open state so the first opening row uses the actual
    // preceding snapshot rather than silently dropping one recurrent step.
    if (snapshot.time_of_day_micros < kSnapshotGenerateStartMicros ||
        snapshot.time_of_day_micros >= kSnapshotGenerateEndMicros) return;
    const std::vector<float> factors = sse_snapshot36::build(previous, snapshot);
    if (factors.size() != 36U) fail("SSE Snapshot36 factor count mismatch");

    Output output;
    output.kind = kSnapshotOutput;
    output.snapshot.snapshot = snapshot;
    output.snapshot.snapshot36 = factors;
    output.snapshot.provenance = provenance(
        event, event.channel_id, snapshot.sequence, record_offset);
    if (!factors_only_) {
        std::map<std::string, std::vector<float> >::const_iterator auction =
            auction59_inputs_.find(snapshot.security_id);
        if (auction == auction59_inputs_.end())
            fail("SSE Snapshot Auction59 inputs are missing");
        output.snapshot.auction59 = auction->second;
        std::vector<float> enhanced(factors);
        enhanced.insert(enhanced.end(), auction->second.begin(), auction->second.end());
        std::string model_error;
        output.snapshot.prediction_valid = model_->on_snapshot(
            factors, enhanced, "sse", snapshot.time_of_day_micros,
            &state->model_state, &output.snapshot.prediction, &model_error);
        if (!output.snapshot.prediction_valid)
            fail(error_text("SSE Snapshot model rejected factors", model_error));
    }
    callback_(output);
}

}  // namespace sse_stream
