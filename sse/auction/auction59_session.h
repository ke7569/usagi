#ifndef SSE_T0_AUCTION59_SESSION_H
#define SSE_T0_AUCTION59_SESSION_H

#include "sse/auction/auction59_engine.h"
#include "sse/auction/sse_opening_boundary.h"

#include <memory>

namespace sse_auction59 {

// One trading day and one security. Feed decoded events in receive order on
// the calculation thread; neither CSV files nor a second market-data reader
// are required. Snapshot prices are yuan and volume is shares, while tick
// prices and quantities retain the decoder's raw 1/1000 units.
class Session {
public:
    Session(const std::string& security_id, const StaticMetadata& metadata,
            bool ipo_known = false);

    void on_tick(const sse_live::TickEvent& event, std::uint64_t realtime_ns,
                 std::uint64_t arrival_index);
    void on_snapshot(const sse_live::Snapshot& snapshot);

    bool ready() const { return ready_; }
    bool failed() const { return failed_; }
    bool shared_static_valid() const { return shared_static_valid_; }
    const std::string& reason() const { return reason_; }
    const std::vector<float>& factors() const { return factors_; }

    // Irreversible for this day's session. The caller seals a pending session
    // at its first prediction snapshot, so late data cannot hot-start a model.
    void disable(const std::string& reason);
    void seal_missing();

private:
    void try_ready();
    void fail_static(const std::string& reason);

    std::string security_id_;
    StaticMetadata metadata_;
    std::unique_ptr<AuctionAccumulator> accumulator_;
    sse_live::OpeningBoundaryTracker boundary_tracker_;
    sse_live::OpeningBoundary boundary_;
    sse_live::Snapshot snapshot_;
    bool have_snapshot_;
    bool have_boundary_;
    bool ready_;
    bool failed_;
    bool shared_static_valid_;
    std::string reason_;
    std::vector<float> factors_;

    Session(const Session&);
    Session& operator=(const Session&);
};

}  // namespace sse_auction59

#endif  // SSE_T0_AUCTION59_SESSION_H
