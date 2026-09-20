# The deployed ATP query bundle keeps the same runtime libraries in one lib/
# directory. Accept that existing layout without manufacturing symlink trees.
if(EXISTS "${RUNTIME}/lib/libwingchuntd.so")
  set(WC_LIB "${RUNTIME}/lib")
  set(YJJ_LIB "${RUNTIME}/lib")
  set(TPWC_LIB "${RUNTIME}/lib")
  set(BOOST_LIB "${RUNTIME}/lib")
endif()

set(SZE_COMMON_SOURCES
  ${USAGI_ROOT}/common/strategy/common.cpp
  ${USAGI_ROOT}/common/market_data/legacy/snap_generator.cpp
  ${USAGI_ROOT}/common/market_data/legacy/order_match_engine.cpp
  ${USAGI_ROOT}/sze/market_data/sz_hp_orderbook.cpp
  ${USAGI_ROOT}/sze/market_data/sz_hp_realtime_state.cpp
  ${USAGI_ROOT}/sze/factors/sz_hp_factor_adapter.cpp
  ${USAGI_ROOT}/sze/market_data/sz_hp_latency.cpp
  ${USAGI_ROOT}/common/market_data/legacy/shsz_full_orderbook_engine.cpp
  ${USAGI_ROOT}/common/market_data/legacy/shsz_full_orderbook_diagnostics.cpp
  ${USAGI_ROOT}/sze/factors/shsz_full_orderbook_factor.cpp
  ${USAGI_ROOT}/common/market_data/legacy/shsz_full_orderbook_manager.cpp
  ${USAGI_ROOT}/sze/runtime/shsz_predictor_transition_adapter.cpp
  ${USAGI_ROOT}/common/market_data/legacy/order_list.cpp
  ${USAGI_ROOT}/common/factors/legacy/md_utils.cpp
  ${USAGI_ROOT}/common/factors/legacy/factor.cpp
  ${USAGI_ROOT}/adapters/deepwin/legacy/predictor/predictor.cpp
  ${USAGI_ROOT}/common/model/mix153060/mix153060_model.cpp
  ${USAGI_ROOT}/sze/runtime/mix153060_capture.cpp
  ${USAGI_ROOT}/sze/sampling/mix153060_runtime.cpp
  ${USAGI_ROOT}/sze/runtime/mix153060_live_adapter.cpp
  ${USAGI_ROOT}/common/market_data/legacy/snap_latency.cpp
  ${USAGI_ROOT}/adapters/deepwin/legacy/bse_latency.cpp
  ${USAGI_ROOT}/adapters/deepwin/strategy/StrategyBase.cpp
  ${USAGI_ROOT}/common/strategy/ZStrategy.cpp
  ${USAGI_ROOT}/adapters/deepwin/strategy/StrategyExecutionFramework.cpp
  ${USAGI_ROOT}/adapters/deepwin/sze/sze_config_guard.cpp
  ${USAGI_ROOT}/adapters/deepwin/sze/sze_get_obj.cpp
  ${USAGI_ROOT}/reference/snapshot_legacy15/snapshot_legacy15_factors.cpp
  ${USAGI_ROOT}/reference/snapshot_legacy15/snapshot_legacy15_model.cpp
  ${USAGI_ROOT}/sze/market_data/SZEProtocol.cpp
  ${USAGI_ROOT}/sze/market_data/SZERecoverable.cpp
  ${USAGI_ROOT}/sze/market_data/SZEHealthState.cpp)

set(SSE_COMMON_SOURCES ${SZE_COMMON_SOURCES})
list(REMOVE_ITEM SSE_COMMON_SOURCES ${USAGI_ROOT}/adapters/deepwin/sze/sze_get_obj.cpp)
list(APPEND SSE_COMMON_SOURCES
  ${USAGI_ROOT}/adapters/deepwin/sse/sse_config_guard.cpp
  ${USAGI_ROOT}/adapters/deepwin/sse/sse_get_obj.cpp)

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

