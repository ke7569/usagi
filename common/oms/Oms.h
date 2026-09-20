#ifndef USAGI_ACCOUNT_OMS_H
#define USAGI_ACCOUNT_OMS_H

#include "common/oms/Backend.h"
#include <memory>

namespace oms {

class Engine : public std::enable_shared_from_this<Engine> {
public:
    static std::shared_ptr<Engine> create(const Config& config,
                                          const std::shared_ptr<Backend>& backend);
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    Scope scope() const;
    Capabilities capabilities() const;
    bool start_epoch(std::uint64_t epoch, bool connected = true);
    bool set_connected(const Scope& scope, bool connected);
    bool begin_reconcile(std::uint64_t token, bool request_backend = true);
    bool complete_snapshot(const Snapshot& snapshot);
    bool report(const Report& report);

    SubmitResult submit(const Intent& intent, const std::function<bool()>& gate = std::function<bool()>());
    Error cancel(const std::string& owner, OrderId id);
    bool schedule_cancel(const std::string& owner, OrderId id, Time delay_ns);
    void advance_to(Time now_ns);
    void begin_stop();
    void drain();

    bool owns(const std::string& owner, OrderId id, const Instrument& instrument) const;
    bool has_working_order(const Instrument& instrument) const;
    bool position(const Instrument& instrument, Position* output) const;
    bool order(OrderId id, OrderView* output) const;
    bool day_fills(const Instrument&, Quantity* quantity, Money* net_amount) const;
    // Hot-path queries avoid constructing the diagnostic account snapshot.
    bool ready() const;
    Time now_ns() const;
    AccountView account() const;
    std::vector<AuditEvent> audit_events() const;

private:
    Engine(const Config& config, const std::shared_ptr<Backend>& backend);
    struct Impl;
    std::unique_ptr<Impl> impl_;
    bool bound_ = false;
};

}  // namespace oms
#endif
