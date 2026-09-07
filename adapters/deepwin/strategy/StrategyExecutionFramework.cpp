#include "adapters/deepwin/strategy/StrategyExecutionFramework.h"

#include "common/contracts/StrategyExecution.h"
#include "adapters/deepwin/legacy/util.h"

#include <sstream>

namespace {

class FrameworkStrategyExecution
    : public StrategyExecution,
      public std::enable_shared_from_this<FrameworkStrategyExecution> {
public:
    FrameworkStrategyExecution(WCStrategyUtilPtr util, KfLogPtr logger)
        : util_(util), logger_(logger) {}

    bool permits_new_orders() const { return false; }

    long long now_ns() const {
        return util_ == 0 ? 0LL : util_->get_nano();
    }

    int submit_limit(short, const std::string&, const std::string&, double, int, char, char) { return -1; }
    int cancel(short, int) { return -1; }
    bool schedule_cancel(short, int, int) { return false; }

    void log(const char* level, const std::string& message) {
        if (logger_ == 0) return;
        const std::string line = std::string(level == 0 ? "" : level) +
                                 (level == 0 ? "" : " ") + message;
#ifdef T0_USE_DEEPWIN
        if (std::string(level == 0 ? "" : level) == "ERROR")
            KF_LOG_ERROR(logger_, line);
        else
            KF_LOG_INFO(logger_, line);
#else
        (void)line;
#endif
    }

private:
    WCStrategyUtilPtr util_;
    KfLogPtr logger_;
};

}  // namespace

std::shared_ptr<StrategyExecution> make_strategy_execution(
    WCStrategyUtilPtr util, KfLogPtr logger) {
    return std::shared_ptr<StrategyExecution>(
        new FrameworkStrategyExecution(util, logger));
}
