#ifndef USAGI_ORDER_LATENCY_LOG_H
#define USAGI_ORDER_LATENCY_LOG_H
#include "common/oms/OrderLatency.h"
#include "common/oms/AsyncDiagnosticLog.h"
#include "common/oms/Types.h"
#include "third_party/nlohmann/json.hpp"
#include <fstream>
#include <mutex>
#include <cstdlib>
#include <cstdio>
#include <stdexcept>
namespace order_latency {
inline diagnostic_log::Sink& sink() {
    static diagnostic_log::Sink value("SSE_ORDER_LATENCY_PATH"); return value;
}
inline void record(const oms::Command& command,const oms::SendResult& result,
                   bool called,std::uint64_t begin,std::uint64_t end,
                   const SendStages& stages=SendStages()) noexcept {
    if(command.cancel)return;
    Timing t;if(!parse(command.intent.signal_id,&t))return;
    try {
        diagnostic_log::Sink& out=sink();if(!out.enabled())return;
        // Keep JSON tree construction off the strategy owner too, not only IO.
        out.enqueue([command,result,called,begin,end,stages,t]() {
        const bool valid=t.strategy<=begin && begin<=end;
        nlohmann::json row={{"event","order_send_latency"},{"request_id",command.id},
            {"epoch",command.scope.epoch},{"instrument",command.intent.instrument.code},
            {"signal_id",command.intent.signal_id},{"quantity",command.intent.quantity},
            {"side",command.intent.side==oms::Side::Buy?"buy":"sell"},
            {"clock","CLOCK_MONOTONIC"},{"endpoint","td_backend_submit_return"},
            {"backend_called",called},{"timing_valid",valid},
            {"send_disposition",result.disposition==oms::SendDisposition::Submitted?"submitted":
                result.disposition==oms::SendDisposition::Unknown?"unknown":"not_sent"},
            {"error_category",static_cast<int>(result.error.category)},{"raw_code",result.error.raw_code},
            {"receive_ns",t.receive},{"signal_ns",t.signal},{"strategy_ns",t.strategy},
            {"td_begin_ns",begin},{"td_return_ns",end}};
        if(valid) {
            row["receive_to_signal_us"]=(t.signal-t.receive)/1000.0;
            row["signal_queue_us"]=(t.strategy-t.signal)/1000.0;
            row["strategy_and_oms_us"]=(begin-t.strategy)/1000.0;
            row["td_submit_us"]=(end-begin)/1000.0;
            row["signal_to_send_us"]=(end-t.signal)/1000.0;
            row["receive_to_send_us"]=(end-t.receive)/1000.0;
        }
        const bool stages_valid=valid && stages.enter>=t.strategy &&
            stages.enter<=stages.gate_done && stages.gate_done<=stages.lock_acquired &&
            stages.lock_acquired<=stages.intent_begin && stages.intent_begin<=stages.intent_durable &&
            stages.intent_durable<=stages.registered && stages.registered<=begin;
        row["oms_stages_valid"]=stages_valid;
        row["intent_waited_for_durability"]=stages.intent_durable_wait;
        if(stages_valid) {
            row["oms_stages_us"]={{"strategy_before_oms",(stages.enter-t.strategy)/1000.0},
                {"external_gate",(stages.gate_done-stages.enter)/1000.0},
                {"account_lock_wait",(stages.lock_acquired-stages.gate_done)/1000.0},
                {"risk_and_construction",(stages.intent_begin-stages.lock_acquired)/1000.0},
                {stages.intent_durable_wait?"intent_encode_and_durable_wait":"intent_encode_and_enqueue",(stages.intent_durable-stages.intent_begin)/1000.0},
                {"registration",(stages.registered-stages.intent_durable)/1000.0},
                {"registration_to_backend",(begin-stages.registered)/1000.0}};
        }
        // Timestamp before formatting/writing; never let a log failure change
        // a possibly submitted order into an unknown/retried send.
        return row;
        });
    }catch(...){std::fputs("order_send_latency: log write failed\n",stderr);}
}
}
#endif
