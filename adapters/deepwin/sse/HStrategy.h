#ifndef SSE_T0_HSTRATEGY_H
#define SSE_T0_HSTRATEGY_H

// Shanghai starts with the proven ZStrategy lifecycle. The named HStrategy
// entry point makes the exchange boundary explicit while the implementation
// remains shared until SSE-specific order rules are validated.
#include "common/strategy/ZStrategy.h"
typedef ZStrategy HStrategy;

#endif
