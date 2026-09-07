#include "common/oms/Oms.h"
#include "common/oms/Profile.h"
#include "common/oms/Records.h"
#include "third_party/nlohmann/json.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <sched.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>

namespace {
using namespace oms;
typedef nlohmann::json Json;
typedef std::chrono::steady_clock Clock;
typedef Clock::time_point Point;
const Instrument kInstrument = {"SZE", "000001"};
const Money kPrice = 100000;
const Quantity kQuantity = 100;
const std::size_t kWarmup = 32;

void require(bool ok, const std::string& message) {
    if (!ok) throw std::runtime_error(message);
}
long long elapsed(Point begin, Point end) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count();
}
const char* build_name() {
#ifdef USAGI_OMS_PROFILE
    return "profiled";
#else
    return "normal";
#endif
}

struct Options {
    std::size_t repetitions = 3, orders = 10000, durable_orders = 100;
    int cpu = -1;
    bool audit_text = false;
    std::string mode = "both", journal_root = "/tmp", focus;
};

std::size_t integer(const std::string& name, const char* text, std::size_t maximum) {
    const std::string input(text);
    if (input.empty() || input.find_first_not_of("0123456789") != std::string::npos)
        throw std::runtime_error(name + " requires an unsigned integer");
    char* end = 0;
    errno = 0;
    const unsigned long long value = std::strtoull(text, &end, 10);
    if (errno || *end || value > maximum) throw std::runtime_error(name + " exceeds its limit");
    return static_cast<std::size_t>(value);
}
Options parse(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string key(argv[i]);
        if (++i == argc) throw std::runtime_error("missing value for " + key);
        if (key == "--orders") options.orders = integer(key, argv[i], 100000);
        else if (key == "--durable-orders") options.durable_orders = integer(key, argv[i], 10000);
        else if (key == "--repetitions") options.repetitions = integer(key, argv[i], 20);
        else if (key == "--cpu") options.cpu = static_cast<int>(integer(key, argv[i], CPU_SETSIZE - 1));
        else if (key == "--journal-root") options.journal_root = argv[i];
        else if (key == "--mode") options.mode = argv[i];
        else if (key == "--focus") options.focus = argv[i];
        else if (key == "--audit-text") options.audit_text = integer(key, argv[i], 1) != 0;
        else throw std::runtime_error("unknown option: " + key);
    }
    require(options.orders && options.durable_orders && options.repetitions, "counts must be positive");
    require(options.mode == "both" || options.mode == "memory" || options.mode == "durable",
            "--mode must be both, memory or durable");
    require(!options.journal_root.empty() && options.journal_root[0] == '/', "--journal-root must be absolute");
    if (!options.focus.empty()) {
#ifdef USAGI_OMS_PROFILE
        bool valid = false;
        for (std::size_t stage = 0; stage < profile::kStages; ++stage)
            valid = valid || options.focus == profile::stage_name(static_cast<profile::Stage>(stage));
        require(valid, "unknown --focus stage");
#else
        throw std::runtime_error("--focus requires the profiled benchmark");
#endif
    }
    return options;
}

class TempJournal {
public:
    explicit TempJournal(const std::string& root) {
        const std::string pattern = root + "/usagi-oms-benchmark-XXXXXX";
        std::vector<char> name(pattern.begin(), pattern.end());
        name.push_back(0);
        char* created = ::mkdtemp(name.data());
        require(created != 0, "cannot create temporary journal directory");
        directory_ = created;
    }
    ~TempJournal() {
        ::unlink((directory_ + "/audit.jsonl").c_str());
        ::unlink(path().c_str());
        ::unlink((directory_ + "/oms-7061706572-62656e63686d61726b.lock").c_str());
        ::rmdir(directory_.c_str());
    }
    std::string path() const { return directory_ + "/benchmark.journal"; }
    std::string text_path() const { return directory_ + "/audit.jsonl"; }
private:
    std::string directory_;
};

