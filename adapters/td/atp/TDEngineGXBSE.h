/*****************************************************************************/
/* Guoxin BSE ATPQuantAPI trade adapter. */
/*****************************************************************************/

#ifndef TDENGINEGXBSE_H
#define TDENGINEGXBSE_H

#include "ITDEngine.h"
#include "longfist/LFConstants.h"
#include "atp_quant_api.h"
#include "adapters/td/atp/GxbseDirectApi.h"
#include "adapters/td/atp/OmsAtpBackend.h"
#include "adapters/td/atp/AtpOmsRequests.h"
#include "common/execution/AtpSnapshotCollector.h"
#include "common/execution/LiveTd.h"
#include <map>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <array>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifndef T0_TD_DEFAULT_MARKET_ID
#define T0_TD_DEFAULT_MARKET_ID 104
#endif
#ifndef T0_TD_ENGINE_KEY
#define T0_TD_ENGINE_KEY "gxbse"
#endif

WC_NAMESPACE_START

struct AccountUnitGXBSE
{
    AccountUnitGXBSE() = default;
    AccountUnitGXBSE(const AccountUnitGXBSE&) = delete;
    AccountUnitGXBSE& operator=(const AccountUnitGXBSE&) = delete;
    AccountUnitGXBSE(AccountUnitGXBSE&& other) noexcept;
    AccountUnitGXBSE& operator=(AccountUnitGXBSE&& other) noexcept;

    std::unique_ptr<atp::quant_api::ATPQuantAPI> api;
    std::unique_ptr<atp::quant_api::ATPQuantHandler> handler;

    std::string locations;
    std::string order_way = "N";
    std::string client_feature_code;
    std::string bind_ip_address;
    std::string agw_user;
    std::string agw_password;

    std::string cust_id;
    std::string fund_account_id;
    std::string branch_id;
    std::string account_id;
    std::string password;

    int market_id = T0_TD_DEFAULT_MARKET_ID;
    int business_type = 1;
    int return_num = 100;
    int receive_thread_cpu = 255;
    int send_thread_cpu = 255;
    bool is_tcp_direct = false;
    bool enable_latency = false;
    bool bypass_account_queries = false;
    bool enable_position_sync = true;
    bool startup_cancel_all_orders = false;
    int position_sync_interval_sec = 5;
    int log_level = 4;
    std::string api_log_dir;

    std::atomic<bool> connected{false};
    std::atomic<bool> logged_in{false};
    std::atomic<bool> periodic_position_query_inflight{false};
    std::atomic<bool> periodic_account_query_inflight{false};
    int startup_order_query_rid = -1;
    std::mutex startup_cancel_mutex;
    std::unordered_set<int64_t> startup_cancel_submitted_clordnos;
};

class TDEngineGXBSE : public ITDEngine
{
public:
    TDEngineGXBSE();
    ~TDEngineGXBSE() override;

    void init() override;
    void pre_load(const json& j_config) override;
    TradeAccount load_account(int idx, const json& j_account) override;
    void resize_accounts(int account_num) override;
    void connect(long timeout_nsec) override;
    void login(long timeout_nsec) override;
    void logout() override;
    void release_api() override;
    bool is_connected() const override;
    bool is_logged_in() const override;
    std::string name() const override { return T0_TD_ENGINE_KEY; }

    void req_investor_position(const LFQryPositionField* data, int account_index, int requestId) override;
    void req_qry_account(const LFQryAccountField* data, int account_index, int requestId) override;
    int req_order_insert(const LFInputOrderField* data, int account_index, int requestId, long rcv_time) override;
    void req_order_action(const LFOrderActionField* data, int account_index, int requestId, long rcv_time) override;
    void req_qry_order_info(const LFQryOrderField* data, int account_index, int requestId) override;
    void req_qry_limit_price(const LFQryLimitPrice* data, int account_index, int requestId) override;
    void pre_run() override;
    int direct_cash_order(const GxbseDirectOrderRequest* request, GxbseDirectOrderResult* result);
    int direct_cancel_order(const GxbseDirectCancelRequest* request, GxbseDirectCancelResult* result);
    std::shared_ptr<oms::Backend> make_oms_backend(int account_index, const oms::Scope& scope);
    void init_stream(const json& account, const oms::Scope& scope,
                     const std::set<oms::Instrument>& universe, bool allow_orders);
    void attach_stream_oms(const std::shared_ptr<oms::Engine>& engine) { stream_oms_ = engine; }
    std::string stream_status() const;
    void on_rsp_cash_trade_query(int account_index,
        const atp::quant_api::ATPRspCashTradeOrderQueryResultMsg& msg,
        int64_t request_id, const atp::quant_api::ATPRspErrorInfo& error_info, bool is_last);

