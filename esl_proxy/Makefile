# esl_proxy build
#
# Usage:
#   make          # default: qwen3_dynamic_manual_scope.h, THREAD_CNT=1
#   make run
#   make run WORKER_LOG=1
#
# Cases (cases/*.h):
#   make CASE=qwen3_dynamic_manual_scope.h run
#   make CASE=qwen3_dynamic_tensormap.h run
#   make CASE=qwen3_14b_decode_subgraph.h run
#   make CASE=paged_attention_unroll.h run
#   make CASE=paged_attention_unroll_manual_scope.h run
#
# Thread count (default 1):
#   make CASE=qwen3_14b_decode_subgraph.h THREAD_CNT=2 run
#
# Qwen3 SPMD tier (0=non-spmd .. 4=all-spmd; default 2 = spmd-4 for qwen3 cases):
#   make CASE=qwen3_dynamic_tensormap.h QWEN3_SPMD_TIER=0 run
#
# Other compile / run flags:
#   make MAIN_LOG=0 CASE=paged_attention_unroll.h run
#   make clean
#
# Swimlane trace (可视化，默认关闭；开启后 makespan 不可用于性能对比):
#   make swimlane ED_ENABLE=1 SWIMLANE_DIR=report/ed_on
#   make swimlane ED_ENABLE=0 SWIMLANE_DIR=report/ed_off
#   然后把 <dir>/merged_swimlane.json 拖进 https://ui.perfetto.dev/
#
# CASE selects which cases/*.h orchestration header main.c compiles in.

ALL_CASES := \
	qwen3_dynamic_manual_scope.h \
	qwen3_dynamic_tensormap.h \
	paged_attention_unroll.h \
	paged_attention_unroll_manual_scope.h \
	ed_a10_probe.h \
	ed_a11_probe.h \
	ed_a12_ring_stress.h

CASE ?= qwen3_dynamic_manual_scope.h

ifneq ($(filter $(CASE),$(ALL_CASES)),$(CASE))
$(error Unknown CASE='$(CASE)'. Valid: $(ALL_CASES))
endif

QWEN3_CASES := qwen3_dynamic_manual_scope.h qwen3_dynamic_tensormap.h

# Thread count for cutter/dispatch threads (default: 1)
# 目前只支持 THREAD_CNT=1：cutter 的 send_2_ready_queue 把 target_ctrl 写死为 0，
# THREAD_CNT>=2 时 1 号 dispatcher 永远拿不到任务，收尾条件不成立会挂住。
THREAD_CNT ?= 1

CC       := cc
# -MMD -MP emits a .d alongside each .o listing the headers it included, so a
# change to any include/algorithm/*.h (e.g. tensormap.h) rebuilds the dependent objects.
# _POSIX_C_SOURCE 必须 >= 200809L：199309L (POSIX.1b-1993) 早于 C99，会让 macOS 的
# <stdio.h> 隐藏 snprintf 等 C99 函数，log.c 因此报 implicit-function-declaration。
# 200809L 是 199309L 的超集，不会丢失原有特性。改这里时 src/*.c、tests/*.c 与
# tools/gen_compile_commands.py 里的同名 #define 必须同步，否则会触发宏重定义告警。
CFLAGS   := -g -std=c11 -Wall -Wextra -pedantic -O2 -D_POSIX_C_SOURCE=200809L \
	-I include/algorithm -I cases -DORCH_CASE=$(CASE) -MMD -MP -Wno-error=implicit-function-declaration \
	-DCUTTER_THREAD_CNT=$(THREAD_CNT) -DDISPATCH_THREAD_CNT=$(THREAD_CNT)

# -latomic is needed on Linux but not on macOS (clang)
UNAME_S  := $(shell uname -s)
ifeq ($(UNAME_S),Linux)
    LDFLAGS  := -static -lpthread -latomic
else
    LDFLAGS  := -lpthread
endif

# All four cases use struct Tensor / tensormap helpers.
# Qwen3 only: default spmd-4 tier; override via make QWEN3_SPMD_TIER=N.
ifneq ($(filter $(CASE),$(QWEN3_CASES)),)
QWEN3_SPMD_TIER ?= 2
CFLAGS   += -DQWEN3_SPMD_TIER=$(QWEN3_SPMD_TIER)
endif

