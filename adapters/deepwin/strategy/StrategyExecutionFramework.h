#ifndef T0_STRATEGY_EXECUTION_FRAMEWORK_H
#define T0_STRATEGY_EXECUTION_FRAMEWORK_H

#include <memory>
#include "adapters/deepwin/legacy/util.h"

class StrategyExecution;

std::shared_ptr<StrategyExecution> make_strategy_execution(
    WCStrategyUtilPtr util, KfLogPtr logger);

#endif
