#ifndef USAGI_ORDER_REPORT_LOG_H
#define USAGI_ORDER_REPORT_LOG_H

#include "common/oms/Types.h"
#include "common/oms/AsyncDiagnosticLog.h"
#include "third_party/nlohmann/json.hpp"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <string>
#include <time.h>

namespace order_report_log {
using Json = nlohmann::json;

inline std::uint64_t monotonic_ns() noexcept {
    timespec t = {};
    return clock_gettime(CLOCK_MONOTONIC, &t) == 0
        ? std::uint64_t(t.tv_sec) * 1000000000ULL + std::uint64_t(t.tv_nsec) : 0;
}

inline diagnostic_log::Sink& sink() {
    static diagnostic_log::Sink value("SSE_ORDER_REPORT_PATH"); return value;
}
inline void write(Json row) noexcept { sink().push(std::move(row)); }

inline void atp_received(int account_index, long cl_ord_no, long batch_cl_ord_no,
                         long orig_cl_ord_no, int ord_sign, int market_id,
                         const char* security, int side, int ord_status,
                         double order_qty, double leaves_qty, double cum_qty,
                         double last_qty, const char* exec_id, long route_cl_ord_no,
                         bool cancel_object, bool route_found, bool stream_mode) noexcept {
    if(!sink().enabled())return;
    try {
    write(Json{{"event", "atp_order_report_received"}, {"source", "atp"},
        {"callback_ns", monotonic_ns()}, {"account_index", account_index},
        {"cl_ord_no", cl_ord_no}, {"batch_cl_ord_no", batch_cl_ord_no},
        {"orig_cl_ord_no", orig_cl_ord_no}, {"ord_sign", ord_sign},
        {"market_id", market_id}, {"security_id", security ? security : ""},
        {"side", side}, {"ord_status", ord_status}, {"order_qty", order_qty},
        {"leaves_qty", leaves_qty}, {"cum_qty", cum_qty}, {"last_qty", last_qty},
        {"exec_id", exec_id ? exec_id : ""}, {"route_cl_ord_no", route_cl_ord_no},
        {"cancel_object", cancel_object}, {"route_found", route_found},
        {"stream_mode", stream_mode}});
    } catch(...) { std::fputs("order_report_log: record construction failed\n",stderr); }
}

inline Json report_fields(const oms::Report& report) {
    return Json{{"id", report.id}, {"broker_id", report.broker_id},
        {"market", report.instrument.market}, {"instrument", report.instrument.code},
        {"side", static_cast<int>(report.side)}, {"kind", static_cast<int>(report.kind)},
        {"state", static_cast<int>(report.state)}, {"original", report.original},
        {"cumulative", report.cumulative}, {"leaves", report.leaves},
        {"trade_id", report.trade_id}, {"trade_quantity", report.trade_quantity},
        {"cumulative_after", report.cumulative_after}, {"trade_price", report.trade_price},
        {"trade_fee", report.trade_fee}, {"fee_is_final", report.fee_is_final},
        {"error_category", static_cast<int>(report.error.category)},
        {"error_raw_code", report.error.raw_code}, {"error_message", report.error.message}};
}

inline void oms_received(const oms::Report& report, oms::Time oms_time_ns,
                         std::uint64_t entry_ns) noexcept {
    if(!sink().enabled())return;
    try {
        sink().enqueue([report, oms_time_ns, entry_ns]() {
            Json row=report_fields(report);
            row["event"]="oms_report_received";row["source"]="oms";
            row["entry_ns"]=entry_ns;row["oms_time_ns"]=oms_time_ns;
            row["scope_epoch"]=report.scope.epoch;return row;
        });
    } catch(...) { std::fputs("order_report_log: snapshot failed\n",stderr); }
}

inline void oms_result(const oms::Report& report, oms::Time oms_time_ns,
                       std::uint64_t entry_ns, bool applied,
                       const char* outcome, const std::string& detail = std::string(),
                       std::uint64_t done_ns = 0) noexcept {
    if(!sink().enabled())return;
    try {
        const std::uint64_t done=done_ns?done_ns:monotonic_ns();
        const std::string outcome_text=outcome?outcome:"";
        const std::string detail_text=detail.substr(0,256);
        sink().enqueue([report,oms_time_ns,entry_ns,done,applied,outcome_text,detail_text](){
            Json row=report_fields(report);
            row["event"]="oms_report_result";row["source"]="oms";
            row["entry_ns"]=entry_ns;row["done_ns"]=done;row["oms_time_ns"]=oms_time_ns;
            row["duration_us"]=done>=entry_ns?(done-entry_ns)/1000.0:0.0;
            row["applied"]=applied;row["outcome"]=outcome_text;
            if(!detail_text.empty())row["detail"]=detail_text;
            row["scope_epoch"]=report.scope.epoch;return row;
        });
    } catch(...) { std::fputs("order_report_log: snapshot failed\n",stderr); }
}
}  // namespace order_report_log

#endif
