#ifndef TDENGINEGXSSE_H
#define TDENGINEGXSSE_H

/*
 * This header is intentionally a thin type-level facade.  The ATP Quant API
 * order, cancel, query, recovery, and callback state machine is exchange
 * neutral; the SSE target selects its own plugin key/source/market defaults at
 * compile time in TDEngineGXSSE.cpp.  Keeping one implementation prevents the
 * two gateways from drifting in request/reply correlation and latency paths.
 */
#include "TDEngineGXBSE.h"
#include "GxsseDirectApi.h"

WC_NAMESPACE_START

using AccountUnitGXSSE = AccountUnitGXBSE;
using TDEngineGXSSE = TDEngineGXBSE;

WC_NAMESPACE_END

#endif
