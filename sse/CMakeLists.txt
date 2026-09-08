add_library(sse_runtime_support STATIC
  ${USAGI_ROOT}/sse/runtime/sse_cpu_affinity.cpp
  ${USAGI_ROOT}/sse/runtime/sse_shard_plan.cpp)
target_include_directories(sse_runtime_support PUBLIC ${USAGI_ROOT})
target_link_libraries(sse_runtime_support PUBLIC pthread)

add_executable(sse_hwstamp_ctl ${USAGI_ROOT}/tools/sse/sse_hwstamp_ctl.cpp)
target_link_libraries(sse_hwstamp_ctl PRIVATE deepwin_market_data_runtime)
add_executable(sse_timestamp_probe ${USAGI_ROOT}/tools/sse/sse_timestamp_probe.cpp)
target_link_libraries(sse_timestamp_probe PRIVATE deepwin_market_data_runtime sse_runtime_support)

# Reuse the mature journal/SHM protocol without changing Shenzhen's build or
# source files. The copied core retains its ABI and source-89 payloads identify
# Shanghai transport metadata explicitly.
add_library(sse_journal_core STATIC ${USAGI_ROOT}/common/recovery/SZERecoverable.cpp)
target_include_directories(sse_journal_core PUBLIC ${USAGI_ROOT} ${USAGI_ROOT}/common/recovery)
target_link_libraries(sse_journal_core PUBLIC pthread rt)
add_executable(t0_sse_journal_capture ${USAGI_ROOT}/sse/runtime/sse_journal_capture.cpp)
target_link_libraries(t0_sse_journal_capture PRIVATE
  sse_journal_core sse_runtime_support deepwin_market_data_runtime)
target_include_directories(t0_sse_journal_capture PRIVATE ${BOOST_INCLUDE})

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
  add_executable(sse_fast_decode_test ${USAGI_ROOT}/tests/sse/sse_fast_decode_test.cpp)
  target_link_libraries(sse_fast_decode_test PRIVATE sse_stream_processing)
  add_test(NAME sse_fast_decode_test COMMAND sse_fast_decode_test)
  add_executable(sse_book_incremental_test ${USAGI_ROOT}/tests/sse/sse_book_incremental_test.cpp)
  target_link_libraries(sse_book_incremental_test PRIVATE sse_stream_processing)
  add_test(NAME sse_book_incremental_test COMMAND sse_book_incremental_test)
  add_executable(sse_model_runtime_benchmark ${USAGI_ROOT}/tools/model/sse_model_runtime_benchmark.cpp)
  target_link_libraries(sse_model_runtime_benchmark PRIVATE sse_model_runtime pthread)
  add_executable(sse_model_sequence_compare ${USAGI_ROOT}/tools/model/sse_model_sequence_compare.cpp)
  target_link_libraries(sse_model_sequence_compare PRIVATE sse_model_runtime)
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
  target_link_libraries(t0_sse_runtime PRIVATE sse_strategy_session sse_runtime_support)
  add_executable(t0_sse_stream ${USAGI_ROOT}/apps/t0_market_runtime_main.cpp)
  target_link_libraries(t0_sse_stream PRIVATE t0_sse_runtime pthread)
  add_executable(t0_sse_journal_predict ${USAGI_ROOT}/sse/runtime/sse_journal_prediction.cpp)
  target_link_libraries(t0_sse_journal_predict PRIVATE
    sse_journal_core sse_runtime_support sse_strategy_session)
  target_include_directories(t0_sse_journal_predict PRIVATE ${BOOST_INCLUDE})
endif()

if(T0_BUILD_SSE_STREAM_PROCESSOR)
  # Auction59 opening-auction package: opening-boundary quality, per-stock
  # static metadata (CSV) and the auction reconstruction engine, with its
  # standalone replay/follow runner. Not yet wired into the SSE stream
  # processor; the processing profile reserves an optional "auction59" toggle
  # under "prediction" for that integration (see StreamProcessingCli). The
  # library stays self-contained (no decoder.cpp dependency); the runner
  # compiles the decoder directly.
  add_library(sse_auction59 STATIC
    ${USAGI_ROOT}/sse/auction/sse_opening_boundary.cpp
    ${USAGI_ROOT}/sse/auction/auction_static_metadata.cpp
    ${USAGI_ROOT}/sse/auction/auction59_engine.cpp)
  target_link_libraries(sse_auction59 PUBLIC pthread)
  add_executable(sse_auction59_live
    ${USAGI_ROOT}/sse/auction/sse_auction59_live.cpp
    ${USAGI_ROOT}/sse/market_data/sse_primary_decoder.cpp)
  target_include_directories(sse_auction59_live PRIVATE ${USAGI_ROOT}/sse/market_data)
  target_link_libraries(sse_auction59_live PRIVATE sse_auction59 pthread rt)
  # Production pre-open gate: reconcile per-security snapshot pre_close against
  # the daily auction static CSV; nonzero on conflict/invalid (Auction59 stays
  # disabled until it passes). Mirrors live deploy name audit_snapshot_static_metadata.
  add_executable(audit_snapshot_static_metadata
    ${USAGI_ROOT}/sse/auction/audit_snapshot_static_metadata.cpp
    ${USAGI_ROOT}/sse/market_data/sse_primary_decoder.cpp)
  target_include_directories(audit_snapshot_static_metadata PRIVATE ${USAGI_ROOT}/sse/market_data)
  target_link_libraries(audit_snapshot_static_metadata PRIVATE sse_auction59 pthread)
  foreach(auction_test auction59_engine_test auction_static_metadata_test sse_opening_boundary_test)
    add_executable(${auction_test} ${USAGI_ROOT}/tests/sse/${auction_test}.cpp)
    target_link_libraries(${auction_test} PRIVATE sse_auction59)
    target_compile_options(${auction_test} PRIVATE -UNDEBUG)
    add_test(NAME ${auction_test} COMMAND ${auction_test})
    set_tests_properties(${auction_test} PROPERTIES TIMEOUT 60)
  endforeach()
endif()