    void on_login(int account_index, const atp::quant_api::ATPCustomerInfo& msg);
    void on_logout(int account_index, const char* desc);
    void on_recovering(int account_index, const char* desc);
    void on_rsp_cash_auction_order(int account_index,
                                   const atp::quant_api::ATPRspCashAuctionOrderMsg& msg,
                                   const atp::quant_api::ATPRspErrorInfo& error_info,
                                   int64_t request_id);
    void on_rsp_cash_cancel_order(int account_index,
                                  const atp::quant_api::ATPRspCashCancelOrderMsg& msg,
                                  const atp::quant_api::ATPRspErrorInfo& error_info,
                                  int64_t request_id);
    void on_rtn_cash_auction_order(int account_index, const atp::quant_api::ATPRtnCashAuctionOrderMsg& msg);
    void on_rsp_cash_share_query(int account_index,
                                 const atp::quant_api::ATPRspCashShareQueryResultMsg& msg,
                                 int64_t request_id,
                                 const atp::quant_api::ATPRspErrorInfo& error_info,
                                 bool is_last);
    void on_rsp_cash_fund_query(int account_index,
                                const atp::quant_api::ATPRspCashFundQueryResultMsg& msg,
                                int64_t request_id,
                                const atp::quant_api::ATPRspErrorInfo& error_info,
                                bool is_last);
    void on_rsp_cash_order_query(int account_index,
                                 const atp::quant_api::ATPRspCashOrderQueryResultMsg& msg,
                                 int64_t request_id,
                                 const atp::quant_api::ATPRspErrorInfo& error_info,
                                 bool is_last);
    void on_rsp_cash_trade_order_query(int account_index,
                                       const atp::quant_api::ATPRspCashTradeOrderQueryResultMsg& msg,
                                       int64_t request_id,
                                       const atp::quant_api::ATPRspErrorInfo& error_info,
                                       bool is_last);
    void on_rsp_cash_security_info_query(int account_index,
                                         const atp::quant_api::ATPRspCashExtQueryResultSecurityInfoMsg& msg,
                                         int64_t request_id,
                                         const atp::quant_api::ATPRspErrorInfo& error_info,
                                         bool is_last);

private:
    enum StreamOmsQueryKind { OmsFunds, OmsPositions, OmsOrders, OmsTrades };
    struct StreamOmsQueryRequest {
        StreamOmsQueryKind kind;
        std::uint64_t token, index;
        std::size_t rows;
        StreamOmsQueryRequest() : kind(OmsFunds), token(0), index(0), rows(0) {}
    };
    bool stream_mode_ = false;
    bool stream_allow_orders_ = false;
    bool stream_disconnected_ = false;
    oms::Scope stream_scope_;
    std::set<oms::Instrument> stream_universe_;
    std::weak_ptr<oms::Engine> stream_oms_;
    mutable std::mutex stream_query_mutex_;
    strategy_runtime::AtpSnapshotCollector oms_collector_;
    std::map<int64_t, StreamOmsQueryRequest> stream_queries_;
    bool oms_snapshot_published_ = false;
    std::string oms_query_error_;
    oms::Error query_oms_snapshot(int account_index, const oms::Scope&, std::uint64_t token);
    oms::Error send_oms_query(StreamOmsQueryKind, std::uint64_t token, std::uint64_t index);
    void finish_oms_query(int64_t request_id, std::uint64_t last_index, bool is_last);
    void fail_oms_query(const std::string& reason);
    bool collect_oms_funds(const atp::quant_api::ATPRspCashFundQueryResultMsg&, int64_t,
                          const atp::quant_api::ATPRspErrorInfo&, bool);
    bool collect_oms_positions(const atp::quant_api::ATPRspCashShareQueryResultMsg&, int64_t,
                              const atp::quant_api::ATPRspErrorInfo&, bool);
    bool collect_oms_orders(const atp::quant_api::ATPRspCashOrderQueryResultMsg&, int64_t,
                           const atp::quant_api::ATPRspErrorInfo&, bool);
    struct OrderRoute
    {
        oms::Scope oms_scope;
        oms::Instrument oms_instrument;
        std::weak_ptr<oms::AtpBackend> oms_backend;
        long order_ref = 0;
        int request_id = -1;
        int account_index = 0;
        std::string instrument;
        double price = 0.0;
        int volume = 0;
        char direction = LF_CHAR_Buy;
        char order_price_type = LF_CHAR_LimitPrice;
        char time_condition = LF_CHAR_GFD;
        char volume_condition = LF_CHAR_AV;
        char offset_flag = LF_CHAR_Open;
        uint64_t send_time_ns = 0;
        uint64_t api_return_time_ns = 0;
        bool first_rtn_observed = false;
        bool direct_submit = false;
    };

