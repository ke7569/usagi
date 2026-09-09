#include "adapters/td/atp/TDEngineGXBSE.h"
#include "common/config/StreamConfigJson.h"
#include <cmath>
#include <cstring>
#include <limits>
#include <sys/stat.h>
#include <sched.h>

USING_WC_NAMESPACE
using namespace atp::quant_api;
namespace {
typedef strategy_runtime::AtpSnapshotCollector Collector;
const std::size_t kQueryPage = 100;
std::string text_or_empty(const char* value) { return value ? value : ""; }
oms::Quantity quantity(double value) {
    if (!std::isfinite(value) || value < 0 || value != std::floor(value) ||
        value > static_cast<double>(std::numeric_limits<int>::max()))
        throw std::runtime_error("ATP quantity is not a supported integer share count");
    return static_cast<oms::Quantity>(value);
}
oms::Money money(double value) {
    oms::Money result = 0;
    if (!oms::money_from_double(value, &result) || result < 0)
        throw std::runtime_error("ATP money is not representable");
    return result;
}
oms::Side side(char value) {
    if (value == ATPSideConst::kBuy) return oms::Side::Buy;
    if (value == ATPSideConst::kSell) return oms::Side::Sell;
    throw std::runtime_error("unsupported ATP order side");
}
oms::OrderState order_state(char value) {
    if (value == LF_CHAR_AllTraded) return oms::OrderState::Filled;
    if (value == LF_CHAR_Canceled || value == LF_CHAR_PartTradedNotQueueing) return oms::OrderState::Canceled;
    if (value == LF_CHAR_Error) return oms::OrderState::Rejected;
    if (value == LF_CHAR_PartTradedQueueing) return oms::OrderState::Partial;
    return oms::OrderState::Accepted;
}
void check_identity(const oms::Scope& scope, const AccountUnitGXBSE& unit,
                    const char* fund, const char* account, int market) {
    if (text_or_empty(fund) != scope.account.account ||
        text_or_empty(account) != unit.account_id || market != unit.market_id)
        throw std::runtime_error("ATP response account or market mismatch");
}
oms::Error query_error(const std::string& message, int code = 0) {
    oms::Error error; error.category = oms::ErrorCategory::NotReady;
    error.message = message; error.raw_code = code; return error;
}
}

void TDEngineGXBSE::init_stream(const json& account, const oms::Scope& scope,
        const std::set<oms::Instrument>& universe, bool allow_orders) {
    if (scope.source != 190 || scope.account.broker != "guoxin" || !scope.epoch ||
        account.value("bypass_account_queries", false) || account.value("startup_cancel_all_orders", false) ||
        account.value("market_id", 0) != 101)
        throw std::runtime_error("live TD requires Shanghai account, source 190 and complete account queries");
    stream_mode_ = true; stream_allow_orders_ = allow_orders;
    stream_scope_ = scope; stream_universe_ = universe;
    logger = yijinjing::KfLog::getLogger("TradeEngine.usagi_sse_td");
    json input = account;
    // OMS owns reconciliation. Do not start the legacy periodic query/helper path.
    input["enable_position_sync"] = false;
    resize_accounts(1); load_account(0, input);
    if (account_units_[0].fund_account_id != scope.account.account ||
        account_units_[0].cust_id.empty() || account_units_[0].account_id.empty() ||
        account_units_[0].locations.empty() || account_units_[0].client_feature_code.empty())
        throw std::runtime_error("live TD account identity or connection fields missing");
}

std::string TDEngineGXBSE::stream_status() const {
    std::lock_guard<std::mutex> guard(oms_query_mutex_);
    if (!oms_query_error_.empty()) return oms_query_error_;
    if (!oms_collector_.error().empty()) return oms_collector_.error();
    return oms_snapshot_published_ ? "account snapshot delivered" : "account snapshot pending";
}

void TDEngineGXBSE::fail_oms_query(const std::string& reason) {
    std::lock_guard<std::mutex> guard(oms_query_mutex_);
    oms_query_error_ = reason;
    oms_collector_.fail(stream_scope_, oms_collector_.snapshot().token, reason);
}

