// Fixed-prefix production processor throughput, without a second raw decode
// pass, ATP, per-output audit serialization, or per-output disk writes.
#include "sse/runtime/sse_journal_transport.h"
#include "sse/runtime/sse_stream_processor.h"
#include "sse/runtime/sse_parallel_processor.h"
#include "sse/model/v06_model.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace {
using nlohmann::json;
typedef std::uint64_t U64;
typedef std::chrono::steady_clock Clock;
U64 ns(const Clock::time_point& start) {return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-start).count();}
std::string hexadecimal(U64 value){std::ostringstream s;s<<std::hex<<std::setw(16)<<std::setfill('0')<<value;return s.str();}
void mix(U64& checksum,U64 value){checksum^=value;checksum*=1099511628211ULL;}
unsigned float_bits(float value){unsigned result;std::memcpy(&result,&value,sizeof(result));return result;}
U64 code_value(const std::string& code){U64 result=0;for(char c:code)result=(result<<8)^static_cast<unsigned char>(c);return result;}
unsigned phase(U64 ns_utc){const unsigned second=static_cast<unsigned>((ns_utc/1000000000ULL+28800ULL)%86400ULL);return second<33900?0:second<34200?1:second<34380?2:second<34500?3:4;}
const char* phase_name(unsigned id){const char* names[]={"before_09:25","09:25_to_09:30","09:30_to_09:33","09:33_to_09:35","at_or_after_09:35"};return names[std::min(id,4U)];}
struct Counts {
    U64 events=0,datagrams=0,idles=0,bytes=0,callbacks=0,tick_samples=0,predictions=0,selected=0,snapshots=0,batches=0;
    U64 wall_ns=0,call_ns=0,read_ns=0;
    U64 enqueue_calls=0,enqueue_ns=0,flush_calls=0,flush_ns=0,final_flush_ns=0;
    U64 first_event=0,last_event=0,first_receive_ns=0,last_receive_ns=0;
    U64 first_prediction_time=0,last_prediction_time=0;
    void add(const Counts& v){
        events+=v.events;datagrams+=v.datagrams;idles+=v.idles;bytes+=v.bytes;
        callbacks+=v.callbacks;tick_samples+=v.tick_samples;predictions+=v.predictions;selected+=v.selected;snapshots+=v.snapshots;batches+=v.batches;
        wall_ns+=v.wall_ns;call_ns+=v.call_ns;read_ns+=v.read_ns;enqueue_calls+=v.enqueue_calls;enqueue_ns+=v.enqueue_ns;
        flush_calls+=v.flush_calls;flush_ns+=v.flush_ns;final_flush_ns+=v.final_flush_ns;
        if(!first_event&&v.first_event){first_event=v.first_event;first_receive_ns=v.first_receive_ns;}
        if(v.last_event){last_event=v.last_event;last_receive_ns=v.last_receive_ns;}
        if(!first_prediction_time&&v.first_prediction_time)first_prediction_time=v.first_prediction_time;
        if(v.last_prediction_time)last_prediction_time=v.last_prediction_time;
    }
    json output()const{
        return json{{"events",events},{"datagrams",datagrams},{"idle_events",idles},{"payload_bytes",bytes},
            {"callbacks",callbacks},{"tick_samples",tick_samples},{"model_predictions",predictions},{"selected",selected},
            {"snapshot_outputs",snapshots},{"batch_outputs",batches},{"wall_ns",wall_ns},{"processor_call_ns",call_ns},{"journal_read_decode_ns",read_ns},
            {"enqueue_calls",enqueue_calls},{"enqueue_ns",enqueue_ns},{"flush_calls",flush_calls},{"flush_call_ns",flush_ns},{"final_flush_ns",final_flush_ns},
            {"first_event_id",first_event},{"last_event_id",last_event},{"first_receive_ns",first_receive_ns},{"last_receive_ns",last_receive_ns},
            {"first_prediction_exchange_us",first_prediction_time},{"last_prediction_exchange_us",last_prediction_time},
            {"events_per_second",wall_ns?events*1e9/wall_ns:0},{"predictions_per_second",wall_ns?predictions*1e9/wall_ns:0},
            {"amortized_processor_us_per_event",events?call_ns/1000.0/events:0},
            {"amortized_processor_us_per_prediction",predictions?call_ns/1000.0/predictions:0}};
    }
};
struct ModelInput {sse_v06::Factors factors;sse_v06::State state;};
json model_microbenchmark(const char* artifact,const std::map<std::string,sse_v06::Factors>& factors){
    if(factors.empty())return json{{"ok",false},{"error","no captured actual factor rows"}};
    sse_v06::Model model;std::string error;if(!model.load(artifact,&error))throw std::runtime_error(error);
    std::vector<ModelInput> rows;for(const auto& f:factors)rows.push_back(ModelInput{f.second,sse_v06::State()});
    sse_v06::Heads heads;const U64 warmup=4096,iterations=32768;U64 checksum=14695981039346656037ULL;
    for(U64 i=0;i<warmup;++i){ModelInput& row=rows[i%rows.size()];if(!model.predict(row.factors,&row.state,&heads))throw std::runtime_error("model warmup failed");}
    U64 total=0,minimum=~0ULL,maximum=0;std::vector<U64> measured;measured.reserve(iterations);
    const auto wall=Clock::now();
    for(U64 i=0;i<iterations;++i){ModelInput& row=rows[i%rows.size()];const auto start=Clock::now();
        if(!model.predict(row.factors,&row.state,&heads))throw std::runtime_error("model microbenchmark failed");
        const U64 elapsed=ns(start);measured.push_back(elapsed);total+=elapsed;minimum=std::min(minimum,elapsed);maximum=std::max(maximum,elapsed);
        for(float h:heads)mix(checksum,float_bits(h));}
    const U64 wall_ns=ns(wall);std::sort(measured.begin(),measured.end());
    return json{{"ok",true},{"stocks",rows.size()},{"warmup_calls",warmup},{"calls",iterations},{"total_model_call_ns",total},
        {"mean_us",total/1000.0/iterations},{"min_us",minimum/1000.0},{"p50_us",measured[iterations/2]/1000.0},
        {"p90_us",measured[iterations*90/100]/1000.0},{"p99_us",measured[iterations*99/100]/1000.0},{"max_us",maximum/1000.0},
        {"wall_ns",wall_ns},{"checksum",hexadecimal(checksum)},
        {"scope","Native v0.6 Model::predict only; 32768 calls round-robin over per-stock GRU states with actual first accepted factor rows; warm model tensors; excludes journal, decode, book, factors, sampling, output audit, strategy, OMS, ATP."}};
}
} // namespace

