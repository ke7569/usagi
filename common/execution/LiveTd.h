#ifndef USAGI_LIVE_TD_H
#define USAGI_LIVE_TD_H

#include "common/oms/Oms.h"
#include <memory>
#include <set>
#include <string>

namespace strategy_runtime {

// The TD plugin lives in the prediction/strategy process. Only this boundary
// depends on its C++ ABI; build both sides with the same compiler and headers.
class LiveTdSession {
public:
    virtual ~LiveTdSession() {}
    virtual std::shared_ptr<oms::Backend> backend() const = 0;
    virtual void attach(const std::shared_ptr<oms::Engine>& engine) = 0;
    virtual bool connect(long timeout_ns, std::string* error) = 0;
    virtual void stop() = 0;
    virtual std::string status() const = 0;
};

typedef LiveTdSession* (*CreateLiveTd)(const char* config_path,
    const oms::Scope* scope, const std::set<oms::Instrument>* universe,
    bool allow_orders, char* error, std::size_t error_size);
typedef void (*DestroyLiveTd)(LiveTdSession*);

class LiveTdPlugin {
public:
    LiveTdPlugin(const std::string& library, const std::string& config,
                 const oms::Scope& scope, const std::set<oms::Instrument>& universe,
                 bool allow_orders);
    ~LiveTdPlugin();
    LiveTdSession& session() const { return *session_; }
private:
    LiveTdPlugin(const LiveTdPlugin&) = delete;
    LiveTdPlugin& operator=(const LiveTdPlugin&) = delete;
    void* handle_;
    LiveTdSession* session_;
    DestroyLiveTd destroy_;
};

}  // namespace strategy_runtime
#endif
