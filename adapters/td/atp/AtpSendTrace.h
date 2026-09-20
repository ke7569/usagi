#ifndef SSE_ATP_SEND_TRACE_H
#define SSE_ATP_SEND_TRACE_H
#include "common/oms/OrderLatency.h"
#include "third_party/nlohmann/json.hpp"
#include <atomic>
#include <thread>
#include <fstream>
#include <cstdlib>
#include <sched.h>
#include <unistd.h>
namespace atp_send_trace {
struct Record {std::uint64_t epoch,id,begin,route,allocated,fields,returned,deleted;int before_cpu,after_cpu,status;bool cancel;Record():epoch(0),id(0),begin(0),route(0),allocated(0),fields(0),returned(0),deleted(0),before_cpu(-1),after_cpu(-1),status(0),cancel(false){}};
class Sink {
    Record records_[4096];
    alignas(64) std::atomic<std::uint64_t>head_;
    alignas(64) std::atomic<std::uint64_t>tail_;
    std::atomic<bool>stop_;std::thread worker_;std::ofstream file_;bool enabled_;
    void run(){try{const char*c=std::getenv("SSE_ATP_TRACE_CPU");if(c){int cpu=std::atoi(c);cpu_set_t s;CPU_ZERO(&s);if(cpu<0||cpu>=CPU_SETSIZE)throw std::runtime_error("ATP trace CPU");CPU_SET(cpu,&s);if(sched_setaffinity(0,sizeof(s),&s))throw std::runtime_error("ATP trace affinity");}for(;;){auto t=tail_.load(std::memory_order_relaxed);if(t==head_.load(std::memory_order_acquire)){if(stop_.load()&&t>=head_.load(std::memory_order_acquire))break;usleep(1000);continue;}const auto&r=records_[t%4096];using J=nlohmann::json;file_<<J{{"event","atp_send_stages"},{"epoch",r.epoch},{"id",r.id},{"cancel",r.cancel},{"clock","CLOCK_MONOTONIC"},{"begin_ns",r.begin},{"sdk_begin_ns",r.fields},{"sdk_return_ns",r.returned},{"cpu_before",r.before_cpu},{"cpu_after",r.after_cpu},{"status",r.status},{"route_us",(r.route-r.begin)/1000.0},{"allocate_us",(r.allocated-r.route)/1000.0},{"fields_us",(r.fields-r.allocated)/1000.0},{"sdk_us",(r.returned-r.fields)/1000.0},{"delete_us",(r.deleted-r.returned)/1000.0}}.dump()<<'\n';file_.flush();tail_.store(t+1,std::memory_order_release);}}catch(...){std::fputs("ATP send trace writer failed\n",stderr);}}
public:
    Sink():head_(0),tail_(0),stop_(false),enabled_(false){const char*p=std::getenv("SSE_ATP_SEND_TIMING_PATH");if(p&&*p){if(*p!='/')throw std::runtime_error("ATP trace absolute path required");file_.exceptions(std::ios::failbit|std::ios::badbit);file_.open(p,std::ios::out|std::ios::app);enabled_=true;worker_=std::thread(&Sink::run,this);}}
    ~Sink(){stop_.store(true);if(worker_.joinable())worker_.join();}
    void push(const Record&r)noexcept{if(!enabled_)return;auto h=head_.load(std::memory_order_relaxed);if(h-tail_.load(std::memory_order_acquire)>=4096){std::fputs("ATP send trace overflow\n",stderr);return;}records_[h%4096]=r;head_.store(h+1,std::memory_order_release);}
};
inline Sink& sink(){static Sink s;return s;}
}
#endif
