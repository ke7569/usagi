#ifndef GXBSE_DIRECT_API_H
#define GXBSE_DIRECT_API_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct GxbseDirectOrderRequest {
    int source_id;
    int account_index;
    int request_id;
    long order_ref;
    const char* instrument_id;
    double price;
    int volume;
    char direction;
    char offset_flag;
    char order_price_type;
    char time_condition;
    char volume_condition;
};

struct GxbseDirectOrderResult {
    int ret;
    int64_t atp_request_id;
    uint64_t func_enter_ns;
    uint64_t msg_new_ns;
    uint64_t msg_fill_ns;
    uint64_t route_init_ns;
    uint64_t route_store_before_api_ns;
    uint64_t api_send_ns;
    uint64_t msg_delete_ns;
    uint64_t pre_log_total_ns;
};

struct GxbseDirectCancelRequest {
    int source_id;
    int account_index;
    int request_id;
    int order_request_id;
    long order_ref;
};

struct GxbseDirectCancelResult {
    int ret;
    int64_t atp_request_id;
    uint64_t total_ns;
    uint64_t api_send_ns;
};

typedef int (*GxbseDirectOrderFn)(const GxbseDirectOrderRequest* request,
                                  GxbseDirectOrderResult* result);
typedef int (*GxbseDirectCancelFn)(const GxbseDirectCancelRequest* request,
                                   GxbseDirectCancelResult* result);

#ifdef __cplusplus
}
#endif

#endif
