#include "sse/market_data/sse_primary_decoder.h"
#include "sse/auction/auction59_engine.h"
#include "sse/auction/auction_static_metadata.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <glob.h>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <sstream>
#include <string>
#include <thread>
#include <time.h>
#include <unistd.h>
#include <vector>

namespace {

const std::uint64_t kWindowStart = (9ULL * 3600ULL + 15ULL * 60ULL) * 1000000ULL;
const std::uint64_t kWindowEnd = (9ULL * 3600ULL + 30ULL * 60ULL) * 1000000ULL;
std::atomic<bool> g_stop(false);

std::uint64_t monotonic_ns() {
    timespec value;
    clock_gettime(CLOCK_MONOTONIC, &value);
    return static_cast<std::uint64_t>(value.tv_sec) * 1000000000ULL + value.tv_nsec;
}

std::uint64_t local_time_micros() {
    timespec value;
    clock_gettime(CLOCK_REALTIME, &value);
    time_t seconds = value.tv_sec;
    tm local;
    if (!localtime_r(&seconds, &local)) return 0;
    return static_cast<std::uint64_t>(local.tm_hour * 3600 + local.tm_min * 60 + local.tm_sec) *
               1000000ULL + static_cast<std::uint64_t>(value.tv_nsec / 1000);
}

void on_signal(int) { g_stop.store(true); }

struct Config {
    std::string tick_root, static_csv, output_csv, quality_csv, metrics_json;
    std::uint32_t date;
    std::size_t workers;
    bool follow;
    std::uint64_t stop_time_micros;
    Config() : date(0), workers(4), follow(false), stop_time_micros(34020000000ULL) {}
};

bool parse_hms(const std::string& value, std::uint64_t* output) {
    unsigned hour = 0, minute = 0, second = 0;
    char tail = 0;
    if (std::sscanf(value.c_str(), "%u:%u:%u%c", &hour, &minute, &second, &tail) != 3 ||
        hour > 23 || minute > 59 || second > 59) return false;
    *output = static_cast<std::uint64_t>(hour * 3600 + minute * 60 + second) * 1000000ULL;
    return true;
}

bool parse_args(int argc, char** argv, Config* config) {
    for (int i = 1; i < argc; ++i) {
        const std::string key(argv[i]);
        if (key == "--follow") { config->follow = true; continue; }
        if (i + 1 >= argc) return false;
        const std::string value(argv[++i]);
        if (key == "--tick-root") config->tick_root = value;
        else if (key == "--static-csv") config->static_csv = value;
        else if (key == "--output-csv") config->output_csv = value;
        else if (key == "--quality-csv") config->quality_csv = value;
        else if (key == "--metrics-json") config->metrics_json = value;
        else if (key == "--date") config->date = static_cast<std::uint32_t>(std::strtoul(value.c_str(), 0, 10));
        else if (key == "--workers") config->workers = static_cast<std::size_t>(std::strtoul(value.c_str(), 0, 10));
        else if (key == "--stop-time") { if (!parse_hms(value, &config->stop_time_micros)) return false; }
        else return false;
    }
    return !config->tick_root.empty() && !config->static_csv.empty() &&
           !config->output_csv.empty() && !config->quality_csv.empty() &&
           !config->metrics_json.empty() && config->date > 20000000U && config->workers > 0U;
}

void usage(const char* executable) {
    std::cerr << "usage: " << executable
              << " --tick-root DIR --static-csv FILE --date YYYYMMDD"
              << " --output-csv FILE --quality-csv FILE --metrics-json FILE"
              << " [--workers N] [--follow] [--stop-time HH:MM:SS]\n";
}

std::vector<std::string> record_files(const std::string& root) {
    glob_t matches;
    std::memset(&matches, 0, sizeof(matches));
    std::vector<std::string> output;
    const std::string pattern = root + "/channel_*/records.bin";
    if (glob(pattern.c_str(), 0, 0, &matches) == 0) {
        for (std::size_t i = 0; i < matches.gl_pathc; ++i) output.push_back(matches.gl_pathv[i]);
    }
    globfree(&matches);
    return output;
}

struct Metrics {
    std::atomic<std::uint64_t> disk_records, decoded_ticks, filtered_records;
    std::atomic<std::uint64_t> dispatched_ticks, static_missing_events, status_events;
    std::atomic<std::uint64_t> decode_ns, dispatch_ns, update_ns, factor_ns;
    std::atomic<std::uint64_t> max_processing_lag_ns;
    Metrics() : disk_records(0), decoded_ticks(0), filtered_records(0), dispatched_ticks(0),
        static_missing_events(0), status_events(0), decode_ns(0), dispatch_ns(0),
        update_ns(0), factor_ns(0), max_processing_lag_ns(0) {}
};

void atomic_max(std::atomic<std::uint64_t>* target, std::uint64_t value) {
    std::uint64_t old = target->load();
    while (old < value && !target->compare_exchange_weak(old, value)) {}
}

struct WorkItem {
    sse_live::DiskRecordHeader header;
    sse_live::TickEvent event;
};

class WorkQueue {
public:
    WorkQueue() : closed_(false) {}
    bool push(WorkItem&& item) {
        std::unique_lock<std::mutex> lock(mutex_);
        not_full_.wait(lock, [this] { return closed_ || items_.size() < 32768U; });
        if (closed_) return false;
        items_.push_back(std::move(item)); not_empty_.notify_one(); return true;
    }
    bool pop(WorkItem* item) {
        std::unique_lock<std::mutex> lock(mutex_);
        not_empty_.wait(lock, [this] { return closed_ || !items_.empty(); });
        if (items_.empty()) return false;
        *item = std::move(items_.front()); items_.pop_front(); not_full_.notify_one(); return true;
    }
    void close() {
        std::lock_guard<std::mutex> lock(mutex_); closed_ = true;
        not_empty_.notify_all(); not_full_.notify_all();
    }
private:
    std::mutex mutex_;
    std::condition_variable not_empty_, not_full_;
    std::deque<WorkItem> items_;
    bool closed_;
};

class ResultCollector {
public:
    void put(const sse_auction59::AuctionResult& result) {
        std::lock_guard<std::mutex> lock(mutex_); results_[result.security_id] = result;
    }
    std::map<std::string, sse_auction59::AuctionResult> take() {
        std::lock_guard<std::mutex> lock(mutex_); return results_;
    }
private:
    std::mutex mutex_;
    std::map<std::string, sse_auction59::AuctionResult> results_;
};

class AuctionWorker {
public:
    AuctionWorker(const sse_auction59::StaticMetadataMap* metadata,
                  ResultCollector* collector, Metrics* metrics)
        : metadata_(metadata), collector_(collector), metrics_(metrics) {}
    WorkQueue* queue() { return &queue_; }
    void run() {
        WorkItem item;
        while (queue_.pop(&item)) process(item);
        for (AccumulatorMap::iterator it = states_.begin(); it != states_.end(); ++it) {
            const std::uint64_t started = monotonic_ns();
            collector_->put(it->second->finalize());
            metrics_->factor_ns.fetch_add(monotonic_ns() - started);
        }
        states_.clear();
    }
private:
    typedef std::map<std::string, std::unique_ptr<sse_auction59::AuctionAccumulator> > AccumulatorMap;
    void process(const WorkItem& item) {
        const std::uint64_t started = monotonic_ns();
        sse_auction59::StaticMetadataMap::const_iterator metadata = metadata_->find(item.event.security_id);
        if (metadata == metadata_->end()) { metrics_->static_missing_events.fetch_add(1); return; }
        AccumulatorMap::iterator state = states_.find(item.event.security_id);
        if (state == states_.end()) {
            std::unique_ptr<sse_auction59::AuctionAccumulator> value(
                new sse_auction59::AuctionAccumulator(item.event.security_id, metadata->second));
            state = states_.insert(std::make_pair(item.event.security_id, std::move(value))).first;
        }
        state->second->observe(item.event, item.header.realtime_ns, item.header.arrival_index);
        const bool finish = item.event.event_type == 'S' && state->second->has_status();
        metrics_->update_ns.fetch_add(monotonic_ns() - started);
        if (finish) {
            const std::uint64_t factor_started = monotonic_ns();
            collector_->put(state->second->finalize());
            metrics_->factor_ns.fetch_add(monotonic_ns() - factor_started);
            states_.erase(state);
        }
    }
    const sse_auction59::StaticMetadataMap* metadata_;
    ResultCollector* collector_;
    Metrics* metrics_;
    WorkQueue queue_;
    AccumulatorMap states_;
};

struct RawRecord {
    sse_live::DiskRecordHeader header;
    std::vector<unsigned char> payload;
};

class InputReader {
public:
    explicit InputReader(const std::string& path)
        : path_(path), input_(path.c_str(), std::ios::binary), past_window_(false) {}
    bool good() const { return input_.good(); }
    bool past_window() const { return past_window_; }
    void mark_past_window() { past_window_ = true; }
    bool next(RawRecord* record, bool follow) {
        if (past_window_) return false;
        const std::streampos position = input_.tellg();
        input_.read(reinterpret_cast<char*>(&record->header), sizeof(record->header));
        if (input_.gcount() != static_cast<std::streamsize>(sizeof(record->header))) {
            input_.clear(); input_.seekg(position); return false;
        }
        if (record->header.magic != sse_live::kDiskRecordMagic || record->header.version != 1U ||
            record->header.header_size != sizeof(record->header)) {
            std::cerr << "invalid disk record header: " << path_ << "\n"; g_stop.store(true); return false;
        }
        record->payload.resize(record->header.payload_length);
        input_.read(reinterpret_cast<char*>(&record->payload[0]), record->payload.size());
        if (input_.gcount() != static_cast<std::streamsize>(record->payload.size())) {
            input_.clear(); input_.seekg(position);
            if (!follow) std::cerr << "truncated disk record: " << path_ << "\n";
            return false;
        }
        return true;
    }
private:
    std::string path_;
    std::ifstream input_;
    bool past_window_;
};

struct ReaderHead { std::size_t reader; WorkItem item; };
struct Earlier {
    bool operator()(const ReaderHead& a, const ReaderHead& b) const {
        if (a.item.header.monotonic_ns != b.item.header.monotonic_ns)
            return a.item.header.monotonic_ns > b.item.header.monotonic_ns;
        return a.item.header.arrival_index > b.item.header.arrival_index;
    }
};

std::size_t worker_for(const std::string& security, std::size_t workers) {
    return static_cast<std::size_t>(std::strtoul(security.c_str(), 0, 10)) % workers;
}

class InputMerger {
public:
    InputMerger(const Config& config, const std::vector<std::string>& files,
                const sse_auction59::StaticMetadataMap* metadata,
                std::vector<AuctionWorker*>* workers, Metrics* metrics)
        : config_(config), metadata_(metadata), workers_(workers), metrics_(metrics) {
        for (std::size_t i = 0; i < files.size(); ++i) readers_.push_back(new InputReader(files[i]));
    }
    ~InputMerger() { for (std::size_t i = 0; i < readers_.size(); ++i) delete readers_[i]; }
    bool good() const {
        for (std::size_t i = 0; i < readers_.size(); ++i) if (!readers_[i]->good()) return false;
        return true;
    }
    void run() {
        std::priority_queue<ReaderHead, std::vector<ReaderHead>, Earlier> heads;
        std::vector<bool> have(readers_.size(), false);
        while (!g_stop.load()) {
            if (config_.follow && local_time_micros() >= config_.stop_time_micros) break;
            bool progress = false, all_past = true;
            for (std::size_t i = 0; i < readers_.size(); ++i) {
                if (!readers_[i]->past_window()) all_past = false;
                if (have[i] || readers_[i]->past_window()) continue;
                ReaderHead head;
                if (next_decoded(i, &head.item)) {
                    head.reader = i; heads.push(std::move(head)); have[i] = true; progress = true;
                }
            }
            if (!config_.follow && all_past && heads.empty()) break;
            if (heads.empty()) {
                if (!config_.follow) break;
                usleep(5000); continue;
            }
            if (config_.follow && !progress) {
                const std::uint64_t now = monotonic_ns();
                if (heads.top().item.header.monotonic_ns + 20000000ULL > now) { usleep(500); continue; }
            }
            ReaderHead head = heads.top(); heads.pop(); have[head.reader] = false;
            const std::uint64_t dispatch_started = monotonic_ns();
            const std::size_t worker = worker_for(head.item.event.security_id, workers_->size());
            if (!(*workers_)[worker]->queue()->push(std::move(head.item))) break;
            metrics_->dispatch_ns.fetch_add(monotonic_ns() - dispatch_started);
            metrics_->dispatched_ticks.fetch_add(1);
        }
        for (std::size_t i = 0; i < workers_->size(); ++i) (*workers_)[i]->queue()->close();
    }
private:
    bool next_decoded(std::size_t index, WorkItem* output) {
        RawRecord raw;
        while (readers_[index]->next(&raw, config_.follow)) {
            metrics_->disk_records.fetch_add(1);
            const std::uint64_t started = monotonic_ns();
            sse_live::TickEvent event;
            std::string error;
            if (!sse_live::decode_primary_tick(&raw.payload[0], raw.payload.size(), &event, &error)) {
                metrics_->decode_ns.fetch_add(monotonic_ns() - started);
                metrics_->filtered_records.fetch_add(1); continue;
            }
            metrics_->decode_ns.fetch_add(monotonic_ns() - started);
            metrics_->decoded_ticks.fetch_add(1);
            const std::uint32_t date = sse_live::local_trading_date(raw.header.realtime_ns);
            if (date != config_.date) { metrics_->filtered_records.fetch_add(1); continue; }
            if (!config_.follow && event.time_of_day_micros >= kWindowEnd) {
                readers_[index]->mark_past_window(); return false;
            }
            if (event.time_of_day_micros < kWindowStart || event.time_of_day_micros >= kWindowEnd) {
                metrics_->filtered_records.fetch_add(1); continue;
            }
            if (metadata_->find(event.security_id) == metadata_->end()) {
                metrics_->static_missing_events.fetch_add(1);
                metrics_->filtered_records.fetch_add(1); continue;
            }
            if (event.event_type == 'S') metrics_->status_events.fetch_add(1);
            const std::uint64_t now = monotonic_ns();
            if (now >= raw.header.monotonic_ns) atomic_max(&metrics_->max_processing_lag_ns,
                                                            now - raw.header.monotonic_ns);
            output->header = raw.header; output->event = event; return true;
        }
        return false;
    }
    Config config_;
    std::vector<InputReader*> readers_;
    const sse_auction59::StaticMetadataMap* metadata_;
    std::vector<AuctionWorker*>* workers_;
    Metrics* metrics_;
};

bool replace_file(const std::string& temporary, const std::string& destination) {
    if (std::rename(temporary.c_str(), destination.c_str()) != 0) {
        std::cerr << "rename failed " << temporary << " -> " << destination
                  << ": " << std::strerror(errno) << "\n"; return false;
    }
    return true;
}

std::string quality_names(std::uint64_t mask) {
    if (mask == 0) return "OK";
    std::ostringstream output;
    bool first = true;
    for (std::size_t bit = 0; bit < 20U; ++bit) if (mask & (1ULL << bit)) {
        if (!first) output << '|'; first = false; output << sse_auction59::quality_reason_name(bit);
    }
    return output.str();
}

bool write_outputs(const Config& config,
                   const sse_auction59::StaticMetadataMap& metadata,
                   std::map<std::string, sse_auction59::AuctionResult>* results,
                   std::uint64_t* hard_valid, std::uint64_t* write_ns) {
    const std::uint64_t started = monotonic_ns();
    for (sse_auction59::StaticMetadataMap::const_iterator it = metadata.begin(); it != metadata.end(); ++it) {
        if (results->find(it->first) == results->end()) {
            sse_auction59::AuctionAccumulator missing(it->first, it->second);
            (*results)[it->first] = missing.finalize();
        }
    }
    const std::string factor_tmp = config.output_csv + ".tmp." + std::to_string(getpid());
    const std::string quality_tmp = config.quality_csv + ".tmp." + std::to_string(getpid());
    std::ofstream factors(factor_tmp.c_str()), quality(quality_tmp.c_str());
    if (!factors || !quality) return false;
    factors << "security_id";
    for (std::size_t i = 0; i < sse_auction59::kAuction59FactorCount; ++i)
        factors << ',' << sse_auction59::auction59_factor_name(i);
    factors << '\n';
    quality << "security_id,hard_valid,quality_reason_mask,quality_reasons,has_status,status_channel_no,"
               "opening_auction_last_app_seq,has_auction_match,source_order_rows,source_trade_rows,"
               "conservation_error_count,pre_close_price_tick,lower_limit_tick,upper_limit_tick,"
               "observed_auction_price_tick,observed_auction_qty,reconstructed_auction_price_tick,"
               "reconstructed_auction_qty,canonical_valid_mask,projected_valid_mask\n";
    *hard_valid = 0;
    for (std::map<std::string, sse_auction59::AuctionResult>::const_iterator it = results->begin();
         it != results->end(); ++it) {
        const sse_auction59::AuctionResult& row = it->second;
        if (row.hard_valid) {
            ++*hard_valid; factors << row.security_id;
            for (std::size_t i = 0; i < sse_auction59::kAuction59FactorCount; ++i)
                factors << ',' << std::setprecision(9) << row.factors[i];
            factors << '\n';
        }
        quality << row.security_id << ',' << (row.hard_valid ? 1 : 0) << ','
                << row.quality_reason_mask << ',' << quality_names(row.quality_reason_mask) << ','
                << (row.has_status ? 1 : 0) << ',' << row.status_channel_no << ','
                << row.opening_auction_last_app_seq << ',' << (row.has_auction_match ? 1 : 0) << ','
                << row.source_order_rows << ',' << row.source_trade_rows << ','
                << row.conservation_error_count << ',' << row.pre_close_price_tick << ','
                << row.lower_limit_tick << ',' << row.upper_limit_tick << ','
                << row.observed_auction_price_tick << ',' << row.observed_auction_qty << ','
                << row.reconstructed_auction_price_tick << ',' << row.reconstructed_auction_qty << ','
                << row.canonical_valid_mask << ',' << row.projected_valid_mask << '\n';
    }
    factors.flush(); quality.flush();
    if (!factors.good() || !quality.good()) return false;
    factors.close(); quality.close();
    if (!replace_file(factor_tmp, config.output_csv) || !replace_file(quality_tmp, config.quality_csv)) return false;
    *write_ns = monotonic_ns() - started; return true;
}

bool write_metrics(const Config& config, const Metrics& metrics, std::uint64_t elapsed,
                   std::uint64_t write_ns, std::size_t result_rows, std::uint64_t hard_valid) {
    const std::string temporary = config.metrics_json + ".tmp." + std::to_string(getpid());
    std::ofstream out(temporary.c_str());
    if (!out) return false;
    const std::uint64_t dispatched = metrics.dispatched_ticks.load();
    out << "{\n"
        << "  \"mode\": \"" << (config.follow ? "follow" : "replay") << "\",\n"
        << "  \"date\": " << config.date << ",\n"
        << "  \"workers\": " << config.workers << ",\n"
        << "  \"elapsed_ns\": " << elapsed << ",\n"
        << "  \"disk_records\": " << metrics.disk_records.load() << ",\n"
        << "  \"decoded_ticks\": " << metrics.decoded_ticks.load() << ",\n"
        << "  \"filtered_records\": " << metrics.filtered_records.load() << ",\n"
        << "  \"dispatched_ticks\": " << dispatched << ",\n"
        << "  \"status_events\": " << metrics.status_events.load() << ",\n"
        << "  \"static_missing_events\": " << metrics.static_missing_events.load() << ",\n"
        << "  \"result_rows\": " << result_rows << ",\n"
        << "  \"hard_valid_rows\": " << hard_valid << ",\n"
        << "  \"decode_ns\": " << metrics.decode_ns.load() << ",\n"
        << "  \"dispatch_ns\": " << metrics.dispatch_ns.load() << ",\n"
        << "  \"update_ns\": " << metrics.update_ns.load() << ",\n"
        << "  \"factor_ns\": " << metrics.factor_ns.load() << ",\n"
        << "  \"write_ns\": " << write_ns << ",\n"
        << "  \"max_processing_lag_ns\": " << metrics.max_processing_lag_ns.load() << ",\n"
        << "  \"avg_decode_ns_per_disk_record\": "
        << (metrics.disk_records.load() ? metrics.decode_ns.load() / metrics.disk_records.load() : 0) << ",\n"
        << "  \"avg_dispatch_ns_per_tick\": " << (dispatched ? metrics.dispatch_ns.load() / dispatched : 0) << ",\n"
        << "  \"avg_update_ns_per_tick\": " << (dispatched ? metrics.update_ns.load() / dispatched : 0) << ",\n"
        << "  \"avg_factor_ns_per_result\": " << (result_rows ? metrics.factor_ns.load() / result_rows : 0) << "\n"
        << "}\n";
    out.close(); return replace_file(temporary, config.metrics_json);
}

}  // namespace

