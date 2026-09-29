#define SSE_OWNER_TEST_PROBE 1
namespace sse_stream {struct Output;}
namespace sse_live {struct RawTickEvent;}
void sse_owner_test_tick(const sse_live::RawTickEvent&);
void sse_owner_test_full();
void sse_owner_test_output(const sse_stream::Output&);
#include "sse/runtime/sse_parallel_processor.h"
#include "tests/oms/TestExecution.h"
#include <cassert>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <thread>

std::atomic<unsigned> blocked_code(0);
std::atomic<bool> output_blocked(false), release_output(false);
void sse_owner_test_output(const sse_stream::Output& out) {
    if(out.kind!=sse_stream::kTickOutput ||
       std::strtoul(out.tick.event.security_id.c_str(),0,10)!=blocked_code.load())return;
    output_blocked.store(true,std::memory_order_release);
    while(!release_output.load(std::memory_order_acquire))std::this_thread::yield();
}
std::atomic<bool> hold_ticks(false),ticks_blocked(false),tick_queue_full(false),release_ticks(false);
void sse_owner_test_tick(const sse_live::RawTickEvent&) {
    if(!hold_ticks.load())return;
    ticks_blocked.store(true,std::memory_order_release);
    while(!release_ticks.load(std::memory_order_acquire))std::this_thread::yield();
}
void sse_owner_test_full(){if(hold_ticks.load())tick_queue_full.store(true,std::memory_order_release);}
using Bytes=std::vector<unsigned char>;
using deepwin_market_data::StreamEvent;
using sse_stream::Output;
namespace {
void put(Bytes& b,unsigned o,std::uint64_t n,unsigned width) {
    for(unsigned i=0;i<width;++i) b[o+i]=(n>>(8*i))&255;
}
Bytes tick(unsigned n,char type,char side,std::uint64_t buy,std::uint64_t sell,
           const char* code="600000",unsigned qty=1000000) {
    Bytes b(72); put(b,0,n,4); b[8]=0x3e; put(b,9,n,8); b[17]=7;
    std::memcpy(&b[21],code,6); b[27]=b[28]=' '; put(b,30,9300000+n,4);
    b[34]=type;put(b,35,buy,8);put(b,43,sell,8);put(b,51,10000,4);
    put(b,55,qty,8);b[71]=side;return b;
}
Bytes snapshot(unsigned seq,const char* code="600000") {
    Bytes b(440);put(b,0,seq,4);b[8]=0x27;put(b,21,seq,4);put(b,26,93000,4);
    std::memcpy(&b[30],code,6);b[36]=b[37]=' ';
    for(unsigned o:{42,46,58})put(b,o,10000,4);
    put(b,50,11000,4);put(b,54,9000,4);return b;
}
StreamEvent event(const Bytes& bytes,std::uint64_t n,std::uint64_t hw,unsigned channel=4) {
    StreamEvent e={};e.kind=deepwin_market_data::kDatagramEvent;e.sequence=n;
    e.monotonic_ns=1000000+n*100;e.realtime_ns=100000000000ULL+e.monotonic_ns;
    e.channel_id=channel;e.hardware_ns=hw;e.timestamp_flags=deepwin_market_data::kHardwareTimestampRequested;
    e.data=bytes.data();e.size=bytes.size();return e;
}
StreamEvent idle(unsigned n) {
    StreamEvent e={};e.kind=deepwin_market_data::kIdleEvent;e.sequence=n;
    e.monotonic_ns=1000000+n*100;return e;
}
sse_tick::DailyStaticMetadataMap metadata() {
    sse_tick::DailyStaticMetadata m;m.date=20260914;m.avg_amount=1e8;
    m.turnover_threshold=.1;m.free_share=1e6;m.pre_close=10;m.limit_price=11;m.stop_price=9;
    m.has_date=m.has_avg_amount=m.has_turnover_threshold=m.has_free_share=true;
    m.has_pre_close=m.has_limit_price=m.has_stop_price=true;
    return {{"600000",m},{"600001",m}};
}
void equal(std::vector<Output> a,std::vector<Output> b) {
    // Cross-stock completion order is free; each stock's sequence and each
    // aggregate batch marker must still exactly match the serial processor.
    auto key=[](const Output& o){
        if(o.kind==sse_stream::kBatchEndOutput)return std::string("marker");
        return o.kind==sse_stream::kTickOutput?o.tick.event.security_id:o.snapshot.snapshot.security_id;
    };
    auto less=[&](const Output& x,const Output& y){return key(x)<key(y);};
    std::stable_sort(a.begin(),a.end(),less);std::stable_sort(b.begin(),b.end(),less);
    assert(a.size()==b.size());
    for(std::size_t i=0;i<a.size();++i) {
        assert(a[i].kind==b[i].kind);
        if(a[i].kind==sse_stream::kBatchEndOutput) {
            const auto& x=a[i].batch_end;const auto& y=b[i].batch_end;
            assert(x.batch_id==y.batch_id && x.last_hardware_ns==y.last_hardware_ns);
            assert(x.emitted_monotonic_ns==y.emitted_monotonic_ns && x.packet_count==y.packet_count);
            assert(x.candidate_count==y.candidate_count && x.prediction_count==y.prediction_count);
        } else if(a[i].kind==sse_stream::kTickOutput) {
            const auto& x=a[i].tick;const auto& y=b[i].tick;
            assert(x.event.security_id==y.event.security_id && x.event.tick_index==y.event.tick_index);
            assert(x.provenance.record_offset==y.provenance.record_offset && x.provenance.batch_id==y.provenance.batch_id);
            assert(x.factors.values==y.factors.values && x.prediction.heads==y.prediction.heads);
            assert(x.sample_decision.reasons==y.sample_decision.reasons);
        } else {
            assert(a[i].snapshot.snapshot.sequence==b[i].snapshot.snapshot.sequence);
            assert(a[i].snapshot.snapshot.security_id==b[i].snapshot.snapshot.security_id);
            assert(a[i].snapshot.provenance.record_offset==b[i].snapshot.provenance.record_offset);
        }
    }
}
struct Pair {
    std::vector<Output> reference,actual;
    std::vector<std::vector<Output> > batches;
    sse_stream::SseStreamProcessor serial;
    sse_stream::SseParallelProcessor parallel;
    Pair(sse_hybrid_model::Model& model) : reference(),actual(),
        serial(metadata(),&model,false,[this](const Output& o){reference.push_back(o);},{},{},false),
        parallel(metadata(),&model,false,[this](const std::vector<Output>& outputs){
            actual.insert(actual.end(),outputs.begin(),outputs.end());
            batches.push_back(outputs);
        },{},{},false) {}
    void send(const StreamEvent& e) {serial.on_event(e);parallel.on_event(e);}
    void compare() {parallel.flush();equal(reference,actual);}
};
void test_boundaries(sse_hybrid_model::Model& model) {
    Pair p(model);p.send(idle(1));
    Bytes burst;
    const Bytes parts[]={tick(1,'A',0,101,0),tick(2,'A',1,0,201),
        tick(3,'A',0,102,0,"600001"),tick(4,'A',1,0,202,"600001"),
        tick(5,'T',0,101,201,"600000",100000),tick(6,'T',0,102,202,"600001",100000)};
    for(const auto& b:parts)burst.insert(burst.end(),b.begin(),b.end());
    p.send(event(burst,2,10000));p.parallel.flush();assert(p.actual.empty());
    auto snap=snapshot(1,"600001");p.send(event(snap,3,80000,9));
    p.send(idle(4));p.compare();
    unsigned ticks=0;for(const auto& o:p.actual)ticks+=o.kind==sse_stream::kTickOutput;
    assert(ticks==2); // Full batch has both stocks, in deterministic order.
    unsigned delivered_predictions=0;
    for(const auto& batch:p.batches) {
        assert(batch.size()==1);
        if(batch[0].kind==sse_stream::kTickOutput) {
            ++delivered_predictions;
            assert(batch[0].tick.provenance.batch_emitted_ns);
        }
    }
    assert(delivered_predictions==2);
    auto t=tick(7,'T',0,101,201,"600000",100000);
    p.send(event(t,5,30000));
    t=tick(8,'T',0,102,202,"600001",100000);p.send(event(t,6,29999)); // PHC backwards: no slice.
    t=tick(9,'T',0,101,201,"600000",100000);p.send(event(t,7,0)); // missing PHC: no slice.
    p.parallel.flush();p.compare();
    p.send(idle(8));p.compare();
    t=tick(9,'T',0,101,201,"600000",100000);p.send(event(t,9,90000)); // duplicate
    p.send(idle(10));p.compare();
}
void test_capacity_and_ownership(sse_hybrid_model::Model& model) {
    Pair p(model);Bytes burst;
    for(unsigned i=1;i<=6000;++i) {
        auto b=snapshot(i,i%2?"600000":"600001");burst.insert(burst.end(),b.begin(),b.end());
    }
    p.send(event(burst,1,10000));
    std::fill(burst.begin(),burst.end(),0xff); // borrowed payload may be reused immediately
    p.send(idle(2));p.compare();assert(p.actual.size()==6001);
}
void test_transport_epoch(sse_hybrid_model::Model& model) {
    sse_stream::SseParallelProcessor p(metadata(),&model,false,[](const std::vector<Output>&){},{},{},false);
    auto a=tick(1,'A',0,101,0);p.on_event(event(a,1,10000));
    auto b=tick(2,'A',1,0,201);p.on_event(event(b,2,10100));
    auto t=tick(3,'T',0,101,201,"600000",100000);p.on_event(event(t,3,10200));
    p.begin_transport_epoch(1000400);
    auto d=tick(4,'D',0,101,0,"600000",900000);auto e=event(d,1,1);e.monotonic_ns=100;p.on_event(e);p.flush();assert(!p.invalid());
    bool gap=false;try{auto missing=tick(6,'A',0,303,0);e=event(missing,2,2);e.monotonic_ns=200;p.on_event(e);}catch(...){gap=true;}
    assert(gap && p.invalid());
}
void test_fallback(sse_hybrid_model::Model& model) {
    Pair p(model);p.send(idle(1));auto b=snapshot(1);
    auto e=event(b,2,0);e.timestamp_flags=0;p.send(e);p.send(idle(3));p.compare();
    assert(!p.parallel.parallel_enabled());
    for(const auto& batch:p.batches)assert(batch.size()==1);
}
void test_failures(sse_hybrid_model::Model& model) {
    for(unsigned scenario=0;scenario<3;++scenario) {
        sse_stream::SseParallelProcessor p(metadata(),&model,false,
            [scenario](const std::vector<Output>&){if(scenario==2)throw std::runtime_error("callback failure");},{},{},false);
        bool caught=false;
        try {
            if(scenario==0) { auto a=tick(1,'A',0,1,0);p.on_event(event(a,1,10000));
                auto b=tick(3,'A',0,2,0);p.on_event(event(b,2,10100)); }
            if(scenario==1) { auto b=tick(1,'D',0,999,0);p.on_event(event(b,1,10000)); }
            if(scenario==2) { auto b=snapshot(1);p.on_event(event(b,1,10000)); }
            p.flush();
        }catch(const std::exception&){caught=true;}
        assert(caught && p.invalid());
    }
    // Destructor stops outstanding work without invoking callbacks after scope exit.
    for(unsigned i=0;i<10;++i) {
        sse_stream::SseParallelProcessor p(metadata(),&model,false,[](const std::vector<Output>&){},{},{},false);
        auto b=tick(1,'A',0,1,0);p.on_event(event(b,1,10000));
    }
}
}
void test_async_delivery(sse_hybrid_model::Model& model) {
    cpu_set_t allowed; CPU_ZERO(&allowed);
    assert(!sched_getaffinity(0, sizeof(allowed), &allowed));
    std::vector<int> cpus;
    for (unsigned i = 0; i < CPU_SETSIZE; ++i)
        if (CPU_ISSET(i, &allowed)) cpus.push_back(static_cast<int>(i));
    assert(cpus.size() >= 3);
    std::string workers = std::to_string(cpus[0]) + "," + std::to_string(cpus[1]);
    setenv("SSE_PREDICTION_CPUS", workers.c_str(), 1);
    setenv("SSE_STRATEGY_CPU", std::to_string(cpus[2]).c_str(), 1);
    std::vector<int> sequence;
    const std::thread::id owner = std::this_thread::get_id();
    std::thread::id callback_thread;
    {
        sse_stream::SseParallelProcessor processor(metadata(), &model, false,
            [&](const std::vector<Output>& outputs) {
                assert(outputs.size()==1);
                callback_thread = std::this_thread::get_id();
                sequence.push_back(1);
            }, {}, {}, false);
        Bytes bytes = snapshot(1);
        processor.on_event(event(bytes, 1, 10000));
        processor.post_strategy([&]() {
            assert(std::this_thread::get_id() != owner);
            sequence.push_back(2);
        });
        processor.wait_strategy();
        // A posted control is immediately part of the delivery waterline;
        // it must not hide behind an unfinished worker ticket outside the queue.
        assert(std::find(sequence.begin(),sequence.end(),2)!=sequence.end());
        processor.flush();
        assert(processor.strategy_consumer_enabled());
        assert(processor.strategy_consumer_started());
        assert(processor.pending_delivery() == 0);
        assert(processor.strategy_consumer_published() ==
               processor.strategy_consumer_consumed());
        assert(callback_thread != owner);
        assert(sequence.size()==2 && sequence[0]+sequence[1]==3);
        processor.post_strategy([&](){sequence.push_back(3);});
        processor.post_strategy([&](){sequence.push_back(4);});
        processor.wait_strategy();
        assert(sequence.size()==4 && sequence[2]==3 && sequence[3]==4);
        processor.stop_strategy();
    }
    {
        sse_stream::SseParallelProcessor processor(metadata(), &model, false,
            [](const std::vector<Output>&) { throw std::runtime_error("async callback failure"); },
            {}, {}, false);
        Bytes bytes = snapshot(2);
        processor.on_event(event(bytes, 1, 10000));
        bool caught = false;
        try { processor.flush(); } catch (const std::exception&) { caught = true; }
        assert(caught && processor.invalid());
    }
    unsetenv("SSE_STRATEGY_CPU");
}
void test_async_queue_wrap_and_control_failure(sse_hybrid_model::Model& model) {
    cpu_set_t allowed; CPU_ZERO(&allowed);
    assert(!sched_getaffinity(0, sizeof(allowed), &allowed));
    std::vector<int> cpus;
    for (unsigned i = 0; i < CPU_SETSIZE; ++i)
        if (CPU_ISSET(i, &allowed)) cpus.push_back(static_cast<int>(i));
    assert(cpus.size() >= 3);
    std::string workers = std::to_string(cpus[0]) + "," + std::to_string(cpus[1]);
    setenv("SSE_PREDICTION_CPUS", workers.c_str(), 1);
    setenv("SSE_STRATEGY_CPU", std::to_string(cpus[2]).c_str(), 1);
    const std::size_t count = 8193;
    std::vector<std::size_t> sequence;
    {
        sse_stream::SseParallelProcessor processor(metadata(), &model, false,
            [&](const std::vector<Output>&) {}, {}, {}, false);
        for (std::size_t i = 0; i < count; ++i) {
            processor.post_strategy([&sequence, i]() {
                if (i == 0)
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                sequence.push_back(i);
            });
        }
        processor.flush();
        assert(sequence.size() == count);
        for (std::size_t i = 0; i < count; ++i) assert(sequence[i] == i);
        assert(processor.strategy_consumer_high_water() <= 4096);
        assert(processor.strategy_consumer_full_waits() > 0);
        processor.stop_strategy();
    }
    {
        sse_stream::SseParallelProcessor processor(metadata(), &model, false,
            [](const std::vector<Output>&) {}, {}, {}, false);
        processor.post_strategy([]() {
            throw std::runtime_error("async control failure");
        });
        bool caught = false;
        try { processor.flush(); } catch (const std::exception&) { caught = true; }
        assert(caught && processor.invalid());
        bool stopped = false;
        try { processor.stop_strategy(); } catch (const std::exception&) { stopped = true; }
        assert(stopped);
    }
    unsetenv("SSE_STRATEGY_CPU");
}
void test_async_poll_and_output_ownership(sse_hybrid_model::Model& model) {
    cpu_set_t allowed; CPU_ZERO(&allowed);
    assert(!sched_getaffinity(0,sizeof(allowed),&allowed));
    std::vector<int> cpus;
    for(unsigned i=0;i<CPU_SETSIZE;++i)
        if(CPU_ISSET(i,&allowed))cpus.push_back(static_cast<int>(i));
    assert(cpus.size()>=3);
    const std::string workers=std::to_string(cpus[0])+","+std::to_string(cpus[1]);
    setenv("SSE_PREDICTION_CPUS",workers.c_str(),1);
    setenv("SSE_STRATEGY_CPU",std::to_string(cpus[2]).c_str(),1);
    const auto owner=std::this_thread::get_id();
    {
        // Multi-output batches span logical credits and must wrap without
        // treating their unused physical slots as new delivery items.
        Pair p(model);
        Bytes initial;
        const Bytes orders[]={tick(1,'A',0,101,0,"600000",1000000000),
            tick(2,'A',1,0,201,"600000",1000000000),
            tick(3,'A',0,102,0,"600001",1000000000),
            tick(4,'A',1,0,202,"600001",1000000000)};
        for(const auto& bytes:orders)initial.insert(initial.end(),bytes.begin(),bytes.end());
        p.send(event(initial,1,10000));
        unsigned wire=5,sequence=2;
        for(unsigned batch=0;batch<2500;++batch) {
            Bytes trades=tick(wire++,'T',0,101,201,"600000",100000);
            const Bytes second=tick(wire++,'T',0,102,202,"600001",100000);
            trades.insert(trades.end(),second.begin(),second.end());
            p.send(event(trades,sequence++,20000+batch*10000));
            p.send(idle(sequence++));
        }
        p.compare();
        assert(p.actual.size()>4096);
        assert(p.parallel.strategy_consumer_published()==p.actual.size());
        assert(p.parallel.strategy_consumer_consumed()==p.actual.size());
        assert(p.parallel.strategy_consumer_high_water()<=4096);
        for(const auto& batch:p.batches)assert(batch.size()==1);
    }
    {
        std::atomic<unsigned> polls(0);
        std::atomic<bool> blocked(false),release(false);
        bool polled=false; // Owned exclusively by the strategy consumer.
        std::vector<Output> actual;
        sse_stream::SseParallelProcessor processor(metadata(),&model,false,
            [&](const std::vector<Output>& outputs) {
                assert(std::this_thread::get_id()!=owner && polled);
                polled=false;
                actual.insert(actual.end(),outputs.begin(),outputs.end());
            },{},{},false);
        assert(polls.load()==0 && processor.strategy_consumer_published()==0);
        processor.set_strategy_poll([&]() {
            assert(std::this_thread::get_id()!=owner);
            polled=true;
            polls.fetch_add(1,std::memory_order_release);
        });
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
        while(polls.load(std::memory_order_acquire)<3) {
            assert(std::chrono::steady_clock::now()<deadline);
            std::this_thread::yield();
        }
        // No market event or repeated control message is needed to advance the owner.
        assert(processor.strategy_consumer_published()==1);
        assert(processor.strategy_consumer_consumed()==1);
        processor.post_strategy([&]() {
            blocked.store(true,std::memory_order_release);
            while(!release.load(std::memory_order_acquire))std::this_thread::yield();
            polled=false;
        });
        while(!blocked.load(std::memory_order_acquire)) {
            assert(std::chrono::steady_clock::now()<deadline);
            std::this_thread::yield();
        }
        // Both worker rings wrap while the consumer retains the moved outputs.
        // The entire backlog fits the delivery ring, so the producer can finish.
        const unsigned count=3000;
        Bytes burst;
        for(unsigned i=1;i<=count;++i) {
            const auto bytes=snapshot(i,i%2?"600000":"600001");
            burst.insert(burst.end(),bytes.begin(),bytes.end());
        }
        processor.on_event(event(burst,1,10000,9));
        std::fill(burst.begin(),burst.end(),0xff);
        release.store(true,std::memory_order_release);
        processor.flush();
        assert(actual.size()==count);
        std::vector<bool> seen(count+1,false);std::map<std::string,unsigned> last;
        for(const auto& out:actual) {
            assert(out.kind==sse_stream::kSnapshotOutput);
            const auto& snapshot=out.snapshot.snapshot;const auto sequence=snapshot.sequence;
            assert(sequence>=1 && sequence<=count && !seen[sequence]);seen[sequence]=true;
            assert(snapshot.security_id==(sequence%2?"600000":"600001"));
            assert(sequence>last[snapshot.security_id]);last[snapshot.security_id]=sequence;
        }
        processor.stop_strategy();
    }
    {
        const nlohmann::json legacy=nlohmann::json::parse(
            "{\"ins_params\":{\"600000.SH\":{\"vol_unit\":100}}}");
        oms_test::ManagedFixture fixture(legacy,"SH",1,"owner-idle-timer");
        auto monotonic=[]() -> oms::Time {
            timespec now;clock_gettime(CLOCK_MONOTONIC,&now);
            return oms::Time(now.tv_sec)*1000000000LL+now.tv_nsec;
        };
        fixture.engine->advance_to(monotonic());
        oms::Intent intent;
        intent.owner="owner-idle-timer";
        intent.intent_id="cancel-without-market-data";
        intent.instrument={"SSE","600000"};
        intent.price=100000;
        intent.quantity=100;
        const oms::SubmitResult order=fixture.engine->submit(intent);
        assert(order.accepted);
        assert(fixture.engine->schedule_cancel(intent.owner,order.id,1000000));
        std::atomic<unsigned> cancels(0);
        fixture.backend->cancel_hook=[&](const oms::Command& command) {
            assert(std::this_thread::get_id()!=owner && command.id==order.id);
            cancels.fetch_add(1,std::memory_order_release);
            oms::SendResult result;
            result.disposition=oms::SendDisposition::Submitted;
            return result;
        };
        sse_stream::SseParallelProcessor processor(metadata(),&model,false,
            [](const std::vector<Output>&){assert(false);},{},{},false);
        processor.set_strategy_poll([&]() {fixture.engine->advance_to(monotonic());});
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
        while(!cancels.load(std::memory_order_acquire)) {
            assert(std::chrono::steady_clock::now()<deadline);
            std::this_thread::yield();
        }
        processor.stop_strategy();
        assert(processor.strategy_consumer_published()==1);
        assert(processor.strategy_consumer_consumed()==1);
        oms::OrderView view;
        assert(fixture.engine->order(order.id,&view) && view.cancel_requested);
    }
    {
        std::atomic<bool> throw_now(false);
        sse_stream::SseParallelProcessor processor(metadata(),&model,false,
            [](const std::vector<Output>&){assert(false);},{},{},false);
        processor.set_strategy_poll([&]() {
            if(throw_now.load(std::memory_order_acquire))
                throw std::runtime_error("idle strategy poll failure");
        });
        throw_now.store(true,std::memory_order_release);
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
        while(processor.strategy_consumer_valid()) {
            assert(std::chrono::steady_clock::now()<deadline);
            std::this_thread::yield();
        }
        assert(processor.strategy_consumer_published()==1);
        bool caught=false;
        try {processor.flush();}
        catch(const std::exception& e) {caught=std::string(e.what())=="idle strategy poll failure";}
        assert(caught);
        bool stopped=false;
        try {processor.stop_strategy();}catch(const std::exception&) {stopped=true;}
        assert(stopped);
    }
    unsetenv("SSE_STRATEGY_CPU");
}
void test_independent_output(sse_hybrid_model::Model& model) {
    unsetenv("SSE_STRATEGY_CPU");
    auto data=metadata();data["600002"]=data.begin()->second;
    std::vector<Output> actual;
    sse_stream::SseParallelProcessor processor(data,&model,false,
        [&](const std::vector<Output>& out){actual.insert(actual.end(),out.begin(),out.end());},{},{},false);
    blocked_code.store(600000);output_blocked.store(false);release_output.store(false);
    Bytes burst;unsigned wire=1;
    for(const char* code:{"600000","600001","600002"}) {
        for(const auto& part:{tick(wire++,'A',0,101,0,code),tick(wire++,'A',1,0,201,code),
                             tick(wire++,'T',0,101,201,code,100000)})
            burst.insert(burst.end(),part.begin(),part.end());
    }
    processor.on_event(event(burst,1,10000));processor.flush();assert(actual.empty());
    processor.on_event(idle(2));
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
    bool first=false,other=false;
    while(!first||!other||!output_blocked.load(std::memory_order_acquire)) {
        assert(std::chrono::steady_clock::now()<deadline);processor.poll_completed();
        for(const auto& out:actual) {
            assert(out.kind!=sse_stream::kBatchEndOutput);
            assert(out.tick.event.security_id!="600002");
            first|=out.tick.event.security_id=="600000";other|=out.tick.event.security_id=="600001";
        }
    }
    // A later batch on the other worker must not queue behind the blocked one.
    auto later=tick(wire++,'T',0,101,201,"600001",100000);
    processor.on_event(event(later,3,20000));processor.on_event(idle(4));
    while(actual.size()<3) {
        assert(std::chrono::steady_clock::now()<deadline);processor.poll_completed();
    }
    assert(actual.back().tick.event.security_id=="600001");
    release_output.store(true,std::memory_order_release);blocked_code.store(0);processor.flush();
    unsigned markers=0,ticks=0;for(const auto&o:actual){markers+=o.kind==sse_stream::kBatchEndOutput;ticks+=o.kind==sse_stream::kTickOutput;}
    assert(markers==2&&ticks==4);
}

