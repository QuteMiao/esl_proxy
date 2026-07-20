# esl_proxy a2a3 AICPU kernel. Self-contained: platform runtime lives under
# platform/a2a3 (passed as ESL_ONBOARD_DIR). No SIMPLER_ROOT. All kernel
# sources/includes arrive via CUSTOM_SOURCE_DIRS / CUSTOM_INCLUDE_DIRS; only the
# separate aicpu_dispatcher lib is named here.
cmake_minimum_required(VERSION 3.16.3)

project(aicpu_kernel LANGUAGES C CXX)

if(NOT DEFINED ASCEND_HOME_PATH)
    if(DEFINED ENV{ASCEND_HOME_PATH})
        set(ASCEND_HOME_PATH "$ENV{ASCEND_HOME_PATH}")
    else()
        message(FATAL_ERROR "MUST set ASCEND_HOME_PATH")
    endif()
endif()
if(NOT DEFINED ESL_ONBOARD_DIR)
    get_filename_component(ESL_ONBOARD_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../esl_proxy/src/platform/a2a3" ABSOLUTE)
endif()

set(CMAKE_CUSTOM_INCLUDE_DIRS "")
if(DEFINED CUSTOM_INCLUDE_DIRS)
    foreach(INC_DIR ${CUSTOM_INCLUDE_DIRS})
        list(APPEND CMAKE_CUSTOM_INCLUDE_DIRS "${INC_DIR}")
    endforeach()
else()
    message(FATAL_ERROR "MUST set CUSTOM_INCLUDE_DIRS to build AICPU kernel")
endif()

# AICPU kernel sources come from the shared list in cmake/sources.cmake
# (relative paths), made absolute via ESL_CORE = <repo>/esl_proxy.
include(${CMAKE_CURRENT_LIST_DIR}/../sources.cmake)
get_filename_component(ESL_CORE "${CMAKE_CURRENT_SOURCE_DIR}/../../esl_proxy" ABSOLUTE)
set(AICPU_KERNEL_SOURCES "")
foreach(_src ${ESL_PLATFORM_A2A3_SRCS} ${ESL_ALGORITHM_SRCS})
    list(APPEND AICPU_KERNEL_SOURCES "${ESL_CORE}/${_src}")
endforeach()
list(LENGTH AICPU_KERNEL_SOURCES NUM_AICPU_KERNEL_SOURCES)
message(STATUS "esl_proxy AICpu kernel: ${NUM_AICPU_KERNEL_SOURCES} source files")

add_library(aicpu_kernel SHARED ${AICPU_KERNEL_SOURCES})

option(WERROR "Treat compiler warnings as errors" ON)
if(DEFINED ENV{SIMPLER_DISABLE_WARNINGS_AS_ERRORS})
    set(WERROR OFF)
endif()

# ESL_PROXY_ONBOARD isolates onboard cmake from Makefile/sim (do not define there).
# a2a3: AIC_CNT=24 overrides conf.h default 60 (also passed via run_onboard CMAKE_C_FLAGS).
target_compile_definitions(aicpu_kernel PRIVATE ESL_PROXY_ONBOARD=1 AIC_CNT=24)

# Match sim Makefile DISPATCH=double_buffer (-DESL_DISPATCH_DOUBLE_BUFFER=1).
if(DEFINED ESL_PROXY_DOUBLE_BUFFER AND ESL_PROXY_DOUBLE_BUFFER)
    target_compile_definitions(aicpu_kernel PRIVATE ESL_DISPATCH_DOUBLE_BUFFER=1)
endif()

target_compile_options(aicpu_kernel
    PRIVATE
        -Wall
        -Wextra
        -rdynamic
        -O3
        -fPIC
        $<$<BOOL:${WERROR}>:-Werror>
        -Wno-error=variadic-macros
        -fno-common
        -g
        $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++17>
        $<$<COMPILE_LANGUAGE:C>:-std=gnu11>
)

target_include_directories(aicpu_kernel
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CUSTOM_INCLUDE_DIRS}
        ${ASCEND_HOME_PATH}/include
        ${ASCEND_HOME_PATH}/include/toolchain
        ${ASCEND_HOME_PATH}/pkg_inc/base
)

target_link_directories(aicpu_kernel
    PRIVATE
        ${ASCEND_HOME_PATH}/${CMAKE_SYSTEM_PROCESSOR}-linux/devlib/linux/aarch64/
        ${ASCEND_HOME_PATH}/${CMAKE_SYSTEM_PROCESSOR}-linux/devlib/device/
        ${ASCEND_HOME_PATH}/lib64
)

set_target_properties(aicpu_kernel PROPERTIES OUTPUT_NAME aicpu_kernel)

set(AICPU_DISPATCHER_SOURCES
    "${ESL_ONBOARD_DIR}/aicpu_dispatcher.c"
    "${ESL_ONBOARD_DIR}/tools.c"
)
add_library(aicpu_dispatcher SHARED ${AICPU_DISPATCHER_SOURCES})

target_compile_options(aicpu_dispatcher
    PRIVATE
        -Wall
        -Wextra
        -rdynamic
        -O3
        -fPIC
        -g
        $<$<COMPILE_LANGUAGE:C>:-std=gnu11>
)

target_include_directories(aicpu_dispatcher
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CUSTOM_INCLUDE_DIRS}
        ${ASCEND_HOME_PATH}/include
        ${ASCEND_HOME_PATH}/pkg_inc/base
)

target_link_libraries(aicpu_dispatcher PRIVATE dl)

set_target_properties(aicpu_dispatcher PROPERTIES
    LINK_FLAGS "-Wl,--build-id"
    OUTPUT_NAME "esl_aicpu_dispatcher"
)
