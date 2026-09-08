#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "common/stream/MarketDataStream.h"
#include "common/stream/UdpChannelRuntime.h"
#include "common/stream/ReceiveTimestamps.h"
#include "common/stream/HardwareTimestamping.h"
#include <linux/net_tstamp.h>

#include <boost/crc.hpp>
#include <algorithm>
#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <limits>
#include <mutex>
#include <poll.h>
#include <sched.h>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

namespace deepwin_market_data {
namespace {

const std::uint32_t kRecordMagic = 0x31444d54U;
const std::uint16_t kEndRecord = 3;
const std::uint32_t kEndian = 0x01020304U;

struct FileHeader {
    char magic[8];
    std::uint32_t version, endian, header_bytes, segment_index;
    std::uint32_t max_payload, channel_count;
    std::uint64_t stream_id, first_sequence, checksum, idle_gap_ns;
};

struct ChannelHeader {
    char name[64], group[16], interface_ip[16];
    std::uint32_t port, reserved;
};

struct RecordHeader {
    std::uint32_t magic;
    std::uint16_t kind, timestamp_flags;
    std::uint32_t payload_bytes, checksum;
    std::uint64_t sequence, monotonic_ns, realtime_ns, receive_batch;
    std::uint32_t channel_id, batch_index, batch_size, source_ipv4;
    std::uint16_t source_port, reserved16;
    std::uint32_t reserved32;
};

static_assert(sizeof(FileHeader) == 64, "recording header ABI");
static_assert(sizeof(ChannelHeader) == 104, "recording channel ABI");
static_assert(sizeof(RecordHeader) == 72, "recording event ABI");

std::uint64_t clock_ns(clockid_t clock) {
    timespec value;
    if (::clock_gettime(clock, &value) != 0) throw std::runtime_error("clock_gettime failed");
    return static_cast<std::uint64_t>(value.tv_sec) * 1000000000ULL + value.tv_nsec;
}

std::uint64_t add_ns(std::uint64_t start, std::uint64_t delta) {
    const std::uint64_t maximum = (std::numeric_limits<std::uint64_t>::max)();
    return delta > maximum - start ? maximum : start + delta;
}

std::string io_error(const char* operation) {
    return std::string(operation) + ": " + std::strerror(errno);
}

void pin(int cpu) {
    if (cpu < 0) return;
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    const int status = ::pthread_setaffinity_np(::pthread_self(), sizeof(set), &set);
    if (status) throw std::runtime_error(std::string("CPU affinity: ") + std::strerror(status));
}

std::string segment_path(const std::string& directory, unsigned index) {
    char suffix[40];
    std::snprintf(suffix, sizeof(suffix), "/stream_%06u.t0md", index);
    return directory + suffix;
}

std::uint32_t record_crc(RecordHeader header, const unsigned char* payload) {
    header.checksum = 0;
    boost::crc_32_type crc;
    crc.process_bytes(&header, sizeof(header));
    if (header.payload_bytes) crc.process_bytes(payload, header.payload_bytes);
    return crc.checksum();
}

std::uint32_t file_crc(FileHeader header, const std::vector<ChannelHeader>& channels) {
    header.checksum = 0;
    boost::crc_32_type crc;
    crc.process_bytes(&header, sizeof(header));
    crc.process_bytes(channels.data(), channels.size() * sizeof(ChannelHeader));
    return crc.checksum();
}

void write_all(int fd, const void* memory, std::size_t size) {
    const unsigned char* data = static_cast<const unsigned char*>(memory);
    while (size) {
        const ssize_t count = ::write(fd, data, size);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) throw std::runtime_error(io_error("recording write"));
        data += count;
        size -= static_cast<std::size_t>(count);
    }
}

bool read_exact(int fd, void* memory, std::size_t size, bool allow_eof = false) {
    unsigned char* data = static_cast<unsigned char*>(memory);
    std::size_t remaining = size;
    while (remaining) {
        const ssize_t count = ::read(fd, data, remaining);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) throw std::runtime_error(io_error("recording read"));
        if (!count) {
            if (remaining == size && allow_eof) return false;
            throw std::runtime_error("truncated recording");
        }
        data += count;
        remaining -= static_cast<std::size_t>(count);
    }
    return true;
}

class File {
public:
    explicit File(int value = -1) : fd(value) {}
    ~File() { if (fd >= 0) ::close(fd); }
    void reset(int value) { if (fd >= 0) ::close(fd); fd = value; }
    int fd;
private:
    File(const File&);
    File& operator=(const File&);
};

// Each queue has exactly one producer and one consumer. Payload storage is
// touched once at construction; enqueue does no allocation or disk I/O.
class Queue {
public:
    Queue(std::size_t capacity, std::size_t payload, bool extra_clocks = false)
        : high_water(0), capacity_(capacity), payload_(payload), headers_(capacity),
          bytes_(capacity * payload, 0), clocks_(extra_clocks ? capacity : 0) {}

    bool push(const RecordHeader& header, const unsigned char* data, const ReceiveClockInfo* clocks = 0) {
        const std::uint64_t head = head_.value.load(std::memory_order_relaxed);
        const std::uint64_t tail = tail_.value.load(std::memory_order_acquire);
        if (head - tail == capacity_) return false;
        const std::size_t index = static_cast<std::size_t>(head % capacity_);
        headers_[index] = header;
        if (!clocks_.empty()) clocks_[index] = clocks ? *clocks : ReceiveClockInfo();
        if (header.payload_bytes) std::memcpy(&bytes_[index * payload_], data, header.payload_bytes);
        const std::uint64_t size = head - tail + 1;
        if (size > high_water) high_water = size;
        head_.value.store(head + 1, std::memory_order_release);
        return true;
    }

