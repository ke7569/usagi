#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <dlfcn.h>
#include <pthread.h>

namespace {

typedef int (*PthreadCreate)(pthread_t*, const pthread_attr_t*, void* (*)(void*), void*);
std::atomic<unsigned long> calls(0);

unsigned long failure_call() {
    const char* value = std::getenv("T0_TEST_FAIL_PTHREAD_CREATE_AT");
    if (!value || !*value) return 0;
    char* end = 0;
    const unsigned long result = std::strtoul(value, &end, 10);
    return end && *end == '\0' ? result : 0;
}

}  // namespace

// Loaded only by subprocess tests; production runtime code has no fault switch.
extern "C" int pthread_create(pthread_t* thread, const pthread_attr_t* attributes,
                              void* (*entry)(void*), void* argument) noexcept {
    static const unsigned long fail_at = failure_call();
    if (calls.fetch_add(1) + 1 == fail_at) return EAGAIN;
    static const PthreadCreate original =
        reinterpret_cast<PthreadCreate>(::dlsym(RTLD_NEXT, "pthread_create"));
    return original ? original(thread, attributes, entry, argument) : EAGAIN;
}
