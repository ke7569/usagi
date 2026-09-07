#include "../market_data/sse_cpu_affinity.h"

#include <iostream>
#include <stdexcept>

namespace {
void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
}

int main() {
    try {
        const std::vector<sse_cpu::Cpu> topology = {
            {0, "0,1,128,129"}, {1, "0,1,128,129"}, {128, "0,1,128,129"},
            {8, "8,9,136,137"}, {136, "8,9,136,137"}, {16, "16,17,144,145"}
        };
        std::vector<sse_cpu::Cpu> plan;
        std::string error;
        check(sse_cpu::select(topology, {-1, -1}, {}, &plan, &error), "auto plan");
        check(plan[0].id == 0 && plan[1].id == 8, "auto must skip L3 siblings");
        check(!sse_cpu::select(topology, {0, 1}, {}, &plan, &error), "same L3 must fail");
        check(!sse_cpu::select(topology, {0, 128}, {}, &plan, &error), "SMT sibling must fail");
        check(!sse_cpu::select(topology, {0, 0}, {}, &plan, &error), "duplicate CPU must fail");
        check(!sse_cpu::select(topology, {99}, {}, &plan, &error), "unavailable CPU must fail");
        check(!sse_cpu::select(topology, {-2}, {}, &plan, &error), "invalid CPU must fail");
        check(!sse_cpu::select(topology, {-1, -1, -1, -1}, {}, &plan, &error), "insufficient domains must fail");
        check(sse_cpu::select(topology, {-1, 0}, {}, &plan, &error), "mixed explicit/auto plan");
        check(plan[0].id == 8 && plan[1].id == 0, "explicit domain must be reserved first");
        check(sse_cpu::select(topology, {-1, -1}, {"0,1,128,129"}, &plan, &error), "busy exclusion");
        check(plan[0].id == 8 && plan[1].id == 16, "busy L3 must be excluded");
        check(!sse_cpu::select(topology, {128}, {"0,1,128,129"}, &plan, &error), "explicit busy domain must fail");
        std::vector<int> parsed;
        check(sse_cpu::parse_cpu_list("0,8-9,128", &parsed, &error) && parsed.size() == 4, "CPU ranges");
        for (const std::string value : {"", "-1", "0,", "0,,8", "8-0", "0,0", "1x", "99999999999999999"})
            check(!sse_cpu::parse_cpu_list(value, &parsed, &error), "invalid CPU list accepted");
        check(!sse_cpu::bind_current_thread(-1, &error), "invalid affinity must fail");
        std::cout << "sse_cpu_affinity_test: PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
