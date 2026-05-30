# esl_proxy build
#
# Usage:
#   make          # compile (orchestration: cases/qwen3_decode.h, all-SPMD)
#   make run      # compile and run
#   make run WORKER_LOG=1   # run with worker logs
#   make clean    # remove build artifacts

CC       := cc
CFLAGS   := -std=c11 -Wall -Wextra -pedantic -O3 -I include -I cases
LDFLAGS  := -lpthread -latomic

OBJ_DIR := build/obj
BIN_DIR := bin
TARGET  := $(BIN_DIR)/esl_proxy

SRCS := \
	src/main.c \
	src/dispatch.c \
	src/cutter.c \
	src/manager.c \
	src/log.c \
	src/shm.c

OBJS := $(SRCS:src/%.c=$(OBJ_DIR)/%.o)

.PHONY: all run clean

all: $(TARGET)

$(TARGET): $(OBJS) | $(BIN_DIR)
	$(CC) $(CFLAGS) $(OBJS) -o $@ $(LDFLAGS)

$(OBJ_DIR)/%.o: src/%.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR) $(BIN_DIR):
	mkdir -p $@

run: all
	WORKER_LOG=$(WORKER_LOG) $(TARGET)

clean:
	rm -rf build $(BIN_DIR)
