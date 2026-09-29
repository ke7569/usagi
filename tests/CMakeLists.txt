add_executable(receive_timestamps_test ${USAGI_ROOT}/tests/common/receive_timestamps_test.cpp)
add_test(NAME receive_timestamps_test COMMAND receive_timestamps_test)

add_executable(external_execution_test ${USAGI_ROOT}/tests/common/external_execution_test.cpp)
target_link_libraries(external_execution_test PRIVATE t0_strategy_core)
target_compile_options(external_execution_test PRIVATE -UNDEBUG)
add_test(NAME external_execution_test COMMAND external_execution_test)

add_executable(sse_journal_timestamp_test ${USAGI_ROOT}/tests/sse/sse_journal_timestamp_test.cpp)
target_link_libraries(sse_journal_timestamp_test PRIVATE sse_journal_core deepwin_market_data_runtime)
add_test(NAME sse_journal_timestamp_test COMMAND sse_journal_timestamp_test)
set_tests_properties(sse_journal_timestamp_test PROPERTIES TIMEOUT 30)

foreach(oms_test_name oms_test oms_recovery_test journal_test atp_boundary_test atp_snapshot_collector_test async_journal_test)
  add_executable(${oms_test_name} ${USAGI_ROOT}/tests/oms/${oms_test_name}.cpp)
  target_include_directories(${oms_test_name} PRIVATE ${BOOST_INCLUDE})
  target_link_libraries(${oms_test_name} PRIVATE t0_oms_core)
  add_test(NAME ${oms_test_name} COMMAND ${oms_test_name})
  set_tests_properties(${oms_test_name} PROPERTIES TIMEOUT 30)
endforeach()

# Test-only TD shared objects have no SDK or production account dependency.
if(T0_BUILD_SSE_STREAM_PROCESSOR)
  add_library(fake_live_td_plugin SHARED ${USAGI_ROOT}/tests/oms/fake_live_td_plugin.cpp)
  target_compile_options(fake_live_td_plugin PRIVATE -fno-gnu-unique)
  target_link_libraries(fake_live_td_plugin PRIVATE pthread)
  add_library(missing_live_td_plugin SHARED ${USAGI_ROOT}/tests/oms/fake_live_td_plugin.cpp)
  target_compile_definitions(missing_live_td_plugin PRIVATE USAGI_TEST_MISSING_TD_ENTRYPOINT)
  target_compile_options(missing_live_td_plugin PRIVATE -fno-gnu-unique)
  target_link_libraries(missing_live_td_plugin PRIVATE pthread)
  add_executable(live_td_plugin_test ${USAGI_ROOT}/tests/oms/live_td_plugin_test.cpp)
  target_link_libraries(live_td_plugin_test PRIVATE sse_strategy_session pthread dl)
  add_dependencies(live_td_plugin_test fake_live_td_plugin missing_live_td_plugin)
  add_test(NAME live_td_plugin_test COMMAND live_td_plugin_test
    $<TARGET_FILE:fake_live_td_plugin> $<TARGET_FILE:missing_live_td_plugin>)
  set_tests_properties(live_td_plugin_test PROPERTIES TIMEOUT 30)
  add_executable(sse_live_td_gate_test ${USAGI_ROOT}/tests/sse/sse_live_td_gate_test.cpp)
  target_include_directories(sse_live_td_gate_test PRIVATE ${BOOST_INCLUDE})
  target_link_libraries(sse_live_td_gate_test PRIVATE sse_strategy_session sse_runtime_support)
  add_dependencies(sse_live_td_gate_test fake_live_td_plugin)
  add_test(NAME sse_live_td_gate_test COMMAND sse_live_td_gate_test $<TARGET_FILE:fake_live_td_plugin>)
  set_tests_properties(sse_live_td_gate_test PROPERTIES TIMEOUT 30)
endif()