ifneq ($(MAIN_LOG),)
CFLAGS   += -DMAIN_LOG=$(MAIN_LOG)
endif

ifneq ($(EXTRA_CFLAGS),)
CFLAGS   += $(EXTRA_CFLAGS)
endif

# Early-dispatch compile switch (Step 1+)
ED_ENABLE ?= 0
ED_UNFIN_THRESHOLD ?= 0xFFFF
# 进入 ED 的最大 block 数。默认 1 = 只接非 SPMD 任务；放开的实测代价见 conf.h 说明。
ED_SPMD_MAX_BLOCKS ?= 1
# swimlane 目标会把默认值改成 200，原因见那里
ifeq ($(MAKECMDGOALS),swimlane)
EXEC_DURATION_SCALE ?= 200
else
EXEC_DURATION_SCALE ?= 10000
endif
# 每核每 type 的在途槽位数（预装载深度），语义见 conf.h。
AIC_OSTD ?= 2
# 逐任务 ready->runnable 延迟分解探针；诊断用，默认关闭。
# 开启后 CSV 路径由运行时环境变量 LAT_TRACE_CSV 指定。
LAT_TRACE ?= 0
# 泳道图打点；可视化用，默认关闭。开启后 makespan 不可用于性能对比，见 swimlane.h。
# 输出 JSON 路径由运行时环境变量 SWIMLANE_JSON 指定，默认 report/swimlane_records.json。
SWIMLANE ?= 0
CFLAGS   += -DED_ENABLE=$(ED_ENABLE) -DED_UNFIN_THRESHOLD=$(ED_UNFIN_THRESHOLD) \
	-DED_SPMD_MAX_BLOCKS=$(ED_SPMD_MAX_BLOCKS) \
	-DEXEC_DURATION_SCALE=$(EXEC_DURATION_SCALE) -DAIC_OSTD=$(AIC_OSTD) \
	-DLAT_TRACE=$(LAT_TRACE) -DSWIMLANE=$(SWIMLANE)

OBJ_DIR := build/obj
BIN_DIR := bin
TARGET  := $(BIN_DIR)/esl_proxy

SRCS := \
	src/main.c \
	src/algorithm/executor.c \
	src/algorithm/dispatch.c \
	src/algorithm/cutter.c \
	src/algorithm/manager.c \
	src/algorithm/log.c \
	src/algorithm/shm.c \
	src/algorithm/early_dispatch.c \
	src/algorithm/swimlane.c

OBJS := $(patsubst src/%.c,$(OBJ_DIR)/%.o,$(SRCS))
DEPS := $(OBJS:.o=.d)

# 配置指纹：任何影响编译结果的开关都必须出现在这里，包括 EXTRA_CFLAGS
# （scripts/ed_a11_probe.sh、ed_a12_probe.sh 用它注入 -DED_A11_PROBE 等探针宏）。
ORCH_STAMP := build/.orch-config
ORCH_CONFIG := case=$(CASE) tier=$(QWEN3_SPMD_TIER) log=$(MAIN_LOG) ed=$(ED_ENABLE) ed_thr=$(ED_UNFIN_THRESHOLD) ed_spmd_max=$(ED_SPMD_MAX_BLOCKS) dur_scale=$(EXEC_DURATION_SCALE) ostd=$(AIC_OSTD) lat_trace=$(LAT_TRACE) swimlane=$(SWIMLANE) threads=$(THREAD_CNT) extra=$(EXTRA_CFLAGS)

