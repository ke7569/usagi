//
// Created by Administrator on 25-9-11.
//

#ifndef STATEGY_BASE_H
#define STATEGY_BASE_H


#include "../wc_strategy.h"
#include "../predictor/predictor.h"
#include "../predictor/mix153060_capture.h"
#include "../predictor/mix153060_live_adapter.h"
#include "../predictor/mix153060_model.h"
#include "../predictor/mix153060_runtime.h"
#include "ZStrategy.h"
#include "../snap_generator.h"
#include "../shsz_full_orderbook_diagnostics.h"
#include "../shsz_full_orderbook_manager.h"
#include "../shsz_predictor_transition_adapter.h"
#include "../sz_hp_factor_adapter.h"
#include "../sz_hp_realtime_state.h"
#include "snapshot_legacy15_factors.h"
#include "snapshot_legacy15_model.h"
#include "../../../sse-t0/market_data/sse_primary_decoder.h"
#include "../../../sse-t0/market_data/sse_tick_factors.h"
#include "../../../sse-t0/market_data/sse_tick_order_book.h"
#include "../../../sse-t0/model/auction59_sidecar.h"
#include "../../../sse-t0/model/snapshot36.h"
#include "../../../sse-t0/model/sse_hybrid_model.h"
#include <unordered_set>
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>

#ifdef T0_SZE_STRATEGY_ONLY
#include "SZERecoverable.h"
#include "SZEHealthState.h"
#include "sze_prediction_arbiter.h"
#endif

struct InsParams {
    int32_t Date = 0;
    double Close = 0.0;
    double Amount = 0.0;
    double Range = 0.0;
    double HistoryAmount = 0.0;
    double FreeShare = 0.0;
    double HpUpperPrice = 0.0;
    double HpLowerPrice = 0.0;
    double HpFeeShare = 0.0;
    double HistoryVolatility20d = 0.0;
    bool HasHistoryVolatility20d = false;
    int32_t static_position = 0;
    int32_t last_position = 0;
};

struct ShSzFullOrderBookSampleState {
    bool has_sample = false;
    int last_mid_price = 0;
    uint32_t last_sample_time_ms = 0;
};

// SSE owns a separate order book/factor state from the legacy SsePredictor.
// The two model families also carry independent recurrent states; no state is
// shared with Snapshot or Shenzhen code paths.
struct SseHybridRuntimeState {
    explicit SseHybridRuntimeState(const std::string& code);
    sse_hybrid_model::State model_state;
    sse_tick::OrderBook order_book;
    sse_tick::FactorState factor_state;
    sse_live::Snapshot previous_snapshot;
    bool has_previous_snapshot;
    std::vector<float> auction59_factors;
    std::unique_ptr<MSMarketDataField> signal_view;
    sse_live::TickEvent pending_tick;
    bool pending_tick_valid;
    bool pending_flush_scheduled;
    std::uint64_t pending_tick_receive_ns;
    short pending_tick_source;
    long pending_tick_rcv_time;
    std::uint64_t last_tick_receive_ns;
    std::uint64_t tick_sample_count;
};


class StrategyBase:public IWCStrategy {

public:
    enum OrderBookRuntimeMode {
        LEGACY_SNAPSHOT_MODE,
        FULL_ORDERBOOK_MODE,
        HP_SHADOW_MODE,
        HP_REALTIME_MODE
    };