void test_tick_only_backpressure(sse_hybrid_model::Model& model) {
    unsetenv("SSE_STRATEGY_CPU");
    sse_stream::SseParallelProcessor processor(metadata(),&model,false,
        [](const std::vector<Output>&){assert(false);},{},{},false);
    hold_ticks.store(true);release_ticks.store(false);ticks_blocked.store(false);tick_queue_full.store(false);
    // Each datagram now occupies one task per affected owner. Fill the ring
    // with distinct already-received datagrams, keeping the input batch open.
    std::atomic<bool> done(false);
    std::thread feeder([&](){for(unsigned i=1;i<=1500;++i){auto b=tick(i,'A',0,i,0);processor.on_event(event(b,i,10000));}processor.flush();done.store(true,std::memory_order_release);});
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
    while(!tick_queue_full.load(std::memory_order_acquire)||!ticks_blocked.load(std::memory_order_acquire)){assert(std::chrono::steady_clock::now()<deadline);std::this_thread::yield();}
    assert(ticks_blocked.load());release_ticks.store(true,std::memory_order_release);
    while(!done.load(std::memory_order_acquire)){assert(std::chrono::steady_clock::now()<deadline);std::this_thread::yield();}
    feeder.join();hold_ticks.store(false);assert(processor.pending_events()==0);
}

