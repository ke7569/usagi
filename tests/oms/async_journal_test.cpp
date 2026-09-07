#include "common/oms/AsyncJournal.h"
#include "common/oms/Records.h"
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unistd.h>

namespace {
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
struct Temp {
    std::string dir, path;
    Temp() {
        char name[] = "/tmp/usagi-async-log-XXXXXX";
        char* created = ::mkdtemp(name); require(created != 0, "mkdtemp");
        dir = created; path = dir + "/journal";
    }
    ~Temp() { ::unlink(path.c_str()); ::rmdir(dir.c_str()); }
};
void test_ordered_drain_and_barrier() {
    Temp temp;
    {
        oms::AsyncJournal journal(temp.path, 2048);
        require(journal.replay(0), "initial replay");
        for (unsigned i = 0; i < 1000; ++i) require(journal.append(std::to_string(i), false), "async append");
        require(journal.sequence() == 1000, "accepted watermark");
        require(journal.sync() && journal.durable_sequence() == 1000 && journal.written_sequence() == 1000, "durable barrier");
        require(journal.append("last", false), "destructor drain record");
    }
    oms::Journal reader(temp.path);
    unsigned count = 0;
    require(reader.replay([&](const std::string& line) {
        const std::string expected = count == 1000 ? "last" : std::to_string(count);
        ++count; return line == expected;
    }) && count == 1001, "worker preserves order and drains at destruction");
}
void test_slow_sink_does_not_block_producer_and_queue_is_bounded() {
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false, release = false;
    const std::thread::id producer = std::this_thread::get_id();
    oms::AsyncJournal journal("", 2, [&](const std::string&) {
        require(std::this_thread::get_id() != producer, "sink ran on producer");
        std::unique_lock<std::mutex> lock(mutex);
        entered = true; changed.notify_all(); changed.wait(lock, [&]() { return release; });
    });
    require(journal.replay(0), "sink replay");
    require(journal.append("first", false), "first async sink record");
    {
        std::unique_lock<std::mutex> lock(mutex);
        changed.wait(lock, [&]() { return entered; });
    }
    const bool second = journal.append("second", false);
    const bool full = !journal.append("full", false) && !journal.healthy();
    {
        std::lock_guard<std::mutex> lock(mutex); release = true;
    }
    changed.notify_all();
    require(second && full, "slow logger never blocks producer; capacity failure explicit");
}
void test_printing_does_not_delay_wal_barrier() {
    Temp temp;
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false, release = false;
    oms::AsyncJournal journal(temp.path, 8, [&](const std::string&) {
        std::unique_lock<std::mutex> lock(mutex);
        entered = true; changed.notify_all(); changed.wait(lock, [&]() { return release; });
    });
    require(journal.replay(0), "WAL and sink replay");
    require(journal.append("first", true), "first durable intent");
    {
        std::unique_lock<std::mutex> lock(mutex); changed.wait(lock, [&]() { return entered; });
    }
    const bool second = journal.append("second", true);
    {
        std::lock_guard<std::mutex> lock(mutex); release = true;
    }
    changed.notify_all();
    require(second && journal.durable_sequence() == 2, "blocked text sink must not block WAL confirmation");
}
void test_worker_failure() {
    oms::AsyncJournal journal("", 8, [](const std::string&) { throw std::runtime_error("sink failure"); });
    require(journal.replay(0) && journal.append("record", false), "failing sink enqueue");
    require(!journal.sync() && !journal.healthy() && !journal.append("later", false), "worker failure closes producer");
    require(journal.error() == "sink failure", "worker failure observable");
}
void test_compact_roundtrip() {
    oms::Command command;
    command.scope.account.broker = "paper"; command.scope.account.account = "unit";
    command.scope.gateway = "g"; command.scope.source = 1; command.scope.day = 20260907; command.scope.epoch = 9;
    command.id = 7; command.intent.owner = "owner"; command.intent.intent_id = "intent";
    command.intent.signal_id = "signal"; command.intent.instrument = oms::Instrument{"SZE", "000001"};
    command.intent.quantity = 100; command.intent.price = 100000; command.intent.type = oms::OrderType::LimitThenCancel;
    command.intent.cancel_delay_ns = 1001000000;
    const std::string payload = oms::records::intent(100, command);
    const auto decoded = oms::records::decode(payload);
    require(decoded.at("type") == "intent" && decoded.at("data").at("intent").at("quantity") == 100 &&
        decoded.at("data").at("scope").at("epoch") == 9, "compact intent roundtrip");
    require(payload.size() < decoded.dump().size(), "compact record smaller than JSON");
    require(oms::records::decode(decoded.dump()) == decoded, "legacy JSON compatibility");
    const auto brief = nlohmann::json::parse(oms::records::format(payload));
    require(brief.at("id") == 7 && brief.at("quantity") == 100 && !brief.count("scope") &&
        !brief.count("cancel_delay_ns") && brief.at("signal_id") == "signal", "compact text retains order identity");
    for (unsigned mutation = 0; mutation < 3; ++mutation) {
        std::string malformed = payload;
        if (mutation == 0) malformed[3] = 99;
        else if (mutation == 1) malformed[4] = 99;
        else malformed.push_back('x');
        bool rejected = false;
        try { oms::records::decode(malformed); } catch (...) { rejected = true; }
        require(rejected, "invalid version, kind or trailing bytes accepted");
    }
    for (std::size_t size = 0; size < payload.size(); ++size) {
        bool rejected = false;
        try { oms::records::decode(payload.substr(0, size)); } catch (...) { rejected = true; }
        require(rejected, "truncated compact record accepted");
    }
    oms::Report report;
    report.scope = command.scope; report.id = command.id; report.instrument = command.intent.instrument;
    report.kind = oms::ReportKind::Trade; report.trade_id = "trade"; report.trade_quantity = 100;
    report.trade_price = 0; report.trade_fee = -1; report.error.raw_code = -2010;
    const auto trade = oms::records::decode(oms::records::report(101, report));
    require(trade["data"]["trade_fee"] == -1 && trade["data"]["trade_price"] == 0 &&
        trade["data"]["error"]["raw_code"] == -2010, "unknown price/fees and signed errors preserved");
    oms::SendResult send; send.disposition = oms::SendDisposition::Unknown; send.error.raw_code = -27;
    require(oms::records::decode(oms::records::send(102, 7, send, false))["data"]["error"]["raw_code"] == -27,
            "send result preserved");
}
}  // namespace
int main() {
    try {
        test_ordered_drain_and_barrier();
        test_slow_sink_does_not_block_producer_and_queue_is_bounded();
        test_printing_does_not_delay_wal_barrier();
        test_worker_failure(); test_compact_roundtrip();
        std::cout << "async_journal_test: PASS\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