oms::Error TDEngineGXBSE::query_oms_snapshot(int index, const oms::Scope& scope, std::uint64_t token) {
    if (!stream_mode_ || index != 0 || !(scope == stream_scope_) || !is_logged_in())
        return query_error("ATP snapshot requires the current logged-in Shanghai stream account");
    {
        std::lock_guard<std::mutex> guard(oms_query_mutex_);
        if (stream_disconnected_) return query_error("ATP connection epoch retired");
        if (!oms_collector_.begin(scope, token, stream_universe_))
            return query_error(oms_collector_.last_rejection());
        oms_queries_.clear(); oms_snapshot_published_ = false; oms_query_error_.clear();
    }
    for (int kind = OmsFunds; kind <= OmsTrades; ++kind) {
        const oms::Error error = send_oms_query(static_cast<OmsQueryKind>(kind), token, 0);
        if (error.failed()) { fail_oms_query(error.message); return error; }
    }
    return oms::Error();
}

oms::Error TDEngineGXBSE::send_oms_query(OmsQueryKind kind, std::uint64_t token, std::uint64_t index) {
    AccountUnitGXBSE& unit = account_units_[0];
    if (!unit.api || !unit.logged_in.load()) return query_error("ATP query while disconnected");
    const int64_t rid = next_request_id();
    {
        std::lock_guard<std::mutex> guard(oms_query_mutex_);
        if (stream_disconnected_ || token != oms_collector_.snapshot().token)
            return query_error("ATP query token retired");
        OmsQueryRequest request; request.kind = kind; request.token = token; request.index = index;
        oms_queries_[rid] = request;
    }
    ATPErrorCodeType ec = ATPErrorCode::kSuccess;
    if (kind == OmsFunds) {
        ATPReqCashFundQueryMsg* m = ATPReqCashFundQueryMsg::NewMessage();
        m->SetCustId(unit.cust_id.c_str()); m->SetFundAccountId(unit.fund_account_id.c_str());
        m->SetAccountId(unit.account_id.c_str()); m->SetBranchId(unit.branch_id.c_str());
        m->SetPassword(unit.password.c_str()); m->SetCurrency("CNY"); m->SetMarketId(unit.market_id);
        ec = unit.api->ReqCashFundQuery(m, rid); ATPReqCashFundQueryMsg::DeleteMessage(m);
    } else if (kind == OmsPositions) {
        ATPReqCashShareQueryMsg* m = ATPReqCashShareQueryMsg::NewMessage();
        m->SetCustId(unit.cust_id.c_str()); m->SetFundAccountId(unit.fund_account_id.c_str());
        m->SetAccountId(unit.account_id.c_str()); m->SetBranchId(unit.branch_id.c_str());
        m->SetPassword(unit.password.c_str()); m->SetMarketId(unit.market_id);
        m->SetBusinessType(ATPBusinessTypeConst::kAll); m->SetSecurityId("");
        // The SDK has no share-query cursor. ReturnNum=0 explicitly means all rows.
        m->SetReturnNum(0);
        ec = unit.api->ReqCashShareQuery(m, rid); ATPReqCashShareQueryMsg::DeleteMessage(m);
    } else if (kind == OmsOrders) {
        ATPReqCashOrderQueryMsg* m = ATPReqCashOrderQueryMsg::NewMessage();
        m->SetCustId(unit.cust_id.c_str()); m->SetFundAccountId(unit.fund_account_id.c_str());
        m->SetAccountId(unit.account_id.c_str()); m->SetBranchId(unit.branch_id.c_str());
        m->SetPassword(unit.password.c_str()); m->SetMarketId(unit.market_id); m->SetSecurityId("");
        m->SetBusinessType(ATPBusinessTypeConst::kAll); m->SetSide(ATPSideConst::kAll);
        m->SetOrderQueryCondition(ATPOrderQueryConditionConst::kAll);
        m->SetReturnNum(kQueryPage); m->SetReturnSeq(ATPReturnSeqConst::kTimeOrder);
        m->SetClOrdNo(0); m->SetBatchClOrdNo(0); m->SetQueryIndex(index);
        ec = unit.api->ReqCashOrderQuery(m, rid); ATPReqCashOrderQueryMsg::DeleteMessage(m);
    } else {
        ATPReqCashTradeOrderQueryMsg* m = ATPReqCashTradeOrderQueryMsg::NewMessage();
        m->SetCustId(unit.cust_id.c_str()); m->SetFundAccountId(unit.fund_account_id.c_str());
        m->SetAccountId(unit.account_id.c_str()); m->SetPassword(unit.password.c_str());
        m->SetMarketId(unit.market_id); m->SetSecurityId(""); m->SetBusinessType(ATPBusinessTypeConst::kAll);
        m->SetReturnNum(kQueryPage); m->SetReturnSeq(ATPReturnSeqConst::kTimeOrder);
        m->SetClOrdNo(0); m->SetClOrdId(""); m->SetExecId(""); m->SetBatchClOrdNo(0);
        m->SetTradeOrderQueryCondition(ATPTradeOrderQueryConditionConst::kTradeReport); m->SetQueryIndex(index);
        ec = unit.api->ReqCashTradeOrderQuery(m, rid); ATPReqCashTradeOrderQueryMsg::DeleteMessage(m);
    }
    if (ec != ATPErrorCode::kSuccess) return query_error("ATP snapshot request failed", ec);
    return oms::Error();
}