# OMS merge item regression: Engine report idempotency, has_working_order and
# AtpBackend closed loop (see docs/oms-merge-oms1-2-20260908.md).
add_executable(oms_merge_test ${USAGI_ROOT}/tests/oms/oms_merge_test.cpp)
target_include_directories(oms_merge_test PRIVATE ${BOOST_INCLUDE})
target_link_libraries(oms_merge_test PRIVATE t0_oms_core)
add_test(NAME oms_merge_test COMMAND oms_merge_test)
set_tests_properties(oms_merge_test PROPERTIES TIMEOUT 30)

add_executable(oms_benchmark EXCLUDE_FROM_ALL ${USAGI_ROOT}/tests/oms/oms_benchmark.cpp)
target_link_libraries(oms_benchmark PRIVATE t0_oms_core)

add_executable(oms_benchmark_profiled EXCLUDE_FROM_ALL ${USAGI_ROOT}/tests/oms/oms_benchmark.cpp)
target_link_libraries(oms_benchmark_profiled PRIVATE t0_oms_profiled_core)
add_executable(oms_profile_test ${USAGI_ROOT}/tests/oms/profile_test.cpp)
target_link_libraries(oms_profile_test PRIVATE t0_oms_profiled_core)
add_test(NAME oms_profile_test COMMAND oms_profile_test)
set_tests_properties(oms_profile_test PROPERTIES TIMEOUT 30)
if(T0_STREAM_PYTHON)
  add_dependencies(oms_profile_test oms_benchmark oms_benchmark_profiled)
  add_test(NAME oms_benchmark_contract_test COMMAND ${T0_STREAM_PYTHON} -B
    ${USAGI_ROOT}/tests/oms/test_benchmark_contract.py
    $<TARGET_FILE:oms_benchmark> $<TARGET_FILE:oms_benchmark_profiled>)
  set_tests_properties(oms_benchmark_contract_test PROPERTIES TIMEOUT 60)
endif()
foreach(oms_test_name oms_test oms_recovery_test)
  add_executable(${oms_test_name}_profiled ${USAGI_ROOT}/tests/oms/${oms_test_name}.cpp)
  target_link_libraries(${oms_test_name}_profiled PRIVATE t0_oms_profiled_core)
  add_test(NAME ${oms_test_name}_profiled COMMAND ${oms_test_name}_profiled)
  set_tests_properties(${oms_test_name}_profiled PROPERTIES TIMEOUT 30)
endforeach()

add_executable(zstrategy_execution_test ${USAGI_ROOT}/tests/common/zstrategy_execution_test.cpp)
target_link_libraries(zstrategy_execution_test PRIVATE t0_strategy_core)
target_compile_options(zstrategy_execution_test PRIVATE -UNDEBUG)
add_test(NAME zstrategy_execution_test COMMAND zstrategy_execution_test)
add_executable(stream_strategy_execution_test ${USAGI_ROOT}/tests/common/stream_strategy_execution_test.cpp)
target_link_libraries(stream_strategy_execution_test PRIVATE t0_strategy_core)
add_test(NAME stream_strategy_execution_test COMMAND stream_strategy_execution_test)
add_executable(account_reconciliation_test ${USAGI_ROOT}/tests/common/account_reconciliation_test.cpp)
target_link_libraries(account_reconciliation_test PRIVATE t0_strategy_core)
add_test(NAME account_reconciliation_test COMMAND account_reconciliation_test)
add_executable(strategy_session_test ${USAGI_ROOT}/tests/common/strategy_session_test.cpp)
target_link_libraries(strategy_session_test PRIVATE t0_strategy_core)
target_compile_options(strategy_session_test PRIVATE -UNDEBUG)
add_test(NAME strategy_session_test COMMAND strategy_session_test)