InstrumentRules rules() {
    InstrumentRules r;
    r.tick = 100; r.lot = 100; r.lower_price = 90000; r.upper_price = 110000;
    r.max_order_quantity = 1000000; r.max_position = 100000000;
    r.max_order_notional = 1000000000000LL;
    return r;
}
Config config(std::size_t count, const std::string& journal) {
    Config c;
    c.scope.account.broker = "paper"; c.scope.account.account = "benchmark";
    c.scope.gateway = "benchmark"; c.scope.day = 20260904; c.scope.source = 1;
    c.instance = "oms-benchmark"; c.enabled = true;
    c.ownership = journal.empty() ? OwnershipMode::Simulation : OwnershipMode::ExclusiveLocal;
    c.journal_path = journal;
    if (!journal.empty()) c.lock_directory = journal.substr(0, journal.find_last_of('/'));
    c.limits.max_orders = count + kWarmup;
    c.limits.max_pending = count + kWarmup;
    c.limits.max_actions = count + kWarmup;
    c.limits.max_audit_events = 1024;
    c.limits.new_orders_per_window = count + kWarmup;
    c.limits.combined_per_window = count + kWarmup;
    c.limits.rate_window_ns = 1000000000000LL;
    c.instruments[kInstrument] = rules();
    return c;
}
Snapshot snapshot(const Scope& scope) {
    Snapshot s;
    s.scope = scope; s.token = 1; s.free_cash = 1000000000000000LL;
    s.account_success = s.positions_success = s.orders_success = s.trades_success = true;
    s.all_day_orders = s.all_day_trades = true;
    SnapshotPosition p;
    p.instrument = kInstrument; p.total = p.free_sellable = 10000000;
    s.positions.push_back(p);
    return s;
}
std::vector<Intent> intents(std::size_t count) {
    std::vector<Intent> result;
    result.reserve(count + kWarmup);
    for (std::size_t i = 0; i < count + kWarmup; ++i) {
        Intent value;
        value.owner = "benchmark"; value.intent_id = "intent-" + std::to_string(i);
        value.signal_id = "signal-" + std::to_string(i); value.instrument = kInstrument;
        value.side = Side::Buy; value.type = OrderType::LimitThenCancel;
        value.price = kPrice; value.quantity = kQuantity; value.cancel_delay_ns = 100000000000LL;
        result.push_back(value);
    }
    return result;
}
long long resident_kb() {
    std::ifstream input("/proc/self/statm");
    unsigned long long pages = 0, resident = 0;
    const long page_size = ::sysconf(_SC_PAGESIZE);
    if (!(input >> pages >> resident) || page_size <= 0) return -1;
    return static_cast<long long>(resident * static_cast<unsigned long long>(page_size) / 1024);
}

struct Observed {
    OrderId id = 0;
    Money price = 0;
    Quantity quantity = 0;
    Side side = Side::Buy;
    OrderType type = OrderType::Limit;
    bool cancel = false;
};
struct Observer {
    std::vector<Observed> commands;
    std::size_t count = 0;
    explicit Observer(std::size_t capacity) : commands(capacity) {}
    void operator()(const Command& command) {
        if (count < commands.size()) {
            Observed& value = commands[count];
            value.id = command.id; value.price = command.intent.price; value.quantity = command.intent.quantity;
            value.side = command.intent.side; value.type = command.intent.type; value.cancel = command.cancel;
        }
        ++count;
    }
    void verify(std::size_t expected) const {
        require(count == expected, "backend observer count mismatch");
        for (std::size_t i = 0; i < expected; ++i) {
            const Observed& o = commands[i];
            require(o.id == i + 1 && o.price == kPrice && o.quantity == kQuantity &&
                o.side == Side::Buy && o.type == OrderType::LimitThenCancel && !o.cancel,
                "outbound command changed");
        }
    }
};

class TimedPaperBackend : public PaperBackend {
public:
    explicit TimedPaperBackend(Observer& observer)
        : PaperBackend([&observer](const Command& command) { observer(command); }) {}
    SendResult submit(const Command& command) override {
        entry = Clock::now();
        SendResult result = PaperBackend::submit(command);
        returned = Clock::now();
        return result;
    }
    Point entry, returned;
};

