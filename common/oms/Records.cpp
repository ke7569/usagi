#include "common/oms/Records.h"
#include <cstring>
#include <limits>
#include <stdexcept>

namespace oms {
namespace records {
namespace {
typedef nlohmann::json Json;
const char kPrefix[] = {'O', 'M', 'R', 2};
class Writer {
public:
    Writer(Kind kind, Time time) {
        bytes.reserve(512); bytes.append(kPrefix, sizeof(kPrefix));
        u(static_cast<unsigned char>(kind), 1); signed_value(time);
    }
    void u(std::uint64_t value, unsigned width = 8) {
        for (unsigned i = 0; i < width; ++i) bytes.push_back(static_cast<char>((value >> (8 * i)) & 255));
    }
    void signed_value(std::int64_t value) { u(static_cast<std::uint64_t>(value)); }
    void text(const std::string& value, std::size_t maximum = 128) {
        if (value.size() > maximum) throw std::invalid_argument("OMS record string exceeds bound");
        u(value.size(), 2); bytes.append(value);
    }
    void instrument(const Instrument& i) { text(i.market, 3); text(i.code, 6); }
    void scope(const Scope& s) {
        text(s.account.broker); text(s.account.account); text(s.gateway);
        u(s.day, 4); u(s.epoch); u(static_cast<unsigned short>(s.source), 2);
    }
    void error(const Error& e) {
        u(static_cast<unsigned char>(e.category), 1); signed_value(e.raw_code);
        text(e.message.substr(0, 256), 256); text(e.raw_type.substr(0, 64), 64);
    }
    std::string bytes;
};
class Reader {
public:
    explicit Reader(const std::string& bytes) : bytes_(bytes), offset_(sizeof(kPrefix)) {}
    std::uint64_t u(unsigned width = 8) {
        if (width > bytes_.size() - offset_) throw std::runtime_error("truncated OMS record");
        std::uint64_t value = 0;
        for (unsigned i = 0; i < width; ++i) value |= std::uint64_t(static_cast<unsigned char>(bytes_[offset_++])) << (8 * i);
        return value;
    }
    std::int64_t signed_value() {
        const std::uint64_t value = u();
        return value <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())
            ? static_cast<std::int64_t>(value) : -1 - static_cast<std::int64_t>(~value);
    }
    unsigned enumeration(unsigned last) {
        const unsigned value = static_cast<unsigned>(u(1));
        if (value > last) throw std::runtime_error("invalid OMS record enum");
        return value;
    }
    bool boolean() { return enumeration(1) != 0; }
    std::string text(std::size_t maximum = 128) {
        const std::size_t size = static_cast<std::size_t>(u(2));
        if (size > maximum || size > bytes_.size() - offset_) throw std::runtime_error("invalid OMS record text");
        std::string result = bytes_.substr(offset_, size); offset_ += size; return result;
    }
    Json instrument() {
        const std::string market = text(3), code = text(6);
        return Json{{"market", market}, {"code", code}};
    }
    Json scope() {
        Json s; s["broker"] = text(); s["account"] = text(); s["gateway"] = text();
        s["day"] = u(4); s["epoch"] = u(); s["source"] = u(2); return s;
    }
    Json error() {
        Json e; e["category"] = enumeration(static_cast<unsigned>(ErrorCategory::Protocol));
        const std::int64_t code = signed_value();
        if (code < std::numeric_limits<int>::min() || code > std::numeric_limits<int>::max())
            throw std::runtime_error("invalid OMS raw error code");
        e["raw_code"] = code; e["message"] = text(256); e["raw_type"] = text(64); return e;
    }
    bool done() const { return offset_ == bytes_.size(); }
private:
    const std::string& bytes_;
    std::size_t offset_;
};
}  // namespace

std::string intent(Time time, const Command& c) {
    Writer w(Kind::Intent, time);
    w.scope(c.scope); w.u(c.id); w.text(c.intent.owner); w.text(c.intent.intent_id); w.text(c.intent.signal_id);
    w.instrument(c.intent.instrument); w.u(static_cast<unsigned char>(c.intent.side), 1);
    w.u(static_cast<unsigned char>(c.intent.type), 1); w.signed_value(c.intent.price); w.signed_value(c.intent.quantity);
    w.signed_value(c.intent.cancel_delay_ns); w.u(static_cast<unsigned char>(c.intent.cancel_clock), 1);
    w.text(c.broker_id); w.signed_value(c.time_ns); w.u(c.cancel, 1); return std::move(w.bytes);
}
std::string send(Time time, OrderId id, const SendResult& r, bool cancel) {
    Writer w(cancel ? Kind::CancelSend : Kind::Send, time);
    w.u(id); w.u(static_cast<unsigned char>(r.disposition), 1); w.text(r.broker_id); w.error(r.error);
    return std::move(w.bytes);
}
std::string report(Time time, const Report& r) {
    Writer w(Kind::Report, time);
    w.scope(r.scope); w.u(r.id); w.text(r.broker_id); w.instrument(r.instrument);
    w.u(static_cast<unsigned char>(r.side), 1); w.u(static_cast<unsigned char>(r.kind), 1);
    w.u(static_cast<unsigned char>(r.state), 1); w.signed_value(r.original); w.signed_value(r.cumulative);
    w.signed_value(r.leaves); w.text(r.trade_id); w.signed_value(r.trade_quantity); w.signed_value(r.cumulative_after);
    w.signed_value(r.trade_price); w.signed_value(r.trade_fee); w.u(r.fee_is_final, 1); w.error(r.error); w.u(r.sequence);
    return std::move(w.bytes);
}
std::string id(Time time, Kind kind, OrderId id, std::uint64_t value) {
    Writer w(kind, time); w.u(id);
    if (kind == Kind::CancelDispatch || kind == Kind::Schedule) w.u(value);
    else if (kind != Kind::Dispatch && kind != Kind::CancelIntent) throw std::invalid_argument("invalid OMS ID record kind");
    return std::move(w.bytes);
}
std::string rejected(Time time, const Intent& intent, const Error& error) {
    Writer w(Kind::Rejected, time); w.text(intent.owner.substr(0, 128)); w.text(intent.intent_id.substr(0, 128));
    w.error(error); return std::move(w.bytes);
}

Json decode(const std::string& payload) {
    if (payload.size() < sizeof(kPrefix) || std::memcmp(payload.data(), kPrefix, 3) != 0) return Json::parse(payload);
    if (payload[3] != kPrefix[3]) throw std::runtime_error("unsupported OMS record version");
    Reader r(payload);
    const Kind kind = static_cast<Kind>(r.u(1));
    const Time time = r.signed_value();
    if (time < 0) throw std::runtime_error("invalid OMS record time");
    Json data; const char* type = 0;
    switch (kind) {
    case Kind::Intent: {
        type = "intent"; data["scope"] = r.scope(); data["id"] = r.u();
        Json& i = data["intent"];
        i["owner"] = r.text(); i["intent_id"] = r.text(); i["signal_id"] = r.text(); i["instrument"] = r.instrument();
        i["side"] = r.enumeration(1); i["type"] = r.enumeration(2); i["price"] = r.signed_value();
        i["quantity"] = r.signed_value(); i["cancel_delay_ns"] = r.signed_value(); i["cancel_clock"] = r.enumeration(1);
        data["broker_id"] = r.text(); data["time_ns"] = r.signed_value(); data["cancel"] = r.boolean(); break;
    }
    case Kind::Send: case Kind::CancelSend:
        type = kind == Kind::Send ? "send" : "cancel-send";
        data["id"] = r.u(); data["disposition"] = r.enumeration(2); data["broker_id"] = r.text(); data["error"] = r.error(); break;
    case Kind::Report:
        type = "report"; data["scope"] = r.scope(); data["id"] = r.u(); data["broker_id"] = r.text();
        data["instrument"] = r.instrument(); data["side"] = r.enumeration(1); data["kind"] = r.enumeration(2);
        data["state"] = r.enumeration(static_cast<unsigned>(OrderState::Reconcile));
        data["original"] = r.signed_value(); data["cumulative"] = r.signed_value(); data["leaves"] = r.signed_value();
        data["trade_id"] = r.text(); data["trade_quantity"] = r.signed_value(); data["cumulative_after"] = r.signed_value();
        data["trade_price"] = r.signed_value(); data["trade_fee"] = r.signed_value(); data["fee_is_final"] = r.boolean();
        data["error"] = r.error(); data["sequence"] = r.u(); break;
    case Kind::Dispatch: case Kind::CancelIntent: case Kind::CancelDispatch: case Kind::Schedule:
        type = kind == Kind::Dispatch ? "dispatch" : kind == Kind::CancelIntent ? "cancel-intent" :
            kind == Kind::CancelDispatch ? "cancel-dispatch" : "schedule";
        data["id"] = r.u();
        if (kind == Kind::CancelDispatch) data["attempt"] = r.u();
        if (kind == Kind::Schedule) data["deadline"] = r.signed_value();
        break;
    case Kind::Rejected:
        type = "rejected"; data["owner"] = r.text(); data["intent"] = r.text(); data["error"] = r.error(); break;
    default: throw std::runtime_error("unsupported OMS record kind");
    }
    if (!r.done()) throw std::runtime_error("unexpected OMS record trailing bytes");
    return Json{{"v", 1}, {"type", type}, {"time", time}, {"data", data}};
}
std::string format(const std::string& payload) {
    const Json row = decode(payload);
    const Json& data = row.at("data");
    Json brief = {{"t", row.at("time")}, {"event", row.at("type")}};
    for (const char* key : {"id", "broker_id", "disposition", "state", "cumulative", "trade_id", "trade_quantity"})
        if (data.count(key) && data.at(key) != "") brief[key] = data.at(key);
    const Json& order = data.count("intent") && data.at("intent").is_object() ? data.at("intent") : data;
    for (const char* key : {"instrument", "side", "price", "quantity"})
        if (order.count(key)) brief[key] = order.at(key);
    if (row.at("type") == "intent") {
        brief["owner"] = order.at("owner"); brief["intent_id"] = order.at("intent_id");
        brief["signal_id"] = order.at("signal_id");
    }
    if (data.count("error") && data.at("error").at("category") != 0) brief["error"] = data.at("error");
    return brief.dump();
}
}  // namespace records
}  // namespace oms
