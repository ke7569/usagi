// Offline fixed-prefix replay. Never opens the live ring or an ATP session.
// Build with SSE_PARITY_HAS_PARALLEL after the facade header/library are available.
#include "sse/runtime/sse_journal_transport.h"
#include "sse/runtime/sse_stream_processor.h"
#ifdef SSE_PARITY_HAS_PARALLEL
#include "sse/runtime/sse_parallel_processor.h"
#endif
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>

namespace {
typedef nlohmann::json Json;
typedef std::chrono::steady_clock Clock;
typedef std::uint64_t U64;
U64 ns_since(const Clock::time_point& t) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-t).count();
}
struct Hash {
    U64 value;
    Hash():value(14695981039346656037ULL) {}
    void bytes(const void* p,std::size_t n) {
        const unsigned char* b=static_cast<const unsigned char*>(p);
        for(std::size_t i=0;i<n;++i) { value^=b[i];value*=1099511628211ULL; }
    }
    template<class T> void add(const T& value) { bytes(&value,sizeof(value)); }
    void text(const std::string& text) { U64 n=text.size();add(n);bytes(text.data(),text.size()); }
};
std::string hex(U64 v) { std::ostringstream s;s<<std::hex<<std::setw(16)<<std::setfill('0')<<v;return s.str(); }
std::uint32_t crc32(const void* p,std::size_t size) {
    static std::uint32_t table[256];static bool ready=false;
    if(!ready) { for(unsigned i=0;i<256;++i) { std::uint32_t c=i;for(unsigned j=0;j<8;++j)c=(c&1)?0xedb88320U^(c>>1):(c>>1);table[i]=c; }ready=true; }
    std::uint32_t c=~0U;const unsigned char* b=static_cast<const unsigned char*>(p);
    for(std::size_t i=0;i<size;++i)c=table[(c^b[i])&255]^(c>>8);
    return ~c;
}
struct Stock {
    U64 raw_ticks,tick_datagrams,adds,deletes,trades,statuses,raw_snapshots;
    U64 tick_samples,tick_model_calls,model_predictions,selected,snapshot_outputs;
    U64 turnover_triggers,time_triggers,change_triggers;
    Hash ordered_output_hash;
    Stock():raw_ticks(0),tick_datagrams(0),adds(0),deletes(0),trades(0),statuses(0),raw_snapshots(0),
        tick_samples(0),tick_model_calls(0),model_predictions(0),selected(0),snapshot_outputs(0),
        turnover_triggers(0),time_triggers(0),change_triggers(0) {}
};

