#include "common/execution/LiveTd.h"
#include <dlfcn.h>
#include <stdexcept>

namespace strategy_runtime {
LiveTdPlugin::LiveTdPlugin(const std::string& library, const std::string& config,
        const oms::Scope& scope, const std::set<oms::Instrument>& universe, bool allow_orders)
    : handle_(0), session_(0), destroy_(0) {
    if (library.empty() || library[0] != '/' || config.empty() || config[0] != '/')
        throw std::runtime_error("live TD requires absolute library and private configuration paths");
    handle_ = ::dlopen(library.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!handle_) throw std::runtime_error(std::string("load live TD: ") + ::dlerror());
    try {
        CreateLiveTd create = reinterpret_cast<CreateLiveTd>(::dlsym(handle_, "usagi_create_live_td_v1"));
        destroy_ = reinterpret_cast<DestroyLiveTd>(::dlsym(handle_, "usagi_destroy_live_td_v1"));
        if (!create || !destroy_) throw std::runtime_error("TD library has no usagi live OMS entry points");
        char error[512] = {};
        session_ = create(config.c_str(), &scope, &universe, allow_orders, error, sizeof(error));
        if (!session_) throw std::runtime_error(error[0] ? error : "live TD creation failed");
    } catch (...) {
        ::dlclose(handle_); handle_ = 0; throw;
    }
}
LiveTdPlugin::~LiveTdPlugin() {
    if (session_) destroy_(session_);
    if (handle_) ::dlclose(handle_);
}
}  // namespace strategy_runtime
