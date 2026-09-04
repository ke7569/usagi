//
// Created by Administrator on 25-9-11.
//


#include "StrategyBase.h"
#include "ZStrategy.h"
#include "sze_position_risk.h"
#include "../RawDataStructAPI.h"
#include "../wc_strategy.h"
#include "../predictor/factor.h"
#include "../sz_hp_latency.h"
#include "../deepwin_strategy/sze_config_guard.h"
#include <cstring>
#include <cstdlib>
#include <cfloat>
#include <iostream>
#include <memory>
#include <sstream>
#include <utility>
#include <cmath>
#include <limits>
#include <algorithm>
#include <stdexcept>
#include <cstdio>
#include <ctime>
#include <chrono>

#ifdef T0_SZE_STRATEGY_ONLY
#include "SZEProtocol.h"
#include <immintrin.h>
#include <pthread.h>
#include <sched.h>
#include <time.h>
#endif

SseHybridRuntimeState::SseHybridRuntimeState(const std::string& code)
    : model_state(), order_book(code), factor_state(), previous_snapshot(),
      has_previous_snapshot(false), auction59_factors(), signal_view(),
      pending_tick(), pending_tick_valid(false), pending_flush_scheduled(false),
      pending_tick_receive_ns(0), pending_tick_source(0), pending_tick_rcv_time(0),
      last_tick_receive_ns(0), tick_sample_count(0) {}


namespace {
std::string NormalizeInstrumentId(const std::string& instrument_id) {
    auto dot_pos = instrument_id.find('.');
    if (dot_pos == std::string::npos) {
        return instrument_id;
    }
    return instrument_id.substr(0, dot_pos);
}

MSMarketDataField NewMixSignalView() {
    MSMarketDataField value = {MSMarketData()};
    return value;
}

double MixMarketTimeValue(int64_t exchange_time_us) {
    const int64_t day_us = 86400000000LL;
    int64_t tod = exchange_time_us % day_us;
    if (tod < 0) {
        tod += day_us;
    }
    const int64_t hour = tod / 3600000000LL;
    tod %= 3600000000LL;
    const int64_t minute = tod / 60000000LL;
    tod %= 60000000LL;
    const int64_t second = tod / 1000000LL;
    const int64_t millisecond = (tod % 1000000LL) / 1000LL;
    return static_cast<double>(hour * 10000000LL + minute * 100000LL +
                               second * 1000LL + millisecond);
}

std::uint64_t SnapshotExchangeTimeMs(const LFMarketDataField& value) {
    int hour = 0;
    int minute = 0;
    int second = 0;
    if (std::sscanf(value.UpdateTime, "%d:%d:%d", &hour, &minute, &second) != 3 ||
        hour < 0 || hour > 23 || minute < 0 || minute > 59 ||
        second < 0 || second > 59 || value.UpdateMillisec < 0 ||
        value.UpdateMillisec > 999) {
        return 0;
    }
    return (static_cast<std::uint64_t>(hour) * 3600U +
            static_cast<std::uint64_t>(minute) * 60U +
            static_cast<std::uint64_t>(second)) * 1000U +
           static_cast<std::uint64_t>(value.UpdateMillisec);
}

std::uint64_t SseMonotonicNowNs() {
    timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) return 0;
    return static_cast<std::uint64_t>(value.tv_sec) * 1000000000ULL +
           static_cast<std::uint64_t>(value.tv_nsec);
}

bool ParseSseTimeMicros(const char* text, std::uint64_t* output) {
    if (text == 0 || output == 0) return false;
    unsigned int hour = 0, minute = 0, second = 0, fraction = 0;
    char separator = 0;
    const int fields = std::sscanf(text, "%u:%u:%u%c%u",
                                    &hour, &minute, &second, &separator, &fraction);
    if (fields < 3 || hour > 23U || minute > 59U || second > 59U) return false;
    std::uint64_t micros = 0;
    if (fields >= 5 && separator == '.') {
        std::size_t digits = 0;
        const char* dot = std::strchr(text, '.');
        if (dot != 0) {
            for (const char* p = dot + 1; *p >= '0' && *p <= '9'; ++p) ++digits;
        }
        if (digits == 0) return false;
        if (digits <= 6) {
            micros = static_cast<std::uint64_t>(fraction);
            for (std::size_t i = digits; i < 6; ++i) micros *= 10ULL;
        } else {
            std::uint64_t scale = 1ULL;
            for (std::size_t i = 0; i < digits - 6; ++i) scale *= 10ULL;
            micros = static_cast<std::uint64_t>(fraction) / scale;
        }
    }
    *output = (static_cast<std::uint64_t>(hour) * 3600ULL +
               static_cast<std::uint64_t>(minute) * 60ULL + second) * 1000000ULL + micros;
    return true;
}

std::uint64_t SseRawPrice(double price) {
    if (!std::isfinite(price) || price <= 0.0) return 0;
    return static_cast<std::uint64_t>(std::llround(price * 1000.0));
}

std::uint64_t SseRawQuantity(double quantity) {
    if (!std::isfinite(quantity) || quantity <= 0.0) return 0;
    return static_cast<std::uint64_t>(std::llround(quantity * 1000.0));
}

std::uint64_t ExchangeTimeOfDayUs(std::int64_t exchange_time_us) {
    const std::int64_t day_us = 86400000000LL;
    std::int64_t value = exchange_time_us % day_us;
    if (value < 0) {
        value += day_us;
    }
    return static_cast<std::uint64_t>(value);
}

std::uint32_t ParseTradingDay(const std::string& value) {
    if (value.size() != 8U) {
        return 0U;
    }
    std::uint32_t result = 0U;
    for (std::string::const_iterator it = value.begin(); it != value.end(); ++it) {
        if (*it < '0' || *it > '9') {
            return 0U;
        }
        result = result * 10U + static_cast<std::uint32_t>(*it - '0');
    }
    return result;
}

sze_snapshot15::Snapshot SnapshotLegacy15FromLf(const LFMarketDataField& value) {
    sze_snapshot15::Snapshot snapshot;
    snapshot.symbol = NormalizeInstrumentId(value.InstrumentID);
    snapshot.trading_day = value.TradingDay;
    snapshot.exchange_time_ms = SnapshotExchangeTimeMs(value);
    snapshot.last_price = value.LastPrice;
    snapshot.volume = value.Volume;
    snapshot.turnover = value.Turnover;
    for (std::size_t level = 0; level < 5; ++level) {
        snapshot.bid_prices[level] = value.aBidPrice[level];
        snapshot.ask_prices[level] = value.aAskPrice[level];
        snapshot.bid_volumes[level] = value.aBidVolume[level];
        snapshot.ask_volumes[level] = value.aAskVolume[level];
    }
    return snapshot;
}

const uint32_t kContinuousAuctionStartMs = 9U * 3600000U + 26U * 60000U;

bool IsContinuousAuctionOrder(const LFL2OrderField* data) {
    if (data == 0) {
        return false;
    }
    return ShSzFullOrderBookEngine::parse_event_time_ms(data->OrderTime) > kContinuousAuctionStartMs;
}

StrategyBase::OrderBookRuntimeMode ParseOrderBookRuntimeMode(const json& src_config,
                                                             const std::string& market) {
    const char* market_key = market == "SZ" ? "sz_orderbook_mode" : "sh_orderbook_mode";
    std::string mode = "legacy-snapshot";
    json::const_iterator market_it = src_config.find(market_key);
    if (market_it != src_config.end() && market_it->is_string()) {
        mode = market_it->get<std::string>();
    } else {
        json::const_iterator common_it = src_config.find("mode");
        if (common_it == src_config.end() || !common_it->is_string()) {
            common_it = src_config.find("orderbook_mode");
        }
        if (common_it != src_config.end() && common_it->is_string()) {
            mode = common_it->get<std::string>();
        }
    }

    if (mode == "full-orderbook" || mode == "full_orderbook") {
        return StrategyBase::FULL_ORDERBOOK_MODE;
    }
    if (market == "SZ" && (mode == "hp-shadow" || mode == "hp_shadow")) {
        return StrategyBase::HP_SHADOW_MODE;
    }
    if (market == "SZ" && (mode == "hp-realtime" || mode == "hp_realtime")) {
        return StrategyBase::HP_REALTIME_MODE;
    }
    return StrategyBase::LEGACY_SNAPSHOT_MODE;
}

#ifdef T0_USE_DEEPWIN
bool IsEventBatchTail(const LFL2OrderField* data) {
    (void)data;
    return true;
}

bool IsEventBatchTail(const LFL2TradeField* data) {
    (void)data;
    return true;
}
#else
bool IsEventBatchTail(const LFL2OrderField* data) {
    return data->IsLast == 1;
}

bool IsEventBatchTail(const LFL2TradeField* data) {
    return data->IsLast == 1;
}
#endif

bool ParseBoolEnv(const char* env_name, bool default_value) {
    const char* env = std::getenv(env_name);
    if (env == 0 || env[0] == '\0') {
        return default_value;
    }
    if (env[0] == '0' || env[0] == 'f' || env[0] == 'F' || env[0] == 'n' || env[0] == 'N') {
        return false;
    }
    return true;
}

uint64_t ParseUint64Env(const char* env_name, uint64_t default_value) {
    const char* env = std::getenv(env_name);
    if (env == 0 || env[0] == '\0') {
        return default_value;
    }
    return static_cast<uint64_t>(std::strtoull(env, 0, 10));
}

bool ParseBoolConfigOrEnv(const json& src_config,
                          const char* config_key,
                          const char* env_name,
                          bool default_value) {
    json::const_iterator it = src_config.find(config_key);
    if (it != src_config.end() && it->is_boolean()) {
        return it->get<bool>();
    }
    return ParseBoolEnv(env_name, default_value);
}

uint64_t ParseUint64ConfigOrEnv(const json& src_config,
                                const char* config_key,
                                const char* env_name,
                                uint64_t default_value) {
    json::const_iterator it = src_config.find(config_key);
    if (it != src_config.end() && it->is_number_unsigned()) {
        return it->get<uint64_t>();
    }
    if (it != src_config.end() && it->is_number_integer()) {
        const int64_t value = it->get<int64_t>();
        return value > 0 ? static_cast<uint64_t>(value) : 0;
    }
    return ParseUint64Env(env_name, default_value);
}

void AppendUniqueSource(std::vector<short>* sources, short source) {
    if (sources == 0) {
        return;
    }
    if (std::find(sources->begin(), sources->end(), source) == sources->end()) {
        sources->push_back(source);
    }
}

void AddInstrumentFilterToken(const std::string& token, std::unordered_set<std::string>* out) {
    if (out == 0 || token.empty()) {
        return;
    }
    out->insert(NormalizeInstrumentId(token));
}

void AppendInstrumentFilterCsv(const std::string& csv, std::unordered_set<std::string>* out) {
    size_t begin = 0;
    while (begin < csv.size()) {
        size_t end = csv.find(',', begin);
        if (end == std::string::npos) {
            end = csv.size();
        }
        std::string token = csv.substr(begin, end - begin);
        size_t left = token.find_first_not_of(" \t");
        size_t right = token.find_last_not_of(" \t");
        if (left != std::string::npos && right != std::string::npos) {
            AddInstrumentFilterToken(token.substr(left, right - left + 1), out);
        }
        begin = end + 1;
    }
}

void LoadTraceInstrumentFilter(const json& src_config,
                               std::unordered_set<std::string>* out) {
    if (out == 0) {
        return;
    }
    out->clear();

    json::const_iterator it = src_config.find("full_orderbook_trace_instruments");
    if (it != src_config.end() && it->is_array()) {
        for (json::const_iterator item = it->begin(); item != it->end(); ++item) {
            if (item->is_string()) {
                AddInstrumentFilterToken(item->get<std::string>(), out);
            }
        }
    }

    const char* env = std::getenv("T0_FULL_ORDERBOOK_TRACE_INSTRUMENTS");
    if (env != 0 && env[0] != '\0') {
        AppendInstrumentFilterCsv(env, out);
    }
}

mix153060::CaptureConfig ParseMix153060CaptureConfig(const json& src_config) {
    mix153060::CaptureConfig result;
    json::const_iterator capture_it = src_config.find("sze_prediction_capture");
    if (capture_it == src_config.end()) {
        capture_it = src_config.find("mix153060_capture");
    }
    if (capture_it == src_config.end() || !capture_it->is_object()) {
        return result;
    }
    const json& capture = *capture_it;
    json::const_iterator it = capture.find("enabled");
    if (it != capture.end() && it->is_boolean()) {
        result.enabled = it->get<bool>();
    }
    it = capture.find("directory");
    if (it != capture.end() && it->is_string()) {
        result.directory = it->get<std::string>();
    }
    it = capture.find("prefix");
    if (it != capture.end() && it->is_string()) {
        result.prefix = it->get<std::string>();
    }
    it = capture.find("instruments");
    if (it != capture.end() && it->is_array()) {
        for (json::const_iterator item = it->begin(); item != it->end(); ++item) {
            if (item->is_string()) {
                result.instruments.push_back(item->get<std::string>());
            }
        }
    }
    if (result.instruments.empty()) {
        json::const_iterator params = src_config.find("ins_params");
        if (params != src_config.end() && params->is_object()) {
            for (json::const_iterator item = params->begin(); item != params->end(); ++item) {
                if (item.key().find('.') == std::string::npos) {
                    result.instruments.push_back(item.key() + ".SZ");
                } else {
                    result.instruments.push_back(item.key());
                }
            }
        }
    }
    it = capture.find("output_format");
    if (it != capture.end() && it->is_string()) {
        result.output_format = it->get<std::string>();
    }
    it = capture.find("detail_instruments");
    if (it != capture.end() && it->is_array()) {
        for (json::const_iterator item = it->begin(); item != it->end(); ++item) {
            if (item->is_string()) {
                result.detail_instruments.push_back(item->get<std::string>());
            }
        }
    }
    it = capture.find("detail");
    if (result.detail_instruments.empty() && it != capture.end() &&
        ((it->is_boolean() && it->get<bool>()) ||
         (it->is_string() && it->get<std::string>() == "all"))) {
        result.detail_instruments = result.instruments;
    }
    it = capture.find("events");
    if (it != capture.end() && it->is_boolean()) {
        result.record_events = it->get<bool>();
    }
    it = capture.find("samples");
    if (it != capture.end() && it->is_boolean()) {
        result.record_samples = it->get<bool>();
    }
    it = capture.find("capture_only");
    if (it != capture.end() && it->is_boolean()) {
        result.capture_only = it->get<bool>();
    }
    it = capture.find("flush_rows");
    if (it != capture.end() && it->is_number_unsigned()) {
        result.flush_rows = static_cast<std::size_t>(it->get<std::uint64_t>());
    } else if (it != capture.end() && it->is_number_integer()) {
        const std::int64_t value = it->get<std::int64_t>();
        result.flush_rows = value > 0 ? static_cast<std::size_t>(value) : 0;
    }
    it = capture.find("flush_interval_ms");
    if (it != capture.end() && it->is_number_unsigned()) {
        result.flush_interval_ms = it->get<std::uint32_t>();
    } else if (it != capture.end() && it->is_number_integer()) {
        const std::int64_t value = it->get<std::int64_t>();
        result.flush_interval_ms = value > 0 ? static_cast<std::uint32_t>(value) : 0U;
    }
    it = capture.find("log_batch_bytes");
    if (it != capture.end() && it->is_number_unsigned()) {
        result.log_batch_bytes = static_cast<std::size_t>(it->get<std::uint64_t>());
    }
    it = capture.find("log_queue_bytes");
    if (it != capture.end() && it->is_number_unsigned()) {
        result.log_queue_bytes = static_cast<std::size_t>(it->get<std::uint64_t>());
    }
    return result;
}

double NormalizeSnapshotPrice(double price) {
    if (price == DBL_MAX || !std::isfinite(price) || price <= 0.0) {
        return 0.0;
    }
    return price;
}

double NormalizeIntPrice(int price) {
    return price > 0 ? static_cast<double>(price) / PRICE_MULTIPLIER : 0.0;
}

const char* BoolText(bool value) {
    return value ? "1" : "0";
}
}

void StrategyBase::request_startup_risk_state() {
    if (mRiskQueriesSubmitted) {
        return;
    }
    mRiskQueriesSubmitted = true;
    mTdSources.clear();
    mPendingAccountRid.clear();
    mPendingPositionRid.clear();
    mEarlyAccountRid.clear();
    mEarlyPositionRid.clear();
    mAccountReady.clear();
    mPositionReady.clear();
    if (j_config.find("td_source_index") != j_config.end()) {
        for (const auto& src : j_config["td_source_index"]) {
            mTdSources.push_back(static_cast<short>(src.get<int>()));
        }
    }
    if (mTdSources.empty()) {
        KF_LOG_INFO(logger, "[RiskInit] no td_source_index configured, skip startup account/position query");
        return;
    }

    if (util != nullptr) {
        util->set_pos_flag(false);
    }

    for (short src : mTdSources) {
        const int account_rid = req_account(src);
        const int position_rid = req_position(src);
        mPendingAccountRid[src] = account_rid;
        mPendingPositionRid[src] = position_rid;
        mAccountReady[src] = account_rid < 0;
        mPositionReady[src] = !mSzeLiveRoutingEnabled && position_rid < 0;
        const auto early_account_it = mEarlyAccountRid.find(src);
        if (early_account_it != mEarlyAccountRid.end() && early_account_it->second == account_rid) {
            mAccountReady[src] = true;
        }
        const auto early_position_it = mEarlyPositionRid.find(src);
        if (early_position_it != mEarlyPositionRid.end() &&
            early_position_it->second == position_rid &&
            (!mSzeLiveRoutingEnabled ||
             mSzeLivePositionReady.size() == mInstrumentVec.size())) {
            mPositionReady[src] = true;
        }
        KF_LOG_INFO(logger, "[RiskInit] source=" << src
            << " req_account_rid=" << account_rid
            << " req_position_rid=" << position_rid
            << " configured_instruments=" << mInstrumentVec.size()
            << " prediction_continues_while_routing_gated=1");
    }

    if (mSzeLiveRoutingEnabled && !is_risk_data_ready()) {
        schedule_startup_position_retry();
    }

    if (!mRiskReadyLogged && is_risk_data_ready()) {
        mRiskReadyLogged = true;
        KF_LOG_INFO(logger, "[RiskInit] startup account/position ready, trading enabled");
    }
}

void StrategyBase::schedule_startup_position_retry() {
    if (!mSzeLiveRoutingEnabled || mSzePositionRetryScheduled ||
        mSzePositionCutoffApplied ||
        mSzeLivePositionReady.size() == mInstrumentVec.size() ||
        util == nullptr) {
        return;
    }
    mSzePositionRetryScheduled = true;
    const long long now = util->get_nano();
    BLCallback callback = std::bind(
        &StrategyBase::retry_or_finalize_startup_positions, this);
    util->insert_callback(
        now + static_cast<long long>(mSzePositionRetryIntervalMs) * 1000000LL,
        callback);
}

void StrategyBase::finalize_unresolved_startup_positions(const char* reason) {
    std::size_t defaulted = 0;
    std::ostringstream examples;
    for (std::unordered_map<std::string, ZStrategy*>::iterator it =
             mZStrategyMap.begin(); it != mZStrategyMap.end(); ++it) {
        if (it->second == nullptr ||
            mSzeLivePositionReady.find(it->first) != mSzeLivePositionReady.end()) {
            continue;
        }
        it->second->sync_startup_position(0, 0);
        mSzeLivePositionReady.insert(it->first);
        if (defaulted < 10) {
            if (defaulted != 0) {
                examples << '|';
            }
            examples << it->first;
        }
        ++defaulted;
    }
    mSzePositionCutoffApplied = true;
    for (short src : mTdSources) {
        mPositionReady[src] = true;
    }
    KF_LOG_INFO(logger, "[RiskInit][PositionCutoff] reason="
        << (reason == nullptr ? "unknown" : reason)
        << " cutoff_hhmmss=" << mSzePositionCutoffHhmmss
        << " defaulted=" << defaulted
        << " examples=" << examples.str()
        << " position_instruments=" << mSzeLivePositionReady.size()
        << "/" << mInstrumentVec.size());
}

void StrategyBase::retry_or_finalize_startup_positions() {
    mSzePositionRetryScheduled = false;
    if (!mSzeLiveRoutingEnabled || mSzePositionCutoffApplied ||
        mSzeLivePositionReady.size() == mInstrumentVec.size()) {
        return;
    }

    std::time_t now = std::time(nullptr);
    std::tm local = {};
    localtime_r(&now, &local);
    const int market_hhmmss =
        local.tm_hour * 10000 + local.tm_min * 100 + local.tm_sec;
    if (sze_position_risk::AtOrAfterCutoff(
            market_hhmmss, mSzePositionCutoffHhmmss)) {
        finalize_unresolved_startup_positions("market_time_cutoff");
        if (!mRiskReadyLogged && is_risk_data_ready()) {
            mRiskReadyLogged = true;
            KF_LOG_INFO(logger, "[RiskInit] startup account/position ready, trading enabled");
        }
        return;
    }

    for (short src : mTdSources) {
        if (mPositionReady[src]) {
            continue;
        }
        const int position_rid = req_position(src);
        mPendingPositionRid[src] = position_rid;
        // A failed submission is not authoritative position data. Keep the
        // source gated and retry until a complete result or the cutoff.
        mPositionReady[src] = false;
        const auto early = mEarlyPositionRid.find(src);
        if (early != mEarlyPositionRid.end() && early->second == position_rid &&
            mSzeLivePositionReady.size() == mInstrumentVec.size()) {
            mPositionReady[src] = true;
        }
        KF_LOG_INFO(logger, "[RiskInitRetry] position request source=" << src
            << " rid=" << position_rid
            << " resolved=" << mSzeLivePositionReady.size()
            << "/" << mInstrumentVec.size()
            << " cutoff_hhmmss=" << mSzePositionCutoffHhmmss);
    }
    schedule_startup_position_retry();
}

