#ifndef SSE_CPU_AFFINITY_H
#define SSE_CPU_AFFINITY_H

#include <set>
#include <string>
#include <vector>

namespace sse_cpu {

struct Cpu {
    int id;
    std::string l3;
    Cpu(int cpu, const std::string& domain) : id(cpu), l3(domain) {}
};

bool parse_cpu_list(const std::string& text, std::vector<int>* cpus, std::string* error);
bool discover(std::vector<Cpu>* cpus, std::string* error);
bool select(const std::vector<Cpu>& available, const std::vector<int>& requested,
            const std::set<std::string>& busy, std::vector<Cpu>* selected,
            std::string* error);
bool bind_current_thread(int cpu, std::string* error);

// Held until receive workers stop, so simultaneous SSE launches cannot claim
// different CPUs in the same L3 domain.
class Lease {
public:
    Lease() {}
    ~Lease() { release(); }
    Lease(const Lease&) = delete;
    Lease& operator=(const Lease&) = delete;
    bool acquire(const std::vector<int>& requested, std::string* error);
    void release();
    const std::vector<Cpu>& cpus() const { return cpus_; }
private:
    std::vector<Cpu> cpus_;
    std::vector<int> locks_;
};

}  // namespace sse_cpu
#endif
