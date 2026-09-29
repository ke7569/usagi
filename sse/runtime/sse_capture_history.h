#ifndef SSE_CAPTURE_HISTORY_H
#define SSE_CAPTURE_HISTORY_H
#include "sse/runtime/sse_journal_transport.h"
#include <map>
namespace sse_journal {
// A boundary is proved by the identical last datagram of every subscription.
// Each capture has its own event IDs and socket arrival interleaving.
class OverlapFilter {
    struct Mark {
        std::uint64_t event, hardware;
        std::uint32_t crc;
        std::size_t size;
        bool seen;
    };
    std::map<std::uint32_t, Mark> marks_;
    bool new_packet_;
public:
    OverlapFilter() : new_packet_(false) {}
    void configure(const nlohmann::json& proofs,std::size_t channels) {
        if(!proofs.is_array() || proofs.size()!=channels)
            throw std::runtime_error("capture overlap must prove every subscription");
        marks_.clear();new_packet_=false;
        for(const auto& p:proofs) {
            const auto channel=p.at("channel").get<std::uint32_t>();
            Mark m={p.at("event_id").get<std::uint64_t>(),p.at("hardware_ns").get<std::uint64_t>(),
                p.at("payload_crc").get<std::uint32_t>(),p.at("payload_bytes").get<std::size_t>(),false};
            if(channel>=channels || !m.event || !m.size || m.size>kMaxDatagram || !marks_.insert(std::make_pair(channel,m)).second)
                throw std::runtime_error("invalid capture overlap watermark");
        }
    }
    bool accept(const deepwin_market_data::StreamEvent& e,std::uint64_t id) {
        if(marks_.empty())return true;
        if(e.kind==deepwin_market_data::kIdleEvent)return new_packet_;
        auto it=marks_.find(e.channel_id);
        if(it==marks_.end())throw std::runtime_error("capture overlap unknown subscription");
        Mark& m=it->second;
        if(id<m.event)return false;
        if(id==m.event) {
            if(e.hardware_ns!=m.hardware || e.size!=m.size || sze_recovery::crc32(e.data,e.size)!=m.crc)
                throw std::runtime_error("capture overlap boundary datagram mismatch");
            m.seen=true;return false;
        }
        if(!m.seen)throw std::runtime_error("capture overlap watermark was not observed");
        new_packet_=true;return true;
    }
    void complete()const {
        for(const auto& m:marks_)if(!m.second.seen)
            throw std::runtime_error("capture overlap prefix incomplete");
    }
};
}
#endif
