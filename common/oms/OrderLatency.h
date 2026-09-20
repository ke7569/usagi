#ifndef USAGI_ORDER_LATENCY_H
#define USAGI_ORDER_LATENCY_H
#include <cstdint>
#include <string>
#include <sstream>
#include <time.h>
namespace order_latency {
inline std::uint64_t now_ns() {
    timespec t={};
    return clock_gettime(CLOCK_MONOTONIC,&t)==0 ? std::uint64_t(t.tv_sec)*1000000000ULL+t.tv_nsec : 0;
}
// Internal OMS diagnostics; deliberately absent from persisted Intent/TD ABI.
struct SendStages {
    bool intent_durable_wait=true;
    std::uint64_t enter=0,gate_done=0,lock_acquired=0,intent_begin=0,intent_durable=0,registered=0;
};
struct Timing { std::uint64_t receive=0,signal=0,strategy=0; };
// Carry timestamps through the existing persisted signal identity: no change
// to the TD plugin's Intent/Command ABI. All values use this host's monotonic clock.
inline std::string identity(const std::string& code,std::uint64_t sequence,const Timing& t) {
    return "v06t:"+code+":"+std::to_string(sequence)+":"+std::to_string(t.receive)+":"+
        std::to_string(t.signal)+":"+std::to_string(t.strategy);
}
inline bool parse(const std::string& id,Timing* t) {
    if(id.compare(0,5,"v06t:")!=0)return false;
    std::istringstream stream(id);std::string part;
    for(int i=0;i<3;++i)if(!std::getline(stream,part,':'))return false;
    std::uint64_t* values[]={&t->receive,&t->signal,&t->strategy};
    try {
        for(int i=0;i<3;++i) {
            if(!std::getline(stream,part,':')||part.empty()||part.find_first_not_of("0123456789")!=std::string::npos)return false;
            *values[i]=std::stoull(part);
        }
    }catch(...){return false;}
    return !std::getline(stream,part,':') && t->receive && t->receive<=t->signal && t->signal<=t->strategy;
}
}
#endif