bool StrategyBase::is_risk_data_ready() const {
    if (!mRiskQueriesSubmitted) {
        return true;
    }
    if (mTdSources.empty()) {
        return true;
    }
    for (short src : mTdSources) {
        const auto account_it = mAccountReady.find(src);
        const auto position_it = mPositionReady.find(src);
        const bool account_ready = (account_it != mAccountReady.end() && account_it->second);
        const bool position_ready = (position_it != mPositionReady.end() && position_it->second);
        if (!account_ready || !position_ready) {
            return false;
        }
    }
    if (mSzeLiveRoutingEnabled &&
        mSzeLivePositionReady.size() != mInstrumentVec.size()) {
        return false;
    }
    return true;
}

StrategyBase::StrategyBase(const std::string &name, json& src_config): IWCStrategy(name),j_config(src_config) {
    auto& ins_params = src_config["ins_params"];
    mInstrumentVec.reserve(ins_params.size());
    for (auto it = ins_params.begin(); it != ins_params.end(); ++it) {
        const auto& raw_code = it.key();
        const auto& params_json = it.value();
        const std::string code = NormalizeInstrumentId(raw_code);
        InsParams params{};
        if (params_json.find("Date") != params_json.end()) {
            params.Date = params_json["Date"].get<int32_t>();
        }
        if (params_json.find("Close") != params_json.end()) {
            params.Close = params_json["Close"].get<double>();
        }
        if (params_json.find("Amount") != params_json.end()) {
            params.Amount = params_json["Amount"].get<double>();
        }
        if (params_json.find("Range") != params_json.end()) {
            params.Range = params_json["Range"].get<double>();
        }
        if (params_json.find("HistoryAmount") != params_json.end()) {
            params.HistoryAmount = params_json["HistoryAmount"].get<double>();
        }
        if (params_json.find("FreeShare") != params_json.end() &&
            params_json["FreeShare"].is_number()) {
            params.FreeShare = params_json["FreeShare"].get<double>();
        } else if (params_json.find("free_share") != params_json.end() &&
                   params_json["free_share"].is_number()) {
            params.FreeShare = params_json["free_share"].get<double>();
        }
        if (params_json.find("HpUpperPrice") != params_json.end() &&
            params_json["HpUpperPrice"].is_number()) {
            params.HpUpperPrice = params_json["HpUpperPrice"].get<double>();
        }
        if (params_json.find("HpLowerPrice") != params_json.end() &&
            params_json["HpLowerPrice"].is_number()) {
            params.HpLowerPrice = params_json["HpLowerPrice"].get<double>();
        }
        if (params_json.find("UpperLimitPrice") != params_json.end() &&
            params_json["UpperLimitPrice"].is_number()) {
            params.HpUpperPrice = params_json["UpperLimitPrice"].get<double>();
        } else if (params_json.find("upper_limit_price") != params_json.end() &&
                   params_json["upper_limit_price"].is_number()) {
            params.HpUpperPrice = params_json["upper_limit_price"].get<double>();
        }
        if (params_json.find("LowerLimitPrice") != params_json.end() &&
            params_json["LowerLimitPrice"].is_number()) {
            params.HpLowerPrice = params_json["LowerLimitPrice"].get<double>();
        } else if (params_json.find("lower_limit_price") != params_json.end() &&
                   params_json["lower_limit_price"].is_number()) {
            params.HpLowerPrice = params_json["lower_limit_price"].get<double>();
        }
        if (params_json.find("HpFeeShare") != params_json.end() &&
            params_json["HpFeeShare"].is_number()) {
            params.HpFeeShare = params_json["HpFeeShare"].get<double>();
        }
        if (params_json.find("HistoryVolatility20d") != params_json.end() &&
            params_json["HistoryVolatility20d"].is_number()) {
            params.HistoryVolatility20d = params_json["HistoryVolatility20d"].get<double>();
            params.HasHistoryVolatility20d = true;
        }
        if (params_json.find("static_position") != params_json.end()) {
            params.static_position = static_cast<int32_t>(
                params_json["static_position"].get<double>());
        }
        if (params_json.find("last_position") != params_json.end()) {
            params.last_position = static_cast<int32_t>(
                params_json["last_position"].get<double>());
        }
        auto insert_result = mInsParamsMap.emplace(code, params);
        if (!insert_result.second) {
            insert_result.first->second = params;
        } else {
            mInstrumentVec.push_back(code);
        }
    }
#ifdef T0_SZE_STRATEGY_ONLY
    std::string sze_market_error;
    if (!sze_strategy_library::validate_config(src_config, &sze_market_error)) {
        throw std::runtime_error(sze_market_error);
    }
#endif
    mMarket = src_config["market"].get<std::string>();
    mOrderBookMode = ParseOrderBookRuntimeMode(src_config, mMarket);
    configure_sse_hybrid(src_config);
    const std::string sse_prediction_log_path =
        src_config.value("sse_prediction_log_path", std::string());
    if (!sse_prediction_log_path.empty() &&
        !mSsePredictionLog.open(sse_prediction_log_path)) {
        KF_LOG_ERROR(logger, "[SSEHybrid] failed to open async prediction log: "
            << sse_prediction_log_path);
    }
    // Trading/risk startup is exchange-specific.  SSE uses its own routing
    // block and must not inherit the Shenzhen gate by accident.
    const char* routing_key = mMarket == "SH"
        ? "sse_order_routing" : "sze_order_routing";
    if (src_config.find(routing_key) != src_config.end() &&
        src_config[routing_key].is_object()) {
        const json& routing = src_config[routing_key];
        mSzeLiveRoutingEnabled = routing.value("enabled", false);
        mSzePositionRetryIntervalMs = std::max(
            1000, routing.value("position_query_retry_ms", 5000));
        mSzePositionCutoffHhmmss = routing.value(
            "position_query_cutoff_hhmmss", 93100);
    }
#ifdef T0_SZE_STRATEGY_ONLY
    parse_sze_recovery_consumer_config(src_config);
#endif
    json::const_iterator snapshot_config = src_config.find("snapshot_legacy15");
    if (snapshot_config != src_config.end()) {
        if (!snapshot_config->is_object()) {
            throw std::runtime_error("snapshot_legacy15 must be an object");
        }
        mSnapshotLegacy15Enabled = snapshot_config->value("enabled", false);
        mSnapshotLegacy15Source = static_cast<short>(
            snapshot_config->value("source_id", 90));
        if (mSnapshotLegacy15Enabled) {
            if (mMarket != "SZ") {
                throw std::runtime_error("snapshot_legacy15 requires market=SZ");
            }
            const std::string snapshot_model_path =
                snapshot_config->value("model_path", std::string());
            const std::string snapshot_scaler_path =
                snapshot_config->value("scaler_path", std::string());
            if (snapshot_model_path.empty() || snapshot_scaler_path.empty()) {
                throw std::runtime_error(
                    "snapshot_legacy15 requires model_path and scaler_path");
            }
            std::string snapshot_error;
            if (!mSnapshotLegacy15Model.load(
                    snapshot_model_path, snapshot_scaler_path, &snapshot_error)) {
                throw std::runtime_error(
                    "snapshot_legacy15 model load failed: " + snapshot_error);
            }
            for (std::vector<std::string>::const_iterator code = mInstrumentVec.begin();
                 code != mInstrumentVec.end(); ++code) {
                mSnapshotLegacy15StateMap.emplace(
                    *code, SnapshotLegacy15RuntimeState());
                mSnapshotLegacy15SignalViewMap.emplace(
                    *code, std::unique_ptr<MSMarketDataField>(
                        new MSMarketDataField(NewMixSignalView())));
            }
            KF_LOG_INFO(logger, "[SZSnapshotFallback] enabled=1 source="
                << mSnapshotLegacy15Source
                << " instruments=" << mInstrumentVec.size()
                << " recovery_required=0");
        }
    }
    const mix153060::CaptureConfig mix_capture_config =
        ParseMix153060CaptureConfig(src_config);
    mMix153060CaptureOnly = mix_capture_config.enabled && mix_capture_config.capture_only;
    if (mMix153060CaptureOnly && !using_hp_shadow_mode()) {
        throw std::runtime_error("mix153060 capture_only requires Shenzhen hp-shadow mode");
    }
#ifdef T0_SZE_STRATEGY_ONLY
    if (mSzeRecoveryConsumerConfig.allow_invalid_replay_for_analysis) {
        if (!using_hp_shadow_mode() || !mMix153060CaptureOnly ||
            !mix_capture_config.record_samples) {
            throw std::runtime_error(
                "allow_invalid_replay_for_analysis requires hp-shadow capture_only samples");
        }
        json::const_iterator td_sources = src_config.find("td_source_index");
        if (td_sources == src_config.end() || !td_sources->is_array() ||
            !td_sources->empty()) {
            throw std::runtime_error(
                "allow_invalid_replay_for_analysis requires empty td_source_index");
        }
        json::const_iterator vtd_sources = src_config.find("vtd");
        if (vtd_sources == src_config.end() || !vtd_sources->is_array() ||
            !vtd_sources->empty()) {
            throw std::runtime_error(
                "allow_invalid_replay_for_analysis requires explicit empty vtd");
        }
        mSzeRecoveryAnalysisMode = true;
    }
#endif
    double hp_fee_share = 1.0;
    if (src_config.find("hp_fee_share") != src_config.end() &&
        src_config["hp_fee_share"].is_number()) {
        hp_fee_share = src_config["hp_fee_share"].get<double>();
    }
    uint32_t hp_downsample = sz_hp::kDefaultDownsample;
    if (src_config.find("hp_downsample") != src_config.end() &&
        src_config["hp_downsample"].is_number()) {
        const int64_t value = src_config["hp_downsample"].get<int64_t>();
        hp_downsample = value > 0 ? static_cast<uint32_t>(value) : 0U;
    }
    double hp_upper_price = std::numeric_limits<double>::max();
    double hp_lower_price = 0.0;
    if (src_config.find("hp_upper_price") != src_config.end() &&
        src_config["hp_upper_price"].is_number()) {
        hp_upper_price = src_config["hp_upper_price"].get<double>();
    }
    if (src_config.find("hp_lower_price") != src_config.end() &&
        src_config["hp_lower_price"].is_number()) {
        hp_lower_price = src_config["hp_lower_price"].get<double>();
    }
    const bool hp_capture_failure_digest = ParseBoolConfigOrEnv(
        src_config, "hp_capture_failure_digest", "T0_HP_CAPTURE_FAILURE_DIGEST", false);
    std::string model_path = "model/model_test.json";
    json::const_iterator model_path_it = src_config.find("model_path");
    if (model_path_it != src_config.end()) {
        if (!model_path_it->is_string()) {
            throw std::runtime_error("model_path must be a string");
        }
        model_path = model_path_it->get<std::string>();
    }
    bool mix_model_loaded = false;
    bool mix_inputs_valid = true;
    if (using_hp_mode()) {
        if (src_config.find("mix153060_model_artifact") != src_config.end() ||
            src_config.find("hp_model_artifact") != src_config.end()) {
            throw std::runtime_error(
                "HP-mode Shenzhen config accepts only model_path; remove model artifact aliases");
        }
        if (model_path_it == src_config.end() || model_path.empty()) {
            throw std::runtime_error("HP-mode Shenzhen config requires non-empty model_path");
        }
        std::string model_error;
        mix_model_loaded = mMix153060Model.load(model_path, &model_error);
        if (!mix_model_loaded) {
            throw std::runtime_error(
                "Cannot load Shenzhen model file: " + model_path + ": " + model_error);
        }
        KF_LOG_INFO(logger, "[SZ][prediction] model loaded path=" << model_path
                           << " checkpoint_sha256=" << mMix153060Model.checkpoint_sha256()
                           << " factor_sha256=" << mMix153060Model.factor_names_sha256());
    }
    mFullOrderBookTraceEnabled = ParseBoolConfigOrEnv(
        src_config, "full_orderbook_trace", "T0_FULL_ORDERBOOK_TRACE", false);
    mFullOrderBookFactorTraceOnly = ParseBoolConfigOrEnv(
        src_config, "full_orderbook_factor_trace_only", "T0_FULL_ORDERBOOK_FACTOR_TRACE_ONLY", false);
    mFullOrderBookLazySampleTransition = ParseBoolConfigOrEnv(
        src_config,
        "full_orderbook_lazy_sample_transition",
        "T0_FULL_ORDERBOOK_LAZY_SAMPLE_TRANSITION",
        mFullOrderBookFactorTraceOnly);
    mFullOrderBookLatencyEnabled = ParseBoolConfigOrEnv(
        src_config, "full_orderbook_latency", "T0_FULL_ORDERBOOK_LATENCY", false);
    shsz_full_orderbook_set_latency_enabled(mFullOrderBookLatencyEnabled);
    mFullOrderBookLatencyLogInterval = ParseUint64ConfigOrEnv(
        src_config, "full_orderbook_latency_log_interval", "T0_FULL_ORDERBOOK_LATENCY_LOG_INTERVAL", 0);
    mFullOrderBookTraceMaxEvents = ParseUint64ConfigOrEnv(
        src_config, "full_orderbook_trace_max_events", "T0_FULL_ORDERBOOK_TRACE_MAX_EVENTS", 0);
    LoadTraceInstrumentFilter(src_config, &mFullOrderBookTraceInstrumentFilter);
    std::string model_type = "real_gru";
    if (src_config.find("model_type") != src_config.end()) {
        model_type = src_config["model_type"].get<std::string>();
    }
    std::string scaler_path;
    if (src_config.find("scaler_path") != src_config.end()) {
        scaler_path = src_config["scaler_path"].get<std::string>();
    }
    for (const auto& code : mInstrumentVec) {
        const auto& params = mInsParamsMap.at(code);
        double sample_num = 8000;
        if (mMarket == "SH") {
            sample_num = 12000;
        }
        double thres = params.HistoryAmount / sample_num;
        if (!using_hp_mode() && !mSseHybridEnabled) {
            mSnapGeneratorMap[code] = SnapGenerator();
            if (mMarket == "SZ") {
                mPredictorMap[code] = new SzePredictor(
                    thres, params.HistoryAmount, model_type, model_path, scaler_path);
            } else if (mMarket == "SH") {
                mPredictorMap[code] = new SsePredictor(
                    thres, params.HistoryAmount, model_type, model_path, scaler_path);
            } else {
                std::cerr << "Unsupported market: " << mMarket << " (supported: SZ, SH)" << std::endl;
            }
        }
        mFullOrderFlowSummaryMap[code] = ShSzOrderFlowSummary();
        mPredictorTransitionMap[code] = ShSzPredictorTransitionInput();
        if (mMarket == "SZ" && using_hp_mode()) {
            sz_hp::SamplerConfig hp_config;
            if (std::fabs(params.HistoryAmount) < 1e-12) {
                hp_config.history_amount_threshold = std::numeric_limits<double>::max();
                hp_config.downsample = 0U;
            } else {
                hp_config.history_amount_threshold = params.HistoryAmount;
                hp_config.downsample = hp_downsample;
            }
            hp_config.fee_share = params.HpFeeShare > 0.0
                                      ? params.HpFeeShare
                                      : hp_fee_share;
            hp_config.upper_price = params.HpUpperPrice > 0.0
                                        ? params.HpUpperPrice
                                        : hp_upper_price;
            hp_config.lower_price = params.HpLowerPrice > 0.0
                                        ? params.HpLowerPrice
                                        : hp_lower_price;
            hp_config.capture_failure_digest = hp_capture_failure_digest;
            mSzHpStateMap.insert(std::make_pair(code, sz_hp::InstrumentState(code, hp_config)));
            mSzHpEventIndexMap[code] = 0;
            if (mix_model_loaded) {
                mix153060::StaticInputs mix_inputs;
                mix_inputs.instrument = code;
                mix_inputs.trading_date = params.Date;
                mix_inputs.average_amount = params.HistoryAmount;
                mix_inputs.turnover_threshold = params.HistoryAmount / 8000.0;
                mix_inputs.free_share = params.FreeShare;
                mix_inputs.pre_close = params.Close;
                mix_inputs.upper_limit = params.HpUpperPrice > 0.0
                                             ? params.HpUpperPrice
                                             : (std::isfinite(hp_upper_price) &&
                                                hp_upper_price < std::numeric_limits<double>::max()
                                                    ? hp_upper_price : 0.0);
                mix_inputs.lower_limit = params.HpLowerPrice > 0.0
                                             ? params.HpLowerPrice : hp_lower_price;
                mix_inputs.history_volatility_20d = params.HasHistoryVolatility20d
                                                        ? params.HistoryVolatility20d
                                                        : std::numeric_limits<double>::quiet_NaN();
                if (mMix153060Model.factor_names_sha256() ==
                        "e20ed70098a025f597f8b9cda41fb79b3188d875ad227f243c872ddbfbbed97e" &&
                    (!std::isfinite(mix_inputs.free_share) || mix_inputs.free_share <= 0.0)) {
                    mix_inputs_valid = false;
                    KF_LOG_ERROR(logger, "[SZ][prediction] v0.4 requires positive FreeShare instrument="
                                         << code << "; add FreeShare/free_share to ins_params");
                }
                std::unique_ptr<mix153060::Runtime> runtime(
                    new mix153060::Runtime(mix_inputs));
                if (!runtime->configured()) {
                    mix_inputs_valid = false;
                    KF_LOG_ERROR(logger, "[SZ][prediction] invalid static inputs instrument="
                                         << code << " date=" << mix_inputs.trading_date
                                         << " average_amount=" << mix_inputs.average_amount
                                         << " free_share=" << mix_inputs.free_share
                                         << " pre_close=" << mix_inputs.pre_close
                                         << " upper_limit=" << mix_inputs.upper_limit
                                         << " lower_limit=" << mix_inputs.lower_limit);
                }
                mMix153060RuntimeMap.insert(std::make_pair(code, std::move(runtime)));
                mMix153060ModelStateMap.insert(std::make_pair(code, mix153060::State()));
                const MSMarketDataField initial_view = NewMixSignalView();
                mMix153060SignalViewMap.insert(std::make_pair(
                    code, std::unique_ptr<MSMarketDataField>(new MSMarketDataField(initial_view))));
            }
        }
    }
    mMix153060Enabled = mix_model_loaded && mix_inputs_valid && !mMix153060RuntimeMap.empty();
    mHpRealtimeModelReady = mMix153060Enabled;
    if (mix_model_loaded && !mMix153060Enabled) {
        KF_LOG_ERROR(logger, "[SZ][prediction] model loaded but runtime inputs are incomplete; "
                             "state will remain signal-suppressed");
    }
    if (mix_capture_config.enabled) {
        if (!using_hp_mode()) {
            throw std::runtime_error(
                "sze_prediction_capture requires Shenzhen hp-shadow or hp-realtime mode");
        }
        if (!mMix153060Enabled) {
            throw std::runtime_error(
                "sze_prediction_capture requires a loaded model and valid daily static inputs");
        }
        mMix153060Capture.reset(new mix153060::Capture(mix_capture_config));
        if (!mMix153060Capture->ready()) {
            throw std::runtime_error("sze_prediction_capture initialization failed: " +
                                     mMix153060Capture->error());
        }
        KF_LOG_INFO(logger, "[SZ][prediction] capture enabled events="
                             << mMix153060Capture->events_path()
                             << " samples=" << mMix153060Capture->samples_path()
                             << " market_resolutions="
                             << mMix153060Capture->market_resolutions_path());
    }
    mTradeIndex=0;
}

StrategyBase::~StrategyBase() {
#ifdef T0_SZE_STRATEGY_ONLY
    stop_sze_recovery_consumer();
#endif
    if (mMix153060Capture.get() != 0) {
        mMix153060Capture->flush();
    }
    if (using_hp_mode()) {
        KF_LOG_INFO(logger, "[SZ][prediction] shutdown"
            << " enabled=" << BoolText(mMix153060Enabled)
            << " adapter_rejects=" << mMix153060AdapterRejectCount
            << " book_rejects=" << mMix153060BookRejectCount
            << " prediction_rejects=" << mMix153060PredictionRejectCount);
    }
    if (mSnapshotLegacy15Enabled) {
        KF_LOG_INFO(logger, "[SZSnapshotFallback] shutdown predictions="
            << mSnapshotLegacy15PredictionCount
            << " rejects=" << mSnapshotLegacy15RejectCount);
    }
    if (mSseHybridEnabled) {
        for (std::unordered_map<std::string, std::unique_ptr<SseHybridRuntimeState> >::iterator
                 it = mSseHybridStateMap.begin(); it != mSseHybridStateMap.end(); ++it) {
            if (it->second.get() != 0 && it->second->pending_tick_valid) {
                flush_sse_hybrid_tick(it->first, mSseHybridSnapshotSource, 0);
            }
        }
        KF_LOG_INFO(logger, "[SSEHybrid] shutdown snapshot_predictions="
            << mSseHybridSnapshotPredictionCount
            << " tick_predictions=" << mSseHybridTickPredictionCount
            << " rejects=" << mSseHybridRejectCount);
    }
    dump_full_orderbook_latency_summary();
}