# 配置切换保护：在 Makefile 解析阶段（早于任何目标被考虑）比对指纹，不一致就直接
# 删掉全部产物，强制重编重链。
#
# 不能用"戳文件 + $(OBJS): $(ORCH_STAMP)"的时间戳写法：macOS 自带 GNU Make 3.81
# 只比较到「秒」，1 秒内连续切换 CASE/ED_ENABLE 时，新戳文件与旧 .o 落在同一秒，
# make 判定 .o 仍是最新而复用。轻则链接报 undefined symbol，重则静默链出与请求
# 配置不符的二进制——性能数据会因此对不上配置。实测连跑 8 个 case 有 5 个中招。
ORCH_RESET := $(shell \
	mkdir -p $(dir $(ORCH_STAMP)); \
	printf '%s\n' '$(ORCH_CONFIG)' | cmp -s - $(ORCH_STAMP) 2>/dev/null || { \
		rm -rf $(OBJ_DIR) $(BIN_DIR); \
		printf '%s\n' '$(ORCH_CONFIG)' > $(ORCH_STAMP); \
	})

.PHONY: all run clean swimlane test-tensormap test-ed-record test-step2 test-step3 test-step4 test-step5 test-step6 test-step7 test-step9 test-cutter-window test-ed-spmd compile_commands

compile_commands:
	python3 ./tools/gen_compile_commands.py

all: $(TARGET)

$(TARGET): $(OBJS) | $(BIN_DIR)
	$(CC) $(CFLAGS) $(OBJS) -o $@ $(LDFLAGS)

$(OBJ_DIR)/%.o: src/%.c | $(OBJ_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR) $(BIN_DIR):
	mkdir -p $@


run: all
	WORKER_LOG=$(WORKER_LOG) $(TARGET)

# 一键出泳道图: 强制 SWIMLANE=1 重编、跑一次、转成 Perfetto trace。
#
# 默认把 EXEC_DURATION_SCALE 从 10000 放大到 200: 默认缩放下几乎每个任务都只跑
# 1 tick, run 段退化成零宽, 图上只剩排队结构看不到核占用。想看真实比例请显式
# 覆盖该变量。
#
# SWIMLANE_DIR 决定产物落在哪, 做 ED on/off 对照时分开放:
#   make swimlane ED_ENABLE=1 SWIMLANE_DIR=report/ed_on
#   make swimlane ED_ENABLE=0 SWIMLANE_DIR=report/ed_off
SWIMLANE_DIR ?= report
swimlane:
	$(MAKE) SWIMLANE=1 EXEC_DURATION_SCALE=$(EXEC_DURATION_SCALE) all
	SWIMLANE_JSON=$(SWIMLANE_DIR)/swimlane_records.json $(TARGET) >/dev/null
	python3 ../tools/swimlane_to_perfetto.py $(SWIMLANE_DIR)/swimlane_records.json

test-tensormap: $(BIN_DIR)/test_tensormap
	$(BIN_DIR)/test_tensormap

TEST_TENSORMAP_CFLAGS := -std=c11 -Wall -Wextra -Werror -pedantic -O2 -D_POSIX_C_SOURCE=200809L -I include/algorithm

$(BIN_DIR)/test_tensormap: tests/test_tensormap.c | $(BIN_DIR)
	$(CC) $(TEST_TENSORMAP_CFLAGS) tests/test_tensormap.c -o $@ $(LDFLAGS)

test-ed-record: $(BIN_DIR)/test_ed_record
	$(BIN_DIR)/test_ed_record

# early_dispatch.o 引用 g_basic_buf / g_ctrl_t / g_executors（均定义在 shm.c），
# 故必须联 shm.o。该规则写于 Step 1，当时 early_dispatch.c 还没有这些依赖，
# 后续步骤加进来后未同步更新，导致 test-ed-record 一直链接失败。
$(BIN_DIR)/test_ed_record: tests/test_ed_record.c \
	$(OBJ_DIR)/algorithm/early_dispatch.o \
	$(OBJ_DIR)/algorithm/shm.o \
	$(OBJ_DIR)/algorithm/log.o \
	$(OBJ_DIR)/algorithm/swimlane.o | $(BIN_DIR)
	$(CC) $(CFLAGS) tests/test_ed_record.c \
		$(OBJ_DIR)/algorithm/early_dispatch.o \
		$(OBJ_DIR)/algorithm/shm.o \
		$(OBJ_DIR)/algorithm/log.o \
		$(OBJ_DIR)/algorithm/swimlane.o -o $@ $(LDFLAGS)

