#include <cassert>
#include <string>
#include <vector>
int main() {
    const std::string sample = "239.35.80.5:37105";
    assert(sample.find(':') != std::string::npos);
    assert(sample.substr(0, sample.find(':')) == "239.35.80.5");
    assert(sample.substr(sample.find(':') + 1) == "37105");
    return 0;
}