bool StrategyBase::configure_sse_hybrid(const json& config) {
    if (mMarket != "SH") return false;
    const std::string model_type = config.value("model_type", std::string());
    if (model_type != "sse_hybrid_native") return false;

    const std::string tick_path = config.value("model_path", std::string());
    const std::string baseline_path = config.value(
        "snapshot_baseline_model_path", std::string());
    const std::string baseline_scaler = config.value(
        "snapshot_baseline_scaler_path", std::string());
    const std::string auction_path = config.value(
        "snapshot_auction59_model_path", std::string());
    const std::string auction_scaler = config.value(
        "snapshot_auction59_scaler_path", std::string());
    if (tick_path.empty() || baseline_path.empty() || baseline_scaler.empty() ||
        auction_path.empty() || auction_scaler.empty()) {
        throw std::runtime_error("SSE hybrid config requires all model/scaler paths");
    }
    std::string error;
    if (!mSseHybridModel.load(tick_path, baseline_path, baseline_scaler,
                             auction_path, auction_scaler, &error)) {
        throw std::runtime_error("SSE hybrid model load failed: " + error);
    }

    const std::string auction_factors_path = config.value(
        "snapshot_auction59_factors_path", std::string());
    if (!auction_factors_path.empty() &&
        !sse_auction59::load_csv(auction_factors_path, &mSseAuction59Factors,
                                 &error)) {
        throw std::runtime_error("SSE Auction59 factor load failed: " + error);
    }
    json::const_iterator source = config.find("snapshot_source_id");
    if (source != config.end() && source->is_number_integer()) {
        mSseHybridSnapshotSource = static_cast<short>(source->get<int>());
    }

    for (std::vector<std::string>::const_iterator it = mInstrumentVec.begin();
         it != mInstrumentVec.end(); ++it) {
        const std::string& code = *it;
        std::unique_ptr<SseHybridRuntimeState> state(
            new SseHybridRuntimeState(code));
        state->signal_view.reset(new MSMarketDataField(NewMixSignalView()));
        const InsParams& params = mInsParamsMap.at(code);
        sse_tick::DailyStaticMetadata metadata;
        metadata.date = static_cast<std::uint32_t>(params.Date);
        metadata.avg_amount = params.HistoryAmount;
        metadata.turnover_threshold = params.HistoryAmount / 8000.0;
        metadata.free_share = params.FreeShare;
        metadata.pre_close = params.Close;
        metadata.limit_price = params.HpUpperPrice > 0.0
                                   ? params.HpUpperPrice : params.Close * 1.10;
        metadata.stop_price = params.HpLowerPrice > 0.0
                                  ? params.HpLowerPrice : params.Close * 0.90;
        metadata.has_date = metadata.date != 0U;
        metadata.has_avg_amount = metadata.avg_amount > 0.0;
        metadata.has_turnover_threshold = metadata.turnover_threshold > 0.0;
        metadata.has_free_share = metadata.free_share > 0.0;
        metadata.has_pre_close = metadata.pre_close > 0.0;
        metadata.has_limit_price = metadata.limit_price > 0.0;
        metadata.has_stop_price = metadata.stop_price > 0.0;
        metadata.quality = "runtime_config";
        state->factor_state.set_static_metadata(metadata);
        std::map<std::string, std::vector<float> >::const_iterator auction =
            mSseAuction59Factors.find(code);
        if (auction != mSseAuction59Factors.end() && auction->second.size() == 59U) {
            state->auction59_factors = auction->second;
        } else {
            state->auction59_factors.assign(59U,
                std::numeric_limits<float>::quiet_NaN());
        }
        mSseHybridStateMap[code] = std::move(state);
    }
    mSseHybridEnabled = true;
    KF_LOG_INFO(logger, "[SSEHybrid] enabled=1 instruments="
        << mSseHybridStateMap.size() << " snapshot_source="
        << mSseHybridSnapshotSource << " tick_batch_gap_ns=100000"
        << " real_td_unchanged=1");
    return true;
}

SseHybridRuntimeState* StrategyBase::sse_hybrid_state_for(const std::string& code) {
    std::unordered_map<std::string, std::unique_ptr<SseHybridRuntimeState> >::iterator it =
        mSseHybridStateMap.find(code);
    if (it == mSseHybridStateMap.end() || it->second.get() == 0) return 0;
    return it->second.get();
}

bool StrategyBase::make_sse_tick_event(const LFL2OrderField& data,
                                       short source,
                                       std::uint64_t receive_ns,
                                       sse_live::TickEvent* event) const {
    if (event == 0) return false;
    const std::string code = NormalizeInstrumentId(data.InstrumentID);
    if (!sse_live::is_sse_stock(code) || data.ApplSeqNum <= 0 ||
        data.OrderKind[0] != 'B' && data.OrderKind[0] != 'S') return false;
    std::uint64_t exchange_time = 0;
    if (!ParseSseTimeMicros(data.OrderTime, &exchange_time)) return false;
    const std::uint64_t sequence = data.BizIndex > 0 ?
        static_cast<std::uint64_t>(data.BizIndex) :
        static_cast<std::uint64_t>(data.ApplSeqNum);
    if (sequence == 0 || SseRawPrice(data.Price) == 0 ||
        SseRawQuantity(data.Volume) == 0) return false;
    *event = sse_live::TickEvent();
    event->security_id = code;
    event->channel_no = source > 0 ? static_cast<std::uint32_t>(source) : 1U;
    event->provider_sequence = sequence;
    event->tick_index = sequence;
    event->app_seq_num = static_cast<std::uint64_t>(data.ApplSeqNum);
    event->time_of_day_micros = exchange_time;
    event->event_type = (data.OrdType[0] == 'D' || data.OrdType[0] == '4') ? 'D' : 'A';
    event->buy_order_no = data.OrderKind[0] == 'B'
                              ? static_cast<std::uint64_t>(data.OrderNo) : 0U;
    event->sell_order_no = data.OrderKind[0] == 'S'
                               ? static_cast<std::uint64_t>(data.OrderNo) : 0U;
    event->price_raw = static_cast<std::uint32_t>(SseRawPrice(data.Price));
    event->quantity_raw = SseRawQuantity(data.Volume);
    event->amount_raw = 0U;
    event->side = data.OrderKind[0] == 'B' ? 0 : 1;
    (void)receive_ns;
    return true;
}

bool StrategyBase::make_sse_tick_event(const LFL2TradeField& data,
                                       short source,
                                       std::uint64_t receive_ns,
                                       sse_live::TickEvent* event) const {
    if (event == 0) return false;
    const std::string code = NormalizeInstrumentId(data.InstrumentID);
    if (!sse_live::is_sse_stock(code) || data.ApplSeqNum <= 0) return false;
    std::uint64_t exchange_time = 0;
    if (!ParseSseTimeMicros(data.TradeTime, &exchange_time)) return false;
    const std::uint64_t sequence = data.BizIndex > 0 ?
        static_cast<std::uint64_t>(data.BizIndex) :
        static_cast<std::uint64_t>(data.ApplSeqNum);
    if (sequence == 0 || SseRawQuantity(data.Volume) == 0) return false;
    *event = sse_live::TickEvent();
    event->security_id = code;
    event->channel_no = source > 0 ? static_cast<std::uint32_t>(source) : 1U;
    event->provider_sequence = sequence;
    event->tick_index = sequence;
    event->app_seq_num = static_cast<std::uint64_t>(data.ApplSeqNum);
    event->time_of_day_micros = exchange_time;
    event->event_type = data.OrderKind[0] == '4' ? 'D' : 'T';
    event->buy_order_no = data.BidApplSeqNum > 0
                              ? static_cast<std::uint64_t>(data.BidApplSeqNum) : 0U;
    event->sell_order_no = data.OfferApplSeqNum > 0
                               ? static_cast<std::uint64_t>(data.OfferApplSeqNum) : 0U;
    event->price_raw = static_cast<std::uint32_t>(SseRawPrice(data.Price));
    event->quantity_raw = SseRawQuantity(data.Volume);
    event->amount_raw = SseRawQuantity(data.TurnOver);
    event->side = (data.OrderKind[0] == '4' && data.BidApplSeqNum == 0 &&
                   data.OfferApplSeqNum != 0) ? 1 : 0;
    (void)receive_ns;
    return true;
}

void StrategyBase::log_sse_prediction(const std::string& code,
                                        std::uint64_t exchange_us,
                                        const char* model_type,
                                        double prediction,
                                        bool selected,
                                        bool factor_complete,
                                        std::uint64_t factor_ns,
                                        std::uint64_t infer_ns,
                                        std::uint64_t strategy_ns) {
    std::ostringstream line;
    line << code << ',' << exchange_us << ','
         << sse_trading::local_time_of_day_micros() << ',' << model_type << ','
         << std::setprecision(9) << prediction << ',' << (selected ? 1 : 0) << ','
         << (factor_complete ? 1 : 0) << ',' << factor_ns << ',' << infer_ns
         << ',' << strategy_ns;
    mSsePredictionLog.enqueue(line.str());
}

void StrategyBase::flush_sse_hybrid_tick(const std::string& code,
                                         short source,
                                         long rcv_time) {
    SseHybridRuntimeState* state = sse_hybrid_state_for(code);
    if (state == 0 || !state->pending_tick_valid) return;
    state->pending_flush_scheduled = false;
    const sse_live::TickEvent event = state->pending_tick;
    state->pending_tick_valid = false;
    const std::uint64_t factor_started = SseMonotonicNowNs();
    const sse_tick::FactorRow factor_row = state->factor_state.build(
        state->order_book, event.time_of_day_micros);
    const std::uint64_t factor_ns = SseMonotonicNowNs() - factor_started;
    std::array<float, sse_model::kFeatureCount> factors = factor_row.values;
    sse_hybrid_model::Prediction output;
    std::string error;
    const std::uint64_t infer_started = SseMonotonicNowNs();
    const bool infer_ok = mSseHybridModel.on_tick(
        factors, "sse", event.time_of_day_micros, &state->model_state, &output,
        &error);
    const std::uint64_t infer_ns = SseMonotonicNowNs() - infer_started;
    if (!infer_ok) {
        ++mSseHybridRejectCount;
        KF_LOG_ERROR(logger, "[SSEHybrid][TickReject] instrument=" << code
            << " reason=" << error);
        return;
    }
    ++state->tick_sample_count;
    ++mSseHybridTickPredictionCount;
    if (output.selected) {
        std::uint64_t strategy_ns = 0U;
        if (factor_row.validity.complete && state->signal_view.get() != 0 &&
            can_dispatch_trading_signal()) {
            std::unordered_map<std::string, ZStrategy*>::iterator strategy_it =
                mZStrategyMap.find(code);
            if (strategy_it != mZStrategyMap.end() && strategy_it->second != 0) {
                const std::uint64_t strategy_started = SseMonotonicNowNs();
                strategy_it->second->on_signal(state->signal_view.get(),
                    output.selected_pred, source, rcv_time);
                strategy_ns = SseMonotonicNowNs() - strategy_started;
            }
        }
        log_sse_prediction(code, event.time_of_day_micros, "tick",
                           output.selected_pred, true,
                           factor_row.validity.complete, factor_ns, infer_ns,
                           strategy_ns);
    }
    if (state->tick_sample_count <= 3U || state->tick_sample_count % 1000U == 0U) {
        KF_LOG_INFO(logger, "[SSEHybrid][Tick] instrument=" << code
            << " source=" << output.selected_source
            << " prediction=" << output.tick_pred
            << " factor_complete=" << factor_row.validity.complete
            << " tick_index=" << event.tick_index);
    }
}

void StrategyBase::schedule_sse_hybrid_tick_flush(const std::string& code) {
    SseHybridRuntimeState* state = sse_hybrid_state_for(code);
    if (state == 0 || !state->pending_tick_valid ||
        state->pending_flush_scheduled || util == 0) {
        return;
    }
    state->pending_flush_scheduled = true;
    BLCallback callback = std::bind(
        &StrategyBase::flush_sse_hybrid_tick_if_quiet, this, code);
    const long due = static_cast<long>(
        state->pending_tick_receive_ns + 100000ULL);
    if (!util->insert_callback(due, callback)) {
        state->pending_flush_scheduled = false;
        KF_LOG_ERROR(logger, "[SSEHybrid][FlushScheduleReject] instrument=" << code);
    }
}

void StrategyBase::flush_sse_hybrid_tick_if_quiet(const std::string& code) {
    SseHybridRuntimeState* state = sse_hybrid_state_for(code);
    if (state == 0 || !state->pending_tick_valid) return;
    state->pending_flush_scheduled = false;
    const std::uint64_t now = SseMonotonicNowNs();
    const std::uint64_t due = state->pending_tick_receive_ns + 100000ULL;
    if (now <= due) {
        // The callback heap may run a little early.  Re-arm the same single
        // callback; a new market event does not allocate another callback.
        schedule_sse_hybrid_tick_flush(code);
        return;
    }
    flush_sse_hybrid_tick(code, state->pending_tick_source,
                          state->pending_tick_rcv_time);
}

void StrategyBase::process_sse_hybrid_order(const LFL2OrderField* data,
                                            short source,
                                            long rcv_time) {
    if (data == 0) return;
    const std::string code = NormalizeInstrumentId(data->InstrumentID);
    SseHybridRuntimeState* state = sse_hybrid_state_for(code);
    if (state == 0) return;
    const std::uint64_t now = SseMonotonicNowNs();
    if (state->pending_tick_valid && now > state->pending_tick_receive_ns + 100000ULL) {
        flush_sse_hybrid_tick(code, source, rcv_time);
    }
    sse_live::TickEvent event;
    if (!make_sse_tick_event(*data, source, now, &event)) {
        ++mSseHybridRejectCount;
        return;
    }
    const sse_tick::ApplyResult result = state->order_book.apply(event);
    if (!result.accepted || !result.sequence_healthy) {
        ++mSseHybridRejectCount;
        return;
    }
    state->pending_tick = event;
    state->pending_tick_valid = true;
    state->pending_tick_receive_ns = now;
    state->pending_tick_source = source;
    state->pending_tick_rcv_time = rcv_time;
    state->last_tick_receive_ns = now;
    schedule_sse_hybrid_tick_flush(code);
}

void StrategyBase::process_sse_hybrid_trade(const LFL2TradeField* data,
                                            short source,
                                            long rcv_time) {
    if (data == 0) return;
    const std::string code = NormalizeInstrumentId(data->InstrumentID);
    SseHybridRuntimeState* state = sse_hybrid_state_for(code);
    if (state == 0) return;
    const std::uint64_t now = SseMonotonicNowNs();
    if (state->pending_tick_valid && now > state->pending_tick_receive_ns + 100000ULL) {
        flush_sse_hybrid_tick(code, source, rcv_time);
    }
    sse_live::TickEvent event;
    if (!make_sse_tick_event(*data, source, now, &event)) {
        ++mSseHybridRejectCount;
        return;
    }
    const sse_tick::ApplyResult result = state->order_book.apply(event);
    if (!result.accepted || !result.sequence_healthy) {
        ++mSseHybridRejectCount;
        return;
    }
    state->pending_tick = event;
    state->pending_tick_valid = true;
    state->pending_tick_receive_ns = now;
    state->pending_tick_source = source;
    state->pending_tick_rcv_time = rcv_time;
    state->last_tick_receive_ns = now;
    schedule_sse_hybrid_tick_flush(code);
}

void StrategyBase::process_sse_hybrid_snapshot(const LFMarketDataField* data,
                                               short source,
                                               long rcv_time) {
    if (data == 0 || source != mSseHybridSnapshotSource) return;
    const std::string code = NormalizeInstrumentId(data->InstrumentID);
    SseHybridRuntimeState* state = sse_hybrid_state_for(code);
    if (state == 0) return;
    sse_live::Snapshot snapshot = {};
    snapshot.security_id = code;
    snapshot.channel_no = 0;
    snapshot.time_of_day_micros = SnapshotExchangeTimeMs(*data) * 1000ULL;
    snapshot.last_price = data->LastPrice;
    snapshot.pre_close_price = data->PreClosePrice;
    snapshot.open_price = data->OpenPrice;
    snapshot.high_price = data->HighestPrice;
    snapshot.low_price = data->LowestPrice;
    snapshot.volume = data->Volume;
    snapshot.turnover = data->Turnover;
    for (std::size_t level = 0; level < 5U; ++level) {
        snapshot.bid_prices[level] = data->aBidPrice[level];
        snapshot.bid_volumes[level] = data->aBidVolume[level];
        snapshot.ask_prices[level] = data->aAskPrice[level];
        snapshot.ask_volumes[level] = data->aAskVolume[level];
    }
    if (!sse_snapshot36::valid(snapshot)) return;
    if (state->has_previous_snapshot &&
        snapshot.time_of_day_micros <= state->previous_snapshot.time_of_day_micros) return;
    const sse_live::Snapshot previous = state->has_previous_snapshot
        ? state->previous_snapshot : snapshot;
    state->previous_snapshot = snapshot;
    state->has_previous_snapshot = true;
    if (!state->has_previous_snapshot || previous.time_of_day_micros == snapshot.time_of_day_micros)
        return;
    const std::uint64_t factor_started = SseMonotonicNowNs();
    const std::vector<float> factors = sse_snapshot36::build(previous, snapshot);
    const std::uint64_t factor_ns = SseMonotonicNowNs() - factor_started;
    std::vector<float> enhanced(factors);
    if (state->auction59_factors.size() == 59U) {
        enhanced.insert(enhanced.end(), state->auction59_factors.begin(),
                        state->auction59_factors.end());
    } else {
        enhanced.insert(enhanced.end(), 59U,
                        std::numeric_limits<float>::quiet_NaN());
    }
    sse_hybrid_model::Prediction output;
    std::string error;
    const std::uint64_t infer_started = SseMonotonicNowNs();
    const bool infer_ok = mSseHybridModel.on_snapshot(
        factors, enhanced, "sse", snapshot.time_of_day_micros,
        &state->model_state, &output, &error);
    const std::uint64_t infer_ns = SseMonotonicNowNs() - infer_started;
    if (!infer_ok) {
        ++mSseHybridRejectCount;
        return;
    }
    MSMarketData& view = state->signal_view->ms_market_data;
    view = MSMarketData();
    view.ms_market_data[InstrumentIDIndex] = std::strtod(code.c_str(), 0);
    view.ms_market_data[MarketTimeIndex] = MixMarketTimeValue(
        static_cast<std::int64_t>(snapshot.time_of_day_micros));
    view.ms_market_data[LastPriceIndex] = snapshot.last_price;
    view.ms_market_data[MidPriceIndex] =
        (snapshot.bid_prices[0] + snapshot.ask_prices[0]) * 0.5;
    view.ms_market_data[VolumeIndex] = snapshot.volume;
    view.ms_market_data[TurnoverIndex] = snapshot.turnover;
    for (std::size_t level = 0; level < 5U; ++level) {
        view.ms_market_data[BidPrice1Index + level] = snapshot.bid_prices[level];
        view.ms_market_data[AskPrice1Index + level] = snapshot.ask_prices[level];
        view.ms_market_data[BidVolume1Index + level] = snapshot.bid_volumes[level];
        view.ms_market_data[AskVolume1Index + level] = snapshot.ask_volumes[level];
    }
    ++mSseHybridSnapshotPredictionCount;
    if (output.selected) {
        std::uint64_t strategy_ns = 0U;
        if (can_dispatch_trading_signal()) {
            std::unordered_map<std::string, ZStrategy*>::iterator strategy_it =
                mZStrategyMap.find(code);
            if (strategy_it != mZStrategyMap.end() && strategy_it->second != 0) {
                const std::uint64_t strategy_started = SseMonotonicNowNs();
                strategy_it->second->on_signal(state->signal_view.get(),
                    output.selected_pred, source, rcv_time);
                strategy_ns = SseMonotonicNowNs() - strategy_started;
            }
        }
        log_sse_prediction(code, snapshot.time_of_day_micros, "snapshot",
                           output.selected_pred, true, true, factor_ns, infer_ns,
                           strategy_ns);
    }
    if (mSseHybridSnapshotPredictionCount <= 3U ||
        mSseHybridSnapshotPredictionCount % 1000U == 0U) {
        KF_LOG_INFO(logger, "[SSEHybrid][Snapshot] instrument=" << code
            << " prediction=" << output.snapshot_pred
            << " selected=" << output.selected
            << " exchange_time_us=" << snapshot.time_of_day_micros);
    }
}

bool StrategyBase::can_dispatch_trading_signal() const {
    if (!is_risk_data_ready()) {
        return false;
    }
#ifdef T0_SZE_STRATEGY_ONLY
    if (mSzeRecoveryAnalysisMode) {
        return false;
    }
    if (mSzeRecoveryConsumerConfig.enabled) {
        return mSzeRecoveryLiveReady.load(std::memory_order_acquire) &&
               mSzeRecoveryContinuityValid.load(std::memory_order_acquire) &&
               mSzeTradingQueueHealthy.load(std::memory_order_acquire) &&
               !mSzeRecoveryReplayContext.load(std::memory_order_acquire);
    }
#endif
    return true;
}