void TDEngineGXBSE::finish_oms_query(int64_t rid, std::uint64_t last_index, bool last) {
    if (!last) return;
    OmsQueryRequest request; bool next = false; std::uint64_t next_index = 0;
    std::shared_ptr<oms::AtpBackend> backend;
    { std::lock_guard<std::mutex> guard(route_mutex_); backend = oms_backends_[0].lock(); }
    {
        std::lock_guard<std::mutex> guard(oms_query_mutex_);
        const auto found = oms_queries_.find(rid); if (found == oms_queries_.end()) return;
        request = found->second; oms_queries_.erase(found);
        if (!oms_query_error_.empty() || stream_disconnected_) return;
        bool accepted = false;
        if (request.kind == OmsFunds) accepted = oms_collector_.finish_funds(stream_scope_, request.token);
        else if (request.kind == OmsPositions)
            accepted = oms_collector_.finish_all(stream_scope_, request.token, Collector::Positions);
        else {
            const Collector::PageResult page = oms_collector_.finish_page(stream_scope_, request.token,
                request.kind == OmsOrders ? Collector::Orders : Collector::Trades,
                request.index, last_index, request.rows, kQueryPage);
            accepted = page.accepted; next = accepted && !page.done; next_index = page.next_index;
        }
        if (!accepted) { oms_query_error_ = oms_collector_.error(); return; }
        if (oms_collector_.ready() && backend) {
            // Queue while holding the activity lock, preserving the snapshot/report boundary.
            backend->publish_snapshot(oms_collector_.snapshot()); oms_snapshot_published_ = true;
        }
    }
    if (next) {
        const oms::Error error = send_oms_query(request.kind, request.token, next_index);
        if (error.failed()) fail_oms_query(error.message);
    }
}

bool TDEngineGXBSE::collect_oms_funds(const ATPRspCashFundQueryResultMsg& msg,
        int64_t rid, const ATPRspErrorInfo& error, bool last) {
    try {
        {
            std::lock_guard<std::mutex> guard(oms_query_mutex_);
            const auto found = oms_queries_.find(rid); if (found == oms_queries_.end()) return false;
            if (found->second.kind != OmsFunds || error.error_id) throw std::runtime_error("ATP funds query failed");
            check_identity(stream_scope_, account_units_[0], msg.GetFundAccountId(), msg.GetAccountId(), 101);
            if (text_or_empty(msg.GetCurrency()) != "CNY") throw std::runtime_error("ATP funds currency mismatch");
            // SDK: AvailableT1 = 当日可用资金. AvailableT0 is withdrawable, not buying power.
            if (!oms_collector_.add_funds(stream_scope_, found->second.token, money(msg.GetAvailableT1())))
                throw std::runtime_error(oms_collector_.error());
        }
        finish_oms_query(rid, 0, last); return true;
    } catch (const std::exception& e) { fail_oms_query(e.what()); return false; }
}

bool TDEngineGXBSE::collect_oms_positions(const ATPRspCashShareQueryResultMsg& msg,
        int64_t rid, const ATPRspErrorInfo& error, bool last) {
    try {
        {
            std::lock_guard<std::mutex> guard(oms_query_mutex_);
            const auto found = oms_queries_.find(rid); if (found == oms_queries_.end()) return false;
            const bool empty = error.error_id == ATPErrorCode::kQNotFoundShare ||
                error.error_id == ATPErrorCode::kQSharePositionNotExist;
            if (found->second.kind != OmsPositions || (error.error_id && !empty) || (empty && !last))
                throw std::runtime_error("ATP positions query failed");
            if (!empty && !text_or_empty(msg.GetSecurityId()).empty()) {
                check_identity(stream_scope_, account_units_[0], msg.GetFundAccountId(), msg.GetAccountId(), msg.GetMarketId());
                oms::SnapshotPosition row; row.instrument = oms::Instrument{"SSE", msg.GetSecurityId()};
                row.total = quantity(msg.GetLeavesQty()); row.free_sellable = quantity(msg.GetAvailableQty());
                if (!oms_collector_.add_position(stream_scope_, found->second.token, row))
                    throw std::runtime_error(oms_collector_.error());
            }
        }
        finish_oms_query(rid, 0, last); return true;
    } catch (const std::exception& e) { fail_oms_query(e.what()); return false; }
}

