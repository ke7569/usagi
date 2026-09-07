#include "MDEngineSSE.h"
#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <sstream>
#include <sched.h>

WC_NAMESPACE_START
namespace {
std::string get_string(const json& j, const char* key, const std::string& d = std::string()) {
    return j.find(key) != j.end() && j[key].is_string() ? j[key].get<std::string>() : d;
}
int get_int(const json& j, const char* key, int d) {
    return j.find(key) != j.end() && j[key].is_number() ? j[key].get<int>() : d;
}
int receive_cpu(const json& config) {
    const json::const_iterator cpu = config.find("cpu");
    if (cpu == config.end()) return -1;
    if (!cpu->is_number_integer() || *cpu < 0 || *cpu >= CPU_SETSIZE)
        throw std::runtime_error("SSE channel cpu must be a non-negative CPU index");
    return cpu->get<int>();
}
}

MDEngineSSE::MDEngineSSE() : IMDEngine(89), filter_enabled_(false), connected_(false), logged_in_(false), running_(false) {}
MDEngineSSE::~MDEngineSSE() { logout(); }

void MDEngineSSE::init() {
    // IEngine::initialize() invokes the virtual init() and then starts
    // logging; the inherited KfLogPtr logger is null until an engine sets it
    // (the framework's IMDEngine::init() in this runtime is a no-op).  Set it
    // exactly like the validated TD engine before the first log statement.
    logger = yijinjing::KfLog::getLogger("MDEngineSSE.sse_fpga");
    IMDEngine::init();
    KF_LOG_INFO(logger, "[MDEngineSSE] init source=89 name=sse_fpga");
}

void MDEngineSSE::load(const json& config) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (running_) throw std::runtime_error("cannot reload SSE MD while running");
    channels_.clear();
    seen_keys_.clear();
    filter_enabled_ = config.find("use_subscribe_filter") != config.end()
        ? config["use_subscribe_filter"].get<bool>() : false;
    const json* list = 0;
    if (config.find("channels") != config.end() && config["channels"].is_array()) list = &config["channels"];
    if (list && !list->empty()) {
        for (json::const_iterator it = list->begin(); it != list->end(); ++it) {
            deepwin_market_data::ChannelSpec c;
            c.name = get_string(*it, "name", "sse");
            c.group = get_string(*it, "group", get_string(*it, "multicast_ip"));
            c.port = get_int(*it, "port", get_int(*it, "multicast_port", 0));
            c.interface_ip = get_string(*it, "interface_ip", get_string(*it, "iface_ip", "0.0.0.0"));
            c.receive_cpu = receive_cpu(*it);
            if (c.group.empty() || c.port <= 0) throw std::runtime_error("invalid SSE channel");
            channels_.push_back(c);
        }
    } else {
        deepwin_market_data::ChannelSpec c;
        c.name = "sse"; c.group = get_string(config, "group", get_string(config, "multicast_ip"));
        c.port = get_int(config, "port", get_int(config, "multicast_port", 0));
        c.interface_ip = get_string(config, "interface_ip", get_string(config, "iface_ip", "0.0.0.0"));
        c.receive_cpu = receive_cpu(config);
        if (c.group.empty() || c.port <= 0) throw std::runtime_error("invalid SSE channel");
        channels_.push_back(c);
    }
    symbols_.clear();
    if (config.find("symbols") != config.end() && config["symbols"].is_array())
        for (json::const_iterator it = config["symbols"].begin(); it != config["symbols"].end(); ++it)
            if (it->is_string()) symbols_.push_back(it->get<std::string>());
}

void MDEngineSSE::connect(long) { connected_ = !channels_.empty(); }
void MDEngineSSE::login(long) {
    if (!connected_) connect(0);
    if (!connected_ || running_) return;
    if (worker_.joinable()) worker_.join();
    std::vector<int> requested;
    for (const deepwin_market_data::ChannelSpec& channel : channels_) requested.push_back(channel.receive_cpu);
    if (!cpu_lease_.acquire(requested, &error_)) throw std::runtime_error(error_);
    for (std::size_t i = 0; i < channels_.size(); ++i) {
        const sse_cpu::Cpu& cpu = cpu_lease_.cpus()[i];
        KF_LOG_INFO(logger, "[SSE affinity] channel=" << channels_[i].name << " cpu=" << cpu.id << " l3=" << cpu.l3);
    }
    running_ = true; logged_in_ = true;
    worker_ = std::thread(&MDEngineSSE::run, this);
}
void MDEngineSSE::logout() {
    running_ = false;
    runtime_.stop();
    if (worker_.joinable()) worker_.join();
    cpu_lease_.release();
    logged_in_ = false; connected_ = false;
}
void MDEngineSSE::release_api() { logout(); }

