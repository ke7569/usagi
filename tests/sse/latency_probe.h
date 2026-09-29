#pragma once
#include "sse/runtime/sse_stream_processor.h"
#include <atomic>
#include <time.h>
extern std::atomic<bool> trace_active;
inline std::uint64_t trace_ns(){timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return std::uint64_t(t.tv_sec)*1000000000ULL+t.tv_nsec;}
void trace_compute(const sse_stream::Output&,std::uint64_t,std::uint64_t,std::uint64_t,std::uint64_t,std::uint64_t);
void trace_strategy(const sse_stream::Output&);

void trace_wrapped_output(const sse_stream::Output&);

void trace_close(unsigned,std::uint64_t,std::uint64_t);
void trace_job(unsigned,std::uint64_t,int,std::uint64_t,std::uint64_t,std::uint64_t,std::size_t);
void trace_delivery(const sse_stream::Output&);