test-step2: $(BIN_DIR)/test_step2_protocol
	$(BIN_DIR)/test_step2_protocol

$(BIN_DIR)/test_step2_protocol: tests/test_step2_protocol.c \
	$(OBJ_DIR)/algorithm/dispatch.o \
	$(OBJ_DIR)/algorithm/executor.o \
	$(OBJ_DIR)/algorithm/cutter.o \
	$(OBJ_DIR)/algorithm/shm.o \
	$(OBJ_DIR)/algorithm/early_dispatch.o \
	$(OBJ_DIR)/algorithm/log.o \
	$(OBJ_DIR)/algorithm/swimlane.o | $(BIN_DIR)
	$(CC) $(CFLAGS) tests/test_step2_protocol.c \
		$(OBJ_DIR)/algorithm/dispatch.o \
		$(OBJ_DIR)/algorithm/executor.o \
		$(OBJ_DIR)/algorithm/cutter.o \
		$(OBJ_DIR)/algorithm/shm.o \
		$(OBJ_DIR)/algorithm/early_dispatch.o \
		$(OBJ_DIR)/algorithm/log.o \
		$(OBJ_DIR)/algorithm/swimlane.o -o $@ $(LDFLAGS)

# Step 3 send_task CAS 协议回归：需要 dispatch/executor 完整链路 + shm/ed 全局符号 + log。
# 该测试文件自带 g_state_buf stub（详见 tests/test_step3_next_block_idx.c），
# 因此这里不联 cutter.o，避免 g_state_buf 多重定义。
test-step3: $(BIN_DIR)/test_step3_next_block_idx
	$(BIN_DIR)/test_step3_next_block_idx

$(BIN_DIR)/test_step3_next_block_idx: tests/test_step3_next_block_idx.c \
	$(OBJ_DIR)/algorithm/dispatch.o \
	$(OBJ_DIR)/algorithm/executor.o \
	$(OBJ_DIR)/algorithm/shm.o \
	$(OBJ_DIR)/algorithm/early_dispatch.o \
	$(OBJ_DIR)/algorithm/log.o \
	$(OBJ_DIR)/algorithm/swimlane.o | $(BIN_DIR)
	$(CC) $(CFLAGS) tests/test_step3_next_block_idx.c \
		$(OBJ_DIR)/algorithm/dispatch.o \
		$(OBJ_DIR)/algorithm/executor.o \
		$(OBJ_DIR)/algorithm/shm.o \
		$(OBJ_DIR)/algorithm/early_dispatch.o \
		$(OBJ_DIR)/algorithm/log.o \
		$(OBJ_DIR)/algorithm/swimlane.o -o $@ $(LDFLAGS)

# Step 4 Hook 0 + late-arrival 线性化回归：
# 直接驱动 cutter::add_successors 与 propagate_dispatch_fanin，验证 fanin 贡献恰好一次。
test-step4: $(BIN_DIR)/test_step4_hook0_late_arrival
	$(BIN_DIR)/test_step4_hook0_late_arrival

$(BIN_DIR)/test_step4_hook0_late_arrival: tests/test_step4_hook0_late_arrival.c \
	$(OBJ_DIR)/algorithm/cutter.o \
	$(OBJ_DIR)/algorithm/shm.o \
	$(OBJ_DIR)/algorithm/early_dispatch.o \
	$(OBJ_DIR)/algorithm/log.o \
	$(OBJ_DIR)/algorithm/swimlane.o | $(BIN_DIR)
	$(CC) $(CFLAGS) tests/test_step4_hook0_late_arrival.c \
		$(OBJ_DIR)/algorithm/cutter.o \
		$(OBJ_DIR)/algorithm/shm.o \
		$(OBJ_DIR)/algorithm/early_dispatch.o \
		$(OBJ_DIR)/algorithm/log.o \
		$(OBJ_DIR)/algorithm/swimlane.o -o $@ $(LDFLAGS)

# Step 5 Hook 2 回归：resolve_dep 在 unfin 1->0 时切 DISPATCHED 并统一 push ready_queue。
test-step5: $(BIN_DIR)/test_step5_hook2_resolve_dep
	$(BIN_DIR)/test_step5_hook2_resolve_dep