void MDEngineSSE::subscribeMarketData(const std::vector<std::string>& i, const std::vector<std::string>&) { subscribeL2MD(i, std::vector<std::string>()); }
void MDEngineSSE::subscribeL2MD(const std::vector<std::string>& i, const std::vector<std::string>&) {
    std::lock_guard<std::mutex> lock(mutex_); symbols_.insert(symbols_.end(), i.begin(), i.end());
    for (std::vector<std::string>::const_iterator it = i.begin(); it != i.end(); ++it) sub(*it);
}
void MDEngineSSE::subscribeOrderTrade(const std::vector<std::string>& i, const std::vector<std::string>& m) { subscribeL2MD(i, m); }

bool MDEngineSSE::allowed(const std::string& symbol) const {
    if (!filter_enabled_ || symbols_.empty()) return true;
    return std::find(symbols_.begin(), symbols_.end(), symbol) != symbols_.end();
}

void MDEngineSSE::run() {
    std::string error;
    std::vector<deepwin_market_data::ChannelSpec> bound_channels = channels_;
    for (std::size_t i = 0; i < bound_channels.size(); ++i) bound_channels[i].receive_cpu = cpu_lease_.cpus()[i].id;
    if (sse_cpu::bind_current_thread(bound_channels[0].receive_cpu, &error))
        runtime_.run(bound_channels, [this](const deepwin_market_data::Datagram& d) { on_datagram(d); }, 0, &error);
    running_ = false;
    logged_in_ = false;
    if (!error.empty()) KF_LOG_ERROR(logger, "[SSE receive] " << error);
    std::lock_guard<std::mutex> lock(mutex_); error_ = error;
}

void MDEngineSSE::on_datagram(const deepwin_market_data::Datagram& d) {
    if (!running_) return;
    // A datagram may contain one or more fixed-size EFH records.  Decode each
    // record independently so a mixed snapshot/tick packet cannot be lost.
    std::size_t offset = 0;
    while (offset < d.size) {
        const std::size_t remaining = d.size - offset;
        std::string error;
        sse_live::TickEvent tick;
        if (remaining >= 72U && sse_live::decode_primary_tick(d.data + offset, 72U, &tick, &error)) {
            if (allowed(tick.security_id) && !seen_tick(tick)) on_tick(tick, static_cast<std::uint64_t>(d.receive_ns));
            offset += 72U;
            continue;
        }
        sse_live::Snapshot s;
        if (remaining >= 440U && sse_live::decode_primary_snapshot(d.data + offset, 440U, &s, &error)) {
            if (allowed(s.security_id) && !seen_snapshot(s)) {
                LFMarketDataField out = {};
                const std::uint32_t day = sse_live::local_trading_date(static_cast<std::uint64_t>(d.receive_ns));
                std::snprintf(out.TradingDay, sizeof(out.TradingDay), "%u", day);
                std::strncpy(out.InstrumentID, s.security_id.c_str(), sizeof(out.InstrumentID)-1);
                std::strncpy(out.ExchangeID, "SSE", sizeof(out.ExchangeID)-1);
                out.LastPrice=s.last_price; out.PreClosePrice=s.pre_close_price; out.OpenPrice=s.open_price;
                out.HighestPrice=s.high_price; out.LowestPrice=s.low_price; out.Volume=static_cast<int>(s.volume); out.Turnover=s.turnover;
                const std::uint64_t tod = s.time_of_day_micros;
                const unsigned h = static_cast<unsigned>(tod / 3600000000ULL);
                const unsigned m = static_cast<unsigned>((tod / 60000000ULL) % 60ULL);
                const unsigned sec = static_cast<unsigned>((tod / 1000000ULL) % 60ULL);
                std::snprintf(out.UpdateTime, sizeof(out.UpdateTime), "%02u:%02u:%02u", h, m, sec);
                out.UpdateMillisec = static_cast<int>((tod / 1000ULL) % 1000ULL);
                for (int n=0;n<5;++n) { out.aBidPrice[n]=s.bid_prices[n]; out.aAskPrice[n]=s.ask_prices[n]; out.aBidVolume[n]=static_cast<int>(s.bid_volumes[n]); out.aAskVolume[n]=static_cast<int>(s.ask_volumes[n]); }
                on_market_data(&out);
            }
            offset += 440U;
            continue;
        }
        // Unknown framing: advance one byte to allow a valid record later in
        // a vendor header/trailer without spinning forever.
        ++offset;
    }
}