bool TDEngineGXBSE::collect_oms_orders(const ATPRspCashOrderQueryResultMsg& msg,
        int64_t rid, const ATPRspErrorInfo& error, bool last) {
    try {
        {
            std::lock_guard<std::mutex> guard(oms_query_mutex_);
            const auto found = oms_queries_.find(rid); if (found == oms_queries_.end()) return false;
            const bool empty = error.error_id == ATPErrorCode::kQNotFoundOrder;
            if (found->second.kind != OmsOrders || (error.error_id && !empty) || (empty && !last))
                throw std::runtime_error("ATP orders query failed");
            if (!empty && msg.GetClOrdNo() > 0) {
                ++found->second.rows;
                check_identity(stream_scope_, account_units_[0], msg.GetFundAccountId(), msg.GetAccountId(), msg.GetMarketId());
                if (msg.GetOrigClOrdNo() != 0) {
                    oms_collector_.skip_row(stream_scope_, found->second.token, Collector::Orders);
                } else {
                    oms::SnapshotOrder row; row.broker_id = std::to_string(msg.GetClOrdNo());
                    row.instrument = oms::Instrument{"SSE", msg.GetSecurityId()}; row.side = side(msg.GetSide());
                    row.price = money(msg.GetOrderPrice()); row.original = quantity(msg.GetOrderQty());
                    row.filled = quantity(msg.GetCumQty()); row.working = quantity(msg.GetLeavesQty());
                    row.state = order_state(to_lf_order_status(msg.GetOrdStatus()));
                    const auto engine = stream_oms_.lock(); oms::OrderView owned;
                    if (engine && msg.GetBatchClOrdNo() && engine->order(msg.GetBatchClOrdNo(), &owned) && owned.owned &&
                        owned.command.intent.instrument == row.instrument && owned.command.intent.side == row.side &&
                        owned.command.intent.price == row.price && owned.command.intent.quantity == row.original &&
                        (owned.command.broker_id.empty() || owned.command.broker_id == row.broker_id)) {
                        row.id = owned.command.id; row.owner = owned.command.intent.owner;
                        OrderRoute route; route.oms_scope = stream_scope_; route.oms_instrument = row.instrument;
                        route.order_ref = row.id; route.account_index = 0; route.instrument = row.instrument.code;
                        route.price = msg.GetOrderPrice(); route.volume = row.original;
                        route.direction = msg.GetSide() == ATPSideConst::kBuy ? LF_CHAR_Buy : LF_CHAR_Sell;
                        route.offset_flag = row.side == oms::Side::Buy ? LF_CHAR_Open : LF_CHAR_Close;
                        std::lock_guard<std::mutex> routes(route_mutex_);
                        route.oms_backend = oms_backends_[0]; oms_routes_[std::make_pair(0, route.order_ref)] = route;
                        clord_to_route_[msg.GetClOrdNo()] = route; order_ref_to_clord_[route.order_ref] = msg.GetClOrdNo();
                    }
                    if (!oms_collector_.add_order(stream_scope_, found->second.token, row))
                        throw std::runtime_error(oms_collector_.error());
                }
            }
        }
        finish_oms_query(rid, msg.GetIndex() > 0 ? msg.GetIndex() : 0, last); return true;
    } catch (const std::exception& e) { fail_oms_query(e.what()); return false; }
}

