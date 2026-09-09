set(T0_OMS_SOURCES
  ${USAGI_ROOT}/common/oms/Types.cpp ${USAGI_ROOT}/common/oms/Journal.cpp
  ${USAGI_ROOT}/common/oms/Records.cpp ${USAGI_ROOT}/common/oms/AsyncJournal.cpp
  ${USAGI_ROOT}/common/oms/Backend.cpp ${USAGI_ROOT}/common/oms/Oms.cpp
  ${USAGI_ROOT}/common/execution/AccountReconciliation.cpp)
add_library(t0_oms_core STATIC ${T0_OMS_SOURCES})
target_include_directories(t0_oms_core PUBLIC ${USAGI_ROOT} PRIVATE ${BOOST_INCLUDE})
target_link_libraries(t0_oms_core PUBLIC pthread)

# Isolated diagnostic build: no probe symbols or clock reads in normal targets.
add_library(t0_oms_profiled_core STATIC EXCLUDE_FROM_ALL
  ${T0_OMS_SOURCES} ${USAGI_ROOT}/common/oms/Profile.cpp)
target_compile_definitions(t0_oms_profiled_core PUBLIC USAGI_OMS_PROFILE=1)
target_include_directories(t0_oms_profiled_core PUBLIC ${USAGI_ROOT} PRIVATE ${BOOST_INCLUDE})
target_link_libraries(t0_oms_profiled_core PUBLIC pthread)

add_library(t0_strategy_core STATIC
  ${USAGI_ROOT}/common/strategy/ZStrategy.cpp ${USAGI_ROOT}/common/execution/StreamStrategyExecution.cpp
  ${USAGI_ROOT}/common/strategy/StrategySession.cpp ${USAGI_ROOT}/common/execution/OmsStrategyExecution.cpp)
target_sources(t0_strategy_core PRIVATE ${USAGI_ROOT}/common/execution/LiveTd.cpp)
target_include_directories(t0_strategy_core PUBLIC ${USAGI_ROOT} ${USAGI_ROOT}/common/strategy)
target_link_libraries(t0_strategy_core PUBLIC t0_oms_core pthread dl)

add_library(deepwin_market_data_runtime STATIC
  ${USAGI_ROOT}/common/stream/UdpChannelRuntime.cpp
  ${USAGI_ROOT}/common/stream/HardwareTimestamping.cpp
  ${USAGI_ROOT}/common/stream/MarketDataStream.cpp)
target_include_directories(deepwin_market_data_runtime PUBLIC
  ${USAGI_ROOT}/common/stream)
target_include_directories(deepwin_market_data_runtime PRIVATE ${BOOST_INCLUDE})
target_link_libraries(deepwin_market_data_runtime PUBLIC pthread rt)

if(T0_BUILD_SZE_STREAM_PROCESSOR)
  add_library(common_mix153060_model STATIC ${USAGI_ROOT}/common/model/mix153060/mix153060_model.cpp)
  target_include_directories(common_mix153060_model PUBLIC ${USAGI_ROOT})
  target_compile_options(common_mix153060_model PRIVATE -ffp-contract=off)
endif()

if(T0_BUILD_SSE_STREAM_PROCESSOR)
  add_library(sse_model_runtime STATIC
    ${USAGI_ROOT}/common/model/legacy_midmix/sse_model_runtime.cpp)
  target_include_directories(sse_model_runtime PUBLIC
    ${USAGI_ROOT}/sse/model
    ${USAGI_ROOT}/third_party/eigen3)
  target_compile_options(sse_model_runtime PRIVATE -ffp-contract=off)
  
  add_library(snapshot_gru_engine STATIC ${USAGI_ROOT}/common/model/snapshot_gru/snapshot_gru_runtime.cpp)
  target_include_directories(snapshot_gru_engine PUBLIC ${USAGI_ROOT})
  target_compile_options(snapshot_gru_engine PRIVATE -ffp-contract=off)
endif()
