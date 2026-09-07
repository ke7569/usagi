#include "common/oms/Types.h"
#include <cmath>
#include <limits>

namespace oms {

bool money_from_double(double value, Money* output) {
    if (!output || !std::isfinite(value) || value < 0) return false;
    const long double scaled = static_cast<long double>(value) * kMoneyScale;
    const long double rounded = std::floor(scaled + 0.5L);
    if (rounded > static_cast<long double>(std::numeric_limits<Money>::max()) ||
        std::fabs(scaled - rounded) > 0.0001L) return false;
    *output = static_cast<Money>(rounded);
    return true;
}

const char* state_name(OrderState value) {
    switch (value) {
    case OrderState::Queued: return "queued";
    case OrderState::Submitted: return "submitted";
    case OrderState::Unknown: return "send-unknown";
    case OrderState::Accepted: return "accepted";
    case OrderState::Partial: return "partial";
    case OrderState::CancelPending: return "cancel-pending";
    case OrderState::Canceled: return "canceled";
    case OrderState::Rejected: return "rejected";
    case OrderState::Filled: return "filled";
    case OrderState::Reconcile: return "reconciliation-required";
    }
    return "invalid";
}

const char* error_name(ErrorCategory value) {
    switch (value) {
    case ErrorCategory::None: return "none";
    case ErrorCategory::Invalid: return "invalid";
    case ErrorCategory::Ownership: return "ownership";
    case ErrorCategory::Duplicate: return "duplicate";
    case ErrorCategory::NotReady: return "not-ready";
    case ErrorCategory::Cash: return "cash";
    case ErrorCategory::Shares: return "shares";
    case ErrorCategory::Limit: return "limit";
    case ErrorCategory::SelfTrade: return "self-trade";
    case ErrorCategory::RateLimited: return "rate-limited";
    case ErrorCategory::Unsupported: return "unsupported";
    case ErrorCategory::Temporary: return "temporary";
    case ErrorCategory::MissingId: return "missing-id";
    case ErrorCategory::AlreadyFinal: return "already-final";
    case ErrorCategory::Rejected: return "rejected";
    case ErrorCategory::Unknown: return "unknown";
    case ErrorCategory::Capacity: return "capacity";
    case ErrorCategory::Persistence: return "persistence";
    case ErrorCategory::Protocol: return "protocol";
    }
    return "invalid";
}

}  // namespace oms