    StrategyBase(const std::string &name, json &src_config);
    ~StrategyBase() override;
    json j_config;
    std::vector<std::string> mInstrumentVec;
    std::unordered_map<std::string, InsParams> mInsParamsMap;
    std::unordered_map<std::string,ZStrategy*> mZStrategyMap{};
    std::unordered_map<std::string,long> mLastSnapIndexMap;
    std::unordered_map<std::string, PredictorBase*> mPredictorMap;
    std::unordered_map<std::string,SnapGenerator> mSnapGeneratorMap;
    std::unordered_map<std::string,MSMarketDataField> curr_ob_dict;
    std::unordered_map<std::string,MSMarketDataField> last_ob_dict;
    int mTradeIndex;

private:
    std::string mMarket;
    OrderBookRuntimeMode mOrderBookMode = LEGACY_SNAPSHOT_MODE;
    std::unordered_set<std::string> mPendingPredictSet;
    ShSzFullOrderBookManager mFullOrderBookManager;
    std::unordered_map<std::string, ShSzOrderFlowSummary> mFullOrderFlowSummaryMap;
    std::unordered_map<std::string, ShSzPredictorTransitionInput> mPredictorTransitionMap;
    std::unordered_map<std::string, sz_hp::InstrumentState> mSzHpStateMap;
    std::unordered_map<std::string, uint64_t> mSzHpEventIndexMap;
    std::unordered_map<std::string, sz_hp::AdapterDiagnostic> mSzHpAdapterDiagnosticMap;
    std::unordered_map<std::string, sz_hp::FactorInput> mSzHpFactorInputMap;
    mix153060::Model mMix153060Model;
    std::unique_ptr<mix153060::Capture> mMix153060Capture;
    std::unordered_map<std::string, std::unique_ptr<mix153060::Runtime> > mMix153060RuntimeMap;
    std::unordered_map<std::string, mix153060::State> mMix153060ModelStateMap;
    std::unordered_map<std::string, std::unique_ptr<MSMarketDataField> > mMix153060SignalViewMap;
    bool mMix153060Enabled = false;
    bool mMix153060CaptureOnly = false;
    uint64_t mMix153060AdapterRejectCount = 0;
    uint64_t mMix153060BookRejectCount = 0;
    uint64_t mMix153060PredictionRejectCount = 0;
    bool mHpRealtimeModelReady = false;
    bool mFullOrderBookTraceEnabled = false;
    bool mFullOrderBookFactorTraceOnly = false;
    bool mFullOrderBookLazySampleTransition = false;
    bool mFullOrderBookLatencyEnabled = false;
    uint64_t mFullOrderBookLatencyLogInterval = 0;
    uint64_t mFullOrderBookLatencyLastLogEventCount = 0;
    uint64_t mFullOrderBookTraceMaxEvents = 0;
    uint64_t mFullOrderBookTraceEmitted = 0;
    std::unordered_set<std::string> mFullOrderBookTraceInstrumentFilter;
    std::unordered_map<std::string, uint64_t> mFullOrderBookTraceSequenceMap;
    std::unordered_map<std::string, ShSzFullOrderBookSampleState> mFullOrderBookSampleStateMap;
    void flush_pending_predictions(short source, long rcv_time);
    void process_l2_order_event(const LFL2OrderField* data,
                                short source,
                                long rcv_time);
    void process_l2_trade_event(const LFL2TradeField* data,
                                short source,
                                long rcv_time);
    bool can_dispatch_trading_signal() const;
    bool using_full_orderbook_mode() const;
    bool using_hp_shadow_mode() const;
    bool using_hp_realtime_mode() const;
    bool using_hp_mode() const;
    sz_hp::InstrumentState* hp_state_for(const std::string& code);
    bool process_hp_order(const std::string& code, const LFL2OrderField* data);
    sz_hp::SampleDecision process_hp_trade(const std::string& code, const LFL2TradeField* data);
    sz_hp::SampleDecision process_hp_observation(const std::string& code,
                                                 const LFL2MarketDataField* data);
    void consume_hp_sample_if_ready(const std::string& code,
                                     const sz_hp::SampleDecision& decision);
    mix153060::Runtime* mix153060_runtime_for(const std::string& code);
    void process_mix153060_order(const std::string& code,
                                 const LFL2OrderField* data,
                                 short source,
                                 long rcv_time);
    void process_mix153060_trade(const std::string& code,
                                 const LFL2TradeField* data,
                                 short source,
                                 long rcv_time);
#ifdef T0_SZE_STRATEGY_ONLY
    void update_sze_book_health(const std::string& code,
                                mix153060::Runtime* runtime);
#endif
    void flush_mix153060_pending(const std::string& code,
                                 short source,
                                 long rcv_time);
    void consume_mix153060_samples(const std::string& code,
                                   const mix153060::SampleBuffer& samples,
                                   short source,
                                   long rcv_time);
    MSMarketDataField* update_mix153060_signal_view(
        const std::string& code,
        const mix153060::Sample& sample);
    bool process_order_full_orderbook(const std::string& code, const LFL2OrderField* data, short source, long rcv_time);
    bool process_trade_full_orderbook(const std::string& code, const LFL2TradeField* data);
    bool using_lazy_sample_transition_mode() const;
    bool should_sample_predictor_transition(const std::string& code, uint32_t now_time_ms, int* mid_price);
    void mark_sample_predictor_transition(const std::string& code, uint32_t now_time_ms, int mid_price);
    void update_full_orderflow_from_order(const std::string& code, const LFL2OrderField* data);
    void update_full_orderflow_from_trade(const std::string& code, const LFL2TradeField* data);
    bool refresh_predictor_transition(const std::string& code, uint32_t now_time_ms);
    const MSMarketDataField* current_predict_signal_snapshot(const std::string& code) const;
    void reset_full_orderflow_summary(const std::string& code);
    void dump_full_orderbook_latency_summary();
    void maybe_log_full_orderbook_key_timing();
    void log_runtime_line(const std::string& line) const;
    bool should_trace_full_orderbook(const std::string& code);
    void trace_full_orderbook_order(const std::string& code,
                                    const LFL2OrderField* data,
                                    short source,
                                    long rcv_time,
                                    uint32_t now_time_ms,
                                    bool transition_ok,
                                    bool may_predict,
                                    bool did_predict,
                                    double prediction);
    void trace_full_orderbook_trade(const std::string& code,
                                    const LFL2TradeField* data,
                                    short source,
                                    long rcv_time,
                                    uint32_t now_time_ms,
                                    bool transition_ok,
                                    bool may_predict,
                                    bool did_predict,
                                    double prediction);
    std::vector<short> mTdSources;
    std::unordered_map<short, int> mPendingAccountRid;
    std::unordered_map<short, int> mPendingPositionRid;
    std::unordered_map<short, int> mEarlyAccountRid;
    std::unordered_map<short, int> mEarlyPositionRid;
    std::unordered_map<short, bool> mAccountReady;
    std::unordered_map<short, bool> mPositionReady;
    bool mRiskQueriesSubmitted = false;
    bool mRiskReadyLogged = false;
    bool mSzeLiveRoutingEnabled = false;
    std::unordered_set<std::string> mSzeLivePositionReady;
    int mSzePositionRetryIntervalMs = 5000;
    int mSzePositionCutoffHhmmss = 93100;
    bool mSzePositionRetryScheduled = false;
    bool mSzePositionCutoffApplied = false;
    void request_startup_risk_state();
    void schedule_startup_position_retry();
    void retry_or_finalize_startup_positions();
    void finalize_unresolved_startup_positions(const char* reason);
    bool is_risk_data_ready() const;