    const RecordHeader* front(const unsigned char** data, const ReceiveClockInfo** clocks = 0) const {
        const std::uint64_t tail = tail_.value.load(std::memory_order_relaxed);
        if (tail == head_.value.load(std::memory_order_acquire)) return 0;
        const std::size_t index = static_cast<std::size_t>(tail % capacity_);
        *data = &bytes_[index * payload_];
        if (clocks) *clocks = clocks_.empty() ? 0 : &clocks_[index];
        return &headers_[index];
    }

    void pop() {
        const std::uint64_t tail = tail_.value.load(std::memory_order_relaxed);
        tail_.value.store(tail + 1, std::memory_order_release);
    }
    bool empty() const {
        return tail_.value.load(std::memory_order_acquire) == head_.value.load(std::memory_order_acquire);
    }
    std::uint64_t high_water;

private:
    struct Cursor {
        Cursor() : value(0) {}
        std::atomic<std::uint64_t> value;
        char padding[128];
    };
    std::size_t capacity_, payload_;
    std::vector<RecordHeader> headers_;
    std::vector<unsigned char> bytes_;
    std::vector<ReceiveClockInfo> clocks_;
    Cursor head_, tail_;
};

StreamEvent view(const RecordHeader& header, const unsigned char* bytes, const ReceiveClockInfo* clocks = 0) {
    StreamEvent result = {};
    result.kind = static_cast<StreamEventKind>(header.kind);
    result.sequence = header.sequence;
    result.monotonic_ns = header.monotonic_ns;
    result.realtime_ns = header.realtime_ns;
    result.receive_batch = header.receive_batch;
    result.channel_id = header.channel_id;
    result.batch_index = header.batch_index;
    result.batch_size = header.batch_size;
    result.source_ipv4 = header.source_ipv4;
    result.source_port = header.source_port;
    result.timestamp_flags = header.timestamp_flags;
    result.data = header.payload_bytes ? bytes : 0;
    result.size = header.payload_bytes;
    result.hardware_clock_index = -1;
    if (clocks) {
        result.hardware_ns = clocks->hardware_ns;
        result.application_realtime_ns = clocks->application_realtime_ns;
        result.hardware_clock_index = clocks->hardware_clock_index;
    }
    return result;
}

std::vector<ChannelHeader> channel_headers(const std::vector<ChannelSpec>& channels) {
    std::vector<ChannelHeader> result(channels.size());
    for (std::size_t i = 0; i < channels.size(); ++i) {
        const ChannelSpec& channel = channels[i];
        if (channel.name.empty() || channel.name.size() >= sizeof(result[i].name) ||
            channel.group.size() >= sizeof(result[i].group) ||
            channel.interface_ip.size() >= sizeof(result[i].interface_ip) ||
            channel.port <= 0 || channel.port > 65535) throw std::runtime_error("invalid stream channel");
        for (std::size_t j = 0; j < i; ++j) {
            if (channel.name == channels[j].name ||
                (channel.group == channels[j].group && channel.port == channels[j].port))
                throw std::runtime_error("duplicate stream channel");
        }
        std::memcpy(result[i].name, channel.name.data(), channel.name.size());
        std::memcpy(result[i].group, channel.group.data(), channel.group.size());
        std::memcpy(result[i].interface_ip, channel.interface_ip.data(), channel.interface_ip.size());
        result[i].port = channel.port;
    }
    return result;
}

