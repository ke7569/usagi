#ifndef USAGI_OMS_JOURNAL_H
#define USAGI_OMS_JOURNAL_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace oms {

// A single owner serializes journal operations. Replay never repairs or truncates.
class Journal {
public:
    explicit Journal(const std::string& path, std::size_t max_record_bytes = 1048576,
                     std::uint64_t max_file_bytes = 1073741824ULL);
    ~Journal();
    Journal(const Journal&) = delete;
    Journal& operator=(const Journal&) = delete;
    bool replay(const std::function<bool(const std::string&)>& visitor);
    bool append(const std::string& payload, bool synchronize);
    bool sync();
    bool healthy() const { return error_.empty(); }
    bool persistent() const { return !path_.empty(); }
    const std::string& error() const { return error_; }
    std::uint64_t sequence() const { return sequence_; }
    std::uint64_t durable_sequence() const { return durable_sequence_; }
private:
    bool fail(const std::string& message);
    std::string path_, error_;
    int fd_;
    std::size_t max_record_bytes_;
    std::uint64_t max_file_bytes_, bytes_, sequence_, durable_sequence_;
    bool replayed_;
};

class AccountLease {
public:
    AccountLease(const std::string& directory, const std::string& broker,
                 const std::string& account);
    ~AccountLease();
    AccountLease(const AccountLease&) = delete;
    AccountLease& operator=(const AccountLease&) = delete;
private:
    int fd_;
};

}  // namespace oms
#endif
