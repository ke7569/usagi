#include "common/oms/Journal.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <new>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
void require(bool value, const std::string& what) {
    if (!value) throw std::runtime_error(what);
}
std::string temp_dir() {
    char name[] = "/tmp/usagi-oms-journal-XXXXXX";
    char* p = mkdtemp(name);
    require(p != 0, "mkdtemp");
    return p;
}
void test_memory() {
    oms::Journal j("", 8, 64);
    require(j.replay(std::function<bool(const std::string&)>()), "memory replay");
    require(j.append("abc", false), "memory append");
    require(j.sequence() == 1 && j.durable_sequence() == 0, "memory watermarks");
    require(j.sync(), "memory sync");
    require(j.durable_sequence() == 0, "memory durable remains zero");
    oms::Journal limited("", 2, 64);
    require(limited.replay(0) && !limited.append("abc", false) && !limited.healthy(), "memory record limit");
}
void test_append_and_visitor_errors() {
    oms::Journal before("", 8, 64);
    require(!before.append("x", false) && !before.healthy(), "append before replay error");
    oms::Journal visitor("", 8, 64);
    require(visitor.replay(0) && visitor.append("x", false), "visitor source");
    oms::Journal oversized_limit("", static_cast<std::size_t>(UINT32_MAX) + 1U, 64);
    require(oversized_limit.replay(0) && !oversized_limit.append("x", false) && !oversized_limit.healthy(),
            "uint32 record limit");
}
void test_reopen_crc_and_truncate(const std::string& dir) {
    const std::string path = dir + "/journal";
    { oms::Journal j(path); require(j.replay(0), "initial replay"); require(j.append("one", true), "append one"); require(j.append("two", false), "append two"); require(j.sequence() == 2 && j.durable_sequence() == 1, "sync watermark"); }
    { oms::Journal j(path); std::string seen; require(j.replay([&](const std::string& p) { seen += p; return true; }), "reopen replay"); require(seen == "onetwo" && j.sequence() == 2 && j.durable_sequence() == 2, "reopen records"); }
    int fd = open(path.c_str(), O_RDWR | O_CLOEXEC);
    require(fd >= 0, "open corruption target");
    require(pwrite(fd, "x", 1, 20) == 1, "corrupt crc");
    close(fd);
    { oms::Journal j(path); require(!j.replay(0) && !j.healthy() && !j.append("x", false), "sticky crc error"); }
    const std::string journal_link = dir + "/journal-link";
    require(::symlink(path.c_str(), journal_link.c_str()) == 0, "journal symlink setup");
    { oms::Journal j(journal_link); require(!j.healthy() && !j.replay(0), "journal symlink rejected"); }
    const std::string reserved = dir + "/reserved";
    { oms::Journal j(reserved); require(j.replay(0) && j.append("x", true), "reserved source"); }
    fd = open(reserved.c_str(), O_RDWR | O_CLOEXEC); require(fd >= 0, "open reserved"); require(pwrite(fd, "x", 1, 24) == 1, "corrupt reserved"); close(fd);
    { oms::Journal j(reserved); require(!j.replay(0) && !j.healthy(), "reserved validation"); }
    const std::string prefix = dir + "/prefix";
    { oms::Journal j(prefix); require(j.replay(0) && j.append("a", true) && j.append("b", true), "prefix source"); }
    struct stat prefix_st; require(stat(prefix.c_str(), &prefix_st) == 0, "stat prefix");
    fd = open(prefix.c_str(), O_RDWR | O_CLOEXEC); require(fd >= 0, "open prefix"); require(pwrite(fd, "x", 1, prefix_st.st_size - 1) == 1, "corrupt prefix tail"); close(fd);
    { oms::Journal j(prefix); require(!j.replay(0) && j.sequence() == 1 && j.durable_sequence() == 0, "valid prefix is observed, not synced"); require(!j.replay(0), "failed replay sticky"); }
    const std::string throwing = dir + "/throwing";
    { oms::Journal j(throwing); require(j.replay(0) && j.append("x", true), "throw source"); }
    { oms::Journal j(throwing); require(!j.replay([](const std::string&) -> bool { throw std::runtime_error("visitor"); }), "visitor throw"); require(!j.healthy(), "visitor sticky"); }
    const std::string trunc = dir + "/truncated";
    { oms::Journal j(trunc); require(j.replay(0) && j.append("payload", true), "truncate source"); }
    struct stat st; require(stat(trunc.c_str(), &st) == 0, "stat truncate source");
    fd = open(trunc.c_str(), O_RDWR | O_CLOEXEC); require(fd >= 0, "open truncate"); require(ftruncate(fd, st.st_size - 1) == 0, "truncate"); close(fd);
    { oms::Journal j(trunc); require(!j.replay(0) && !j.healthy(), "sticky truncation"); }
}
void test_capacity(const std::string& dir) {
    const std::string path = dir + "/capacity";
    oms::Journal j(path, 3, 31); require(j.replay(0), "capacity replay"); require(j.append("abc", false), "capacity first"); require(!j.append("d", false) && !j.healthy(), "capacity sticky");
}
void test_leases(const std::string& dir) {
    oms::AccountLease first(dir, "broker", "account");
    bool conflicted = false;
    try { oms::AccountLease second(dir, "broker", "account"); } catch (const std::runtime_error&) { conflicted = true; }
    require(conflicted, "same account lease conflict");
    { oms::AccountLease other(dir, "broker", "other"); }
    const std::string symlink_dir = dir + "/symlink";
    require(::symlink(dir.c_str(), symlink_dir.c_str()) == 0, "symlink setup");
    bool symlink_rejected = false;
    try { oms::AccountLease bad(symlink_dir, "broker", "bad"); } catch (const std::runtime_error&) { symlink_rejected = true; }
    require(symlink_rejected, "symlink directory rejected");
    pid_t child = fork();
    require(child >= 0, "fork lease");
    if (child == 0) { try { oms::AccountLease held(dir, "broker", "account"); _exit(2); } catch (...) { _exit(0); } }
    int status = 0; require(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0, "cross process lease conflict");
    first.~AccountLease();
    new (&first) oms::AccountLease(dir, "broker", "account");
}
}  // namespace

int main() {
    try {
        const std::string dir = temp_dir();
        test_memory();
        test_append_and_visitor_errors();
        test_reopen_crc_and_truncate(dir);
        test_capacity(dir);
        test_leases(dir);
        std::cout << "oms journal tests passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "oms journal test failure: " << e.what() << "\n";
        return 1;
    }
}
