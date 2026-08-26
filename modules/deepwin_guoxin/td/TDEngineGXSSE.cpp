/* Shanghai ATP Quant API TD entry point.
 *
 * The implementation is shared with the validated GXBSE adapter.  These
 * aliases are applied before the implementation header is parsed, giving the
 * SSE shared object an independent plugin key, source id, and direct API ABI.
 */
#define T0_TD_PLUGIN_LABEL "sse_td"
#define T0_TD_SOURCE_ID 190
#define T0_TD_ENGINE_KEY "sse_td"
#define T0_TD_DEFAULT_MARKET_ID 101

#define GxbseDirectOrderRequest GxsseDirectOrderRequest
#define GxbseDirectOrderResult GxsseDirectOrderResult
#define GxbseDirectCancelRequest GxsseDirectCancelRequest
#define GxbseDirectCancelResult GxsseDirectCancelResult
#define GxbseDirectOrderFn GxsseDirectOrderFn
#define GxbseDirectCancelFn GxsseDirectCancelFn
#define gxbse_direct_cash_order gxsse_direct_cash_order
#define gxbse_direct_cancel_order gxsse_direct_cancel_order

#include "TDEngineGXBSE.cpp"