    struct PositionQueryContext
    {
        int account_index = 0;
        uint16_t market_id = 0;
        std::string requested_instrument;
        bool saw_any = false;
        bool saw_requested = false;
        bool periodic_sync = false;
        std::unordered_set<std::string> seen_instruments;
    };

    enum OmsQueryKind { OmsQueryFund = 0, OmsQueryShare = 1,
                        OmsQueryOrder = 2, OmsQueryTrade = 3,
                        OmsQueryKindCount = 4 };

    struct OmsQueryState
    {
        OmsQueryState(int account_index_value, const oms::Scope& scope_value,
                      std::uint64_t token_value,
                      const std::string& account_id_value,
                      const std::string& cust_id_value,
                      uint16_t market_id_value,
                      const std::shared_ptr<oms::AtpBackend>& backend_value)
            : account_index(account_index_value), scope(scope_value), token(token_value),
              account_id(account_id_value), cust_id(cust_id_value), market_id(market_id_value),
              backend(backend_value), assembler(scope_value, token_value) {
            for (int i = 0; i < OmsQueryKindCount; ++i) {
                done[i] = false;
                success[i] = true;
            }
        }

        int account_index;
        oms::Scope scope;
        std::uint64_t token;
        std::string account_id;
        std::string cust_id;
        uint16_t market_id;
        std::weak_ptr<oms::AtpBackend> backend;
        oms::AtpSnapshotAssembler assembler;
        bool done[OmsQueryKindCount];
        bool success[OmsQueryKindCount];
    };

    struct OmsQueryRequest
    {
        std::shared_ptr<OmsQueryState> state;
        OmsQueryKind kind = OmsQueryFund;
    };

    struct LatencyCounter
    {
        uint64_t count = 0;
        uint64_t total_ns = 0;
        uint64_t max_ns = 0;
    };

    std::vector<AccountUnitGXBSE> account_units_;
    std::atomic<int64_t> next_request_id_{1};
    std::atomic<bool> api_initialized_{false};
    std::mutex login_mutex_;
    std::condition_variable login_cv_;

    std::mutex route_mutex_;
    friend struct AtpSendTest;
    std::map<int, std::weak_ptr<oms::AtpBackend> > oms_backends_;
    // Pointers are protected by route_mutex_; unordered_map rehash preserves
    // element addresses. Sent routes live in request_to_route_, recovered
    // routes in clord_to_route_; neither store is pruned while referenced.
    std::map<std::pair<int, long>, const OrderRoute*> oms_routes_;
    oms::SendResult send_oms_command(int account_index, const oms::Command& command,
                                    atp_oms::Requests& requests);
    void publish_oms_order(const OrderRoute& route, const atp::quant_api::ATPRtnCashAuctionOrderMsg& msg);
    // A sent order owns one full route record keyed by ATP request id.  The
    // callback indexes below carry only ids; snapshot/reconciliation code
    // still uses the legacy maps above for routes that have no sent request.
    std::unordered_map<int64_t, OrderRoute> request_to_route_;
    std::unordered_map<int64_t, int64_t> clord_to_request_;
    std::map<std::pair<int, long>, int64_t> order_ref_to_request_;
    std::unordered_map<int64_t, OrderRoute> clord_to_route_;
    std::unordered_map<long, int64_t> order_ref_to_clord_;
    std::unordered_map<int64_t, int> limit_price_request_account_;

