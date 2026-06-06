# esl_proxy build
#
# Usage:
#   make          # default: qwen3_dynamic_manual_scope.h, QWEN3_SPMD_TIER=4
#   make run
#   make run WORKER_LOG=1
#
# Cases (cases/*.h):
#   make CASE=qwen3_dynamic_manual_scope.h run
#   make CASE=qwen3_dynamic_tensormap.h run
#   make CASE=paged_attention_unroll.h run
#   make CASE=paged_attention_unroll_manual_scope.h run
#
# Qwen3 SPMD tier (0=non-spmd .. 4=all-spmd; default 4 for qwen3 cases only):
#   make CASE=qwen3_dynamic_tensormap.h QWEN3_SPMD_TIER=0 run
#
# Other compile / run flags:
#   make DEP_DUMP=1 CASE=qwen3_dynamic_tensormap.h run
#   make NO_DEPS=1 CASE=qwen3_dynamic_tensormap.h run
#   make MAIN_LOG=0 CASE=paged_attention_unroll.h run
#   DEP_DUMP=1 DEP_DUMP_FILE=build/dep_ ./bin/esl_proxy
#   make test-dep-dump
#   make clean
#
# CASE selects which cases/*.h orchestration header main.c compiles in.

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
# -MMD -MP emits a .d alongside each .o listing the headers it included, so a
# change to any include/*.h (e.g. tensormap.h) rebuilds the dependent objects.
CFLAGS   := -std=c11 -Wall -Wextra -pedantic -O3 -I include -I cases -DORCH_CASE=$(CASE) -MMD -MP

# -latomic is needed on Linux but not on macOS (clang)
UNAME_S  := $(shell uname -s)
ifeq ($(UNAME_S),Linux)
    LDFLAGS  := -lpthread -latomic
else
    LDFLAGS  := -lpthread
endif

# All four cases use struct Tensor / tensormap helpers.
CFLAGS   += -DUSE_TENSORMAP

# Qwen3 only: default all-SPMD tier; override via make QWEN3_SPMD_TIER=N.
ifneq ($(filter $(CASE),$(QWEN3_CASES)),)
QWEN3_SPMD_TIER ?= 4
CFLAGS   += -DQWEN3_SPMD_TIER=$(QWEN3_SPMD_TIER)
endif

ifneq ($(DEP_DUMP),)
CFLAGS   += -DDEP_DUMP=$(DEP_DUMP)
endif

ifneq ($(NO_DEPS),)
CFLAGS   += -DNO_DEPS=$(NO_DEPS)
endif

ifneq ($(MAIN_LOG),)
CFLAGS   += -DMAIN_LOG=$(MAIN_LOG)
endif

OBJ_DIR := build/obj
BIN_DIR := bin
TARGET  := $(BIN_DIR)/esl_proxy

SRCS := \
	src/main.c \
	src/executor.c \
	src/dispatch.c \
	src/cutter.c \
	src/manager.c \
	src/log.c \
	src/shm.c

ifeq ($(DEP_DUMP),1)
SRCS += src/dep_dump.c
endif

OBJS := $(SRCS:src/%.c=$(OBJ_DIR)/%.o)
DEPS := $(OBJS:.o=.d)

# Rebuild orchestration when case or orch-related flags change.
ORCH_STAMP := $(OBJ_DIR)/.orch-stamp
ORCH_CONFIG := case=$(CASE) tier=$(QWEN3_SPMD_TIER) dep=$(DEP_DUMP) nodeps=$(NO_DEPS) log=$(MAIN_LOG)

TEST_CFLAGS := -std=c11 -Wall -Wextra -pedantic -O0 -I include -I cases -DUSE_TENSORMAP -DDEP_DUMP=1

.PHONY: all run clean force test-dep-dump

all: $(TARGET)

$(TARGET): $(OBJS) | $(BIN_DIR)
	$(CC) $(CFLAGS) $(OBJS) -o $@ $(LDFLAGS)

$(OBJ_DIR)/%.o: src/%.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Rebuild main.o whenever orchestration config changes.
$(OBJ_DIR)/main.o: $(ORCH_STAMP)
$(ORCH_STAMP): force | $(OBJ_DIR)
	@echo '$(ORCH_CONFIG)' | cmp -s - $@ 2>/dev/null || echo '$(ORCH_CONFIG)' > $@

$(OBJ_DIR) $(BIN_DIR):
	mkdir -p $@


run: all
	WORKER_LOG=$(WORKER_LOG) DEP_DUMP=$(DEP_DUMP) $(TARGET)

test-dep-dump: $(BIN_DIR)/test_dep_dump $(BIN_DIR)/test_dep_dump_qwen3_edges
	$(BIN_DIR)/test_dep_dump
	$(BIN_DIR)/test_dep_dump_qwen3_edges

$(BIN_DIR)/test_dep_dump: tests/test_dep_dump.c src/dep_dump.c | $(BIN_DIR)
	$(CC) $(TEST_CFLAGS) tests/test_dep_dump.c src/dep_dump.c -o $@ $(LDFLAGS)

QWEN3_TEST_TIER ?= 4
$(BIN_DIR)/test_dep_dump_qwen3_edges: tests/test_dep_dump_qwen3_edges.c src/dep_dump.c | $(BIN_DIR)
	$(CC) $(TEST_CFLAGS) -DQWEN3_SPMD_TIER=$(QWEN3_TEST_TIER) tests/test_dep_dump_qwen3_edges.c src/dep_dump.c -o $@ $(LDFLAGS)

clean:
	rm -rf build $(BIN_DIR)

-include $(DEPS)