#ifdef T0_SZE_STRATEGY_ONLY
void StrategyBase::parse_sze_recovery_consumer_config(const json& config) {
    json::const_iterator root = config.find("sze_recovery_consumer");
    if (root == config.end()) {
        return;
    }
    if (!root->is_object()) {
        throw std::runtime_error("sze_recovery_consumer must be an object");
    }
    const json& value = *root;
    json::const_iterator item = value.find("enabled");
    mSzeRecoveryConsumerConfig.enabled =
        item != value.end() && item->is_boolean() && item->get<bool>();
    item = value.find("allow_invalid_replay_for_analysis");
    if (item != value.end() && !item->is_boolean()) {
        throw std::runtime_error(
            "allow_invalid_replay_for_analysis must be boolean");
    }
    mSzeRecoveryConsumerConfig.allow_invalid_replay_for_analysis =
        item != value.end() && item->is_boolean() && item->get<bool>();
    item = value.find("trading_enabled");
    if (item != value.end() && !item->is_boolean()) {
        throw std::runtime_error("trading_enabled must be boolean");
    }
    mSzeRecoveryConsumerConfig.trading_enabled =
        item != value.end() && item->is_boolean() && item->get<bool>();
    if (!mSzeRecoveryConsumerConfig.enabled) {
        if (mSzeRecoveryConsumerConfig.allow_invalid_replay_for_analysis) {
            throw std::runtime_error(
                "allow_invalid_replay_for_analysis requires enabled recovery consumer");
        }
        return;
    }

    if (mSzeRecoveryConsumerConfig.trading_enabled && !using_hp_realtime_mode()) {
        throw std::runtime_error("recovery trading requires hp-realtime mode");
    }

    if (!using_hp_mode()) {
        throw std::runtime_error("sze_recovery_consumer requires Shenzhen HP mode");
    }
    item = value.find("trading_day");
    if (item != value.end() && item->is_number()) {
        mSzeRecoveryConsumerConfig.trading_day = item->get<std::uint32_t>();
    }
    item = value.find("source_id");
    if (item != value.end() && item->is_number()) {
        mSzeRecoveryConsumerConfig.source_id = item->get<std::uint32_t>();
    }
    item = value.find("journal_directory");
    if (item != value.end() && item->is_string()) {
        mSzeRecoveryConsumerConfig.journal_directory = item->get<std::string>();
    }
    item = value.find("journal_prefix");
    if (item != value.end() && item->is_string()) {
        mSzeRecoveryConsumerConfig.journal_prefix = item->get<std::string>();
    }
    std::uint64_t segment_mb = 1024U;
    item = value.find("journal_segment_mb");
    if (item != value.end() && item->is_number()) {
        segment_mb = item->get<std::uint64_t>();
    }
    mSzeRecoveryConsumerConfig.journal_segment_bytes =
        segment_mb * 1024ULL * 1024ULL;
    item = value.find("journal_segment_bytes");
    if (item != value.end() && item->is_number()) {
        mSzeRecoveryConsumerConfig.journal_segment_bytes =
            item->get<std::uint64_t>();
    }
    item = value.find("journal_max_payload_bytes");
    if (item != value.end() && item->is_number()) {
        mSzeRecoveryConsumerConfig.journal_max_payload_bytes =
            item->get<std::uint32_t>();
    }
    item = value.find("shm_path");
    if (item != value.end() && item->is_string()) {
        mSzeRecoveryConsumerConfig.shm_path = item->get<std::string>();
    }
    item = value.find("state_cpu");
    if (item != value.end() && item->is_number()) {
        mSzeRecoveryConsumerConfig.state_cpu = item->get<int>();
    }
    item = value.find("strategy_cpu");
    if (item != value.end() && item->is_number()) {
        mSzeRecoveryConsumerConfig.strategy_cpu = item->get<int>();
    }
    item = value.find("health_state_enabled");
    if (item != value.end() && !item->is_boolean()) {
        throw std::runtime_error("health_state_enabled must be boolean");
    }
    mSzeRecoveryConsumerConfig.health_state_enabled =
        item != value.end() && item->is_boolean() && item->get<bool>();
    item = value.find("shard_id");
    if (item != value.end() && item->is_number()) {
        mSzeRecoveryConsumerConfig.shard_id = item->get<std::uint32_t>();
    }
    item = value.find("shard_count");
    if (item != value.end() && item->is_number()) {
        mSzeRecoveryConsumerConfig.shard_count = item->get<std::uint32_t>();
    }
    item = value.find("health_state_path");
    if (item != value.end() && item->is_string()) {
        mSzeRecoveryConsumerConfig.health_state_path = item->get<std::string>();
    }
    if (mSzeRecoveryConsumerConfig.health_state_enabled &&
        mSzeRecoveryConsumerConfig.health_state_path.empty()) {
        mSzeRecoveryConsumerConfig.health_state_path =
            sze_health::shard_health_path(
                mSzeRecoveryConsumerConfig.shm_path,
                mSzeRecoveryConsumerConfig.shard_id);
    }

    json::const_iterator md_sources = config.find("md_source_index");
    if ((md_sources != config.end() && md_sources->is_array() &&
         !md_sources->empty()) ||
        mSzeRecoveryConsumerConfig.trading_day < 20000101U ||
        mSzeRecoveryConsumerConfig.trading_day > 99991231U ||
        mSzeRecoveryConsumerConfig.source_id != 88U ||
        mSzeRecoveryConsumerConfig.journal_directory.empty() ||
        mSzeRecoveryConsumerConfig.journal_prefix.empty() ||
        mSzeRecoveryConsumerConfig.journal_segment_bytes < 8192U ||
        mSzeRecoveryConsumerConfig.journal_max_payload_bytes <
            sizeof(sze_md::SzeHpfOrder) ||
        mSzeRecoveryConsumerConfig.journal_max_payload_bytes > 65535U ||
        (!mSzeRecoveryConsumerConfig.allow_invalid_replay_for_analysis &&
         mSzeRecoveryConsumerConfig.shm_path.empty()) ||
        mSzeRecoveryConsumerConfig.state_cpu < 0 ||
        mSzeRecoveryConsumerConfig.state_cpu >= CPU_SETSIZE ||
        mSzeRecoveryConsumerConfig.strategy_cpu < 0 ||
        mSzeRecoveryConsumerConfig.strategy_cpu >= CPU_SETSIZE ||
        mSzeRecoveryConsumerConfig.state_cpu ==
            mSzeRecoveryConsumerConfig.strategy_cpu ||
        (mSzeRecoveryConsumerConfig.health_state_enabled &&
         (mSzeRecoveryConsumerConfig.shard_count == 0U ||
          mSzeRecoveryConsumerConfig.shard_id >=
              mSzeRecoveryConsumerConfig.shard_count ||
          mSzeRecoveryConsumerConfig.health_state_path.empty()))) {
        throw std::runtime_error("invalid sze_recovery_consumer configuration");
    }
    for (std::unordered_map<std::string, InsParams>::const_iterator it =
             mInsParamsMap.begin(); it != mInsParamsMap.end(); ++it) {
        if (it->second.Date != static_cast<int32_t>(
                mSzeRecoveryConsumerConfig.trading_day)) {
            throw std::runtime_error(
                "sze_recovery_consumer trading day does not match instrument statics");
        }
    }
}

void StrategyBase::start_sze_recovery_consumer() {
    if (!mSzeRecoveryConsumerConfig.enabled) {
        return;
    }
    if (mSzeRecoveryConsumerRunning.exchange(true, std::memory_order_acq_rel)) {
        KF_LOG_INFO(logger, "[SZRecovery] start skipped already_running=1");
        return;
    }
    KF_LOG_INFO(logger, "[SZRecovery] start requested"
        << " trading_day=" << mSzeRecoveryConsumerConfig.trading_day
        << " state_cpu=" << mSzeRecoveryConsumerConfig.state_cpu
        << " journal=" << mSzeRecoveryConsumerConfig.journal_directory
        << " shm=" << mSzeRecoveryConsumerConfig.shm_path
        << " capture_only=" << BoolText(mMix153060CaptureOnly));
    mSzeRecoveryConsumerEntered.store(false, std::memory_order_release);
    mSzeRecoveryConsumerAttached.store(false, std::memory_order_release);
    mSzeTradingSignalHead.store(0U, std::memory_order_relaxed);
    mSzeTradingSignalTail.store(0U, std::memory_order_relaxed);
    mSzeSnapshotSignalHead.store(0U, std::memory_order_relaxed);
    mSzeSnapshotSignalTail.store(0U, std::memory_order_relaxed);
    mSzePredictionSignalStateMap.clear();
    mSzeTradingQueueHealthy.store(true, std::memory_order_release);
    mSzeRecoveryReplayContext.store(true, std::memory_order_release);
    mSzeRecoveryContinuityValid.store(false, std::memory_order_release);
    mSzeRecoveryLiveReady.store(false, std::memory_order_release);
    mSzeTradingPollRunning.store(!mSzeRecoveryAnalysisMode && !mMix153060CaptureOnly,
                                 std::memory_order_release);
    try {
        if (!mSzeRecoveryAnalysisMode && !mMix153060CaptureOnly) {
            mSzeTradingPollThread = std::thread(
                &StrategyBase::sze_trading_poll_loop, this);
        }
        mSzeRecoveryConsumerThread = std::thread(
            &StrategyBase::sze_recovery_consumer_loop, this);
    } catch (...) {
        mSzeRecoveryConsumerRunning.store(false, std::memory_order_release);
        mSzeTradingPollRunning.store(false, std::memory_order_release);
        if (mSzeTradingPollThread.joinable()) {
            mSzeTradingPollThread.join();
        }
        throw;
    }
    for (int attempt = 0;
         attempt < 10000 &&
         !(mSzeRecoveryAnalysisMode
               ? mSzeRecoveryConsumerEntered.load(std::memory_order_acquire)
               : mSzeRecoveryConsumerAttached.load(std::memory_order_acquire));
         ++attempt) {
        const struct timespec delay = {0, 1000000L};
        (void)::nanosleep(&delay, 0);
    }
    const bool startup_ready = mSzeRecoveryAnalysisMode
        ? mSzeRecoveryConsumerEntered.load(std::memory_order_acquire)
        : mSzeRecoveryConsumerAttached.load(std::memory_order_acquire);
    if (!startup_ready) {
        KF_LOG_ERROR(logger, "[SZRecovery] consumer attach timeout");
        std::cerr << "[SZRecovery] consumer_attached=0 timeout_ms=10000" << '\n';
        mSzeRecoveryConsumerRunning.store(false, std::memory_order_release);
        if (mSzeRecoveryConsumerThread.joinable()) {
            mSzeRecoveryConsumerThread.join();
        }
    }
}

void StrategyBase::stop_sze_recovery_consumer() {
    mSzeRecoveryConsumerRunning.store(false, std::memory_order_release);
    mSzeTradingPollRunning.store(false, std::memory_order_release);
    if (mSzeRecoveryConsumerThread.joinable()) {
        mSzeRecoveryConsumerThread.join();
    }
    if (mSzeTradingPollThread.joinable()) {
        mSzeTradingPollThread.join();
    }
    mSzeRecoveryReplayContext.store(false, std::memory_order_release);
    mSzeRecoveryLiveReady.store(false, std::memory_order_release);
    mSzeRecoveryClockMappingValid.store(false, std::memory_order_release);
}

bool StrategyBase::sze_recovery_consumer_started() const {
    return !mSzeRecoveryConsumerConfig.enabled ||
           (mSzeRecoveryConsumerRunning.load(std::memory_order_acquire) &&
            (mSzeRecoveryAnalysisMode
                 ? mSzeRecoveryConsumerEntered.load(std::memory_order_acquire)
                 : mSzeRecoveryConsumerAttached.load(std::memory_order_acquire)));
}

void StrategyBase::initialize_sze_recovery_clock_mapping() {
    const std::uint64_t mono_ns = sze_recovery::monotonic_time_ns();
    const std::uint64_t realtime_ns = sze_recovery::realtime_ns();
    if (mono_ns == 0U || realtime_ns == 0U) {
        mSzeRecoveryClockMappingValid.store(false, std::memory_order_release);
        KF_LOG_ERROR(logger, "[SZRecovery] unable to initialize monotonic/realtime mapping");
        return;
    }
    mSzeRecoveryClockReferenceMonoNs.store(mono_ns, std::memory_order_release);
    mSzeRecoveryClockReferenceRealtimeNs.store(realtime_ns, std::memory_order_release);
    mSzeRecoveryClockMappingValid.store(true, std::memory_order_release);
    KF_LOG_INFO(logger, "[SZRecovery] receive clock mapping initialized"
        << " mono_ns=" << mono_ns << " realtime_ns=" << realtime_ns);
}

bool StrategyBase::process_sze_recovery_event(
    const sze_recovery::CanonicalEvent& event,
    const void* payload,
    std::size_t payload_size) {
    if (payload == 0 || payload_size == 0U) {
        mSzeRecoveryDecodeErrors.fetch_add(1U, std::memory_order_relaxed);
        return false;
    }
    LFL2OrderField order;
    LFL2TradeField trade;
    std::memset(&order, 0, sizeof(order));
    std::memset(&trade, 0, sizeof(trade));
    const sze_md::DecodeStatus decoded = sze_md::decode_recovery_record(
        event, payload, payload_size, &order, &trade);
    long receive_time = 0;
    if (mSzeRecoveryClockMappingValid.load(std::memory_order_acquire)) {
        const int64_t local_time_us = mix153060::recover_monotonic_receive_time_us(
            event.receive_mono_ns,
            mSzeRecoveryClockReferenceMonoNs.load(std::memory_order_acquire),
            mSzeRecoveryClockReferenceRealtimeNs.load(std::memory_order_acquire),
            static_cast<int32_t>(event.trading_day), 0);
        if (local_time_us > 0) {
            receive_time = static_cast<long>(local_time_us);
        }
    }
    mSzeCurrentRecoveryEventId = event.event_id;
    if (decoded == sze_md::DecodeStatus::kOrder) {
        if (mInsParamsMap.find(NormalizeInstrumentId(order.InstrumentID)) ==
            mInsParamsMap.end()) {
            return true;
        }
        process_l2_order_event(&order,
                               static_cast<short>(event.source_id),
                               receive_time);
    } else if (decoded == sze_md::DecodeStatus::kExecution) {
        if (mInsParamsMap.find(NormalizeInstrumentId(trade.InstrumentID)) ==
            mInsParamsMap.end()) {
            return true;
        }
        process_l2_trade_event(&trade,
                               static_cast<short>(event.source_id),
                               receive_time);
    } else {
        mSzeRecoveryDecodeErrors.fetch_add(1U, std::memory_order_relaxed);
        KF_LOG_ERROR(logger, "[SZRecovery] persisted record decode failed"
            << " event_id=" << event.event_id
            << " decode_status=" << static_cast<int>(decoded));
        return false;
    }
    mSzeRecoveryEvents.fetch_add(1U, std::memory_order_relaxed);
    return true;
}

void StrategyBase::sze_invalid_analysis_replay_loop() {
    // Journal-only analysis can span a reboot, so its monotonic timestamps
    // cannot be mapped to realtime safely. The adapter falls back to exchange
    // time and keeps this mode explicitly non-trading.
    mSzeRecoveryClockMappingValid.store(false, std::memory_order_release);
    sze_recovery::JournalConfig journal_config;
    journal_config.directory = mSzeRecoveryConsumerConfig.journal_directory;
    journal_config.prefix = mSzeRecoveryConsumerConfig.journal_prefix;
    journal_config.trading_day = mSzeRecoveryConsumerConfig.trading_day;
    journal_config.source_id = mSzeRecoveryConsumerConfig.source_id;
    journal_config.segment_bytes =
        mSzeRecoveryConsumerConfig.journal_segment_bytes;
    journal_config.max_payload_bytes =
        mSzeRecoveryConsumerConfig.journal_max_payload_bytes;

    sze_recovery::JournalReader reader;
    const sze_recovery::JournalOpenResult opened = reader.open(journal_config);
    if (opened.status != sze_recovery::kJournalOk) {
        KF_LOG_ERROR(logger, "[SZRecoveryAnalysis] failed to open journal"
            << " status=" << static_cast<int>(opened.status)
            << " directory=" << journal_config.directory
            << " prefix=" << journal_config.prefix);
        mSzeTradingPollRunning.store(false, std::memory_order_release);
        mSzeRecoveryConsumerRunning.store(false, std::memory_order_release);
        return;
    }
    KF_LOG_INFO(logger, "[SZRecoveryAnalysis] journal opened"
        << " trading_day=" << journal_config.trading_day
        << " continuity=" << static_cast<int>(opened.continuity_state)
        << " invalid_reason=" << static_cast<int>(opened.invalid_reason)
        << " last_event_id=" << opened.last_event_id
        << " unclean_restart=" << (opened.unclean_restart ? 1 : 0));

    std::vector<unsigned char> raw(journal_config.max_payload_bytes);
    bool completed = false;
    while (mSzeRecoveryConsumerRunning.load(std::memory_order_acquire)) {
        sze_recovery::CanonicalEvent event;
        const sze_recovery::JournalStatus status = reader.next(
            &event, raw.data(), raw.size());
        if (status == sze_recovery::kJournalEnd ||
            status == sze_recovery::kJournalWouldBlock) {
            completed = true;
            KF_LOG_INFO(logger, "[SZRecoveryAnalysis] journal replay reached"
                << " committed end status=" << static_cast<int>(status));
            break;
        }
        if (status != sze_recovery::kJournalOk) {
            KF_LOG_ERROR(logger, "[SZRecoveryAnalysis] journal replay failed"
                << " status=" << static_cast<int>(status)
                << " next_event_id=" << reader.next_event_id());
            break;
        }
        mSzeRecoveryReplayContext.store(true, std::memory_order_release);
        mSzeRecoveryContinuityValid.store(false, std::memory_order_release);
        mSzeRecoveryLiveReady.store(false, std::memory_order_release);
        if (!process_sze_recovery_event(event, raw.data(), event.payload_size)) {
            break;
        }
    }
    reader.close();
    mSzeRecoveryContinuityValid.store(false, std::memory_order_release);
    mSzeRecoveryLiveReady.store(false, std::memory_order_release);
    mSzeTradingPollRunning.store(false, std::memory_order_release);
    mSzeRecoveryConsumerRunning.store(false, std::memory_order_release);
    KF_LOG_INFO(logger, "[SZRecoveryAnalysis] replay stopped"
        << " completed=" << (completed ? 1 : 0)
        << " events=" << mSzeRecoveryEvents.load(std::memory_order_relaxed)
        << " decode_errors="
        << mSzeRecoveryDecodeErrors.load(std::memory_order_relaxed)
        << " continuity_valid=0 readiness=0");
}