    std::mutex position_query_mutex_;
    std::unordered_map<int64_t, PositionQueryContext> position_query_contexts_;
    std::mutex account_query_mutex_;
    std::unordered_set<int64_t> periodic_account_query_requests_;
    std::mutex oms_query_mutex_;
    std::unordered_map<int64_t, OmsQueryRequest> oms_query_requests_;
    std::unordered_map<int64_t, int> oms_query_retired_requests_;
    std::unordered_map<int, std::shared_ptr<OmsQueryState> > oms_queries_;
    std::atomic<bool> position_sync_running_{false};
    std::thread position_sync_thread_;
    std::mutex latency_mutex_;
    std::unordered_map<int64_t, uint64_t> request_send_time_ns_;
    LatencyCounter insert_rsp_latency_;
    LatencyCounter insert_first_rtn_latency_;
    LatencyCounter cancel_rsp_latency_;
    LatencyCounter account_query_latency_;
    LatencyCounter position_query_latency_;
    LatencyCounter order_query_latency_;
    LatencyCounter limit_price_query_latency_;
    LatencyCounter periodic_position_query_latency_;
    uint64_t latency_log_interval_ns_ = 60000000000ULL;
    uint64_t next_latency_log_ns_ = 0;
    struct TdOrderSendLogEntry {
        int request_id = 0;
        int64_t atp_request_id = 0;
        long order_ref = 0;
        char instrument[32] = {};
        uint64_t func_enter_ns = 0;
        uint64_t send_time_ns = 0;
        uint64_t api_return_time_ns = 0;
        uint64_t log_begin_ns = 0;
        uint64_t enqueue_begin_ns = 0;
        uint64_t api_send_ns = 0;
        uint64_t func_to_msg_new_begin_ns = 0;
        uint64_t msg_new_ns = 0;
        uint64_t msg_fill_ns = 0;
        uint64_t msg_fill_account_ns = 0;
        uint64_t msg_fill_security_ns = 0;
        uint64_t msg_fill_order_ns = 0;
        uint64_t msg_fill_password_batch_ns = 0;
        uint64_t route_init_ns = 0;
        uint64_t route_store_before_api_ns = 0;
        uint64_t latency_store_ns = 0;
        uint64_t api_return_to_route_store_ns = 0;
        uint64_t route_update_after_api_ns = 0;
        uint64_t msg_delete_ns = 0;
        uint64_t pre_log_total_ns = 0;
        uint64_t dropped_before = 0;
    };
    static constexpr std::size_t kTdOrderSendLogRingCapacity = 1024;
    std::array<TdOrderSendLogEntry, kTdOrderSendLogRingCapacity> td_order_send_log_ring_;
    std::atomic<uint64_t> td_order_send_log_head_{0};
    std::atomic<uint64_t> td_order_send_log_tail_{0};
    std::atomic<uint64_t> td_order_send_log_dropped_{0};
    std::mutex async_info_logger_mutex_;
    std::thread async_info_logger_thread_;
    std::atomic<bool> async_info_logger_started_{false};
    std::atomic<bool> async_info_logger_stop_requested_{false};

    int64_t next_request_id();
    bool valid_account(int account_index) const;
    AccountUnitGXBSE* unit_at(int account_index);
    const AccountUnitGXBSE* unit_at(int account_index) const;

    void store_request_route(int64_t request_id, const OrderRoute& route);
    void update_request_route_api_return_time(int64_t request_id, uint64_t api_return_time_ns);
    void bind_clord_route(int64_t request_id, int64_t cl_ord_no);
    void bind_clord_route(int64_t cl_ord_no, const OrderRoute& route);
    bool lookup_route_by_request_id(int64_t request_id, OrderRoute* route);
    bool lookup_route_by_clord(int64_t cl_ord_no, OrderRoute* route);
    bool lookup_route_by_order_ref(int account_index, long order_ref, OrderRoute* route);
    bool lookup_clord_by_order_ref(long order_ref, int64_t* cl_ord_no);