// Each record has explicit field serialization. No C++ padding enters the file.
// The 64-bit full-output hash additionally covers the tick, book, snapshot,
// prediction flags and complete factor validity metadata. Ordered record bytes
// expose heads, sample decisions, factor CRC and all provenance directly.
class Evidence {
public:
    explicit Evidence(const std::string& path):file_(std::getenv("SSE_PARITY_HASH_ONLY") ? "/dev/null" : path.c_str(),std::ios::binary),count_(0),tick_count_(0),
        snapshot_count_(0),batch_count_(0),model_count_(0),hash_() {
        file_.exceptions(std::ios::badbit|std::ios::failbit);
        file_.write("SSEPAR01",8);
    }
    template<class T> void write(const T& value) { file_.write(reinterpret_cast<const char*>(&value),sizeof(value));hash_.add(value); }
    void bytes(const void* p,std::size_t n) {file_.write(static_cast<const char*>(p),n);hash_.bytes(p,n);}
    void provenance(const sse_stream::Provenance& p,Hash& full) {
        const U64 values[]={static_cast<U64>(p.stream_kind),p.stream_sequence,p.monotonic_ns,p.realtime_ns,
            p.receive_batch,p.batch_index,p.batch_size,p.stream_channel_id,p.wire_channel_no,p.wire_sequence,
            p.record_offset,p.source_ipv4,p.source_port,p.timestamp_flags,p.batch_id,p.batch_emitted_ns,
            static_cast<U64>(p.batch_close_reason)};
        bytes(values,sizeof(values));full.bytes(values,sizeof(values));
    }
    void output(const sse_stream::Output& o,std::map<std::string,Stock>& stocks) {
        ++count_;write(count_);const std::uint32_t kind=o.kind;write(kind);
        const std::string code=o.kind==sse_stream::kTickOutput?o.tick.event.security_id:
            o.kind==sse_stream::kSnapshotOutput?o.snapshot.snapshot.security_id:std::string();
        char symbol[8]={};std::memcpy(symbol,code.data(),std::min(code.size(),sizeof(symbol)));bytes(symbol,sizeof(symbol));
        Hash full;full.add(kind);full.text(code);
        U64 time=0,wire=0;std::uint32_t factors_crc=0;float heads[4]={};
        std::uint32_t flags=0,reasons=0;double turnover=0;std::int64_t volume=0;U64 elapsed=0;
        sse_stream::Provenance prov;
        if(o.kind==sse_stream::kTickOutput) {
            ++tick_count_;const auto& t=o.tick;Stock& stock=stocks[code];++stock.tick_samples;
            time=t.event.time_of_day_micros;wire=t.event.tick_index;prov=t.provenance;
            const auto& e=t.event;
            full.add(e.channel_no);full.add(e.provider_sequence);full.add(e.tick_index);full.add(e.app_seq_num);
            full.add(e.time_of_day_micros);full.add(e.event_type);full.add(e.buy_order_no);full.add(e.sell_order_no);
            full.add(e.price_raw);full.add(e.quantity_raw);full.add(e.amount_raw);full.add(e.side);
            factors_crc=crc32(t.factors.values.data(),sizeof(float)*t.factors.values.size());
            full.bytes(t.factors.values.data(),sizeof(float)*t.factors.values.size());
            full.add(t.factors.mid_price);full.add(t.factors.tick_index);
            const auto& validity=t.factors.validity;
            full.add(validity.has_two_sided_book);full.add(validity.has_previous_book);full.add(validity.has_flow);
            full.add(validity.has_free_share);full.add(validity.has_static_metadata);full.add(validity.static_metadata_complete);full.add(validity.complete);
            for(const auto* levels:{&t.bid_levels,&t.ask_levels}) {
                U64 n=levels->size();full.add(n);
                for(const auto& l:*levels) {full.add(l.price_raw);full.add(l.quantity);full.add(l.order_count);full.add(l.add_time_sum_micros);full.add(l.young_quantity);}
            }
            full.add(t.last_trade_price);full.add(t.total_trade_volume);full.add(t.total_trade_turnover);
            const auto& p=t.prediction;
            flags=(t.prediction_valid?1U:0U)|(p.multi_head?2U:0U)|(p.selected?4U:0U)|
                (p.tick_generated?8U:0U)|(p.snapshot_generated?16U:0U)|(static_cast<unsigned>(p.selected_source)<<8);
            for(unsigned i=0;i<4;++i)heads[i]=p.heads[i];
            full.add(p.tick_pred);full.add(p.snapshot_pred);full.add(p.selected_pred);
            full.add(t.sample_decision.accepted);reasons=t.sample_decision.reasons;
            turnover=t.sample_decision.window_turnover;volume=t.sample_decision.window_volume;elapsed=t.sample_decision.window_exchange_time_micros;
            if(reasons&sse_live_sampling::kTurnoverSampleReason)++stock.turnover_triggers;
            if(reasons&sse_live_sampling::kTimeSampleReason)++stock.time_triggers;
            if(reasons&sse_live_sampling::kChangeSampleReason)++stock.change_triggers;
            if(t.prediction_valid)++stock.tick_model_calls;
            if(t.prediction_valid&&p.multi_head) {++stock.model_predictions;++model_count_;}
            if(t.prediction_valid&&p.selected)++stock.selected;
        } else if(o.kind==sse_stream::kSnapshotOutput) {
            ++snapshot_count_;++stocks[code].snapshot_outputs;const auto& s=o.snapshot.snapshot;
            time=s.time_of_day_micros;wire=s.sequence;prov=o.snapshot.provenance;
            full.add(s.channel_no);full.add(s.provider_sequence);full.add(s.msg_seq_id);full.add(s.sequence);
            full.add(s.exchange_time_raw);full.add(s.time_of_day_micros);full.add(s.pre_close_price);full.add(s.open_price);
            full.add(s.high_price);full.add(s.low_price);full.add(s.last_price);full.add(s.volume);full.add(s.turnover);
            full.bytes(s.bid_prices,sizeof(s.bid_prices));full.bytes(s.bid_volumes,sizeof(s.bid_volumes));
            full.bytes(s.ask_prices,sizeof(s.ask_prices));full.bytes(s.ask_volumes,sizeof(s.ask_volumes));
            full.add(o.snapshot.prediction_valid);U64 n=o.snapshot.snapshot36.size();full.add(n);
            full.bytes(o.snapshot.snapshot36.data(),n*sizeof(float));n=o.snapshot.auction59.size();full.add(n);
            full.bytes(o.snapshot.auction59.data(),n*sizeof(float));
        } else if(o.kind==sse_stream::kBatchEndOutput) {
            ++batch_count_;const auto& b=o.batch_end;full.add(b.batch_id);full.add(b.last_hardware_ns);
            full.add(b.emitted_monotonic_ns);full.add(b.packet_count);full.add(b.candidate_count);full.add(b.prediction_count);
            prov.batch_id=b.batch_id;prov.batch_emitted_ns=b.emitted_monotonic_ns;
            time=b.last_hardware_ns;wire=b.packet_count;reasons=b.candidate_count;flags=b.prediction_count;
        } else throw std::runtime_error("unknown output kind");
        write(time);write(wire);write(flags);write(reasons);write(turnover);write(volume);write(elapsed);write(factors_crc);bytes(heads,sizeof(heads));
        full.add(time);full.add(wire);full.add(flags);full.add(reasons);full.add(turnover);full.add(volume);full.add(elapsed);full.bytes(heads,sizeof(heads));
        provenance(prov,full);write(full.value);
        if(!code.empty())stocks[code].ordered_output_hash.add(full.value);
    }
    void finish() { file_.flush(); }
    Json summary()const {return Json{{"outputs",count_},{"tick_samples",tick_count_},{"snapshot_outputs",snapshot_count_},
        {"batch_outputs",batch_count_},{"model_predictions",model_count_},{"ordered_evidence_hash",hex(hash_.value)}};}
private:
    std::ofstream file_;U64 count_,tick_count_,snapshot_count_,batch_count_,model_count_;Hash hash_;
};