enum class Measurement { Total, Coarse, Detailed, Focused };
const char* measurement_name(Measurement value) {
    return value == Measurement::Total ? "total" : value == Measurement::Coarse ? "coarse" :
        value == Measurement::Detailed ? "detailed" : "focused";
}
Json distribution(const std::vector<long long>& input) {
    require(!input.empty(), "empty timing distribution");
    std::vector<long long> sorted(input);
    std::sort(sorted.begin(), sorted.end());
    long double total = 0;
    for (long long value : input) total += value;
    const auto rank = [&](double fraction) { return sorted[static_cast<std::size_t>(fraction * (sorted.size() - 1))]; };
    return Json{{"p50", rank(.50)}, {"p95", rank(.95)}, {"p99", rank(.99)},
        {"max", sorted.back()}, {"mean", static_cast<double>(total / input.size())}};
}
struct Timings {
    std::vector<long long> total, pre, backend, post;
    explicit Timings(std::size_t count) : total(count), pre(count), backend(count), post(count) {}
    void record(std::size_t index, Point start, Point end, const TimedPaperBackend* timed) {
        total[index] = elapsed(start, end);
        require(total[index] >= 0, "outer clock regressed");
        if (timed) {
            pre[index] = elapsed(start, timed->entry);
            backend[index] = elapsed(timed->entry, timed->returned);
            post[index] = elapsed(timed->returned, end);
            require(pre[index] >= 0 && backend[index] >= 0 && post[index] >= 0 &&
                pre[index] + backend[index] + post[index] == total[index], "backend timestamps do not partition total");
        }
    }
    Json json(bool coarse) const {
        Json value = {{"total", distribution(total)}};
        if (coarse) {
            value["pre_backend"] = distribution(pre);
            value["backend"] = distribution(backend);
            value["post_backend"] = distribution(post);
        }
        return value;
    }
};

Json state(Engine& engine, std::size_t expected) {
    const AccountView a = engine.account();
    Position p;
    require(engine.position(kInstrument, &p), "missing position");
    require(a.ready && !a.anomalies && !a.rejections && a.orders == expected && a.pending_orders == expected &&
        a.timers == expected && a.pending_actions == 0 && a.admissions == expected &&
        a.cash_reserved == static_cast<Money>(expected) * kPrice * kQuantity &&
        p.total == 10000000 && p.sellable == 10000000 && p.working_buy == static_cast<Quantity>(expected) * kQuantity &&
        p.working_sell == 0, "final account state changed");
    std::uint64_t digest = 14695981039346656037ULL;
    const auto feed = [&](std::uint64_t value) {
        for (unsigned byte = 0; byte < 8; ++byte) {
            digest ^= (value >> (byte * 8)) & 255U; digest *= 1099511628211ULL;
        }
    };
    for (std::size_t i = 0; i < expected; ++i) {
        OrderView o;
        require(engine.order(i + 1, &o), "registered ID missing");
        require(o.command.id == i + 1 && o.command.intent.owner == "benchmark" &&
            o.command.intent.intent_id == "intent-" + std::to_string(i) &&
            o.command.intent.signal_id == "signal-" + std::to_string(i) &&
            o.command.intent.instrument == kInstrument && o.command.scope == a.scope &&
            o.command.intent.price == kPrice && o.command.intent.quantity == kQuantity &&
            o.command.intent.type == OrderType::LimitThenCancel && o.command.intent.side == Side::Buy &&
            o.filled == 0 && o.working == kQuantity && o.cash_reserved == kPrice * kQuantity &&
            o.state == OrderState::Submitted && o.send == SendDisposition::Submitted &&
            !o.terminal && !o.cancel_requested && !o.cancel_attempts, "registered order changed");
        feed(o.command.id); feed(o.working); feed(o.cash_reserved);
    }
    std::ostringstream encoded;
    encoded << std::hex << digest;
    return Json{{"order_digest", encoded.str()}, {"orders", a.orders}, {"pending", a.pending_orders},
        {"timers", a.timers}, {"actions", a.pending_actions}, {"cash", a.cash_balance},
        {"reserved", a.cash_reserved}, {"journal_records", a.audit_sequence}, {"position", p.total},
        {"sellable", p.sellable}, {"working_buy", p.working_buy},
        {"audit_retained", engine.audit_events().size()}};
}