if(T0_BUILD_SZE_STREAM_PROCESSOR)
  add_executable(sze_recoverable_test
    ${USAGI_ROOT}/tests/sze/sze_recoverable_test.cpp ${USAGI_ROOT}/common/recovery/SZERecoverable.cpp)
  target_include_directories(sze_recoverable_test PRIVATE ${USAGI_ROOT}/sze/market_data)
  target_link_libraries(sze_recoverable_test PRIVATE pthread rt)
  target_compile_options(sze_recoverable_test PRIVATE -UNDEBUG)
  add_test(NAME sze_recoverable_test COMMAND sze_recoverable_test)
  
  add_executable(sze_health_state_test
    ${USAGI_ROOT}/tests/sze/sze_health_state_test.cpp
    ${USAGI_ROOT}/sze/market_data/SZEHealthState.cpp ${USAGI_ROOT}/common/recovery/SZERecoverable.cpp)
  target_include_directories(sze_health_state_test PRIVATE ${USAGI_ROOT}/sze/market_data)
  target_link_libraries(sze_health_state_test PRIVATE pthread rt)
  target_compile_options(sze_health_state_test PRIVATE -UNDEBUG)
  add_test(NAME sze_health_state_test COMMAND sze_health_state_test)
  
  if(SZE_BUILD_DIRECT_SHARDED_RING)
    add_executable(sze_direct_sharded_ring_test
      ${USAGI_ROOT}/tests/sze/sze_direct_sharded_ring_test.cpp
      ${USAGI_ROOT}/sze/market_data/SZEDirectShardedRing.cpp ${USAGI_ROOT}/common/recovery/SZERecoverable.cpp)
    target_include_directories(sze_direct_sharded_ring_test PRIVATE ${USAGI_ROOT}/sze/market_data)
    target_link_libraries(sze_direct_sharded_ring_test PRIVATE pthread rt)
    target_compile_options(sze_direct_sharded_ring_test PRIVATE -UNDEBUG)
    add_test(NAME sze_direct_sharded_ring_test COMMAND sze_direct_sharded_ring_test)
  endif()
  
  add_executable(sze_protocol_test
    ${USAGI_ROOT}/tests/sze/sze_protocol_test.cpp ${USAGI_ROOT}/sze/market_data/SZEProtocol.cpp
    ${USAGI_ROOT}/common/recovery/SZERecoverable.cpp)
  target_include_directories(sze_protocol_test PRIVATE ${USAGI_ROOT}/sze/market_data ${DEEPWIN_INCLUDE})
  target_link_libraries(sze_protocol_test PRIVATE pthread rt)
  target_compile_options(sze_protocol_test PRIVATE -UNDEBUG)
  add_test(NAME sze_protocol_test COMMAND sze_protocol_test)
  
  add_executable(sze_config_guard_test
    ${USAGI_ROOT}/tests/sze/sze_config_guard_test.cpp ${USAGI_ROOT}/adapters/deepwin/sze/sze_config_guard.cpp)
  target_include_directories(sze_config_guard_test PRIVATE ${USAGI_ROOT})
  add_test(NAME sze_config_guard_test COMMAND sze_config_guard_test)
  
  add_executable(sze_position_risk_test
    ${USAGI_ROOT}/tests/sze/sze_position_risk_test.cpp)
  target_include_directories(sze_position_risk_test PRIVATE ${USAGI_ROOT})
  add_test(NAME sze_position_risk_test COMMAND sze_position_risk_test)
  
  add_executable(sze_prediction_arbiter_test
    ${USAGI_ROOT}/tests/sze/sze_prediction_arbiter_test.cpp)
  target_include_directories(sze_prediction_arbiter_test PRIVATE ${USAGI_ROOT})
  add_test(NAME sze_prediction_arbiter_test COMMAND sze_prediction_arbiter_test)
endif()

