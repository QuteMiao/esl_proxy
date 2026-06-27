# esl_proxy build
#
# Usage:
#   make          # default: qwen3_dynamic_manual_scope.h, QWEN3_SPMD_TIER=2
#   make run
#   make run WORKER_LOG=1
#
# Cases (cases/*.h):
#   make CASE=qwen3_dynamic_manual_scope.h run
#   make CASE=qwen3_dynamic_tensormap.h run
#   make CASE=paged_attention_unroll.h run
#   make CASE=paged_attention_unroll_manual_scope.h run
#
# Qwen3 SPMD tier (0=non-spmd .. 4=all-spmd; default 2 = spmd-4 for qwen3 cases):
#   make CASE=qwen3_dynamic_tensormap.h QWEN3_SPMD_TIER=0 run
#
# Other compile / run flags:
#   make MAIN_LOG=0 CASE=paged_attention_unroll.h run
#   make clean
#
# CASE selects which cases/*.h orchestration header main.c compiles in.
# Algorithm / platform sim source lists: ../build/sources.sh

ALL_CASES := \
	qwen3_dynamic_manual_scope.h \
	qwen3_dynamic_tensormap.h \
	paged_attention_unroll.h \
	paged_attention_unroll_manual_scope.h

CASE ?= qwen3_dynamic_manual_scope.h

ifneq ($(filter $(CASE),$(ALL_CASES)),$(CASE))
$(error Unknown CASE='$(CASE)'. Valid: $(ALL_CASES))
endif

QWEN3_CASES := qwen3_dynamic_manual_scope.h qwen3_dynamic_tensormap.h

CC       := cc
CXX      := c++
# -MMD -MP emits a .d alongside each .o listing the headers it included, so a
# change to any include/*.h (e.g. tensormap.h) rebuilds the dependent objects.
CFLAGS   := -g -std=c11 -Wall -Wextra -pedantic -O2 -D_POSIX_C_SOURCE=199309L \
	-I include/algorithm -I include/platform -I include/platform/sim \
	-I include/platform/onboard -I include/swimlane \
	-I cases -DORCH_CASE=$(CASE) -MMD -MP -Wno-error=implicit-function-declaration

CXXFLAGS := -g -std=c++17 -Wall -Wextra -pedantic -O2 -D_POSIX_C_SOURCE=199309L \
	-I include/algorithm -I include/platform -I include/platform/sim \
	-I include/platform/onboard -I include/swimlane \
	-I cases -DORCH_CASE=$(CASE) -MMD -MP -Wno-error=implicit-function-declaration \
	-DESL_PROXY_AICORE_HOST

# -latomic is needed on Linux but not on macOS (clang)
UNAME_S  := $(shell uname -s)
ifeq ($(UNAME_S),Linux)
    LDFLAGS  := -lpthread -latomic
else
    LDFLAGS  := -lpthread
endif

# Qwen3 only: default spmd-4 tier; override via make QWEN3_SPMD_TIER=N.
ifneq ($(filter $(CASE),$(QWEN3_CASES)),)
QWEN3_SPMD_TIER ?= 0
CFLAGS   += -DQWEN3_SPMD_TIER=$(QWEN3_SPMD_TIER)
CXXFLAGS += -DQWEN3_SPMD_TIER=$(QWEN3_SPMD_TIER)
endif

ifneq ($(MAIN_LOG),)
CFLAGS   += -DMAIN_LOG=$(MAIN_LOG)
CXXFLAGS += -DMAIN_LOG=$(MAIN_LOG)
endif

ifneq ($(EXTRA_CFLAGS),)
CFLAGS   += $(EXTRA_CFLAGS)
CXXFLAGS += $(EXTRA_CFLAGS)
endif

OBJ_DIR := build/obj
BIN_DIR := bin
TARGET  := $(BIN_DIR)/esl_proxy

SRCS := \
	src/main.c \
	src/algorithm/dispatch.c \
	src/algorithm/dispatch_expand.c \
	src/algorithm/cutter.c \
	src/algorithm/executor.c \
	src/algorithm/handshake.c \
	src/platform/sim/platform_init.c \
	src/platform/sim/platform_regs.c \
	src/platform/sim/cache_ops.c \
	src/platform/sim/platform_sim.c \
	src/platform/sim/sim_core_regs.c \
	src/platform/sim/aicore.c \
	src/platform/sim/device_runner.c \
	src/algorithm/manager.c \
	src/algorithm/log.c \
	src/algorithm/shm.c \
	src/algorithm/aicore_executor.c

CXXSRCS := \
	src/platform/sim/aicore_wrapper.cpp

OBJS := $(patsubst src/%.c,$(OBJ_DIR)/%.o,$(SRCS))
CXXOBJS := $(patsubst src/%.cpp,$(OBJ_DIR)/%.o,$(CXXSRCS))
DEPS := $(OBJS:.o=.d) $(CXXOBJS:.o=.d)

# Rebuild orchestration when case or orch-related flags change.
ORCH_STAMP := $(OBJ_DIR)/.orch-stamp
ORCH_CONFIG := case=$(CASE) tier=$(QWEN3_SPMD_TIER) log=$(MAIN_LOG)

.PHONY: all run asm clean force test-tensormap

# Default target is the sim binary (matches main). `asm` precedes `all` textually,
# so pin the default goal explicitly instead of relying on rule order.
.DEFAULT_GOAL := all

# Generate .s assembly: sim → gcc -S; onboard AICore → ccec .o + tools/obj_to_gas.py
# Requires ASCEND_HOME_PATH for AICore output (see ../tools/gen_asm.sh).
asm:
	bash ../tools/gen_asm.sh

all: $(TARGET)

gen-dag:
	bash ../tools/gen_all_dags.sh

$(TARGET): $(OBJS) $(CXXOBJS) | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(OBJS) $(CXXOBJS) -o $@ $(LDFLAGS)

$(OBJ_DIR)/algorithm/aicore_executor.o: CFLAGS += -DESL_PROXY_AICORE_HOST

$(OBJ_DIR)/%.o: src/%.c | $(OBJ_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/%.o: src/%.cpp | $(OBJ_DIR)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Rebuild main.o whenever orchestration config changes.
$(OBJ_DIR)/main.o: $(ORCH_STAMP)
$(ORCH_STAMP): force | $(OBJ_DIR)
	@echo '$(ORCH_CONFIG)' | cmp -s - $@ 2>/dev/null || echo '$(ORCH_CONFIG)' > $@

$(OBJ_DIR) $(BIN_DIR):
	mkdir -p $@


run: all
	WORKER_LOG=$(WORKER_LOG) $(TARGET)

test-tensormap: $(BIN_DIR)/test_tensormap
	$(BIN_DIR)/test_tensormap

TEST_TENSORMAP_CFLAGS := -std=c11 -Wall -Wextra -Werror -pedantic -O2 -D_POSIX_C_SOURCE=200809L -I include/algorithm

$(BIN_DIR)/test_tensormap: tests/test_tensormap.c | $(BIN_DIR)
	$(CC) $(TEST_TENSORMAP_CFLAGS) tests/test_tensormap.c -o $@ $(LDFLAGS)

clean:
	rm -rf build $(BIN_DIR)

-include $(DEPS)
