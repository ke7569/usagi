#include "tests/sse/latency_probe.h"
#include "sse/runtime/sse_shm_prefetch.h"
#include "sse/runtime/sse_journal_transport.h"
#include "sse/runtime/sse_capture_history.h"
#include <fstream>
#include <iostream>
#include <mutex>
#include <vector>
#include <array>
#include <iomanip>
using U=std::uint64_t;using J=nlohmann::json;
std::atomic<bool> trace_active(false);
U source_anchor=0,wall_anchor=0;
std::mutex registry_mutex;
struct Compute {std::array<U,11> v;};
std::vector<std::vector<std::array<U,7>>*> job_rows;
std::map<std::pair<U,U>,U> close_times;
void trace_close(unsigned c,U b,U t){close_times[std::make_pair(c,b)]=t;}
void trace_job(unsigned c,U b,int cpu,U q,U start,U done,std::size_t count){
 thread_local std::vector<std::array<U,7>>* rows=nullptr;
 if(!rows){rows=new std::vector<std::array<U,7>>;rows->reserve(100000);std::lock_guard<std::mutex>g(registry_mutex);job_rows.push_back(rows);}
 rows->push_back({{c,b,U(cpu),q,start,done,count}});
}

std::vector<std::vector<Compute>*> computations;
std::map<std::vector<Compute>*,int> compute_cpu;
std::vector<std::array<U,13>> delivery_rows;
std::vector<sse_v06::Heads> delivery_heads;
std::vector<std::array<U,6>> strategy_rows;
bool selected(const sse_stream::Output&o){return o.kind==sse_stream::kTickOutput&&o.tick.prediction_valid&&o.tick.provenance.monotonic_ns>=source_anchor;}
void trace_compute(const sse_stream::Output&o,U a,U b,U c,U d,U e){
    if(!selected(o))return;
    thread_local std::vector<Compute>* rows=nullptr;
    if(!rows){rows=new std::vector<Compute>;rows->reserve(100000);std::lock_guard<std::mutex>g(registry_mutex);computations.push_back(rows);compute_cpu[rows]=sched_getcpu();}
    rows->push_back({{{std::stoull(o.tick.event.security_id),o.tick.provenance.stream_channel_id,
        o.tick.provenance.batch_id,o.tick.provenance.stream_sequence,o.tick.provenance.wire_sequence,
        o.tick.provenance.monotonic_ns,a,b,c,d,e}}});
}
void trace_strategy(const sse_stream::Output&o){if(!selected(o))return;
    strategy_rows.push_back({{std::stoull(o.tick.event.security_id),o.tick.provenance.stream_channel_id,
        o.tick.provenance.batch_id,o.tick.provenance.wire_sequence,o.tick.provenance.monotonic_ns,trace_ns()}});
}
void sse_critical_path_probe(const sse_stream::Output&o,U close,U queued,U start,U ready,U done,U delivery,std::size_t worker,std::size_t predictions){
    if(!trace_active.load(std::memory_order_acquire)||!selected(o))return;
    delivery_rows.push_back({{std::stoull(o.tick.event.security_id),o.tick.provenance.stream_channel_id,
        o.tick.provenance.batch_id,o.tick.provenance.wire_sequence,o.tick.provenance.monotonic_ns,
        close,queued,start,ready,done,delivery,worker,predictions}});
    delivery_heads.push_back(o.tick.prediction.heads);
}

