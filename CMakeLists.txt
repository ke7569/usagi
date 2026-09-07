cmake_minimum_required(VERSION 3.10)
project(sze_dev_debug CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
option(SZE_MARCH_NATIVE "Compile for the build machine CPU" OFF)
option(SZE_BUILD_TD "Build the Shenzhen ATP TD plugin" OFF)
option(SSE_BUILD_TD "Build the Shanghai ATP TD plugin" OFF)
option(SSE_MODEL_MARCH_NATIVE "Use the target host ISA for Shanghai model inference" OFF)
option(SZE_BUILD_DIRECT_SHARDED_RING
  "Build the experimental direct sharded-ring tests/benchmark" OFF)
set(SZE_TD_API_DIR "${CMAKE_CURRENT_SOURCE_DIR}/modules/deepwin_guoxin/api" CACHE PATH
    "Directory containing ATP Quant API include/ and lib/")

if(SZE_MARCH_NATIVE)
  add_compile_options(-march=native)
else()
  add_compile_options(-march=x86-64 -mtune=generic)
endif()
add_compile_options(-O3 -DNDEBUG)

set(T0 ${CMAKE_CURRENT_SOURCE_DIR}/src/t0-main)
set(SZE_MD ${CMAKE_CURRENT_SOURCE_DIR}/modules/deepwin_guoxin/md)
set(SZE_TD ${CMAKE_CURRENT_SOURCE_DIR}/modules/deepwin_guoxin/td)
set(RUNTIME ${CMAKE_CURRENT_SOURCE_DIR}/runtime_so CACHE PATH
  "Deepwin runtime root (may be supplied by the validated external builder)")
set(DEEPWIN_INCLUDE ${CMAKE_CURRENT_SOURCE_DIR}/toolchain/deepwin_include)
set(BOOST_INCLUDE ${CMAKE_CURRENT_SOURCE_DIR}/toolchain/boost_include)
set(PYTHON_INCLUDE ${CMAKE_CURRENT_SOURCE_DIR}/toolchain/python_include)
set(WC_LIB ${RUNTIME}/deepwin_core/lib/wingchun)
set(YJJ_LIB ${RUNTIME}/deepwin_core/lib/yijinjing)
set(TPWC_LIB ${RUNTIME}/third_party/wingchun)
set(BOOST_LIB ${RUNTIME}/third_party/boost)

set(SZE_COMMON_SOURCES
  ${T0}/common.cpp
  ${T0}/snap_generator.cpp
  ${T0}/order_match_engine.cpp
  ${T0}/sz_hp_orderbook.cpp
  ${T0}/sz_hp_realtime_state.cpp
  ${T0}/sz_hp_factor_adapter.cpp
  ${T0}/sz_hp_latency.cpp
  ${T0}/shsz_full_orderbook_engine.cpp
  ${T0}/shsz_full_orderbook_diagnostics.cpp
  ${T0}/shsz_full_orderbook_factor.cpp
  ${T0}/shsz_full_orderbook_manager.cpp
  ${T0}/shsz_predictor_transition_adapter.cpp
  ${T0}/order_list.cpp
  ${T0}/predictor/md_utils.cpp
  ${T0}/predictor/factor.cpp
  ${T0}/predictor/predictor.cpp
  ${T0}/predictor/mix153060_model.cpp
  ${T0}/predictor/mix153060_capture.cpp
  ${T0}/predictor/mix153060_runtime.cpp
  ${T0}/predictor/mix153060_live_adapter.cpp
  ${T0}/snap_latency.cpp
  ${T0}/bse_latency.cpp
  ${T0}/strategy/StrategyBase.cpp
  ${T0}/strategy/ZStrategy.cpp
  ${T0}/deepwin_strategy/sze_config_guard.cpp
  ${T0}/deepwin_strategy/sze_get_obj.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/reference/snapshot_legacy15/snapshot_legacy15_factors.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/reference/snapshot_legacy15/snapshot_legacy15_model.cpp
  ${SZE_MD}/SZEProtocol.cpp
  ${SZE_MD}/SZERecoverable.cpp
  ${SZE_MD}/SZEHealthState.cpp)

set(SSE_COMMON_SOURCES ${SZE_COMMON_SOURCES})
list(REMOVE_ITEM SSE_COMMON_SOURCES ${T0}/deepwin_strategy/sze_get_obj.cpp)
list(APPEND SSE_COMMON_SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/sse-t0/strategy/sse_config_guard.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/sse-t0/strategy/sse_get_obj.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/sse-t0/market_data/sse_primary_decoder.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/sse-t0/market_data/sse_tick_order_book.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/sse-t0/market_data/sse_tick_factors.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/sse-t0/market_data/sse_tick_static_metadata.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/sse-t0/model/snapshot36.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/sse-t0/model/auction59_sidecar.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/sse-t0/model/auction_static_metadata.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/sse-t0/model/auction59_engine.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/sse-t0/model/sse_model_runtime.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/sse-t0/model/snapshot_gru_runtime.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/sse-t0/model/snapshot_ensemble.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/sse-t0/model/sse_hybrid_model.cpp)

set(DEEPWIN_LIBS
  ${WC_LIB}/libwingchunstrategy.so
  ${YJJ_LIB}/libjournal.so.1.1
  ${YJJ_LIB}/libkflog.so
  ${TPWC_LIB}/libpython3.6m.so.1.0
  ${BOOST_LIB}/libboost_locale.so.1.62.0
  ${BOOST_LIB}/libboost_date_time.so.1.62.0
  ${BOOST_LIB}/libboost_filesystem.so.1.62.0
  ${BOOST_LIB}/libboost_system.so.1.62.0
  ${BOOST_LIB}/libboost_thread.so.1.62.0
  ${BOOST_LIB}/libboost_serialization.so.1.62.0
  ${BOOST_LIB}/libboost_math_tr1.so.1.62.0
  ${BOOST_LIB}/libboost_python.so.1.62.0
  ${BOOST_LIB}/libboost_chrono.so.1.62.0
  ${BOOST_LIB}/libboost_regex.so.1.62.0
  ${BOOST_LIB}/libboost_program_options.so.1.62.0
  ${BOOST_LIB}/libboost_atomic.so.1.62.0
  pthread dl util rt m)

add_library(t0_strategy_sze SHARED ${SZE_COMMON_SOURCES})
target_compile_definitions(t0_strategy_sze PRIVATE T0_USE_DEEPWIN T0_SZE_STRATEGY_ONLY)
target_include_directories(t0_strategy_sze PRIVATE
  ${T0} ${T0}/strategy ${T0}/predictor ${T0}/cpp_model/eigen3 ${SZE_MD}
  ${T0}/cpp_model
  ${CMAKE_CURRENT_SOURCE_DIR}/reference/snapshot_legacy15
  ${DEEPWIN_INCLUDE} ${DEEPWIN_INCLUDE}/longfist ${BOOST_INCLUDE} ${PYTHON_INCLUDE})
target_link_libraries(t0_strategy_sze PRIVATE ${DEEPWIN_LIBS})
set_target_properties(t0_strategy_sze PROPERTIES
  OUTPUT_NAME t0_strategy_sze
  BUILD_RPATH "${WC_LIB};${YJJ_LIB};${TPWC_LIB};${BOOST_LIB}")
set_source_files_properties(${T0}/predictor/mix153060_runtime.cpp
  PROPERTIES COMPILE_OPTIONS -ffp-contract=off)

add_library(t0_strategy_sse SHARED ${SSE_COMMON_SOURCES})
target_compile_definitions(t0_strategy_sse PRIVATE T0_USE_DEEPWIN)
target_include_directories(t0_strategy_sse PRIVATE ${T0} ${T0}/strategy ${T0}/predictor ${T0}/cpp_model ${T0}/cpp_model/eigen3 ${SZE_MD} ${CMAKE_CURRENT_SOURCE_DIR}/reference/snapshot_legacy15 ${DEEPWIN_INCLUDE} ${DEEPWIN_INCLUDE}/longfist ${BOOST_INCLUDE} ${PYTHON_INCLUDE})
target_link_libraries(t0_strategy_sse PRIVATE ${DEEPWIN_LIBS})
set_target_properties(t0_strategy_sse PROPERTIES OUTPUT_NAME t0_strategy_sse BUILD_RPATH "${WC_LIB};${YJJ_LIB};${TPWC_LIB};${BOOST_LIB}")

if(SZE_BUILD_TD)
  if(NOT EXISTS "${SZE_TD_API_DIR}/include/atp_quant_api.h")
    message(FATAL_ERROR "SZE_BUILD_TD requires ${SZE_TD_API_DIR}/include/atp_quant_api.h")
  endif()
  if(NOT EXISTS "${SZE_TD_API_DIR}/lib/libatpquantapi.so")
    message(FATAL_ERROR "SZE_BUILD_TD requires vendor ${SZE_TD_API_DIR}/lib/libatpquantapi.so")
  endif()
  add_library(sze_td SHARED
    ${SZE_TD}/TDEngineGXBSE.cpp)
  target_compile_definitions(sze_td PRIVATE
    GXBSE_TD_BUILD_VERSION="sze-dev"
    T0_TD_PLUGIN_LABEL="sze_td"
    T0_TD_SOURCE_ID=180
    T0_TD_ENGINE_KEY="sze_td"
    T0_TD_DEFAULT_MARKET_ID=102)
  target_include_directories(sze_td PRIVATE
    ${SZE_TD} ${SZE_TD_API_DIR}/include
    ${DEEPWIN_INCLUDE} ${DEEPWIN_INCLUDE}/longfist
    ${BOOST_INCLUDE} ${PYTHON_INCLUDE})
  target_link_libraries(sze_td PRIVATE
    ${YJJ_LIB}/libjournal.so.1.1
    ${YJJ_LIB}/libkflog.so
    ${WC_LIB}/libwingchuntd.so
    ${SZE_TD_API_DIR}/lib/libatpquantapi.so
    ${BOOST_LIB}/libboost_locale.so.1.62.0
    ${BOOST_LIB}/libboost_date_time.so.1.62.0
    ${BOOST_LIB}/libboost_filesystem.so.1.62.0
    ${BOOST_LIB}/libboost_system.so.1.62.0
    ${BOOST_LIB}/libboost_thread.so.1.62.0
    ${BOOST_LIB}/libboost_serialization.so.1.62.0
    ${BOOST_LIB}/libboost_math_tr1.so.1.62.0
    ${BOOST_LIB}/libboost_python.so.1.62.0
    ${BOOST_LIB}/libboost_chrono.so.1.62.0
    ${BOOST_LIB}/libboost_regex.so.1.62.0
    ${BOOST_LIB}/libboost_program_options.so.1.62.0
    ${BOOST_LIB}/libboost_atomic.so.1.62.0 pthread dl rt)
  set_target_properties(sze_td PROPERTIES
    OUTPUT_NAME sze_td
    BUILD_WITH_INSTALL_RPATH TRUE
    INSTALL_RPATH "\$ORIGIN;/home/zane/lib;/opt/deepwin/master/lib/wingchun;/opt/deepwin/master/lib/yijinjing;/opt/deepwin/toolchain/boost-1.62.0/lib")
endif()

if(SSE_BUILD_TD)
  if(NOT EXISTS "${SZE_TD_API_DIR}/include/atp_quant_api.h")
    message(FATAL_ERROR "SSE_BUILD_TD requires ${SZE_TD_API_DIR}/include/atp_quant_api.h")
  endif()
  if(NOT EXISTS "${SZE_TD_API_DIR}/lib/libatpquantapi.so")
    message(FATAL_ERROR "SSE_BUILD_TD requires vendor ${SZE_TD_API_DIR}/lib/libatpquantapi.so")
  endif()
  add_library(sse_td SHARED
    ${SZE_TD}/TDEngineGXSSE.cpp)
  target_compile_definitions(sse_td PRIVATE
    GXBSE_TD_BUILD_VERSION="sse-dev"
    T0_TD_PLUGIN_LABEL="sse_td"
    T0_TD_SOURCE_ID=190
    T0_TD_ENGINE_KEY="sse_td"
    T0_TD_DEFAULT_MARKET_ID=101)
  target_include_directories(sse_td PRIVATE
    ${SZE_TD} ${SZE_TD_API_DIR}/include
    ${DEEPWIN_INCLUDE} ${DEEPWIN_INCLUDE}/longfist
    ${BOOST_INCLUDE} ${PYTHON_INCLUDE})
  target_link_libraries(sse_td PRIVATE
    ${YJJ_LIB}/libjournal.so.1.1
    ${YJJ_LIB}/libkflog.so
    ${WC_LIB}/libwingchuntd.so
    ${SZE_TD_API_DIR}/lib/libatpquantapi.so
    ${BOOST_LIB}/libboost_locale.so.1.62.0
    ${BOOST_LIB}/libboost_date_time.so.1.62.0
    ${BOOST_LIB}/libboost_filesystem.so.1.62.0
    ${BOOST_LIB}/libboost_system.so.1.62.0
    ${BOOST_LIB}/libboost_thread.so.1.62.0
    ${BOOST_LIB}/libboost_serialization.so.1.62.0
    ${BOOST_LIB}/libboost_math_tr1.so.1.62.0
    ${BOOST_LIB}/libboost_python.so.1.62.0
    ${BOOST_LIB}/libboost_chrono.so.1.62.0
    ${BOOST_LIB}/libboost_regex.so.1.62.0
    ${BOOST_LIB}/libboost_program_options.so.1.62.0
    ${BOOST_LIB}/libboost_atomic.so.1.62.0 pthread dl rt)
  set_target_properties(sse_td PROPERTIES
    OUTPUT_NAME sse_td
    BUILD_WITH_INSTALL_RPATH TRUE
    INSTALL_RPATH "\$ORIGIN;/home/zane/lib;/opt/deepwin/master/lib/wingchun;/opt/deepwin/master/lib/yijinjing;/opt/deepwin/toolchain/boost-1.62.0/lib")
endif()

add_library(sze_md SHARED
  ${SZE_MD}/MDEngineSZE.cpp ${SZE_MD}/SZEProtocol.cpp
  ${SZE_MD}/SZERecoverable.cpp ${SZE_MD}/SZEHealthState.cpp)
target_include_directories(sze_md PRIVATE
  ${SZE_MD} ${DEEPWIN_INCLUDE} ${DEEPWIN_INCLUDE}/longfist
  ${BOOST_INCLUDE} ${PYTHON_INCLUDE})
target_link_libraries(sze_md PRIVATE
  ${YJJ_LIB}/libjournal.so.1.1 ${YJJ_LIB}/libkflog.so
  ${WC_LIB}/libwingchunmd.so ${TPWC_LIB}/libpython3.6m.so.1.0
  ${TPWC_LIB}/liblog4cplus.so.0
  ${BOOST_LIB}/libboost_locale.so.1.62.0
  ${BOOST_LIB}/libboost_date_time.so.1.62.0
  ${BOOST_LIB}/libboost_filesystem.so.1.62.0
  ${BOOST_LIB}/libboost_system.so.1.62.0
  ${BOOST_LIB}/libboost_thread.so.1.62.0
  ${BOOST_LIB}/libboost_serialization.so.1.62.0
  ${BOOST_LIB}/libboost_math_tr1.so.1.62.0
  ${BOOST_LIB}/libboost_python.so.1.62.0
  ${BOOST_LIB}/libboost_chrono.so.1.62.0
  ${BOOST_LIB}/libboost_regex.so.1.62.0
  ${BOOST_LIB}/libboost_program_options.so.1.62.0
  ${BOOST_LIB}/libboost_atomic.so.1.62.0 pthread dl rt)
set_target_properties(sze_md PROPERTIES OUTPUT_NAME sze_md
  BUILD_RPATH "${WC_LIB};${YJJ_LIB};${TPWC_LIB};${BOOST_LIB}")

# Shanghai SSE market-data plugin, loaded by source 89.
add_library(sse_md SHARED
  ${SZE_MD}/MDEngineSSE.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/sse-t0/market_data/sse_cpu_affinity.cpp
  ${SZE_MD}/common/UdpChannelRuntime.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/sse-t0/market_data/sse_primary_decoder.cpp)
target_include_directories(sse_md PRIVATE
  ${SZE_MD} ${SZE_MD}/common
  ${CMAKE_CURRENT_SOURCE_DIR}/sse-t0/market_data
  ${DEEPWIN_INCLUDE} ${DEEPWIN_INCLUDE}/longfist
  ${BOOST_INCLUDE} ${PYTHON_INCLUDE})
target_link_libraries(sse_md PRIVATE
  ${YJJ_LIB}/libjournal.so.1.1 ${YJJ_LIB}/libkflog.so
  ${WC_LIB}/libwingchunmd.so ${TPWC_LIB}/libpython3.6m.so.1.0
  ${TPWC_LIB}/liblog4cplus.so.0
  ${BOOST_LIB}/libboost_locale.so.1.62.0
  ${BOOST_LIB}/libboost_date_time.so.1.62.0
  ${BOOST_LIB}/libboost_filesystem.so.1.62.0
  ${BOOST_LIB}/libboost_system.so.1.62.0
  ${BOOST_LIB}/libboost_thread.so.1.62.0
  ${BOOST_LIB}/libboost_serialization.so.1.62.0
  ${BOOST_LIB}/libboost_math_tr1.so.1.62.0
  ${BOOST_LIB}/libboost_python.so.1.62.0
  ${BOOST_LIB}/libboost_chrono.so.1.62.0
  ${BOOST_LIB}/libboost_regex.so.1.62.0
  ${BOOST_LIB}/libboost_program_options.so.1.62.0
  ${BOOST_LIB}/libboost_atomic.so.1.62.0 pthread dl rt)
set_target_properties(sse_md PROPERTIES OUTPUT_NAME sse_md
  BUILD_RPATH "/home/zane/td_query_runtime_20260824/lib")

add_library(sze_l1_md SHARED
  ${SZE_MD}/MDEngineSZEL1.cpp ${SZE_MD}/SZEProtocol.cpp
  ${SZE_MD}/SZERecoverable.cpp)
target_include_directories(sze_l1_md PRIVATE
  ${SZE_MD} ${DEEPWIN_INCLUDE} ${DEEPWIN_INCLUDE}/longfist
  ${BOOST_INCLUDE} ${PYTHON_INCLUDE})
target_link_libraries(sze_l1_md PRIVATE
  ${YJJ_LIB}/libjournal.so.1.1 ${YJJ_LIB}/libkflog.so
  ${WC_LIB}/libwingchunmd.so ${TPWC_LIB}/libpython3.6m.so.1.0
  ${TPWC_LIB}/liblog4cplus.so.0
  ${BOOST_LIB}/libboost_locale.so.1.62.0
  ${BOOST_LIB}/libboost_date_time.so.1.62.0
  ${BOOST_LIB}/libboost_filesystem.so.1.62.0
  ${BOOST_LIB}/libboost_system.so.1.62.0
  ${BOOST_LIB}/libboost_thread.so.1.62.0
  ${BOOST_LIB}/libboost_serialization.so.1.62.0
  ${BOOST_LIB}/libboost_math_tr1.so.1.62.0
  ${BOOST_LIB}/libboost_python.so.1.62.0
  ${BOOST_LIB}/libboost_chrono.so.1.62.0
  ${BOOST_LIB}/libboost_regex.so.1.62.0
  ${BOOST_LIB}/libboost_program_options.so.1.62.0
  ${BOOST_LIB}/libboost_atomic.so.1.62.0 pthread dl rt)
set_target_properties(sze_l1_md PROPERTIES OUTPUT_NAME sze_l1_md
  BUILD_RPATH "${WC_LIB};${YJJ_LIB};${TPWC_LIB};${BOOST_LIB}")

enable_testing()
add_executable(sze_recoverable_test
  ${SZE_MD}/tests/sze_recoverable_test.cpp ${SZE_MD}/SZERecoverable.cpp)
target_include_directories(sze_recoverable_test PRIVATE ${SZE_MD})
target_link_libraries(sze_recoverable_test PRIVATE pthread rt)
target_compile_options(sze_recoverable_test PRIVATE -UNDEBUG)
add_test(NAME sze_recoverable_test COMMAND sze_recoverable_test)

add_executable(sze_health_state_test
  ${SZE_MD}/tests/sze_health_state_test.cpp
  ${SZE_MD}/SZEHealthState.cpp ${SZE_MD}/SZERecoverable.cpp)
target_include_directories(sze_health_state_test PRIVATE ${SZE_MD})
target_link_libraries(sze_health_state_test PRIVATE pthread rt)
target_compile_options(sze_health_state_test PRIVATE -UNDEBUG)
add_test(NAME sze_health_state_test COMMAND sze_health_state_test)

if(SZE_BUILD_DIRECT_SHARDED_RING)
  add_executable(sze_direct_sharded_ring_test
    ${SZE_MD}/tests/sze_direct_sharded_ring_test.cpp
    ${SZE_MD}/SZEDirectShardedRing.cpp ${SZE_MD}/SZERecoverable.cpp)
  target_include_directories(sze_direct_sharded_ring_test PRIVATE ${SZE_MD})
  target_link_libraries(sze_direct_sharded_ring_test PRIVATE pthread rt)
  target_compile_options(sze_direct_sharded_ring_test PRIVATE -UNDEBUG)
  add_test(NAME sze_direct_sharded_ring_test COMMAND sze_direct_sharded_ring_test)
endif()

add_executable(sze_protocol_test
  ${SZE_MD}/tests/sze_protocol_test.cpp ${SZE_MD}/SZEProtocol.cpp
  ${SZE_MD}/SZERecoverable.cpp)
target_include_directories(sze_protocol_test PRIVATE ${SZE_MD} ${DEEPWIN_INCLUDE})
target_link_libraries(sze_protocol_test PRIVATE pthread rt)
target_compile_options(sze_protocol_test PRIVATE -UNDEBUG)
add_test(NAME sze_protocol_test COMMAND sze_protocol_test)

add_executable(sze_config_guard_test
  ${T0}/tests/sze_config_guard_test.cpp ${T0}/deepwin_strategy/sze_config_guard.cpp)
target_include_directories(sze_config_guard_test PRIVATE ${T0})
add_test(NAME sze_config_guard_test COMMAND sze_config_guard_test)

add_executable(sze_position_risk_test
  ${T0}/tests/sze_position_risk_test.cpp)
target_include_directories(sze_position_risk_test PRIVATE ${T0})
add_test(NAME sze_position_risk_test COMMAND sze_position_risk_test)

add_executable(sse_config_guard_test
  ${CMAKE_CURRENT_SOURCE_DIR}/sse-t0/tests/sse_config_guard_test.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/sse-t0/strategy/sse_config_guard.cpp)
target_include_directories(sse_config_guard_test PRIVATE ${T0})
add_test(NAME sse_config_guard_test COMMAND sse_config_guard_test
  ${CMAKE_CURRENT_SOURCE_DIR}/sse-t0/config/config_sse_hybrid_prediction_20260818.json)

add_library(deepwin_market_data_runtime STATIC
  ${SZE_MD}/common/UdpChannelRuntime.cpp)
target_include_directories(deepwin_market_data_runtime PUBLIC
  ${SZE_MD}/common)
target_link_libraries(deepwin_market_data_runtime PUBLIC pthread)

add_executable(sse_udp_observer
  ${CMAKE_CURRENT_SOURCE_DIR}/sse-t0/market_data/sse_cpu_affinity.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/sse-t0/market_observer/sse_udp_observer.cpp)
target_link_libraries(sse_udp_observer PRIVATE deepwin_market_data_runtime)

add_library(sse_live_sampling STATIC
  ${CMAKE_CURRENT_SOURCE_DIR}/sse-t0/market_data/sse_batch_end_sampler.cpp)
target_include_directories(sse_live_sampling PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/sse-t0/market_data)
target_link_libraries(sse_live_sampling PUBLIC rt)

add_executable(sse_live_sampling_test
  ${CMAKE_CURRENT_SOURCE_DIR}/sse-t0/market_data/sse_batch_end_sampler_test.cpp)
target_link_libraries(sse_live_sampling_test PRIVATE sse_live_sampling)
target_compile_options(sse_live_sampling_test PRIVATE -UNDEBUG)
add_test(NAME sse_live_sampling_test COMMAND sse_live_sampling_test)

add_library(sse_model_runtime STATIC
  ${CMAKE_CURRENT_SOURCE_DIR}/sse-t0/model/sse_model_runtime.cpp)
target_include_directories(sse_model_runtime PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/sse-t0/model
  ${T0}/cpp_model/eigen3)
target_compile_options(sse_model_runtime PRIVATE -ffp-contract=off)
if(SSE_MODEL_MARCH_NATIVE)
  target_compile_options(sse_model_runtime PRIVATE -march=native)
endif()

add_executable(sse_model_runtime_test
  ${CMAKE_CURRENT_SOURCE_DIR}/sse-t0/model/sse_model_runtime_test.cpp)
target_link_libraries(sse_model_runtime_test PRIVATE sse_model_runtime)
add_test(NAME sse_model_runtime_test COMMAND sse_model_runtime_test)

add_executable(sse_model_sequence_probe
  ${CMAKE_CURRENT_SOURCE_DIR}/sse-t0/model/sse_model_sequence_probe.cpp)
target_link_libraries(sse_model_sequence_probe PRIVATE sse_model_runtime)

add_library(sse_snapshot_gru_runtime STATIC
  ${CMAKE_CURRENT_SOURCE_DIR}/sse-t0/model/snapshot_gru_runtime.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/sse-t0/model/snapshot_ensemble.cpp)
target_include_directories(sse_snapshot_gru_runtime PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/sse-t0/model
  ${T0} ${T0}/cpp_model)
target_compile_options(sse_snapshot_gru_runtime PRIVATE -ffp-contract=off)

add_executable(sse_snapshot_gru_test
  ${CMAKE_CURRENT_SOURCE_DIR}/sse-t0/model/snapshot_gru_test.cpp)
target_link_libraries(sse_snapshot_gru_test PRIVATE sse_snapshot_gru_runtime)
target_compile_options(sse_snapshot_gru_test PRIVATE -UNDEBUG)
add_test(NAME sse_snapshot_gru_test COMMAND sse_snapshot_gru_test)

add_library(sse_hybrid_model_runtime STATIC
  ${CMAKE_CURRENT_SOURCE_DIR}/sse-t0/model/sse_hybrid_model.cpp)
target_include_directories(sse_hybrid_model_runtime PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/sse-t0/model
  ${T0} ${T0}/cpp_model)
target_link_libraries(sse_hybrid_model_runtime PUBLIC
  sse_model_runtime sse_snapshot_gru_runtime)
target_compile_options(sse_hybrid_model_runtime PRIVATE -ffp-contract=off)

add_executable(sse_hybrid_model_test
  ${CMAKE_CURRENT_SOURCE_DIR}/sse-t0/model/sse_hybrid_model_test.cpp)
target_link_libraries(sse_hybrid_model_test PRIVATE sse_hybrid_model_runtime)
target_compile_options(sse_hybrid_model_test PRIVATE -UNDEBUG)
add_test(NAME sse_hybrid_model_test COMMAND sse_hybrid_model_test)

add_executable(sse_udp_observer_test
  ${CMAKE_CURRENT_SOURCE_DIR}/sse-t0/market_observer/sse_udp_observer_test.cpp)
add_test(NAME sse_udp_observer_test COMMAND sse_udp_observer_test)
add_executable(sse_cpu_affinity_test
  ${CMAKE_CURRENT_SOURCE_DIR}/sse-t0/tests/sse_cpu_affinity_test.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/sse-t0/market_data/sse_cpu_affinity.cpp)
add_test(NAME sse_cpu_affinity_test COMMAND sse_cpu_affinity_test)
add_library(sse_event_runtime STATIC
  ${CMAKE_CURRENT_SOURCE_DIR}/sse-t0/market_data/sse_event_codec.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/sse-t0/market_data/sse_event_pipeline.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/sse-t0/market_data/sse_event_journal.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/sse-t0/market_data/sse_shard_plan.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/sse-t0/market_data/sse_cpu_affinity.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/sse-t0/market_data/sse_primary_decoder.cpp
  ${SZE_MD}/SZERecoverable.cpp)
target_link_libraries(sse_event_runtime PUBLIC pthread)
add_library(sse_event_model_consumer STATIC
  ${CMAKE_CURRENT_SOURCE_DIR}/sse-t0/strategy/sse_model_consumer.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/sse-t0/strategy/sse_tick_prediction_engine.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/sse-t0/strategy/sse_tick_sample_gate.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/sse-t0/market_data/sse_tick_order_book.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/sse-t0/market_data/sse_tick_factors.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/sse-t0/market_data/sse_tick_static_metadata.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/sse-t0/model/snapshot36.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/sse-t0/model/auction59_sidecar.cpp)
target_include_directories(sse_event_model_consumer PRIVATE ${T0})
target_link_libraries(sse_event_model_consumer PUBLIC sse_event_runtime sse_model_runtime sse_snapshot_gru_runtime)
add_executable(sse_event_pipeline
  ${CMAKE_CURRENT_SOURCE_DIR}/sse-t0/market_observer/sse_event_pipeline_main.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/sse-t0/market_data/sse_history_input.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/sse-t0/market_data/sse_tick_order_book.cpp)
target_link_libraries(sse_event_pipeline PRIVATE sse_event_runtime sse_event_model_consumer deepwin_market_data_runtime)
foreach(test_name sse_shard_plan sse_event_queue sse_event_journal sse_event_pipeline)
  add_executable(${test_name}_test ${CMAKE_CURRENT_SOURCE_DIR}/sse-t0/tests/${test_name}_test.cpp)
  target_link_libraries(${test_name}_test PRIVATE sse_event_runtime)
  add_test(NAME ${test_name}_test COMMAND ${test_name}_test)
endforeach()
add_executable(sse_latency_histogram_test ${CMAKE_CURRENT_SOURCE_DIR}/sse-t0/tests/sse_latency_histogram_test.cpp)
add_test(NAME sse_latency_histogram_test COMMAND sse_latency_histogram_test)
add_executable(sse_event_sampling_test ${CMAKE_CURRENT_SOURCE_DIR}/sse-t0/tests/sse_event_sampling_test.cpp)
target_link_libraries(sse_event_sampling_test PRIVATE sse_event_runtime)
add_test(NAME sse_event_sampling_test COMMAND sse_event_sampling_test)
add_executable(sse_model_consumer_test ${CMAKE_CURRENT_SOURCE_DIR}/sse-t0/tests/sse_model_consumer_test.cpp)
target_link_libraries(sse_model_consumer_test PRIVATE sse_event_model_consumer)
set(SSE_TEST_TICK_MODEL "" CACHE FILEPATH "Shanghai real-model test weights")
set(SSE_TEST_STATIC_JSON "" CACHE FILEPATH "Shanghai real-model test daily metadata")
set(SSE_TEST_TRADING_DAY "" CACHE STRING "Shanghai real-model test trading day")
if(SSE_TEST_TICK_MODEL AND SSE_TEST_STATIC_JSON AND SSE_TEST_TRADING_DAY)
  add_test(NAME sse_model_consumer_test COMMAND sse_model_consumer_test
    ${SSE_TEST_TICK_MODEL} ${SSE_TEST_STATIC_JSON} ${SSE_TEST_TRADING_DAY})
endif()
find_program(SSE_AFFINITY_TEST_PYTHON NAMES python3 python3.6m python)
add_test(NAME sse_event_pipeline_integration_test
  COMMAND ${SSE_AFFINITY_TEST_PYTHON}
  ${CMAKE_CURRENT_SOURCE_DIR}/sse-t0/tests/sse_event_pipeline_integration_test.py
  $<TARGET_FILE:sse_event_pipeline>)
add_test(NAME sse_cpu_affinity_integration_test
  COMMAND ${SSE_AFFINITY_TEST_PYTHON}
  ${CMAKE_CURRENT_SOURCE_DIR}/sse-t0/tests/sse_cpu_affinity_integration_test.py
  $<TARGET_FILE:sse_udp_observer>)
add_test(NAME sse_udp_observer_offline_test
  COMMAND /usr/bin/python3.6m
  ${CMAKE_CURRENT_SOURCE_DIR}/sse-t0/market_observer/sse_udp_observer_offline_test.py
  $<TARGET_FILE:sse_udp_observer>)

add_executable(sze_prediction_arbiter_test
  ${T0}/tests/sze_prediction_arbiter_test.cpp)
target_include_directories(sze_prediction_arbiter_test PRIVATE ${T0})
add_test(NAME sze_prediction_arbiter_test COMMAND sze_prediction_arbiter_test)

add_executable(sze_recovery_status
  ${SZE_MD}/tools/sze_recovery_status.cpp ${SZE_MD}/SZERecoverable.cpp
  ${SZE_MD}/SZEHealthState.cpp)
target_include_directories(sze_recovery_status PRIVATE ${SZE_MD})
target_link_libraries(sze_recovery_status PRIVATE pthread rt)

add_executable(sze_health_status
  ${SZE_MD}/tools/sze_health_status.cpp
  ${SZE_MD}/SZEHealthState.cpp ${SZE_MD}/SZERecoverable.cpp)
target_include_directories(sze_health_status PRIVATE ${SZE_MD})
target_link_libraries(sze_health_status PRIVATE pthread rt)

if(SZE_BUILD_DIRECT_SHARDED_RING)
  add_executable(sze_sharded_ring_benchmark
    ${SZE_MD}/tools/sze_sharded_ring_benchmark.cpp
    ${SZE_MD}/SZEDirectShardedRing.cpp ${SZE_MD}/SZERecoverable.cpp)
  target_include_directories(sze_sharded_ring_benchmark PRIVATE ${SZE_MD})
  target_link_libraries(sze_sharded_ring_benchmark PRIVATE pthread rt)
endif()

add_executable(sze_recovery_verify
  ${SZE_MD}/tools/sze_recovery_verify.cpp ${SZE_MD}/SZEProtocol.cpp
  ${SZE_MD}/SZERecoverable.cpp)
target_include_directories(sze_recovery_verify PRIVATE ${SZE_MD} ${DEEPWIN_INCLUDE})
target_link_libraries(sze_recovery_verify PRIVATE pthread rt)

# SSE merged trading-main helpers: catch-up gate + async prediction log.
add_executable(sse_trading_gate_test
  ${T0}/tests/sse_trading_gate_test.cpp)
target_include_directories(sse_trading_gate_test PRIVATE ${T0}/strategy)
target_compile_options(sse_trading_gate_test PRIVATE -UNDEBUG)
add_test(NAME sse_trading_gate_test COMMAND sse_trading_gate_test)

add_executable(async_prediction_log_test
  ${T0}/tests/async_prediction_log_test.cpp)
target_include_directories(async_prediction_log_test PRIVATE ${T0}/strategy)
target_link_libraries(async_prediction_log_test PRIVATE pthread)
target_compile_options(async_prediction_log_test PRIVATE -UNDEBUG)
add_test(NAME async_prediction_log_test COMMAND async_prediction_log_test)