void StrategyBase::sze_recovery_consumer_loop() {
    mSzeRecoveryConsumerEntered.store(true, std::memory_order_release);
    if (mSzeRecoveryAnalysisMode) {
        KF_LOG_INFO(logger, "[SZRecoveryAnalysis] mode=journal-only"
            << " shm_handoff=0 producer_required=0 readiness=0");
        sze_invalid_analysis_replay_loop();
        return;
    }

    cpu_set_t cpu_set;
    CPU_ZERO(&cpu_set);
    CPU_SET(mSzeRecoveryConsumerConfig.state_cpu, &cpu_set);
    const int affinity_status = ::pthread_setaffinity_np(
        ::pthread_self(), sizeof(cpu_set), &cpu_set);
    if (affinity_status != 0) {
        KF_LOG_ERROR(logger, "[SZRecovery] failed to bind state thread; continuing unpinned"
            << " cpu=" << mSzeRecoveryConsumerConfig.state_cpu
            << " status=" << affinity_status);
    }

    sze_recovery::JournalConfig journal_config;
    journal_config.directory = mSzeRecoveryConsumerConfig.journal_directory;
    journal_config.prefix = mSzeRecoveryConsumerConfig.journal_prefix;
    journal_config.trading_day = mSzeRecoveryConsumerConfig.trading_day;
    journal_config.source_id = mSzeRecoveryConsumerConfig.source_id;
    journal_config.segment_bytes =
        mSzeRecoveryConsumerConfig.journal_segment_bytes;
    journal_config.max_payload_bytes =
        mSzeRecoveryConsumerConfig.journal_max_payload_bytes;

    sze_recovery::ReplayHandoffConsumer consumer;
    bool open_logged = false;
    sze_recovery::ReplayOpenStatus last_open_status =
        sze_recovery::kReplayOpenNotAttempted;
    while (mSzeRecoveryConsumerRunning.load(std::memory_order_acquire) &&
           !consumer.open(journal_config, mSzeRecoveryConsumerConfig.shm_path,
                          !mSzeRecoveryConsumerConfig.health_state_enabled)) {
        if (consumer.last_open_status() != last_open_status) {
            last_open_status = consumer.last_open_status();
            std::cerr << "[SZRecovery] consumer_attached=0 open_status="
                      << static_cast<int>(last_open_status)
                      << " journal=" << mSzeRecoveryConsumerConfig.journal_directory
                      << " prefix=" << mSzeRecoveryConsumerConfig.journal_prefix
                      << " shm=" << mSzeRecoveryConsumerConfig.shm_path << '\n';
        }
        if (!open_logged) {
            KF_LOG_INFO(logger, "[SZRecovery] waiting for capture journal/shm");
            open_logged = true;
        }
        struct timespec retry_delay = {0, 100000000L};
        (void)::nanosleep(&retry_delay, 0);
    }
    if (!mSzeRecoveryConsumerRunning.load(std::memory_order_acquire)) {
        return;
    }
    mSzeRecoveryConsumerAttached.store(true, std::memory_order_release);
    std::cerr << "[SZRecovery] consumer_attached=1"
              << " journal=" << mSzeRecoveryConsumerConfig.journal_directory
              << " shm=" << mSzeRecoveryConsumerConfig.shm_path << '\n';
    KF_LOG_INFO(logger, "[SZRecovery] consumer attached"
        << " trading_day=" << mSzeRecoveryConsumerConfig.trading_day
        << " state_cpu=" << mSzeRecoveryConsumerConfig.state_cpu
        << " journal=" << mSzeRecoveryConsumerConfig.journal_directory
        << " shm=" << mSzeRecoveryConsumerConfig.shm_path);
    initialize_sze_recovery_clock_mapping();
    if (mSzeRecoveryConsumerConfig.health_state_enabled) {
        std::vector<std::uint32_t> health_symbols;
        health_symbols.reserve(mInsParamsMap.size());
        for (std::unordered_map<std::string, InsParams>::const_iterator item =
                 mInsParamsMap.begin(); item != mInsParamsMap.end(); ++item) {
            const std::uint32_t symbol_id =
                sze_health::parse_symbol_id(item->first.c_str());
            if (symbol_id < 1000000U) health_symbols.push_back(symbol_id);
        }
        std::unique_ptr<sze_health::RecoveryShardHealthWriter> writer(
            new sze_health::RecoveryShardHealthWriter());
        if (!writer->create(
                mSzeRecoveryConsumerConfig.health_state_path,
                mSzeRecoveryConsumerConfig.trading_day,
                mSzeRecoveryConsumerConfig.source_id,
                consumer.generation(),
                mSzeRecoveryConsumerConfig.shard_id,
                mSzeRecoveryConsumerConfig.shard_count,
                health_symbols, true)) {
            KF_LOG_ERROR(logger, "[SZRecovery] failed to create shard health page"
                << " path=" << mSzeRecoveryConsumerConfig.health_state_path);
            consumer.close();
            mSzeRecoveryConsumerAttached.store(false, std::memory_order_release);
            mSzeRecoveryConsumerRunning.store(false, std::memory_order_release);
            return;
        }
        mSzeRecoveryHealthWriter = std::move(writer);
        mSzeRecoveryHealthWriter->publish_shard(
            sze_recovery::kReadinessReplaying, sze_health::kHealthHealthy,
            0U, consumer.next_event_id() > 0U ? consumer.next_event_id() - 1U : 0U,
            0U, consumer.latest_feed_sequence(), consumer.replay_lag(),
            0U, consumer.ring_overruns(), 0U);
    }
    mSzeRecoveryContinuityValid.store(
        consumer.mode() != sze_recovery::kReplayInvalid,
        std::memory_order_release);

    std::vector<unsigned char> raw(
        mSzeRecoveryConsumerConfig.journal_max_payload_bytes);
    const std::uint64_t recovery_start_ns = sze_recovery::monotonic_time_ns();
    std::uint64_t last_metric_ns = recovery_start_ns;
    std::uint64_t last_metric_events = 0U;
    bool live_handoff_logged = false;
    const auto publish_recovery_metrics = [&]() {
        const std::uint64_t now_ns = sze_recovery::monotonic_time_ns();
        if (now_ns < last_metric_ns + 1000000000ULL) {
            return;
        }
        const std::uint64_t events =
            mSzeRecoveryEvents.load(std::memory_order_relaxed);
        const std::uint64_t elapsed_ns = now_ns - last_metric_ns;
        const std::uint64_t rate_milli = elapsed_ns > 0U
            ? ((events - last_metric_events) * 1000000000ULL /
               elapsed_ns) * 1000ULL
            : 0U;
        consumer.publish_metrics(
            rate_milli, (now_ns - recovery_start_ns) / 1000000ULL);
        if (mSzeRecoveryHealthWriter) {
            const sze_recovery::ReadinessState readiness =
                consumer.mode() == sze_recovery::kReplayLive
                    ? sze_recovery::kReadinessLiveReady
                    : (consumer.mode() == sze_recovery::kReplayHandoff
                       ? sze_recovery::kReadinessHandoff
                       : sze_recovery::kReadinessReplaying);
            mSzeRecoveryHealthWriter->publish_shard(
                readiness,
                consumer.mode() == sze_recovery::kReplayInvalid
                    ? sze_health::kHealthFailed : sze_health::kHealthHealthy,
                0U,
                consumer.next_event_id() > 0U
                    ? consumer.next_event_id() - 1U : 0U,
                events, consumer.latest_feed_sequence(), consumer.replay_lag(),
                rate_milli, consumer.ring_overruns(), events);
        }
        last_metric_ns = now_ns;
        last_metric_events = events;
    };
    while (mSzeRecoveryConsumerRunning.load(std::memory_order_acquire)) {
        sze_recovery::CanonicalEvent event;
        const sze_recovery::ReplayReadStatus status = consumer.next(
            &event, raw.data(), raw.size());
        if (status == sze_recovery::kReplayReadWouldBlock) {
            if (consumer.mode() == sze_recovery::kReplayLive) {
                mSzeRecoveryReplayContext.store(false, std::memory_order_release);
                mSzeRecoveryContinuityValid.store(true, std::memory_order_release);
                mSzeRecoveryLiveReady.store(true, std::memory_order_release);
                if (!live_handoff_logged) {
                    live_handoff_logged = true;
                    KF_LOG_INFO(logger, "[SZRecovery] live handoff ready"
                        << " events="
                        << mSzeRecoveryEvents.load(std::memory_order_relaxed)
                        << " next_event_id=" << consumer.next_event_id()
                        << " routing_ready=" << BoolText(is_risk_data_ready())
                        << " idle=1");
                }
            }
            publish_recovery_metrics();
            _mm_pause();
            continue;
        }
        if (status != sze_recovery::kReplayReadEvent) {
            mSzeRecoveryReplayContext.store(false, std::memory_order_release);
            mSzeRecoveryContinuityValid.store(false, std::memory_order_release);
            mSzeRecoveryLiveReady.store(false, std::memory_order_release);
            KF_LOG_ERROR(logger, "[SZRecovery] consumer invalid"
                << " status=" << static_cast<int>(status)
                << " next_event_id=" << consumer.next_event_id());
            if (mSzeRecoveryHealthWriter) {
                mSzeRecoveryHealthWriter->publish_shard(
                    sze_recovery::kReadinessNotReady,
                    sze_health::kHealthFailed,
                    static_cast<std::uint32_t>(status),
                    consumer.next_event_id() > 0U
                        ? consumer.next_event_id() - 1U : 0U,
                    mSzeRecoveryEvents.load(std::memory_order_relaxed),
                    consumer.latest_feed_sequence(), consumer.replay_lag(),
                    0U, consumer.ring_overruns(),
                    mSzeRecoveryEvents.load(std::memory_order_relaxed));
            }
            // Invalid data is observed rather than traded. Avoid burning a core
            // after a terminal continuity/ring failure while capture continues.
            struct timespec invalid_delay = {0, 10000000L};
            while (mSzeRecoveryConsumerRunning.load(std::memory_order_acquire)) {
                (void)::nanosleep(&invalid_delay, 0);
            }
            break;
        }

        const bool live = consumer.mode() == sze_recovery::kReplayLive;
        mSzeRecoveryReplayContext.store(!live, std::memory_order_release);
        mSzeRecoveryContinuityValid.store(true, std::memory_order_release);
        mSzeRecoveryLiveReady.store(live, std::memory_order_release);
        if (live && !live_handoff_logged) {
            live_handoff_logged = true;
            KF_LOG_INFO(logger, "[SZRecovery] live handoff ready"
                << " events=" << mSzeRecoveryEvents.load(std::memory_order_relaxed)
                << " next_event_id=" << consumer.next_event_id()
                << " routing_ready=" << BoolText(is_risk_data_ready()));
        }

        if (!process_sze_recovery_event(event, raw.data(), event.payload_size)) {
            mSzeRecoveryContinuityValid.store(false, std::memory_order_release);
            mSzeRecoveryLiveReady.store(false, std::memory_order_release);
            break;
        }
        publish_recovery_metrics();
    }
    consumer.close();
    if (mSzeRecoveryHealthWriter) {
        mSzeRecoveryHealthWriter->publish_shard(
            sze_recovery::kReadinessNotReady, sze_health::kHealthFailed,
            static_cast<std::uint32_t>(sze_recovery::kInvalidReceiverStopped),
            0U, mSzeRecoveryEvents.load(std::memory_order_relaxed), 0U, 0U, 0U,
            0U, mSzeRecoveryEvents.load(std::memory_order_relaxed));
        mSzeRecoveryHealthWriter->close();
        mSzeRecoveryHealthWriter.reset();
    }
    mSzeRecoveryConsumerAttached.store(false, std::memory_order_release);
    KF_LOG_INFO(logger, "[SZRecovery] consumer stopped"
        << " events=" << mSzeRecoveryEvents.load(std::memory_order_relaxed)
        << " decode_errors="
        << mSzeRecoveryDecodeErrors.load(std::memory_order_relaxed)
        << " replayed=" << consumer.replayed_events()
        << " live=" << consumer.live_events()
        << " handoff_retries=" << consumer.handoff_retries()
        << " ring_overruns=" << consumer.ring_overruns());
}

bool StrategyBase::enqueue_sze_trading_signal(
    const std::string& code,
    const MSMarketDataField* market_data,
    double prediction,
    short source,
    long receive_time,
    sze_prediction::Source prediction_source,
    std::uint32_t trading_day,
    std::uint64_t exchange_time_us) {
    if (market_data == 0 || code.empty() || code.size() >= 16U) {
        return false;
    }
    const bool snapshot = prediction_source == sze_prediction::kSnapshot;
    std::atomic<std::uint64_t>& head_counter = snapshot
        ? mSzeSnapshotSignalHead : mSzeTradingSignalHead;
    std::atomic<std::uint64_t>& tail_counter = snapshot
        ? mSzeSnapshotSignalTail : mSzeTradingSignalTail;
    std::array<SzeTradingSignal, kSzeTradingSignalCapacity>& slots = snapshot
        ? mSzeSnapshotSignalSlots : mSzeTradingSignalSlots;
    const std::uint64_t head = head_counter.load(std::memory_order_relaxed);
    const std::uint64_t tail = tail_counter.load(std::memory_order_acquire);
    if (head - tail >= kSzeTradingSignalCapacity) {
        return false;
    }
    SzeTradingSignal& slot = slots[
        head & (kSzeTradingSignalCapacity - 1U)];
    slot.market_data = market_data->ms_market_data.ms_market_data;
    std::memset(slot.instrument, 0, sizeof(slot.instrument));
    std::memcpy(slot.instrument, code.data(), code.size());
    slot.prediction = prediction;
    slot.source = source;
    slot.receive_time = receive_time;
    slot.prediction_source = prediction_source;
    slot.trading_day = trading_day;
    slot.exchange_time_us = exchange_time_us;
    slot.turnover = market_data->Turnover;
    slot.queue_sequence = head + 1U;
    head_counter.store(head + 1U, std::memory_order_release);
    return true;
}

bool StrategyBase::dequeue_sze_trading_signal(
    sze_prediction::Source prediction_source,
    SzeTradingSignal* signal) {
    if (signal == 0) {
        return false;
    }
    const bool snapshot = prediction_source == sze_prediction::kSnapshot;
    std::atomic<std::uint64_t>& head_counter = snapshot
        ? mSzeSnapshotSignalHead : mSzeTradingSignalHead;
    std::atomic<std::uint64_t>& tail_counter = snapshot
        ? mSzeSnapshotSignalTail : mSzeTradingSignalTail;
    std::array<SzeTradingSignal, kSzeTradingSignalCapacity>& slots = snapshot
        ? mSzeSnapshotSignalSlots : mSzeTradingSignalSlots;
    const std::uint64_t tail = tail_counter.load(std::memory_order_relaxed);
    const std::uint64_t head = head_counter.load(std::memory_order_acquire);
    if (tail == head) {
        return false;
    }
    *signal = slots[tail & (kSzeTradingSignalCapacity - 1U)];
    tail_counter.store(tail + 1U, std::memory_order_release);
    return true;
}

bool StrategyBase::update_sze_prediction_candidate(
    const SzeTradingSignal& signal) {
    const std::string code(signal.instrument);
    std::unordered_map<std::string, SzePredictionSignalState>::iterator state_it =
        mSzePredictionSignalStateMap.find(code);
    if (state_it == mSzePredictionSignalStateMap.end()) {
        state_it = mSzePredictionSignalStateMap.emplace(
            code, SzePredictionSignalState()).first;
    }
    SzePredictionSignalState& state = state_it->second;
    sze_prediction::Candidate candidate;
    candidate.source = signal.prediction_source;
    candidate.trading_day = signal.trading_day;
    candidate.exchange_time_us = signal.exchange_time_us;
    candidate.turnover = signal.turnover;
    candidate.sequence = signal.queue_sequence;
    candidate.valid = true;
    if (!state.arbiter.update(candidate)) {
        return false;
    }
    if (signal.prediction_source == sze_prediction::kSnapshot) {
        state.snapshot = signal;
        state.has_snapshot = true;
    } else {
        state.full_orderbook = signal;
        state.has_full_orderbook = true;
    }
    return true;
}

void StrategyBase::dispatch_sze_prediction_candidate(const std::string& code) {
    std::unordered_map<std::string, SzePredictionSignalState>::iterator state_it =
        mSzePredictionSignalStateMap.find(code);
    if (state_it == mSzePredictionSignalStateMap.end()) {
        return;
    }
    SzePredictionSignalState& state = state_it->second;
    const bool full_runtime_valid =
        mSzeRecoveryLiveReady.load(std::memory_order_acquire) &&
        mSzeRecoveryContinuityValid.load(std::memory_order_acquire) &&
        !mSzeRecoveryReplayContext.load(std::memory_order_acquire);
    const sze_prediction::Selection selected =
        state.arbiter.select(full_runtime_valid);
    if (!state.arbiter.should_dispatch(selected) || !is_risk_data_ready() ||
        !mSzeTradingQueueHealthy.load(std::memory_order_acquire)) {
        return;
    }
    const SzeTradingSignal* chosen = 0;
    if (selected.source == sze_prediction::kSnapshot && state.has_snapshot) {
        chosen = &state.snapshot;
    } else if (selected.source == sze_prediction::kFullOrderBook &&
               state.has_full_orderbook) {
        chosen = &state.full_orderbook;
    }
    if (chosen == 0) {
        return;
    }
    std::unordered_map<std::string, ZStrategy*>::iterator strategy_it =
        mZStrategyMap.find(code);
    if (strategy_it == mZStrategyMap.end() || strategy_it->second == 0) {
        return;
    }
    MSMarketDataField market_data = {MSMarketData()};
    market_data.ms_market_data.ms_market_data = chosen->market_data;
    strategy_it->second->on_signal(
        &market_data, chosen->prediction, chosen->source,
        chosen->receive_time);
    state.arbiter.mark_dispatched(selected);
    ++state.dispatch_count;
    if (!state.has_selected_source ||
        state.selected_source != selected.source ||
        state.dispatch_count % 10000U == 0U) {
        KF_LOG_INFO(logger, "[SZPredictionArbiter] instrument=" << code
            << " selected="
            << (selected.source == sze_prediction::kFullOrderBook
                    ? "full_orderbook" : "snapshot")
            << " full_turnover="
            << (state.has_full_orderbook
                    ? state.full_orderbook.turnover : -1.0)
            << " snapshot_turnover="
            << (state.has_snapshot ? state.snapshot.turnover : -1.0)
            << " full_runtime_valid=" << (full_runtime_valid ? 1 : 0)
            << " dispatch_count=" << state.dispatch_count);
    }
    state.selected_source = selected.source;
    state.has_selected_source = true;
}

void StrategyBase::dispatch_or_queue_trading_signal(
    const std::string& code,
    const MSMarketDataField* market_data,
    double prediction,
    short source,
    long receive_time,
    sze_prediction::Source prediction_source,
    std::uint32_t trading_day,
    std::uint64_t exchange_time_us) {
    const bool snapshot = prediction_source == sze_prediction::kSnapshot;
    if (!is_risk_data_ready() || mSzeRecoveryAnalysisMode ||
        !mSzeTradingQueueHealthy.load(std::memory_order_acquire) ||
        (!snapshot && !can_dispatch_trading_signal())) {
        return;
    }
    if (mSzeRecoveryConsumerConfig.enabled) {
        if (!enqueue_sze_trading_signal(
                code, market_data, prediction, source, receive_time,
                prediction_source, trading_day, exchange_time_us)) {
            mSzeTradingQueueHealthy.store(false, std::memory_order_release);
            mSzeRecoveryLiveReady.store(false, std::memory_order_release);
            KF_LOG_ERROR(logger, "[SZPredictionArbiter] "
                << (snapshot ? "snapshot" : "full_orderbook")
                << " queue overrun; signal dispatch disabled for the session");
        }
        return;
    }
    std::unordered_map<std::string, ZStrategy*>::iterator strategy_it =
        mZStrategyMap.find(code);
    if (strategy_it != mZStrategyMap.end() && strategy_it->second != 0) {
        strategy_it->second->on_signal(
            market_data, prediction, source, receive_time);
    }
}

void StrategyBase::sze_trading_poll_loop() {
    cpu_set_t cpu_set;
    CPU_ZERO(&cpu_set);
    CPU_SET(mSzeRecoveryConsumerConfig.strategy_cpu, &cpu_set);
    const int affinity_status = ::pthread_setaffinity_np(
        ::pthread_self(), sizeof(cpu_set), &cpu_set);
    if (affinity_status != 0) {
        KF_LOG_ERROR(logger, "[SZRecovery] failed to bind strategy poll thread"
            << " cpu=" << mSzeRecoveryConsumerConfig.strategy_cpu
            << " status=" << affinity_status
            << "; continuing without affinity");
    }
    KF_LOG_INFO(logger, "[SZRecovery] strategy poll thread started"
        << " cpu=" << mSzeRecoveryConsumerConfig.strategy_cpu);
    while (mSzeTradingPollRunning.load(std::memory_order_acquire)) {
        SzeTradingSignal signal;
        bool consumed = false;
        std::string full_code;
        std::string snapshot_code;
        if (dequeue_sze_trading_signal(
                sze_prediction::kFullOrderBook, &signal)) {
            if (update_sze_prediction_candidate(signal)) {
                full_code = signal.instrument;
            }
            consumed = true;
        }
        if (dequeue_sze_trading_signal(sze_prediction::kSnapshot, &signal)) {
            if (update_sze_prediction_candidate(signal)) {
                snapshot_code = signal.instrument;
            }
            consumed = true;
        }
        if (!full_code.empty()) {
            dispatch_sze_prediction_candidate(full_code);
        }
        if (!snapshot_code.empty() && snapshot_code != full_code) {
            dispatch_sze_prediction_candidate(snapshot_code);
        }
        if (!consumed) {
            _mm_pause();
        }
    }
    KF_LOG_INFO(logger, "[SZRecovery] strategy poll thread stopped");
}
#endif

bool StrategyBase::using_full_orderbook_mode() const {
    return mOrderBookMode == FULL_ORDERBOOK_MODE;
}

bool StrategyBase::using_hp_shadow_mode() const {
    return mMarket == "SZ" && mOrderBookMode == HP_SHADOW_MODE;
}

bool StrategyBase::using_hp_realtime_mode() const {
    return mMarket == "SZ" && mOrderBookMode == HP_REALTIME_MODE;
}

bool StrategyBase::using_hp_mode() const {
    return using_hp_shadow_mode() || using_hp_realtime_mode();
}

sz_hp::InstrumentState* StrategyBase::hp_state_for(const std::string& code) {
    std::unordered_map<std::string, sz_hp::InstrumentState>::iterator it =
        mSzHpStateMap.find(code);
    return it == mSzHpStateMap.end() ? 0 : &it->second;
}

bool StrategyBase::process_hp_order(const std::string& code, const LFL2OrderField* data) {
    sz_hp::InstrumentState* state = hp_state_for(code);
    if (state == 0 || data == 0) {
        return false;
    }
    const uint64_t event_index = ++mSzHpEventIndexMap[code];
    sz_hp::OrderEvent event;
    if (!sz_hp::EventAdapter::normalize_order(*data, &event, 0, event_index)) {
        sz_hp::AdapterDiagnostic diagnostic;
        sz_hp::EventAdapter::normalize_order(*data, &event, &diagnostic, event_index);
        state->reject_event(diagnostic.sequence > 0
                                ? static_cast<uint64_t>(diagnostic.sequence)
                                : event_index,
                            diagnostic.reason.c_str());
        mSzHpAdapterDiagnosticMap[code] = diagnostic;
        return false;
    }
    const bool success = state->process_order(event);
    if (!success) {
        sz_hp::AdapterDiagnostic diagnostic;
        sz_hp::EventAdapter::normalize_order(*data, &event, &diagnostic, event_index);
        diagnostic.code = sz_hp::AdapterDiagnostic::kBookFailure;
        diagnostic.sequence = static_cast<int64_t>(event.sequence);
        diagnostic.reason = state->book().failure_reason();
        mSzHpAdapterDiagnosticMap[code] = diagnostic;
    }
    return success;
}

sz_hp::SampleDecision StrategyBase::process_hp_trade(const std::string& code,
                                                     const LFL2TradeField* data) {
    sz_hp::SampleDecision decision;
    decision.trigger = sz_hp::SampleTrigger::kTrade;
    sz_hp::InstrumentState* state = hp_state_for(code);
    if (state == 0 || data == 0) {
        decision.reason = sz_hp::SampleBlockReason::kAdapterRejected;
        return decision;
    }
    const uint64_t event_index = ++mSzHpEventIndexMap[code];
    sz_hp::TradeEvent event;
    if (!sz_hp::EventAdapter::normalize_trade(*data, &event, 0, event_index)) {
        sz_hp::AdapterDiagnostic diagnostic;
        sz_hp::EventAdapter::normalize_trade(*data, &event, &diagnostic, event_index);
        state->reject_event(diagnostic.sequence > 0
                                ? static_cast<uint64_t>(diagnostic.sequence)
                                : event_index,
                            diagnostic.reason.c_str());
        mSzHpAdapterDiagnosticMap[code] = diagnostic;
        decision.reason = sz_hp::SampleBlockReason::kAdapterRejected;
        decision.book_available = false;
        return decision;
    }
    decision = state->process_trade(event);
    if (!state->available()) {
        sz_hp::AdapterDiagnostic diagnostic;
        sz_hp::EventAdapter::normalize_trade(*data, &event, &diagnostic, event_index);
        diagnostic.code = sz_hp::AdapterDiagnostic::kBookFailure;
        diagnostic.sequence = static_cast<int64_t>(event.sequence);
        diagnostic.bid_id = static_cast<int64_t>(event.bid_id);
        diagnostic.ask_id = static_cast<int64_t>(event.ask_id);
        diagnostic.reason = state->book().failure_reason();
        mSzHpAdapterDiagnosticMap[code] = diagnostic;
    }
    return decision;
}

sz_hp::SampleDecision StrategyBase::process_hp_observation(
    const std::string& code,
    const LFL2MarketDataField* data) {
    sz_hp::SampleDecision decision;
    decision.trigger = sz_hp::SampleTrigger::kObservation;
    sz_hp::InstrumentState* state = hp_state_for(code);
    if (state == 0 || data == 0) {
        decision.reason = sz_hp::SampleBlockReason::kAdapterRejected;
        return decision;
    }
    const uint64_t event_index = ++mSzHpEventIndexMap[code];
    sz_hp::MarketObservation observation;
    if (!sz_hp::EventAdapter::normalize_observation(*data, &observation, 0, event_index)) {
        sz_hp::AdapterDiagnostic diagnostic;
        sz_hp::EventAdapter::normalize_observation(*data, &observation, &diagnostic, event_index);
        state->reject_event(event_index, diagnostic.reason.c_str());
        mSzHpAdapterDiagnosticMap[code] = diagnostic;
        decision.reason = sz_hp::SampleBlockReason::kAdapterRejected;
        decision.book_available = false;
        return decision;
    }
    decision = state->process_observation(observation);
    return decision;
}

void StrategyBase::consume_hp_sample_if_ready(const std::string& code,
                                              const sz_hp::SampleDecision& decision) {
    if (!decision.ready) {
        return;
    }
    sz_hp::InstrumentState* state = hp_state_for(code);
    if (state == 0) {
        return;
    }
    sz_hp::SampleBatch batch;
    if (!state->consume_sample(&batch)) {
        return;
    }
    // Shadow and realtime currently retain this artifact for parity inspection. The
    // generated HP model is intentionally not invoked until its accepted artifact exists.
    mSzHpFactorInputMap[code] = sz_hp::build_factor_input(*state, batch);
}