int main() {
    cpu_set_t cpus;CPU_ZERO(&cpus);assert(!sched_getaffinity(0,sizeof(cpus),&cpus));
    std::ostringstream list;unsigned count=0;
    for(unsigned i=0;i<CPU_SETSIZE && count<2;++i)if(CPU_ISSET(i,&cpus)){if(count++)list<<',';list<<i;}
    if(count<2)throw std::runtime_error("test requires two allowed CPUs");
    setenv("SSE_PREDICTION_CPUS",list.str().c_str(),1);
    const std::string path="/tmp/sse-owner-test-"+std::to_string(getpid())+".bin";
    { std::ofstream f(path.c_str(),std::ios::binary);f.write("SSEV06M1",8);
      const unsigned sizes[]={50,50,6400,128,49152,49152,384,384,49152,49152,384,384,512,4};
      for(unsigned n:sizes){std::vector<float> zero(n,0);f.write(reinterpret_cast<char*>(zero.data()),n*4);} }
    sse_hybrid_model::Model model;std::string error;assert(model.load_v06(path,&error));unlink(path.c_str());
    // Malformed duplicates/unconfigured records must retain validation even
    // when field materialization is delegated to a worker.
    for(unsigned mode=0;mode<3;++mode){
        sse_stream::SseParallelProcessor p(metadata(),&model,false,[](const std::vector<Output>&){},{},{},false);
        bool caught=false;try{
            auto first=tick(1,'A',0,777,0);p.on_event(event(first,1,10000));p.flush();
            if(mode==0){first[34]='X';p.on_event(event(first,2,10001));}
            if(mode==1){first[30]=0xff;first[31]=0xff;first[32]=0xff;first[33]=0xff;p.on_event(event(first,2,10001));}
            if(mode==2){auto snap=snapshot(0,"600999");p.on_event(event(snap,2,10001));}
            p.flush();
        }catch(const std::exception&){caught=true;}
        assert(caught&&p.invalid());
    }
    test_tick_only_backpressure(model);test_independent_output(model);test_transport_epoch(model);test_boundaries(model);test_capacity_and_ownership(model);test_fallback(model);test_failures(model);test_async_delivery(model);test_async_queue_wrap_and_control_failure(model);test_async_poll_and_output_ownership(model);
    std::cout<<"sse_owner_dispatch_test: PASS\n";
}