void trace_delivery(const sse_stream::Output&o){if(!selected(o))return;
 const U worker=(std::stoull(o.tick.event.security_id));
 delivery_rows.push_back({{worker,o.tick.provenance.stream_channel_id,o.tick.provenance.batch_id,o.tick.provenance.wire_sequence,o.tick.provenance.monotonic_ns,0,0,0,0,0,trace_ns(),0,0}});
 delivery_heads.push_back(o.tick.prediction.heads);
}
void sse_strategy_signal_probe(const sse_stream::Output&o){if(trace_active.load(std::memory_order_acquire))trace_strategy(o);}
struct Digest { U count=0,hash=1469598103934665603ULL; };
std::vector<std::map<std::string,Digest>*> digests;
std::vector<J> recorded_intents;
namespace sse_application {void opt_order_intent(const J& j){recorded_intents.push_back(j);}}
void verify_output(const sse_stream::Output& o) {
    if(o.kind==sse_stream::kBatchEndOutput)return;
    thread_local std::map<std::string,Digest>* rows=0;
    if(!rows){rows=new std::map<std::string,Digest>;std::lock_guard<std::mutex>g(registry_mutex);digests.push_back(rows);}
    const bool tick=o.kind==sse_stream::kTickOutput;
    const std::string& code=tick?o.tick.event.security_id:o.snapshot.snapshot.security_id;
    Digest& d=(*rows)[code];++d.count;
    auto bytes=[&](const void* data,size_t size){auto*p=static_cast<const unsigned char*>(data);for(size_t i=0;i<size;++i){d.hash^=p[i];d.hash*=1099511628211ULL;}};
    const U kind=o.kind,wire=tick?o.tick.event.tick_index:o.snapshot.snapshot.sequence;
    bytes(&kind,sizeof(kind));bytes(&wire,sizeof(wire));
    if(tick)bytes(o.tick.factors.values.data(),o.tick.factors.values.size()*sizeof(float));
    else bytes(o.snapshot.snapshot36.data(),o.snapshot.snapshot36.size()*sizeof(float));
    const auto& p=tick?o.tick.prediction:o.snapshot.prediction;
    const bool valid=tick?o.tick.prediction_valid:o.snapshot.prediction_valid;
    bytes(&valid,sizeof(valid));bytes(&p.selected,sizeof(p.selected));
    if(valid)bytes(p.heads.data(),p.heads.size()*sizeof(float));
}
#include "apps/StreamProcessingCli.h"
J read_json(const char*p){std::ifstream f(p);J j;f>>j;return j;}
template<class Row>void csv(std::ostream&out,const Row&r){for(size_t i=0;i<r.size();++i){if(i)out<<',';out<<r[i];}}
int main(int argc,char**argv){try{
    if(argc!=7){std::cerr<<"usage: sse_latency_replay CHAIN ACTIVE PROFILE OUTPUT START_NS END_NS\n";return 2;}
    const J chain=read_json(argv[1]);const auto active=sse_journal::load(argv[2],true);
    if(active.stream.idle_gap_ns!=5000 || chain.at("active_generation").get<U>()!=active.journal.generation)throw std::runtime_error("wrong active capture");
    sse_application::StreamProcessingCli app(argv[3],false,active.journal.directory,2,[](){return true;});
    delivery_rows.reserve(200000);delivery_heads.reserve(200000);strategy_rows.reserve(200000);
    U total_history=0;
    for(const auto& segment:chain.at("segments")){
        const auto cfg=sse_journal::load(segment.at("capture_config").get<std::string>(),true);
        sse_journal::OverlapFilter overlap;if(segment.count("overlap"))overlap.configure(segment.at("overlap"),active.channels.size());
        sze_recovery::JournalReader reader;if(reader.open(cfg.journal).status!=sze_recovery::kJournalOk)throw std::runtime_error("history open");
        std::vector<unsigned char> payload(cfg.journal.max_payload_bytes);sze_recovery::CanonicalEvent c={};U last=0;
        const U limit=segment.at("last_event_id").get<U>();
        for(U n=1;n<=limit;++n){
            if(reader.next(&c,payload.data(),payload.size())!=sze_recovery::kJournalOk||c.event_id!=n)throw std::runtime_error("history sequence");
            const auto e=sse_journal::decode(c,payload.data(),cfg);
            if(overlap.accept(e,n)){app.on_event(e);last=e.monotonic_ns;++total_history;}
            if(n%1024==0 && sse_v06::audit_sink().pending()>16384){app.flush_processing();while(sse_v06::audit_sink().pending()>1024)std::this_thread::yield();}
            if(n%2000000==0)std::cerr<<"history "<<n<<"/"<<limit<<'\n';
        }
        overlap.complete();if(last)app.begin_transport_epoch(last);
    }
    sse_journal::OverlapFilter overlap;overlap.configure(chain.at("active_overlap"),active.channels.size());
    sze_recovery::JournalReader reader;if(reader.open(active.journal).status!=sze_recovery::kJournalOk)throw std::runtime_error("active open");
    std::vector<unsigned char> payload(active.journal.max_payload_bytes);sze_recovery::CanonicalEvent c={};
    const U target_start=std::stoull(argv[5]);
    const U target_end=std::stoull(argv[6]);
    if(target_end<=target_start)throw std::runtime_error("invalid replay window");
    struct Saved {sze_recovery::CanonicalEvent event;std::vector<unsigned char> bytes;};
    std::vector<Saved> window;window.reserve(100000);
    std::vector<std::array<U,3>> publish_rows;publish_rows.reserve(100000);
    sze_recovery::ShmEventRing shared_ring;
    std::atomic<bool> producer_ready(false),producer_go(false),producer_abort(false),producer_failed(false);
    std::thread producer;
    struct Join {std::thread& t;std::atomic<bool>& abort;~Join(){abort.store(true);if(t.joinable())t.join();}} join={producer,producer_abort};
    std::string ring_path="/dev/shm/usagi-ingress-bench-"+std::to_string(getpid());
    struct Unlink {std::string path;~Unlink(){unlink(path.c_str());}} cleanup={ring_path};
    std::unique_ptr<sse_journal::ShmPrefetch> prefetch;
    size_t window_index=0;
    U measured=0,last=0;std::vector<std::array<U,7>> ingress_rows;ingress_rows.reserve(100000);
    for(U n=1;;++n){
        U read_start=trace_ns(),read_end=0;
        if(window.empty()) {
            if(reader.next(&c,payload.data(),payload.size())!=sze_recovery::kJournalOk||c.event_id!=n)throw std::runtime_error("active sequence");
            auto before=sse_journal::decode(c,payload.data(),active);
            if(!overlap.accept(before,n))continue;
            if(before.monotonic_ns>=target_start) {
                overlap.complete();app.flush_processing();source_anchor=before.monotonic_ns;
                // Read and validate the whole measurement window before timing.
                for(;;) {
                    Saved item;item.event=c;item.bytes.assign(payload.begin(),payload.begin()+c.payload_size);window.push_back(std::move(item));
                    if(c.receive_mono_ns>=target_end)break;
                    const U next=c.event_id+1;
                    if(reader.next(&c,payload.data(),payload.size())!=sze_recovery::kJournalOk||c.event_id!=next)throw std::runtime_error("prefetch sequence");
                }
                auto ring_config=active.ring;ring_config.path=ring_path;
                producer=std::thread([&](){
                    cpu_set_t set;CPU_ZERO(&set);CPU_SET(16,&set);
                    if(pthread_setaffinity_np(pthread_self(),sizeof(set),&set)||!shared_ring.create(ring_config)){
                        producer_failed.store(true);producer_ready.store(true);return;
                    }
                    producer_ready.store(true,std::memory_order_release);
                    while(!producer_go.load(std::memory_order_acquire)){if(producer_abort.load())return;__builtin_ia32_pause();}
                    for(const auto& item:window) {
                        while(trace_ns()<wall_anchor+item.event.receive_mono_ns-source_anchor){if(producer_abort.load())return;__builtin_ia32_pause();}
                        if(producer_abort.load())return;
                        const U before_publish=trace_ns();
                        if(!shared_ring.publish(item.event,item.bytes.data())){producer_failed.store(true);return;}
                        publish_rows.push_back({{item.event.event_id,before_publish,trace_ns()}});
                    }
                });
                while(!producer_ready.load(std::memory_order_acquire))__builtin_ia32_pause();
                if(producer_failed.load())throw std::runtime_error("isolated SHM publisher start");
                wall_anchor=trace_ns()+500000000ULL;trace_active.store(true,std::memory_order_release);
                producer_go.store(true,std::memory_order_release);
                prefetch.reset(new sse_journal::ShmPrefetch(shared_ring,128));
                std::cerr<<"SHM paced start event="<<n<<" source="<<source_anchor<<" events="<<window.size()<<'\n';
            }
        }
        const unsigned char* borrowed=payload.data();
        if(!window.empty()) {
            const sse_journal::ShmPrefetch::Record* record=nullptr;
            for(;;){
                const auto status=prefetch->read(n,&record);
                if(status==sze_recovery::kRingReadOk)break;
                if(status!=sze_recovery::kRingReadNotReady||producer_failed.load())throw std::runtime_error("SHM prefetch failed");
                app.poll();__builtin_ia32_pause();
            }
            c=record->event;read_start=record->read_start;read_end=record->read_end;borrowed=record->payload.data();
        } else read_end=trace_ns();
        const auto e=sse_journal::decode(c,borrowed,active);
        if(trace_active.load(std::memory_order_acquire)){
            // SHM producer already paces publication; consume immediately.
            ++measured;
        }
        const U entry=trace_ns();app.on_event(e);const U done=trace_ns();last=e.monotonic_ns;
        if(!window.empty()){++window_index;}
        if(trace_active.load())ingress_rows.push_back({{n,U(e.kind),e.monotonic_ns,read_start,read_end,entry,done}});
        if(window.empty() && n%1024==0 && sse_v06::audit_sink().pending()>16384){app.flush_processing();while(sse_v06::audit_sink().pending()>1024)std::this_thread::yield();}
        if(n%500000==0)std::cerr<<"active "<<n<<" source="<<last<<'\n';
        if(last>=target_end)break;
    }
    app.flush_processing();if(prefetch)prefetch->stop();if(producer.joinable())producer.join();trace_active.store(false,std::memory_order_release);
    using Key=std::tuple<U,U,U,U,U>;
    std::map<Key,std::pair<const Compute*,int>> lookup;
    for(auto rows:computations)for(const auto& row:*rows)lookup[Key(row.v[1],row.v[2],row.v[0],row.v[4],row.v[5])]=std::make_pair(&row,compute_cpu.at(rows));
    std::map<std::tuple<U,U,U>,std::array<U,7>> jobs;
    for(auto rows:job_rows)for(const auto& row:*rows)jobs[std::make_tuple(row[0],row[1],row[2])]=row;
    for(auto& row:delivery_rows){
       const auto found=lookup.at(Key(row[1],row[2],row[0],row[3],row[4]));const auto& job=jobs.at(std::make_tuple(row[1],row[2],U(found.second-1)));
       row[5]=close_times.at(std::make_pair(row[1],row[2]));row[6]=job[3];row[7]=job[4];row[8]=found.first->v[10];row[9]=job[5];row[11]=(found.second-144)/8;row[12]=job[6];
    }
    J summary=app.summary();summary["source_anchor"]=source_anchor;summary["wall_anchor"]=wall_anchor;
    summary["history_events"]=total_history;summary["measured_events"]=measured;summary["measured_end"]=last;
    summary["capture_idle_gap_ns"]=active.stream.idle_gap_ns;summary["scope"]="Historical recovery then original receive pacing via real ShmEventRing publish/read; publisher CPU16, owner136, all stocks, paper strategy, no NIC or ATP sends";
    const std::string out=argv[4];std::ofstream summary_file(out+"/summary.json");summary_file<<summary.dump(2)<<'\n';
    std::ofstream work(out+"/compute.csv");work<<"code,channel,batch,stream,wire,receive_ns,gate_start,factor_start,factor_end,model_start,model_end\n";
    for(auto rows:computations)for(const auto&r:*rows){csv(work,r.v);work<<'\n';}
    std::ofstream deliveries(out+"/delivery.csv");deliveries<<"code,channel,batch,wire,receive_ns,close,queued,worker_start,output_ready,worker_done,delivery,worker,predictions,head0,head1,head2,head3\n";deliveries<<std::setprecision(9);
    for(size_t i=0;i<delivery_rows.size();++i){csv(deliveries,delivery_rows[i]);for(float h:delivery_heads[i])deliveries<<','<<h;deliveries<<'\n';}
    std::ofstream strategy(out+"/strategy.csv");strategy<<"code,channel,batch,wire,receive_ns,strategy_entry\n";for(const auto&r:strategy_rows){csv(strategy,r);strategy<<'\n';}
    std::ofstream pub(out+"/publisher.csv");pub<<"event,begin,end\n";for(const auto&r:publish_rows){csv(pub,r);pub<<'\n';}
    std::ofstream ingress(out+"/ingress.csv");ingress<<"event,kind,receive_ns,read_start,read_end,app_entry,app_done\n";for(const auto&r:ingress_rows){csv(ingress,r);ingress<<'\n';}

    J verified=J::object();
    for(auto rows:digests)for(const auto&row:*rows)verified[row.first]={{"count",row.second.count},{"digest",row.second.hash}};
    std::ofstream verified_file(out+"/per-stock.json");verified_file<<verified.dump(2)<<'\n';
    std::ofstream intents_file(out+"/intents.json");intents_file<<J(recorded_intents).dump(2)<<'\n';
    std::cout<<"history_events="<<total_history<<" measured_events="<<measured<<" predictions="<<delivery_rows.size()<<" strategy_entries="<<strategy_rows.size()<<'\n';
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
