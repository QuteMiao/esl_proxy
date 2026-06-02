# esl_proxy build
#
# Usage:
#   make          # compile (orchestration: cases/qwen3_decode.h, all-SPMD)
#   make run      # compile and run
#   make run WORKER_LOG=1   # run with worker logs
#   make CASE=qwen3_decode_tensormap.h run   # run the tensormap auto-dep case
#   make clean    # remove build artifacts
#
# CASE selects which cases/*.h orchestration header main.c compiles in.

CASE     ?= qwen3_decode.h

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

ifneq ($(filter qwen3_decode_tensormap.h,$(CASE)),)
CFLAGS   += -DUSE_TENSORMAP
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

OBJS := $(SRCS:src/%.c=$(OBJ_DIR)/%.o)
DEPS := $(OBJS:.o=.d)

CASE_STAMP := $(OBJ_DIR)/.case-stamp

.PHONY: all run clean force

all: $(TARGET)

$(TARGET): $(OBJS) | $(BIN_DIR)
	$(CC) $(CFLAGS) $(OBJS) -o $@ $(LDFLAGS)

$(OBJ_DIR)/%.o: src/%.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Rebuild main.o whenever CASE changes (it bakes ORCH_CASE in via -D).
$(OBJ_DIR)/main.o: $(CASE_STAMP)
$(CASE_STAMP): force | $(OBJ_DIR)
	@echo '$(CASE)' | cmp -s - $@ 2>/dev/null || echo '$(CASE)' > $@

$(OBJ_DIR) $(BIN_DIR):
	mkdir -p $@

run: all
	WORKER_LOG=$(WORKER_LOG) $(TARGET)

clean:
	rm -rf build $(BIN_DIR)

-include $(DEPS)