void count_raw(const deepwin_market_data::StreamEvent& event,std::map<std::string,Stock>& stocks,
               U64& datagrams,U64& idle,U64& heartbeats,U64& raw_ticks,U64& raw_snapshots,U64& record_bytes) {
    if(event.kind==deepwin_market_data::kIdleEvent) {++idle;return;}
    if(event.kind!=deepwin_market_data::kDatagramEvent)throw std::runtime_error("unknown stream kind");
    ++datagrams;
    if(sse_live::is_primary_heartbeat(event.data,event.size)){++heartbeats;return;}
    std::set<std::string> seen;std::size_t offset=0;
    while(offset<event.size) {
        const std::size_t left=event.size-offset;std::string err;sse_live::TickEvent t;sse_live::Snapshot s;
        if(left>=72 && sse_live::decode_primary_tick(event.data+offset,72,&t,&err,false)) {
            ++raw_ticks;Stock& stock=stocks[t.security_id];++stock.raw_ticks;seen.insert(t.security_id);
            switch(t.event_type){case 'A':++stock.adds;break;case 'D':++stock.deletes;break;case 'T':++stock.trades;break;case 'S':++stock.statuses;break;}
            offset+=72;record_bytes+=72;
        }else if(left>=440 && sse_live::decode_primary_snapshot(event.data+offset,440,&s,&err,false)) {
            ++raw_snapshots;++stocks[s.security_id].raw_snapshots;offset+=440;record_bytes+=440;
        }else throw std::runtime_error("raw decode failed at event "+std::to_string(event.sequence)+" offset "+std::to_string(offset));
    }
    for(const auto& code:seen)++stocks[code].tick_datagrams;
}
} // namespace