    void respond_bypass_account(const AccountUnitGXBSE& unit, int request_id);
    void respond_bypass_position(const LFQryPositionField* data, const AccountUnitGXBSE& unit, int request_id);
    void store_position_query(int64_t request_id, const PositionQueryContext& context);
    void erase_position_query(int64_t request_id);
    void finish_position_query(int64_t request_id);
    void start_position_sync_thread();
    void stop_position_sync_thread();
    void position_sync_loop();
    void send_periodic_account_query(int account_index);
    void send_periodic_position_query(int account_index);
    bool is_periodic_account_query(int64_t request_id);
    bool consume_periodic_account_query(int64_t request_id);
    bool is_known_oms_query_request(int64_t request_id, OmsQueryKind kind);
    bool is_oms_query_part(int account_index, int64_t request_id, OmsQueryKind kind);
    oms::Error start_oms_query(int account_index, const oms::Scope& scope, std::uint64_t token);
    bool consume_oms_query_part(int account_index, int64_t request_id, OmsQueryKind kind,
                                bool is_last, bool callback_ok,
                                const std::function<bool(OmsQueryState&)>& append,
                                oms::Snapshot* snapshot,
                                std::shared_ptr<oms::AtpBackend>* backend);
    void erase_oms_query(const std::shared_ptr<OmsQueryState>& state);
    void abort_oms_query(int account_index);
    bool valid_oms_query_identity(const OmsQueryState& state,
                                  const char* fund_account_id,
                                  const char* account_id,
                                  const char* cust_id,
                                  uint16_t market_id,
                                  bool validate_market) const;
    void forward_periodic_position(const LFRspPositionField& pos, bool is_last, int request_id);
    LFRspPositionField make_zero_position(const AccountUnitGXBSE& unit,
                                          const PositionQueryContext& context) const;
    bool should_startup_cancel_order(const atp::quant_api::ATPRspCashOrderQueryResultMsg& msg,
                                     std::string* reason) const;
    bool send_startup_cancel_for_order_query_item(int account_index,
                                                  int request_id,
                                                  const atp::quant_api::ATPRspCashOrderQueryResultMsg& msg);

    uint16_t resolve_market_id(const AccountUnitGXBSE& unit, const char* exchange_id) const;
    uint8_t resolve_business_type(const AccountUnitGXBSE& unit) const;
    char to_atp_side(char direction) const;
    char to_atp_order_type(char order_price_type) const;
    char to_lf_direction(char side) const;
    char to_lf_order_status(uint8_t ord_status) const;
    const char* exchange_from_market(uint16_t market_id) const;

    void fill_order_from_route(const OrderRoute& route, const AccountUnitGXBSE& unit,
                               int64_t cl_ord_no, uint8_t ord_status,
                               double order_price, double order_qty,
                               double leaves_qty, double cum_qty,
                               const char* order_id, LFRtnOrderField* out) const;
    void forward_order_return(const LFRtnOrderField& rtn, int request_id, const OrderRoute& route);
    void forward_trade_return(const LFRtnTradeField& trade, int request_id, const OrderRoute& route);

    static std::string json_string_or(const json& j, const char* key, const std::string& fallback);
    static int json_int_or(const json& j, const char* key, int fallback);
    static bool json_bool_or(const json& j, const char* key, bool fallback);
    static std::string join_locations(const json& j);
    static void copy_text(char* dst, std::size_t dst_size, const std::string& value);
    static void copy_text(char* dst, std::size_t dst_size, const char* value);
    static int qty_to_int(double qty);
    void record_latency(LatencyCounter& counter, uint64_t latency_ns);
    bool consume_request_send_latency(int64_t request_id, uint64_t* latency_ns);
    bool peek_request_send_latency(int64_t request_id, uint64_t* latency_ns);
    void erase_request_send_latency(int64_t request_id);
    void maybe_log_latency_stats(uint64_t now_ns_value);
    void enqueue_td_order_send_log(const TdOrderSendLogEntry& entry);
    void log_td_order_send_entry(const TdOrderSendLogEntry& entry);
    void ensure_async_info_logger_started();
    void stop_async_info_logger(bool drain);
    void async_info_logger_loop();
};

DECLARE_PTR(TDEngineGXBSE);

WC_NAMESPACE_END

#endif
