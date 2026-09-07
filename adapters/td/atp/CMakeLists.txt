CMAKE_MINIMUM_REQUIRED(VERSION 3.7)
PROJECT(sze_td)
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
STRING(TIMESTAMP GXBSE_TD_BUILD_VERSION "%Y%m%d%H%M%S")

set(MY_INSTALL_PATH "/opt/deepwin/master/lib64/api")
set(DEEPWIN_LIB journal kflog wingchuntd)
set(DEEPWIN_INCLUDE_DIR "/opt/deepwin/master/include")
set(DEEPWIN_WC_LIB_DIR "/opt/deepwin/master/lib/wingchun")
set(DEEPWIN_YJJ_LIB_DIR "/opt/deepwin/master/lib/yijinjing")
set(DEEPWIN_BOOST_INCLUDE_DIR "/opt/deepwin/toolchain/boost-1.62.0/include")
set(DEEPWIN_BOOST_LIB_DIR "/opt/deepwin/toolchain/boost-1.62.0/lib")
set(DEEPWIN_PYTHON_INCLUDE_DIR "/usr/include/python3.6m")
get_filename_component(USAGI_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
set(GUOXIN_API_DIR "${CMAKE_CURRENT_SOURCE_DIR}/api" CACHE PATH "ATP SDK root containing include/ and lib/")

include_directories(${USAGI_ROOT} ${DEEPWIN_INCLUDE_DIR} ${DEEPWIN_BOOST_INCLUDE_DIR} ${DEEPWIN_PYTHON_INCLUDE_DIR} ${GUOXIN_API_DIR}/include)
link_directories(${DEEPWIN_WC_LIB_DIR} ${DEEPWIN_YJJ_LIB_DIR} ${DEEPWIN_BOOST_LIB_DIR} ${GUOXIN_API_DIR}/lib)

SET(GUOXIN_TD_SOURCE_FILES TDEngineGXBSE.cpp TDEngineGXBSE.h ${USAGI_ROOT}/common/oms/Types.cpp)

function(add_guoxin_td_engine target output_name source_id engine_key default_market_id)
    ADD_LIBRARY(${target} SHARED ${GUOXIN_TD_SOURCE_FILES})
    set_target_properties(${target} PROPERTIES OUTPUT_NAME "${output_name}")
    target_compile_definitions(${target} PRIVATE
        GXBSE_TD_BUILD_VERSION="${GXBSE_TD_BUILD_VERSION}"
        T0_TD_SOURCE_ID=${source_id}
        T0_TD_ENGINE_KEY="${engine_key}"
        T0_TD_DEFAULT_MARKET_ID=${default_market_id})
    TARGET_LINK_LIBRARIES(${target} ${DEEPWIN_LIB} ${Boost_LIBRARIES} atpquantapi pthread rt)
    set_target_properties(${target} PROPERTIES
        BUILD_RPATH "${DEEPWIN_WC_LIB_DIR};${DEEPWIN_YJJ_LIB_DIR};${DEEPWIN_BOOST_LIB_DIR};${GUOXIN_API_DIR}/lib"
        INSTALL_RPATH "${DEEPWIN_WC_LIB_DIR};${DEEPWIN_YJJ_LIB_DIR};${DEEPWIN_BOOST_LIB_DIR};${GUOXIN_API_DIR}/lib")
    INSTALL(TARGETS ${target} DESTINATION "lib/wingchun")
endfunction()

add_guoxin_td_engine(sze_td sze_td 180 sze_td 102)
add_guoxin_td_engine(sse_td sse_td 190 sse_td 101)
target_compile_definitions(sse_td PRIVATE SSE_TD_BUILD=1)