int main(int argc, char** argv) {
    Config config;
    if (!parse_args(argc, argv, &config)) { usage(argv[0]); return 2; }
    signal(SIGINT, on_signal); signal(SIGTERM, on_signal);
    sse_auction59::StaticMetadataMap metadata;
    std::string error;
    const bool static_ok = sse_auction59::load_static_metadata_csv(
        config.static_csv, config.date, &metadata, &error);
    if (!static_ok) {
        std::cerr << error << "\n"; return 3;
    }
    const std::vector<std::string> files = record_files(config.tick_root);
    if (files.empty()) { std::cerr << "no channel records under " << config.tick_root << "\n"; return 4; }
    Metrics metrics;
    ResultCollector collector;
    std::vector<AuctionWorker*> workers;
    std::vector<std::thread> threads;
    for (std::size_t i = 0; i < config.workers; ++i) {
        workers.push_back(new AuctionWorker(&metadata, &collector, &metrics));
        threads.push_back(std::thread(&AuctionWorker::run, workers.back()));
    }
    const std::uint64_t started = monotonic_ns();
    InputMerger merger(config, files, &metadata, &workers, &metrics);
    if (!merger.good()) { std::cerr << "cannot open one or more tick files\n"; return 5; }
    merger.run();
    for (std::size_t i = 0; i < threads.size(); ++i) threads[i].join();
    for (std::size_t i = 0; i < workers.size(); ++i) delete workers[i];
    std::map<std::string, sse_auction59::AuctionResult> results = collector.take();
    std::uint64_t hard_valid = 0, write_ns = 0;
    if (!write_outputs(config, metadata, &results, &hard_valid, &write_ns)) {
        std::cerr << "failed to write Auction59 outputs\n"; return 6;
    }
    const std::uint64_t elapsed = monotonic_ns() - started;
    if (!write_metrics(config, metrics, elapsed, write_ns, results.size(), hard_valid)) {
        std::cerr << "failed to write Auction59 metrics\n"; return 7;
    }
    std::cerr << "auction59 live stopped ticks=" << metrics.dispatched_ticks.load()
              << " statuses=" << metrics.status_events.load() << " rows=" << results.size()
              << " hard_valid=" << hard_valid << "\n";
    return hard_valid > 0 ? 0 : 8;
}
