#include "common/oms/Journal.h"
#include "common/oms/Profile.h"

#include <boost/crc.hpp>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sstream>
#include <stdexcept>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <climits>

namespace oms {
namespace {

const std::uint32_t kMagic = 0x4f4d5331U;
const std::uint32_t kVersion = 1U;
const std::size_t kHeaderBytes = 28U;

void put32(std::string* out, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) out->push_back(static_cast<char>((value >> (8 * i)) & 0xff));
}
void put64(std::string* out, std::uint64_t value) {
    for (unsigned i = 0; i < 8; ++i) out->push_back(static_cast<char>((value >> (8 * i)) & 0xff));
}
std::uint32_t get32(const char* p) {
    std::uint32_t v = 0;
    for (unsigned i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(static_cast<unsigned char>(p[i])) << (8 * i);
    return v;
}
std::uint64_t get64(const char* p) {
    std::uint64_t v = 0;
    for (unsigned i = 0; i < 8; ++i) v |= static_cast<std::uint64_t>(static_cast<unsigned char>(p[i])) << (8 * i);
    return v;
}
std::uint32_t checksum(const std::string& payload) {
    boost::crc_32_type crc;
    crc.process_bytes(payload.data(), payload.size());
    return crc.checksum();
}
bool regular_fd(int fd) {
    struct stat st;
    return fstat(fd, &st) == 0 && S_ISREG(st.st_mode);
}
std::string errno_text(const char* what) {
    std::ostringstream os;
    os << what << ": " << std::strerror(errno);
    return os.str();
}
std::string hex_name(const std::string& value) {
    static const char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(value.size() * 2);
    for (std::size_t i = 0; i < value.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(value[i]);
        out.push_back(digits[c >> 4]);
        out.push_back(digits[c & 15]);
    }
    return out;
}
bool write_all(int fd, const char* data, std::size_t size) {
    while (size != 0) {
        const ssize_t n = write(fd, data, size);
        if (n < 0 && errno == EINTR) continue;
        if (n == 0) { errno = EIO; return false; }
        if (n < 0) return false;
        data += n;
        size -= static_cast<std::size_t>(n);
    }
    return true;
}
std::string parent_of(const std::string& path, std::string* base) {
    const std::string::size_type slash = path.find_last_of('/');
    if (slash == std::string::npos) { *base = path; return "."; }
    *base = path.substr(slash + 1);
    if (slash == 0) return "/";
    return path.substr(0, slash);
}
bool sync_fd(int fd) {
    int rc;
    do { rc = fsync(fd); } while (rc < 0 && errno == EINTR);
    return rc == 0;
}

}  // namespace

Journal::Journal(const std::string& path, std::size_t max_record_bytes, std::uint64_t max_file_bytes)
    : path_(path), fd_(-1), max_record_bytes_(max_record_bytes), max_file_bytes_(max_file_bytes),
      bytes_(0), sequence_(0), durable_sequence_(0), replayed_(false) {
    if (max_record_bytes_ == 0 || max_file_bytes_ < kHeaderBytes) {
        error_ = "invalid journal limits";
        return;
    }
    if (path_.empty()) return;
    std::string base;
    const std::string parent = parent_of(path_, &base);
    const int dirfd = open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (dirfd < 0) { error_ = errno_text("open journal directory"); return; }
    bool created = false;
    fd_ = openat(dirfd, base.c_str(), O_RDWR | O_APPEND | O_NOFOLLOW | O_CLOEXEC | O_CREAT | O_EXCL, 0600);
    if (fd_ >= 0) created = true;
    else if (errno == EEXIST)
        fd_ = openat(dirfd, base.c_str(), O_RDWR | O_APPEND | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd_ < 0) { error_ = errno_text("open journal"); close(dirfd); return; }
    if (created && !sync_fd(dirfd)) { error_ = errno_text("sync journal directory"); close(fd_); fd_ = -1; close(dirfd); return; }
    close(dirfd);
    if (!regular_fd(fd_)) { error_ = "journal is not a regular file"; close(fd_); fd_ = -1; return; }
    if (flock(fd_, LOCK_EX | LOCK_NB) != 0) { error_ = errno_text("lock journal"); close(fd_); fd_ = -1; return; }
    struct stat st;
    if (fstat(fd_, &st) != 0) { error_ = errno_text("stat journal"); return; }
    bytes_ = static_cast<std::uint64_t>(st.st_size);
    if (bytes_ > max_file_bytes_) error_ = "journal exceeds configured capacity";
}

Journal::~Journal() { if (fd_ >= 0) close(fd_); }

bool Journal::fail(const std::string& message) {
    if (error_.empty()) error_ = message;
    return false;
}

bool Journal::replay(const std::function<bool(const std::string&)>& visitor) {
    if (!healthy() || replayed_) return healthy();
    if (path_.empty()) { replayed_ = true; return true; }
    if (lseek(fd_, 0, SEEK_SET) < 0) return fail(errno_text("seek journal"));
    std::uint64_t offset = 0;
    std::uint64_t expected = 1;
    while (offset < bytes_) {
        if (bytes_ - offset < kHeaderBytes) return fail("truncated journal frame");
        char header[kHeaderBytes];
        ssize_t got = 0;
        while (got < static_cast<ssize_t>(kHeaderBytes)) {
            const ssize_t n = read(fd_, header + got, kHeaderBytes - static_cast<std::size_t>(got));
            if (n < 0 && errno == EINTR) continue;
            if (n == 0) return fail("short journal header");
            if (n < 0) return fail(errno_text("read journal header"));
            got += n;
        }
        const std::uint32_t magic = get32(header), version = get32(header + 4);
        const std::uint32_t len = get32(header + 8);
        const std::uint64_t seq = get64(header + 12);
        const std::uint32_t crc = get32(header + 20);
        if (magic != kMagic || version != kVersion || get32(header + 24) != 0) return fail("invalid journal frame");
        if (len > max_record_bytes_ || static_cast<std::uint64_t>(len) > bytes_ - offset - kHeaderBytes)
            return fail("journal frame exceeds bounds");
        std::string payload(len, '\0');
        std::size_t done = 0;
        while (done < len) {
            const ssize_t n = read(fd_, &payload[done], len - done);
            if (n < 0 && errno == EINTR) continue;
            if (n == 0) return fail("short journal payload");
            if (n < 0) return fail(errno_text("read journal payload"));
            done += static_cast<std::size_t>(n);
        }
        if (seq != expected || checksum(payload) != crc) return fail("journal sequence or checksum failure");
        try {
            if (visitor && !visitor(payload)) return fail("journal visitor failed");
        } catch (const std::exception&) {
            return fail("journal visitor failed");
        } catch (...) {
            return fail("journal visitor failed");
        }
        ++expected;
        sequence_ = expected - 1;
        offset += kHeaderBytes + len;
    }
    replayed_ = true;
    return sync();
}

bool Journal::append(const std::string& payload, bool synchronize) {
    OMS_PROFILE_SCOPE(profile_append, JournalAppend);
    if (!healthy()) return false;
    if (!replayed_) return fail("journal replay required before append");
    if (max_record_bytes_ > UINT32_MAX || payload.size() > max_record_bytes_ || sequence_ == UINT64_MAX) return fail("journal capacity exceeded");
    const std::uint64_t frame = kHeaderBytes + static_cast<std::uint64_t>(payload.size());
    if (bytes_ > max_file_bytes_ || frame > max_file_bytes_ - bytes_) return fail("journal capacity exceeded");
    const std::uint64_t seq = sequence_ + 1;
    if (path_.empty()) { sequence_ = seq; return true; }
    std::string header;
    header.reserve(kHeaderBytes);
    put32(&header, kMagic); put32(&header, kVersion); put32(&header, static_cast<std::uint32_t>(payload.size()));
    put64(&header, seq); put32(&header, checksum(payload)); put32(&header, 0);
    if (!write_all(fd_, header.data(), header.size()) || !write_all(fd_, payload.data(), payload.size()))
        return fail(errno_text("write journal"));
    bytes_ += frame; sequence_ = seq;
    if (synchronize && !sync()) return false;
    return true;
}

bool Journal::sync() {
    if (!healthy() || !replayed_) return false;
    if (path_.empty()) return true;
    OMS_PROFILE_SCOPE(profile_sync, JournalSync);
    int rc;
    do { rc = fdatasync(fd_); } while (rc < 0 && errno == EINTR);
    if (rc != 0) return fail(errno_text("sync journal"));
    durable_sequence_ = sequence_;
    return true;
}

AccountLease::AccountLease(const std::string& directory, const std::string& broker, const std::string& account)
    : fd_(-1) {
    if (broker.empty() || broker.size() > 32 || account.empty() || account.size() > 64)
        throw std::runtime_error("invalid account lease key");
    const int dirfd = open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (dirfd < 0) throw std::runtime_error(errno_text("open lease directory"));
    const std::string path = directory + "/oms-" + hex_name(broker) + "-" + hex_name(account) + ".lock";
    const std::string base = path.substr(path.find_last_of('/') + 1);
    bool created = false;
    fd_ = openat(dirfd, base.c_str(), O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd_ >= 0) created = true;
    else if (errno == EEXIST)
        fd_ = openat(dirfd, base.c_str(), O_RDWR | O_NOFOLLOW | O_CLOEXEC);
    if (fd_ < 0) { const std::string msg = errno_text("open account lease"); close(dirfd); throw std::runtime_error(msg); }
    if (created && !sync_fd(dirfd)) { const std::string msg = errno_text("sync lease directory"); close(fd_); fd_ = -1; close(dirfd); throw std::runtime_error(msg); }
    close(dirfd);
    if (!regular_fd(fd_) || flock(fd_, LOCK_EX | LOCK_NB) != 0) {
        const std::string msg = errno_text("lock account lease");
        close(fd_); fd_ = -1; throw std::runtime_error(msg);
    }
}

AccountLease::~AccountLease() { if (fd_ >= 0) close(fd_); }

}  // namespace oms