if(T0_BUILD_SSE_STREAM_PROCESSOR)
  add_executable(sse_crc_ring_test ${USAGI_ROOT}/tests/sse/sse_crc_ring_test.cpp)
  target_include_directories(sse_crc_ring_test PRIVATE ${BOOST_INCLUDE})
  target_link_libraries(sse_crc_ring_test PRIVATE sse_journal_core)
  add_test(NAME sse_crc_ring_test COMMAND sse_crc_ring_test)
  set_tests_properties(sse_crc_ring_test PROPERTIES TIMEOUT 120)
  add_executable(sse_journal_handoff_test ${USAGI_ROOT}/tests/sse/sse_journal_handoff_test.cpp)
  target_link_libraries(sse_journal_handoff_test PRIVATE sse_journal_core)
  target_compile_options(sse_journal_handoff_test PRIVATE -UNDEBUG)
  add_test(NAME sse_journal_handoff_test COMMAND sse_journal_handoff_test)
  set_tests_properties(sse_journal_handoff_test PROPERTIES TIMEOUT 30)
  add_executable(sse_journal_core_test ${USAGI_ROOT}/tests/sze/sze_recoverable_test.cpp)
  target_link_libraries(sse_journal_core_test PRIVATE sse_journal_core)
  target_compile_options(sse_journal_core_test PRIVATE -UNDEBUG)
  add_test(NAME sse_journal_core_test COMMAND sse_journal_core_test)
  foreach(sse_support_test sse_cpu_affinity_test sse_shard_plan_test)
    add_executable(${sse_support_test} ${USAGI_ROOT}/tests/sse/${sse_support_test}.cpp)
    target_link_libraries(${sse_support_test} PRIVATE sse_runtime_support)
    target_compile_options(${sse_support_test} PRIVATE -UNDEBUG)
    add_test(NAME ${sse_support_test} COMMAND ${sse_support_test})
  endforeach()
  find_program(T0_SSE_AFFINITY_PYTHON NAMES python3 python3.6 python2.7 python2)
  if(T0_SSE_AFFINITY_PYTHON)
    add_test(NAME sse_capture_retention_test
      COMMAND ${T0_SSE_AFFINITY_PYTHON} -B ${USAGI_ROOT}/tests/sse/test_capture_retention.py)
    add_test(NAME sse_capture_check_test
      COMMAND ${T0_SSE_AFFINITY_PYTHON} -B ${USAGI_ROOT}/tests/sse/test_capture_check.py)
    add_test(NAME sse_journal_trading_launcher_test
      COMMAND ${T0_SSE_AFFINITY_PYTHON} -B ${USAGI_ROOT}/tests/sse/test_journal_trading_launcher.py)
    set_tests_properties(sse_journal_trading_launcher_test PROPERTIES TIMEOUT 30)
    add_test(NAME sse_journal_launcher_test
      COMMAND ${T0_SSE_AFFINITY_PYTHON} -B ${USAGI_ROOT}/tests/sse/sse_journal_launcher_test.py
        --launcher ${USAGI_ROOT}/tools/sse/run_journal_capture.py
        --binary $<TARGET_FILE:t0_sse_journal_capture>)
    set_tests_properties(sse_journal_launcher_test PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)
    add_test(NAME sse_td_query_cli_test
      COMMAND ${T0_SSE_AFFINITY_PYTHON} -B ${USAGI_ROOT}/tests/sse/test_td_query_cli.py
        $<TARGET_FILE:t0_sse_journal_predict>)
    set_tests_properties(sse_capture_check_test sse_td_query_cli_test PROPERTIES TIMEOUT 30)
    add_test(NAME sse_journal_integration_test
      COMMAND ${T0_SSE_AFFINITY_PYTHON} -B ${USAGI_ROOT}/tests/sse/sse_journal_integration_test.py
        $<TARGET_FILE:t0_sse_journal_capture> $<TARGET_FILE:t0_sse_journal_predict>)
    set_tests_properties(sse_journal_integration_test PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)
    add_test(NAME sse_runtime_cpu_affinity_test
      COMMAND ${T0_SSE_AFFINITY_PYTHON} -B ${USAGI_ROOT}/tests/sse/sse_runtime_cpu_affinity_test.py
        $<TARGET_FILE:t0_sse_runtime>)
    set_tests_properties(sse_runtime_cpu_affinity_test PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE)
  endif()
  add_executable(sse_config_guard_test
    ${USAGI_ROOT}/tests/sse/sse_config_guard_test.cpp
    ${USAGI_ROOT}/adapters/deepwin/sse/sse_config_guard.cpp)
  target_include_directories(sse_config_guard_test PRIVATE ${USAGI_ROOT})
  add_test(NAME sse_config_guard_test COMMAND sse_config_guard_test
    ${USAGI_ROOT}/config/examples/sse/config_sse_hybrid_prediction_20260818.json)
  
  add_executable(sse_live_sampling_test
    ${USAGI_ROOT}/tests/sse/sse_batch_end_sampler_test.cpp)
  target_link_libraries(sse_live_sampling_test PRIVATE sse_live_sampling)
  target_compile_options(sse_live_sampling_test PRIVATE -UNDEBUG)
  add_test(NAME sse_live_sampling_test COMMAND sse_live_sampling_test)
  
  add_executable(market_data_stream_test
    ${USAGI_ROOT}/tests/common/market_data_stream_test.cpp)
  target_link_libraries(market_data_stream_test PRIVATE
    deepwin_market_data_runtime sse_live_sampling)
  target_compile_options(market_data_stream_test PRIVATE -UNDEBUG)
  add_test(NAME market_data_stream_test COMMAND market_data_stream_test)
  set_tests_properties(market_data_stream_test PROPERTIES TIMEOUT 15)
  find_program(T0_STREAM_PYTHON NAMES python3 python3.6)
  if(T0_STREAM_PYTHON)
    add_test(NAME market_data_stream_cli_test
      COMMAND ${T0_STREAM_PYTHON} -B ${USAGI_ROOT}/tests/integration/test_md_stream_cli.py
        $<TARGET_FILE:t0_md_stream> $<TARGET_FILE:sse_udp_observer>)
    set_tests_properties(market_data_stream_cli_test PROPERTIES TIMEOUT 30)
  endif()
  
  add_executable(sse_model_runtime_test
    ${USAGI_ROOT}/tests/sse/sse_model_runtime_test.cpp)
  target_link_libraries(sse_model_runtime_test PRIVATE sse_model_runtime)
  add_test(NAME sse_model_runtime_test COMMAND sse_model_runtime_test)
  target_compile_options(sse_model_runtime_test PRIVATE -UNDEBUG)
  
  add_executable(sse_snapshot_gru_test
    ${USAGI_ROOT}/tests/sse/snapshot_gru_test.cpp)
  target_link_libraries(sse_snapshot_gru_test PRIVATE sse_snapshot_gru_runtime)
  target_compile_options(sse_snapshot_gru_test PRIVATE -UNDEBUG)
  add_test(NAME sse_snapshot_gru_test COMMAND sse_snapshot_gru_test)
  
  add_executable(sse_hybrid_model_test
    ${USAGI_ROOT}/tests/sse/sse_hybrid_model_test.cpp)
  target_link_libraries(sse_hybrid_model_test PRIVATE sse_hybrid_model_runtime)
  target_compile_options(sse_hybrid_model_test PRIVATE -UNDEBUG)
  add_test(NAME sse_hybrid_model_test COMMAND sse_hybrid_model_test)
  
  add_executable(sse_udp_observer_test
    ${USAGI_ROOT}/tests/sse/sse_udp_observer_test.cpp)
  add_test(NAME sse_udp_observer_test COMMAND sse_udp_observer_test)
  if(T0_SSE_AFFINITY_PYTHON)
    add_test(NAME sse_udp_observer_offline_test
      COMMAND ${T0_SSE_AFFINITY_PYTHON}
      ${USAGI_ROOT}/tests/sse/sse_udp_observer_offline_test.py
      $<TARGET_FILE:sse_udp_observer>)
    set_tests_properties(sse_udp_observer_offline_test PROPERTIES TIMEOUT 10 RUN_SERIAL TRUE)
  endif()