    struct SnapshotLegacy15RuntimeState {
        sze_snapshot15::Snapshot previous;
        sze_snapshot15::State hidden;
        bool has_previous = false;
        std::string trading_day;
    };
    bool mSnapshotLegacy15Enabled = false;
    short mSnapshotLegacy15Source = 90;
    sze_snapshot15::Model mSnapshotLegacy15Model;
    std::unordered_map<std::string, SnapshotLegacy15RuntimeState>
        mSnapshotLegacy15StateMap;
    std::unordered_map<std::string, std::unique_ptr<MSMarketDataField> >
        mSnapshotLegacy15SignalViewMap;
    std::uint64_t mSnapshotLegacy15PredictionCount = 0;
    std::uint64_t mSnapshotLegacy15RejectCount = 0;

    bool mSseHybridEnabled = false;
    short mSseHybridSnapshotSource = 89;
    sse_hybrid_model::Model mSseHybridModel;
    sse_auction59::FactorMap mSseAuction59Factors;
    std::unordered_map<std::string, std::unique_ptr<SseHybridRuntimeState> >
        mSseHybridStateMap;
    std::uint64_t mSseHybridSnapshotPredictionCount = 0;
    std::uint64_t mSseHybridTickPredictionCount = 0;
    std::uint64_t mSseHybridRejectCount = 0;

