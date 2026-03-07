# tests/Makefile — build and run RIVR C-layer test suites (Linux / macOS)
#
# Usage (from project root):
#   make -C tests            # build + run all suites  (alias: make -C tests test)
#   make -C tests acceptance # acceptance suite only
#   make -C tests recovery   # radio-recovery suite only
#   make -C tests replay     # deterministic replay/sim suite only
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
# REPLAY / SIM TEST  (Step 7)
#   Deterministic JSONL-driven harness that feeds events into the
#   routing / queue / airtime C-layer and verifies outcomes via assertions.
#   Includes fault injection (crc_fail, busy_stuck, tx_timeout, queue_fill).
# ─────────────────────────────────────────────────────────────────────────────
REP_SRCS := \
    $(FW)/protocol.c    \
    $(FW)/routing.c     \
    $(FW)/route_cache.c \
    $(FW)/rivr_metrics.c  \
    $(FW)/rivr_log.c      \
    $(FW)/airtime_sched.c \
    test_stubs.c           \
    replay.c               \
    test_replay.c

REP_BIN   := replay_test
REP_FLAGS := $(COMMON_FLAGS)

# ─────────────────────────────────────────────────────────────────────────────
# DUTY CYCLE TEST
#   Tests the sliding-window limiter: normal operation, budget gate,
#   window expiry, buffer-overflow LRU eviction, and full recovery.
# ─────────────────────────────────────────────────────────────────────────────
DC_SRCS := \
    $(FW)/dutycycle.c     \
    $(FW)/rivr_metrics.c  \
    $(FW)/rivr_log.c      \
    test_dutycycle.c

DC_BIN   := dutycycle_test
DC_FLAGS := $(COMMON_FLAGS)

# ─────────────────────────────────────────────────────────────────────────────
# POLICY ENGINE TEST (P2)
#   Tests policy_init, policy_classify, policy_check (TTL cap + token bucket).
# ─────────────────────────────────────────────────────────────────────────────
POL_SRCS := \
    $(FW)/policy.c \
    test_policy.c

POL_BIN   := policy_test
POL_FLAGS := $(COMMON_FLAGS)

# ─────────────────────────────────────────────────────────────────────────────
# OTA GATE TEST (P2)
#   Tests rivr_ota_verify + rivr_ota_activate with RIVR_SIGNED_PROG enabled.
#   Ed25519 verify compiled in directly (no shared-object dep).
# ─────────────────────────────────────────────────────────────────────────────
OTA_SRCS := \
    $(FW)/ed25519_verify.c \
    $(FW)/rivr_ota_core.c \
    test_ota.c
# Note: rivr_ota_platform.c is NOT linked here.
# test_ota.c provides host stubs for the ota_platform_* and ota_storage_* interface.

OTA_BIN   := ota_test
OTA_FLAGS := $(COMMON_FLAGS) -DRIVR_SIGNED_PROG

# ─────────────────────────────────────────────────────────────────────────────
# RETRY / ACK RELIABILITY TEST
#   Tests retry_table: enqueue, timeout-driven retransmit, ACK clearing,
#   exhaustion / fallback flood, table-full rejection, routing_build_ack.
# ─────────────────────────────────────────────────────────────────────────────
RETRY_SRCS := \
    $(FW)/protocol.c      \
    $(FW)/routing.c       \
    $(FW)/route_cache.c   \
    $(FW)/rivr_metrics.c  \
    $(FW)/rivr_log.c      \
    $(FW)/retry_table.c   \
    test_stubs.c           \
    test_retry.c

RETRY_BIN   := retry_test
RETRY_FLAGS := $(COMMON_FLAGS)

# ─────────────────────────────────────────────────────────────────────────────
# ASAN / UBSAN (P3.2)
#   Rebuilds the four main suites + P2 suites with AddressSanitizer and
#   UndefinedBehaviourSanitizer.  Binaries are suffixed _asan so they do
#   not overwrite the normal builds.
# ─────────────────────────────────────────────────────────────────────────────
ASAN_FLAGS := $(COMMON_FLAGS) -O1 -g -fsanitize=address,undefined \
              -fno-omit-frame-pointer

# ─────────────────────────────────────────────────────────────────────────────
.PHONY: all test acceptance recovery replay dutycycle policy ota retry asan fuzz_harness clean

all: acceptance recovery replay dutycycle policy ota retry

# `make test` is the CI-friendly single-command alias.
test: all

acceptance: $(ACC_BIN)
	@echo "── Running acceptance tests ──────────────────────────────────────"
	./$(ACC_BIN)
	@echo

recovery: $(REC_BIN)
	@echo "── Running radio recovery tests ──────────────────────────────────"
	./$(REC_BIN)
	@echo