#ifdef USAGI_OMS_PROFILE
struct Details {
    std::vector<std::vector<long long> > inclusive, exclusive;
    std::vector<std::uint64_t> calls;
    explicit Details(std::size_t count)
        : inclusive(profile::kStages, std::vector<long long>(count)),
          exclusive(profile::kStages, std::vector<long long>(count)), calls(profile::kStages, 0) {}
    void record(std::size_t index, const profile::Sample& sample, bool complete) {
        std::uint64_t total = 0;
        for (std::size_t stage = 0; stage < profile::kStages; ++stage) {
            inclusive[stage][index] = sample.inclusive_ns[stage];
            exclusive[stage][index] = sample.exclusive_ns[stage];
            calls[stage] += sample.calls[stage]; total += sample.exclusive_ns[stage];
        }
        require(!complete || total == sample.inclusive_ns[static_cast<std::size_t>(profile::Stage::Submit)],
                "exclusive stage accounting does not sum to submit scope");
    }
    Json json() const {
        Json value = Json::object();
        for (std::size_t i = 0; i < profile::kStages; ++i)
            value[profile::stage_name(static_cast<profile::Stage>(i))] =
                Json{{"calls", calls[i]}, {"inclusive_ns", distribution(inclusive[i])},
                    {"exclusive_ns", distribution(exclusive[i])}};
        return value;
    }
};
#endif

Json run_engine(const Options& options, bool durable, Measurement measurement, unsigned repetition) {
    const std::size_t count = durable ? options.durable_orders : options.orders;
    const std::vector<Intent> input = intents(count);
    std::unique_ptr<TempJournal> journal;
    if (durable || options.audit_text) journal.reset(new TempJournal(options.journal_root));
    Config settings = config(count, durable ? journal->path() : std::string());
    std::shared_ptr<std::ofstream> audit_output;
    std::atomic<std::uint64_t> audit_records(0), audit_bytes(0);
    if (options.audit_text) {
        audit_output.reset(new std::ofstream(journal->text_path()));
        require(audit_output->good(), "cannot open audit output");
        settings.audit_sink = [audit_output, &audit_records, &audit_bytes](const std::string& payload) {
            const std::string line = records::format(payload);
            *audit_output << line << '\n';
            require(audit_output->good(), "audit output failed");
            audit_records.fetch_add(1); audit_bytes.fetch_add(line.size() + 1);
        };
    }
    Observer observer(input.size());
    std::shared_ptr<Backend> backend;
    std::shared_ptr<TimedPaperBackend> timed;
    if (measurement == Measurement::Total || measurement == Measurement::Focused) {
        backend.reset(new PaperBackend([&observer](const Command& c) { observer(c); }));
    } else {
        timed.reset(new TimedPaperBackend(observer)); backend = timed;
    }
    std::shared_ptr<Engine> engine = Engine::create(settings, backend);
    require(engine->start_epoch(1) && engine->begin_reconcile(1, false) &&
        engine->complete_snapshot(snapshot(engine->scope())), "account setup failed");
    for (std::size_t i = 0; i < kWarmup; ++i) require(engine->submit(input[i]).accepted, "warmup submit failed");
    Timings timings(count);
#ifdef USAGI_OMS_PROFILE
    std::unique_ptr<Details> details;
    profile::Mask mask = profile::kAllStages;
    if (measurement == Measurement::Focused) {
        for (std::size_t stage = 0; stage < profile::kStages; ++stage)
            if (options.focus == profile::stage_name(static_cast<profile::Stage>(stage)))
                mask = profile::only(static_cast<profile::Stage>(stage));
    }
    if (measurement == Measurement::Detailed || measurement == Measurement::Focused) details.reset(new Details(count));
#endif
    for (std::size_t i = 0; i < count; ++i) {
        Point start, end;
        SubmitResult result;
#ifdef USAGI_OMS_PROFILE
        profile::Sample sample;
        if (details) {
            profile::Session session(sample, mask);
            start = Clock::now(); result = engine->submit(input[i + kWarmup]); end = Clock::now();
        } else
#endif
        {
            start = Clock::now(); result = engine->submit(input[i + kWarmup]); end = Clock::now();
        }
        require(result.accepted && result.id == i + kWarmup + 1, "measured order rejected or reordered");
        timings.record(i, start, end, timed.get());
#ifdef USAGI_OMS_PROFILE
        if (details) details->record(i, sample, measurement == Measurement::Detailed);
#endif
    }
    observer.verify(input.size());
    Json output = {{"kind", "run"}, {"build", build_name()}, {"repetition", repetition},
        {"path", durable ? "durable" : "memory"}, {"measurement", measurement_name(measurement)},
        {"count", count}, {"warmup", kWarmup}, {"cpu", ::sched_getcpu()},
        {"resident_kb", resident_kb()}, {"latency_ns", timings.json(timed != 0)},
        {"state", state(*engine, input.size())}};
    if (durable) output["journal_root"] = options.journal_root;
    output["audit_text"] = options.audit_text;
    if (options.audit_text) {
        const AccountView account = engine->account();
        const Point drain_start = Clock::now();
        engine.reset();
        audit_output->flush();
        require(audit_output->good() && audit_records.load() == account.audit_sequence, "audit drain/count mismatch");
        output["audit"] = Json{{"records", audit_records.load()}, {"bytes", audit_bytes.load()},
            {"drain_ns", elapsed(drain_start, Clock::now())}};
    }
    if (measurement == Measurement::Focused) output["focus"] = options.focus;
#ifdef USAGI_OMS_PROFILE
    if (details) output["stages"] = details->json();
#endif
    return output;
}

