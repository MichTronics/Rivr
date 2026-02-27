# tests/Makefile — build and run RIVR C-layer test suites (Linux / macOS)
#
# Usage (from project root):
#   make -C tests            # build + run both suites
#   make -C tests acceptance # acceptance suite only
#   make -C tests recovery   # radio-recovery suite only
#   make -C tests clean      # remove binaries
#
# Compiler search order: CC env var → gcc → cc

CC    ?= gcc
ROOT  := ..
FW    := $(ROOT)/firmware_core

# ── Common flags ──────────────────────────────────────────────────────────────
# -I$(ROOT)       :  top-level includes (e.g. rivr_layer/…)
# -I$(FW)         :  firmware headers without "firmware_core/" prefix
# -Iinclude       :  host stubs for esp_log.h, driver/gpio.h, etc.
COMMON_FLAGS := -std=c11 -Wall -Wextra -Wno-unused-parameter -g \
                -I$(ROOT) -I$(FW) -Iinclude

# ─────────────────────────────────────────────────────────────────────────────
# ACCEPTANCE TEST
#   Tests protocol / routing / pending_queue / route_cache logic.
#   Brings in rivr_log.c and rivr_metrics.c so newly added metric wiring in
#   pending_queue.c and route_cache.c links cleanly.
# ─────────────────────────────────────────────────────────────────────────────
ACC_SRCS := \
    $(FW)/protocol.c    \
    $(FW)/routing.c     \
    $(FW)/route_cache.c \
    $(FW)/pending_queue.c \
    $(FW)/rivr_metrics.c  \
    $(FW)/rivr_log.c      \
    $(FW)/airtime_sched.c \
    test_stubs.c           \
    test_acceptance.c

ACC_BIN  := acceptance_test
ACC_FLAGS := $(COMMON_FLAGS)

# ─────────────────────────────────────────────────────────────────────────────
# RADIO RECOVERY TEST
#   Tests SX1262 fault-injection / recovery paths compiled for host.
#   -DRIVR_FAULT_INJECT=1 enables the g_fault_* flags and radio_fault_reset_state().
# ─────────────────────────────────────────────────────────────────────────────
REC_SRCS := \
    $(FW)/radio_sx1262.c  \
    $(FW)/rivr_metrics.c  \
    $(FW)/rivr_log.c      \
    test_radio_recovery.c

REC_BIN   := radio_recovery_test
REC_FLAGS := $(COMMON_FLAGS) -DRIVR_FAULT_INJECT=1

# ─────────────────────────────────────────────────────────────────────────────
.PHONY: all acceptance recovery clean

all: acceptance recovery

acceptance: $(ACC_BIN)
	@echo "── Running acceptance tests ──────────────────────────────────────"
	./$(ACC_BIN)
	@echo

recovery: $(REC_BIN)
	@echo "── Running radio recovery tests ──────────────────────────────────"
	./$(REC_BIN)
	@echo

$(ACC_BIN): $(ACC_SRCS)
	$(CC) $(ACC_FLAGS) $^ -o $@

$(REC_BIN): $(REC_SRCS)
	$(CC) $(REC_FLAGS) $^ -o $@

clean:
	rm -f $(ACC_BIN) $(REC_BIN)