if(USAGI_BUILD_DEEPWIN)
  add_library(t0_strategy_sze SHARED ${SZE_COMMON_SOURCES})
  target_compile_definitions(t0_strategy_sze PRIVATE T0_USE_DEEPWIN T0_SZE_STRATEGY_ONLY)
  target_include_directories(t0_strategy_sze PRIVATE
    ${USAGI_ROOT} ${USAGI_ROOT}/common/strategy ${USAGI_ROOT}/common/model/mix153060 ${USAGI_ROOT}/sze/market_data
    ${USAGI_ROOT}/common/model/legacy
    ${USAGI_ROOT}/reference/snapshot_legacy15
    ${DEEPWIN_INCLUDE} ${DEEPWIN_INCLUDE}/longfist ${BOOST_INCLUDE} ${PYTHON_INCLUDE})
  target_link_libraries(t0_strategy_sze PRIVATE ${DEEPWIN_LIBS})
  set_target_properties(t0_strategy_sze PROPERTIES
    OUTPUT_NAME t0_strategy_sze
    BUILD_RPATH "${WC_LIB};${YJJ_LIB};${TPWC_LIB};${BOOST_LIB}")
  set_source_files_properties(${USAGI_ROOT}/sze/sampling/mix153060_runtime.cpp
    PROPERTIES COMPILE_OPTIONS -ffp-contract=off)
  
  add_library(t0_strategy_sse SHARED ${SSE_COMMON_SOURCES})
  target_compile_definitions(t0_strategy_sse PRIVATE T0_USE_DEEPWIN)
  target_include_directories(t0_strategy_sse PRIVATE ${USAGI_ROOT} ${USAGI_ROOT}/common/strategy ${USAGI_ROOT}/common/model/mix153060 ${USAGI_ROOT}/sze/market_data ${USAGI_ROOT}/common/model/legacy ${USAGI_ROOT}/reference/snapshot_legacy15 ${DEEPWIN_INCLUDE} ${DEEPWIN_INCLUDE}/longfist ${BOOST_INCLUDE} ${PYTHON_INCLUDE})
  target_link_libraries(t0_strategy_sse PRIVATE ${DEEPWIN_LIBS})
  set_target_properties(t0_strategy_sse PROPERTIES OUTPUT_NAME t0_strategy_sse BUILD_RPATH "${WC_LIB};${YJJ_LIB};${TPWC_LIB};${BOOST_LIB}")
  
  add_library(sze_md SHARED
    ${USAGI_ROOT}/adapters/deepwin/md/MDEngineSZE.cpp ${USAGI_ROOT}/sze/market_data/SZEProtocol.cpp
    ${USAGI_ROOT}/sze/market_data/SZERecoverable.cpp ${USAGI_ROOT}/sze/market_data/SZEHealthState.cpp)
  target_include_directories(sze_md PRIVATE
    ${USAGI_ROOT}/sze/market_data ${DEEPWIN_INCLUDE} ${DEEPWIN_INCLUDE}/longfist
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
    ${USAGI_ROOT}/adapters/deepwin/md/MDEngineSZEL1.cpp ${USAGI_ROOT}/sze/market_data/SZEProtocol.cpp
    ${USAGI_ROOT}/sze/market_data/SZERecoverable.cpp)
  target_include_directories(sze_l1_md PRIVATE
    ${USAGI_ROOT}/sze/market_data ${DEEPWIN_INCLUDE} ${DEEPWIN_INCLUDE}/longfist
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
endif()

if(SZE_BUILD_TD)
  if(NOT EXISTS "${SZE_TD_API_DIR}/include/atp_quant_api.h")
    message(FATAL_ERROR "SZE_BUILD_TD requires ${SZE_TD_API_DIR}/include/atp_quant_api.h")
  endif()
  if(NOT EXISTS "${SZE_TD_API_DIR}/lib/libatpquantapi.so")
    message(FATAL_ERROR "SZE_BUILD_TD requires vendor ${SZE_TD_API_DIR}/lib/libatpquantapi.so")
  endif()
  add_library(sze_td SHARED
    ${USAGI_ROOT}/adapters/td/atp/TDEngineGXBSE.cpp
    ${USAGI_ROOT}/adapters/td/atp/TDEngineGXBSEOms.cpp)
  target_compile_definitions(sze_td PRIVATE
    GXBSE_TD_BUILD_VERSION="sze-dev"
    T0_TD_PLUGIN_LABEL="sze_td"
    T0_TD_SOURCE_ID=180
    T0_TD_ENGINE_KEY="sze_td"
    T0_TD_DEFAULT_MARKET_ID=102)
  target_include_directories(sze_td PRIVATE
    ${USAGI_ROOT}/adapters/td/atp ${SZE_TD_API_DIR}/include
    ${DEEPWIN_INCLUDE} ${DEEPWIN_INCLUDE}/longfist
    ${BOOST_INCLUDE} ${PYTHON_INCLUDE})
  target_link_libraries(sze_td PRIVATE
    t0_oms_core
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
  if(NOT EXISTS "${SZE_TD_API_DIR}/include/atp_quant_msg.h")
    message(FATAL_ERROR "SSE_BUILD_TD requires ${SZE_TD_API_DIR}/include/atp_quant_msg.h")
  endif()
  if(NOT EXISTS "${SZE_TD_API_DIR}/lib/libatpquantapi.so")
    message(FATAL_ERROR "SSE_BUILD_TD requires ${SZE_TD_API_DIR}/lib/libatpquantapi.so")
  endif()
  add_library(sse_td SHARED
    ${USAGI_ROOT}/adapters/td/atp/TDEngineGXBSE.cpp
    ${USAGI_ROOT}/adapters/td/atp/TDEngineGXBSEOms.cpp)
  target_compile_definitions(sse_td PRIVATE
    GXBSE_TD_BUILD_VERSION="sse-dev"
    SSE_TD_BUILD=1
    T0_TD_PLUGIN_LABEL="sse_td"
    T0_TD_SOURCE_ID=190
    T0_TD_ENGINE_KEY="sse_td"
    T0_TD_DEFAULT_MARKET_ID=101)
  target_include_directories(sse_td PRIVATE
    ${USAGI_ROOT}/adapters/td/atp ${SZE_TD_API_DIR}/include
    ${DEEPWIN_INCLUDE} ${DEEPWIN_INCLUDE}/longfist
    ${BOOST_INCLUDE} ${PYTHON_INCLUDE})
  target_link_libraries(sse_td PRIVATE
    t0_oms_core
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
    INSTALL_RPATH "\$ORIGIN;${SZE_TD_API_DIR}/lib;${WC_LIB};${YJJ_LIB};${TPWC_LIB};${BOOST_LIB}")
endif()

# Exercise real SDK request encoding and the complete TD adapter without any
# network connection. Only atp_send_path_test interposes the gateway send calls.
if(TARGET sse_td)
  add_executable(atp_request_reuse_test ${USAGI_ROOT}/tests/oms/atp_request_reuse_test.cpp)
  target_include_directories(atp_request_reuse_test PRIVATE ${SZE_TD_API_DIR}/include)
  target_link_libraries(atp_request_reuse_test PRIVATE ${SZE_TD_API_DIR}/lib/libatpquantapi.so)
  add_test(NAME atp_request_reuse_test COMMAND atp_request_reuse_test)
  get_target_property(atp_test_includes sse_td INCLUDE_DIRECTORIES)
  get_target_property(atp_test_libraries sse_td LINK_LIBRARIES)
  add_executable(atp_send_path_test ${USAGI_ROOT}/tests/oms/atp_send_path_test.cpp)
  target_include_directories(atp_send_path_test PRIVATE ${atp_test_includes})
  target_link_libraries(atp_send_path_test PRIVATE sse_td ${atp_test_libraries} ${TPWC_LIB}/libpython3.6m.so.1.0)
  add_test(NAME atp_send_path_test COMMAND atp_send_path_test)
  set_tests_properties(atp_request_reuse_test atp_send_path_test PROPERTIES TIMEOUT 30)
endif()
