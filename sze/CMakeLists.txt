if(T0_BUILD_SZE_STREAM_PROCESSOR)
  add_library(sze_stream_processing STATIC
    ${USAGI_ROOT}/sze/runtime/sze_stream_processor.cpp
    ${USAGI_ROOT}/sze/runtime/sze_recovery_driver.cpp
    ${USAGI_ROOT}/sze/sampling/mix153060_runtime.cpp
    ${USAGI_ROOT}/sze/runtime/mix153060_live_adapter.cpp
    ${USAGI_ROOT}/sze/market_data/SZEProtocol.cpp ${USAGI_ROOT}/common/recovery/SZERecoverable.cpp)
  target_include_directories(sze_stream_processing PUBLIC ${USAGI_ROOT} ${USAGI_ROOT}/sze/market_data ${DEEPWIN_INCLUDE})
  target_compile_options(sze_stream_processing PRIVATE -ffp-contract=off)
  target_link_libraries(sze_stream_processing PUBLIC deepwin_market_data_runtime common_mix153060_model)
  add_executable(sze_stream_processor_test ${USAGI_ROOT}/tests/sze/sze_stream_processor_test.cpp)
  target_link_libraries(sze_stream_processor_test PRIVATE sze_stream_processing)
  target_compile_options(sze_stream_processor_test PRIVATE -UNDEBUG)
  add_test(NAME sze_stream_processor_test COMMAND sze_stream_processor_test)
  set_tests_properties(sze_stream_processor_test PROPERTIES TIMEOUT 20)
  add_executable(sze_recovery_processor_test ${USAGI_ROOT}/tests/sze/sze_recovery_processor_test.cpp)
  target_link_libraries(sze_recovery_processor_test PRIVATE sze_stream_processing)
  target_compile_options(sze_recovery_processor_test PRIVATE -UNDEBUG)
  add_test(NAME sze_recovery_processor_test COMMAND sze_recovery_processor_test)
  add_library(sze_strategy_session STATIC ${USAGI_ROOT}/sze/runtime/SzeStrategySession.cpp)
  target_link_libraries(sze_strategy_session PUBLIC sze_stream_processing t0_strategy_core)
  add_executable(sze_strategy_session_test ${USAGI_ROOT}/tests/sze/sze_strategy_session_test.cpp)
  target_link_libraries(sze_strategy_session_test PRIVATE sze_strategy_session)
  target_compile_options(sze_strategy_session_test PRIVATE -UNDEBUG)
  add_test(NAME sze_strategy_session_test COMMAND sze_strategy_session_test)
  add_executable(sze_recovery_runtime_fixture ${USAGI_ROOT}/tests/sze/sze_recovery_runtime_fixture.cpp)
  target_link_libraries(sze_recovery_runtime_fixture PRIVATE sze_stream_processing)
  add_executable(sze_strategy_fixture ${USAGI_ROOT}/tests/sze/sze_strategy_fixture.cpp)
  if(T0_STREAM_PYTHON)
    add_test(NAME recovery_runtime_cli_test
      COMMAND ${T0_STREAM_PYTHON} -B ${USAGI_ROOT}/tests/integration/test_recovery_runtime_cli.py
        $<TARGET_FILE:t0_sze_stream> $<TARGET_FILE:t0_sze_runtime>
        $<TARGET_FILE:sze_recovery_runtime_fixture>)
    set_tests_properties(recovery_runtime_cli_test PROPERTIES TIMEOUT 45)
    add_test(NAME sze_strategy_stream_cli_test
      COMMAND ${T0_STREAM_PYTHON} -B ${USAGI_ROOT}/tests/integration/test_sze_strategy_stream_cli.py
        $<TARGET_FILE:t0_sze_stream> $<TARGET_FILE:sze_strategy_fixture>)
    set_tests_properties(sze_strategy_stream_cli_test PROPERTIES TIMEOUT 30)
  endif()
  add_library(t0_sze_runtime SHARED ${USAGI_ROOT}/apps/MarketRuntime.cpp)
  target_compile_definitions(t0_sze_runtime PRIVATE T0_STREAM_SZE)
  target_include_directories(t0_sze_runtime PRIVATE ${BOOST_INCLUDE})
  target_link_libraries(t0_sze_runtime PRIVATE sze_strategy_session)
  add_executable(t0_sze_stream ${USAGI_ROOT}/apps/t0_market_runtime_main.cpp)
  target_link_libraries(t0_sze_stream PRIVATE t0_sze_runtime pthread)
endif()
