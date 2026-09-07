#ifndef SSE_T0_AUCTION59_SIDECAR_H
#define SSE_T0_AUCTION59_SIDECAR_H

#include <map>
#include <string>
#include <vector>

namespace sse_auction59 {

typedef std::map<std::string, std::vector<float> > FactorMap;

bool load_csv(const std::string& path, FactorMap* output,
              std::string* error = 0);

}  // namespace sse_auction59

#endif  // SSE_T0_AUCTION59_SIDECAR_H