mix153060::Runtime* StrategyBase::mix153060_runtime_for(const std::string& code) {
    std::unordered_map<std::string, std::unique_ptr<mix153060::Runtime> >::iterator it =
        mMix153060RuntimeMap.find(code);
    return it == mMix153060RuntimeMap.end() ? 0 : it->second.get();
}

#ifdef T0_SZE_STRATEGY_ONLY
void StrategyBase::update_sze_book_health(const std::string& code,
                                          mix153060::Runtime* runtime) {
    if (!mSzeRecoveryHealthWriter || !runtime) return;
    const std::uint32_t symbol_id = sze_health::parse_symbol_id(code.c_str());
    if (symbol_id >= 1000000U) return;
    const std::uint64_t now_ns = sze_recovery::monotonic_time_ns();
    if (runtime->available()) {
        mSzeRecoveryHealthWriter->publish_book_valid_once(
            symbol_id, mSzeCurrentRecoveryEventId, now_ns);
    } else {
        mSzeRecoveryHealthWriter->publish_book(
            symbol_id, sze_health::kBookInvalid, 1U,
            mSzeCurrentRecoveryEventId, now_ns);
    }
}
#endif

void StrategyBase::process_mix153060_order(const std::string& code,
                                           const LFL2OrderField* data,
                                           short source,
                                           long rcv_time) {
    mix153060::Runtime* runtime = mix153060_runtime_for(code);
    std::unordered_map<std::string, InsParams>::const_iterator params_it =
        mInsParamsMap.find(code);
    if (runtime == 0 || !runtime->available() || data == 0 ||
        params_it == mInsParamsMap.end()) {
        return;
    }
    mix153060::OrderEvent event;
    if (!mix153060::normalize_order_event(
            *data, params_it->second.Date, rcv_time, &event, 0)) {
        std::string reason;
        mix153060::normalize_order_event(
            *data, params_it->second.Date, rcv_time, &event, &reason);
        runtime->invalidate();
#ifdef T0_SZE_STRATEGY_ONLY
        update_sze_book_health(code, runtime);
#endif
        ++mMix153060AdapterRejectCount;
        KF_LOG_ERROR(logger, "[SZ][prediction] order adapter rejected instrument="
                             << code << " app_sequence=" << data->ApplSeqNum
                             << " reason=" << reason << "; instrument suppressed");
        return;
    }
    mix153060::SampleBuffer samples;
    const bool capture_enabled = mMix153060Capture.get() != 0 &&
                                 mMix153060Capture->enabled_for(code);
    const bool capture_detail = capture_enabled &&
                                mMix153060Capture->detail_enabled_for(code);
    mix153060::EventTiming timing;
    runtime->on_order(event, &samples, capture_detail ? &timing : 0);
#ifdef T0_SZE_STRATEGY_ONLY
    update_sze_book_health(code, runtime);
#endif
    if (capture_detail) {
        mix153060::OrderEvent resolved_market;
        bool from_linked_fill = false;
        while (runtime->take_resolved_market_order(&resolved_market, &from_linked_fill)) {
            mMix153060Capture->record_market_resolution(
                code, resolved_market, from_linked_fill, source,
                static_cast<std::int64_t>(rcv_time),
#ifdef T0_SZE_STRATEGY_ONLY
                !mSzeRecoveryAnalysisMode);
#else
                true);
#endif
        }
        mMix153060Capture->record_order(code, event, source,
                                        static_cast<std::int64_t>(rcv_time), timing,
                                        runtime->available(), samples.count,
#ifdef T0_SZE_STRATEGY_ONLY
                                        !mSzeRecoveryAnalysisMode);
#else
                                        true);
#endif
    }
    if (!runtime->available()) {
        ++mMix153060BookRejectCount;
        KF_LOG_ERROR(logger, "[SZ][prediction] order book rejected instrument="
                             << code << " app_sequence=" << runtime->failure_sequence()
                             << " reason=" << runtime->failure_reason()
                             << "; instrument suppressed");
        return;
    }
    consume_mix153060_samples(code, samples, source, rcv_time);
}

void StrategyBase::process_mix153060_trade(const std::string& code,
                                           const LFL2TradeField* data,
                                           short source,
                                           long rcv_time) {
    mix153060::Runtime* runtime = mix153060_runtime_for(code);
    std::unordered_map<std::string, InsParams>::const_iterator params_it =
        mInsParamsMap.find(code);
    if (runtime == 0 || !runtime->available() || data == 0 ||
        params_it == mInsParamsMap.end()) {
        return;
    }
    mix153060::TradeEvent event;
    if (!mix153060::normalize_trade_event(
            *data, params_it->second.Date, rcv_time, &event, 0)) {
        std::string reason;
        mix153060::normalize_trade_event(
            *data, params_it->second.Date, rcv_time, &event, &reason);
        runtime->invalidate();
#ifdef T0_SZE_STRATEGY_ONLY
        update_sze_book_health(code, runtime);
#endif
        ++mMix153060AdapterRejectCount;
        KF_LOG_ERROR(logger, "[SZ][prediction] trade adapter rejected instrument="
                             << code << " app_sequence=" << data->ApplSeqNum
                             << " reason=" << reason << "; instrument suppressed");
        return;
    }
    mix153060::SampleBuffer samples;
    const bool capture_enabled = mMix153060Capture.get() != 0 &&
                                 mMix153060Capture->enabled_for(code);
    const bool capture_detail = capture_enabled &&
                                mMix153060Capture->detail_enabled_for(code);
    mix153060::EventTiming timing;
    runtime->on_trade(event, &samples, capture_detail ? &timing : 0);
#ifdef T0_SZE_STRATEGY_ONLY
    update_sze_book_health(code, runtime);
#endif
    if (capture_detail) {
        mix153060::OrderEvent resolved_market;
        bool from_linked_fill = false;
        while (runtime->take_resolved_market_order(&resolved_market, &from_linked_fill)) {
            mMix153060Capture->record_market_resolution(
                code, resolved_market, from_linked_fill, source,
                static_cast<std::int64_t>(rcv_time),
#ifdef T0_SZE_STRATEGY_ONLY
                !mSzeRecoveryAnalysisMode);
#else
                true);
#endif
        }
        mMix153060Capture->record_trade(code, event, source,
                                        static_cast<std::int64_t>(rcv_time), timing,
                                        runtime->available(), samples.count,
#ifdef T0_SZE_STRATEGY_ONLY
                                        !mSzeRecoveryAnalysisMode);
#else
                                        true);
#endif
    }
    if (!runtime->available()) {
        ++mMix153060BookRejectCount;
        KF_LOG_ERROR(logger, "[SZ][prediction] trade book rejected instrument="
                             << code << " app_sequence=" << runtime->failure_sequence()
                             << " reason=" << runtime->failure_reason()
                             << "; instrument suppressed");
        return;
    }
    consume_mix153060_samples(code, samples, source, rcv_time);
}

void StrategyBase::flush_mix153060_pending(const std::string& code,
                                           short source,
                                           long rcv_time) {
    mix153060::Runtime* runtime = mix153060_runtime_for(code);
    if (runtime == 0) {
        return;
    }
    mix153060::SampleBuffer samples;
    runtime->flush(&samples);
    consume_mix153060_samples(code, samples, source, rcv_time);
}

MSMarketDataField* StrategyBase::update_mix153060_signal_view(
    const std::string& code,
    const mix153060::Sample& sample) {
    std::unordered_map<std::string, std::unique_ptr<MSMarketDataField> >::iterator it =
        mMix153060SignalViewMap.find(code);
    if (it == mMix153060SignalViewMap.end() || it->second.get() == 0) {
        return 0;
    }
    MSMarketData& view = it->second->ms_market_data;
    view = MSMarketData();
    view.ms_market_data[InstrumentIDIndex] = std::strtod(code.c_str(), 0);
    view.ms_market_data[MarketTimeIndex] = MixMarketTimeValue(sample.exchange_time_us);
    view.ms_market_data[LastPriceIndex] = sample.last_price > 0.0
                                             ? sample.last_price : sample.mid_price;
    view.ms_market_data[MidPriceIndex] = sample.mid_price;
    view.ms_market_data[VolumeIndex] = sample.volume;
    view.ms_market_data[TurnoverIndex] = sample.turnover;
    for (std::size_t level = 0; level < 10; ++level) {
        view.ms_market_data[BidVolume1Index + level] = sample.bid_volume[level];
        view.ms_market_data[AskVolume1Index + level] = sample.ask_volume[level];
        view.ms_market_data[BidPrice1Index + level] = sample.bid_price[level];
        view.ms_market_data[AskPrice1Index + level] = sample.ask_price[level];
    }
    view.ms_market_data[AppSeqIndex] = static_cast<double>(sample.app_sequence);
    return it->second.get();
}

void StrategyBase::consume_mix153060_samples(const std::string& code,
                                             const mix153060::SampleBuffer& samples,
                                             short source,
                                             long rcv_time) {
    if (!mMix153060Enabled || samples.count == 0) {
        return;
    }
    std::unordered_map<std::string, mix153060::State>::iterator state_it =
        mMix153060ModelStateMap.find(code);
    if (state_it == mMix153060ModelStateMap.end()) {
        return;
    }
    const bool capture_enabled = mMix153060Capture.get() != 0 &&
                                 mMix153060Capture->enabled_for(code);
    const bool capture_detail = capture_enabled &&
                                mMix153060Capture->detail_enabled_for(code);
    for (std::size_t index = 0; index < samples.count; ++index) {
        const mix153060::Sample& sample = samples.values[index];
        float prediction = 0.0f;
        const std::uint64_t model_begin = capture_detail ? sz_hp::latency_now_ns() : 0;
        if (!mMix153060Model.predict(sample.factors, &state_it->second, &prediction)) {
            ++mMix153060PredictionRejectCount;
            mix153060::Runtime* runtime = mix153060_runtime_for(code);
            if (runtime != 0) {
                runtime->invalidate();
            }
#ifdef T0_SZE_STRATEGY_ONLY
            if (mSzeRecoveryHealthWriter) {
                const std::uint32_t symbol_id =
                    sze_health::parse_symbol_id(code.c_str());
                if (symbol_id < 1000000U) {
                    mSzeRecoveryHealthWriter->publish_prediction(
                        symbol_id, sze_health::kPredictionInvalid, 1U,
                        sze_recovery::monotonic_time_ns(), 0.0);
                }
            }
#endif
            KF_LOG_ERROR(logger, "[SZ][prediction] prediction rejected instrument="
                                 << code << " row=" << sample.row_in_stock_day
                                 << "; instrument suppressed");
            return;
        }
#ifdef T0_SZE_STRATEGY_ONLY
        if (mSzeRecoveryHealthWriter) {
            const std::uint32_t symbol_id =
                sze_health::parse_symbol_id(code.c_str());
            if (symbol_id < 1000000U) {
                mSzeRecoveryHealthWriter->publish_prediction(
                    symbol_id, sze_health::kPredictionHealthy, 0U,
                    sze_recovery::monotonic_time_ns(), sample.turnover);
            }
        }
#endif
        if (capture_enabled) {
            const std::uint64_t model_end = capture_detail ? sz_hp::latency_now_ns() : 0;
            mMix153060Capture->record_sample(
                code, sample, source, static_cast<std::int64_t>(rcv_time), prediction,
                model_end >= model_begin ? model_end - model_begin : 0,
#ifdef T0_SZE_STRATEGY_ONLY
                !mSzeRecoveryAnalysisMode);
#else
                true);
#endif
        }
        if (using_hp_shadow_mode()) {
            continue;
        }
        MSMarketDataField* view = update_mix153060_signal_view(code, sample);
        if (view == 0 || view->BidPrice1 <= 0.0 || view->AskPrice1 <= 0.0 ||
            view->BidVolume1 <= 0.0 || view->AskVolume1 <= 0.0 ||
            view->LastPrice <= 0.0) {
            continue;
        }
#ifdef T0_SZE_STRATEGY_ONLY
        dispatch_or_queue_trading_signal(
            code, view, prediction, source, rcv_time,
            sze_prediction::kFullOrderBook,
            mSzeRecoveryConsumerConfig.trading_day,
            ExchangeTimeOfDayUs(sample.exchange_time_us));
#else
        std::unordered_map<std::string, ZStrategy*>::iterator strategy_it =
            mZStrategyMap.find(code);
        if (can_dispatch_trading_signal() &&
            strategy_it != mZStrategyMap.end() && strategy_it->second != 0) {
            strategy_it->second->on_signal(view, prediction, source, rcv_time);
        }
#endif
    }
}

bool StrategyBase::using_lazy_sample_transition_mode() const {
    return using_full_orderbook_mode() && mMarket == "SZ" && mFullOrderBookLazySampleTransition;
}

bool StrategyBase::should_sample_predictor_transition(const std::string& code,
                                                      uint32_t now_time_ms,
                                                      int* mid_price) {
    const ShSzFullOrderBookEngine* engine = mFullOrderBookManager.get_engine(code.c_str());
    if (engine == 0) {
        return false;
    }

    const int current_mid_price = engine->mid_price();
    if (mid_price != 0) {
        *mid_price = current_mid_price;
    }
    if (current_mid_price <= 0) {
        return false;
    }

    const std::unordered_map<std::string, ShSzFullOrderBookSampleState>::const_iterator it =
        mFullOrderBookSampleStateMap.find(code);
    if (it == mFullOrderBookSampleStateMap.end() || !it->second.has_sample) {
        return true;
    }
    if (it->second.last_mid_price != current_mid_price) {
        return true;
    }
    if (it->second.last_sample_time_ms == now_time_ms) {
        return false;
    }
    return false;
}

void StrategyBase::mark_sample_predictor_transition(const std::string& code,
                                                    uint32_t now_time_ms,
                                                    int mid_price) {
    ShSzFullOrderBookSampleState& state = mFullOrderBookSampleStateMap[code];
    state.has_sample = true;
    state.last_mid_price = mid_price;
    state.last_sample_time_ms = now_time_ms;
}

void StrategyBase::reset_full_orderflow_summary(const std::string& code) {
    mFullOrderFlowSummaryMap[code] = ShSzOrderFlowSummary();
}

void StrategyBase::update_full_orderflow_from_order(const std::string& code, const LFL2OrderField* data) {
    if (data == 0) {
        return;
    }
    ShSzOrderFlowSummary& summary = mFullOrderFlowSummaryMap[code];
    const double volume = data->Volume;
    if (volume <= 0.0) {
        return;
    }

    const bool is_sh_cancel = (mMarket == "SH" && data->OrdType[0] != 'A');
    if (is_sh_cancel) {
        if (data->OrderKind[0] == 'B') {
            summary.cxl_buy_flow += volume;
        } else {
            summary.cxl_sell_flow += volume;
        }
        return;
    }

    if (data->OrderKind[0] == 'B') {
        summary.buy_order_volume += volume;
    } else {
        summary.sell_order_volume += volume;
    }
}

void StrategyBase::update_full_orderflow_from_trade(const std::string& code, const LFL2TradeField* data) {
    if (data == 0) {
        return;
    }
    ShSzOrderFlowSummary& summary = mFullOrderFlowSummaryMap[code];
    const double volume = data->Volume;
    if (volume <= 0.0) {
        return;
    }

    if (data->OrderKind[0] == '4') {
        if (data->OfferApplSeqNum == 0) {
            summary.cxl_buy_flow += volume;
        }
        if (data->BidApplSeqNum == 0) {
            summary.cxl_sell_flow += volume;
        }
        return;
    }

    if (data->BidApplSeqNum > data->OfferApplSeqNum) {
        summary.trade_pt += volume;
    } else {
        summary.trade_nt += volume;
    }
}

bool StrategyBase::refresh_predictor_transition(const std::string& code, uint32_t now_time_ms) {
    std::unordered_map<std::string, ShSzPredictorTransitionInput>::const_iterator it = mPredictorTransitionMap.find(code);
    const MSMarketDataField* seed_snapshot = 0;
    if (it != mPredictorTransitionMap.end() && it->second.has_signal_snapshot) {
        seed_snapshot = it->second.signal_snapshot_ptr();
    }

    const uint64_t transition_begin_ns =
        mFullOrderBookLatencyEnabled ? shsz_full_orderbook_now_ns() : 0;
    ShSzPredictorTransitionInput input = ShSzPredictorTransitionAdapter::from_full_orderbook(
        mFullOrderBookManager,
        code.c_str(),
        mFullOrderFlowSummaryMap[code],
        now_time_ms,
        1.0,
        seed_snapshot);
    if (!input.valid) {
        return false;
    }
    if (mFullOrderBookLatencyEnabled) {
        ShSzFullOrderBookLatencyStats& stats = shsz_full_orderbook_latency_stats();
        stats.transition_count += 1;
        stats.transition_ns += (shsz_full_orderbook_now_ns() - transition_begin_ns);
    }
    mPredictorTransitionMap[code] = input;
    return true;
}

const MSMarketDataField* StrategyBase::current_predict_signal_snapshot(const std::string& code) const {
    if (using_full_orderbook_mode()) {
        std::unordered_map<std::string, ShSzPredictorTransitionInput>::const_iterator it = mPredictorTransitionMap.find(code);
        if (it == mPredictorTransitionMap.end()) {
            return 0;
        }
        return it->second.signal_snapshot_ptr();
    }

    std::unordered_map<std::string, SnapGenerator>::const_iterator snap_it = mSnapGeneratorMap.find(code);
    if (snap_it == mSnapGeneratorMap.end() || snap_it->second.mSnapIndex < 0) {
        return 0;
    }
    const size_t snap_index = static_cast<size_t>(snap_it->second.mSnapIndex % MARKET_ARRAY_LENGTH);
    return &snap_it->second.mMsMarketDataFieldArray[snap_index];
}

bool StrategyBase::process_order_full_orderbook(const std::string& code,
                                                const LFL2OrderField* data,
                                                short source,
                                                long rcv_time) {
    const uint64_t process_begin_ns =
        mFullOrderBookLatencyEnabled ? shsz_full_orderbook_now_ns() : 0;
    const uint64_t manager_begin_ns =
        mFullOrderBookLatencyEnabled ? shsz_full_orderbook_now_ns() : 0;
    mFullOrderBookManager.process_order(data);
    if (mFullOrderBookLatencyEnabled) {
        ShSzFullOrderBookLatencyStats& stats = shsz_full_orderbook_latency_stats();
        stats.manager_order_count += 1;
        stats.manager_order_ns += (shsz_full_orderbook_now_ns() - manager_begin_ns);
    }
    if (IsContinuousAuctionOrder(data)) {
        mPredictorMap[code]->handle_order(data);
        update_full_orderflow_from_order(code, data);
    }

    const uint32_t now_time_ms = ShSzFullOrderBookEngine::parse_event_time_ms(data->OrderTime);
    if (mFullOrderBookLatencyEnabled) {
        shsz_full_orderbook_latency_stats().last_event_time_ms = now_time_ms;
    }
    const bool should_refresh_transition =
        !using_lazy_sample_transition_mode() &&
        (mMarket == "SH" || mFullOrderBookTraceEnabled);
    const bool transition_ok =
        should_refresh_transition ? refresh_predictor_transition(code, now_time_ms) : false;
    bool may_predict = false;
    if (transition_ok) {
        const MSMarketDataField* signal_snapshot = current_predict_signal_snapshot(code);
        if (signal_snapshot != 0 && mMarket == "SH") {
            const uint64_t may_predict_begin_ns =
                mFullOrderBookLatencyEnabled ? shsz_full_orderbook_now_ns() : 0;
            may_predict = mPredictorMap[code]->MayPredict(signal_snapshot);
            if (mFullOrderBookLatencyEnabled) {
                ShSzFullOrderBookLatencyStats& stats = shsz_full_orderbook_latency_stats();
                stats.may_predict_count += 1;
                stats.may_predict_ns += (shsz_full_orderbook_now_ns() - may_predict_begin_ns);
            }
            if (may_predict) {
                mPendingPredictSet.insert(code);
            }
        }
    }

    if (!using_lazy_sample_transition_mode() || transition_ok) {
        trace_full_orderbook_order(
            code,
            data,
            source,
            rcv_time,
            now_time_ms,
            transition_ok,
            may_predict,
            false,
            0.0);
    }

    if (mFullOrderBookLatencyEnabled) {
        ShSzFullOrderBookLatencyStats& stats = shsz_full_orderbook_latency_stats();
        stats.process_order_count += 1;
        stats.process_order_ns += (shsz_full_orderbook_now_ns() - process_begin_ns);
    }
    maybe_log_full_orderbook_key_timing();
    return transition_ok;
}

bool StrategyBase::process_trade_full_orderbook(const std::string& code, const LFL2TradeField* data) {
    const uint64_t process_begin_ns =
        mFullOrderBookLatencyEnabled ? shsz_full_orderbook_now_ns() : 0;
    const uint64_t manager_begin_ns =
        mFullOrderBookLatencyEnabled ? shsz_full_orderbook_now_ns() : 0;
    mFullOrderBookManager.process_trade(data);
    if (mFullOrderBookLatencyEnabled) {
        ShSzFullOrderBookLatencyStats& stats = shsz_full_orderbook_latency_stats();
        stats.manager_trade_count += 1;
        stats.manager_trade_ns += (shsz_full_orderbook_now_ns() - manager_begin_ns);
    }
    mPredictorMap[code]->handle_trade(data);
    update_full_orderflow_from_trade(code, data);

    const uint32_t now_time_ms = ShSzFullOrderBookEngine::parse_event_time_ms(data->TradeTime);
    if (mFullOrderBookLatencyEnabled) {
        shsz_full_orderbook_latency_stats().last_event_time_ms = now_time_ms;
    }
    bool transition_ok = false;
    if (using_lazy_sample_transition_mode()) {
        int mid_price = 0;
        if (should_sample_predictor_transition(code, now_time_ms, &mid_price)) {
            transition_ok = refresh_predictor_transition(code, now_time_ms);
            if (transition_ok) {
                mark_sample_predictor_transition(code, now_time_ms, mid_price);
            }
        }
    } else {
        transition_ok = refresh_predictor_transition(code, now_time_ms);
    }
    if (mFullOrderBookLatencyEnabled) {
        ShSzFullOrderBookLatencyStats& stats = shsz_full_orderbook_latency_stats();
        stats.process_trade_count += 1;
        stats.process_trade_ns += (shsz_full_orderbook_now_ns() - process_begin_ns);
    }
    maybe_log_full_orderbook_key_timing();
    return transition_ok;
}