void TDEngineGXBSE::on_rsp_cash_trade_query(int account_index, const ATPRspCashTradeOrderQueryResultMsg& msg,
        int64_t rid, const ATPRspErrorInfo& error, bool last) {
    if (!stream_mode_ || account_index != 0) return;
    try {
        {
            std::lock_guard<std::mutex> guard(oms_query_mutex_);
            const auto found = oms_queries_.find(rid); if (found == oms_queries_.end()) return;
            const bool empty = error.error_id == ATPErrorCode::kQNotFoundTER;
            if (found->second.kind != OmsTrades || (error.error_id && !empty) || (empty && !last))
                throw std::runtime_error("ATP trades query failed");
            if (!empty && msg.GetClOrdNo() > 0) {
                ++found->second.rows;
                check_identity(stream_scope_, account_units_[0], msg.GetFundAccountId(), msg.GetAccountId(), msg.GetMarketId());
                if (msg.GetExecType() != ATPExecTypeConst::Trade || msg.GetCancelFlag()) {
                    oms_collector_.skip_row(stream_scope_, found->second.token, Collector::Trades);
                } else {
                    oms::Report row; row.scope = stream_scope_; row.kind = oms::ReportKind::Trade;
                    row.broker_id = std::to_string(msg.GetClOrdNo()); row.trade_id = text_or_empty(msg.GetExecId());
                    row.instrument = oms::Instrument{"SSE", msg.GetSecurityId()}; row.side = side(msg.GetSide());
                    row.trade_quantity = quantity(msg.GetLastQty()); row.trade_price = money(msg.GetLastPx());
                    row.trade_fee = money(msg.GetFee()); row.fee_is_final = true;
                    if (!oms_collector_.add_trade(stream_scope_, found->second.token, row))
                        throw std::runtime_error(oms_collector_.error());
                }
            }
        }
        finish_oms_query(rid, msg.GetIndex() > 0 ? msg.GetIndex() : 0, last);
    } catch (const std::exception& e) { fail_oms_query(e.what()); }
}

namespace {
class StreamAtpSession : public strategy_runtime::LiveTdSession {
public:
    StreamAtpSession(const char* path, const oms::Scope& scope,
            const std::set<oms::Instrument>& universe, bool allow_orders) {
        struct stat info;
        if (::stat(path, &info) || !S_ISREG(info.st_mode) || (info.st_mode & 0077))
            throw std::runtime_error("TD credentials must be a private regular file (mode 0600)");
        const nlohmann::json root = load_stream_json(path);
        const nlohmann::json& td = root.at("td").at("sse_td");
        if (td.at("accounts").size() != 1) throw std::runtime_error("stream TD supports one explicit account");
        nlohmann::json account = td.at("accounts").at(0).at("info");
        // The journal entry temporarily pins its owner to the leased TD CPU
        // during creation/login. Reuse ATP's own affinity fields so SDK
        // workers cannot override that lease with stale credential settings.
        cpu_set_t affinity; CPU_ZERO(&affinity);
        if (::sched_getaffinity(0, sizeof(affinity), &affinity))
            throw std::runtime_error("cannot read stream TD CPU affinity");
        if (CPU_COUNT(&affinity) == 1) {
            for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
                if (!CPU_ISSET(cpu, &affinity)) continue;
                if (cpu >= 255) throw std::runtime_error("stream TD CPU exceeds ATP's supported CPU range");
                account["receive_thread_cpu"] = cpu;
                account["send_thread_cpu"] = cpu;
            }
        }
        engine_.init_stream(account, scope, universe, allow_orders);
        backend_ = std::dynamic_pointer_cast<oms::AtpBackend>(engine_.make_oms_backend(0, scope));
    }
    ~StreamAtpSession() override { stop(); }
    std::shared_ptr<oms::Backend> backend() const override { return backend_; }
    void attach(const std::shared_ptr<oms::Engine>& engine) override { engine_.attach_stream_oms(engine); }
    bool connect(long timeout, std::string* error) override {
        engine_.connect(timeout); engine_.login(timeout);
        if (!engine_.is_logged_in()) { if (error) *error = "ATP login failed or timed out"; return false; }
        backend_->connected(); return true;
    }
    void stop() override { if (backend_) backend_->close(); engine_.release_api(); }
    std::string status() const override { return engine_.stream_status(); }
private:
    TDEngineGXBSE engine_;
    std::shared_ptr<oms::AtpBackend> backend_;
};
}

extern "C" __attribute__((visibility("default"))) strategy_runtime::LiveTdSession*
usagi_create_live_td_v1(const char* path, const oms::Scope* scope,
        const std::set<oms::Instrument>* universe, bool allow_orders, char* error, std::size_t size) {
    try {
        if (!path || !scope || !universe) throw std::runtime_error("missing live TD arguments");
        return new StreamAtpSession(path, *scope, *universe, allow_orders);
    } catch (const std::exception& e) {
        if (error && size) { std::strncpy(error, e.what(), size - 1); error[size - 1] = 0; }
        return 0;
    }
}
extern "C" __attribute__((visibility("default"))) void
usagi_destroy_live_td_v1(strategy_runtime::LiveTdSession* session) { delete session; }
