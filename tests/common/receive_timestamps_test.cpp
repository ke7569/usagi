#include "common/stream/ReceiveTimestamps.h"
#include <iostream>
#include <stdexcept>

using namespace deepwin_market_data;
static void check(bool value, const char* reason) { if (!value) throw std::runtime_error(reason); }
int main() try {
    union { cmsghdr align; unsigned char bytes[CMSG_SPACE(3 * sizeof(timespec))]; } buffer = {};
    msghdr message = {};
    message.msg_control = buffer.bytes;
    message.msg_controllen = sizeof(buffer.bytes);
    cmsghdr* cmsg = CMSG_FIRSTHDR(&message);
    cmsg->cmsg_level = SOL_SOCKET; cmsg->cmsg_type = SCM_TIMESTAMPING;
    cmsg->cmsg_len = CMSG_LEN(3 * sizeof(timespec));
    timespec timestamps[3] = {{100, 123}, {7, 888}, {93, 456}};
    std::memcpy(CMSG_DATA(cmsg), timestamps, sizeof(timestamps));
    std::uint64_t software; std::uint16_t flags;
    ReceiveClockInfo clocks = read_receive_timestamps(message, true, 3, 100000009999ULL, &software, &flags);
    check(software == 100000000123ULL && clocks.hardware_ns == 93000000456ULL,
          "hardware and kernel clock domains were not preserved separately");
    check(clocks.application_realtime_ns == 100000009999ULL && clocks.hardware_clock_index == 3,
          "application clock or PHC identity lost");
    check(flags == (kKernelRealtimeTimestamp | kHardwareTimestampRequested | kHardwareReceiveTimestamp),
          "timestamp source flags incorrect");

    timestamps[2] = timespec();
    std::memcpy(CMSG_DATA(cmsg), timestamps, sizeof(timestamps));
    clocks = read_receive_timestamps(message, true, 3, 999, &software, &flags);
    check(clocks.hardware_ns == 0 && clocks.hardware_clock_index == 3 &&
          flags == (kKernelRealtimeTimestamp | kHardwareTimestampRequested), "missing hardware stamp was fabricated");
    timestamps[0] = timespec(); timestamps[2] = {93, 456};
    std::memcpy(CMSG_DATA(cmsg), timestamps, sizeof(timestamps));
    clocks = read_receive_timestamps(message, true, 3, 999, &software, &flags);
    check(software == 999 && flags == (kUserspaceRealtimeTimestamp | kHardwareTimestampRequested | kHardwareReceiveTimestamp),
          "missing kernel software stamp was not identified");

    cmsg->cmsg_type = SCM_TIMESTAMPNS; cmsg->cmsg_len = CMSG_LEN(sizeof(timespec));
    timespec legacy = {100, 123}; std::memcpy(CMSG_DATA(cmsg), &legacy, sizeof(legacy));
    clocks = read_receive_timestamps(message, false, -1, 999, &software, &flags);
    check(software == 100000000123ULL && flags == kKernelRealtimeTimestamp && clocks.hardware_ns == 0,
          "legacy timestamp parsing regressed");
    clocks = read_receive_timestamps(message, true, 3, 999, &software, &flags);
    check(software == 999 && flags == (kUserspaceRealtimeTimestamp | kHardwareTimestampRequested),
          "legacy cmsg was mistaken for requested timestamping metadata");
    message.msg_control = 0; message.msg_controllen = 0;
    clocks = read_receive_timestamps(message, false, -1, 999, &software, &flags);
    check(software == 999 && flags == kUserspaceRealtimeTimestamp, "fallback source incorrect");

    message.msg_control = buffer.bytes; message.msg_controllen = sizeof(buffer.bytes);
    cmsg->cmsg_type = SCM_TIMESTAMPING; // Too short for three timespecs.
    bool rejected = false;
    try { read_receive_timestamps(message, true, 3, 999, &software, &flags); }
    catch (const std::runtime_error&) { rejected = true; }
    check(rejected, "truncated hardware metadata accepted");
    cmsg->cmsg_len = CMSG_LEN(3 * sizeof(timespec));
    timestamps[0] = {100, 1000000000L};
    std::memcpy(CMSG_DATA(cmsg), timestamps, sizeof(timestamps));
    rejected = false;
    try { read_receive_timestamps(message, true, 3, 999, &software, &flags); }
    catch (const std::runtime_error&) { rejected = true; }
    check(rejected, "invalid nanoseconds accepted");
    message.msg_flags = MSG_CTRUNC; rejected = false;
    try { read_receive_timestamps(message, false, -1, 999, &software, &flags); }
    catch (const std::runtime_error&) { rejected = true; }
    check(rejected, "MSG_CTRUNC accepted");
    std::cout << "receive_timestamps_test: PASS\n"; return 0;
} catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