int main(int argc,char**argv){
    if(argc!=8){std::cerr<<"usage: journal-throughput-benchmark serial|parallel capture.json daily.json v06.bin MAX_EVENT_ID OUTPUT_PREFIX CPU_LIST\n";return 2;}
    try{
        const std::string mode=argv[1],prefix=argv[6];if(mode!="serial"&&mode!="parallel")throw std::runtime_error("invalid mode");
        const U64 limit=std::stoull(argv[5]);if(!limit)throw std::runtime_error("fixed prefix required");
        setenv("SSE_V06_INFER_WORKERS","1",1);if(mode=="parallel")setenv("SSE_PREDICTION_CPUS",argv[7],1);else unsetenv("SSE_PREDICTION_CPUS");
        const auto transport=sse_journal::load(argv[2]);const auto daily=load_stream_json(argv[3]);
        if(daily.at("trading_day").get<unsigned>()!=transport.journal.trading_day)throw std::runtime_error("daily date mismatch");
        sse_tick::DailyStaticMetadataMap metadata;
        for(auto it=daily.at("ins_params").begin();it!=daily.at("ins_params").end();++it){const auto& p=it.value();sse_tick::DailyStaticMetadata m;
            m.date=transport.journal.trading_day;m.has_date=true;m.avg_amount=p.at("HistoryAmount");m.has_avg_amount=true;
            m.turnover_threshold=m.avg_amount/8000;m.has_turnover_threshold=true;m.free_share=p.at("FreeShare");m.has_free_share=true;
            m.pre_close=p.at("Close");m.has_pre_close=true;m.limit_price=p.at("HpUpperPrice");m.has_limit_price=true;m.stop_price=p.at("HpLowerPrice");m.has_stop_price=true;
            metadata[it.key().substr(0,6)]=m;}
        sse_hybrid_model::Model model;std::string error;if(!model.load_v06(argv[4],&error))throw std::runtime_error(error);
        Counts current,total;std::array<Counts,5> phases;U64 checksum=14695981039346656037ULL;
        std::map<std::string,sse_v06::Factors> micro_inputs;std::vector<json> intervals;
        const sse_stream::OutputCallback callback=[&](const sse_stream::Output& o){++current.callbacks;mix(checksum,o.kind);
            if(o.kind==sse_stream::kTickOutput){++current.tick_samples;const auto& t=o.tick;mix(checksum,code_value(t.event.security_id));mix(checksum,t.event.time_of_day_micros);mix(checksum,t.provenance.stream_sequence);mix(checksum,t.event.tick_index);
                if(t.prediction_valid&&t.prediction.multi_head){++current.predictions;if(t.prediction.selected)++current.selected;
                    if(!current.first_prediction_time)current.first_prediction_time=t.event.time_of_day_micros;current.last_prediction_time=t.event.time_of_day_micros;
                    for(float h:t.prediction.heads)mix(checksum,float_bits(h));mix(checksum,t.prediction.selected);
                    if(micro_inputs.size()<512 && !micro_inputs.count(t.event.security_id))micro_inputs[t.event.security_id]=t.factors.values;}
            }else if(o.kind==sse_stream::kSnapshotOutput){++current.snapshots;mix(checksum,code_value(o.snapshot.snapshot.security_id));mix(checksum,o.snapshot.snapshot.sequence);mix(checksum,o.snapshot.provenance.stream_sequence);
            }else if(o.kind==sse_stream::kBatchEndOutput){++current.batches;mix(checksum,o.batch_end.batch_id);mix(checksum,o.batch_end.packet_count);mix(checksum,o.batch_end.candidate_count);mix(checksum,o.batch_end.prediction_count);}};
        std::unique_ptr<sse_stream::SseStreamProcessor> serial;std::unique_ptr<sse_stream::SseParallelProcessor> parallel;
        if(mode=="parallel")parallel.reset(new sse_stream::SseParallelProcessor(metadata,&model,false,
            [&](const std::vector<sse_stream::Output>& batch){for(const auto& output:batch)callback(output);},sse_stream::Auction59Provider(),sse_auction59::StaticMetadataMap(),false));
        else serial.reset(new sse_stream::SseStreamProcessor(metadata,&model,false,callback,sse_stream::Auction59Provider(),sse_auction59::StaticMetadataMap(),false));
        sze_recovery::JournalReader reader;const auto opened=reader.open(transport.journal);if(opened.status!=sze_recovery::kJournalOk)throw std::runtime_error("journal open failed");
        sze_recovery::CanonicalEvent canonical={};std::vector<unsigned char> payload(transport.journal.max_payload_bytes);
        std::ofstream progress((prefix+".intervals.jsonl").c_str());progress.exceptions(std::ios::failbit|std::ios::badbit);
        U64 event_id=0,interval_start=0;unsigned current_phase=0;const auto overall=Clock::now();auto window=Clock::now();
        auto report=[&](){if(!current.events)return;const auto start=Clock::now();if(parallel)parallel->flush();const U64 tail=ns(start);current.final_flush_ns+=tail;current.call_ns+=tail;
            current.wall_ns=ns(window);json row=current.output();row["receive_phase"]=phase_name(current_phase);row["checksum_so_far"]=hexadecimal(checksum);
            intervals.push_back(row);total.add(current);phases[current_phase].add(current);progress<<row.dump()<<'\n';progress.flush();
            std::cerr<<"throughput mode="<<mode<<" event_id="<<event_id<<" predictions="<<total.predictions<<" phase="<<phase_name(current_phase)<<" events_s="<<row["events_per_second"]<<'\n';
            current=Counts();interval_start=event_id;window=Clock::now();};
        while(event_id<limit){const auto read_start=Clock::now();const auto status=reader.next(&canonical,payload.data(),payload.size());
            if(status==sze_recovery::kJournalWouldBlock||status==sze_recovery::kJournalEnd)break;
            if(status!=sze_recovery::kJournalOk)throw std::runtime_error("journal read failed "+std::to_string(status));
            if(canonical.event_id!=event_id+1)throw std::runtime_error("non-contiguous event prefix");
            const auto event=sse_journal::decode(canonical,payload.data(),transport);const U64 read_time=ns(read_start);
            const U64 receive=event.application_realtime_ns?event.application_realtime_ns:event.realtime_ns;const unsigned event_phase=phase(receive);
            // Only a few phase boundaries force an early worker block drain;
            // all sampler boundaries still derive from the original events.
            if(current.events&&event_phase!=current_phase)report();current_phase=event_phase;
            current.read_ns+=read_time;if(!current.first_event){current.first_event=canonical.event_id;current.first_receive_ns=receive;}
            current.last_event=canonical.event_id;current.last_receive_ns=receive;++current.events;current.bytes+=event.size;
            if(event.kind==deepwin_market_data::kDatagramEvent)++current.datagrams;else ++current.idles;
            const bool will_flush=parallel&&parallel->parallel_enabled()&&parallel->pending_events()==255;
            const auto call_start=Clock::now();if(parallel)parallel->on_event(event);else serial->on_event(event);const U64 call_time=ns(call_start);current.call_ns+=call_time;
            if(parallel){if(will_flush){++current.flush_calls;current.flush_ns+=call_time;}else{++current.enqueue_calls;current.enqueue_ns+=call_time;}}
            event_id=canonical.event_id;if(event_id-interval_start>=250000)report();
        }
        report();if(event_id!=limit)throw std::runtime_error("journal does not contain full requested prefix");
        const U64 wall=ns(overall);json result=total.output();result["ok"]=true;result["mode"]=mode;result["last_event_id"]=event_id;
        result["workers"]=parallel?parallel->worker_count():1;result["cpu_list"]=mode=="parallel"?argv[7]:"serial caller affinity";
        result["ordered_output_checksum"]=hexadecimal(checksum);result["universe"]=metadata.size();result["overall_wall_ns"]=wall;result["intervals"]=intervals;
        result["phase_summaries"]=json::array();for(unsigned i=0;i<5;++i){auto row=phases[i].output();row["receive_phase"]=phase_name(i);result["phase_summaries"].push_back(row);}
        result["timing_scope"]="Uninstrumented production processors with lightweight serialized callback counts/checksum. No second raw tick decode, per-output evidence/audit IO, strategy, OMS, ATP. Journal read/decode separately timed. Flush-call time includes worker execution, barrier wait and ordered output merge.";
        result["phase_clock"]="Original application receive wall clock, Asia/Shanghai; predictions separately report exchange-time bounds. Phase transition drains do not synthesize sampler events.";
        std::ofstream output((prefix+".summary.json").c_str());output<<result.dump(2)<<'\n';output.close();std::cout<<result.dump()<<'\n';
        if(mode=="serial"){
            serial.reset();const auto micro=model_microbenchmark(argv[4],micro_inputs);std::ofstream micro_out((prefix+".model-micro.json").c_str());micro_out<<micro.dump(2)<<'\n';
            std::cerr<<"model_micro "<<micro.dump()<<'\n';}
    }catch(const std::exception& e){std::cerr<<"benchmark failed: "<<e.what()<<'\n';return 1;}
}
