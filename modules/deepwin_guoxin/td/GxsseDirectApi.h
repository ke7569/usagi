#ifndef GXSSE_DIRECT_API_H
#define GXSSE_DIRECT_API_H

#include "GxbseDirectApi.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Keep a separate public name while sharing the exact validated layout. */
typedef GxbseDirectOrderRequest GxsseDirectOrderRequest;
typedef GxbseDirectOrderResult GxsseDirectOrderResult;
typedef GxbseDirectCancelRequest GxsseDirectCancelRequest;
typedef GxbseDirectCancelResult GxsseDirectCancelResult;

typedef int (*GxsseDirectOrderFn)(const GxsseDirectOrderRequest* request,
                                  GxsseDirectOrderResult* result);
typedef int (*GxsseDirectCancelFn)(const GxsseDirectCancelRequest* request,
                                   GxsseDirectCancelResult* result);

int gxsse_direct_cash_order(const GxsseDirectOrderRequest* request,
                            GxsseDirectOrderResult* result);
int gxsse_direct_cancel_order(const GxsseDirectCancelRequest* request,
                              GxsseDirectCancelResult* result);

#ifdef __cplusplus
}
#endif

#endif
