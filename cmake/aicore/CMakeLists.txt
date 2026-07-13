# esl_proxy onboard AICore kernel — platform entry + algorithm executor.
cmake_minimum_required(VERSION 3.16.3)

if(NOT DEFINED BISHENG_CC OR NOT DEFINED BISHENG_LD)
    message(FATAL_ERROR "BISHENG_CC and BISHENG_LD must be set (ccec toolchain)")
endif()

set(CMAKE_CUSTOM_INCLUDE_DIRS "")
if(DEFINED CUSTOM_INCLUDE_DIRS)
    foreach(INC_DIR ${CUSTOM_INCLUDE_DIRS})
        list(APPEND CMAKE_CUSTOM_INCLUDE_DIRS "${INC_DIR}")
    endforeach()
else()
    message(FATAL_ERROR "MUST set CUSTOM_INCLUDE_DIRS")
endif()

if(NOT DEFINED ESL_PROXY_ENABLE_L2_SWIMLANE)
    set(ESL_PROXY_ENABLE_L2_SWIMLANE 0)
endif()

if(NOT DEFINED AICORE_ENTRY_SRC OR NOT DEFINED AICORE_EXECUTOR_SRC)
    message(FATAL_ERROR "MUST set AICORE_ENTRY_SRC and AICORE_EXECUTOR_SRC")
endif()

set(ALL_SOURCES
    "${AICORE_ENTRY_SRC}"
    "${AICORE_EXECUTOR_SRC}"
)

list(LENGTH ALL_SOURCES NUM_SOURCES)
message(STATUS "esl_proxy AICore kernel: ${NUM_SOURCES} source files")

set(AICORE_KERNEL_OBJ ${CMAKE_CURRENT_BINARY_DIR}/aicore_kernel.o)

set(CMAKE_CUSTOM_INCLUDE_DIR_FLAGS "")
foreach(INC_DIR ${CMAKE_CUSTOM_INCLUDE_DIRS})
    list(APPEND CMAKE_CUSTOM_INCLUDE_DIR_FLAGS "-I${INC_DIR}")
endforeach()

if(NOT DEFINED ESL_LANE_CNT)
    set(ESL_LANE_CNT 1)
endif()

set(AICORE_FLAGS_COMMON
    "-c -O3 -g -x cce -Wall \
    --cce-aicore-only \
    -DESL_PROXY_AICORE_FAKE_FIN \
    -DESL_LANE_CNT=${ESL_LANE_CNT} \
    -DESL_PROXY_ENABLE_L2_SWIMLANE=${ESL_PROXY_ENABLE_L2_SWIMLANE} \
    -mllvm -cce-aicore-stack-size=0x8000 \
    -mllvm -cce-aicore-function-stack-size=0x8000 \
    -mllvm -cce-aicore-record-overflow=false \
    -mllvm -cce-aicore-addr-transform \
    -mllvm -cce-aicore-dcci-insert-for-scalar=false \
    ${CMAKE_CUSTOM_INCLUDE_DIR_FLAGS}"
)
separate_arguments(AICORE_FLAGS_COMMON)

set(ALL_AIC_OBJECTS "")
set(ALL_AIV_OBJECTS "")

foreach(SRC_FILE ${ALL_SOURCES})
    get_filename_component(SRC_NAME ${SRC_FILE} NAME_WE)
    get_filename_component(SRC_EXT ${SRC_FILE} EXT)
    set(OBJ_AIC "${CMAKE_CURRENT_BINARY_DIR}/${SRC_NAME}_aic.o")
    set(OBJ_AIV "${CMAKE_CURRENT_BINARY_DIR}/${SRC_NAME}_aiv.o")
    list(APPEND ALL_AIC_OBJECTS ${OBJ_AIC})
    list(APPEND ALL_AIV_OBJECTS ${OBJ_AIV})

    if(SRC_EXT STREQUAL ".cpp")
        set(AICORE_LANG_FLAGS -std=c++17)
    else()
        # ccec CCE front-end rejects -std=c11; rely on default C dialect for .c sources.
        set(AICORE_LANG_FLAGS "")
    endif()

    add_custom_command(
        OUTPUT ${OBJ_AIC}
        COMMAND ${BISHENG_CC} ${AICORE_FLAGS_COMMON} ${AICORE_LANG_FLAGS}
                --cce-aicore-arch=dav-c220-cube -o ${OBJ_AIC} ${SRC_FILE}
        DEPENDS ${SRC_FILE}
        COMMENT "Compiling ${SRC_NAME} for AIC"
    )
    add_custom_command(
        OUTPUT ${OBJ_AIV}
        COMMAND ${BISHENG_CC} ${AICORE_FLAGS_COMMON} ${AICORE_LANG_FLAGS}
                --cce-aicore-arch=dav-c220-vec -o ${OBJ_AIV} ${SRC_FILE}
        DEPENDS ${SRC_FILE}
        COMMENT "Compiling ${SRC_NAME} for AIV"
    )
endforeach()

set(COMBINED_AIC_OBJ "${CMAKE_CURRENT_BINARY_DIR}/aicore_aic_combined.o")
set(COMBINED_AIV_OBJ "${CMAKE_CURRENT_BINARY_DIR}/aicore_aiv_combined.o")

add_custom_command(
    OUTPUT ${COMBINED_AIC_OBJ}
    COMMAND ${BISHENG_LD} -r -o ${COMBINED_AIC_OBJ} ${ALL_AIC_OBJECTS}
    DEPENDS ${ALL_AIC_OBJECTS}
    COMMENT "Combining AIC object files"
)
add_custom_command(
    OUTPUT ${COMBINED_AIV_OBJ}
    COMMAND ${BISHENG_LD} -r -o ${COMBINED_AIV_OBJ} ${ALL_AIV_OBJECTS}
    DEPENDS ${ALL_AIV_OBJECTS}
    COMMENT "Combining AIV object files"
)

add_custom_target(aicore_kernel ALL
    DEPENDS ${COMBINED_AIC_OBJ} ${COMBINED_AIV_OBJ}
    COMMAND ${BISHENG_LD} -m aicorelinux -Ttext=0 -static -n
            -o ${AICORE_KERNEL_OBJ} ${COMBINED_AIC_OBJ} ${COMBINED_AIV_OBJ}
    BYPRODUCTS ${AICORE_KERNEL_OBJ}
    COMMENT "Linking esl_proxy AICore kernel binary"
)
