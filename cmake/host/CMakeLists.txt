cmake_minimum_required(VERSION 3.16.3)

if(NOT DEFINED ESL_PROXY_ENABLE_L2_SWIMLANE)
    set(ESL_PROXY_ENABLE_L2_SWIMLANE 0)
endif()

if(NOT DEFINED ESL_LANE_CNT)
    set(ESL_LANE_CNT 1)
endif()

# Pure-C host runner: swimlane host code is C now, so no CXX language needed.
project(esl_onboard_runner LANGUAGES C)

if(NOT DEFINED ASCEND_HOME_PATH)
    if(DEFINED ENV{ASCEND_HOME_PATH})
        set(ASCEND_HOME_PATH "$ENV{ASCEND_HOME_PATH}")
    else()
        message(FATAL_ERROR "ASCEND_HOME_PATH must be set")
    endif()
endif()

get_filename_component(REPO "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
if(NOT DEFINED ONBOARD_INC)
    set(ONBOARD_INC "${REPO}/esl_proxy/include/platform/onboard")
endif()
if(NOT DEFINED ONBOARD_SRC)
    set(ONBOARD_SRC "${REPO}/esl_proxy/src/platform/onboard")
endif()
set(SWIMLANE_SRC "${REPO}/esl_proxy/src/swimlane")
set(ROOT "${REPO}")

set(ESL_CORE "${REPO}/esl_proxy")
set(ESL_SRC "${ESL_CORE}/src")

set(ESL_ONBOARD_RUNNER_SOURCES
    "${ESL_SRC}/main.c"
    "${ONBOARD_SRC}/host_onboard.c"
    "${ONBOARD_SRC}/tools.c"
)
if(ESL_PROXY_ENABLE_L2_SWIMLANE)
    list(APPEND ESL_ONBOARD_RUNNER_SOURCES
        "${SWIMLANE_SRC}/host_swimlane.c"
        "${ONBOARD_SRC}/onboard_log.c"
    )
endif()

add_executable(esl_onboard_runner ${ESL_ONBOARD_RUNNER_SOURCES})

target_compile_definitions(esl_onboard_runner PRIVATE
    ESL_PROXY_ONBOARD_HOST=1
    ESL_PROXY_ENABLE_L2_SWIMLANE=${ESL_PROXY_ENABLE_L2_SWIMLANE}
    ESL_LANE_CNT=${ESL_LANE_CNT}
)

target_compile_options(esl_onboard_runner PRIVATE
    -Wall -Wextra -O2 -g
    $<$<COMPILE_LANGUAGE:C>:-std=c11>
)

target_include_directories(esl_onboard_runner
    PRIVATE
        "${ONBOARD_INC}"
        "${ESL_CORE}/include/algorithm"
        "${ESL_CORE}/include/platform"
        "${ESL_CORE}/include/swimlane"
        "${ASCEND_HOME_PATH}/include"
        "${ASCEND_HOME_PATH}/pkg_inc"
        "${ASCEND_HOME_PATH}/pkg_inc/base"
        "${ASCEND_HOME_PATH}/pkg_inc/runtime"
        "${ASCEND_HOME_PATH}/pkg_inc/runtime/runtime"
        "${ASCEND_HOME_PATH}/pkg_inc/profiling"
        "${ASCEND_HOME_PATH}/${CMAKE_SYSTEM_PROCESSOR}-linux/asc/include"
        "${ASCEND_HOME_PATH}/${CMAKE_SYSTEM_PROCESSOR}-linux/include/driver"
)

target_link_directories(esl_onboard_runner
    PRIVATE
        "${ASCEND_HOME_PATH}/lib64"
        "${ASCEND_HOME_PATH}/runtime/lib64"
)

target_link_libraries(esl_onboard_runner
    PRIVATE
        ascendcl
        runtime
        unified_dlog
        dl
        pthread
)

set_target_properties(esl_onboard_runner PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${ROOT}/build/onboard/host"
)
