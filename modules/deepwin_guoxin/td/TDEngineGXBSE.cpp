/*****************************************************************************/
/* Guoxin BSE ATPQuantAPI trade adapter. */
/*****************************************************************************/

#include "TDEngineGXBSE.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <sstream>
#include <thread>

#ifndef T0_TD_SOURCE_ID
#define T0_TD_SOURCE_ID 180
#endif

#ifndef T0_TD_ENGINE_KEY
#define T0_TD_ENGINE_KEY "gxbse"
#endif

USING_WC_NAMESPACE

using namespace atp::quant_api;

namespace {

#ifndef GXBSE_TD_BUILD_VERSION
#define GXBSE_TD_BUILD_VERSION "unknown"
#endif

#ifndef T0_TD_PLUGIN_LABEL
#define T0_TD_PLUGIN_LABEL "gxbse_td"
#endif

constexpr const char* kGxbseTdBuildVersion = T0_TD_PLUGIN_LABEL ":" GXBSE_TD_BUILD_VERSION;

std::atomic<TDEngineGXBSE*> g_gxbse_direct_engine{nullptr};

class GXBSEQuantHandler : public ATPQuantHandler
{
public:
    GXBSEQuantHandler(TDEngineGXBSE* engine, int account_index)
        : engine_(engine), account_index_(account_index) {}

    void OnLogin(const ATPCustomerInfo& msg) override
    {
        if (engine_ != nullptr) {
            engine_->on_login(account_index_, msg);
        }
    }

    void OnLogout(const char* desc) override
    {
        if (engine_ != nullptr) {
            engine_->on_logout(account_index_, desc);
        }
    }

    void OnRecovering(const char* desc) override
    {
        if (engine_ != nullptr) {
            engine_->on_recovering(account_index_, desc);
        }
    }

    void OnRspCashAuctionOrder(const ATPRspCashAuctionOrderMsg& msg,
                               const ATPRspErrorInfo& error_info,
                               const int64_t request_id) override
    {
        if (engine_ != nullptr) {
            engine_->on_rsp_cash_auction_order(account_index_, msg, error_info, request_id);
        }
    }

    void OnRspCashCancelOrder(const ATPRspCashCancelOrderMsg& msg,
                              const ATPRspErrorInfo& error_info,
                              const int64_t request_id) override
    {
        if (engine_ != nullptr) {
            engine_->on_rsp_cash_cancel_order(account_index_, msg, error_info, request_id);
        }
    }

    void OnRtnCashAuctionOrder(const ATPRtnCashAuctionOrderMsg& msg) override
    {
        if (engine_ != nullptr) {
            engine_->on_rtn_cash_auction_order(account_index_, msg);
        }
    }

    void OnRspCashShareQueryResult(const ATPRspCashShareQueryResultMsg& msg,
                                   const int64_t request_id,
                                   const ATPRspErrorInfo& error_info,
                                   const bool isLast) override
    {
        if (engine_ != nullptr) {
            engine_->on_rsp_cash_share_query(account_index_, msg, request_id, error_info, isLast);
        }
    }

    void OnRspCashFundQueryResult(const ATPRspCashFundQueryResultMsg& msg,
                                  const int64_t request_id,
                                  const ATPRspErrorInfo& error_info,
                                  const bool isLast) override
    {
        if (engine_ != nullptr) {
            engine_->on_rsp_cash_fund_query(account_index_, msg, request_id, error_info, isLast);
        }
    }

    void OnRspCashOrderQueryResult(const ATPRspCashOrderQueryResultMsg& msg,
                                   const int64_t request_id,
                                   const ATPRspErrorInfo& error_info,
                                   const bool isLast) override
    {
        if (engine_ != nullptr) {
            engine_->on_rsp_cash_order_query(account_index_, msg, request_id, error_info, isLast);
        }
    }

    void OnRspCashExtQueryResultSecurityInfo(const ATPRspCashExtQueryResultSecurityInfoMsg& msg,
                                             const int64_t request_id,
                                             const ATPRspErrorInfo& error_info,
                                             const bool isLast) override
    {
        if (engine_ != nullptr) {
            engine_->on_rsp_cash_security_info_query(account_index_, msg, request_id, error_info, isLast);
        }
    }

private:
    TDEngineGXBSE* engine_;
    int account_index_;
};

uint64_t now_ns()
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

uint64_t realtime_ns()
{
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

const char* latency_name(const char* name)
{
    return name != nullptr ? name : "unknown";
}

std::string StripExchangeSuffix(const char* instrument_id)
{
    if (instrument_id == nullptr) {
        return "";
    }
    std::string instrument(instrument_id);
    const std::size_t dot_pos = instrument.find('.');
    if (dot_pos != std::string::npos) {
        instrument.resize(dot_pos);
    }
    return instrument;
}

const char* SecurityIdNoExchangeSuffix(const char* instrument_id)
{
    return instrument_id != nullptr ? instrument_id : "";
}

}

AccountUnitGXBSE::AccountUnitGXBSE(AccountUnitGXBSE&& other) noexcept
{
    *this = std::move(other);
}

AccountUnitGXBSE& AccountUnitGXBSE::operator=(AccountUnitGXBSE&& other) noexcept
{
    if (this != &other) {
        api = std::move(other.api);
        handler = std::move(other.handler);
        locations = std::move(other.locations);
        order_way = std::move(other.order_way);
        client_feature_code = std::move(other.client_feature_code);
        bind_ip_address = std::move(other.bind_ip_address);
        agw_user = std::move(other.agw_user);
        agw_password = std::move(other.agw_password);
        cust_id = std::move(other.cust_id);
        fund_account_id = std::move(other.fund_account_id);
        branch_id = std::move(other.branch_id);
        account_id = std::move(other.account_id);
        password = std::move(other.password);
        market_id = other.market_id;
        business_type = other.business_type;
        return_num = other.return_num;
        receive_thread_cpu = other.receive_thread_cpu;
        send_thread_cpu = other.send_thread_cpu;
        is_tcp_direct = other.is_tcp_direct;
        enable_latency = other.enable_latency;
        bypass_account_queries = other.bypass_account_queries;
        enable_position_sync = other.enable_position_sync;
        startup_cancel_all_orders = other.startup_cancel_all_orders;
        position_sync_interval_sec = other.position_sync_interval_sec;
        log_level = other.log_level;
        api_log_dir = std::move(other.api_log_dir);
        connected.store(other.connected.load());
        logged_in.store(other.logged_in.load());
        periodic_position_query_inflight.store(other.periodic_position_query_inflight.load());
        periodic_account_query_inflight.store(other.periodic_account_query_inflight.load());
        startup_order_query_rid = other.startup_order_query_rid;
        startup_cancel_submitted_clordnos = std::move(other.startup_cancel_submitted_clordnos);
    }
    return *this;
}

TDEngineGXBSE::TDEngineGXBSE() : ITDEngine(T0_TD_SOURCE_ID)
{
    next_request_id_.store(static_cast<int64_t>(now_ns() & 0x7fffffff));
    next_latency_log_ns_ = now_ns() + latency_log_interval_ns_;
    g_gxbse_direct_engine.store(this, std::memory_order_release);
}

TDEngineGXBSE::~TDEngineGXBSE()
{
    TDEngineGXBSE* expected = this;
    g_gxbse_direct_engine.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel);
    release_api();
    stop_async_info_logger(true);
}

void TDEngineGXBSE::ensure_async_info_logger_started()
{
    std::lock_guard<std::mutex> lock(async_info_logger_mutex_);
    if (async_info_logger_started_.load(std::memory_order_acquire)) {
        return;
    }
    async_info_logger_stop_requested_.store(false, std::memory_order_release);
    async_info_logger_thread_ = std::thread(&TDEngineGXBSE::async_info_logger_loop, this);
    async_info_logger_started_.store(true, std::memory_order_release);
}

void TDEngineGXBSE::stop_async_info_logger(bool drain)
{
    std::thread worker;
    {
        std::lock_guard<std::mutex> lock(async_info_logger_mutex_);
        if (!async_info_logger_started_.load(std::memory_order_acquire)) {
            return;
        }
        async_info_logger_stop_requested_.store(true, std::memory_order_release);
        worker = std::move(async_info_logger_thread_);
    }
    if (worker.joinable()) {
        worker.join();
    }
    {
        std::lock_guard<std::mutex> lock(async_info_logger_mutex_);
        async_info_logger_started_.store(false, std::memory_order_release);
    }
}