int udp_socket(const ChannelSpec& channel, int receive_buffer, bool hardware_timestamps) {
    in_addr address;
    if (::inet_pton(AF_INET, channel.group.c_str(), &address) != 1)
        throw std::runtime_error("invalid channel IPv4 address");
    File socket(::socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
    if (socket.fd < 0) throw std::runtime_error(io_error("socket"));
    int one = 1;
    if (::setsockopt(socket.fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) ||
        ::setsockopt(socket.fd, SOL_SOCKET, SO_RCVBUF, &receive_buffer, sizeof(receive_buffer)) ||
        ::setsockopt(socket.fd, SOL_SOCKET, SO_RXQ_OVFL, &one, sizeof(one)))
        throw std::runtime_error(io_error("UDP socket options"));
    if (hardware_timestamps) {
        int timestamp_flags = SOF_TIMESTAMPING_RX_HARDWARE | SOF_TIMESTAMPING_RAW_HARDWARE |
                              SOF_TIMESTAMPING_RX_SOFTWARE | SOF_TIMESTAMPING_SOFTWARE;
        if (::setsockopt(socket.fd, SOL_SOCKET, SO_TIMESTAMPING, &timestamp_flags, sizeof(timestamp_flags)))
            throw std::runtime_error(io_error("enable SO_TIMESTAMPING"));
    } else if (::setsockopt(socket.fd, SOL_SOCKET, SO_TIMESTAMPNS, &one, sizeof(one))) {
        throw std::runtime_error(io_error("enable SO_TIMESTAMPNS"));
    }
    sockaddr_in bind_address = {};
    bind_address.sin_family = AF_INET;
    bind_address.sin_port = htons(static_cast<std::uint16_t>(channel.port));
    bind_address.sin_addr.s_addr = address.s_addr;
    const bool multicast = IN_MULTICAST(ntohl(address.s_addr));
    if (::bind(socket.fd, reinterpret_cast<sockaddr*>(&bind_address), sizeof(bind_address)))
        throw std::runtime_error(io_error("UDP bind"));
    if (multicast) {
        ip_mreq membership = {};
        membership.imr_multiaddr = address;
        if (!channel.interface_ip.empty() &&
            ::inet_pton(AF_INET, channel.interface_ip.c_str(), &membership.imr_interface) != 1)
            throw std::runtime_error("invalid interface IPv4 address");
        if (::setsockopt(socket.fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &membership, sizeof(membership)))
            throw std::runtime_error(io_error("multicast join"));
    }
    const int result = socket.fd;
    socket.fd = -1;
    return result;
}

class Recorder {
public:
    Recorder(const StreamOptions& options, const std::vector<ChannelHeader>& channels, StreamStats* stats)
        : options_(options), channels_(channels), stats_(stats), index_(0), bytes_(0),
          stream_id_(clock_ns(CLOCK_REALTIME) ^ static_cast<std::uint64_t>(::getpid())), last_{},
          buffer_(1U << 20U, 0), buffered_(0), pending_sequence_(0) {
        if (::mkdir(options.recording_directory.c_str(), 0750) != 0)
            throw std::runtime_error(io_error("create new recording directory"));
        std::string created = options.recording_directory;
        while (created.size() > 1 && created.back() == '/') created.pop_back();
        const std::size_t slash = created.find_last_of('/');
        const std::string parent = slash == std::string::npos ? "." :
            (slash == 0 ? "/" : created.substr(0, slash));
        File parent_dir(::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
        if (parent_dir.fd < 0 || ::fsync(parent_dir.fd))
            throw std::runtime_error(io_error("sync recording parent directory"));
        open_segment(1);
    }

    void append(RecordHeader header, const unsigned char* payload) {
        if (bytes_ + sizeof(header) + header.payload_bytes > options_.segment_bytes) {
            sync();
            ++index_;
            open_segment(header.sequence);
        }
        header.checksum = record_crc(header, payload);
        if (buffered_ + sizeof(header) + header.payload_bytes > buffer_.size()) flush_buffer();
        std::memcpy(buffer_.data() + buffered_, &header, sizeof(header));
        buffered_ += sizeof(header);
        if (header.payload_bytes) std::memcpy(buffer_.data() + buffered_, payload, header.payload_bytes);
        buffered_ += header.payload_bytes;
        bytes_ += sizeof(header) + header.payload_bytes;
        last_ = header;
        if (header.kind != kEndRecord) pending_sequence_ = header.sequence;
    }

    void sync() {
        flush_buffer();
        if (::fdatasync(file_.fd)) throw std::runtime_error(io_error("recording fdatasync"));
        stats_->durable_sequence = stats_->written_sequence;
    }

    void finish() {
        sync();
        RecordHeader end = {};
        end.magic = kRecordMagic;
        end.kind = kEndRecord;
        end.sequence = last_.sequence + 1;
        end.monotonic_ns = last_.monotonic_ns;
        end.realtime_ns = last_.realtime_ns;
        append(end, 0);
        sync();
        stats_->clean_recording = true;
    }

private:
    void flush_buffer() {
        if (!buffered_) return;
        write_all(file_.fd, buffer_.data(), buffered_);
        stats_->written_sequence = pending_sequence_;
        buffered_ = 0;
    }

    void open_segment(std::uint64_t first_sequence) {
        if (index_ > 999999U) throw std::runtime_error("recording segment limit reached");
        File next(::open(segment_path(options_.recording_directory, index_).c_str(),
                         O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0640));
        if (next.fd < 0) throw std::runtime_error(io_error("create recording segment"));
        FileHeader header = {};
        std::memcpy(header.magic, "T0MDJ1", 6);
        header.version = 1;
        header.endian = kEndian;
        header.header_bytes = sizeof(header) + channels_.size() * sizeof(ChannelHeader);
        header.segment_index = index_;
        header.max_payload = options_.max_datagram_bytes;
        header.channel_count = channels_.size();
        header.stream_id = stream_id_;
        header.first_sequence = first_sequence;
        header.idle_gap_ns = options_.idle_gap_ns;
        header.checksum = file_crc(header, channels_);
        write_all(next.fd, &header, sizeof(header));
        write_all(next.fd, channels_.data(), channels_.size() * sizeof(ChannelHeader));
        if (::fdatasync(next.fd)) throw std::runtime_error(io_error("sync recording header"));
        File directory(::open(options_.recording_directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
        if (directory.fd < 0 || ::fsync(directory.fd)) throw std::runtime_error(io_error("sync recording directory"));
        file_.reset(next.fd);
        next.fd = -1;
        bytes_ = header.header_bytes;
    }
    StreamOptions options_;
    std::vector<ChannelHeader> channels_;
    StreamStats* stats_;
    File file_;
    unsigned index_;
    std::uint64_t bytes_, stream_id_;
    RecordHeader last_;
    std::vector<unsigned char> buffer_;
    std::size_t buffered_;
    std::uint64_t pending_sequence_;
};

void validate_options(const StreamOptions& options, std::size_t channels, long duration) {
    if (!options.hardware_timestamp_interface.empty() && !options.recording_directory.empty())
        throw std::runtime_error("T0MD v1 cannot record hardware timestamps; use SSE journal sse-stream-v2");
    if (options.recording_required && options.recording_directory.empty())
        throw std::runtime_error("required recording needs a non-empty directory");
    const std::uint32_t endian = kEndian;
    if (*reinterpret_cast<const unsigned char*>(&endian) != 4)
        throw std::runtime_error("recording v1 requires little-endian host");
    if (!channels || channels > 64 || duration < 0 ||
        options.queue_capacity < 2 || options.queue_capacity > (1U << 20U) ||
        options.max_datagram_bytes < 1 || options.max_datagram_bytes > 65536 ||
        options.queue_capacity * options.max_datagram_bytes > (512ULL << 20U) ||
        options.receive_batch_size < 1 || options.receive_batch_size > 256 ||
        options.receive_buffer_bytes < 65536 || options.flush_interval_ms < 1 ||
        options.idle_gap_ns > 1000000000ULL ||
        options.segment_bytes < sizeof(FileHeader) + channels * sizeof(ChannelHeader) +
            sizeof(RecordHeader) + options.max_datagram_bytes ||
        duration > std::numeric_limits<long>::max() / 1000000L)
        throw std::runtime_error("invalid stream limits or memory budget (512 MiB per queue)");
    for (int cpu : {options.receive_cpu, options.dispatch_cpu, options.writer_cpu})
        if (cpu < -1 || cpu >= CPU_SETSIZE) throw std::runtime_error("invalid stream CPU");
}

}  // namespace

StreamOptions::StreamOptions()
    : queue_capacity(4096), max_datagram_bytes(8192), receive_batch_size(64),
      receive_buffer_bytes(16 << 20), idle_gap_ns(100000), segment_bytes(256ULL << 20),
      flush_interval_ms(100), receive_cpu(-1), dispatch_cpu(-1), writer_cpu(-1),
      recording_required(true) {}

StreamStats::StreamStats()
    : received_datagrams(0), receive_batches(0), dispatched_events(0), written_sequence(0),
      durable_sequence(0), ingress_high_water(0), recording_high_water(0),
      ingress_overflows(0), recording_overflows(0), kernel_drops(0), clean_recording(false) {}

struct MarketDataStream::Impl {
    enum { kInputInvalid = 1, kProcessingInvalid = 2, kRecordingFailed = 4, kRecordingRequired = 8 };
    std::atomic<bool> stopping, failed, receiver_done, dispatch_done, receiver_ready;
    std::atomic<unsigned> health_flags;
    StreamStats stats;
    std::mutex error_mutex, writer_mutex;
    std::condition_variable writer_wakeup;
    std::string error, recording_error;
    std::vector<ChannelSpec> channels;

    Impl() : stopping(false), failed(false), receiver_done(false), dispatch_done(false), receiver_ready(false),
             health_flags(kInputInvalid | kProcessingInvalid) {}

    void fail(const std::string& message, unsigned health_flag = kProcessingInvalid) {
        health_flags.fetch_or(health_flag, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(error_mutex);
            if (error.empty()) error = message;
        }
        failed.store(true, std::memory_order_release);
        stopping.store(true, std::memory_order_release);
    }

    bool recording_failed() const {
        return (health_flags.load(std::memory_order_acquire) & kRecordingFailed) != 0;
    }

    void fail_recording(const std::string& message) {
        {
            std::lock_guard<std::mutex> lock(error_mutex);
            if (recording_error.empty()) recording_error = message;
        }
        const unsigned previous = health_flags.fetch_or(kRecordingFailed, std::memory_order_acq_rel);
        if (previous & kRecordingRequired) fail(message, 0);
    }

    void receive(const std::vector<ChannelSpec>& channels, const StreamOptions& options,
                 Queue* ingress, long duration_ms) {
        std::vector<pollfd> sockets;
        try {
            sockets.reserve(channels.size());
            pin(options.receive_cpu);
            const bool hardware = !options.hardware_timestamp_interface.empty();
            std::vector<int> phc_indices;
            for (const ChannelSpec& channel : channels) {
                const int phc = hardware ? timestamping::require_hardware_receive(
                    options.hardware_timestamp_interface, channel.interface_ip) : -1;
                phc_indices.push_back(phc);
                pollfd socket = {udp_socket(channel, options.receive_buffer_bytes, hardware), POLLIN, 0};
                sockets.push_back(socket);
            }
            const std::size_t count = options.receive_batch_size;
            const std::size_t control_bytes = CMSG_SPACE(3 * sizeof(timespec)) +
                                              CMSG_SPACE(sizeof(timespec)) + CMSG_SPACE(sizeof(std::uint32_t));
            std::vector<unsigned char> packets(count * options.max_datagram_bytes, 0);
            std::vector<unsigned char> controls(count * control_bytes, 0);
            std::vector<sockaddr_in> sources(count);
            std::vector<mmsghdr> messages(count);
            std::vector<iovec> vectors(count);
            std::vector<std::uint32_t> dropped(channels.size(), 0);
            for (std::size_t i = 0; i < count; ++i) {
                vectors[i].iov_base = &packets[i * options.max_datagram_bytes];
                vectors[i].iov_len = options.max_datagram_bytes;
                messages[i].msg_hdr.msg_iov = &vectors[i];
                messages[i].msg_hdr.msg_iovlen = 1;
                messages[i].msg_hdr.msg_name = &sources[i];
                messages[i].msg_hdr.msg_control = &controls[i * control_bytes];
            }
            const std::uint64_t start = clock_ns(CLOCK_MONOTONIC);
            const std::uint64_t deadline = duration_ms ? start + static_cast<std::uint64_t>(duration_ms) * 1000000ULL
                                                      : std::numeric_limits<std::uint64_t>::max();
            std::uint64_t sequence = 0, last_receive = 0;
            bool idle_armed = false;
            if (!stopping.load()) receiver_ready.store(true, std::memory_order_release);
            while (!stopping.load(std::memory_order_acquire)) {
                const std::uint64_t now = clock_ns(CLOCK_MONOTONIC);
                if (now >= deadline) {
                    stopping.store(true, std::memory_order_release);
                    break;
                }
                std::uint64_t wait = std::min<std::uint64_t>(1000000ULL, deadline - now);
                if (idle_armed) {
                    const std::uint64_t idle_due = add_ns(last_receive, options.idle_gap_ns);
                    wait = std::min(wait, now >= idle_due ? 0 : idle_due - now);
                }
                timespec timeout = {static_cast<time_t>(wait / 1000000000ULL), static_cast<long>(wait % 1000000000ULL)};
                int ready = ::ppoll(sockets.data(), sockets.size(), &timeout, 0);
                if (ready < 0 && errno == EINTR) continue;
                if (ready < 0) throw std::runtime_error(io_error("UDP ppoll"));
                if (!ready && idle_armed) {
                    const std::uint64_t time = clock_ns(CLOCK_MONOTONIC);
                    const std::uint64_t idle_due = add_ns(last_receive, options.idle_gap_ns);
                    if (time > idle_due && time < deadline) {
                        // A packet can become visible between the timed poll
                        // and this observation. Re-poll with zero timeout and
                        // drain those ready sockets before publishing idle.
                        const timespec zero_timeout = {0, 0};
                        ready = ::ppoll(sockets.data(), sockets.size(),
                                        &zero_timeout, 0);
                        if (ready < 0 && errno == EINTR) continue;
                        if (ready < 0) throw std::runtime_error(io_error("UDP idle drain poll"));
                    }
                    if (!ready && time > idle_due && time < deadline) {
                        RecordHeader idle = {};
                        idle.magic = kRecordMagic;
                        idle.kind = kIdleEvent;
                        idle.sequence = ++sequence;
                        idle.monotonic_ns = time;
                        idle.realtime_ns = clock_ns(CLOCK_REALTIME);
                        if (!ingress->push(idle, 0)) {
                            ++stats.ingress_overflows;
                            throw std::runtime_error("ingress queue overflow; stream incomplete");
                        }
                        idle_armed = false;
                    }
                }
                for (std::size_t channel = 0; channel < sockets.size() && !stopping.load(); ++channel) {
                    if (sockets[channel].revents & (POLLERR | POLLHUP | POLLNVAL))
                        throw std::runtime_error("UDP poll error");
                    if (!(sockets[channel].revents & POLLIN)) continue;
                    for (std::size_t i = 0; i < count; ++i) {
                        messages[i].msg_hdr.msg_namelen = sizeof(sockaddr_in);
                        messages[i].msg_hdr.msg_controllen = control_bytes;
                        messages[i].msg_hdr.msg_flags = 0;
                    }
                    const int received = ::recvmmsg(sockets[channel].fd, messages.data(), count, MSG_DONTWAIT, 0);
                    if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) continue;
                    if (received < 0) throw std::runtime_error(io_error("UDP recvmmsg"));
                    if (!received) continue;
                    const std::uint64_t receive_time = clock_ns(CLOCK_MONOTONIC);
                    const std::uint64_t realtime = clock_ns(CLOCK_REALTIME);
                    const std::uint64_t batch = ++stats.receive_batches;
                    for (int i = 0; i < received; ++i) {
                        const msghdr& message = messages[i].msg_hdr;
                        if (message.msg_flags & (MSG_TRUNC | MSG_CTRUNC))
                            throw std::runtime_error("truncated UDP data or timestamp metadata; stream incomplete");
                        RecordHeader header = {};
                        header.magic = kRecordMagic;
                        header.kind = kDatagramEvent;
                        header.sequence = ++sequence;
                        header.monotonic_ns = receive_time;
                        header.realtime_ns = realtime;
                        header.receive_batch = batch;
                        header.channel_id = channel;
                        header.batch_index = i;
                        header.batch_size = received;
                        header.source_ipv4 = sources[i].sin_addr.s_addr;
                        header.source_port = ntohs(sources[i].sin_port);
                        header.payload_bytes = messages[i].msg_len;
                        const ReceiveClockInfo clocks = read_receive_timestamps(
                            message, hardware, phc_indices[channel], realtime,
                            &header.realtime_ns, &header.timestamp_flags);
                        for (cmsghdr* control = CMSG_FIRSTHDR(&messages[i].msg_hdr); control;
                             control = CMSG_NXTHDR(&messages[i].msg_hdr, control)) {
                            if (control->cmsg_level != SOL_SOCKET) continue;
                            if (control->cmsg_type == SO_RXQ_OVFL && control->cmsg_len >= CMSG_LEN(sizeof(std::uint32_t))) {
                                std::uint32_t current;
                                std::memcpy(&current, CMSG_DATA(control), sizeof(current));
                                const std::uint32_t delta = current - dropped[channel];
                                dropped[channel] = current;
                                if (delta) {
                                    stats.kernel_drops += delta;
                                    throw std::runtime_error("kernel UDP receive overflow; stream incomplete");
                                }
                            }
                        }
                        ++stats.received_datagrams;
                        if (!ingress->push(header, &packets[static_cast<std::size_t>(i) * options.max_datagram_bytes], &clocks)) {
                            ++stats.ingress_overflows;
                            throw std::runtime_error("ingress queue overflow; stream incomplete");
                        }
                    }
                    last_receive = receive_time;
                    idle_armed = options.idle_gap_ns != 0;
                }
            }
        } catch (const std::exception& exception) { fail(exception.what(), kInputInvalid); }
        receiver_ready.store(false, std::memory_order_release);
        for (const pollfd& socket : sockets) ::close(socket.fd);
        receiver_done.store(true, std::memory_order_release);
    }

    void write_records(Queue* queue, Recorder* recorder, const StreamOptions& options) {
        try {
            pin(options.writer_cpu);
            std::uint64_t flushed = clock_ns(CLOCK_MONOTONIC);
            while (!recording_failed()) {
                const unsigned char* payload;
                const RecordHeader* header = queue->front(&payload);
                if (header) {
                    recorder->append(*header, payload);
                    queue->pop();
                } else if (dispatch_done.load(std::memory_order_acquire) && queue->empty()) {
                    break;
                } else {
                    std::unique_lock<std::mutex> lock(writer_mutex);
                    writer_wakeup.wait_for(lock, std::chrono::milliseconds(1));
                }
                const std::uint64_t now = clock_ns(CLOCK_MONOTONIC);
                if (now - flushed >= static_cast<std::uint64_t>(options.flush_interval_ms) * 1000000ULL) {
                    recorder->sync();
                    flushed = now;
                }
            }
            if (!failed.load(std::memory_order_acquire) && !recording_failed()) recorder->finish();
        } catch (const std::exception& exception) { fail_recording(exception.what()); }
    }
};

MarketDataStream::MarketDataStream() : impl_(new Impl()) {}
MarketDataStream::~MarketDataStream() { stop(); }
void MarketDataStream::stop() { impl_->stopping.store(true, std::memory_order_release); }
bool MarketDataStream::stopping() const { return impl_->stopping.load(std::memory_order_acquire); }
bool MarketDataStream::ready() const { return impl_->receiver_ready.load(std::memory_order_acquire); }
const StreamStats& MarketDataStream::stats() const { return impl_->stats; }
const std::vector<ChannelSpec>& MarketDataStream::channels() const { return impl_->channels; }
StreamHealth MarketDataStream::health() const {
    const unsigned flags = impl_->health_flags.load(std::memory_order_acquire);
    StreamHealth result = {!(flags & Impl::kInputInvalid), !(flags & Impl::kProcessingInvalid),
                           (flags & Impl::kRecordingFailed) != 0, (flags & Impl::kRecordingRequired) != 0};
    return result;
}
std::string MarketDataStream::recording_error() const {
    std::lock_guard<std::mutex> lock(impl_->error_mutex);
    return impl_->recording_error;
}

bool MarketDataStream::run(const std::vector<ChannelSpec>& channels, const StreamOptions& options,
                           const StreamCallback& callback, long duration_ms, std::string* error,
                           const StreamPollCallback& owner_poll) {
    if (error) error->clear();
    impl_->stats = StreamStats();
    {
        std::lock_guard<std::mutex> lock(impl_->error_mutex);
        impl_->error.clear();
        impl_->recording_error.clear();
    }
    impl_->health_flags.store(options.recording_required ? Impl::kRecordingRequired : 0);
    impl_->failed.store(false);
    impl_->stopping.store(false);
    impl_->receiver_done.store(false);
    impl_->receiver_ready.store(false);
    impl_->dispatch_done.store(false);
    std::thread receiver, writer;
    cpu_set_t previous_affinity;
    bool restore_affinity = false;
    std::unique_ptr<Queue> ingress, recording;
    std::unique_ptr<Recorder> recorder;
    try {
        validate_options(options, channels.size(), duration_ms);
        if (!callback) throw std::runtime_error("stream callback required");
        const std::vector<ChannelHeader> table = channel_headers(channels);
        impl_->channels = channels;
        ingress.reset(new Queue(options.queue_capacity, options.max_datagram_bytes,
                                !options.hardware_timestamp_interface.empty()));
        if (!options.recording_directory.empty()) {
            recording.reset(new Queue(options.queue_capacity, options.max_datagram_bytes));
            try {
                recorder.reset(new Recorder(options, table, &impl_->stats));
                writer = std::thread(&Impl::write_records, impl_.get(), recording.get(), recorder.get(), std::cref(options));
            } catch (const std::exception& exception) {
                impl_->fail_recording(exception.what());
            }
        }
        receiver = std::thread(&Impl::receive, impl_.get(), std::cref(channels), std::cref(options), ingress.get(), duration_ms);
        if (options.dispatch_cpu >= 0) {
            const int status = ::pthread_getaffinity_np(::pthread_self(), sizeof(previous_affinity), &previous_affinity);
            if (status) throw std::runtime_error("cannot save dispatcher CPU affinity");
            restore_affinity = true;
            pin(options.dispatch_cpu);
        }
        while (!impl_->failed.load(std::memory_order_acquire)) {
            const unsigned char* payload;
            const ReceiveClockInfo* clocks = 0;
            const RecordHeader* header = ingress->front(&payload, &clocks);
            if (!header) {
                if (owner_poll) {
                    try {
                        owner_poll();
                    } catch (const std::exception& exception) {
                        impl_->fail(std::string("stream owner poll failed: ") + exception.what());
                        break;
                    } catch (...) {
                        impl_->fail("stream owner poll failed");
                        break;
                    }
                }
                if (impl_->receiver_done.load(std::memory_order_acquire) && ingress->empty()) break;
                std::this_thread::yield();
                continue;
            }
            if (recording && !impl_->recording_failed()) {
                if (!recording->push(*header, payload)) {
                    ++impl_->stats.recording_overflows;
                    impl_->fail_recording("recording queue overflow; recording incomplete");
                }
                impl_->writer_wakeup.notify_one();
            }
            if (impl_->failed.load(std::memory_order_acquire)) break;
            callback(view(*header, payload, clocks));
            ++impl_->stats.dispatched_events;
            ingress->pop();
        }
    } catch (const std::exception& exception) { impl_->fail(exception.what()); }
      catch (...) { impl_->fail("unknown stream callback error"); }
    stop();
    if (receiver.joinable()) receiver.join();
    impl_->dispatch_done.store(true, std::memory_order_release);
    impl_->writer_wakeup.notify_one();
    if (writer.joinable()) writer.join();
    if (restore_affinity && ::pthread_setaffinity_np(::pthread_self(), sizeof(previous_affinity), &previous_affinity))
        impl_->fail("cannot restore dispatcher CPU affinity");
    if (ingress) impl_->stats.ingress_high_water = ingress->high_water;
    if (recording) impl_->stats.recording_high_water = recording->high_water;
    if (error) *error = impl_->error;
    return !impl_->failed.load();
}

namespace {
std::size_t recording_segment_count(const std::string& directory) {
    std::unique_ptr<DIR, int (*)(DIR*)> dir(::opendir(directory.c_str()), ::closedir);
    if (!dir) throw std::runtime_error(io_error("open recording directory"));
    std::vector<std::string> files;
    for (;;) {
        errno = 0;
        dirent* entry = ::readdir(dir.get());
        if (!entry) {
            if (errno) throw std::runtime_error(io_error("read recording directory"));
            break;
        }
        if (!std::strcmp(entry->d_name, ".") || !std::strcmp(entry->d_name, "..")) continue;
        files.push_back(entry->d_name);
    }
    std::sort(files.begin(), files.end());
    if (files.empty() || files.size() > 1000000U) throw std::runtime_error("invalid recording segment count");
    for (std::size_t i = 0; i < files.size(); ++i) {
        if (files[i] != segment_path("", i).substr(1))
            throw std::runtime_error("non-contiguous or unexpected recording segment");
    }
    return files.size();
}

void read_recording(const std::string& directory, const StreamCallback& callback,
                    StreamStats* stats, std::atomic<bool>* stopping,
                    std::vector<ChannelSpec>* channel_specs, std::int64_t required_idle_gap_ns) {
    std::uint64_t next_sequence = 1, last_time = 0, stream_id = 0;
    std::uint64_t last_batch = 0;
    std::uint32_t next_index = 0, batch_size = 0, batch_channel = 0;
    std::vector<ChannelHeader> expected_channels;
    std::uint32_t expected_max_payload = 0;
    std::uint64_t expected_idle_gap = 0;
    const std::size_t segment_count = recording_segment_count(directory);
    for (unsigned segment = 0; ; ++segment) {
        if (segment == segment_count) throw std::runtime_error("missing clean recording end marker");
        File file(::open(segment_path(directory, segment).c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW));
        if (file.fd < 0) throw std::runtime_error("missing recording segment or clean end marker");
        struct stat file_info;
        if (::fstat(file.fd, &file_info) || !S_ISREG(file_info.st_mode))
            throw std::runtime_error("recording segment must be a regular file");
        FileHeader header;
        read_exact(file.fd, &header, sizeof(header));
        if (std::memcmp(header.magic, "T0MDJ1\0\0", 8) || header.version != 1 ||
            header.endian != kEndian || header.segment_index != segment ||
            !header.channel_count || header.channel_count > 64 ||
            !header.max_payload || header.max_payload > 65536 ||
            header.header_bytes != sizeof(header) + header.channel_count * sizeof(ChannelHeader) ||
            header.first_sequence != next_sequence || header.idle_gap_ns > 1000000000ULL)
            throw std::runtime_error("invalid recording segment header");
        if (required_idle_gap_ns >= 0 && header.idle_gap_ns != static_cast<std::uint64_t>(required_idle_gap_ns))
            throw std::runtime_error("recording idle gap differs from processing contract");
        std::vector<ChannelHeader> channels(header.channel_count);
        read_exact(file.fd, channels.data(), channels.size() * sizeof(ChannelHeader));
        if (header.checksum != file_crc(header, channels)) throw std::runtime_error("recording header checksum mismatch");
        if (!segment) {
            stream_id = header.stream_id;
            expected_channels = channels;
            expected_max_payload = header.max_payload;
            expected_idle_gap = header.idle_gap_ns;
            channel_specs->clear();
            for (const ChannelHeader& channel : channels) {
                if (!std::memchr(channel.name, 0, sizeof(channel.name)) ||
                    !std::memchr(channel.group, 0, sizeof(channel.group)) ||
                    !std::memchr(channel.interface_ip, 0, sizeof(channel.interface_ip)) || channel.reserved)
                    throw std::runtime_error("invalid recording channel metadata");
                ChannelSpec spec;
                spec.name = channel.name;
                spec.group = channel.group;
                spec.interface_ip = channel.interface_ip;
                spec.port = channel.port;
                channel_specs->push_back(spec);
            }
            (void)channel_headers(*channel_specs);
        } else if (header.stream_id != stream_id || header.max_payload != expected_max_payload ||
                   header.idle_gap_ns != expected_idle_gap ||
                   channels.size() != expected_channels.size() ||
                   std::memcmp(channels.data(), expected_channels.data(), channels.size() * sizeof(ChannelHeader))) {
            throw std::runtime_error("recording segment identity mismatch");
        }
        std::vector<unsigned char> payload(header.max_payload);
        RecordHeader event;
        while (read_exact(file.fd, &event, sizeof(event), true)) {
            if (stopping->load(std::memory_order_acquire)) throw std::runtime_error("replay stopped before end");
            if (event.magic != kRecordMagic || event.sequence != next_sequence++ ||
                event.payload_bytes > header.max_payload || event.monotonic_ns < last_time ||
                event.reserved16 || event.reserved32 ||
                (event.kind != kDatagramEvent && event.kind != kIdleEvent && event.kind != kEndRecord))
                throw std::runtime_error("invalid recording event header/order");
            read_exact(file.fd, payload.data(), event.payload_bytes);
            if (event.checksum != record_crc(event, payload.data())) throw std::runtime_error("recording event checksum mismatch");
            last_time = event.monotonic_ns;
            if (event.kind != kDatagramEvent && (event.payload_bytes || event.batch_size || event.receive_batch ||
                event.batch_index || event.channel_id || event.timestamp_flags || event.source_ipv4 || event.source_port))
                throw std::runtime_error("invalid recording control event");
            if (event.kind == kDatagramEvent) {
                if (event.channel_id >= channels.size() || !event.batch_size || event.batch_size > 256 ||
                    event.batch_index >= event.batch_size ||
                    (event.timestamp_flags != kKernelRealtimeTimestamp && event.timestamp_flags != kUserspaceRealtimeTimestamp))
                    throw std::runtime_error("invalid datagram metadata");
                if (event.receive_batch != last_batch) {
                    if (event.receive_batch != last_batch + 1 || next_index != batch_size || event.batch_index)
                        throw std::runtime_error("broken receive batch sequence");
                    last_batch = event.receive_batch;
                    next_index = 0;
                    batch_size = event.batch_size;
                    batch_channel = event.channel_id;
                    ++stats->receive_batches;
                }
                if (event.batch_index != next_index++ || event.batch_size != batch_size || event.channel_id != batch_channel)
                    throw std::runtime_error("broken receive batch boundary");
                ++stats->received_datagrams;
            } else if (next_index != batch_size) {
                throw std::runtime_error("control event interrupts receive batch");
            }
            if (event.kind == kEndRecord) {
                unsigned char extra;
                if (read_exact(file.fd, &extra, 1, true)) throw std::runtime_error("data after clean recording end");
                if (segment + 1 != segment_count)
                    throw std::runtime_error("extra segment after clean recording end");
                stats->clean_recording = true;
                stats->written_sequence = event.sequence - 1;
                // Reading a file verifies integrity, not a new durability barrier.
                stats->durable_sequence = 0;
                return;
            }
            if (callback) {
                if (stopping->load(std::memory_order_acquire)) throw std::runtime_error("replay stopped before end");
                callback(view(event, payload.data()));
            }
            ++stats->dispatched_events;
        }
    }
}
}  // namespace

bool MarketDataStream::replay(const std::string& directory, const StreamCallback& callback, std::string* error,
                              std::int64_t required_idle_gap_ns) {
    if (error) error->clear();
    impl_->stats = StreamStats();
    impl_->stopping.store(false);
    impl_->health_flags.store(0);
    {
        std::lock_guard<std::mutex> lock(impl_->error_mutex);
        impl_->recording_error.clear();
    }
    bool callback_failed = false;
    try {
        if (!callback) throw std::runtime_error("replay callback required");
        if (required_idle_gap_ns < -1 || required_idle_gap_ns > 1000000000LL)
            throw std::runtime_error("invalid required replay idle gap");
        // Preflight completeness/CRC before allowing an application callback.
        StreamStats checked;
        read_recording(directory, StreamCallback(), &checked, &impl_->stopping, &impl_->channels, required_idle_gap_ns);
        const StreamCallback checked_callback = [&](const StreamEvent& event) {
            try { callback(event); }
            catch (...) { callback_failed = true; throw; }
        };
        read_recording(directory, checked_callback, &impl_->stats, &impl_->stopping, &impl_->channels, required_idle_gap_ns);
        return true;
    } catch (const std::exception& exception) {
        impl_->health_flags.fetch_or(callback_failed ? Impl::kProcessingInvalid : Impl::kInputInvalid);
        if (error) *error = exception.what();
    } catch (...) {
        impl_->health_flags.fetch_or(Impl::kProcessingInvalid);
        if (error) *error = "unknown replay callback error";
    }
    return false;
}

}  // namespace deepwin_market_data