    bool configure_sse_hybrid(const json& config);
    SseHybridRuntimeState* sse_hybrid_state_for(const std::string& code);
    bool make_sse_tick_event(const LFL2OrderField& data,
                             short source,
                             std::uint64_t receive_ns,
                             sse_live::TickEvent* event) const;
    bool make_sse_tick_event(const LFL2TradeField& data,
                             short source,
                             std::uint64_t receive_ns,
                             sse_live::TickEvent* event) const;
    void process_sse_hybrid_order(const LFL2OrderField* data,
                                  short source,
                                  long rcv_time);
    void process_sse_hybrid_trade(const LFL2TradeField* data,
                                  short source,
                                  long rcv_time);
    void flush_sse_hybrid_tick(const std::string& code,
                               short source,
                               long rcv_time);
    void schedule_sse_hybrid_tick_flush(const std::string& code);
    void flush_sse_hybrid_tick_if_quiet(const std::string& code);
    void process_sse_hybrid_snapshot(const LFMarketDataField* data,
                                     short source,
                                     long rcv_time);

#ifdef T0_SZE_STRATEGY_ONLY
    struct SzeRecoveryConsumerConfig {
        bool enabled = false;
        std::uint32_t trading_day = 0;
        std::uint32_t source_id = 88;
        std::string journal_directory;
        std::string journal_prefix = "sze";
        std::uint64_t journal_segment_bytes = 1ULL << 30U;
        std::uint32_t journal_max_payload_bytes = 128;
        std::string shm_path;
        int state_cpu = -1;
        int strategy_cpu = -1;
        bool allow_invalid_replay_for_analysis = false;
        bool trading_enabled = false;
        bool health_state_enabled = false;
        std::string health_state_path;
        std::uint32_t shard_id = 0;
        std::uint32_t shard_count = 1;
    };

    struct SzeTradingSignal {
        std::array<double, BASIC_FIELD_NUM> market_data;
        char instrument[16];
        double prediction;
        short source;
        long receive_time;
        sze_prediction::Source prediction_source;
        std::uint32_t trading_day;
        std::uint64_t exchange_time_us;
        double turnover;
        std::uint64_t queue_sequence;
    };

    struct SzePredictionSignalState {
        sze_prediction::Arbiter arbiter;
        SzeTradingSignal full_orderbook;
        SzeTradingSignal snapshot;
        bool has_full_orderbook = false;
        bool has_snapshot = false;
        bool has_selected_source = false;
        sze_prediction::Source selected_source =
            sze_prediction::kFullOrderBook;
        std::uint64_t dispatch_count = 0;
    };

    void parse_sze_recovery_consumer_config(const json& config);
    void start_sze_recovery_consumer();
    void stop_sze_recovery_consumer();
    void sze_recovery_consumer_loop();
    void sze_invalid_analysis_replay_loop();
    void initialize_sze_recovery_clock_mapping();
    bool process_sze_recovery_event(const sze_recovery::CanonicalEvent& event,
                                    const void* payload,
                                    std::size_t payload_size);
    void sze_trading_poll_loop();
    bool enqueue_sze_trading_signal(const std::string& code,
                                    const MSMarketDataField* market_data,
                                    double prediction,
                                    short source,
                                    long receive_time,
                                    sze_prediction::Source prediction_source,
                                    std::uint32_t trading_day,
                                    std::uint64_t exchange_time_us);
    bool dequeue_sze_trading_signal(sze_prediction::Source prediction_source,
                                    SzeTradingSignal* signal);
    bool update_sze_prediction_candidate(const SzeTradingSignal& signal);
    void dispatch_sze_prediction_candidate(const std::string& code);
    void dispatch_or_queue_trading_signal(const std::string& code,
                                          const MSMarketDataField* market_data,
                                          double prediction,
                                          short source,
                                          long receive_time,
                                          sze_prediction::Source prediction_source,
                                          std::uint32_t trading_day,
                                          std::uint64_t exchange_time_us);

