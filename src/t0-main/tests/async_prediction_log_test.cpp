// Async prediction log smoke test: enqueue lines from the calling thread and
// verify they are flushed to disk in order after close().
#include "async_prediction_log.h"

#include <cassert>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

int main() {
    const std::string path = "/tmp/sse_async_prediction_log_test.txt";
    std::remove(path.c_str());

    {
        sse_trading::AsyncPredictionLog log;
        assert(log.open(path));
        for (int i = 0; i < 500; ++i) {
            std::ostringstream line;
            line << "600036," << i << ",snapshot,0.12,1,1," << i * 10 << ","
                 << i * 20 << "," << i * 30;
            log.enqueue(line.str());
        }
        log.close();
    }

    std::ifstream in(path.c_str());
    assert(in.is_open());
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(in, line)) lines.push_back(line);
    assert(lines.size() == 500U);
    assert(lines[0].find(",0,snapshot,") != std::string::npos);
    assert(lines[499].find(",499,snapshot,") != std::string::npos);

    std::remove(path.c_str());
    std::cout << "async_prediction_log_test ok\n";
    return 0;
}