$(BIN_DIR)/test_step5_hook2_resolve_dep: tests/test_step5_hook2_resolve_dep.c \
	$(OBJ_DIR)/algorithm/cutter.o \
	$(OBJ_DIR)/algorithm/shm.o \
	$(OBJ_DIR)/algorithm/early_dispatch.o \
	$(OBJ_DIR)/algorithm/log.o \
	$(OBJ_DIR)/algorithm/swimlane.o | $(BIN_DIR)
	$(CC) $(CFLAGS) tests/test_step5_hook2_resolve_dep.c \
		$(OBJ_DIR)/algorithm/cutter.o \
		$(OBJ_DIR)/algorithm/shm.o \
		$(OBJ_DIR)/algorithm/early_dispatch.o \
		$(OBJ_DIR)/algorithm/log.o \
		$(OBJ_DIR)/algorithm/swimlane.o -o $@ $(LDFLAGS)

# Step 6 Hook 1 + notify_once 回归：验证 stage->GATED、Hook2 释放、唯一 doorbell 写入。
test-step6: $(BIN_DIR)/test_step6_hook1_notify_once
	$(BIN_DIR)/test_step6_hook1_notify_once

$(BIN_DIR)/test_step6_hook1_notify_once: tests/test_step6_hook1_notify_once.c \
	$(OBJ_DIR)/algorithm/cutter.o \
	$(OBJ_DIR)/algorithm/dispatch.o \
	$(OBJ_DIR)/algorithm/executor.o \
	$(OBJ_DIR)/algorithm/shm.o \
	$(OBJ_DIR)/algorithm/early_dispatch.o \
	$(OBJ_DIR)/algorithm/log.o \
	$(OBJ_DIR)/algorithm/swimlane.o | $(BIN_DIR)
	$(CC) $(CFLAGS) tests/test_step6_hook1_notify_once.c \
		$(OBJ_DIR)/algorithm/cutter.o \
		$(OBJ_DIR)/algorithm/dispatch.o \
		$(OBJ_DIR)/algorithm/executor.o \
		$(OBJ_DIR)/algorithm/shm.o \
		$(OBJ_DIR)/algorithm/early_dispatch.o \
		$(OBJ_DIR)/algorithm/log.o \
		$(OBJ_DIR)/algorithm/swimlane.o -o $@ $(LDFLAGS)

# Step 7 race + ABA 回归：覆盖 Hook1/Hook2 两种通知先后，以及 ring 复用 stale notify 防护。
test-step7: $(BIN_DIR)/test_step7_notify_race_aba
	$(BIN_DIR)/test_step7_notify_race_aba

$(BIN_DIR)/test_step7_notify_race_aba: tests/test_step7_notify_race_aba.c \
	$(OBJ_DIR)/algorithm/dispatch.o \
	$(OBJ_DIR)/algorithm/executor.o \
	$(OBJ_DIR)/algorithm/shm.o \
	$(OBJ_DIR)/algorithm/early_dispatch.o \
	$(OBJ_DIR)/algorithm/log.o \
	$(OBJ_DIR)/algorithm/swimlane.o | $(BIN_DIR)
	$(CC) $(CFLAGS) tests/test_step7_notify_race_aba.c \
		$(OBJ_DIR)/algorithm/dispatch.o \
		$(OBJ_DIR)/algorithm/executor.o \
		$(OBJ_DIR)/algorithm/shm.o \
		$(OBJ_DIR)/algorithm/early_dispatch.o \
		$(OBJ_DIR)/algorithm/log.o \
		$(OBJ_DIR)/algorithm/swimlane.o -o $@ $(LDFLAGS)

# Step 9 门铃开闸协议回归：notify 只敲门铃，GATED->RUNNABLE 由 executor 侧完成。
test-step9: $(BIN_DIR)/test_step9_gate_open
	$(BIN_DIR)/test_step9_gate_open

