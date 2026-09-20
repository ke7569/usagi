#include "tests/sse/latency_probe.h"
struct Times {std::uint64_t factor_start,factor_end,model_start,model_end;};
thread_local Times times={};
extern "C" sse_tick::FactorRow __real__ZN8sse_tick11FactorState5buildERNS_9OrderBookEmRA10_KNS_5LevelES6_ddd(sse_tick::FactorState*,sse_tick::OrderBook&,std::uint64_t,const sse_tick::Level(&)[10],const sse_tick::Level(&)[10],double,double,double);
extern "C" sse_tick::FactorRow __wrap__ZN8sse_tick11FactorState5buildERNS_9OrderBookEmRA10_KNS_5LevelES6_ddd(sse_tick::FactorState*self,sse_tick::OrderBook&book,std::uint64_t now,const sse_tick::Level(&bid)[10],const sse_tick::Level(&ask)[10],double a,double b,double c){
 const bool measured=trace_active.load(std::memory_order_acquire);if(measured)times.factor_start=trace_ns();
 auto row=__real__ZN8sse_tick11FactorState5buildERNS_9OrderBookEmRA10_KNS_5LevelES6_ddd(self,book,now,bid,ask,a,b,c);
 if(measured)times.factor_end=trace_ns();return row;
}
extern "C" bool __real__ZNK7sse_v065Model7predictERKSt5arrayIfLm50EEPNS_5StateEPS1_IfLm4EEPNS_6StagesE(const sse_v06::Model*,const sse_v06::Factors&,sse_v06::State*,sse_v06::Heads*,sse_v06::Stages*);
extern "C" bool __wrap__ZNK7sse_v065Model7predictERKSt5arrayIfLm50EEPNS_5StateEPS1_IfLm4EEPNS_6StagesE(const sse_v06::Model*self,const sse_v06::Factors&f,sse_v06::State*s,sse_v06::Heads*h,sse_v06::Stages*st){
 const bool measured=trace_active.load(std::memory_order_acquire);if(measured)times.model_start=trace_ns();
 const bool ok=__real__ZNK7sse_v065Model7predictERKSt5arrayIfLm50EEPNS_5StateEPS1_IfLm4EEPNS_6StagesE(self,f,s,h,st);
 if(measured)times.model_end=trace_ns();return ok;
}
void trace_wrapped_output(const sse_stream::Output&o){if(o.kind==sse_stream::kTickOutput&&o.tick.prediction_valid)trace_compute(o,times.factor_start,times.factor_start,times.factor_end,times.model_start,times.model_end);}


void trace_capture_factor(unsigned long*p){p[0]=times.factor_start;p[1]=times.factor_end;}
void trace_restore_factor(const unsigned long*p){times.factor_start=p[0];times.factor_end=p[1];}
