cmake_minimum_required(VERSION 3.10)
project(sze_dev_debug CXX)
include_directories(${CMAKE_CURRENT_SOURCE_DIR})

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
option(SZE_MARCH_NATIVE "Compile for the build machine CPU" OFF)
option(SZE_BUILD_TD "Build the Shenzhen ATP TD plugin" OFF)
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
  ${T0}/strategy/ZStrategyV06.cpp
  ${T0}/strategy/V06StrategyRules.cpp
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
  ${CMAKE_CURRENT_SOURCE_DIR}/sse-t0/strategy/sse_get_obj.cpp)

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
  ${T0} ${T0}/strategy ${T0}/predictor ${SZE_MD}
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
target_include_directories(t0_strategy_sse PRIVATE ${T0} ${T0}/strategy ${T0}/predictor ${SZE_MD} ${T0}/cpp_model ${CMAKE_CURRENT_SOURCE_DIR}/reference/snapshot_legacy15 ${DEEPWIN_INCLUDE} ${DEEPWIN_INCLUDE}/longfist ${BOOST_INCLUDE} ${PYTHON_INCLUDE})
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

add_library(sze_md SHARED
  ${SZE_MD}/MDEngineSZE.cpp ${SZE_MD}/SZEProtocol.cpp
  ${SZE_MD}/SZERecoverable.cpp ${SZE_MD}/SZEHealthState.cpp
  ${SZE_MD}/SZEDirectShardedRing.cpp)
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
add_executable(v06_live_execution_test
  ${T0}/strategy/v06_live_execution_test.cpp ${T0}/strategy/V06StrategyRules.cpp)
target_compile_options(v06_live_execution_test PRIVATE -UNDEBUG)
add_test(NAME v06_live_execution_test COMMAND v06_live_execution_test)
add_executable(sze_book_runtime_test
  ${T0}/strategy/book_runtime_test.cpp ${T0}/predictor/mix153060_runtime.cpp)
target_compile_options(sze_book_runtime_test PRIVATE -UNDEBUG -ffp-contract=off)
add_test(NAME sze_book_runtime_test COMMAND sze_book_runtime_test)
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

add_executable(sze_direct_sharded_ring_test
  ${SZE_MD}/tests/sze_direct_sharded_ring_test.cpp
  ${SZE_MD}/SZEDirectShardedRing.cpp ${SZE_MD}/SZERecoverable.cpp)
target_include_directories(sze_direct_sharded_ring_test PRIVATE ${SZE_MD})
target_link_libraries(sze_direct_sharded_ring_test PRIVATE pthread rt)
target_compile_options(sze_direct_sharded_ring_test PRIVATE -UNDEBUG)
add_test(NAME sze_direct_sharded_ring_test COMMAND sze_direct_sharded_ring_test)

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
add_test(NAME sse_config_guard_test COMMAND sse_config_guard_test)

add_library(deepwin_market_data_runtime STATIC
  ${SZE_MD}/common/UdpChannelRuntime.cpp)
target_include_directories(deepwin_market_data_runtime PUBLIC
  ${SZE_MD}/common)
target_link_libraries(deepwin_market_data_runtime PUBLIC pthread)

add_executable(sse_udp_observer
  ${CMAKE_CURRENT_SOURCE_DIR}/sse-t0/market_observer/sse_udp_observer.cpp)
target_link_libraries(sse_udp_observer PRIVATE deepwin_market_data_runtime)

add_library(sse_model_runtime STATIC
  ${CMAKE_CURRENT_SOURCE_DIR}/sse-t0/model/sse_model_runtime.cpp)
target_include_directories(sse_model_runtime PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/sse-t0/model
  ${T0}/cpp_model/eigen3)
target_compile_options(sse_model_runtime PRIVATE -ffp-contract=off)

add_executable(sse_model_runtime_test
  ${CMAKE_CURRENT_SOURCE_DIR}/sse-t0/model/sse_model_runtime_test.cpp)
target_link_libraries(sse_model_runtime_test PRIVATE sse_model_runtime)
add_test(NAME sse_model_runtime_test COMMAND sse_model_runtime_test)

add_executable(sse_model_sequence_probe
  ${CMAKE_CURRENT_SOURCE_DIR}/sse-t0/model/sse_model_sequence_probe.cpp)
target_link_libraries(sse_model_sequence_probe PRIVATE sse_model_runtime)

add_executable(sse_udp_observer_test
  ${CMAKE_CURRENT_SOURCE_DIR}/sse-t0/market_observer/sse_udp_observer_test.cpp)
add_test(NAME sse_udp_observer_test COMMAND sse_udp_observer_test)
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

add_executable(sze_sharded_ring_benchmark
  ${SZE_MD}/tools/sze_sharded_ring_benchmark.cpp
  ${SZE_MD}/SZEDirectShardedRing.cpp ${SZE_MD}/SZERecoverable.cpp)
target_include_directories(sze_sharded_ring_benchmark PRIVATE ${SZE_MD})
target_link_libraries(sze_sharded_ring_benchmark PRIVATE pthread rt)

add_executable(sze_recovery_verify
  ${SZE_MD}/tools/sze_recovery_verify.cpp ${SZE_MD}/SZEProtocol.cpp
  ${SZE_MD}/SZERecoverable.cpp)
target_include_directories(sze_recovery_verify PRIVATE ${SZE_MD} ${DEEPWIN_INCLUDE})
target_link_libraries(sze_recovery_verify PRIVATE pthread rt)