void StrategyBase::flush_pending_predictions(short source, long rcv_time) {
    if (mMarket != "SH") {
        return;
    }
    if (mPendingPredictSet.empty()) {
        return;
    }

    const uint64_t flush_begin_ns =
        mFullOrderBookLatencyEnabled ? shsz_full_orderbook_now_ns() : 0;
    for (const auto& code : mInstrumentVec) {
        if (mPendingPredictSet.find(code) == mPendingPredictSet.end()) {
            continue;
        }
        const MSMarketDataField* cur_ob = current_predict_signal_snapshot(code);
        if (cur_ob == 0) {
            continue;
        }
        const uint64_t do_predict_begin_ns =
            mFullOrderBookLatencyEnabled ? shsz_full_orderbook_now_ns() : 0;
        double prediction = mPredictorMap[code]->DoPredict(cur_ob);
        if (mFullOrderBookLatencyEnabled) {
            ShSzFullOrderBookLatencyStats& stats = shsz_full_orderbook_latency_stats();
            stats.do_predict_count += 1;
            stats.do_predict_ns += (shsz_full_orderbook_now_ns() - do_predict_begin_ns);
        }
        if (using_full_orderbook_mode()) {
            reset_full_orderflow_summary(code);
        }


        auto it = mZStrategyMap.find(code);
        if (can_dispatch_trading_signal() &&
            it != mZStrategyMap.end() && it->second != nullptr) {
            it->second->on_signal(cur_ob, prediction, source, rcv_time);
        }
    }

    mPendingPredictSet.clear();
    if (mFullOrderBookLatencyEnabled) {
        ShSzFullOrderBookLatencyStats& stats = shsz_full_orderbook_latency_stats();
        stats.flush_pending_count += 1;
        stats.flush_pending_ns += (shsz_full_orderbook_now_ns() - flush_begin_ns);
    }
}

void StrategyBase::on_rtn_order(const LFRtnOrderField *data, int request_id, short source, long rcv_time) {
    if (data == nullptr) {
        return;
    }
    const std::string code = NormalizeInstrumentId(data->InstrumentID);
    auto it = mZStrategyMap.find(code);
    if (it != mZStrategyMap.end()) {
        it->second->on_rtn_order(data,request_id,source,rcv_time);
    }
}

void StrategyBase::on_rtn_trade(const LFRtnTradeField *data, int request_id, short source, long rcv_time) {
    mTradeIndex+=1;
    if (data == nullptr) {
        return;
    }
    const std::string code = NormalizeInstrumentId(data->InstrumentID);
    auto it = mZStrategyMap.find(code);
    if (it != mZStrategyMap.end()) {
        it->second->on_rtn_trade(data, request_id, source, rcv_time);
    }
}



void StrategyBase::init() {
    const char* orderbook_mode_text = "legacy-snapshot";
    if (using_full_orderbook_mode()) {
        orderbook_mode_text = "full-orderbook";
    } else if (using_hp_shadow_mode()) {
        orderbook_mode_text = "hp-shadow";
    } else if (using_hp_realtime_mode()) {
        orderbook_mode_text = "hp-realtime";
    }
    KF_LOG_INFO(logger, "StrategyBase init"
        << " market=" << mMarket
        << " orderbook_mode=" << orderbook_mode_text
        << " full_orderbook_trace=" << BoolText(mFullOrderBookTraceEnabled)
        << " full_orderbook_factor_trace_only=" << BoolText(mFullOrderBookFactorTraceOnly)
        << " full_orderbook_lazy_sample_transition=" << BoolText(mFullOrderBookLazySampleTransition)
        << " full_orderbook_latency=" << BoolText(mFullOrderBookLatencyEnabled)
        << " full_orderbook_latency_log_interval=" << mFullOrderBookLatencyLogInterval
        << " full_orderbook_trace_max_events=" << mFullOrderBookTraceMaxEvents
        << " full_orderbook_trace_filter_size=" << mFullOrderBookTraceInstrumentFilter.size()
        << " prediction_enabled=" << BoolText(mMix153060Enabled)
        << " prediction_model_loaded=" << BoolText(mMix153060Model.loaded())
        << " prediction_runtime_instruments=" << mMix153060RuntimeMap.size()
        << " prediction_capture_only=" << BoolText(mMix153060CaptureOnly)
        << " hp_realtime_model_ready=" << BoolText(mHpRealtimeModelReady)
        << " instruments=" << mInstrumentVec.size());
    if (!using_hp_shadow_mode() && !mMix153060CaptureOnly) {
        for (const auto& code : mInstrumentVec) {
            const auto& params = mInsParamsMap.at(code);
            mZStrategyMap[code] = new ZStrategy(code, params, j_config, util, logger);
        }
    }
#ifdef T0_USE_DEEPWIN
    std::vector<short> md_sources;
    std::vector<short> td_sources;
    if (j_config.find("md_source_index") != j_config.end()) {
        for (const auto& src : j_config["md_source_index"]) {
            AppendUniqueSource(&md_sources, static_cast<short>(src.get<int>()));
        }
    }
    if (j_config.find("td_source_index") != j_config.end()) {
        for (const auto& src : j_config["td_source_index"]) {
            AppendUniqueSource(&td_sources, static_cast<short>(src.get<int>()));
        }
    }
    if (data != nullptr) {
        for (const auto& src : md_sources) {
            data->add_market_data(src);
            const bool subscribe_ok = util->subscribeOrderTrade(mInstrumentVec, src);
            KF_LOG_INFO(logger, "[MdSubscribe][OrderTrade] source=" << src
                << " requested=" << mInstrumentVec.size()
                << " ok=" << subscribe_ok);
        }
        for (const auto& src : td_sources) {
            data->add_register_td(src);
        }
    }
#endif
    request_startup_risk_state();
#ifdef T0_SZE_STRATEGY_ONLY
    start_sze_recovery_consumer();
#endif
}

void StrategyBase::on_l2_order(const struct LFL2OrderField *data,
                               short source,
                               long rcv_time) {
    if (data == 0) {
        return;
    }
#ifdef T0_SZE_STRATEGY_ONLY
    if (mSzeRecoveryConsumerConfig.enabled) {
        return;
    }
#endif
    process_l2_order_event(data, source, rcv_time);
}

void StrategyBase::process_l2_order_event(const LFL2OrderField* data,
                                          short source,
                                          long rcv_time) {
    if (data == 0) {
        return;
    }
    const std::string code = NormalizeInstrumentId(data->InstrumentID);
    if (mSseHybridEnabled && mMarket == "SH") {
        process_sse_hybrid_order(data, source, rcv_time);
        return;
    }
    if (using_hp_mode()) {
        if (mMix153060Enabled) {
            process_mix153060_order(code, data, source, rcv_time);
        } else {
            process_hp_order(code, data);
        }
        return;
    }
    if (using_full_orderbook_mode()) {
        process_order_full_orderbook(code, data, source, rcv_time);
        if (IsEventBatchTail(data)) {
            flush_pending_predictions(source, rcv_time);
        }
        return;
    }

    mSnapGeneratorMap[code].process_order(data);
    if (strcmp(data->OrderTime, "09:26:00") > 0) {
        mPredictorMap[code]->handle_order(data);
    }

    auto& snap_generator = mSnapGeneratorMap[code];
    if (snap_generator.mSnapIndex >= 0) {
        auto snap_index = static_cast<size_t>(snap_generator.mSnapIndex % MARKET_ARRAY_LENGTH);
        auto* cur_ob = &snap_generator.mMsMarketDataFieldArray[snap_index];
        if (mPredictorMap[code]->MayPredict(cur_ob)) {
            mPendingPredictSet.insert(code);
        }
    }

    if (IsEventBatchTail(data)) {
        flush_pending_predictions(source, rcv_time);
    }
}


void StrategyBase::on_l2_trade(const struct LFL2TradeField *data,
                               short source,
                               long rcv_time) {
    if (data == 0) {
        return;
    }
#ifdef T0_SZE_STRATEGY_ONLY
    if (mSzeRecoveryConsumerConfig.enabled) {
        return;
    }
#endif
    process_l2_trade_event(data, source, rcv_time);
}

void StrategyBase::process_l2_trade_event(const LFL2TradeField* data,
                                          short source,
                                          long rcv_time) {
    if (data == 0) {
        return;
    }
    const std::string code = NormalizeInstrumentId(data->InstrumentID);
    if (mSseHybridEnabled && mMarket == "SH") {
        process_sse_hybrid_trade(data, source, rcv_time);
        return;
    }
    if (using_hp_mode()) {
        if (mMix153060Enabled) {
            process_mix153060_trade(code, data, source, rcv_time);
        } else {
            const sz_hp::SampleDecision hp_decision = process_hp_trade(code, data);
            consume_hp_sample_if_ready(code, hp_decision);
        }
        return;
    }
    timespec ts{};
    if (clock_gettime(CLOCK_REALTIME, &ts) == -1) {
        KF_LOG_ERROR(logger, "clock_gettime failed");
    }
    if (using_full_orderbook_mode()) {
        const uint32_t now_time_ms = ShSzFullOrderBookEngine::parse_event_time_ms(data->TradeTime);
        const bool transition_ok = process_trade_full_orderbook(code, data);
        const MSMarketDataField* cur_ob = transition_ok ? current_predict_signal_snapshot(code) : 0;
        bool may_predict = false;
        bool did_predict = false;
        double prediction = std::numeric_limits<double>::quiet_NaN();

        if (cur_ob != 0) {
            if (mFullOrderBookLatencyEnabled && mMarket == "SZ") {
                mPredictorMap[code]->ProbeFeatureTiming(cur_ob);
            }
            const bool factor_trace_predict_path =
                (mMarket == "SZ" && mFullOrderBookFactorTraceOnly);
            if (factor_trace_predict_path) {
                may_predict = true;
                if (mFullOrderBookLatencyEnabled) {
                    ShSzFullOrderBookLatencyStats& stats = shsz_full_orderbook_latency_stats();
                    stats.may_predict_count += 1;
                }
            } else {
                const uint64_t may_predict_begin_ns =
                    mFullOrderBookLatencyEnabled ? shsz_full_orderbook_now_ns() : 0;
                may_predict = mPredictorMap[code]->MayPredict(cur_ob);
                if (mFullOrderBookLatencyEnabled) {
                    ShSzFullOrderBookLatencyStats& stats = shsz_full_orderbook_latency_stats();
                    stats.may_predict_count += 1;
                    stats.may_predict_ns += (shsz_full_orderbook_now_ns() - may_predict_begin_ns);
                }
            }

            if (mMarket == "SH") {
                if (may_predict) {
                    mPendingPredictSet.insert(code);
                }
                trace_full_orderbook_trade(
                    code,
                    data,
                    source,
                    rcv_time,
                    now_time_ms,
                    transition_ok,
                    may_predict,
                    false,
                    0.0);
                if (IsEventBatchTail(data)) {
                    flush_pending_predictions(source, rcv_time);
                }
                return;
            }

            if (may_predict) {
                const uint64_t do_predict_begin_ns =
                    mFullOrderBookLatencyEnabled ? shsz_full_orderbook_now_ns() : 0;
                if (factor_trace_predict_path) {
                    mPredictorMap[code]->AdvancePredictState(cur_ob);
                } else {
                    prediction = mPredictorMap[code]->DoPredict(cur_ob);
                }
                if (mFullOrderBookLatencyEnabled) {
                    ShSzFullOrderBookLatencyStats& stats = shsz_full_orderbook_latency_stats();
                    stats.do_predict_count += 1;
                    stats.do_predict_ns += (shsz_full_orderbook_now_ns() - do_predict_begin_ns);
                }
                did_predict = true;
                reset_full_orderflow_summary(code);
                if (!factor_trace_predict_path) {
                    auto it = mZStrategyMap.find(code);
                    if (can_dispatch_trading_signal() &&
                        it != mZStrategyMap.end() && it->second != nullptr) {
                        it->second->on_signal(cur_ob, prediction, source, rcv_time);
                    }
                }
            }
        }

        if (!using_lazy_sample_transition_mode() || transition_ok) {
            trace_full_orderbook_trade(
                code,
                data,
                source,
                rcv_time,
                now_time_ms,
                transition_ok,
                may_predict,
                did_predict,
                prediction);
        }
        return;
    }

    mSnapGeneratorMap[code].process_trade(data);
    mPredictorMap[code]->handle_trade(data);

    auto& snap_generator = mSnapGeneratorMap[code];
    if (snap_generator.mSnapIndex < 0) {
        return;
    }
    auto snap_index = static_cast<size_t>(snap_generator.mSnapIndex % MARKET_ARRAY_LENGTH);
    auto* cur_ob = &snap_generator.mMsMarketDataFieldArray[snap_index];

    if (mMarket == "SH") {
        if (mPredictorMap[code]->MayPredict(cur_ob)) {
            mPendingPredictSet.insert(code);
        }
        if (IsEventBatchTail(data)) {
            flush_pending_predictions(source, rcv_time);
        }
        return;
    }

    if (mPredictorMap[code]->MayPredict(cur_ob)) {
        double prediction = mPredictorMap[code]->DoPredict(cur_ob);
        auto it = mZStrategyMap.find(code);
        if (can_dispatch_trading_signal() &&
            it != mZStrategyMap.end() && it->second != nullptr) {
            it->second->on_signal(cur_ob, prediction, source, rcv_time);
        }
    }
}



void StrategyBase::on_signal(const MSMarketDataField * market_data,
                             const char*instrument_id,
                             double signal,
                             short source,
                             long rcv_time) {
    const std::string code = NormalizeInstrumentId(instrument_id);
    auto it = mZStrategyMap.find(code);
    if (can_dispatch_trading_signal() && it != mZStrategyMap.end()) {
        it->second->on_signal(market_data,signal,source,rcv_time);
    }
}

void StrategyBase::on_rsp_account(const LFRspAccountField* data, int request_id, short source, long rcv_time,
    int errorId, const char* errorMsg) {
    IWCStrategy::on_rsp_account(data, request_id, source, rcv_time, errorId, errorMsg);
    KF_LOG_INFO(logger, "[RiskInit][Account] source=" << source
        << " rid=" << request_id
        << " error_id=" << errorId
        << " error_msg=" << (errorMsg == nullptr ? "" : errorMsg)
        << " has_data=" << BoolText(data != nullptr)
        << " available=" << (data == nullptr ? 0.0 : data->Available)
        << " balance=" << (data == nullptr ? 0.0 : data->Balance)
        << " equity=" << (data == nullptr ? 0.0 : data->Equity)
        << " market_value=" << (data == nullptr ? 0.0 : data->MarketValue));
    if (errorId != 0) {
        return;
    }
    auto it = mPendingAccountRid.find(source);
    if (it == mPendingAccountRid.end() || it->second != request_id) {
        mEarlyAccountRid[source] = request_id;
        return;
    }
    mAccountReady[source] = true;
    KF_LOG_INFO(logger, "[RiskInit] account ready source=" << source << " rid=" << request_id);
    if (!mRiskReadyLogged && is_risk_data_ready()) {
        mRiskReadyLogged = true;
        KF_LOG_INFO(logger, "[RiskInit] startup account/position ready, trading enabled");
    }
}

void StrategyBase::on_rtn_pos_option(const LFRspPositionField* data, bool isLast, int request_id, short source, long rcv_time) {
    IWCStrategy::on_rtn_pos_option(data, isLast, request_id, source, rcv_time);
    if (data != nullptr) {
        KF_LOG_INFO(logger, "[RiskInit][Position] source=" << source
            << " rid=" << request_id
            << " is_last=" << BoolText(isLast)
            << " instrument=" << data->InstrumentID
            << " position=" << data->Position
            << " yd_position=" << data->YdPosition
            << " td_position=" << data->TdPosition
            << " available=" << data->Available);
    } else {
        KF_LOG_INFO(logger, "[RiskInit][Position] source=" << source
            << " rid=" << request_id
            << " is_last=" << BoolText(isLast)
            << " has_data=0");
    }
    if (mSzeLiveRoutingEnabled && data != nullptr &&
        data->PosiDirection != LF_CHAR_Short) {
        const std::string code = NormalizeInstrumentId(data->InstrumentID);
        std::unordered_map<std::string, ZStrategy*>::iterator strategy_it =
            mZStrategyMap.find(code);
        if (strategy_it != mZStrategyMap.end() && strategy_it->second != nullptr) {
            const int32_t total = std::max(0, data->Position);
            const int32_t available = std::max(0,
                static_cast<int32_t>(std::llround(data->Available)));
            strategy_it->second->sync_startup_position(total, available);
            mSzeLivePositionReady.insert(code);
        }
    }
    if (!isLast) {
        return;
    }
    auto it = mPendingPositionRid.find(source);
    if (it == mPendingPositionRid.end() || it->second != request_id) {
        mEarlyPositionRid[source] = request_id;
        return;
    }
    const bool all_positions_resolved =
        mSzeLivePositionReady.size() == mInstrumentVec.size();
    mPositionReady[source] = !mSzeLiveRoutingEnabled || all_positions_resolved;
    KF_LOG_INFO(logger, "[RiskInit] position ready source=" << source
        << " rid=" << request_id
        << " position_instruments=" << mSzeLivePositionReady.size()
        << "/" << mInstrumentVec.size()
        << " routing_ready=" << BoolText(is_risk_data_ready())
        << " unresolved=" << (mInstrumentVec.size() - mSzeLivePositionReady.size())
        << " prediction_continues_while_routing_gated=1");
    if (!all_positions_resolved) {
        schedule_startup_position_retry();
    }
    if (!mRiskReadyLogged && is_risk_data_ready()) {
        mRiskReadyLogged = true;
        KF_LOG_INFO(logger, "[RiskInit] startup account/position ready, trading enabled");
    }
}


void StrategyBase::update_info(const char* InstrumentID, short source, long rcv_time) {

}

void StrategyBase::on_market_data(const struct LFMarketDataField *mds, short source, long rcv_time) {
    if (mSseHybridEnabled && mMarket == "SH") {
        process_sse_hybrid_snapshot(mds, source, rcv_time);
        return;
    }
    if (!mSnapshotLegacy15Enabled || mds == 0 ||
        source != mSnapshotLegacy15Source) {
        return;
    }
    const std::string code = NormalizeInstrumentId(mds->InstrumentID);
    std::unordered_map<std::string, SnapshotLegacy15RuntimeState>::iterator state_it =
        mSnapshotLegacy15StateMap.find(code);
    std::unordered_map<std::string, std::unique_ptr<MSMarketDataField> >::iterator view_it =
        mSnapshotLegacy15SignalViewMap.find(code);
    if (state_it == mSnapshotLegacy15StateMap.end() ||
        view_it == mSnapshotLegacy15SignalViewMap.end() ||
        view_it->second.get() == 0) {
        return;
    }
    const sze_snapshot15::Snapshot current = SnapshotLegacy15FromLf(*mds);
    if (current.exchange_time_ms == 0 || current.bid_prices[0] <= 0.0 ||
        current.ask_prices[0] <= 0.0 || current.ask_prices[0] < current.bid_prices[0]) {
        ++mSnapshotLegacy15RejectCount;
        return;
    }
    SnapshotLegacy15RuntimeState& state = state_it->second;
    if (state.trading_day != current.trading_day) {
        state = SnapshotLegacy15RuntimeState();
        state.trading_day = current.trading_day;
    }
    bool predicted = false;
    float prediction = 0.0f;
    if (state.has_previous &&
        current.exchange_time_ms > state.previous.exchange_time_ms &&
        current.exchange_time_ms - state.previous.exchange_time_ms <= 4000U &&
        current.volume >= state.previous.volume &&
        current.turnover >= state.previous.turnover) {
        std::array<float, 36> factors;
        std::string error;
        if (sze_snapshot15::build_factors(state.previous, current, &factors) &&
            mSnapshotLegacy15Model.predict(
                factors, &state.hidden, &prediction, &error) &&
            std::isfinite(prediction)) {
            predicted = true;
        } else {
            ++mSnapshotLegacy15RejectCount;
        }
    }
    state.previous = current;
    state.has_previous = true;
    if (!predicted) {
        return;
    }

    MSMarketData& view = view_it->second->ms_market_data;
    view = MSMarketData();
    view.ms_market_data[InstrumentIDIndex] = std::strtod(code.c_str(), 0);
    view.ms_market_data[MarketTimeIndex] =
        static_cast<double>((current.exchange_time_ms / 3600000U) * 10000000U +
        ((current.exchange_time_ms / 60000U) % 60U) * 100000U +
        ((current.exchange_time_ms / 1000U) % 60U) * 1000U +
        current.exchange_time_ms % 1000U);
    view.ms_market_data[LastPriceIndex] = current.last_price;
    view.ms_market_data[MidPriceIndex] =
        (current.bid_prices[0] + current.ask_prices[0]) * 0.5;
    view.ms_market_data[VolumeIndex] = current.volume;
    view.ms_market_data[TurnoverIndex] = current.turnover;
    for (std::size_t level = 0; level < 10; ++level) {
        view.ms_market_data[BidPrice1Index + level] = mds->aBidPrice[level];
        view.ms_market_data[AskPrice1Index + level] = mds->aAskPrice[level];
        view.ms_market_data[BidVolume1Index + level] = mds->aBidVolume[level];
        view.ms_market_data[AskVolume1Index + level] = mds->aAskVolume[level];
    }
    ++mSnapshotLegacy15PredictionCount;
    if (mSnapshotLegacy15PredictionCount == 1U ||
        mSnapshotLegacy15PredictionCount % 10000U == 0U) {
        KF_LOG_INFO(logger, "[SZSnapshotFallback] prediction_count="
            << mSnapshotLegacy15PredictionCount
            << " instrument=" << code
            << " prediction_permille=" << prediction
            << " exchange_time_ms=" << current.exchange_time_ms);
    }
#ifdef T0_SZE_STRATEGY_ONLY
    dispatch_or_queue_trading_signal(
        code, view_it->second.get(), prediction, source, rcv_time,
        sze_prediction::kSnapshot, ParseTradingDay(current.trading_day),
        current.exchange_time_ms * 1000U);
#else
    std::unordered_map<std::string, ZStrategy*>::iterator strategy_it =
        mZStrategyMap.find(code);
    if (strategy_it != mZStrategyMap.end() && strategy_it->second != 0) {
        strategy_it->second->on_signal(
            view_it->second.get(), prediction, source, rcv_time);
    }
#endif
}