int main(int argc,char**argv) {
    if(argc!=8) {std::cerr<<"usage: parallel-journal-parity serial|parallel|raw capture.json daily.json model.bin MAX_EVENT_ID OUTPUT_PREFIX CPU_LIST\n";return 2;}
    try {
        const std::string mode=argv[1],prefix=argv[6];
        if(mode!="serial"&&mode!="parallel"&&mode!="raw")throw std::runtime_error("invalid mode");
        const U64 max_id=std::stoull(argv[5]);if(!max_id)throw std::runtime_error("fixed maximum event id required");
        setenv("SSE_V06_INFER_WORKERS","1",1);
        if(mode=="serial")unsetenv("SSE_PREDICTION_CPUS");else setenv("SSE_PREDICTION_CPUS",argv[7],1);
        const auto transport=sse_journal::load(argv[2]);
        const Json daily=load_stream_json(argv[3]);
        if(daily.at("trading_day").get<unsigned>()!=transport.journal.trading_day)throw std::runtime_error("daily date mismatch");
        sse_tick::DailyStaticMetadataMap metadata;
        for(auto it=daily.at("ins_params").begin();it!=daily.at("ins_params").end();++it) {
            const auto& p=it.value();sse_tick::DailyStaticMetadata m;m.date=transport.journal.trading_day;m.has_date=true;
            m.avg_amount=p.at("HistoryAmount");m.has_avg_amount=true;m.turnover_threshold=m.avg_amount/8000;m.has_turnover_threshold=true;
            m.free_share=p.at("FreeShare");m.has_free_share=true;m.pre_close=p.at("Close");m.has_pre_close=true;
            m.limit_price=p.at("HpUpperPrice");m.has_limit_price=true;m.stop_price=p.at("HpLowerPrice");m.has_stop_price=true;
            metadata[it.key().substr(0,6)]=m;
        }
        sse_hybrid_model::Model model;std::string err;if(!model.load_v06(argv[4],&err))throw std::runtime_error(err);
        Evidence evidence(prefix+".outputs.bin");std::map<std::string,Stock> stocks;
        for(const auto& x:metadata)stocks[x.first]=Stock();
        const sse_stream::OutputCallback callback=[&](const sse_stream::Output& o){evidence.output(o,stocks);};
        std::unique_ptr<sse_stream::SseStreamProcessor> serial;
#ifdef SSE_PARITY_HAS_PARALLEL
        std::unique_ptr<sse_stream::SseParallelProcessor> parallel;
        if(mode=="parallel")parallel.reset(new sse_stream::SseParallelProcessor(metadata,&model,false,
            [&](const std::vector<sse_stream::Output>& batch){for(const auto& output:batch)callback(output);},
            sse_stream::Auction59Provider(),sse_auction59::StaticMetadataMap(),false));
        else
#else
        if(mode=="parallel")throw std::runtime_error("compile with SSE_PARITY_HAS_PARALLEL to test facade");
#endif
        if(mode!="raw")serial.reset(new sse_stream::SseStreamProcessor(metadata,&model,false,callback,
            sse_stream::Auction59Provider(),sse_auction59::StaticMetadataMap(),false));
        sze_recovery::JournalReader reader;const auto opened=reader.open(transport.journal);
        if(opened.status!=sze_recovery::kJournalOk)throw std::runtime_error("journal open failed "+std::to_string(opened.status));
        std::vector<unsigned char> payload(transport.journal.max_payload_bytes);sze_recovery::CanonicalEvent canonical={};
        U64 events=0,last_id=0,datagrams=0,idle=0,heartbeats=0,raw_ticks=0,raw_snapshots=0,record_bytes=0;
        U64 count_ns=0,processing_ns=0;const auto started=Clock::now();
        while(last_id<max_id) {
            const auto status=reader.next(&canonical,payload.data(),payload.size());
            if(status==sze_recovery::kJournalEnd||status==sze_recovery::kJournalWouldBlock)break;
            if(status!=sze_recovery::kJournalOk)throw std::runtime_error("journal read failed "+std::to_string(status));
            if(canonical.event_id>max_id)break;
            if(canonical.event_id!=last_id+1)throw std::runtime_error("journal event sequence differs from contiguous prefix");
            const auto event=sse_journal::decode(canonical,payload.data(),transport);
            auto begin=Clock::now();count_raw(event,stocks,datagrams,idle,heartbeats,raw_ticks,raw_snapshots,record_bytes);count_ns+=ns_since(begin);
            begin=Clock::now();
#ifdef SSE_PARITY_HAS_PARALLEL
            if(parallel)parallel->on_event(event);else
#endif
            if(serial)serial->on_event(event);
            processing_ns+=ns_since(begin);++events;last_id=canonical.event_id;
            if(events%250000==0)std::cerr<<"parity mode="<<mode<<" events="<<events<<" raw_ticks="<<raw_ticks<<" elapsed_s="<<ns_since(started)/1e9<<'\n';
        }
        auto drain_begin=Clock::now();
#ifdef SSE_PARITY_HAS_PARALLEL
        if(parallel)parallel->flush();
#endif
        const U64 drain_ns=ns_since(drain_begin);processing_ns+=drain_ns;evidence.finish();
        if(last_id!=max_id)throw std::runtime_error("journal prefix shorter than requested max event id");
        std::ofstream per_stock((prefix+".stocks.csv").c_str());
        per_stock<<"instrument,configured,raw_ticks,tick_datagrams,adds,deletes,trades,statuses,raw_snapshots,tick_samples,tick_model_calls,model_predictions,selected,snapshot_outputs,turnover_triggers,time_triggers,change_triggers,ordered_output_hash\n";
        for(const auto& item:stocks){const Stock& s=item.second;per_stock<<item.first<<','<<metadata.count(item.first)<<','<<s.raw_ticks<<','<<s.tick_datagrams<<','<<s.adds<<','<<s.deletes<<','<<s.trades<<','<<s.statuses<<','<<s.raw_snapshots<<','<<s.tick_samples<<','<<s.tick_model_calls<<','<<s.model_predictions<<','<<s.selected<<','<<s.snapshot_outputs<<','<<s.turnover_triggers<<','<<s.time_triggers<<','<<s.change_triggers<<','<<hex(s.ordered_output_hash.value)<<'\n';}
        Json result=evidence.summary();const Stock& s=stocks["600000"];
        result["ok"]=true;result["mode"]=mode;result["last_event_id"]=last_id;result["events"]=events;result["configured_stocks"]=metadata.size();
        result["datagrams"]=datagrams;result["idle_events"]=idle;result["heartbeats"]=heartbeats;
        result["raw_tick_records"]=raw_ticks;result["raw_snapshot_records"]=raw_snapshots;result["decoded_record_bytes"]=record_bytes;
        result["raw_count_ns"]=count_ns;result["processing_call_ns"]=processing_ns;result["drain_ns"]=drain_ns;result["wall_ns"]=ns_since(started);
        result["wall_events_per_second"]=events/(result["wall_ns"].get<double>()/1e9);
        result["timing_scope"]="fixed journal read + raw decode counting + processor + evidence writer; processing_call_ns includes callbacks and drain";
        result["stock_600000"]={{"raw_ticks",s.raw_ticks},{"tick_datagrams",s.tick_datagrams},{"model_predictions",s.model_predictions},
            {"raw_ticks_per_prediction",s.model_predictions?double(s.raw_ticks)/s.model_predictions:0.0}};
        std::ofstream report((prefix+".summary.json").c_str());report<<result.dump(2)<<'\n';std::cout<<result.dump()<<'\n';
    }catch(const std::exception& e){std::cerr<<"parity failed: "<<e.what()<<'\n';return 1;}
}