$(BIN_DIR)/test_step9_gate_open: tests/test_step9_gate_open.c \
	$(OBJ_DIR)/algorithm/dispatch.o \
	$(OBJ_DIR)/algorithm/executor.o \
	$(OBJ_DIR)/algorithm/shm.o \
	$(OBJ_DIR)/algorithm/early_dispatch.o \
	$(OBJ_DIR)/algorithm/log.o \
	$(OBJ_DIR)/algorithm/swimlane.o | $(BIN_DIR)
	$(CC) $(CFLAGS) tests/test_step9_gate_open.c \
		$(OBJ_DIR)/algorithm/dispatch.o \
		$(OBJ_DIR)/algorithm/executor.o \
		$(OBJ_DIR)/algorithm/shm.o \
		$(OBJ_DIR)/algorithm/early_dispatch.o \
		$(OBJ_DIR)/algorithm/log.o \
		$(OBJ_DIR)/algorithm/swimlane.o -o $@ $(LDFLAGS)

# SPMD（count>1）整任务 ED 回归：需要 stage(ed) + skip(send_task) + 开闸执行(executor)
# + Hook2(cutter::resolve_dep) 四方联动，故链接全部算法对象。
#
# 必须递归调 $(MAKE) 并在命令行上传 ED_SPMD_MAX_BLOCKS：该旋钮默认是 1（不接 SPMD），
# 直接构建的话测试只会 SKIP，新功能就默认没人测。命令行变量会参与子 make 的
# parse 阶段配置指纹比对，从而正确触发重编，不会复用旋钮=1 的 .o。
test-ed-spmd:
	$(MAKE) ED_SPMD_MAX_BLOCKS=0xFFFF $(BIN_DIR)/test_ed_spmd_multiblock
	$(BIN_DIR)/test_ed_spmd_multiblock

$(BIN_DIR)/test_ed_spmd_multiblock: tests/test_ed_spmd_multiblock.c \
	$(OBJ_DIR)/algorithm/cutter.o \
	$(OBJ_DIR)/algorithm/dispatch.o \
	$(OBJ_DIR)/algorithm/executor.o \
	$(OBJ_DIR)/algorithm/shm.o \
	$(OBJ_DIR)/algorithm/early_dispatch.o \
	$(OBJ_DIR)/algorithm/log.o \
	$(OBJ_DIR)/algorithm/swimlane.o | $(BIN_DIR)
	$(CC) $(CFLAGS) tests/test_ed_spmd_multiblock.c \
		$(OBJ_DIR)/algorithm/cutter.o \
		$(OBJ_DIR)/algorithm/dispatch.o \
		$(OBJ_DIR)/algorithm/executor.o \
		$(OBJ_DIR)/algorithm/shm.o \
		$(OBJ_DIR)/algorithm/early_dispatch.o \
		$(OBJ_DIR)/algorithm/log.o \
		$(OBJ_DIR)/algorithm/swimlane.o -o $@ $(LDFLAGS)

# cutter 提交窗口右开区间回归：只需 cutter/shm/log + ED（ED 提供未来 stager 相关符号占位，
# cutter.c 自身当前未直接引用，但链接进来对结果无影响并保持与其他 test-* 目标一致）。
test-cutter-window: $(BIN_DIR)/test_cutter_commit_window
	$(BIN_DIR)/test_cutter_commit_window

$(BIN_DIR)/test_cutter_commit_window: tests/test_cutter_commit_window.c \
	$(OBJ_DIR)/algorithm/cutter.o \
	$(OBJ_DIR)/algorithm/shm.o \
	$(OBJ_DIR)/algorithm/early_dispatch.o \
	$(OBJ_DIR)/algorithm/log.o \
	$(OBJ_DIR)/algorithm/swimlane.o | $(BIN_DIR)
	$(CC) $(CFLAGS) tests/test_cutter_commit_window.c \
		$(OBJ_DIR)/algorithm/cutter.o \
		$(OBJ_DIR)/algorithm/shm.o \
		$(OBJ_DIR)/algorithm/early_dispatch.o \
		$(OBJ_DIR)/algorithm/log.o \
		$(OBJ_DIR)/algorithm/swimlane.o -o $@ $(LDFLAGS)

clean:
	rm -rf build $(BIN_DIR)

-include $(DEPS)