    SzeRecoveryConsumerConfig mSzeRecoveryConsumerConfig;
    static const std::uint64_t kSzeTradingSignalCapacity = 1024U;
    std::array<SzeTradingSignal, kSzeTradingSignalCapacity>
        mSzeTradingSignalSlots;
    alignas(64) std::atomic<std::uint64_t> mSzeTradingSignalHead{0};
    alignas(64) std::atomic<std::uint64_t> mSzeTradingSignalTail{0};
    std::array<SzeTradingSignal, kSzeTradingSignalCapacity>
        mSzeSnapshotSignalSlots;
    alignas(64) std::atomic<std::uint64_t> mSzeSnapshotSignalHead{0};
    alignas(64) std::atomic<std::uint64_t> mSzeSnapshotSignalTail{0};
    std::unordered_map<std::string, SzePredictionSignalState>
        mSzePredictionSignalStateMap;
    std::atomic<bool> mSzeTradingQueueHealthy{true};
    std::atomic<bool> mSzeRecoveryConsumerRunning{false};
    std::atomic<bool> mSzeRecoveryConsumerEntered{false};
    std::atomic<bool> mSzeRecoveryConsumerAttached{false};
    std::atomic<bool> mSzeTradingPollRunning{false};
    std::atomic<bool> mSzeRecoveryReplayContext{false};
    std::atomic<bool> mSzeRecoveryContinuityValid{false};
    std::atomic<bool> mSzeRecoveryLiveReady{false};
    std::atomic<bool> mSzeRecoveryClockMappingValid{false};
    std::atomic<std::uint64_t> mSzeRecoveryClockReferenceMonoNs{0};
    std::atomic<std::uint64_t> mSzeRecoveryClockReferenceRealtimeNs{0};
    bool mSzeRecoveryAnalysisMode = false;
    std::atomic<std::uint64_t> mSzeRecoveryEvents{0};
    std::atomic<std::uint64_t> mSzeRecoveryDecodeErrors{0};
    std::uint64_t mSzeCurrentRecoveryEventId = 0U;
    std::unique_ptr<sze_health::RecoveryShardHealthWriter>
        mSzeRecoveryHealthWriter;
    std::thread mSzeRecoveryConsumerThread;
    std::thread mSzeTradingPollThread;
#endif


public:
    void init();
#ifdef T0_SZE_STRATEGY_ONLY
    bool sze_recovery_consumer_started() const;
#endif
    void on_rtn_trade(const LFRtnTradeField *data, int request_id, short source, long rcv_time) final;
    void on_l2_trade(const LFL2TradeField *data, short source, long rcv_time) final;
    void on_market_data(const struct LFMarketDataField *mds, short source, long rcv_time) final;
    void on_ms_market_data(const MSMarketDataField *mds, short source, long rcv_time);
    void on_market_data_level2(const struct LFL2MarketDataField *mds, short source, long rcv_time) final;
    void on_l2_order(const struct LFL2OrderField *data, short source, long rcv_time) final ;
    void on_signal(const MSMarketDataField* market_data,const char*instrument_id,double signal,short source,long rcv_time);
    void update_info(const char *InstrumentID,short source,long rcv_time);
    void on_rtn_order(const struct LFRtnOrderField *data, int request_id, short source, long rcv_time) final;
    void on_rsp_account(const LFRspAccountField* data, int request_id, short source, long rcv_time,
        int errorId = 0, const char* errorMsg = nullptr) override;
    void on_rtn_pos_option(const LFRspPositionField* data, bool isLast, int request_id, short source, long rcv_time) override;



};



#endif //ZSTRATEGY_H
