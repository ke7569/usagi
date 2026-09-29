#ifndef SSE_V06_AUDIT_H
#define SSE_V06_AUDIT_H
#include "sse/model/v06_model.h"
#include "third_party/nlohmann/json.hpp"
#include <atomic>
#include <thread>
#include <memory>
#include <fstream>
#include <iostream>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <sched.h>
#include <unistd.h>
namespace sse_v06 {
// Serialized application producer; dedicated writer owns JSON formatting and IO.
struct AuditRecord {
    enum Kind {Prediction,Decision,Text,Flush} kind;
    char code[7]; Heads heads;
    std::uint64_t time,stream,wire;
    long long position,reserved,requested,allowed;
    int consensus,permission;bool selected,buy,quote;
    std::string text;
    AuditRecord():kind(Flush),code(),heads(),time(0),stream(0),wire(0),position(0),reserved(0),requested(0),allowed(0),consensus(0),permission(0),selected(false),buy(false),quote(false){}
};
class AuditSink {
    static const std::size_t Capacity=65536, Formatters=4;
    struct Slot {AuditRecord record;std::string encoded;std::atomic<std::uint64_t> ready;Slot():ready(0){}};
    std::ofstream file_;std::ostream*output_;
    std::unique_ptr<Slot[]> queue_;
    alignas(64) std::atomic<std::uint64_t> head_;
    alignas(64) std::atomic<std::uint64_t> tail_;
    alignas(64) std::atomic<bool>stop_;
    alignas(64) std::atomic<bool>failed_;
    alignas(64) std::atomic<unsigned>started_;
    alignas(64) std::thread writer_;std::thread formatters_[Formatters];bool sync_;int base_cpu_;
    static std::string render(const AuditRecord&r){
        using J=nlohmann::json;
        if(r.kind==AuditRecord::Flush)return std::string();
        if(r.kind==AuditRecord::Text)return r.text;
        if(r.kind==AuditRecord::Prediction)return J{{"event","v06_prediction"},{"model_version","v0.6"},{"instrument",r.code},{"time_us",r.time},{"stream_sequence",r.stream},{"wire_sequence",r.wire},{"heads",r.heads},{"agreement",r.consensus},{"selected",r.selected},{"units","permille"}}.dump();
        return J{{"event","v06_decision"},{"instrument",r.code},{"time_us",r.time},{"heads",r.heads},{"agreement",r.consensus},{"opening_permission",r.permission},{"position",r.position},{"reserved",r.reserved},{"requested",r.requested},{"allowed",r.allowed},{"side",r.buy?"buy":"sell"},{"quote",r.quote}}.dump();
    }
    void bind(unsigned index){if(base_cpu_<0)return;int cpu=base_cpu_+index;cpu_set_t s;CPU_ZERO(&s);if(cpu>=CPU_SETSIZE)throw std::runtime_error("audit CPU range");CPU_SET(cpu,&s);if(sched_setaffinity(0,sizeof(s),&s))throw std::runtime_error("audit affinity");}
    void failure()noexcept{failed_.store(true,std::memory_order_release);std::fputs("v06 audit worker failed\n",stderr);}
    void format(unsigned index)noexcept{try{bind(index);started_.fetch_add(1,std::memory_order_release);for(std::uint64_t n=index;!failed_.load(std::memory_order_acquire);n+=Formatters){while(n>=head_.load(std::memory_order_acquire)){if(failed_.load(std::memory_order_acquire))return;if(stop_.load(std::memory_order_acquire)&&n>=head_.load(std::memory_order_acquire))return;__builtin_ia32_pause();}Slot&slot=queue_[n%Capacity];slot.encoded=render(slot.record);slot.ready.store(n+1,std::memory_order_release);}}catch(...){failure();}}
    void write()noexcept{try{bind(Formatters);started_.fetch_add(1,std::memory_order_release);for(std::uint64_t n=0;!failed_.load(std::memory_order_acquire);++n){while(n>=head_.load(std::memory_order_acquire)){if(stop_.load(std::memory_order_acquire)&&n>=head_.load(std::memory_order_acquire)){output_->flush();return;}if(failed_.load(std::memory_order_acquire))return;__builtin_ia32_pause();}Slot&slot=queue_[n%Capacity];while(slot.ready.load(std::memory_order_acquire)!=n+1){if(failed_.load(std::memory_order_acquire))return;__builtin_ia32_pause();}if(slot.record.kind==AuditRecord::Flush)output_->flush();else *output_<<slot.encoded<<'\n';tail_.store(n+1,std::memory_order_release);}}catch(...){failure();}}
    void join(){for(auto&t:formatters_)if(t.joinable())t.join();if(writer_.joinable())writer_.join();}
public:
    AuditSink():output_(&std::cerr),queue_(new Slot[Capacity]),head_(0),tail_(0),stop_(false),failed_(false),started_(0),sync_(std::getenv("SSE_AUDIT_SYNC")!=0),base_cpu_(-1){
        const char*c=std::getenv("SSE_AUDIT_CPU");if(c&&*c){char*end=0;long cpu=std::strtol(c,&end,10);if(*end||cpu<0||cpu+Formatters>=CPU_SETSIZE)throw std::runtime_error("audit CPU range");base_cpu_=cpu;}
        const char*p=std::getenv("SSE_V06_AUDIT_PATH");if(p&&*p){if(p[0]!='/')throw std::runtime_error("v06 audit path must be absolute");file_.exceptions(std::ios::badbit|std::ios::failbit);file_.open(p,std::ios::out|std::ios::app);output_=&file_;}
        if(!sync_){try{for(unsigned i=0;i<Formatters;++i)formatters_[i]=std::thread(&AuditSink::format,this,i);writer_=std::thread(&AuditSink::write,this);while(started_.load(std::memory_order_acquire)!=Formatters+1&&!failed_.load(std::memory_order_acquire))__builtin_ia32_pause();if(failed_.load())throw std::runtime_error("v06 audit startup failed");}catch(...){stop_.store(true);failed_.store(true);join();throw;}}
    }
    ~AuditSink(){stop_.store(true,std::memory_order_release);join();}
    void push(AuditRecord&&r){if(failed_.load(std::memory_order_acquire))throw std::runtime_error("v06 audit failed");if(sync_){if(r.kind==AuditRecord::Flush)output_->flush();else *output_<<render(r)<<'\n';return;}auto h=head_.load(std::memory_order_relaxed);if(h-tail_.load(std::memory_order_acquire)>=Capacity)throw std::runtime_error("v06 audit queue full");queue_[h%Capacity].record=std::move(r);head_.store(h+1,std::memory_order_release);}
    std::uint64_t pending()const{return head_.load(std::memory_order_acquire)-tail_.load(std::memory_order_acquire);}
};
inline AuditSink& audit_sink(){static AuditSink s;return s;}
inline void audit_code(AuditRecord&r,const std::string&code){if(code.size()!=6)throw std::runtime_error("audit security id");std::memcpy(r.code,code.data(),6);r.code[6]=0;}
inline void audit_prediction(const std::string&code,std::uint64_t time,std::uint64_t stream,std::uint64_t wire,const Heads&heads,bool selected){AuditRecord r;r.kind=AuditRecord::Prediction;audit_code(r,code);r.time=time;r.stream=stream;r.wire=wire;r.heads=heads;r.consensus=agreement(heads);r.selected=selected;audit_sink().push(std::move(r));}
inline void audit_decision(const std::string&code,std::uint64_t time,const Heads&heads,int consensus,int permission,long long position,long long reserved,long long requested,long long allowed,bool buy,bool quote){AuditRecord r;r.kind=AuditRecord::Decision;audit_code(r,code);r.time=time;r.heads=heads;r.consensus=consensus;r.permission=permission;r.position=position;r.reserved=reserved;r.requested=requested;r.allowed=allowed;r.buy=buy;r.quote=quote;audit_sink().push(std::move(r));}
inline void audit_text(nlohmann::json&&j){AuditRecord r;r.kind=AuditRecord::Text;r.text=j.dump();audit_sink().push(std::move(r));}
inline void audit_flush(){audit_sink().push(AuditRecord());}
}
#endif