endif()

if(T0_STREAM_PYTHON AND T0_BUILD_SZE_STREAM_PROCESSOR AND T0_BUILD_SSE_STREAM_PROCESSOR)
  add_library(runtime_thread_failure_shim SHARED ${USAGI_ROOT}/tests/integration/runtime_thread_failure_shim.cpp)
  target_link_libraries(runtime_thread_failure_shim PRIVATE dl)
  add_test(NAME market_runtime_api_test
    COMMAND ${T0_STREAM_PYTHON} -B ${USAGI_ROOT}/tests/integration/test_market_runtime_api.py
      $<TARGET_FILE:t0_sse_runtime> $<TARGET_FILE:t0_sze_runtime>
      $<TARGET_FILE:t0_sse_stream> $<TARGET_FILE:t0_sze_stream>
      $<TARGET_FILE:runtime_thread_failure_shim>)
  set_tests_properties(market_runtime_api_test PROPERTIES TIMEOUT 90)
  add_test(NAME market_processing_cli_test
    COMMAND ${T0_STREAM_PYTHON} -B ${USAGI_ROOT}/tests/integration/test_market_processing_cli.py
      $<TARGET_FILE:t0_sze_stream> $<TARGET_FILE:t0_sse_stream>)
  set_tests_properties(market_processing_cli_test PROPERTIES TIMEOUT 30)
