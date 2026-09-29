if(NOT TARGET sse_runtime_support)
  message(FATAL_ERROR
    "USAGI_BUILD_SSE_EXPERIMENTS requires the main sse_runtime_support target")
endif()

set(SSE_EXPERIMENT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")

# This helper deliberately remains private to the historical .szej journal
# format.  The main repository's .t0md journal is a different wire contract.
add_library(sse_experimental_legacy_journal STATIC
  ${SSE_EXPERIMENT_ROOT}/legacy_journal/SZERecoverable.cpp)
target_include_directories(sse_experimental_legacy_journal PUBLIC
  ${SSE_EXPERIMENT_ROOT}/legacy_journal)
target_link_libraries(sse_experimental_legacy_journal PUBLIC pthread rt)

# Keep the old UDP implementation ABI-isolated from common/stream.
add_library(sse_experimental_udp_runtime STATIC
  ${SSE_EXPERIMENT_ROOT}/udp_runtime/UdpChannelRuntime.cpp)
target_include_directories(sse_experimental_udp_runtime PUBLIC
  ${SSE_EXPERIMENT_ROOT}/udp_runtime)
target_link_libraries(sse_experimental_udp_runtime PUBLIC pthread)

add_library(sse_experimental_event_runtime STATIC
  ${SSE_EXPERIMENT_ROOT}/market_data/sse_event_codec.cpp
  ${SSE_EXPERIMENT_ROOT}/market_data/sse_event_pipeline.cpp
  ${SSE_EXPERIMENT_ROOT}/market_data/sse_event_journal.cpp
  ${SSE_EXPERIMENT_ROOT}/market_data/sse_primary_decoder.cpp)
target_include_directories(sse_experimental_event_runtime PUBLIC
  ${USAGI_ROOT} ${SSE_EXPERIMENT_ROOT}
  ${SSE_EXPERIMENT_ROOT}/market_data)
target_link_libraries(sse_experimental_event_runtime PUBLIC
  sse_runtime_support sse_experimental_legacy_journal pthread)

add_library(sse_experimental_live_sampling STATIC
  ${SSE_EXPERIMENT_ROOT}/market_data/sse_batch_end_sampler.cpp)
target_include_directories(sse_experimental_live_sampling PUBLIC
  ${SSE_EXPERIMENT_ROOT}/market_data)
target_link_libraries(sse_experimental_live_sampling PUBLIC rt)

add_library(sse_experimental_model_runtime STATIC
  ${SSE_EXPERIMENT_ROOT}/model/sse_model_runtime.cpp)
target_include_directories(sse_experimental_model_runtime PUBLIC
  ${USAGI_ROOT} ${USAGI_ROOT}/third_party/eigen3
  ${SSE_EXPERIMENT_ROOT}/model)
target_compile_options(sse_experimental_model_runtime PRIVATE -ffp-contract=off)

add_library(sse_experimental_snapshot_gru_runtime STATIC
  ${SSE_EXPERIMENT_ROOT}/model/snapshot_gru_runtime.cpp
  ${SSE_EXPERIMENT_ROOT}/model/snapshot_ensemble.cpp)
target_include_directories(sse_experimental_snapshot_gru_runtime PUBLIC
  ${USAGI_ROOT} ${SSE_EXPERIMENT_ROOT}/model)
target_compile_options(sse_experimental_snapshot_gru_runtime PRIVATE -ffp-contract=off)

add_library(sse_experimental_hybrid_model_runtime STATIC
  ${SSE_EXPERIMENT_ROOT}/model/sse_hybrid_model.cpp)
target_include_directories(sse_experimental_hybrid_model_runtime PUBLIC
  ${USAGI_ROOT} ${SSE_EXPERIMENT_ROOT}/model)
target_compile_options(sse_experimental_hybrid_model_runtime PRIVATE -ffp-contract=off)
target_link_libraries(sse_experimental_hybrid_model_runtime PUBLIC
  sse_experimental_model_runtime sse_experimental_snapshot_gru_runtime)

add_library(sse_experimental_event_model_consumer STATIC
  ${SSE_EXPERIMENT_ROOT}/strategy/sse_model_consumer.cpp
  ${SSE_EXPERIMENT_ROOT}/strategy/sse_tick_prediction_engine.cpp
  ${SSE_EXPERIMENT_ROOT}/strategy/sse_tick_sample_gate.cpp
  ${SSE_EXPERIMENT_ROOT}/market_data/sse_tick_order_book.cpp
  ${SSE_EXPERIMENT_ROOT}/market_data/sse_tick_factors.cpp
  ${SSE_EXPERIMENT_ROOT}/market_data/sse_tick_static_metadata.cpp
  ${SSE_EXPERIMENT_ROOT}/model/snapshot36.cpp
  ${SSE_EXPERIMENT_ROOT}/model/auction59_sidecar.cpp)
target_include_directories(sse_experimental_event_model_consumer PUBLIC
  ${USAGI_ROOT} ${SSE_EXPERIMENT_ROOT})
target_compile_options(sse_experimental_event_model_consumer PRIVATE -ffp-contract=off)
target_link_libraries(sse_experimental_event_model_consumer PUBLIC
  sse_experimental_event_runtime
  sse_experimental_model_runtime
  sse_experimental_snapshot_gru_runtime)

add_executable(sse_experimental_event_pipeline
  ${SSE_EXPERIMENT_ROOT}/market_observer/sse_event_pipeline_main.cpp
  ${SSE_EXPERIMENT_ROOT}/market_data/sse_history_input.cpp)
target_include_directories(sse_experimental_event_pipeline PRIVATE
  ${USAGI_ROOT} ${SSE_EXPERIMENT_ROOT})
target_link_libraries(sse_experimental_event_pipeline PRIVATE
  sse_experimental_event_runtime
  sse_experimental_event_model_consumer
  sse_experimental_udp_runtime)

add_executable(sse_experimental_udp_observer
  ${SSE_EXPERIMENT_ROOT}/market_observer/sse_udp_observer.cpp)
target_include_directories(sse_experimental_udp_observer PRIVATE
  ${USAGI_ROOT} ${SSE_EXPERIMENT_ROOT}/udp_runtime)
target_link_libraries(sse_experimental_udp_observer PRIVATE
  sse_experimental_udp_runtime sse_runtime_support)

function(sse_experimental_test target source)
  add_executable(${target} ${source})
  target_include_directories(${target} PRIVATE ${USAGI_ROOT} ${SSE_EXPERIMENT_ROOT})
  target_compile_options(${target} PRIVATE -UNDEBUG)
  add_test(NAME ${target} COMMAND ${target})
endfunction()

sse_experimental_test(sse_experimental_cpu_affinity_test
  ${SSE_EXPERIMENT_ROOT}/tests/sse_cpu_affinity_test.cpp)
target_link_libraries(sse_experimental_cpu_affinity_test PRIVATE sse_runtime_support)

sse_experimental_test(sse_experimental_shard_plan_test
  ${SSE_EXPERIMENT_ROOT}/tests/sse_shard_plan_test.cpp)
target_link_libraries(sse_experimental_shard_plan_test PRIVATE sse_runtime_support)

sse_experimental_test(sse_experimental_event_queue_test
  ${SSE_EXPERIMENT_ROOT}/tests/sse_event_queue_test.cpp)
target_link_libraries(sse_experimental_event_queue_test PRIVATE pthread)

sse_experimental_test(sse_experimental_event_journal_test
  ${SSE_EXPERIMENT_ROOT}/tests/sse_event_journal_test.cpp)
target_link_libraries(sse_experimental_event_journal_test PRIVATE
  sse_experimental_event_runtime)

sse_experimental_test(sse_experimental_event_pipeline_test
  ${SSE_EXPERIMENT_ROOT}/tests/sse_event_pipeline_test.cpp)
target_link_libraries(sse_experimental_event_pipeline_test PRIVATE
  sse_experimental_event_runtime)

sse_experimental_test(sse_experimental_event_sampling_test
  ${SSE_EXPERIMENT_ROOT}/tests/sse_event_sampling_test.cpp)
target_link_libraries(sse_experimental_event_sampling_test PRIVATE
  sse_experimental_event_runtime)

sse_experimental_test(sse_experimental_latency_histogram_test
  ${SSE_EXPERIMENT_ROOT}/tests/sse_latency_histogram_test.cpp)

sse_experimental_test(sse_experimental_batch_end_sampler_test
  ${SSE_EXPERIMENT_ROOT}/market_data/sse_batch_end_sampler_test.cpp)
target_link_libraries(sse_experimental_batch_end_sampler_test PRIVATE
  sse_experimental_live_sampling)

sse_experimental_test(sse_experimental_model_runtime_test
  ${SSE_EXPERIMENT_ROOT}/model/sse_model_runtime_test.cpp)
target_link_libraries(sse_experimental_model_runtime_test PRIVATE
  sse_experimental_model_runtime)

add_executable(sse_experimental_model_sequence_probe
  ${SSE_EXPERIMENT_ROOT}/model/sse_model_sequence_probe.cpp)
target_include_directories(sse_experimental_model_sequence_probe PRIVATE
  ${USAGI_ROOT} ${SSE_EXPERIMENT_ROOT}/model)
target_link_libraries(sse_experimental_model_sequence_probe PRIVATE
  sse_experimental_model_runtime)

sse_experimental_test(sse_experimental_snapshot_gru_test
  ${SSE_EXPERIMENT_ROOT}/model/snapshot_gru_test.cpp)
target_link_libraries(sse_experimental_snapshot_gru_test PRIVATE
  sse_experimental_snapshot_gru_runtime)

sse_experimental_test(sse_experimental_hybrid_model_test
  ${SSE_EXPERIMENT_ROOT}/model/sse_hybrid_model_test.cpp)
target_link_libraries(sse_experimental_hybrid_model_test PRIVATE
  sse_experimental_hybrid_model_runtime)

add_executable(sse_experimental_model_consumer_test
  ${SSE_EXPERIMENT_ROOT}/tests/sse_model_consumer_test.cpp)
target_include_directories(sse_experimental_model_consumer_test PRIVATE
  ${USAGI_ROOT} ${SSE_EXPERIMENT_ROOT})
target_compile_options(sse_experimental_model_consumer_test PRIVATE -UNDEBUG)
target_link_libraries(sse_experimental_model_consumer_test PRIVATE
  sse_experimental_event_model_consumer)
set(SSE_EXPERIMENT_TEST_TICK_MODEL "" CACHE FILEPATH
  "Historical SSE tick model used by the experiment consumer test")
set(SSE_EXPERIMENT_TEST_STATIC_JSON "" CACHE FILEPATH
  "Historical SSE static metadata used by the experiment consumer test")
set(SSE_EXPERIMENT_TEST_TRADING_DAY "" CACHE STRING
  "Historical SSE consumer test trading day (YYYYMMDD)")
if(SSE_EXPERIMENT_TEST_TICK_MODEL AND
   SSE_EXPERIMENT_TEST_STATIC_JSON AND
   SSE_EXPERIMENT_TEST_TRADING_DAY)
  add_test(NAME sse_experimental_model_consumer_test
    COMMAND sse_experimental_model_consumer_test
      ${SSE_EXPERIMENT_TEST_TICK_MODEL}
      ${SSE_EXPERIMENT_TEST_STATIC_JSON}
      ${SSE_EXPERIMENT_TEST_TRADING_DAY})
  set_tests_properties(sse_experimental_model_consumer_test
    PROPERTIES TIMEOUT 120)
endif()

sse_experimental_test(sse_experimental_udp_observer_test
  ${SSE_EXPERIMENT_ROOT}/market_observer/sse_udp_observer_test.cpp)

find_program(SSE_EXPERIMENT_PYTHON
  NAMES python3 python3.6 python2.7 python2 python)
if(SSE_EXPERIMENT_PYTHON)
  add_test(NAME sse_experimental_event_pipeline_integration_test
    COMMAND ${SSE_EXPERIMENT_PYTHON} -B
      ${SSE_EXPERIMENT_ROOT}/tests/sse_event_pipeline_integration_test.py
      $<TARGET_FILE:sse_experimental_event_pipeline>)
  set_tests_properties(sse_experimental_event_pipeline_integration_test
    PROPERTIES TIMEOUT 90)

  add_test(NAME sse_experimental_cpu_affinity_integration_test
    COMMAND ${SSE_EXPERIMENT_PYTHON} -B
      ${SSE_EXPERIMENT_ROOT}/tests/sse_cpu_affinity_integration_test.py
      $<TARGET_FILE:sse_experimental_udp_observer>)
  set_tests_properties(sse_experimental_cpu_affinity_integration_test
    PROPERTIES TIMEOUT 30)

  add_test(NAME sse_experimental_udp_observer_offline_test
    COMMAND ${SSE_EXPERIMENT_PYTHON} -B
      ${SSE_EXPERIMENT_ROOT}/market_observer/sse_udp_observer_offline_test.py
      $<TARGET_FILE:sse_experimental_udp_observer>)
  set_tests_properties(sse_experimental_udp_observer_offline_test
    PROPERTIES TIMEOUT 30)

  add_test(NAME sse_experimental_model_skeleton_test
    COMMAND ${SSE_EXPERIMENT_PYTHON} -B
      ${SSE_EXPERIMENT_ROOT}/model/model_skeleton_test.py)
endif()