void StrategyBase::on_ms_market_data(const MSMarketDataField *mds, short source, long rcv_time) {

}


void StrategyBase::on_market_data_level2(const struct LFL2MarketDataField *mds, short source, long rcv_time) {
    (void)source;
    (void)rcv_time;
    if (mds == 0 || !using_hp_mode()) {
        return;
    }
    const std::string code = NormalizeInstrumentId(mds->InstrumentID);
    if (mMix153060Enabled) {
        flush_mix153060_pending(code, source, rcv_time);
    } else {
        const sz_hp::SampleDecision decision = process_hp_observation(code, mds);
        consume_hp_sample_if_ready(code, decision);
    }
}

void StrategyBase::dump_full_orderbook_latency_summary() {
    if (!mFullOrderBookLatencyEnabled || !using_full_orderbook_mode()) {
        return;
    }
    ShSzFullOrderBookLatencyStats& stats = shsz_full_orderbook_latency_stats();
    const std::vector<std::string> lines = stats.format_lines();
    for (size_t i = 0; i < lines.size(); ++i) {
        log_runtime_line(lines[i]);
    }
    stats.mark_dumped();
}

void StrategyBase::maybe_log_full_orderbook_key_timing() {
    if (!mFullOrderBookLatencyEnabled || !using_full_orderbook_mode()) {
        return;
    }
    if (mFullOrderBookLatencyLogInterval == 0) {
        return;
    }

    ShSzFullOrderBookLatencyStats& stats = shsz_full_orderbook_latency_stats();
    const uint64_t total_event_count = stats.total_event_count();
    if (total_event_count == 0) {
        return;
    }
    if ((total_event_count - mFullOrderBookLatencyLastLogEventCount) < mFullOrderBookLatencyLogInterval) {
        return;
    }

    mFullOrderBookLatencyLastLogEventCount = total_event_count;
    const std::vector<std::string> factor_lines = stats.format_factor_lines();
    for (size_t i = 0; i < factor_lines.size(); ++i) {
        log_runtime_line(factor_lines[i]);
    }
    const std::vector<std::string> legacy_predictor_lines = stats.format_legacy_predictor_lines();
    for (size_t i = 0; i < legacy_predictor_lines.size(); ++i) {
        log_runtime_line(legacy_predictor_lines[i]);
    }
    log_runtime_line(stats.format_key_line());
}

void StrategyBase::log_runtime_line(const std::string& line) const {
#ifdef T0_USE_DEEPWIN
    if (logger != 0) {
        KF_LOG_INFO(logger, line);
        return;
    }
#else
    if (util != 0) {
        BackTestDW_C_kf_log("INFO", line);
        return;
    }
#endif
    std::cout << line << std::endl;
}

bool StrategyBase::should_trace_full_orderbook(const std::string& code) {
    if (!mFullOrderBookTraceEnabled) {
        return false;
    }
    if (!mFullOrderBookTraceInstrumentFilter.empty() &&
        mFullOrderBookTraceInstrumentFilter.find(code) == mFullOrderBookTraceInstrumentFilter.end()) {
        return false;
    }
    if (mFullOrderBookTraceMaxEvents > 0 && mFullOrderBookTraceEmitted >= mFullOrderBookTraceMaxEvents) {
        return false;
    }
    mFullOrderBookTraceEmitted += 1;
    return true;
}

void StrategyBase::trace_full_orderbook_order(const std::string& code,
                                              const LFL2OrderField* data,
                                              short source,
                                              long rcv_time,
                                              uint32_t now_time_ms,
                                              bool transition_ok,
                                              bool may_predict,
                                              bool did_predict,
                                              double prediction) {
    if (data == 0 || !should_trace_full_orderbook(code)) {
        return;
    }

    const uint64_t trace_seq = ++mFullOrderBookTraceSequenceMap[code];
    std::unordered_map<std::string, ShSzPredictorTransitionInput>::const_iterator transition_it =
        mPredictorTransitionMap.find(code);
    const ShSzPredictorTransitionInput* transition =
        transition_it != mPredictorTransitionMap.end() ? &transition_it->second : 0;
    const MSMarketDataField* signal_snapshot =
        transition != 0 ? transition->signal_snapshot_ptr() : 0;
    const ShSzFullOrderBookPredictorInput* predictor_input =
        (transition != 0 && transition->full_orderbook_input.valid) ? &transition->full_orderbook_input : 0;

    if (!mFullOrderBookFactorTraceOnly) {
        const ShSzFullOrderBookManager::InstrumentSnapshot instrument_snapshot =
            mFullOrderBookManager.snapshot_instrument(code.c_str(), now_time_ms);
        const ShSzFullOrderBookSummary& summary = instrument_snapshot.book_state.summary;
        const ShSzVisibleBookLevel& bid1 = instrument_snapshot.book_state.visible_book.bids[0];
        const ShSzVisibleBookLevel& ask1 = instrument_snapshot.book_state.visible_book.asks[0];
        const ShSzVisibleBookLevel& bid5 = instrument_snapshot.book_state.visible_book.bids[4];
        const ShSzVisibleBookLevel& ask5 = instrument_snapshot.book_state.visible_book.asks[4];
        std::ostringstream transition_line;
        transition_line << "[Trace][FullOrderBook][transition]"
            << " market=" << mMarket
            << " instrument=" << code
            << " trace_seq=" << trace_seq
            << " event=order"
            << " source=" << source
            << " rcv_time=" << rcv_time
            << " event_time_ms=" << now_time_ms
            << " event_time=" << data->OrderTime
            << " appl_seq=" << data->ApplSeqNum
            << " quote_tag=" << (mMarket == "SH" ? data->OrderNo : data->ApplSeqNum)
            << " order_no=" << data->OrderNo
            << " biz_index=" << data->BizIndex
            << " side=" << data->OrderKind[0]
            << " ord_type=" << data->OrdType[0]
            << " price=" << data->Price
            << " volume=" << data->Volume
            << " transition_ok=" << BoolText(transition_ok)
            << " may_predict=" << BoolText(may_predict)
            << " did_predict=" << BoolText(did_predict)
            << " prediction=" << (did_predict ? prediction : std::numeric_limits<double>::quiet_NaN())
            << " pending_active=" << BoolText(instrument_snapshot.pending_market_order.active)
            << " pending_quote=" << instrument_snapshot.pending_market_order.quote_tag
            << " pending_volume=" << instrument_snapshot.pending_market_order.volume
            << " pending_price=" << NormalizeIntPrice(instrument_snapshot.pending_market_order.resolved_price)
            << " mid=" << NormalizeIntPrice(summary.mid_price)
            << " total_order_count=" << summary.total_order_count
            << " bid_levels=" << summary.bid.level_count
            << " ask_levels=" << summary.ask.level_count
            << " best_bid=" << NormalizeIntPrice(summary.bid.best_price)
            << " best_bid_volume=" << summary.bid.best_volume
            << " best_ask=" << NormalizeIntPrice(summary.ask.best_price)
            << " best_ask_volume=" << summary.ask.best_volume
            << " visible_bid1=" << NormalizeIntPrice(bid1.price)
            << " visible_bid1_volume=" << bid1.total_volume
            << " visible_ask1=" << NormalizeIntPrice(ask1.price)
            << " visible_ask1_volume=" << ask1.total_volume
            << " visible_bid5=" << NormalizeIntPrice(bid5.price)
            << " visible_bid5_volume=" << bid5.total_volume
            << " visible_ask5=" << NormalizeIntPrice(ask5.price)
            << " visible_ask5_volume=" << ask5.total_volume
            << " projected_bid1=" << (signal_snapshot != 0 ? NormalizeSnapshotPrice(signal_snapshot->BidPrice1) : 0.0)
            << " projected_bid1_volume=" << (signal_snapshot != 0 ? signal_snapshot->BidVolume1 : 0.0)
            << " projected_ask1=" << (signal_snapshot != 0 ? NormalizeSnapshotPrice(signal_snapshot->AskPrice1) : 0.0)
            << " projected_ask1_volume=" << (signal_snapshot != 0 ? signal_snapshot->AskVolume1 : 0.0)
            << " projected_mid=" << (signal_snapshot != 0 ? NormalizeSnapshotPrice(signal_snapshot->MidPrice) : 0.0)
            << " projected_market_time=" << (signal_snapshot != 0 ? signal_snapshot->MarketTime : 0.0);
        log_runtime_line(transition_line.str());
    }

    if (predictor_input == 0) {
        return;
    }

    const ShSzFullOb& aggregate = predictor_input->aggregate;
    if (!mFullOrderBookFactorTraceOnly) {
        std::ostringstream aggregate_line;
        aggregate_line << "[Trace][FullOrderBook][aggregate]"
            << " market=" << mMarket
            << " instrument=" << code
            << " trace_seq=" << trace_seq
            << " event=order"
            << " valid=" << BoolText(aggregate.valid)
            << " mp=" << aggregate.mp
            << " bid_level1_price=" << aggregate.bid_level1.price
            << " bid_level1_volume=" << aggregate.bid_level1.volume_sum
            << " ask_level1_price=" << aggregate.ask_level1.price
            << " ask_level1_volume=" << aggregate.ask_level1.volume_sum
            << " bid_level5_volume=" << aggregate.bid_level5.volume_sum
            << " ask_level5_volume=" << aggregate.ask_level5.volume_sum
            << " bid_pct1_volume=" << aggregate.bid_01.volume_sum
            << " ask_pct1_volume=" << aggregate.ask_01.volume_sum
            << " bid_pct5_volume=" << aggregate.bid_05.volume_sum
            << " ask_pct5_volume=" << aggregate.ask_05.volume_sum
            << " bid_pct10_volume=" << aggregate.bid_10.volume_sum
            << " ask_pct10_volume=" << aggregate.ask_10.volume_sum
            << " bid_total_count=" << aggregate.bid_total_count
            << " ask_total_count=" << aggregate.ask_total_count
            << " bid_max_level_price=" << aggregate.bid_max_level_price
            << " ask_max_level_price=" << aggregate.ask_max_level_price
            << " bid_max_volume=" << aggregate.bid_max_volume
            << " ask_max_volume=" << aggregate.ask_max_volume;
        log_runtime_line(aggregate_line.str());
    }

    const ShSzOrderFlowSummary& order_flow = predictor_input->order_flow;
    const ShSzFullOrderBookFactorSet& factors = predictor_input->factors;
    std::ostringstream factor_line;
    factor_line << "[Trace][FullOrderBook][factor]"
        << " market=" << mMarket
        << " instrument=" << code
        << " trace_seq=" << trace_seq
        << " event=order"
        << " event_time_ms=" << now_time_ms
        << " valid=" << BoolText(factors.valid)
        << " mid_price=" << predictor_input->aggregate.mp
        << " buy_order_volume=" << order_flow.buy_order_volume
        << " sell_order_volume=" << order_flow.sell_order_volume
        << " trade_pt=" << order_flow.trade_pt
        << " trade_nt=" << order_flow.trade_nt
        << " cxl_buy_flow=" << order_flow.cxl_buy_flow
        << " cxl_sell_flow=" << order_flow.cxl_sell_flow
        << " positive_fill_rate=" << factors.PositiveFillRate
        << " negative_fill_rate=" << factors.NegativeFillRate
        << " order_flow_imbalance=" << factors.OrderFlowImbalance
        << " cfr_imbalance=" << factors.CFRImbalance
        << " fix_dis_imbalance_pct1=" << factors.FixDisImbalancePct1
        << " fix_dis_imbalance_pct2=" << factors.FixDisImbalancePct2
        << " weighted_fix_dis_imbalance_pct1=" << factors.WeightedFixDisImbalancePct1
        << " weighted_fix_dis_imbalance_pct2=" << factors.WeightedFixDisImbalancePct2
        << " avg_size_imbalance=" << factors.AvgSizeImbalance
        << " avg_size_imbalance_level1=" << factors.AvgSizeImbalanceLevel1
        << " avg_size_imbalance_level5=" << factors.AvgSizeImbalanceLevel5
        << " order_count_imbalance=" << factors.OrderCountImbalance
        << " order_count_imbalance_level1=" << factors.OrderCountImbalanceLevel1
        << " order_count_imbalance_level5=" << factors.OrderCountImbalanceLevel5
        << " order_life_imbalance=" << factors.OrderLifeImbalance
        << " order_life_imbalance_level1=" << factors.OrderLifeImbalanceLevel1
        << " order_life_imbalance_level5=" << factors.OrderLifeImbalanceLevel5
        << " max_bid_distance=" << factors.MaxBidDistance
        << " max_ask_distance=" << factors.MaxAskDistance
        << " max_vol_distance_imbalance=" << factors.MaxVolDistanceImbalance
        << " young_orderbook_imbalance=" << factors.YoungOrderbookImbalance
        << " fix_dist_hermes=" << factors.FixDistHermes;
    log_runtime_line(factor_line.str());
}

void StrategyBase::trace_full_orderbook_trade(const std::string& code,
                                              const LFL2TradeField* data,
                                              short source,
                                              long rcv_time,
                                              uint32_t now_time_ms,
                                              bool transition_ok,
                                              bool may_predict,
                                              bool did_predict,
                                              double prediction) {
    if (data == 0 || !should_trace_full_orderbook(code)) {
        return;
    }

    const uint64_t trace_seq = ++mFullOrderBookTraceSequenceMap[code];
    std::unordered_map<std::string, ShSzPredictorTransitionInput>::const_iterator transition_it =
        mPredictorTransitionMap.find(code);
    const ShSzPredictorTransitionInput* transition =
        transition_it != mPredictorTransitionMap.end() ? &transition_it->second : 0;
    const MSMarketDataField* signal_snapshot =
        transition != 0 ? transition->signal_snapshot_ptr() : 0;
    const ShSzFullOrderBookPredictorInput* predictor_input =
        (transition != 0 && transition->full_orderbook_input.valid) ? &transition->full_orderbook_input : 0;

    if (!mFullOrderBookFactorTraceOnly) {
        const ShSzFullOrderBookManager::InstrumentSnapshot instrument_snapshot =
            mFullOrderBookManager.snapshot_instrument(code.c_str(), now_time_ms);
        const ShSzFullOrderBookSummary& summary = instrument_snapshot.book_state.summary;
        std::ostringstream transition_line;
        transition_line << "[Trace][FullOrderBook][transition]"
            << " market=" << mMarket
            << " instrument=" << code
            << " trace_seq=" << trace_seq
            << " event=trade"
            << " source=" << source
            << " rcv_time=" << rcv_time
            << " event_time_ms=" << now_time_ms
            << " event_time=" << data->TradeTime
            << " appl_seq=" << data->ApplSeqNum
            << " bid_appl_seq=" << data->BidApplSeqNum
            << " offer_appl_seq=" << data->OfferApplSeqNum
            << " order_kind=" << data->OrderKind[0]
            << " bs_flag=" << data->OrderBSFlag[0]
            << " price=" << data->Price
            << " volume=" << data->Volume
            << " turnover=" << data->TurnOver
            << " transition_ok=" << BoolText(transition_ok)
            << " may_predict=" << BoolText(may_predict)
            << " did_predict=" << BoolText(did_predict)
            << " prediction=" << (did_predict ? prediction : std::numeric_limits<double>::quiet_NaN())
            << " pending_active=" << BoolText(instrument_snapshot.pending_market_order.active)
            << " pending_quote=" << instrument_snapshot.pending_market_order.quote_tag
            << " pending_volume=" << instrument_snapshot.pending_market_order.volume
            << " pending_price=" << NormalizeIntPrice(instrument_snapshot.pending_market_order.resolved_price)
            << " mid=" << NormalizeIntPrice(summary.mid_price)
            << " total_order_count=" << summary.total_order_count
            << " bid_levels=" << summary.bid.level_count
            << " ask_levels=" << summary.ask.level_count
            << " best_bid=" << NormalizeIntPrice(summary.bid.best_price)
            << " best_bid_volume=" << summary.bid.best_volume
            << " best_ask=" << NormalizeIntPrice(summary.ask.best_price)
            << " best_ask_volume=" << summary.ask.best_volume
            << " projected_bid1=" << (signal_snapshot != 0 ? NormalizeSnapshotPrice(signal_snapshot->BidPrice1) : 0.0)
            << " projected_bid1_volume=" << (signal_snapshot != 0 ? signal_snapshot->BidVolume1 : 0.0)
            << " projected_ask1=" << (signal_snapshot != 0 ? NormalizeSnapshotPrice(signal_snapshot->AskPrice1) : 0.0)
            << " projected_ask1_volume=" << (signal_snapshot != 0 ? signal_snapshot->AskVolume1 : 0.0)
            << " projected_mid=" << (signal_snapshot != 0 ? NormalizeSnapshotPrice(signal_snapshot->MidPrice) : 0.0)
            << " projected_market_time=" << (signal_snapshot != 0 ? signal_snapshot->MarketTime : 0.0);
        log_runtime_line(transition_line.str());
    }

    if (predictor_input == 0) {
        return;
    }

    const ShSzFullOb& aggregate = predictor_input->aggregate;
    if (!mFullOrderBookFactorTraceOnly) {
        std::ostringstream aggregate_line;
        aggregate_line << "[Trace][FullOrderBook][aggregate]"
            << " market=" << mMarket
            << " instrument=" << code
            << " trace_seq=" << trace_seq
            << " event=trade"
            << " valid=" << BoolText(aggregate.valid)
            << " mp=" << aggregate.mp
            << " bid_level1_price=" << aggregate.bid_level1.price
            << " bid_level1_volume=" << aggregate.bid_level1.volume_sum
            << " ask_level1_price=" << aggregate.ask_level1.price
            << " ask_level1_volume=" << aggregate.ask_level1.volume_sum
            << " bid_level5_volume=" << aggregate.bid_level5.volume_sum
            << " ask_level5_volume=" << aggregate.ask_level5.volume_sum
            << " bid_pct1_volume=" << aggregate.bid_01.volume_sum
            << " ask_pct1_volume=" << aggregate.ask_01.volume_sum
            << " bid_pct5_volume=" << aggregate.bid_05.volume_sum
            << " ask_pct5_volume=" << aggregate.ask_05.volume_sum
            << " bid_pct10_volume=" << aggregate.bid_10.volume_sum
            << " ask_pct10_volume=" << aggregate.ask_10.volume_sum
            << " bid_total_count=" << aggregate.bid_total_count
            << " ask_total_count=" << aggregate.ask_total_count
            << " bid_max_level_price=" << aggregate.bid_max_level_price
            << " ask_max_level_price=" << aggregate.ask_max_level_price
            << " bid_max_volume=" << aggregate.bid_max_volume
            << " ask_max_volume=" << aggregate.ask_max_volume;
        log_runtime_line(aggregate_line.str());
    }

    const ShSzOrderFlowSummary& order_flow = predictor_input->order_flow;
    const ShSzFullOrderBookFactorSet& factors = predictor_input->factors;
    std::ostringstream factor_line;
    factor_line << "[Trace][FullOrderBook][factor]"
        << " market=" << mMarket
        << " instrument=" << code
        << " trace_seq=" << trace_seq
        << " event=trade"
        << " event_time_ms=" << now_time_ms
        << " valid=" << BoolText(factors.valid)
        << " mid_price=" << predictor_input->aggregate.mp
        << " buy_order_volume=" << order_flow.buy_order_volume
        << " sell_order_volume=" << order_flow.sell_order_volume
        << " trade_pt=" << order_flow.trade_pt
        << " trade_nt=" << order_flow.trade_nt
        << " cxl_buy_flow=" << order_flow.cxl_buy_flow
        << " cxl_sell_flow=" << order_flow.cxl_sell_flow
        << " positive_fill_rate=" << factors.PositiveFillRate
        << " negative_fill_rate=" << factors.NegativeFillRate
        << " order_flow_imbalance=" << factors.OrderFlowImbalance
        << " cfr_imbalance=" << factors.CFRImbalance
        << " fix_dis_imbalance_pct1=" << factors.FixDisImbalancePct1
        << " fix_dis_imbalance_pct2=" << factors.FixDisImbalancePct2
        << " weighted_fix_dis_imbalance_pct1=" << factors.WeightedFixDisImbalancePct1
        << " weighted_fix_dis_imbalance_pct2=" << factors.WeightedFixDisImbalancePct2
        << " avg_size_imbalance=" << factors.AvgSizeImbalance
        << " avg_size_imbalance_level1=" << factors.AvgSizeImbalanceLevel1
        << " avg_size_imbalance_level5=" << factors.AvgSizeImbalanceLevel5
        << " order_count_imbalance=" << factors.OrderCountImbalance
        << " order_count_imbalance_level1=" << factors.OrderCountImbalanceLevel1
        << " order_count_imbalance_level5=" << factors.OrderCountImbalanceLevel5
        << " order_life_imbalance=" << factors.OrderLifeImbalance
        << " order_life_imbalance_level1=" << factors.OrderLifeImbalanceLevel1
        << " order_life_imbalance_level5=" << factors.OrderLifeImbalanceLevel5
        << " max_bid_distance=" << factors.MaxBidDistance
        << " max_ask_distance=" << factors.MaxAskDistance
        << " max_vol_distance_imbalance=" << factors.MaxVolDistanceImbalance
        << " young_orderbook_imbalance=" << factors.YoungOrderbookImbalance
        << " fix_dist_hermes=" << factors.FixDistHermes;
    log_runtime_line(factor_line.str());
}
