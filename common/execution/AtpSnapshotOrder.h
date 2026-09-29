#ifndef USAGI_ATP_SNAPSHOT_ORDER_H
#define USAGI_ATP_SNAPSHOT_ORDER_H
#include "common/oms/Types.h"

namespace strategy_runtime {
// ATP order-query LeavesQty includes canceled shares (observed 2026-09-14:
// Canceled, OrderQty=200, CumQty=0, LeavesQty=200, CanceledQty=200).
// OMS working means shares still eligible for execution. Validate raw
// quantities first; only an explicit terminal status can force working to 0.
inline bool normalize_atp_snapshot_order(oms::SnapshotOrder* row,
                                          oms::Quantity canceled) {
    if(!row || row->original<=0 || row->filled<0 || row->filled>row->original ||
       row->working<0 || row->working>row->original-row->filled ||
       canceled<0 || canceled>row->original-row->filled) return false;
    if(row->state==oms::OrderState::Filled) {
        if(row->filled!=row->original || canceled) return false;
        row->working=0;
    } else if(row->state==oms::OrderState::Canceled || row->state==oms::OrderState::Rejected) {
        row->working=0;
    } else if(canceled || row->working!=row->original-row->filled) {
        return false;
    }
    return true;
}
}
#endif