bool MDEngineSSE::seen_tick(const sse_live::TickEvent& tick) {
    std::ostringstream key; key << "t:" << tick.channel_no << ':' << tick.tick_index;
    std::lock_guard<std::mutex> lock(mutex_);
    return !seen_keys_.insert(key.str()).second;
}

bool MDEngineSSE::seen_snapshot(const sse_live::Snapshot& snapshot) {
    std::ostringstream key; key << "s:" << snapshot.provider_sequence << ':'
                                << snapshot.msg_seq_id << ':' << snapshot.security_id;
    std::lock_guard<std::mutex> lock(mutex_);
    return !seen_keys_.insert(key.str()).second;
}

void MDEngineSSE::on_tick(const sse_live::TickEvent& tick, std::uint64_t realtime_ns) {
    if (tick.event_type == 'S') return;
    const std::uint32_t day = sse_live::local_trading_date(realtime_ns);
    char day_text[16]; std::snprintf(day_text, sizeof(day_text), "%u", day);
    const std::uint64_t tod = tick.time_of_day_micros;
    const unsigned h = static_cast<unsigned>(tod / 3600000000ULL);
    const unsigned m = static_cast<unsigned>((tod / 60000000ULL) % 60ULL);
    const unsigned sec = static_cast<unsigned>((tod / 1000000ULL) % 60ULL);
    char time_text[32]; std::snprintf(time_text, sizeof(time_text), "%02u:%02u:%02u", h, m, sec);
    if (tick.event_type == 'A' || tick.event_type == 'D') {
        LFL2OrderField order = {};
        std::strncpy(order.OrderTime, time_text, sizeof(order.OrderTime)-1);
        std::strncpy(order.ExchangeID, "SSE", sizeof(order.ExchangeID)-1);
        std::strncpy(order.InstrumentID, tick.security_id.c_str(), sizeof(order.InstrumentID)-1);
        order.Price = static_cast<double>(tick.price_raw) / 1000.0;
        order.Volume = static_cast<double>(tick.quantity_raw) / 1000.0;
        order.OrderKind[0] = tick.side == 'S' ? 'S' : 'B';
        order.ApplSeqNum = static_cast<int64_t>(tick.tick_index);
        order.OrdType[0] = tick.event_type == 'A' ? 'A' : 'D';
        order.OrderNo = static_cast<int64_t>(tick.side == 'S' ? tick.sell_order_no : tick.buy_order_no);
        order.BizIndex = static_cast<int64_t>(tick.tick_index);
        on_market_data(&order);
    } else if (tick.event_type == 'T') {
        LFL2TradeField trade = {};
        std::strncpy(trade.TradeTime, time_text, sizeof(trade.TradeTime)-1);
        std::strncpy(trade.ExchangeID, "SSE", sizeof(trade.ExchangeID)-1);
        std::strncpy(trade.InstrumentID, tick.security_id.c_str(), sizeof(trade.InstrumentID)-1);
        trade.Price = static_cast<double>(tick.price_raw) / 1000.0;
        trade.Volume = static_cast<double>(tick.quantity_raw) / 1000.0;
        trade.TurnOver = static_cast<double>(tick.amount_raw) / 100000.0;
        trade.OrderBSFlag[0] = tick.side == 'S' ? 'S' : 'B';
        trade.BidApplSeqNum = static_cast<int64_t>(tick.buy_order_no);
        trade.OfferApplSeqNum = static_cast<int64_t>(tick.sell_order_no);
        trade.ApplSeqNum = static_cast<int64_t>(tick.tick_index);
        trade.BizIndex = static_cast<int64_t>(tick.tick_index);
        on_market_data(&trade);
    }
}

extern "C" IMDEngine* get_obj(IControlCenter* pcc);
extern "C" IMDEngine* get_obj(IControlCenter* pcc) { return kungfu::wingchun::md_get_obj<MDEngineSSE>(pcc, "sse_fpga"); }
WC_NAMESPACE_END