Json run_direct(const Options& options, unsigned repetition) {
    const std::vector<Intent> input = intents(options.orders);
    std::vector<Command> commands(input.size());
    for (std::size_t i = 0; i < input.size(); ++i) {
        commands[i].id = i + 1; commands[i].intent = input[i];
    }
    Observer observer(input.size());
    PaperBackend backend([&observer](const Command& c) { observer(c); });
    for (std::size_t i = 0; i < kWarmup; ++i) backend.submit(commands[i]);
    Timings timings(options.orders);
    for (std::size_t i = 0; i < options.orders; ++i) {
        const Point start = Clock::now();
        const SendResult result = backend.submit(commands[i + kWarmup]);
        const Point end = Clock::now();
        require(result.disposition == SendDisposition::Submitted, "direct submit failed");
        timings.record(i, start, end, 0);
    }
    observer.verify(input.size());
    return Json{{"kind", "run"}, {"build", build_name()}, {"repetition", repetition},
        {"path", "direct_paper"}, {"measurement", "total"}, {"count", options.orders},
        {"warmup", kWarmup}, {"latency_ns", timings.json(false)}};
}

Json metadata(const Options& options) {
    cpu_set_t allowed;
    CPU_ZERO(&allowed);
    require(::sched_getaffinity(0, sizeof(allowed), &allowed) == 0, "cannot read CPU affinity");
    Json cpus = Json::array();
    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) if (CPU_ISSET(cpu, &allowed)) cpus.push_back(cpu);
    std::vector<long long> samples(1000);
    for (std::size_t i = 0; i < samples.size(); ++i) {
        const Point start = Clock::now(), end = Clock::now();
        samples[i] = elapsed(start, end);
    }
    return Json{{"kind", "metadata"}, {"build", build_name()}, {"unit", "ns"},
        {"clock", "steady_clock"}, {"clock_pair", distribution(samples)}, {"affinity", cpus},
        {"cpu", ::sched_getcpu()}, {"orders", options.orders}, {"durable_orders", options.durable_orders},
        {"repetitions", options.repetitions}, {"journal_root", options.journal_root},
        {"focus", options.focus},
        {"audit_text", options.audit_text},
        {"intent_construction_timed", false}, {"timing_overhead_subtracted", false}};
}
}  // namespace

int main(int argc, char** argv) {
    try {
        const Options options = parse(argc, argv);
        if (options.cpu >= 0) {
            cpu_set_t affinity; CPU_ZERO(&affinity); CPU_SET(options.cpu, &affinity);
            require(::sched_setaffinity(0, sizeof(affinity), &affinity) == 0, "requested CPU unavailable");
        }
        std::cout << metadata(options).dump() << '\n';
        for (unsigned rep = 0; rep < options.repetitions; ++rep) {
            if (options.mode != "durable") std::cout << run_direct(options, rep).dump() << '\n';
            for (bool durable : {false, true}) {
                if ((durable && options.mode == "memory") || (!durable && options.mode == "durable")) continue;
                Json expected;
                const std::vector<Measurement> measurements = options.focus.empty()
                    ? std::vector<Measurement>{Measurement::Total, Measurement::Coarse, Measurement::Detailed}
                    : std::vector<Measurement>{Measurement::Total, Measurement::Focused};
                for (Measurement measurement : measurements) {
#ifndef USAGI_OMS_PROFILE
                    if (measurement == Measurement::Detailed) continue;
#endif
                    Json row = run_engine(options, durable, measurement, rep);
                    if (expected.is_null()) expected = row.at("state");
                    else require(row.at("state") == expected, "measurement probes changed business state");
                    std::cout << row.dump() << '\n';
                }
            }
        }
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "oms_benchmark: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