void TDEngineGXBSE::enqueue_td_order_send_log(const TdOrderSendLogEntry& entry)
{
    if (!async_info_logger_started_.load(std::memory_order_acquire)) {
        ensure_async_info_logger_started();
    }
    if (async_info_logger_stop_requested_.load(std::memory_order_acquire)) {
        log_td_order_send_entry(entry);
        return;
    }

    const uint64_t tail = td_order_send_log_tail_.load(std::memory_order_acquire);
    const uint64_t head = td_order_send_log_head_.load(std::memory_order_acquire);
    if (tail - head >= kTdOrderSendLogRingCapacity) {
        td_order_send_log_dropped_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    TdOrderSendLogEntry copy = entry;
    copy.dropped_before = td_order_send_log_dropped_.load(std::memory_order_relaxed);
    td_order_send_log_ring_[tail % kTdOrderSendLogRingCapacity] = copy;
    td_order_send_log_tail_.store(tail + 1, std::memory_order_release);
}

void TDEngineGXBSE::log_td_order_send_entry(const TdOrderSendLogEntry& entry)
{
    const uint64_t now = now_ns();
    const uint64_t log_queue_delay_ns = now >= entry.enqueue_begin_ns ? now - entry.enqueue_begin_ns : 0;
    std::ostringstream line;
    line << "[TdOrderSend]"
         << " engine=" << T0_TD_ENGINE_KEY
         << " request_id=" << entry.request_id
         << " atp_request_id=" << entry.atp_request_id
         << " order_ref=" << entry.order_ref
         << " instrument=" << entry.instrument
         << " td_func_enter_ns=" << entry.func_enter_ns
         << " td_enter_ns=" << entry.send_time_ns
         << " td_send_realtime_ns=" << entry.send_realtime_ns
         << " td_send_done_ns=" << entry.api_return_time_ns
         << " td_log_begin_ns=" << entry.log_begin_ns
         << " td_log_queue_delay_ns=" << log_queue_delay_ns
         << " td_log_dropped_before=" << entry.dropped_before
         << " api_send_ns=" << entry.api_send_ns
         << " total_ns=" << entry.api_send_ns
         << " func_to_msg_new_begin_ns=" << entry.func_to_msg_new_begin_ns
         << " msg_new_ns=" << entry.msg_new_ns
         << " msg_fill_ns=" << entry.msg_fill_ns
         << " msg_fill_account_ns=" << entry.msg_fill_account_ns
         << " msg_fill_security_ns=" << entry.msg_fill_security_ns
         << " msg_fill_order_ns=" << entry.msg_fill_order_ns
         << " msg_fill_password_batch_ns=" << entry.msg_fill_password_batch_ns
         << " route_init_ns=" << entry.route_init_ns
         << " route_store_before_api_ns=" << entry.route_store_before_api_ns
         << " latency_store_ns=" << entry.latency_store_ns
         << " api_return_to_route_store_ns=" << entry.api_return_to_route_store_ns
         << " route_update_after_api_ns=" << entry.route_update_after_api_ns
         << " msg_delete_ns=" << entry.msg_delete_ns
         << " pre_log_total_ns=" << entry.pre_log_total_ns;
    KF_LOG_INFO(logger, line.str());
}

void TDEngineGXBSE::async_info_logger_loop()
{
    while (true) {
        const uint64_t head = td_order_send_log_head_.load(std::memory_order_relaxed);
        const uint64_t tail = td_order_send_log_tail_.load(std::memory_order_acquire);
        if (head < tail) {
            const TdOrderSendLogEntry entry = td_order_send_log_ring_[head % kTdOrderSendLogRingCapacity];
            td_order_send_log_head_.store(head + 1, std::memory_order_release);
            log_td_order_send_entry(entry);
            continue;
        }
        if (async_info_logger_stop_requested_.load(std::memory_order_acquire)) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void TDEngineGXBSE::record_latency(LatencyCounter& counter, uint64_t latency_ns)
{
    ++counter.count;
    counter.total_ns += latency_ns;
    if (latency_ns > counter.max_ns) {
        counter.max_ns = latency_ns;
    }
}

bool TDEngineGXBSE::consume_request_send_latency(int64_t request_id, uint64_t* latency_ns)
{
    std::lock_guard<std::mutex> lock(latency_mutex_);
    auto it = request_send_time_ns_.find(request_id);
    if (it == request_send_time_ns_.end()) {
        return false;
    }
    if (latency_ns != nullptr) {
        *latency_ns = now_ns() - it->second;
    }
    request_send_time_ns_.erase(it);
    return true;
}

bool TDEngineGXBSE::peek_request_send_latency(int64_t request_id, uint64_t* latency_ns)
{
    std::lock_guard<std::mutex> lock(latency_mutex_);
    auto it = request_send_time_ns_.find(request_id);
    if (it == request_send_time_ns_.end()) {
        return false;
    }
    if (latency_ns != nullptr) {
        *latency_ns = now_ns() - it->second;
    }
    return true;
}

void TDEngineGXBSE::erase_request_send_latency(int64_t request_id)
{
    std::lock_guard<std::mutex> lock(latency_mutex_);
    request_send_time_ns_.erase(request_id);
}

void TDEngineGXBSE::maybe_log_latency_stats(uint64_t now_ns_value)
{
    std::lock_guard<std::mutex> lock(latency_mutex_);
    if (next_latency_log_ns_ == 0) {
        next_latency_log_ns_ = now_ns_value + latency_log_interval_ns_;
        return;
    }
    if (now_ns_value < next_latency_log_ns_) {
        return;
    }
    next_latency_log_ns_ = now_ns_value + latency_log_interval_ns_;

    const auto append_counter = [](std::ostringstream& oss, const char* name, const LatencyCounter& counter) {
        const uint64_t avg_ns = counter.count == 0 ? 0 : (counter.total_ns / counter.count);
        oss << ' ' << latency_name(name)
            << "_count=" << counter.count
            << ' ' << latency_name(name)
            << "_avg_ns=" << avg_ns
            << ' ' << latency_name(name)
            << "_max_ns=" << counter.max_ns;
    };

    std::ostringstream oss;
    oss << "[gxbse_td_stats]";
    append_counter(oss, "insert_rsp", insert_rsp_latency_);
    append_counter(oss, "insert_first_rtn", insert_first_rtn_latency_);
    append_counter(oss, "cancel_rsp", cancel_rsp_latency_);
    append_counter(oss, "account_query", account_query_latency_);
    append_counter(oss, "position_query", position_query_latency_);
    append_counter(oss, "order_query", order_query_latency_);
    append_counter(oss, "limit_price_query", limit_price_query_latency_);
    append_counter(oss, "periodic_position_query", periodic_position_query_latency_);
    KF_LOG_INFO(logger, oss.str());

    insert_rsp_latency_ = LatencyCounter();
    insert_first_rtn_latency_ = LatencyCounter();
    cancel_rsp_latency_ = LatencyCounter();
    account_query_latency_ = LatencyCounter();
    position_query_latency_ = LatencyCounter();
    order_query_latency_ = LatencyCounter();
    limit_price_query_latency_ = LatencyCounter();
    periodic_position_query_latency_ = LatencyCounter();
}

void TDEngineGXBSE::init()
{
    logger = yijinjing::KfLog::getLogger("TradeEngine." T0_TD_ENGINE_KEY);
    ITDEngine::init();
    ensure_async_info_logger_started();
    KF_LOG_INFO(logger, "[BuildVersion] component=" << kGxbseTdBuildVersion);
}

void TDEngineGXBSE::pre_load(const json& j_config)
{
    (void)j_config;
}

void TDEngineGXBSE::resize_accounts(int account_num)
{
    account_units_.resize(static_cast<std::size_t>(std::max(0, account_num)));
}

TradeAccount TDEngineGXBSE::load_account(int idx, const json& j_config)
{
    if (idx >= static_cast<int>(account_units_.size())) {
        resize_accounts(idx + 1);
    }
    AccountUnitGXBSE& unit = account_units_[static_cast<std::size_t>(idx)];

    unit.cust_id = json_string_or(j_config, "cust_id", json_string_or(j_config, WC_CONFIG_KEY_INVESTOR_ID, ""));
    unit.fund_account_id = json_string_or(j_config, "fund_account_id", unit.cust_id);
    unit.branch_id = json_string_or(j_config, "branch_id", "");
    unit.account_id = json_string_or(j_config, "account_id", "");
    unit.password = json_string_or(j_config, "trade_password",
        json_string_or(j_config, "password", json_string_or(j_config, WC_CONFIG_KEY_PASSWORD, "")));
    unit.order_way = json_string_or(j_config, "order_way", unit.order_way);
    unit.client_feature_code = json_string_or(j_config, "client_feature_code", unit.client_feature_code);
    unit.bind_ip_address = json_string_or(j_config, "bind_ip_address",
        json_string_or(j_config, "bind_ip", unit.bind_ip_address));
    unit.agw_user = json_string_or(j_config, "agw_user", unit.agw_user);
    unit.agw_password = json_string_or(j_config, "agw_password", unit.agw_password);
    unit.api_log_dir = json_string_or(j_config, "api_log_dir", unit.api_log_dir);
    unit.market_id = json_int_or(j_config, "market_id", unit.market_id);
    unit.business_type = json_int_or(j_config, "business_type", unit.business_type);
    unit.return_num = json_int_or(j_config, "return_num", unit.return_num);
    unit.receive_thread_cpu = json_int_or(j_config, "receive_thread_cpu",
        json_int_or(j_config, "recevie_thread_cpu", unit.receive_thread_cpu));
    unit.send_thread_cpu = json_int_or(j_config, "send_thread_cpu", unit.send_thread_cpu);
    unit.is_tcp_direct = json_bool_or(j_config, "is_tcp_direct", unit.is_tcp_direct);
    unit.enable_latency = json_bool_or(j_config, "enable_latency", unit.enable_latency);
    unit.bypass_account_queries = json_bool_or(j_config, "bypass_account_queries", unit.bypass_account_queries);
    unit.enable_position_sync = json_bool_or(j_config, "enable_position_sync", unit.enable_position_sync);
    unit.startup_cancel_all_orders = json_bool_or(j_config, "startup_cancel_all_orders", unit.startup_cancel_all_orders);
    unit.position_sync_interval_sec = std::max(1, json_int_or(j_config, "position_sync_interval_sec", unit.position_sync_interval_sec));
    unit.log_level = json_int_or(j_config, "log_level", unit.log_level);
    unit.locations = join_locations(j_config);

    TradeAccount account = {};
    copy_text(account.BrokerID, sizeof(account.BrokerID), json_string_or(j_config, WC_CONFIG_KEY_BROKER_ID, ""));
    copy_text(account.UserID, sizeof(account.UserID), json_string_or(j_config, WC_CONFIG_KEY_USER_ID, unit.cust_id));
    copy_text(account.InvestorID, sizeof(account.InvestorID), unit.cust_id);
    copy_text(account.Password, sizeof(account.Password), unit.password);

    KF_LOG_INFO(logger, "[load_account] gxbse idx=" << idx
        << " cust_id=" << unit.cust_id
        << " fund_account_id=" << unit.fund_account_id
        << " account_id=" << unit.account_id
        << " market_id=" << unit.market_id
        << " locations=" << unit.locations
        << " order_way=" << unit.order_way
        << " bypass_account_queries=" << (unit.bypass_account_queries ? 1 : 0)
        << " enable_position_sync=" << (unit.enable_position_sync ? 1 : 0)
        << " startup_cancel_all_orders=" << (unit.startup_cancel_all_orders ? 1 : 0)
        << " position_sync_interval_sec=" << unit.position_sync_interval_sec);
    return account;
}

void TDEngineGXBSE::connect(long timeout_nsec)
{
    (void)timeout_nsec;
    bool expected = false;
    if (api_initialized_.compare_exchange_strong(expected, true)) {
        ATPProperties init_props;
        init_props.SetValue(prop::kLogLevel, static_cast<uint8_t>(ATPLogLevel::kInfo));
        init_props.SetValue(prop::kEncryptSchema, static_cast<uint8_t>(ATPEncryptModeConst::kTransMode));
        init_props.SetValue(prop::kIsEnableLatency, false);
        ATPErrorCodeType ec = ATPQuantAPI::Init(&init_props);
        if (ec != ATPErrorCode::kSuccess) {
            api_initialized_.store(false);
            KF_LOG_ERROR(logger, "[connect] ATPQuantAPI::Init failed ec=" << ec);
            return;
        }
    }

    for (std::size_t i = 0; i < account_units_.size(); ++i) {
        AccountUnitGXBSE& unit = account_units_[i];
        if (!unit.handler) {
            unit.handler.reset(new GXBSEQuantHandler(this, static_cast<int>(i)));
        }
        if (!unit.api) {
            ATPProperties props;
            props.SetValue(prop::kLogLevel, static_cast<uint8_t>(unit.log_level));
            if (!unit.api_log_dir.empty()) {
                props.SetValue(prop::kCommonLogPath, unit.api_log_dir.c_str());
                props.SetValue(prop::kIndicatorLogPath, unit.api_log_dir.c_str());
                props.SetValue(prop::kIoLogPath, unit.api_log_dir.c_str());
            }
            props.SetValue(prop::kEncryptSchema, static_cast<uint8_t>(ATPEncryptModeConst::kTransMode));
            props.SetValue(prop::kIsEnableLatency, unit.enable_latency);
            props.SetValue(prop::kCallbackResourceMode, static_cast<uint8_t>(ATPCallbackResourceModeConst::kLowLatencyMode));
            props.SetValue(prop::kRecevieThreadCpu, static_cast<uint8_t>(unit.receive_thread_cpu));
            props.SetValue(prop::kSendThreadCpu, static_cast<uint8_t>(unit.send_thread_cpu));
            props.SetValue(prop::kIsTcpDirect, unit.is_tcp_direct);
            props.SetValue(prop::kBindIpAddress, unit.bind_ip_address.c_str());
            props.SetValue(prop::kLocations, unit.locations.c_str());
            props.SetValue(prop::kAgwUser, unit.agw_user.c_str());
            props.SetValue(prop::kAgwPassword, unit.agw_password.c_str());
            props.SetValue(prop::kRetransmitMode, static_cast<uint8_t>(ATPRetransmitModeConst::kQuickMode));
            props.SetValue(prop::kMultiChannelFlag, static_cast<uint8_t>(ATPMultiChannelFlagConst::kDefault));
            props.SetValue(prop::kConnectionProtocol, static_cast<uint8_t>(ATPConnectionProtocolConst::kTCPProtocol));
            props.SetValue(prop::kOrderWay, unit.order_way.c_str());
            props.SetValue(prop::kClientFeatureCode, unit.client_feature_code.c_str());
            unit.api.reset(new ATPQuantAPI(unit.handler.get(), &props));
        }
        unit.connected.store(true);
    }
}

void TDEngineGXBSE::login(long timeout_nsec)
{
    if (!is_connected()) {
        connect(timeout_nsec);
    }

    std::size_t login_sent = 0;
    for (std::size_t i = 0; i < account_units_.size(); ++i) {
        AccountUnitGXBSE& unit = account_units_[i];
        if (!unit.api) {
            continue;
        }
        if (unit.bypass_account_queries) {
            unit.connected.store(true);
            unit.logged_in.store(true);
            login_ok();
            login_cv_.notify_all();
            ++login_sent;
            KF_LOG_INFO(logger, "[login] bypass account queries account_index=" << i
                << " cust_id=" << unit.cust_id);
            continue;
        }
        ATPLoginProperty* login = ATPLoginProperty::NewMessage();
        login->SetUserId(unit.cust_id.c_str());
        login->SetBranchId(unit.branch_id.c_str());
        login->SetPassword(unit.password.c_str());
        login->SetLoginMode(ATPLoginModeConst::kCustIDMode);
        login->SetConnectChannel(ATPConnectChannelConst::kSoftChannel);
        ATPErrorCodeType ec = unit.api->Login(login);
        ATPLoginProperty::DeleteMessage(login);
        if (ec != ATPErrorCode::kSuccess) {
            KF_LOG_ERROR(logger, "[login] failed account_index=" << i << " ec=" << ec);
            continue;
        }
        ++login_sent;
        KF_LOG_INFO(logger, "[login] sent account_index=" << i << " cust_id=" << unit.cust_id);
    }

    if (login_sent == 0) {
        KF_LOG_ERROR(logger, "[login] no login request sent");
        return;
    }

    const long wait_nsec = timeout_nsec > 0 ? timeout_nsec : 5L * 1000L * 1000L * 1000L;
    const auto wait_duration = std::chrono::nanoseconds(wait_nsec);
    std::unique_lock<std::mutex> lock(login_mutex_);
    const bool ok = login_cv_.wait_for(lock, wait_duration, [this]() {
        return is_logged_in();
    });
    if (ok) {
        KF_LOG_INFO(logger, "[login] completed account_count=" << account_units_.size());
        start_position_sync_thread();
    } else {
        KF_LOG_ERROR(logger, "[login] timeout wait_nsec=" << wait_nsec
            << " logged_in=" << is_logged_in());
    }
}

void TDEngineGXBSE::logout()
{
    stop_position_sync_thread();
    for (auto& unit : account_units_) {
        if (unit.api) {
            unit.api->Logout();
        }
        unit.logged_in.store(false);
        unit.connected.store(false);
    }
}

void TDEngineGXBSE::release_api()
{
    stop_position_sync_thread();
    for (auto& unit : account_units_) {
        if (unit.api) {
            unit.api->Logout();
            unit.api->Release();
            unit.api.reset();
        }
        unit.handler.reset();
        unit.logged_in.store(false);
        unit.connected.store(false);
    }
    if (api_initialized_.exchange(false)) {
        ATPQuantAPI::Stop();
    }
}

bool TDEngineGXBSE::is_connected() const
{
    if (account_units_.empty()) {
        return false;
    }
    for (const auto& unit : account_units_) {
        if (!unit.connected.load()) {
            return false;
        }
    }
    return true;
}

bool TDEngineGXBSE::is_logged_in() const
{
    if (account_units_.empty()) {
        return false;
    }
    for (const auto& unit : account_units_) {
        if (!unit.logged_in.load()) {
            return false;
        }
    }
    return true;
}

void TDEngineGXBSE::pre_run()
{
    if (accounts.empty()) {
        KF_LOG_ERROR(logger, "[pre_run] accounts is empty");
        return;
    }

    for (std::size_t i = 0; i < accounts.size(); ++i) {
        TradeAccount& acc = accounts[i];
        AccountUnitGXBSE* unit = unit_at(static_cast<int>(i));
        if (unit == nullptr) {
            continue;
        }
        const int account_rid_base = pre_run_rid + static_cast<int>(i) * 10;
        const int startup_order_query_rid = account_rid_base + 2;

        KF_LOG_INFO(logger, "[StartupRiskQuery] account_index=" << i
            << " account_query=disabled_pre_run"
            << " position_query=disabled_pre_run");

        if (unit->startup_cancel_all_orders) {
            {
                std::lock_guard<std::mutex> lock(unit->startup_cancel_mutex);
                unit->startup_cancel_submitted_clordnos.clear();
            }
            unit->startup_order_query_rid = startup_order_query_rid;

            LFQryOrderField req_order = {};
            copy_text(req_order.InvestorID, sizeof(req_order.InvestorID), acc.InvestorID);
            req_qry_order_info(&req_order, static_cast<int>(i), startup_order_query_rid);
            KF_LOG_INFO(logger, "[StartupCancelInit] account_index=" << i
                << " rid=" << startup_order_query_rid
                << " enabled=1");
        } else {
            unit->startup_order_query_rid = -1;
            KF_LOG_INFO(logger, "[StartupCancelInit] account_index=" << i << " enabled=0");
        }
    }
}

int TDEngineGXBSE::req_order_insert(const LFInputOrderField* data, int account_index, int requestId, long rcv_time)
{
    (void)rcv_time;
    const uint64_t func_enter_ns = now_ns();
    AccountUnitGXBSE* unit = unit_at(account_index);
    if (unit == nullptr || !unit->api || data == nullptr) {
        return -1;
    }

    const uint64_t msg_new_begin_ns = now_ns();
    ATPReqCashAuctionOrderMsg* msg = ATPReqCashAuctionOrderMsg::NewMessage(resolve_business_type(*unit));
    const uint64_t msg_new_done_ns = now_ns();
    if (msg == nullptr) {
        KF_LOG_ERROR(logger, "[req_order_insert] NewMessage failed");
        return -1;
    }
    msg->SetCustId(unit->cust_id.c_str());
    msg->SetFundAccountId(unit->fund_account_id.c_str());
    msg->SetBranchId(unit->branch_id.c_str());
    msg->SetAccountId(unit->account_id.c_str());
    const uint64_t msg_account_done_ns = now_ns();
    msg->SetSecurityId(SecurityIdNoExchangeSuffix(data->InstrumentID));
    msg->SetMarketId(resolve_market_id(*unit, data->ExchangeID));
    const uint64_t msg_security_done_ns = now_ns();
    msg->SetSide(to_atp_side(data->Direction));
    msg->SetOrderQty(static_cast<double>(data->Volume));
    msg->SetPrice(data->LimitPrice);
    msg->SetOrderType(to_atp_order_type(data->OrderPriceType));
    const uint64_t msg_order_done_ns = now_ns();
    msg->SetPassword(unit->password.c_str());
    msg->SetBatchClOrdNo(static_cast<uint64_t>(std::max<long>(1, data->OrderRef)));
    const uint64_t msg_fill_done_ns = now_ns();

    const int64_t atp_request_id = next_request_id();
    OrderRoute route;
    route.order_ref = data->OrderRef;
    route.request_id = requestId;
    route.account_index = account_index;
    route.instrument = data->InstrumentID;
    route.price = data->LimitPrice;
    route.volume = data->Volume;
    route.direction = data->Direction;
    route.order_price_type = data->OrderPriceType;
    route.time_condition = data->TimeCondition;
    route.volume_condition = data->VolumeCondition;
    route.offset_flag = data->OffsetFlag;
    route.direct_submit = false;
    route.send_time_ns = now_ns();
    const uint64_t route_send_realtime_ns = realtime_ns();
    const uint64_t route_init_done_ns = route.send_time_ns;
    store_request_route(atp_request_id, route);
    const uint64_t route_store_before_api_done_ns = now_ns();
    const uint64_t latency_store_done_ns = route_store_before_api_done_ns;

    ATPErrorCodeType ec = unit->api->ReqCashAuctionOrder(msg, atp_request_id);
    const uint64_t api_return_time_ns = now_ns();
    route.api_return_time_ns = api_return_time_ns;
    update_request_route_api_return_time(atp_request_id, api_return_time_ns);
    const uint64_t route_update_after_api_done_ns = api_return_time_ns;
    ATPReqCashAuctionOrderMsg::DeleteMessage(msg);
    const uint64_t msg_delete_done_ns = now_ns();
    if (ec != ATPErrorCode::kSuccess) {
        erase_request_send_latency(atp_request_id);
        LFInputOrderField err = *data;
        int rid = requestId;
        on_rsp_order_insert(&err, rid, ec, "gxbse_send_order_failed");
        KF_LOG_ERROR(logger, "[req_order_insert] failed ref=" << data->OrderRef << " ec=" << ec);
        return -1;
    }
    const uint64_t log_begin_ns = now_ns();
    TdOrderSendLogEntry log_entry;
    log_entry.request_id = requestId;
    log_entry.atp_request_id = atp_request_id;
    log_entry.order_ref = data->OrderRef;
    copy_text(log_entry.instrument, sizeof(log_entry.instrument), data->InstrumentID);
    log_entry.func_enter_ns = func_enter_ns;
    log_entry.send_time_ns = route.send_time_ns;
    log_entry.api_return_time_ns = api_return_time_ns;
    log_entry.log_begin_ns = log_begin_ns;
    log_entry.send_realtime_ns = route_send_realtime_ns;
    log_entry.api_send_ns = (api_return_time_ns >= route.send_time_ns) ? api_return_time_ns - route.send_time_ns : 0;
    log_entry.func_to_msg_new_begin_ns = (msg_new_begin_ns >= func_enter_ns) ? msg_new_begin_ns - func_enter_ns : 0;
    log_entry.msg_new_ns = (msg_new_done_ns >= msg_new_begin_ns) ? msg_new_done_ns - msg_new_begin_ns : 0;
    log_entry.msg_fill_ns = (msg_fill_done_ns >= msg_new_done_ns) ? msg_fill_done_ns - msg_new_done_ns : 0;
    log_entry.msg_fill_account_ns = (msg_account_done_ns >= msg_new_done_ns) ? msg_account_done_ns - msg_new_done_ns : 0;
    log_entry.msg_fill_security_ns =
        (msg_security_done_ns >= msg_account_done_ns) ? msg_security_done_ns - msg_account_done_ns : 0;
    log_entry.msg_fill_order_ns =
        (msg_order_done_ns >= msg_security_done_ns) ? msg_order_done_ns - msg_security_done_ns : 0;
    log_entry.msg_fill_password_batch_ns =
        (msg_fill_done_ns >= msg_order_done_ns) ? msg_fill_done_ns - msg_order_done_ns : 0;
    log_entry.route_init_ns = (route_init_done_ns >= msg_fill_done_ns) ? route_init_done_ns - msg_fill_done_ns : 0;
    log_entry.route_store_before_api_ns =
        (route_store_before_api_done_ns >= route_init_done_ns) ? route_store_before_api_done_ns - route_init_done_ns : 0;
    log_entry.latency_store_ns =
        (latency_store_done_ns >= route_store_before_api_done_ns) ? latency_store_done_ns - route_store_before_api_done_ns : 0;
    log_entry.route_update_after_api_ns =
        (route_update_after_api_done_ns >= api_return_time_ns) ? route_update_after_api_done_ns - api_return_time_ns : 0;
    log_entry.api_return_to_route_store_ns = log_entry.route_update_after_api_ns;
    log_entry.msg_delete_ns =
        (msg_delete_done_ns >= route_update_after_api_done_ns) ? msg_delete_done_ns - route_update_after_api_done_ns : 0;
    log_entry.pre_log_total_ns = (log_begin_ns >= func_enter_ns) ? log_begin_ns - func_enter_ns : 0;
    log_entry.enqueue_begin_ns = now_ns();
    enqueue_td_order_send_log(log_entry);
    return 0;
}

int TDEngineGXBSE::direct_cash_order(const GxbseDirectOrderRequest* request, GxbseDirectOrderResult* result)
{
    const bool want_result = (result != nullptr);
    if (result != nullptr) {
        *result = GxbseDirectOrderResult{};
        result->ret = -1;
    }
    const uint64_t func_enter_ns = now_ns();
    if (request == nullptr || request->source_id != source_id || request->instrument_id == nullptr ||
        request->request_id < 0 || request->order_ref <= 0 || request->price <= 0.0 || request->volume <= 0) {
        return -1;
    }
    AccountUnitGXBSE* unit = unit_at(request->account_index);
    if (unit == nullptr || !unit->api) {
        return -1;
    }

    if (!want_result) {
        ATPReqCashAuctionOrderMsg* msg = ATPReqCashAuctionOrderMsg::NewMessage(static_cast<uint8_t>(unit->business_type));
        if (msg == nullptr) {
            KF_LOG_ERROR(logger, "[GxbseDirectOrder] NewMessage failed request_id=" << request->request_id);
            return -1;
        }
        msg->SetCustId(unit->cust_id.c_str());
        msg->SetFundAccountId(unit->fund_account_id.c_str());
        msg->SetBranchId(unit->branch_id.c_str());
        msg->SetAccountId(unit->account_id.c_str());
        msg->SetSecurityId(SecurityIdNoExchangeSuffix(request->instrument_id));
        msg->SetMarketId(static_cast<uint16_t>(unit->market_id));
        msg->SetSide(to_atp_side(request->direction));
        msg->SetOrderQty(static_cast<double>(request->volume));
        msg->SetPrice(request->price);
        msg->SetOrderType(to_atp_order_type(request->order_price_type));
        msg->SetPassword(unit->password.c_str());
        msg->SetBatchClOrdNo(static_cast<uint64_t>(request->order_ref));

        const int64_t atp_request_id = next_request_id();
        OrderRoute route;
        route.order_ref = request->order_ref;
        route.request_id = request->request_id;
        route.account_index = request->account_index;
        route.instrument = request->instrument_id;
        route.price = request->price;
        route.volume = request->volume;
        route.direction = request->direction;
        route.order_price_type = request->order_price_type;
        route.time_condition = request->time_condition;
        route.volume_condition = request->volume_condition;
        route.offset_flag = request->offset_flag;
        route.direct_submit = true;
        route.send_time_ns = now_ns();
        const uint64_t route_send_realtime_ns = realtime_ns();
        store_request_route(atp_request_id, route);

        ATPErrorCodeType ec = unit->api->ReqCashAuctionOrder(msg, atp_request_id);
        const uint64_t api_return_time_ns = now_ns();
        update_request_route_api_return_time(atp_request_id, api_return_time_ns);
        ATPReqCashAuctionOrderMsg::DeleteMessage(msg);

        TdOrderSendLogEntry log_entry;
        log_entry.request_id = request->request_id;
        log_entry.atp_request_id = atp_request_id;
        log_entry.order_ref = request->order_ref;
        copy_text(log_entry.instrument, sizeof(log_entry.instrument), request->instrument_id);
        log_entry.func_enter_ns = func_enter_ns;
        log_entry.send_time_ns = route.send_time_ns;
        log_entry.api_return_time_ns = api_return_time_ns;
        log_entry.log_begin_ns = api_return_time_ns;
        log_entry.send_realtime_ns = route_send_realtime_ns;
        log_entry.api_send_ns = api_return_time_ns >= route.send_time_ns ? api_return_time_ns - route.send_time_ns : 0;
        log_entry.pre_log_total_ns = api_return_time_ns >= func_enter_ns ? api_return_time_ns - func_enter_ns : 0;
        log_entry.enqueue_begin_ns = now_ns();
        enqueue_td_order_send_log(log_entry);

        if (ec != ATPErrorCode::kSuccess) {
            LFInputOrderField err = {};
            copy_text(err.InstrumentID, sizeof(err.InstrumentID), request->instrument_id);
            copy_text(err.ExchangeID, sizeof(err.ExchangeID), "BSE");
            err.OrderRef = request->order_ref;
            err.LimitPrice = request->price;
            err.Volume = request->volume;
            err.Direction = request->direction;
            err.OffsetFlag = request->offset_flag;
            err.OrderPriceType = request->order_price_type;
            int response_request_id = request->request_id;
            on_rsp_order_insert(&err, response_request_id, ec, "gxbse_direct_send_order_failed");
            KF_LOG_ERROR(logger, "[GxbseDirectOrder] failed request_id=" << request->request_id
                << " order_ref=" << request->order_ref
                << " ec=" << ec);
            return -1;
        }
        return request->request_id;
    }

    const uint64_t msg_new_begin_ns = now_ns();
    ATPReqCashAuctionOrderMsg* msg = ATPReqCashAuctionOrderMsg::NewMessage(static_cast<uint8_t>(unit->business_type));
    const uint64_t msg_new_done_ns = now_ns();
    if (msg == nullptr) {
        KF_LOG_ERROR(logger, "[GxbseDirectOrder] NewMessage failed request_id=" << request->request_id);
        return -1;
    }
    msg->SetCustId(unit->cust_id.c_str());
    msg->SetFundAccountId(unit->fund_account_id.c_str());
    msg->SetBranchId(unit->branch_id.c_str());
    msg->SetAccountId(unit->account_id.c_str());
    const uint64_t msg_account_done_ns = now_ns();
    msg->SetSecurityId(SecurityIdNoExchangeSuffix(request->instrument_id));
    msg->SetMarketId(static_cast<uint16_t>(unit->market_id));
    const uint64_t msg_security_done_ns = now_ns();
    msg->SetSide(to_atp_side(request->direction));
    msg->SetOrderQty(static_cast<double>(request->volume));
    msg->SetPrice(request->price);
    msg->SetOrderType(to_atp_order_type(request->order_price_type));
    const uint64_t msg_order_done_ns = now_ns();
    msg->SetPassword(unit->password.c_str());
    msg->SetBatchClOrdNo(static_cast<uint64_t>(request->order_ref));
    const uint64_t msg_fill_done_ns = now_ns();

    const int64_t atp_request_id = next_request_id();
    OrderRoute route;
    route.order_ref = request->order_ref;
    route.request_id = request->request_id;
    route.account_index = request->account_index;
    route.instrument = request->instrument_id;
    route.price = request->price;
    route.volume = request->volume;
    route.direction = request->direction;
    route.order_price_type = request->order_price_type;
    route.time_condition = request->time_condition;
    route.volume_condition = request->volume_condition;
    route.offset_flag = request->offset_flag;
    route.direct_submit = true;
    route.send_time_ns = now_ns();
    const uint64_t route_send_realtime_ns = realtime_ns();
    const uint64_t route_init_done_ns = route.send_time_ns;
    store_request_route(atp_request_id, route);
    const uint64_t route_store_before_api_done_ns = now_ns();

    ATPErrorCodeType ec = unit->api->ReqCashAuctionOrder(msg, atp_request_id);
    const uint64_t api_return_time_ns = now_ns();
    route.api_return_time_ns = api_return_time_ns;
    update_request_route_api_return_time(atp_request_id, api_return_time_ns);
    const uint64_t route_update_after_api_done_ns = api_return_time_ns;
    ATPReqCashAuctionOrderMsg::DeleteMessage(msg);
    const uint64_t msg_delete_done_ns = now_ns();
    const uint64_t log_begin_ns = now_ns();

    if (want_result) {
        result->ret = (ec == ATPErrorCode::kSuccess) ? request->request_id : -1;
        result->atp_request_id = atp_request_id;
        result->func_enter_ns = func_enter_ns;
        result->msg_new_ns = msg_new_done_ns >= msg_new_begin_ns ? msg_new_done_ns - msg_new_begin_ns : 0;
        result->msg_fill_ns = msg_fill_done_ns >= msg_new_done_ns ? msg_fill_done_ns - msg_new_done_ns : 0;
        result->route_init_ns = route_init_done_ns >= msg_fill_done_ns ? route_init_done_ns - msg_fill_done_ns : 0;
        result->route_store_before_api_ns =
            route_store_before_api_done_ns >= route_init_done_ns ? route_store_before_api_done_ns - route_init_done_ns : 0;
        result->api_send_ns = api_return_time_ns >= route.send_time_ns ? api_return_time_ns - route.send_time_ns : 0;
        result->msg_delete_ns =
            msg_delete_done_ns >= route_update_after_api_done_ns ? msg_delete_done_ns - route_update_after_api_done_ns : 0;
        result->pre_log_total_ns = log_begin_ns >= func_enter_ns ? log_begin_ns - func_enter_ns : 0;
    }

    TdOrderSendLogEntry log_entry;
    log_entry.request_id = request->request_id;
    log_entry.atp_request_id = atp_request_id;
    log_entry.order_ref = request->order_ref;
    copy_text(log_entry.instrument, sizeof(log_entry.instrument), request->instrument_id);
    log_entry.func_enter_ns = func_enter_ns;
    log_entry.send_time_ns = route.send_time_ns;
    log_entry.api_return_time_ns = api_return_time_ns;
    log_entry.log_begin_ns = log_begin_ns;
    log_entry.send_realtime_ns = route_send_realtime_ns;
    log_entry.api_send_ns = api_return_time_ns >= route.send_time_ns ? api_return_time_ns - route.send_time_ns : 0;
    log_entry.func_to_msg_new_begin_ns = msg_new_begin_ns >= func_enter_ns ? msg_new_begin_ns - func_enter_ns : 0;
    log_entry.msg_new_ns = msg_new_done_ns >= msg_new_begin_ns ? msg_new_done_ns - msg_new_begin_ns : 0;
    log_entry.msg_fill_ns = msg_fill_done_ns >= msg_new_done_ns ? msg_fill_done_ns - msg_new_done_ns : 0;
    log_entry.msg_fill_account_ns = msg_account_done_ns >= msg_new_done_ns ? msg_account_done_ns - msg_new_done_ns : 0;
    log_entry.msg_fill_security_ns = msg_security_done_ns >= msg_account_done_ns ? msg_security_done_ns - msg_account_done_ns : 0;
    log_entry.msg_fill_order_ns = msg_order_done_ns >= msg_security_done_ns ? msg_order_done_ns - msg_security_done_ns : 0;
    log_entry.msg_fill_password_batch_ns = msg_fill_done_ns >= msg_order_done_ns ? msg_fill_done_ns - msg_order_done_ns : 0;
    log_entry.route_init_ns = route_init_done_ns >= msg_fill_done_ns ? route_init_done_ns - msg_fill_done_ns : 0;
    log_entry.route_store_before_api_ns =
        route_store_before_api_done_ns >= route_init_done_ns ? route_store_before_api_done_ns - route_init_done_ns : 0;
    log_entry.latency_store_ns = 0;
    log_entry.route_update_after_api_ns = 0;
    log_entry.api_return_to_route_store_ns = 0;
    log_entry.msg_delete_ns =
        msg_delete_done_ns >= route_update_after_api_done_ns ? msg_delete_done_ns - route_update_after_api_done_ns : 0;
    log_entry.pre_log_total_ns = log_begin_ns >= func_enter_ns ? log_begin_ns - func_enter_ns : 0;
    log_entry.enqueue_begin_ns = now_ns();
    enqueue_td_order_send_log(log_entry);

    if (ec != ATPErrorCode::kSuccess) {
        LFInputOrderField err = {};
        copy_text(err.InstrumentID, sizeof(err.InstrumentID), request->instrument_id);
        copy_text(err.ExchangeID, sizeof(err.ExchangeID), "BSE");
        err.OrderRef = request->order_ref;
        err.LimitPrice = request->price;
        err.Volume = request->volume;
        err.Direction = request->direction;
        err.OffsetFlag = request->offset_flag;
        err.OrderPriceType = request->order_price_type;
        int response_request_id = request->request_id;
        on_rsp_order_insert(&err, response_request_id, ec, "gxbse_direct_send_order_failed");
        KF_LOG_ERROR(logger, "[GxbseDirectOrder] failed request_id=" << request->request_id
            << " order_ref=" << request->order_ref
            << " ec=" << ec);
        return -1;
    }
    return request->request_id;
}

int TDEngineGXBSE::direct_cancel_order(const GxbseDirectCancelRequest* request, GxbseDirectCancelResult* result)
{
    if (result != nullptr) {
        *result = GxbseDirectCancelResult{};
        result->ret = -1;
    }
    const uint64_t begin_ns = now_ns();
    if (request == nullptr || request->source_id != source_id ||
        request->request_id < 0 || request->order_ref <= 0) {
        return -1;
    }
    AccountUnitGXBSE* unit = unit_at(request->account_index);
    if (unit == nullptr || !unit->api) {
        return -1;
    }

    int64_t cl_ord_no = 0;
    if (!lookup_clord_by_order_ref(request->order_ref, &cl_ord_no)) {
        LFOrderActionField err = {};
        err.OrderRef = request->order_ref;
        on_rsp_order_action(&err, request->request_id, -1, "gxbse_direct_missing_cl_ord_no");
        KF_LOG_ERROR(logger, "[GxbseDirectCancel] missing cl_ord_no"
            << " order_request_id=" << request->order_request_id
            << " cancel_request_id=" << request->request_id
            << " order_ref=" << request->order_ref);
        return -1;
    }

    ATPReqCashCancelOrderMsg* msg = ATPReqCashCancelOrderMsg::NewMessage();
    msg->SetCustId(unit->cust_id.c_str());
    msg->SetFundAccountId(unit->fund_account_id.c_str());
    msg->SetBranchId(unit->branch_id.c_str());
    msg->SetAccountId(unit->account_id.c_str());
    msg->SetPassword(unit->password.c_str());
    msg->SetOrigClOrdNo(cl_ord_no);
    msg->SetBatchClOrdNo(static_cast<uint64_t>(request->order_ref));
    msg->SetMarketId(static_cast<uint16_t>(unit->market_id));
    const int64_t atp_request_id = next_request_id();
    const uint64_t send_begin_ns = now_ns();
    {
        std::lock_guard<std::mutex> lock(latency_mutex_);
        request_send_time_ns_[atp_request_id] = send_begin_ns;
    }
    ATPErrorCodeType ec = unit->api->ReqCashCancelOrder(msg, atp_request_id);
    const uint64_t send_done_ns = now_ns();
    ATPReqCashCancelOrderMsg::DeleteMessage(msg);
    if (result != nullptr) {
        result->ret = (ec == ATPErrorCode::kSuccess) ? request->request_id : -1;
        result->atp_request_id = atp_request_id;
        result->api_send_ns = send_done_ns >= send_begin_ns ? send_done_ns - send_begin_ns : 0;
        result->total_ns = send_done_ns >= begin_ns ? send_done_ns - begin_ns : 0;
    }
    if (ec != ATPErrorCode::kSuccess) {
        erase_request_send_latency(atp_request_id);
        LFOrderActionField err = {};
        err.OrderRef = request->order_ref;
        on_rsp_order_action(&err, request->request_id, ec, "gxbse_direct_cancel_failed");
        KF_LOG_ERROR(logger, "[GxbseDirectCancel] failed"
            << " order_request_id=" << request->order_request_id
            << " cancel_request_id=" << request->request_id
            << " order_ref=" << request->order_ref
            << " cl_ord_no=" << cl_ord_no
            << " ec=" << ec);
        return -1;
    }
    return request->request_id;
}

void TDEngineGXBSE::req_order_action(const LFOrderActionField* data, int account_index, int requestId, long rcv_time)
{
    (void)rcv_time;
    AccountUnitGXBSE* unit = unit_at(account_index);
    if (unit == nullptr || !unit->api || data == nullptr) {
        return;
    }

    int64_t cl_ord_no = 0;
    if (!lookup_clord_by_order_ref(data->OrderRef, &cl_ord_no)) {
        LFOrderActionField err = *data;
        on_rsp_order_action(&err, requestId, -1, "gxbse_missing_cl_ord_no");
        KF_LOG_ERROR(logger, "[req_order_action] missing cl_ord_no ref=" << data->OrderRef);
        return;
    }

    ATPReqCashCancelOrderMsg* msg = ATPReqCashCancelOrderMsg::NewMessage();
    msg->SetCustId(unit->cust_id.c_str());
    msg->SetFundAccountId(unit->fund_account_id.c_str());
    msg->SetBranchId(unit->branch_id.c_str());
    msg->SetAccountId(unit->account_id.c_str());
    msg->SetPassword(unit->password.c_str());
    msg->SetOrigClOrdNo(cl_ord_no);
    msg->SetBatchClOrdNo(static_cast<uint64_t>(std::max<long>(1, data->OrderRef)));
    msg->SetMarketId(resolve_market_id(*unit, data->ExchangeID));
    const int64_t atp_request_id = next_request_id();
    {
        std::lock_guard<std::mutex> lock(latency_mutex_);
        request_send_time_ns_[atp_request_id] = now_ns();
    }
    ATPErrorCodeType ec = unit->api->ReqCashCancelOrder(msg, atp_request_id);
    ATPReqCashCancelOrderMsg::DeleteMessage(msg);
    if (ec != ATPErrorCode::kSuccess) {
        erase_request_send_latency(atp_request_id);
        LFOrderActionField err = *data;
        on_rsp_order_action(&err, requestId, ec, "gxbse_cancel_failed");
        KF_LOG_ERROR(logger, "[req_order_action] failed ref=" << data->OrderRef
            << " cl_ord_no=" << cl_ord_no << " ec=" << ec);
    }
}

void TDEngineGXBSE::req_investor_position(const LFQryPositionField* data, int account_index, int requestId)
{
    AccountUnitGXBSE* unit = unit_at(account_index);
    if (unit == nullptr) {
        return;
    }
    if (unit->bypass_account_queries) {
        respond_bypass_position(data, *unit, requestId);
        return;
    }
    if (!unit->api) {
        return;
    }
    if (!unit->logged_in.load()) {
        // V06 release: the legacy helper drops the position error field.
        // Never turn a failed request into an apparently successful zero row.
        KF_LOG_ERROR(logger, "[req_investor_position] not logged in account_index="
            << account_index << " rid=" << requestId);
        return;
    }
    ATPReqCashShareQueryMsg* msg = ATPReqCashShareQueryMsg::NewMessage();
    msg->SetCustId(unit->cust_id.c_str());
    msg->SetFundAccountId(unit->fund_account_id.c_str());
    msg->SetBranchId(unit->branch_id.c_str());
    msg->SetPassword(unit->password.c_str());
    msg->SetAccountId(unit->account_id.c_str());
    msg->SetBusinessType(ATPBusinessTypeConst::kAll);
    const uint16_t market_id = resolve_market_id(*unit, data ? data->ExchangeID : nullptr);
    msg->SetMarketId(market_id);
    msg->SetSecurityId("");
    msg->SetReturnNum(unit->return_num);

    PositionQueryContext context;
    context.account_index = account_index;
    context.market_id = market_id;
    if (data != nullptr && data->InstrumentID[0] != '\0') {
        context.requested_instrument = data->InstrumentID;
    }
    store_position_query(requestId, context);
    {
        std::lock_guard<std::mutex> lock(latency_mutex_);
        request_send_time_ns_[requestId] = now_ns();
    }

    ATPErrorCodeType ec = unit->api->ReqCashShareQuery(msg, requestId);
    ATPReqCashShareQueryMsg::DeleteMessage(msg);
    if (ec != ATPErrorCode::kSuccess) {
        erase_request_send_latency(requestId);
        erase_position_query(requestId);
        KF_LOG_ERROR(logger, "[V06PositionQueryBlocked] request rejected rid=" << requestId << " ec=" << ec);
    }
}

void TDEngineGXBSE::start_position_sync_thread()
{
    bool expected = false;
    if (!position_sync_running_.compare_exchange_strong(expected, true)) {
        return;
    }
    position_sync_thread_ = std::thread(&TDEngineGXBSE::position_sync_loop, this);
}

void TDEngineGXBSE::stop_position_sync_thread()
{
    if (!position_sync_running_.exchange(false)) {
        return;
    }
    if (position_sync_thread_.joinable()) {
        position_sync_thread_.join();
    }
}

void TDEngineGXBSE::position_sync_loop()
{
    KF_LOG_INFO(logger, "[PositionSync] thread started");
    while (position_sync_running_.load()) {
        int sleep_sec = 5;
        for (std::size_t i = 0; i < account_units_.size(); ++i) {
            AccountUnitGXBSE& unit = account_units_[i];
            if (unit.enable_position_sync) {
                sleep_sec = std::min(sleep_sec, std::max(1, unit.position_sync_interval_sec));
                if (!unit.bypass_account_queries && unit.logged_in.load() && unit.api) {
                    send_periodic_account_query(static_cast<int>(i));
                    send_periodic_position_query(static_cast<int>(i));
                }
            }
        }

        for (int i = 0; i < sleep_sec * 10 && position_sync_running_.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    KF_LOG_INFO(logger, "[PositionSync] thread stopped");
}

void TDEngineGXBSE::send_periodic_account_query(int account_index)
{
    AccountUnitGXBSE* unit = unit_at(account_index);
    if (unit == nullptr || !unit->api || !unit->logged_in.load() || !unit->enable_position_sync) {
        return;
    }
    bool expected = false;
    if (!unit->periodic_account_query_inflight.compare_exchange_strong(expected, true)) {
        KF_LOG_DEBUG(logger, "[AccountSync] skip inflight account_index=" << account_index);
        return;
    }

    ATPReqCashFundQueryMsg* msg = ATPReqCashFundQueryMsg::NewMessage();
    msg->SetCustId(unit->cust_id.c_str());
    msg->SetFundAccountId(unit->fund_account_id.c_str());
    msg->SetBranchId(unit->branch_id.c_str());
    msg->SetAccountId(unit->account_id.c_str());
    msg->SetPassword(unit->password.c_str());
    msg->SetCurrency("CNY");
    msg->SetMarketId(static_cast<uint16_t>(unit->market_id));

    const int64_t request_id = next_request_id();
    {
        std::lock_guard<std::mutex> lock(account_query_mutex_);
        periodic_account_query_requests_.insert(request_id);
    }
    {
        std::lock_guard<std::mutex> lock(latency_mutex_);
        request_send_time_ns_[request_id] = now_ns();
    }

    ATPErrorCodeType ec = unit->api->ReqCashFundQuery(msg, request_id);
    ATPReqCashFundQueryMsg::DeleteMessage(msg);
    if (ec != ATPErrorCode::kSuccess) {
        erase_request_send_latency(request_id);
        consume_periodic_account_query(request_id);
        unit->periodic_account_query_inflight.store(false);
        LFRspAccountField acc = {};
        auto cc = get_cc();
        if (cc != nullptr) {
            cc->on_rsp_account(&acc, static_cast<int>(request_id), source_id, 0, ec, "gxbse_periodic_fund_query_failed");
        }
        KF_LOG_ERROR(logger, "[AccountSync] request failed account_index=" << account_index
            << " rid=" << request_id
            << " ec=" << ec);
        return;
    }

    KF_LOG_INFO(logger, "[AccountSync] request account_index=" << account_index
        << " rid=" << request_id
        << " market_id=" << unit->market_id
        << " interval_sec=" << unit->position_sync_interval_sec);
}

void TDEngineGXBSE::send_periodic_position_query(int account_index)
{
    AccountUnitGXBSE* unit = unit_at(account_index);
    if (unit == nullptr || !unit->api || !unit->logged_in.load() || !unit->enable_position_sync) {
        return;
    }
    bool expected = false;
    if (!unit->periodic_position_query_inflight.compare_exchange_strong(expected, true)) {
        KF_LOG_DEBUG(logger, "[PositionSync] skip inflight account_index=" << account_index);
        return;
    }

    ATPReqCashShareQueryMsg* msg = ATPReqCashShareQueryMsg::NewMessage();
    msg->SetCustId(unit->cust_id.c_str());
    msg->SetFundAccountId(unit->fund_account_id.c_str());
    msg->SetBranchId(unit->branch_id.c_str());
    msg->SetPassword(unit->password.c_str());
    msg->SetAccountId(unit->account_id.c_str());
    msg->SetBusinessType(ATPBusinessTypeConst::kAll);
    const uint16_t market_id = static_cast<uint16_t>(unit->market_id);
    msg->SetMarketId(market_id);
    msg->SetSecurityId("");
    msg->SetReturnNum(unit->return_num);

    const int64_t request_id = next_request_id();
    PositionQueryContext context;
    context.account_index = account_index;
    context.market_id = market_id;
    context.periodic_sync = true;
    store_position_query(request_id, context);
    {
        std::lock_guard<std::mutex> lock(latency_mutex_);
        request_send_time_ns_[request_id] = now_ns();
    }

    ATPErrorCodeType ec = unit->api->ReqCashShareQuery(msg, request_id);
    ATPReqCashShareQueryMsg::DeleteMessage(msg);
    if (ec != ATPErrorCode::kSuccess) {
        erase_request_send_latency(request_id);
        finish_position_query(request_id);
        KF_LOG_ERROR(logger, "[PositionSync] request failed account_index=" << account_index
            << " rid=" << request_id
            << " ec=" << ec);
        return;
    }

    KF_LOG_INFO(logger, "[PositionSync] request account_index=" << account_index
        << " rid=" << request_id
        << " market_id=" << market_id
        << " interval_sec=" << unit->position_sync_interval_sec);
}

bool TDEngineGXBSE::consume_periodic_account_query(int64_t request_id)
{
    std::lock_guard<std::mutex> lock(account_query_mutex_);
    auto it = periodic_account_query_requests_.find(request_id);
    if (it == periodic_account_query_requests_.end()) {
        return false;
    }
    periodic_account_query_requests_.erase(it);
    return true;
}

bool TDEngineGXBSE::is_periodic_account_query(int64_t request_id)
{
    std::lock_guard<std::mutex> lock(account_query_mutex_);
    return periodic_account_query_requests_.find(request_id) != periodic_account_query_requests_.end();
}

void TDEngineGXBSE::forward_periodic_position(const LFRspPositionField& pos, bool is_last, int request_id)
{
    auto cc = get_cc();
    if (cc != nullptr) {
        cc->on_rtn_pos_option(&pos, is_last, request_id, source_id, 0);
    }
}


void TDEngineGXBSE::req_qry_account(const LFQryAccountField* data, int account_index, int requestId)
{
    (void)data;
    AccountUnitGXBSE* unit = unit_at(account_index);
    if (unit == nullptr) {
        return;
    }
    if (unit->bypass_account_queries) {
        respond_bypass_account(*unit, requestId);
        return;
    }
    if (!unit->api) {
        return;
    }
    if (!unit->logged_in.load()) {
        LFRspAccountField acc = {};
        on_rsp_account(&acc, true, requestId, ATPErrorCode::kNotLogin, "gxbse_not_login");
        KF_LOG_ERROR(logger, "[req_qry_account] not logged in account_index="
            << account_index << " rid=" << requestId);
        return;
    }
    ATPReqCashFundQueryMsg* msg = ATPReqCashFundQueryMsg::NewMessage();
    msg->SetCustId(unit->cust_id.c_str());
    msg->SetFundAccountId(unit->fund_account_id.c_str());
    msg->SetBranchId(unit->branch_id.c_str());
    msg->SetAccountId(unit->account_id.c_str());
    msg->SetPassword(unit->password.c_str());
    msg->SetCurrency("CNY");
    msg->SetMarketId(static_cast<uint16_t>(unit->market_id));
    {
        std::lock_guard<std::mutex> lock(latency_mutex_);
        request_send_time_ns_[requestId] = now_ns();
    }
    ATPErrorCodeType ec = unit->api->ReqCashFundQuery(msg, requestId);
    ATPReqCashFundQueryMsg::DeleteMessage(msg);
    if (ec != ATPErrorCode::kSuccess) {
        erase_request_send_latency(requestId);
        LFRspAccountField acc = {};
        on_rsp_account(&acc, true, requestId, ec, "gxbse_fund_query_failed");
    }
}

void TDEngineGXBSE::req_qry_order_info(const LFQryOrderField* data, int account_index, int requestId)
{
    AccountUnitGXBSE* unit = unit_at(account_index);
    if (unit == nullptr || !unit->api) {
        return;
    }
    ATPReqCashOrderQueryMsg* msg = ATPReqCashOrderQueryMsg::NewMessage();
    msg->SetCustId(unit->cust_id.c_str());
    msg->SetFundAccountId(unit->fund_account_id.c_str());
    msg->SetBranchId(unit->branch_id.c_str());
    msg->SetAccountId(unit->account_id.c_str());
    msg->SetPassword(unit->password.c_str());
    msg->SetClOrdNo(0);
    msg->SetMarketId(static_cast<uint16_t>(unit->market_id));
    const std::string security_id =
        (data != nullptr && data->InstrumentID[0] != '\0') ? StripExchangeSuffix(data->InstrumentID) : "";
    msg->SetSecurityId(security_id.c_str());
    msg->SetBusinessType(ATPBusinessTypeConst::kAll);
    msg->SetSide(ATPSideConst::kAll);
    msg->SetOrderQueryCondition(ATPOrderQueryConditionConst::kAll);
    msg->SetReturnNum(unit->return_num);
    msg->SetReturnSeq(ATPReturnSeqConst::kTimeOrderRe);
    msg->SetBatchClOrdNo(0);
    msg->SetQueryIndex(0);
    {
        std::lock_guard<std::mutex> lock(latency_mutex_);
        request_send_time_ns_[requestId] = now_ns();
    }
    ATPErrorCodeType ec = unit->api->ReqCashOrderQuery(msg, requestId);
    ATPReqCashOrderQueryMsg::DeleteMessage(msg);
    if (ec != ATPErrorCode::kSuccess) {
        erase_request_send_latency(requestId);
        KF_LOG_ERROR(logger, "[req_qry_order_info] failed ec=" << ec << " rid=" << requestId);
    }
}

void TDEngineGXBSE::req_qry_limit_price(const LFQryLimitPrice* data, int account_index, int requestId)
{
    AccountUnitGXBSE* unit = unit_at(account_index);
    if (unit == nullptr || !unit->api) {
        KF_LOG_ERROR(logger, "[req_qry_limit_price] invalid account or api account_index=" << account_index);
        return;
    }
    if (data == nullptr || data->InstrumentID[0] == '\0') {
        KF_LOG_ERROR(logger, "[req_qry_limit_price] missing instrument");
        LFMarketDataField rsp = {};
        auto cc = get_cc();
        if (cc != nullptr) {
            cc->on_rsp_limit_price(&rsp, requestId, source_id, -1);
        }
        return;
    }

    ATPReqCashExtQuerySecurityInfoMsg* msg = ATPReqCashExtQuerySecurityInfoMsg::NewMessage();
    msg->SetCustId(unit->cust_id.c_str());
    msg->SetFundAccountId(unit->fund_account_id.c_str());
    msg->SetPassword(unit->password.c_str());
    msg->SetAccountId(unit->account_id.c_str());
    msg->SetMarketId(static_cast<uint16_t>(unit->market_id));
    const std::string security_id = StripExchangeSuffix(data->InstrumentID);
    msg->SetSecurityId(security_id.c_str());
    msg->SetReturnNum(1);

    {
        std::lock_guard<std::mutex> lock(route_mutex_);
        limit_price_request_account_[requestId] = account_index;
    }
    {
        std::lock_guard<std::mutex> lock(latency_mutex_);
        request_send_time_ns_[requestId] = now_ns();
    }
    ATPErrorCodeType ec = unit->api->ReqCashExtQuerySecurityInfo(msg, requestId);
    ATPReqCashExtQuerySecurityInfoMsg::DeleteMessage(msg);
    if (ec != ATPErrorCode::kSuccess) {
        erase_request_send_latency(requestId);
        {
            std::lock_guard<std::mutex> lock(route_mutex_);
            limit_price_request_account_.erase(requestId);
        }
        LFMarketDataField rsp = {};
        copy_text(rsp.InstrumentID, sizeof(rsp.InstrumentID), data->InstrumentID);
        copy_text(rsp.ExchangeID, sizeof(rsp.ExchangeID), exchange_from_market(static_cast<uint16_t>(unit->market_id)));
        auto cc = get_cc();
        if (cc != nullptr) {
            cc->on_rsp_limit_price(&rsp, requestId, source_id, ec);
        }
        KF_LOG_ERROR(logger, "[req_qry_limit_price] failed ec=" << ec
            << " rid=" << requestId
            << " instrument=" << data->InstrumentID);
    }
}

void TDEngineGXBSE::respond_bypass_account(const AccountUnitGXBSE& unit, int request_id)
{
    LFRspAccountField acc = {};
    copy_text(acc.BrokerID, sizeof(acc.BrokerID), unit.branch_id);
    copy_text(acc.InvestorID, sizeof(acc.InvestorID), unit.cust_id);
    acc.Balance = 0.0;
    acc.Available = 0.0;
    acc.WithdrawQuota = 0.0;
    KF_LOG_INFO(logger, "[bypass_account_queries] account source=180 rid=" << request_id
        << " cust_id=" << unit.cust_id << " balance=0 available=0");
    on_rsp_account(&acc, true, request_id, 0, "gxbse_bypass_account");
}

void TDEngineGXBSE::respond_bypass_position(const LFQryPositionField* data,
                                            const AccountUnitGXBSE& unit,
                                            int request_id)
{
    std::vector<std::string> instruments;
    if (data != nullptr && data->InstrumentID[0] != '\0') {
        instruments.emplace_back(data->InstrumentID);
    }
    if (instruments.empty()) {
        instruments.emplace_back("");
    }

    for (std::size_t i = 0; i < instruments.size(); ++i) {
        LFRspPositionField pos = {};
        copy_text(pos.BrokerID, sizeof(pos.BrokerID), unit.branch_id);
        copy_text(pos.InvestorID, sizeof(pos.InvestorID), unit.cust_id);
        copy_text(pos.InstrumentID, sizeof(pos.InstrumentID), instruments[i]);
        pos.PosiDirection = LF_CHAR_Long;
        pos.Position = 0;
        pos.YdPosition = 0;
        pos.TdPosition = 0;
        pos.Available = 0.0;
        const bool is_last = (i + 1 == instruments.size());
        on_rsp_position(&pos, is_last, request_id, 0, "gxbse_bypass_position");
    }
    KF_LOG_INFO(logger, "[bypass_account_queries] position source=180 rid=" << request_id
        << " instruments=" << instruments.size());
}

void TDEngineGXBSE::store_position_query(int64_t request_id, const PositionQueryContext& context)
{
    std::lock_guard<std::mutex> lock(position_query_mutex_);
    position_query_contexts_[request_id] = context;
}

void TDEngineGXBSE::erase_position_query(int64_t request_id)
{
    std::lock_guard<std::mutex> lock(position_query_mutex_);
    position_query_contexts_.erase(request_id);
}

void TDEngineGXBSE::finish_position_query(int64_t request_id)
{
    PositionQueryContext context;
    bool has_context = false;
    {
        std::lock_guard<std::mutex> lock(position_query_mutex_);
        auto it = position_query_contexts_.find(request_id);
        if (it != position_query_contexts_.end()) {
            context = it->second;
            position_query_contexts_.erase(it);
            has_context = true;
        }
    }
    if (has_context && context.periodic_sync) {
        AccountUnitGXBSE* unit = unit_at(context.account_index);
        if (unit != nullptr) {
            unit->periodic_position_query_inflight.store(false);
        }
    }
}

LFRspPositionField TDEngineGXBSE::make_zero_position(const AccountUnitGXBSE& unit,
                                                     const PositionQueryContext& context) const
{
    LFRspPositionField pos = {};
    copy_text(pos.BrokerID, sizeof(pos.BrokerID), unit.branch_id);
    copy_text(pos.InvestorID, sizeof(pos.InvestorID), unit.cust_id);
    copy_text(pos.InstrumentID, sizeof(pos.InstrumentID), context.requested_instrument);
    pos.PosiDirection = LF_CHAR_Long;
    pos.Position = 0;
    pos.YdPosition = 0;
    pos.TdPosition = 0;
    pos.Available = 0.0;
    pos.PositionCost = 0.0;
    pos.ProfitLoss = 0.0;
    return pos;
}

void TDEngineGXBSE::on_login(int account_index, const ATPCustomerInfo& msg)
{
    AccountUnitGXBSE* unit = unit_at(account_index);
    if (unit == nullptr) {
        return;
    }
    unit->logged_in.store(true);
    unit->connected.store(true);
    login_ok();
    login_cv_.notify_all();
    KF_LOG_INFO(logger, "[OnLogin] account_index=" << account_index
        << " cust_id=" << msg.GetCustId()
        << " fund_accounts=" << msg.FundAccountArraySize());
}

void TDEngineGXBSE::on_logout(int account_index, const char* desc)
{
    AccountUnitGXBSE* unit = unit_at(account_index);
    if (unit != nullptr) {
        unit->logged_in.store(false);
        unit->connected.store(false);
    }
    KF_LOG_INFO(logger, "[OnLogout] account_index=" << account_index << " desc=" << (desc ? desc : ""));
}

void TDEngineGXBSE::on_recovering(int account_index, const char* desc)
{
    AccountUnitGXBSE* unit = unit_at(account_index);
    if (unit != nullptr) {
        unit->logged_in.store(false);
    }
    KF_LOG_ERROR(logger, "[OnRecovering] account_index=" << account_index << " desc=" << (desc ? desc : ""));
}

void TDEngineGXBSE::on_rsp_cash_auction_order(int account_index,
                                              const ATPRspCashAuctionOrderMsg& msg,
                                              const ATPRspErrorInfo& error_info,
                                              int64_t request_id)
{
    (void)account_index;
    uint64_t latency_ns = 0;
    if (consume_request_send_latency(request_id, &latency_ns)) {
        const uint64_t rsp_entry_ns = now_ns();
        {
            std::lock_guard<std::mutex> lock(latency_mutex_);
            record_latency(insert_rsp_latency_, latency_ns);
        }
        const uint64_t after_record_ns = now_ns();
        OrderRoute route;
        const bool has_route = lookup_route_by_request_id(request_id, &route);
        const bool has_api_return = has_route && route.api_return_time_ns != 0;
        const uint64_t req_api_sync_ns = has_api_return && route.api_return_time_ns >= route.send_time_ns
            ? route.api_return_time_ns - route.send_time_ns : 0;
        const uint64_t api_return_to_rsp_ns = has_api_return && rsp_entry_ns >= route.api_return_time_ns
            ? rsp_entry_ns - route.api_return_time_ns : 0;
        const uint64_t rsp_record_ns = after_record_ns >= rsp_entry_ns ? after_record_ns - rsp_entry_ns : 0;
        KF_LOG_INFO(logger, "[GxbseTdLatencyBreakdown] event=insert_rsp"
            << " atp_request_id=" << request_id
            << " strategy_request_id=" << (has_route ? route.request_id : -1)
            << " instrument=" << (has_route ? route.instrument : "")
            << " order_ref=" << (has_route ? route.order_ref : 0)
            << " api_return_observed=" << (has_api_return ? 1 : 0)
            << " total_ns=" << latency_ns
            << " req_api_sync_ns=" << req_api_sync_ns
            << " api_return_to_rsp_ns=" << api_return_to_rsp_ns
            << " rsp_record_ns=" << rsp_record_ns);
        maybe_log_latency_stats(after_record_ns);
    }
    bind_clord_route(request_id, msg.GetClOrdNo());
    if (error_info.error_id != 0) {
        LFInputOrderField err = {};
        copy_text(err.InstrumentID, sizeof(err.InstrumentID), msg.GetSecurityId());
        copy_text(err.ExchangeID, sizeof(err.ExchangeID), exchange_from_market(msg.GetMarketId()));
        err.OrderRef = msg.GetBatchClOrdNo() > 0 ? static_cast<long>(msg.GetBatchClOrdNo()) : 0;
        err.LimitPrice = msg.GetPrice();
        err.Volume = qty_to_int(msg.GetOrderQty());
        err.Direction = to_lf_direction(msg.GetSide());
        int rid = 0;
        {
            std::lock_guard<std::mutex> lock(route_mutex_);
            auto it = request_to_route_.find(request_id);
            if (it != request_to_route_.end()) {
                rid = it->second.request_id;
                err.OrderRef = it->second.order_ref;
            }
        }
        on_rsp_order_insert(&err, rid, error_info.error_id, error_info.GetErrorMsg());
    }
}

void TDEngineGXBSE::on_rsp_cash_cancel_order(int account_index,
                                             const ATPRspCashCancelOrderMsg& msg,
                                             const ATPRspErrorInfo& error_info,
                                             int64_t request_id)
{
    (void)account_index;
    uint64_t latency_ns = 0;
    if (consume_request_send_latency(request_id, &latency_ns)) {
        {
            std::lock_guard<std::mutex> lock(latency_mutex_);
            record_latency(cancel_rsp_latency_, latency_ns);
        }
        maybe_log_latency_stats(now_ns());
    }
    LFOrderActionField action = {};
    action.OrderRef = static_cast<long>(msg.GetBatchClOrdNo());
    copy_text(action.OrderSysID, sizeof(action.OrderSysID), std::to_string(msg.GetOrigClOrdNo()));
    action.ActionFlag = '0';
    on_rsp_order_action(&action, static_cast<int>(request_id), error_info.error_id, error_info.GetErrorMsg());
}

void TDEngineGXBSE::on_rtn_cash_auction_order(int account_index, const ATPRtnCashAuctionOrderMsg& msg)
{
    AccountUnitGXBSE* unit = unit_at(account_index);
    if (unit == nullptr) {
        return;
    }

    OrderRoute route;
    if (!lookup_route_by_clord(msg.GetClOrdNo(), &route)) {
        route.order_ref = msg.GetBatchClOrdNo() > 0 ? static_cast<long>(msg.GetBatchClOrdNo()) : msg.GetClOrdNo();
        route.request_id = -1;
        route.account_index = account_index;
        route.instrument = msg.GetSecurityId();
        route.price = msg.GetPrice();
        route.volume = qty_to_int(msg.GetOrderQty());
        route.direction = to_lf_direction(msg.GetSide());
    }
    if (!route.first_rtn_observed && route.send_time_ns != 0) {
        const uint64_t rtn_entry_ns = now_ns();
        const uint64_t total_ns = rtn_entry_ns - route.send_time_ns;
        {
            std::lock_guard<std::mutex> lock(latency_mutex_);
            record_latency(insert_first_rtn_latency_, total_ns);
        }
        const uint64_t after_record_ns = now_ns();
        const bool has_api_return = route.api_return_time_ns != 0;
        const uint64_t req_api_sync_ns = has_api_return && route.api_return_time_ns >= route.send_time_ns
            ? route.api_return_time_ns - route.send_time_ns : 0;
        const uint64_t api_return_to_rtn_ns = has_api_return && rtn_entry_ns >= route.api_return_time_ns
            ? rtn_entry_ns - route.api_return_time_ns : 0;
        const uint64_t rtn_record_ns = after_record_ns >= rtn_entry_ns ? after_record_ns - rtn_entry_ns : 0;
        KF_LOG_INFO(logger, "[GxbseTdLatencyBreakdown] event=insert_first_rtn"
            << " cl_ord_no=" << msg.GetClOrdNo()
            << " strategy_request_id=" << route.request_id
            << " instrument=" << route.instrument
            << " order_ref=" << route.order_ref
            << " api_return_observed=" << (has_api_return ? 1 : 0)
            << " total_ns=" << total_ns
            << " req_api_sync_ns=" << req_api_sync_ns
            << " api_return_to_rtn_ns=" << api_return_to_rtn_ns
            << " rtn_record_ns=" << rtn_record_ns);
        maybe_log_latency_stats(after_record_ns);
        route.first_rtn_observed = true;
        bind_clord_route(msg.GetClOrdNo(), route);
    }

    LFRtnOrderField rtn = {};
    fill_order_from_route(route, *unit, msg.GetClOrdNo(), msg.GetOrdStatus(), msg.GetPrice(),
        msg.GetOrderQty(), msg.GetLeavesQty(), msg.GetCumQty(), msg.GetOrderId(), &rtn);
    int rid = route.request_id;
    if (rtn.OrderStatus == LF_CHAR_Error) {
        KF_LOG_ERROR(logger, "[OrderRejectDetail] func=on_rtn_cash_auction_order"
            << " rid=" << rid
            << " order_ref=" << route.order_ref
            << " cl_ord_no=" << msg.GetClOrdNo()
            << " batch_cl_ord_no=" << msg.GetBatchClOrdNo()
            << " instrument=" << route.instrument
            << " market_id=" << msg.GetMarketId()
            << " side=" << msg.GetSide()
            << " ord_status=" << static_cast<int>(msg.GetOrdStatus())
            << " reject_reason_code=" << msg.GetRejectReasonCode()
            << " ord_rej_reason=" << (msg.GetOrdRejReason() == nullptr ? "" : msg.GetOrdRejReason())
            << " price=" << msg.GetPrice()
            << " order_qty=" << msg.GetOrderQty()
            << " leaves_qty=" << msg.GetLeavesQty()
            << " cum_qty=" << msg.GetCumQty()
            << " order_id=" << (msg.GetOrderId() == nullptr ? "" : msg.GetOrderId())
            << " cl_ord_id=" << (msg.GetClOrdId() == nullptr ? "" : msg.GetClOrdId())
            << " orig_cl_ord_no=" << msg.GetOrigClOrdNo());
    }
    forward_order_return(rtn, rid, route);
    push_trade_if_needed(route, *unit, msg, qty_to_int(msg.GetCumQty()), rid);
}

void TDEngineGXBSE::on_rsp_cash_share_query(int account_index,
                                            const ATPRspCashShareQueryResultMsg& msg,
                                            int64_t request_id,
                                            const ATPRspErrorInfo& error_info,
                                            bool is_last)
{
    AccountUnitGXBSE* unit = unit_at(account_index);
    if (unit == nullptr) {
        return;
    }

    PositionQueryContext context;
    bool has_context = false;
    {
        std::lock_guard<std::mutex> lock(position_query_mutex_);
        auto it = position_query_contexts_.find(request_id);
        if (it != position_query_contexts_.end()) {
            it->second.saw_any = true;
            context = it->second;
            has_context = true;
        }
    }
    if (is_last) {
        uint64_t latency_ns = 0;
        if (consume_request_send_latency(request_id, &latency_ns)) {
            {
                std::lock_guard<std::mutex> lock(latency_mutex_);
                if (has_context && context.periodic_sync) {
                    record_latency(periodic_position_query_latency_, latency_ns);
                } else {
                    record_latency(position_query_latency_, latency_ns);
                }
            }
            maybe_log_latency_stats(now_ns());
        }
    }

    const int error_id = error_info.error_id;
    if (!has_context) {
        // Includes late responses after a failed query was retired.
        KF_LOG_ERROR(logger, "[V06PositionQueryBlocked] unknown/retired rid=" << request_id);
        return;
    }
    const bool forward_to_helper = !(has_context && context.periodic_sync);
    if (error_id == ATPErrorCode::kQNotFoundShare
        || error_id == ATPErrorCode::kQSharePositionNotExist) {
        if (has_context && !context.requested_instrument.empty()) {
            LFRspPositionField zero_pos = make_zero_position(*unit, context);
            if (forward_to_helper) {
                on_rsp_position(&zero_pos, true, static_cast<int>(request_id), 0, "gxbse_empty_position");
            } else {
                forward_periodic_position(zero_pos, true, static_cast<int>(request_id));
            }
            KF_LOG_INFO(logger, "[RspPosition] empty position source=180 rid=" << request_id
                << " instrument=" << context.requested_instrument
                << " errorId=" << error_id
                << " errorMsg=" << error_info.GetErrorMsg());
        } else {
            LFRspPositionField zero_pos = {};
            if (forward_to_helper) {
                on_rsp_position(&zero_pos, true, static_cast<int>(request_id), 0, "gxbse_empty_position");
            } else {
                forward_periodic_position(zero_pos, true, static_cast<int>(request_id));
            }
            KF_LOG_INFO(logger, "[RspPosition] empty all-position source=180 rid=" << request_id
                << " errorId=" << error_id
                << " errorMsg=" << error_info.GetErrorMsg());
        }
        finish_position_query(request_id);
        return;
    }

    if (error_id != 0) {
        erase_request_send_latency(request_id);
        finish_position_query(request_id);
        KF_LOG_ERROR(logger, "[V06PositionQueryBlocked] failed result rid=" << request_id
            << " ec=" << error_id << "; strategy remains gated and retries");
        return;
    }

    LFRspPositionField pos = {};
    copy_text(pos.InstrumentID, sizeof(pos.InstrumentID), msg.GetSecurityId());
    copy_text(pos.BrokerID, sizeof(pos.BrokerID), "");
    copy_text(pos.InvestorID, sizeof(pos.InvestorID), msg.GetCustId());
    pos.PosiDirection = LF_CHAR_Long;
    pos.Position = qty_to_int(msg.GetLeavesQty());
    pos.YdPosition = qty_to_int(msg.GetInitQty());
    pos.TdPosition = std::max(0, pos.Position - pos.YdPosition);
    pos.Available = qty_to_int(msg.GetAvailableQty());
    pos.PositionCost = msg.GetCostPrice() * pos.Position;
    pos.ProfitLoss = msg.GetProfitLoss();
    KF_LOG_INFO(logger, "[RspPositionItemRaw] source=180 rid=" << request_id
        << " instrument=" << pos.InstrumentID
        << " account_id=" << msg.GetAccountId()
        << " market_id=" << msg.GetMarketId()
        << " init_qty=" << msg.GetInitQty()
        << " leaves_qty=" << msg.GetLeavesQty()
        << " available_qty=" << msg.GetAvailableQty()
        << " mapped_position=" << pos.Position
        << " mapped_yd=" << pos.YdPosition
        << " mapped_td=" << pos.TdPosition
        << " mapped_available=" << pos.Available);

    bool emit_current_as_last = is_last;
    bool emit_zero_after_current = false;
    if (has_context && error_id == 0) {
        std::lock_guard<std::mutex> lock(position_query_mutex_);
        auto it = position_query_contexts_.find(request_id);
        if (it != position_query_contexts_.end()) {
            if (pos.InstrumentID[0] != '\0') {
                it->second.saw_any = true;
                it->second.seen_instruments.insert(StripExchangeSuffix(pos.InstrumentID));
            }
            if (!it->second.requested_instrument.empty()
                && StripExchangeSuffix(it->second.requested_instrument.c_str()) == StripExchangeSuffix(pos.InstrumentID)) {
                it->second.saw_requested = true;
            }
            context = it->second;
            if (is_last && !context.requested_instrument.empty() && !context.saw_requested) {
                emit_current_as_last = false;
                emit_zero_after_current = true;
            }
        }
    }

    if (forward_to_helper) {
        on_rsp_position(&pos, emit_current_as_last, static_cast<int>(request_id), error_id, error_info.GetErrorMsg());
    } else {
        forward_periodic_position(pos, emit_current_as_last, static_cast<int>(request_id));
    }

    if (emit_zero_after_current) {
        LFRspPositionField zero_pos = make_zero_position(*unit, context);
        if (forward_to_helper) {
            on_rsp_position(&zero_pos, true, static_cast<int>(request_id), 0, "gxbse_empty_position");
        } else {
            forward_periodic_position(zero_pos, true, static_cast<int>(request_id));
        }
        KF_LOG_INFO(logger, "[RspPosition] append empty requested position source=180 rid=" << request_id
            << " instrument=" << context.requested_instrument);
    }

    if (is_last) {
        finish_position_query(request_id);
    }
}

void TDEngineGXBSE::on_rsp_cash_fund_query(int account_index,
                                           const ATPRspCashFundQueryResultMsg& msg,
                                           int64_t request_id,
                                           const ATPRspErrorInfo& error_info,
                                           bool is_last)
{
    AccountUnitGXBSE* unit = unit_at(account_index);
    const bool periodic_account_sync = is_periodic_account_query(request_id);
    if (is_last) {
        uint64_t latency_ns = 0;
        if (consume_request_send_latency(request_id, &latency_ns)) {
            {
                std::lock_guard<std::mutex> lock(latency_mutex_);
                record_latency(account_query_latency_, latency_ns);
            }
            maybe_log_latency_stats(now_ns());
        }
        if (unit != nullptr && periodic_account_sync) {
            unit->periodic_account_query_inflight.store(false);
        }
        if (periodic_account_sync) {
            consume_periodic_account_query(request_id);
        }
    }
    LFRspAccountField acc = {};
    copy_text(acc.InvestorID, sizeof(acc.InvestorID), msg.GetCustId());
    acc.Balance = msg.GetLeavesValue();
    acc.Available = msg.GetAvailableT1();
    acc.WithdrawQuota = msg.GetAvailableT0();
    acc.FrozenCash = msg.GetFrozenAll();
    if (periodic_account_sync) {
        KF_LOG_INFO(logger, "[AccountSyncForward] account_index=" << account_index
            << " rid=" << request_id
            << " isLast=" << (is_last ? 1 : 0)
            << " cc=0"
            << " via=periodic_log_only"
            << " reason=no_strategy_owner_for_periodic_rid");
        KF_LOG_INFO(logger, "[AccountSync] response account_index=" << account_index
            << " rid=" << request_id
            << " isLast=" << (is_last ? 1 : 0)
            << " errorId=" << error_info.error_id
            << " available=" << acc.Available
            << " balance=" << acc.Balance
            << " forward=periodic_log_only");
    } else {
        on_rsp_account(&acc, is_last, static_cast<int>(request_id), error_info.error_id, error_info.GetErrorMsg());
    }
}

void TDEngineGXBSE::on_rsp_cash_order_query(int account_index,
                                            const ATPRspCashOrderQueryResultMsg& msg,
                                            int64_t request_id,
                                            const ATPRspErrorInfo& error_info,
                                            bool is_last)
{
    if (is_last) {
        uint64_t latency_ns = 0;
        if (consume_request_send_latency(request_id, &latency_ns)) {
            {
                std::lock_guard<std::mutex> lock(latency_mutex_);
                record_latency(order_query_latency_, latency_ns);
            }
            maybe_log_latency_stats(now_ns());
        }
    }
    if (error_info.error_id != 0) {
        KF_LOG_ERROR(logger, "[OnRspCashOrderQueryResult] error rid=" << request_id
            << " ec=" << error_info.error_id << " msg=" << error_info.GetErrorMsg());
        return;
    }
    AccountUnitGXBSE* unit = unit_at(account_index);
    if (unit == nullptr) {
        return;
    }
    OrderRoute route;
    route.order_ref = msg.GetBatchClOrdNo() > 0 ? static_cast<long>(msg.GetBatchClOrdNo()) : msg.GetClOrdNo();
    route.request_id = static_cast<int>(request_id);
    route.account_index = account_index;
    route.instrument = msg.GetSecurityId();
    route.price = msg.GetOrderPrice();
    route.volume = qty_to_int(msg.GetOrderQty());
    route.direction = to_lf_direction(msg.GetSide());
    route.order_price_type = (msg.GetOrdType() == ATPOrderTypeConst::kMarket) ? LF_CHAR_AnyPrice : LF_CHAR_LimitPrice;
    route.time_condition = LF_CHAR_GFD;
    route.volume_condition = LF_CHAR_AV;
    route.offset_flag = LF_CHAR_Open;
    bind_clord_route(msg.GetClOrdNo(), route);

    LFRtnOrderField rtn = {};
    fill_order_from_route(route, *unit, msg.GetClOrdNo(), msg.GetOrdStatus(), msg.GetOrderPrice(),
        msg.GetOrderQty(), msg.GetLeavesQty(), msg.GetCumQty(), msg.GetOrderId(), &rtn);
    int rid = static_cast<int>(request_id);
    forward_order_return(rtn, rid, route);

    const bool startup_cancel_mode = unit->startup_cancel_all_orders
        && request_id == unit->startup_order_query_rid;
    if (startup_cancel_mode) {
        std::string reason;
        if (!should_startup_cancel_order(msg, &reason)) {
            KF_LOG_INFO(logger, "[StartupCancelSkip] account_index=" << account_index
                << " rid=" << request_id
                << " ref=" << route.order_ref
                << " instrument=" << route.instrument
                << " cl_ord_no=" << msg.GetClOrdNo()
                << " ord_status=" << static_cast<int>(msg.GetOrdStatus())
                << " leaves_qty=" << msg.GetLeavesQty()
                << " reason=" << reason);
        } else {
            bool duplicate = false;
            {
                std::lock_guard<std::mutex> lock(unit->startup_cancel_mutex);
                auto inserted = unit->startup_cancel_submitted_clordnos.insert(msg.GetClOrdNo());
                duplicate = !inserted.second;
            }
            if (duplicate) {
                KF_LOG_INFO(logger, "[StartupCancelSkip] account_index=" << account_index
                    << " rid=" << request_id
                    << " ref=" << route.order_ref
                    << " instrument=" << route.instrument
                    << " cl_ord_no=" << msg.GetClOrdNo()
                    << " reason=duplicate_cl_ord_no");
            } else if (send_startup_cancel_for_order_query_item(account_index, static_cast<int>(request_id), msg)) {
                KF_LOG_INFO(logger, "[StartupCancelSent] account_index=" << account_index
                    << " rid=" << request_id
                    << " ref=" << route.order_ref
                    << " instrument=" << route.instrument
                    << " cl_ord_no=" << msg.GetClOrdNo());
            }
        }
        if (is_last) {
            KF_LOG_INFO(logger, "[StartupCancelSummary] account_index=" << account_index
                << " rid=" << request_id
                << " last=1");
        }
    }
}

void TDEngineGXBSE::on_rsp_cash_security_info_query(
    int account_index,
    const ATPRspCashExtQueryResultSecurityInfoMsg& msg,
    int64_t request_id,
    const ATPRspErrorInfo& error_info,
    bool is_last)
{
    (void)account_index;
    if (is_last) {
        uint64_t latency_ns = 0;
        if (consume_request_send_latency(request_id, &latency_ns)) {
            {
                std::lock_guard<std::mutex> lock(latency_mutex_);
                record_latency(limit_price_query_latency_, latency_ns);
            }
            maybe_log_latency_stats(now_ns());
        }
    }
    LFMarketDataField rsp = {};
    copy_text(rsp.InstrumentID, sizeof(rsp.InstrumentID), msg.GetSecurityId());
    copy_text(rsp.ExchangeID, sizeof(rsp.ExchangeID), exchange_from_market(msg.GetMarketId()));
    rsp.UpperLimitPrice = msg.GetPriceUpperLimit();
    rsp.LowerLimitPrice = msg.GetPriceLowerLimit();

    auto cc = get_cc();
    if (cc != nullptr) {
        cc->on_rsp_limit_price(&rsp, static_cast<int>(request_id), source_id, error_info.error_id);
    }
    if (is_last) {
        std::lock_guard<std::mutex> lock(route_mutex_);
        limit_price_request_account_.erase(request_id);
    }
    if (error_info.error_id != 0) {
        KF_LOG_ERROR(logger, "[OnRspSecurityInfo] rid=" << request_id
            << " ec=" << error_info.error_id
            << " msg=" << error_info.GetErrorMsg());
    }
}

int64_t TDEngineGXBSE::next_request_id()
{
    int64_t value = next_request_id_.fetch_add(1);
    if (value <= 0) {
        value = next_request_id_.fetch_add(1);
    }
    return value;
}

bool TDEngineGXBSE::valid_account(int account_index) const
{
    return account_index >= 0 && account_index < static_cast<int>(account_units_.size());
}

AccountUnitGXBSE* TDEngineGXBSE::unit_at(int account_index)
{
    return valid_account(account_index) ? &account_units_[static_cast<std::size_t>(account_index)] : nullptr;
}

const AccountUnitGXBSE* TDEngineGXBSE::unit_at(int account_index) const
{
    return valid_account(account_index) ? &account_units_[static_cast<std::size_t>(account_index)] : nullptr;
}

void TDEngineGXBSE::store_request_route(int64_t request_id, const OrderRoute& route)
{
    std::lock_guard<std::mutex> lock(route_mutex_);
    request_to_route_[request_id] = route;
}

void TDEngineGXBSE::update_request_route_api_return_time(int64_t request_id, uint64_t api_return_time_ns)
{
    std::lock_guard<std::mutex> lock(route_mutex_);
    auto it = request_to_route_.find(request_id);
    if (it != request_to_route_.end()) {
        it->second.api_return_time_ns = api_return_time_ns;
    }
}

void TDEngineGXBSE::bind_clord_route(int64_t request_id, int64_t cl_ord_no)
{
    std::lock_guard<std::mutex> lock(route_mutex_);
    auto it = request_to_route_.find(request_id);
    if (it == request_to_route_.end()) {
        return;
    }
    clord_to_route_[cl_ord_no] = it->second;
    order_ref_to_clord_[it->second.order_ref] = cl_ord_no;
}

void TDEngineGXBSE::bind_clord_route(int64_t cl_ord_no, const OrderRoute& route)
{
    if (cl_ord_no <= 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(route_mutex_);
    clord_to_route_[cl_ord_no] = route;
    if (route.order_ref > 0) {
        order_ref_to_clord_[route.order_ref] = cl_ord_no;
    }
}

bool TDEngineGXBSE::lookup_route_by_request_id(int64_t request_id, OrderRoute* route)
{
    std::lock_guard<std::mutex> lock(route_mutex_);
    auto it = request_to_route_.find(request_id);
    if (it == request_to_route_.end()) {
        return false;
    }
    if (route != nullptr) {
        *route = it->second;
    }
    return true;
}

bool TDEngineGXBSE::lookup_route_by_clord(int64_t cl_ord_no, OrderRoute* route)
{
    std::lock_guard<std::mutex> lock(route_mutex_);
    auto it = clord_to_route_.find(cl_ord_no);
    if (it == clord_to_route_.end()) {
        return false;
    }
    if (route != nullptr) {
        *route = it->second;
    }
    return true;
}

bool TDEngineGXBSE::should_startup_cancel_order(const ATPRspCashOrderQueryResultMsg& msg,
                                                std::string* reason) const
{
    if (msg.GetClOrdNo() <= 0) {
        if (reason != nullptr) {
            *reason = "missing_cl_ord_no";
        }
        return false;
    }
    if (qty_to_int(msg.GetLeavesQty()) <= 0) {
        if (reason != nullptr) {
            *reason = "no_leaves_qty";
        }
        return false;
    }
    const char status = to_lf_order_status(msg.GetOrdStatus());
    if (status == LF_CHAR_AllTraded
        || status == LF_CHAR_PartTradedNotQueueing
        || status == LF_CHAR_NoTradeNotQueueing
        || status == LF_CHAR_Canceled
        || status == LF_CHAR_Error) {
        if (reason != nullptr) {
            *reason = "terminal_status";
        }
        return false;
    }
    if (msg.GetOrigClOrdNo() > 0) {
        if (reason != nullptr) {
            *reason = "is_cancel_order";
        }
        return false;
    }

    if (reason != nullptr) {
        *reason = "ok";
    }
    return true;
}

bool TDEngineGXBSE::send_startup_cancel_for_order_query_item(
    int account_index,
    int request_id,
    const ATPRspCashOrderQueryResultMsg& msg)
{
    AccountUnitGXBSE* unit = unit_at(account_index);
    if (unit == nullptr || unit->api == nullptr) {
        KF_LOG_ERROR(logger, "[StartupCancelSend] invalid account account_index=" << account_index);
        return false;
    }

    ATPReqCashCancelOrderMsg* cancel = ATPReqCashCancelOrderMsg::NewMessage();
    cancel->SetCustId(unit->cust_id.c_str());
    cancel->SetFundAccountId(unit->fund_account_id.c_str());
    cancel->SetBranchId(unit->branch_id.c_str());
    cancel->SetAccountId(unit->account_id.c_str());
    cancel->SetPassword(unit->password.c_str());
    cancel->SetOrigClOrdNo(msg.GetClOrdNo());
    cancel->SetBatchClOrdNo(msg.GetBatchClOrdNo());
    cancel->SetMarketId(msg.GetMarketId());

    const int64_t atp_request_id = next_request_id();
    ATPErrorCodeType ec = unit->api->ReqCashCancelOrder(cancel, atp_request_id);
    ATPReqCashCancelOrderMsg::DeleteMessage(cancel);
    if (ec != ATPErrorCode::kSuccess) {
        KF_LOG_ERROR(logger, "[StartupCancelSend] failed account_index=" << account_index
            << " rid=" << request_id
            << " cl_ord_no=" << msg.GetClOrdNo()
            << " ec=" << ec);
        return false;
    }
    return true;
}

bool TDEngineGXBSE::lookup_clord_by_order_ref(long order_ref, int64_t* cl_ord_no)
{
    std::lock_guard<std::mutex> lock(route_mutex_);
    auto it = order_ref_to_clord_.find(order_ref);
    if (it == order_ref_to_clord_.end()) {
        return false;
    }
    if (cl_ord_no != nullptr) {
        *cl_ord_no = it->second;
    }
    return true;
}

uint16_t TDEngineGXBSE::resolve_market_id(const AccountUnitGXBSE& unit, const char* exchange_id) const
{
    if (exchange_id != nullptr && exchange_id[0] != '\0') {
        if (std::strcmp(exchange_id, "BSE") == 0 || std::strcmp(exchange_id, "BJ") == 0) {
            return ATPMarketIDConst::kBeiJing;
        }
        if (std::strcmp(exchange_id, "SSE") == 0 || std::strcmp(exchange_id, "SH") == 0) {
            return ATPMarketIDConst::kShangHai;
        }
        if (std::strcmp(exchange_id, "SZE") == 0 || std::strcmp(exchange_id, "SZ") == 0) {
            return ATPMarketIDConst::kShenZhen;
        }
    }
    return static_cast<uint16_t>(unit.market_id);
}

uint8_t TDEngineGXBSE::resolve_business_type(const AccountUnitGXBSE& unit) const
{
    return static_cast<uint8_t>(unit.business_type);
}

char TDEngineGXBSE::to_atp_side(char direction) const
{
    return direction == LF_CHAR_Sell ? ATPSideConst::kSell : ATPSideConst::kBuy;
}

char TDEngineGXBSE::to_atp_order_type(char order_price_type) const
{
    return order_price_type == LF_CHAR_AnyPrice ? ATPOrderTypeConst::kMarket : ATPOrderTypeConst::kFixedNew;
}

char TDEngineGXBSE::to_lf_direction(char side) const
{
    return side == ATPSideConst::kSell ? LF_CHAR_Sell : LF_CHAR_Buy;
}

char TDEngineGXBSE::to_lf_order_status(uint8_t ord_status) const
{
    switch (ord_status) {
        case ATPOrdStatusConst::kNew:
        case ATPOrdStatusConst::kSended:
            return LF_CHAR_NoTradeQueueing;
        case ATPOrdStatusConst::kPartiallyFilled:
        case ATPOrdStatusConst::kPartiallyFilledWaitCancelled:
            return LF_CHAR_PartTradedQueueing;
        case ATPOrdStatusConst::kFilled:
            return LF_CHAR_AllTraded;
        case ATPOrdStatusConst::kPartiallyFilledPartiallyCancelled:
        case ATPOrdStatusConst::kPartiallyCancelled:
            return LF_CHAR_PartTradedNotQueueing;
        case ATPOrdStatusConst::kCancelled:
        case ATPOrdStatusConst::kInternalCanceledOrder:
        case ATPOrdStatusConst::kCanceledSuccess:
            return LF_CHAR_Canceled;
        case ATPOrdStatusConst::kReject:
        case ATPOrdStatusConst::kInternalRejectOrder:
            return LF_CHAR_Error;
        default:
            return LF_CHAR_OrderAccepted;
    }
}

const char* TDEngineGXBSE::exchange_from_market(uint16_t market_id) const
{
    if (market_id == ATPMarketIDConst::kBeiJing) {
        return "BSE";
    }
    if (market_id == ATPMarketIDConst::kShangHai) {
        return "SSE";
    }
    if (market_id == ATPMarketIDConst::kShenZhen) {
        return "SZE";
    }
    return "BSE";
}

void TDEngineGXBSE::fill_order_from_route(const OrderRoute& route, const AccountUnitGXBSE& unit,
                                          int64_t cl_ord_no, uint8_t ord_status,
                                          double order_price, double order_qty,
                                          double leaves_qty, double cum_qty,
                                          const char* order_id, LFRtnOrderField* out) const
{
    if (out == nullptr) {
        return;
    }
    copy_text(out->BrokerID, sizeof(out->BrokerID), "");
    copy_text(out->UserID, sizeof(out->UserID), unit.cust_id);
    copy_text(out->InvestorID, sizeof(out->InvestorID), unit.cust_id);
    copy_text(out->InstrumentID, sizeof(out->InstrumentID), route.instrument);
    copy_text(out->ExchangeID, sizeof(out->ExchangeID), exchange_from_market(static_cast<uint16_t>(unit.market_id)));
    copy_text(out->OrderSysID, sizeof(out->OrderSysID), std::to_string(cl_ord_no));
    copy_text(out->OrderNo, sizeof(out->OrderNo), order_id);
    out->OrderRef = route.order_ref;
    out->LimitPrice = order_price;
    out->VolumeTotalOriginal = qty_to_int(order_qty);
    out->VolumeTotal = qty_to_int(leaves_qty);
    out->VolumeTraded = qty_to_int(cum_qty);
    out->TimeCondition = route.time_condition;
    out->VolumeCondition = route.volume_condition;
    out->OrderPriceType = route.order_price_type;
    out->Direction = route.direction;
    out->OffsetFlag = route.offset_flag;
    out->HedgeFlag = LF_CHAR_Speculation;
    out->OrderStatus = to_lf_order_status(ord_status);
    out->RequestID = route.request_id;
}

void TDEngineGXBSE::push_trade_if_needed(const OrderRoute& route, const AccountUnitGXBSE& unit,
                                         const ATPRtnCashAuctionOrderMsg& msg,
                                         int cum_qty, int request_id)
{
    int last_cum = 0;
    {
        std::lock_guard<std::mutex> lock(route_mutex_);
        auto it = clord_to_route_.find(msg.GetClOrdNo());
        if (it != clord_to_route_.end()) {
            last_cum = it->second.last_cum_qty;
            it->second.last_cum_qty = std::max(it->second.last_cum_qty, cum_qty);
        }
    }
    const int delta = std::max(0, cum_qty - last_cum);
    const int last_qty = qty_to_int(msg.GetLastQty());
    const int trade_qty = last_qty > 0 ? last_qty : delta;
    if (trade_qty <= 0) {
        return;
    }

    LFRtnTradeField trade = {};
    copy_text(trade.BrokerID, sizeof(trade.BrokerID), "");
    copy_text(trade.UserID, sizeof(trade.UserID), unit.cust_id);
    copy_text(trade.InvestorID, sizeof(trade.InvestorID), unit.cust_id);
    copy_text(trade.InstrumentID, sizeof(trade.InstrumentID), route.instrument);
    copy_text(trade.ExchangeID, sizeof(trade.ExchangeID), exchange_from_market(msg.GetMarketId()));
    copy_text(trade.TradeID, sizeof(trade.TradeID), msg.GetExecId());
    copy_text(trade.OrderSysID, sizeof(trade.OrderSysID), std::to_string(msg.GetClOrdNo()));
    copy_text(trade.OrderNo, sizeof(trade.OrderNo), msg.GetOrderId());
    trade.OrderRef = route.order_ref;
    trade.Price = msg.GetLastPx() > 0.0 ? msg.GetLastPx() : msg.GetPrice();
    trade.Volume = trade_qty;
    trade.VolumeTraded = qty_to_int(msg.GetCumQty());
    trade.VolumeTotal = qty_to_int(msg.GetLeavesQty());
    trade.VolumeTotalOriginal = qty_to_int(msg.GetOrderQty());
    trade.Direction = route.direction;
    trade.OffsetFlag = route.offset_flag;
    trade.HedgeFlag = LF_CHAR_Speculation;
    int rid = request_id;
    forward_trade_return(trade, rid, route);
}

void TDEngineGXBSE::forward_order_return(const LFRtnOrderField& rtn, int request_id, const OrderRoute& route)
{
    if (route.direct_submit) {
        auto cc = get_cc();
        if (cc != nullptr) {
            cc->on_rtn_order(&rtn, request_id, source_id, 0);
            return;
        }
    }
    LFRtnOrderField copy = rtn;
    int rid = request_id;
    on_rtn_order(&copy, rid);
}

void TDEngineGXBSE::forward_trade_return(const LFRtnTradeField& trade, int request_id, const OrderRoute& route)
{
    if (route.direct_submit) {
        auto cc = get_cc();
        if (cc != nullptr) {
            cc->on_rtn_trade(&trade, request_id, source_id, 0);
            return;
        }
    }
    LFRtnTradeField copy = trade;
    int rid = request_id;
    on_rtn_trade(&copy, rid);
}

std::string TDEngineGXBSE::json_string_or(const json& j, const char* key, const std::string& fallback)
{
    if (j.find(key) == j.end()) {
        return fallback;
    }
    try {
        return j[key].get<std::string>();
    } catch (...) {
        return fallback;
    }
}

int TDEngineGXBSE::json_int_or(const json& j, const char* key, int fallback)
{
    if (j.find(key) == j.end()) {
        return fallback;
    }
    try {
        return j[key].get<int>();
    } catch (...) {
        return fallback;
    }
}

bool TDEngineGXBSE::json_bool_or(const json& j, const char* key, bool fallback)
{
    if (j.find(key) == j.end()) {
        return fallback;
    }
    try {
        return j[key].get<bool>();
    } catch (...) {
        return fallback;
    }
}

std::string TDEngineGXBSE::join_locations(const json& j)
{
    if (j.find("locations") != j.end() && j["locations"].is_array()) {
        std::ostringstream oss;
        for (std::size_t i = 0; i < j["locations"].size(); ++i) {
            if (i != 0) {
                oss << ';';
            }
            oss << j["locations"][i].get<std::string>();
        }
        return oss.str();
    }
    const std::string primary = json_string_or(j, "primary_location", "");
    const std::string backup = json_string_or(j, "backup_location", "");
    if (!primary.empty() && !backup.empty()) {
        return primary + ";" + backup;
    }
    if (!primary.empty()) {
        return primary;
    }
    return json_string_or(j, "location", "127.0.0.1:32001");
}

void TDEngineGXBSE::copy_text(char* dst, std::size_t dst_size, const std::string& value)
{
    copy_text(dst, dst_size, value.c_str());
}

void TDEngineGXBSE::copy_text(char* dst, std::size_t dst_size, const char* value)
{
    if (dst == nullptr || dst_size == 0) {
        return;
    }
    std::snprintf(dst, dst_size, "%s", value == nullptr ? "" : value);
}

int TDEngineGXBSE::qty_to_int(double qty)
{
    if (qty <= 0.0) {
        return 0;
    }
    return static_cast<int>(std::llround(qty));
}

#define EXPORT_FLAG __attribute__((__visibility__("default")))

#ifdef __cplusplus
extern "C"
{
#endif

EXPORT_FLAG ITDEngine* get_obj(IControlCenter* pcc);
EXPORT_FLAG int gxbse_direct_cash_order(const GxbseDirectOrderRequest* request,
                                        GxbseDirectOrderResult* result);
EXPORT_FLAG int gxbse_direct_cancel_order(const GxbseDirectCancelRequest* request,
                                          GxbseDirectCancelResult* result);

#ifdef __cplusplus
}
#endif

ITDEngine* get_obj(IControlCenter* pcc)
{
    return kungfu::wingchun::td_get_obj<TDEngineGXBSE>(pcc, T0_TD_ENGINE_KEY);
}

int gxbse_direct_cash_order(const GxbseDirectOrderRequest* request, GxbseDirectOrderResult* result)
{
    TDEngineGXBSE* engine = g_gxbse_direct_engine.load(std::memory_order_acquire);
    if (engine == nullptr) {
        if (result != nullptr) {
            *result = GxbseDirectOrderResult{};
            result->ret = -1;
        }
        return -1;
    }
    return engine->direct_cash_order(request, result);
}

int gxbse_direct_cancel_order(const GxbseDirectCancelRequest* request, GxbseDirectCancelResult* result)
{
    TDEngineGXBSE* engine = g_gxbse_direct_engine.load(std::memory_order_acquire);
    if (engine == nullptr) {
        if (result != nullptr) {
            *result = GxbseDirectCancelResult{};
            result->ret = -1;
        }
        return -1;
    }
    return engine->direct_cancel_order(request, result);
}
