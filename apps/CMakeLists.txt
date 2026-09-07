add_executable(t0_md_stream ${USAGI_ROOT}/apps/t0_md_stream.cpp)
target_link_libraries(t0_md_stream PRIVATE deepwin_market_data_runtime)

if(T0_BUILD_SSE_STREAM_PROCESSOR)
  add_executable(sse_udp_observer
    ${USAGI_ROOT}/tools/sse/sse_udp_observer.cpp)
  target_link_libraries(sse_udp_observer PRIVATE deepwin_market_data_runtime)
  
  add_executable(sse_model_sequence_probe
    ${USAGI_ROOT}/tools/model/sse_model_sequence_probe.cpp)
  target_link_libraries(sse_model_sequence_probe PRIVATE sse_model_runtime)
  
  add_executable(sse_daily_config_builder
    ${USAGI_ROOT}/tools/sse/sse_daily_config_builder.cpp)
  target_include_directories(sse_daily_config_builder PRIVATE ${USAGI_ROOT})
endif()

if(T0_BUILD_SZE_STREAM_PROCESSOR)
  add_executable(sze_recovery_status
    ${USAGI_ROOT}/tools/sze/sze_recovery_status.cpp ${USAGI_ROOT}/sze/market_data/SZERecoverable.cpp
    ${USAGI_ROOT}/sze/market_data/SZEHealthState.cpp)
  target_include_directories(sze_recovery_status PRIVATE ${USAGI_ROOT}/sze/market_data)
  target_link_libraries(sze_recovery_status PRIVATE pthread rt)
  
  add_executable(sze_health_status
    ${USAGI_ROOT}/tools/sze/sze_health_status.cpp
    ${USAGI_ROOT}/sze/market_data/SZEHealthState.cpp ${USAGI_ROOT}/sze/market_data/SZERecoverable.cpp)
  target_include_directories(sze_health_status PRIVATE ${USAGI_ROOT}/sze/market_data)
  target_link_libraries(sze_health_status PRIVATE pthread rt)
  
  if(SZE_BUILD_DIRECT_SHARDED_RING)
    add_executable(sze_sharded_ring_benchmark
      ${USAGI_ROOT}/tools/sze/sze_sharded_ring_benchmark.cpp
      ${USAGI_ROOT}/sze/market_data/SZEDirectShardedRing.cpp ${USAGI_ROOT}/sze/market_data/SZERecoverable.cpp)
    target_include_directories(sze_sharded_ring_benchmark PRIVATE ${USAGI_ROOT}/sze/market_data)
    target_link_libraries(sze_sharded_ring_benchmark PRIVATE pthread rt)
  endif()
  
  add_executable(sze_recovery_verify
    ${USAGI_ROOT}/tools/sze/sze_recovery_verify.cpp ${USAGI_ROOT}/sze/market_data/SZEProtocol.cpp
    ${USAGI_ROOT}/sze/market_data/SZERecoverable.cpp)
  target_include_directories(sze_recovery_verify PRIVATE ${USAGI_ROOT}/sze/market_data ${DEEPWIN_INCLUDE})
  target_link_libraries(sze_recovery_verify PRIVATE pthread rt)
endif()
