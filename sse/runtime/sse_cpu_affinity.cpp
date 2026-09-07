#include "sse_cpu_affinity.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <sched.h>
#include <sstream>
#include <sys/file.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace sse_cpu {
namespace {
bool fail(std::string* error, const std::string& message) {
    if (error) *error = message;
    return false;
}

bool integer(const std::string& text, int* result) {
    if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos) return false;
    char* end = 0;
    errno = 0;
    const long value = std::strtol(text.c_str(), &end, 10);
    if (errno || *end || value < 0 || value >= CPU_SETSIZE) return false;
    *result = static_cast<int>(value);
    return true;
}

std::string read_line(const std::string& path) {
    std::ifstream input(path.c_str());
    std::string line;
    std::getline(input, line);
    return line;
}

std::string domain(int cpu) {
    std::ostringstream root;
    root << "/sys/devices/system/cpu/cpu" << cpu << "/cache/";
    DIR* cache = opendir(root.str().c_str());
    if (!cache) return std::string();
    std::string result;
    while (dirent* entry = readdir(cache)) {
        const std::string path = root.str() + entry->d_name + "/";
        if (read_line(path + "level") != "3") continue;
        std::vector<int> members;
        if (!parse_cpu_list(read_line(path + "shared_cpu_list"), &members, 0)) continue;
        std::set<int> ordered(members.begin(), members.end());
        std::ostringstream key;
        for (std::set<int>::const_iterator it = ordered.begin(); it != ordered.end(); ++it) {
            if (it != ordered.begin()) key << ',';
            key << *it;
        }
        result = key.str();
        break;
    }
    closedir(cache);
    return result;
}

bool sse_process(const std::string& pid) {
    std::ifstream input(("/proc/" + pid + "/cmdline").c_str(), std::ios::binary);
    std::string command;
    std::getline(input, command, '\0');
    if (command.find("sse_") != std::string::npos) return true;
    std::ifstream maps(("/proc/" + pid + "/maps").c_str());
    std::string line;
    while (std::getline(maps, line))
        if (line.find("libsse_md.so") != std::string::npos ||
            line.find("libt0_sse_runtime.so") != std::string::npos) return true;
    return false;
}

std::set<std::string> busy_domains() {
    std::set<std::string> busy;
    DIR* proc = opendir("/proc");
    if (!proc) return busy;
    while (dirent* entry = readdir(proc)) {
        const std::string pid(entry->d_name);
        if (pid.find_first_not_of("0123456789") != std::string::npos || !sse_process(pid)) continue;
        const bool own_process = std::strtol(pid.c_str(), 0, 10) == getpid();
        const std::string task_root = "/proc/" + pid + "/task/";
        DIR* tasks = opendir(task_root.c_str());
        if (!tasks) continue;
        while (dirent* task = readdir(tasks)) {
            const std::string tid(task->d_name);
            if (tid.find_first_not_of("0123456789") != std::string::npos) continue;
            if (std::strtol(tid.c_str(), 0, 10) == syscall(SYS_gettid)) continue;
            cpu_set_t mask;
            CPU_ZERO(&mask);
            if (sched_getaffinity(std::strtol(tid.c_str(), 0, 10), sizeof(mask), &mask) != 0) continue;
            if (CPU_COUNT(&mask) == 1) {
                for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu)
                    if (CPU_ISSET(cpu, &mask)) busy.insert(domain(cpu));
            } else if (!own_process) {
                // Legacy unpinned SSE threads: retain the original planner's
                // conservative exclusion of their last observed processor.
                const std::string stat = read_line(task_root + tid + "/stat");
                const std::size_t start = stat.rfind(") ");
                if (start == std::string::npos) continue;
                std::istringstream fields(stat.substr(start + 2));
                std::string field;
                for (int i = 0; i <= 36 && fields >> field; ++i) {
                    int cpu = -1;
                    if (i == 36 && integer(field, &cpu)) busy.insert(domain(cpu));
                }
            }
        }
        closedir(tasks);
    }
    closedir(proc);
    return busy;
}
}  // namespace

