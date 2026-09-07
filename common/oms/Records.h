#ifndef USAGI_OMS_RECORDS_H
#define USAGI_OMS_RECORDS_H

#include "common/oms/Types.h"
#include "third_party/nlohmann/json.hpp"

namespace oms {
namespace records {
enum class Kind : unsigned char { Intent = 1, Send, CancelSend, Report, Dispatch,
    CancelIntent, CancelDispatch, Schedule, Rejected };

std::string intent(Time time, const Command& command);
std::string send(Time time, OrderId id, const SendResult& result, bool cancel);
std::string report(Time time, const Report& value);
std::string id(Time time, Kind kind, OrderId id, std::uint64_t value = 0);
std::string rejected(Time time, const Intent& intent, const Error& error);
// Cold-path conversion for replay and human-readable inspection; accepts old JSON.
nlohmann::json decode(const std::string& payload);
// Compact JSON line without a newline; call only from an export worker.
std::string format(const std::string& payload);
}  // namespace records
}  // namespace oms
#endif