endif()

if(T0_STREAM_PYTHON)
  add_test(NAME source_layout_test COMMAND ${T0_STREAM_PYTHON} -B ${USAGI_ROOT}/tests/common/test_source_layout.py)
  add_test(NAME deployment_paths_test COMMAND ${T0_STREAM_PYTHON} -B ${USAGI_ROOT}/tests/common/test_deployment_paths.py)
  add_test(NAME unified_config_test COMMAND ${T0_STREAM_PYTHON} -B -m unittest discover -s ${USAGI_ROOT}/tests/common -p "test_unified_config*.py")
  add_test(NAME processing_config_test COMMAND ${T0_STREAM_PYTHON} -B ${USAGI_ROOT}/tests/common/test_prepare_stream_processing.py)
  add_test(NAME sze_daily_config_test COMMAND ${T0_STREAM_PYTHON} -B ${USAGI_ROOT}/tests/sze/test_prepare_sze_runtime.py)
endif()

# Keep ATP cancel objects separate from their original OMS order.
add_executable(atp_order_report_mapping_test ${USAGI_ROOT}/tests/oms/atp_order_report_mapping_test.cpp)
target_compile_options(atp_order_report_mapping_test PRIVATE -UNDEBUG)
add_test(NAME atp_order_report_mapping_test COMMAND atp_order_report_mapping_test)
add_executable(atp_cancel_lifecycle_test ${USAGI_ROOT}/tests/oms/atp_cancel_lifecycle_test.cpp)
target_link_libraries(atp_cancel_lifecycle_test PRIVATE t0_oms_core)
target_compile_options(atp_cancel_lifecycle_test PRIVATE -UNDEBUG)
add_test(NAME atp_cancel_lifecycle_test COMMAND atp_cancel_lifecycle_test)

if(TARGET sse_stream_processing)
  add_executable(sse_owner_dispatch_test ${USAGI_ROOT}/tests/sse/sse_owner_dispatch_test.cpp)
  target_link_libraries(sse_owner_dispatch_test PRIVATE sse_stream_processing t0_strategy_core)
  target_compile_options(sse_owner_dispatch_test PRIVATE -UNDEBUG)
  add_test(NAME sse_owner_dispatch_test COMMAND sse_owner_dispatch_test)
  set_tests_properties(sse_owner_dispatch_test PROPERTIES TIMEOUT 60)
endif()

add_executable(atp_snapshot_order_test ${USAGI_ROOT}/tests/oms/atp_snapshot_order_test.cpp)
target_link_libraries(atp_snapshot_order_test PRIVATE t0_oms_core)
target_compile_options(atp_snapshot_order_test PRIVATE -UNDEBUG)
add_test(NAME atp_snapshot_order_test COMMAND atp_snapshot_order_test)