bool parse_cpu_list(const std::string& text, std::vector<int>* cpus, std::string* error) {
    cpus->clear();
    std::set<int> seen;
    std::size_t start = 0;
    do {
        const std::size_t comma = text.find(',', start);
        const std::string token = text.substr(start, comma == std::string::npos ? comma : comma - start);
        const std::size_t dash = token.find('-');
        int first = -1, last = -1;
        if (!integer(token.substr(0, dash), &first) ||
            (dash != std::string::npos && !integer(token.substr(dash + 1), &last)))
            return fail(error, "invalid CPU list: " + text);
        if (dash == std::string::npos) last = first;
        if (first > last) return fail(error, "descending CPU range: " + text);
        for (int cpu = first; cpu <= last; ++cpu) {
            if (!seen.insert(cpu).second) return fail(error, "duplicate CPU: " + text);
            cpus->push_back(cpu);
        }
        if (comma == std::string::npos) break;
        start = comma + 1;
    } while (start <= text.size());
    return true;
}

bool discover(std::vector<Cpu>* cpus, std::string* error) {
    cpus->clear();
    cpu_set_t allowed;
    CPU_ZERO(&allowed);
    if (sched_getaffinity(0, sizeof(allowed), &allowed) != 0)
        return fail(error, std::string("sched_getaffinity: ") + std::strerror(errno));
    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
        if (!CPU_ISSET(cpu, &allowed)) continue;
        const std::string l3 = domain(cpu);
        if (l3.empty()) return fail(error, "cannot determine L3 domain for CPU " + std::to_string(cpu));
        cpus->push_back(Cpu(cpu, l3));
    }
    return !cpus->empty() || fail(error, "no allowed CPUs");
}

bool select(const std::vector<Cpu>& available, const std::vector<int>& requested,
            const std::set<std::string>& busy, std::vector<Cpu>* selected, std::string* error) {
    selected->assign(requested.size(), Cpu(-1, ""));
    std::set<std::string> used = busy;
    // Reserve explicit assignments first so an automatic channel cannot take
    // the domain requested by a later channel.
    for (std::size_t i = 0; i < requested.size(); ++i) {
        if (requested[i] == -1) continue;
        bool found = false;
        for (const Cpu& cpu : available) {
            if (cpu.id != requested[i]) continue;
            if (cpu.l3.empty() || !used.insert(cpu.l3).second)
                return fail(error, "SSE CPU " + std::to_string(cpu.id) + " shares an occupied L3 domain " + cpu.l3);
            (*selected)[i] = cpu;
            found = true;
            break;
        }
        if (!found) return fail(error, "requested SSE CPU is unavailable: " + std::to_string(requested[i]));
    }
    for (std::size_t i = 0; i < requested.size(); ++i) {
        if (requested[i] != -1) continue;
        for (const Cpu& cpu : available) {
            if (!cpu.l3.empty() && used.insert(cpu.l3).second) {
                (*selected)[i] = cpu;
                break;
            }
        }
        if ((*selected)[i].id < 0) return fail(error, "not enough unoccupied L3 domains for SSE workers");
    }
    return !requested.empty() || fail(error, "no SSE workers requested");
}

bool bind_current_thread(int cpu, std::string* error) {
    if (cpu < 0 || cpu >= CPU_SETSIZE) return fail(error, "invalid SSE CPU");
    cpu_set_t mask;
    CPU_ZERO(&mask);
    CPU_SET(cpu, &mask);
    if (sched_setaffinity(0, sizeof(mask), &mask) != 0)
        return fail(error, std::string("SSE sched_setaffinity: ") + std::strerror(errno));
    return true;
}

void Lease::release() {
    for (int fd : locks_) close(fd);
    locks_.clear();
    cpus_.clear();
}

bool Lease::acquire(const std::vector<int>& requested, std::string* error) {
    release();
    std::vector<Cpu> available;
    if (!discover(&available, error)) return false;
    std::set<std::string> busy = busy_domains();
    while (select(available, requested, busy, &cpus_, error)) {
        bool retry = false;
        for (const Cpu& cpu : cpus_) {
            const std::string path = "/tmp/usagi-sse-l3-" + std::to_string(geteuid()) + "-" + cpu.l3 + ".lock";
            const int fd = open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
            if (fd < 0) { release(); return fail(error, "cannot open SSE L3 lease: " + path); }
            if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
                const int saved_errno = errno;
                close(fd);
                if (saved_errno != EWOULDBLOCK && saved_errno != EAGAIN) {
                    release(); return fail(error, "cannot lock SSE L3 lease: " + path);
                }
                busy.insert(cpu.l3);
                retry = true;
                break;
            }
            locks_.push_back(fd);
        }
        if (!retry) return true;
        release();
    }
    release();
    return false;
}
}  // namespace sse_cpu
