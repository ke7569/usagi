#include "common/oms/Profile.h"

#ifdef USAGI_OMS_PROFILE
#include <chrono>

namespace oms {
namespace profile {
namespace {
thread_local Sample* active_sample = 0;
thread_local Scope* active_scope = 0;
thread_local Mask active_mask = kAllStages;

std::uint64_t timestamp() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
}  // namespace

Session::Session(Sample& sample, Mask mask)
    : previous_sample_(active_sample), previous_scope_(active_scope), previous_mask_(active_mask) {
    sample = Sample(); active_sample = &sample; active_scope = 0; active_mask = mask;
}
Session::~Session() {
    active_sample = previous_sample_; active_scope = previous_scope_; active_mask = previous_mask_;
}

Scope::Scope(Stage stage)
    : sample_(active_sample), parent_(active_scope), stage_(static_cast<std::size_t>(stage)),
      begin_(0), children_(0) {
    if (!(active_mask & (Mask(1) << stage_))) sample_ = 0;
    if (sample_) { begin_ = timestamp(); active_scope = this; }
}
void Scope::stop() {
    if (!sample_) return;
    const std::uint64_t elapsed = timestamp() - begin_;
    sample_->inclusive_ns[stage_] += elapsed;
    sample_->exclusive_ns[stage_] += elapsed - children_;
    ++sample_->calls[stage_];
    if (parent_) parent_->children_ += elapsed;
    active_scope = parent_; sample_ = 0;
}

const char* stage_name(Stage stage) {
    static const char* const names[] = {"submit", "gate", "account_lock", "risk", "order_construction",
        "intent_audit", "audit_format", "journal_append", "journal_sync", "reservation", "registration",
        "dispatch", "dispatcher_lock", "dispatch_audit", "timer", "backend_call", "send_result",
        "send_audit", "audit_note", "final_lookup"};
    static_assert(sizeof(names) / sizeof(names[0]) == kStages, "profile stage names incomplete");
    return names[static_cast<std::size_t>(stage)];
}
}  // namespace profile
}  // namespace oms
#endif
