#ifndef USAGI_RECEIVE_TIMESTAMPS_H
#define USAGI_RECEIVE_TIMESTAMPS_H

#include "common/stream/MarketDataStream.h"
#include <cstring>
#include <limits>
#include <stdexcept>
#include <sys/socket.h>
#include <time.h>

namespace deepwin_market_data {

struct ReceiveClockInfo {
    std::uint64_t hardware_ns, application_realtime_ns;
    std::int32_t hardware_clock_index;
    ReceiveClockInfo() : hardware_ns(0), application_realtime_ns(0), hardware_clock_index(-1) {}
};

inline std::uint64_t receive_timespec_ns(const timespec& timestamp) {
    if (timestamp.tv_sec < 0 || timestamp.tv_nsec < 0 || timestamp.tv_nsec >= 1000000000L ||
        static_cast<std::uint64_t>(timestamp.tv_sec) >
            (std::numeric_limits<std::uint64_t>::max() - timestamp.tv_nsec) / 1000000000ULL)
        throw std::runtime_error("invalid socket receive timestamp");
    return static_cast<std::uint64_t>(timestamp.tv_sec) * 1000000000ULL + timestamp.tv_nsec;
}

// Software and hardware timestamps occupy different clock domains. Never
// substitute one for the other. Only enable one socket timestamp API at a time:
// SO_TIMESTAMPNS together with SO_TIMESTAMPING can synthesize software stamps.
inline ReceiveClockInfo read_receive_timestamps(const msghdr& message, bool hardware_requested,
                                                std::int32_t phc_index, std::uint64_t application_ns,
                                                std::uint64_t* realtime_ns, std::uint16_t* flags) {
    if (message.msg_flags & MSG_CTRUNC)
        throw std::runtime_error("truncated receive timestamp metadata");
    ReceiveClockInfo clocks;
    clocks.application_realtime_ns = application_ns;
    clocks.hardware_clock_index = hardware_requested ? phc_index : -1;
    *realtime_ns = application_ns;
    *flags = kUserspaceRealtimeTimestamp | (hardware_requested ? kHardwareTimestampRequested : 0);
    for (const cmsghdr* control = CMSG_FIRSTHDR(&message); control;
         control = CMSG_NXTHDR(const_cast<msghdr*>(&message), const_cast<cmsghdr*>(control))) {
        if (control->cmsg_level != SOL_SOCKET) continue;
        if (hardware_requested && control->cmsg_type == SCM_TIMESTAMPING) {
            if (control->cmsg_len < CMSG_LEN(3 * sizeof(timespec)))
                throw std::runtime_error("short SCM_TIMESTAMPING message");
            timespec timestamps[3];
            std::memcpy(timestamps, CMSG_DATA(control), sizeof(timestamps));
            const std::uint64_t software = receive_timespec_ns(timestamps[0]);
            clocks.hardware_ns = receive_timespec_ns(timestamps[2]);
            if (software) {
                *realtime_ns = software;
                *flags = (*flags & ~kUserspaceRealtimeTimestamp) | kKernelRealtimeTimestamp;
            }
            if (clocks.hardware_ns) *flags |= kHardwareReceiveTimestamp;
        } else if (!hardware_requested && control->cmsg_type == SCM_TIMESTAMPNS) {
            if (control->cmsg_len < CMSG_LEN(sizeof(timespec)))
                throw std::runtime_error("short SCM_TIMESTAMPNS message");
            timespec timestamp;
            std::memcpy(&timestamp, CMSG_DATA(control), sizeof(timestamp));
            const std::uint64_t software = receive_timespec_ns(timestamp);
            if (software) { *realtime_ns = software; *flags = kKernelRealtimeTimestamp; }
        }
    }
    return clocks;
}
}  // namespace deepwin_market_data
#endif
