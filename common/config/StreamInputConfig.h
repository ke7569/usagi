#ifndef T0_STREAM_INPUT_CONFIG_H
#define T0_STREAM_INPUT_CONFIG_H

#include "common/config/StreamConfigJson.h"
#include "common/stream/MarketDataStream.h"
#include <limits>

namespace stream_input {
typedef nlohmann::json Json;

inline void fields(const Json& object, const std::set<std::string>& allowed) {
    if (!object.is_object()) throw std::runtime_error("input configuration must be an object");
    for (auto it = object.begin(); it != object.end(); ++it)
        if (!allowed.count(it.key())) throw std::runtime_error("unknown input field: " + it.key());
}

inline std::uint64_t uint_value(const Json& value) {
    if (!value.is_number_integer() || (!value.is_number_unsigned() && value.get<long long>() < 0))
        throw std::runtime_error("input value must be a nonnegative integer");
    return value.get<std::uint64_t>();
}

template <class T> void optional_uint(const Json& object, const char* key, T* destination) {
    if (!object.count(key)) return;
    const std::uint64_t value = uint_value(object.at(key));
    if (value > static_cast<std::uint64_t>(std::numeric_limits<T>::max()))
        throw std::runtime_error(std::string("input integer out of range: ") + key);
    *destination = static_cast<T>(value);
}

inline void cpu(const Json& object, const char* key, int* destination) {
    if (!object.count(key)) return;
    const Json& value = object.at(key);
    if (!value.is_number_integer() ||
        (value.is_number_unsigned() && value.get<std::uint64_t>() > 2147483647ULL))
        throw std::runtime_error("invalid input CPU integer");
    const long long number = value.get<long long>();
    if (number < -1 || number > std::numeric_limits<int>::max()) throw std::runtime_error("input CPU out of range");
    *destination = static_cast<int>(number);
}

inline void load(const std::string& path, std::vector<deepwin_market_data::ChannelSpec>* channels,
                 deepwin_market_data::StreamOptions* options, long* duration) {
    const Json root = load_stream_json(path);
    fields(root, {"channels", "recording_directory", "duration_ms", "queue_capacity",
        "max_datagram_bytes", "receive_batch_size", "receive_buffer_bytes", "idle_gap_ns", "segment_bytes",
        "flush_interval_ms", "receive_cpu", "dispatch_cpu", "writer_cpu", "recording_required"});
    options->recording_directory = root.at("recording_directory").get<std::string>();
    if (options->recording_directory.empty()) throw std::runtime_error("recording directory required");
    if (root.count("recording_required")) options->recording_required = root.at("recording_required").get<bool>();
    const Json& inputs = root.at("channels");
    if (!inputs.is_array() || inputs.empty()) throw std::runtime_error("nonempty input channels required");
    for (const Json& input : inputs) {
        fields(input, {"name", "group", "port", "interface_ip"});
        deepwin_market_data::ChannelSpec channel;
        channel.name = input.at("name").get<std::string>();
        channel.group = input.at("group").get<std::string>();
        if (input.count("interface_ip")) channel.interface_ip = input.at("interface_ip").get<std::string>();
        const std::uint64_t port = uint_value(input.at("port"));
        if (port == 0 || port > 65535) throw std::runtime_error("input port out of range");
        channel.port = static_cast<int>(port);
        channels->push_back(channel);
    }
#define T0_INPUT_UINT(name) optional_uint(root, #name, &options->name)
    T0_INPUT_UINT(queue_capacity); T0_INPUT_UINT(max_datagram_bytes); T0_INPUT_UINT(receive_batch_size);
    T0_INPUT_UINT(receive_buffer_bytes); T0_INPUT_UINT(idle_gap_ns); T0_INPUT_UINT(segment_bytes);
    T0_INPUT_UINT(flush_interval_ms);
#undef T0_INPUT_UINT
    cpu(root, "receive_cpu", &options->receive_cpu); cpu(root, "dispatch_cpu", &options->dispatch_cpu);
    cpu(root, "writer_cpu", &options->writer_cpu);
    optional_uint(root, "duration_ms", duration);
}
}
#endif