replay: $(REP_BIN)
	@echo "── Running replay/sim tests ──────────────────────────────────────"
	./$(REP_BIN)
	@echo

dutycycle: $(DC_BIN)
	@echo "── Running duty-cycle tests ─────────────────────────────────────"
	./$(DC_BIN)
	@echo

$(ACC_BIN): $(ACC_SRCS)
	$(CC) $(ACC_FLAGS) $^ -o $@

$(REC_BIN): $(REC_SRCS)
	$(CC) $(REC_FLAGS) $^ -o $@

$(REP_BIN): $(REP_SRCS)
	$(CC) $(REP_FLAGS) $^ -o $@

$(DC_BIN): $(DC_SRCS)
	$(CC) $(DC_FLAGS) $^ -o $@


policy: $(POL_BIN)
	@echo "── Running policy-engine tests (P2) ─────────────────────────────"
	./$(POL_BIN)
	@echo

ota: $(OTA_BIN)
	@echo "── Running OTA gate tests (P2, signed) ──────────────────────────"
	./$(OTA_BIN)
	@echo
retry: $(RETRY_BIN)
	@echo "── Running ACK/retry reliability tests ─────────────────────"
	./$(RETRY_BIN)
	@echo
asan:
	@echo "── ASan/UBSan: acceptance ────────────────────────────────────────"
	$(CC) $(ASAN_FLAGS) $(ACC_SRCS) -o acceptance_asan && ./acceptance_asan
	@echo
	@echo "── ASan/UBSan: radio-recovery ────────────────────────────────────"
	$(CC) $(ASAN_FLAGS) -DRIVR_FAULT_INJECT=1 $(REC_SRCS) -o recovery_asan && ./recovery_asan
	@echo
	@echo "── ASan/UBSan: replay ────────────────────────────────────────────"
	$(CC) $(ASAN_FLAGS) $(REP_SRCS) -o replay_asan && ./replay_asan
	@echo
	@echo "── ASan/UBSan: duty-cycle ────────────────────────────────────────"
	$(CC) $(ASAN_FLAGS) $(DC_SRCS) -o dutycycle_asan && ./dutycycle_asan
	@echo
	@echo "── ASan/UBSan: policy ────────────────────────────────────────────"
	$(CC) $(ASAN_FLAGS) $(POL_SRCS) -o policy_asan && ./policy_asan
	@echo
	@echo "── ASan/UBSan: OTA (signed) ──────────────────────────────────────"
	$(CC) $(ASAN_FLAGS) -DRIVR_SIGNED_PROG $(OTA_SRCS) -o ota_asan && timeout 60 ./ota_asan
	@echo
	@echo "── ASan/UBSan: retry ─────────────────────────────────────────────"
	$(CC) $(ASAN_FLAGS) $(RETRY_SRCS) -o retry_asan && ./retry_asan
	@echo
$(POL_BIN): $(POL_SRCS)
	$(CC) $(POL_FLAGS) $^ -o $@

$(OTA_BIN): $(OTA_SRCS)
	$(CC) $(OTA_FLAGS) $^ -o $@

$(RETRY_BIN): $(RETRY_SRCS)
	$(CC) $(RETRY_FLAGS) $^ -o $@

# ─────────────────────────────────────────────────────────────────────────────
# FUZZ HARNESS (P3.2)
#   Builds fuzz_ffi_harness with ASan for manual AFL++/libFuzzer runs.
#   Sources mirror the acceptance suite (protocol + routing + deps).
# ─────────────────────────────────────────────────────────────────────────────
FUZZ_SRCS := \
    $(FW)/protocol.c      \
    $(FW)/routing.c       \
    $(FW)/route_cache.c   \
    $(FW)/rivr_metrics.c  \
    $(FW)/rivr_log.c      \
    $(FW)/airtime_sched.c \
    test_stubs.c           \
    fuzz_ffi_harness.c

FUZZ_BIN  := fuzz_ffi_harness
FUZZ_FLAGS := $(ASAN_FLAGS)

fuzz_harness: $(FUZZ_BIN)
	@echo "fuzz_ffi_harness built — run: echo '' | ./$(FUZZ_BIN)"

$(FUZZ_BIN): $(FUZZ_SRCS)
	$(CC) $(FUZZ_FLAGS) $^ -o $@

clean:
	rm -f $(ACC_BIN) $(REC_BIN) $(REP_BIN) $(DC_BIN) $(POL_BIN) $(OTA_BIN) $(RETRY_BIN) $(FUZZ_BIN) acceptance_asan recovery_asan replay_asan dutycycle_asan policy_asan ota_asan retry_asan
