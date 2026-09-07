#ifndef USAGI_OMS_PROFILE_H
#define USAGI_OMS_PROFILE_H

// Diagnostic target only; normal targets compile every probe out.
#ifdef USAGI_OMS_PROFILE
#include <array>
#include <cstddef>
#include <cstdint>

namespace oms {
namespace profile {

enum class Stage : std::size_t {
    Submit, Gate, AccountLock, Risk, OrderConstruction, IntentAudit,
    AuditFormat, JournalAppend, JournalSync, Reservation, Registration,
    Dispatch, DispatcherLock, DispatchAudit, Timer, BackendCall,
    SendResult, SendAudit, AuditNote, FinalLookup, Count
};
static const std::size_t kStages = static_cast<std::size_t>(Stage::Count);
typedef std::uint64_t Mask;
static_assert(kStages < 64, "profile mask capacity exceeded");
static const Mask kAllStages = (Mask(1) << kStages) - 1;
inline Mask only(Stage stage) { return Mask(1) << static_cast<std::size_t>(stage); }

struct Sample {
    std::array<std::uint64_t, kStages> inclusive_ns;
    std::array<std::uint64_t, kStages> exclusive_ns;
    std::array<std::uint64_t, kStages> calls;
    Sample() : inclusive_ns(), exclusive_ns(), calls() {}
};

class Scope;
class Session {
public:
    explicit Session(Sample& sample, Mask mask = kAllStages);
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
private:
    Sample* previous_sample_;
    Scope* previous_scope_;
    Mask previous_mask_;
};

class Scope {
public:
    explicit Scope(Stage stage);
    ~Scope() { stop(); }
    void stop();
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
private:
    Sample* sample_;
    Scope* parent_;
    std::size_t stage_;
    std::uint64_t begin_, children_;
};

const char* stage_name(Stage stage);

}  // namespace profile
}  // namespace oms

#define OMS_PROFILE_SCOPE(name, stage) ::oms::profile::Scope name(::oms::profile::Stage::stage)
#define OMS_PROFILE_STOP(name) name.stop()
#else
#define OMS_PROFILE_SCOPE(name, stage)
#define OMS_PROFILE_STOP(name)
#endif
#endif
