if(T0_BUILD_SSE_STREAM_PROCESSOR)
  add_library(sse_live_sampling STATIC
    ${USAGI_ROOT}/sse/sampling/sse_batch_end_sampler.cpp)
  target_include_directories(sse_live_sampling PUBLIC
    ${USAGI_ROOT}/sse/market_data)
  
  add_library(sse_snapshot_gru_runtime STATIC ${USAGI_ROOT}/sse/model/snapshot_ensemble.cpp)
  target_include_directories(sse_snapshot_gru_runtime PUBLIC ${USAGI_ROOT})
  target_compile_options(sse_snapshot_gru_runtime PRIVATE -ffp-contract=off)
  target_link_libraries(sse_snapshot_gru_runtime PUBLIC snapshot_gru_engine)
  
  add_library(sse_hybrid_model_runtime STATIC
    ${USAGI_ROOT}/sse/model/sse_hybrid_model.cpp)
  target_include_directories(sse_hybrid_model_runtime PUBLIC
    ${USAGI_ROOT}/sse/model
    ${USAGI_ROOT} ${USAGI_ROOT}/common/model/legacy)
  target_link_libraries(sse_hybrid_model_runtime PUBLIC
    sse_model_runtime sse_snapshot_gru_runtime)
  target_compile_options(sse_hybrid_model_runtime PRIVATE -ffp-contract=off)
endif()

if(T0_BUILD_SSE_STREAM_PROCESSOR)
  add_library(sse_stream_processing STATIC
    ${USAGI_ROOT}/sse/runtime/sse_stream_processor.cpp
    ${USAGI_ROOT}/sse/market_data/sse_primary_decoder.cpp
    ${USAGI_ROOT}/sse/market_data/sse_tick_order_book.cpp
    ${USAGI_ROOT}/sse/factors/sse_tick_factors.cpp
    ${USAGI_ROOT}/sse/market_data/sse_tick_static_metadata.cpp
    ${USAGI_ROOT}/sse/factors/snapshot36.cpp
    ${USAGI_ROOT}/sse/factors/auction59_sidecar.cpp)
  target_include_directories(sse_stream_processing PUBLIC ${USAGI_ROOT}/sse)
  target_compile_options(sse_stream_processing PRIVATE -ffp-contract=off)
  target_link_libraries(sse_stream_processing PUBLIC
    deepwin_market_data_runtime sse_live_sampling sse_hybrid_model_runtime)
  add_executable(sse_stream_processor_test ${USAGI_ROOT}/tests/sse/sse_stream_processor_test.cpp)
  target_link_libraries(sse_stream_processor_test PRIVATE sse_stream_processing)
  target_compile_options(sse_stream_processor_test PRIVATE -UNDEBUG)
  add_test(NAME sse_stream_processor_test COMMAND sse_stream_processor_test)
  set_tests_properties(sse_stream_processor_test PROPERTIES TIMEOUT 20)
  add_library(sse_strategy_session STATIC ${USAGI_ROOT}/sse/runtime/sse_strategy_session.cpp)
  target_link_libraries(sse_strategy_session PUBLIC sse_stream_processing t0_strategy_core)
  add_executable(sse_strategy_session_test ${USAGI_ROOT}/tests/sse/sse_strategy_session_test.cpp)
  target_link_libraries(sse_strategy_session_test PRIVATE sse_strategy_session)
  target_compile_options(sse_strategy_session_test PRIVATE -UNDEBUG)
  add_test(NAME sse_strategy_session_test COMMAND sse_strategy_session_test)
  add_executable(sse_strategy_fixture ${USAGI_ROOT}/tests/sse/sse_strategy_fixture.cpp)
  if(T0_STREAM_PYTHON)
    add_test(NAME strategy_stream_cli_test
      COMMAND ${T0_STREAM_PYTHON} -B ${USAGI_ROOT}/tests/integration/test_strategy_stream_cli.py
        $<TARGET_FILE:t0_sse_stream> $<TARGET_FILE:sse_strategy_fixture>)
    set_tests_properties(strategy_stream_cli_test PROPERTIES TIMEOUT 30)
  endif()
  add_library(t0_sse_runtime SHARED ${USAGI_ROOT}/apps/MarketRuntime.cpp)
  target_compile_definitions(t0_sse_runtime PRIVATE T0_STREAM_SSE)
  target_include_directories(t0_sse_runtime PRIVATE ${BOOST_INCLUDE})
  target_link_libraries(t0_sse_runtime PRIVATE sse_strategy_session)
  add_executable(t0_sse_stream ${USAGI_ROOT}/apps/t0_market_runtime_main.cpp)
  target_link_libraries(t0_sse_stream PRIVATE t0_sse_runtime pthread)
endif()
