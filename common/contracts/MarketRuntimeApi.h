#ifndef T0_MARKET_RUNTIME_API_H
#define T0_MARKET_RUNTIME_API_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Each handle is single-start. The owner must not race destruction against
// another API call. Stop and status may be called while join is waiting.
// Returned API pointers stay valid until the library is unloaded after destroy.
typedef struct T0MarketRuntimeApiV1 {
    uint32_t abi_version;
    uint32_t struct_bytes;
    const char* market;
    void* (*create)(const char* action, const char* input, const char* profile,
                    char* error, size_t error_bytes);
    int (*start)(void* handle, char* error, size_t error_bytes);
    void (*request_stop)(void* handle);
    int (*join)(void* handle, char* error, size_t error_bytes);
    // Required bytes including NUL. A too-small buffer is left empty.
    size_t (*status_json)(void* handle, char* output, size_t output_bytes);
    void (*destroy)(void* handle);
} T0MarketRuntimeApiV1;

const T0MarketRuntimeApiV1* t0_market_runtime_v1(void);

#ifdef __cplusplus
}
#endif
#endif