add_executable(async_diagnostic_log_test ${USAGI_ROOT}/tests/common/async_diagnostic_log_test.cpp)
target_include_directories(async_diagnostic_log_test PRIVATE ${USAGI_ROOT})
target_link_libraries(async_diagnostic_log_test PRIVATE pthread)
target_compile_options(async_diagnostic_log_test PRIVATE -UNDEBUG)
add_test(NAME async_diagnostic_log_test COMMAND async_diagnostic_log_test)
set_tests_properties(async_diagnostic_log_test PROPERTIES TIMEOUT 15)
find_program(T0_OPS_PYTHON NAMES python3 python2 python)
if(T0_OPS_PYTHON)
  add_test(NAME sse_receive_cpu_policy_test COMMAND ${T0_OPS_PYTHON} -B ${USAGI_ROOT}/tests/sse/test_receive_cpu_policy.py)
endif()
if(TARGET v06_model_golden_test)
  set(T0_V06_GOLDEN_DIR "" CACHE PATH "Directory containing v06.bin and the golden model fixtures")
  if(T0_V06_GOLDEN_DIR)
    add_test(NAME v06_model_golden_test COMMAND v06_model_golden_test ${T0_V06_GOLDEN_DIR})
  else()
    message(STATUS "v06 model golden test requires explicit T0_V06_GOLDEN_DIR")
  endif()
endif()

if(TARGET sse_journal_core)
  add_executable(sse_capture_history_test ${USAGI_ROOT}/tests/sse/sse_capture_history_test.cpp)
  target_link_libraries(sse_capture_history_test PRIVATE sse_journal_core deepwin_market_data_runtime)
  target_compile_options(sse_capture_history_test PRIVATE -UNDEBUG)
  add_test(NAME sse_capture_history_test COMMAND sse_capture_history_test)
endif()

if(TARGET sse_journal_core)
  add_executable(crc_compatibility_test ${USAGI_ROOT}/tests/sse/crc_compatibility_test.cpp)
  target_link_libraries(crc_compatibility_test PRIVATE sse_journal_core)
  target_compile_options(crc_compatibility_test PRIVATE -UNDEBUG)
  add_test(NAME crc_compatibility_test COMMAND crc_compatibility_test)
endif()

# Reproducible paced SHM replay with optional timing hooks in the same sources.
if(TARGET sse_stream_processing)
  add_executable(sse_latency_replay EXCLUDE_FROM_ALL
    ${USAGI_ROOT}/tests/sse/latency_replay.cpp
    ${USAGI_ROOT}/tests/sse/latency_wrappers.cpp
    ${USAGI_ROOT}/sse/runtime/sse_stream_processor.cpp
    ${USAGI_ROOT}/sse/runtime/sse_strategy_session.cpp)
  target_include_directories(sse_latency_replay PRIVATE
    ${USAGI_ROOT}/common/strategy ${USAGI_ROOT}/common/model/legacy)
  target_compile_definitions(sse_latency_replay PRIVATE SSE_REPLAY_PROBE=1)
  target_compile_options(sse_latency_replay PRIVATE -ffp-contract=off)
  target_link_libraries(sse_latency_replay PRIVATE
    sse_stream_processing sse_journal_core sse_runtime_support
    deepwin_market_data_runtime sse_strategy_session t0_strategy_core)
  set_property(TARGET sse_latency_replay APPEND_STRING PROPERTY LINK_FLAGS
    " -Wl,--wrap=_ZN8sse_tick11FactorState5buildERNS_9OrderBookEmRA10_KNS_5LevelES6_ddd -Wl,--wrap=_ZNK7sse_v065Model7predictERKSt5arrayIfLm50EEPNS_5StateEPS1_IfLm4EEPNS_6StagesE")
endif()
