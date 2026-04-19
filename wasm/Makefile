# wasm/Makefile — Build rivr_routing.wasm via Emscripten
#
# Prerequisites:
#   - Emscripten SDK installed and activated (emcc on PATH)
#     https://emscripten.org/docs/getting_started/downloads.html
#
# Usage (from project root):
#   make -C wasm              # build WASM + JS glue
#   make -C wasm clean        # remove build artefacts
#   make -C wasm copy         # build + copy output to rivr_website/public/wasm/
#
# Output:
#   wasm/out/rivr_routing.wasm   — the WASM binary
#   wasm/out/rivr_routing.js     — Emscripten JS glue (ES module)

CC    := emcc
ROOT  := ..
FW    := $(ROOT)/firmware_core
OUT   := out
SITE  := $(ROOT)/rivr_website/public/wasm

SRCS :=                        \
    $(FW)/routing.c            \
    $(FW)/route_cache.c        \
    $(FW)/neighbor_table.c     \
    $(FW)/protocol.c           \
    $(FW)/rivr_metrics.c       \
    $(FW)/rivr_log.c           \
    $(FW)/airtime_sched.c      \
    wasm_stubs.c               \
    rivr_routing_wasm.c

CFLAGS :=                      \
    -std=c11                   \
    -O2                        \
    -I$(ROOT)                  \
    -I$(FW)                    \
    -I$(ROOT)/tests/include    \
    -DRIVR_SIM_MODE=1          \
    -DRIVR_ROLE_REPEATER=1     \
    -DRIVR_FEATURE_RELAY=1     \
    -DRIVR_LOG_LEVEL=5         \
    -Wno-unused-parameter

# Exported C functions (must match EMSCRIPTEN_KEEPALIVE list in rivr_routing_wasm.c)
EXPORTS := \
    _rivr_wasm_init              \
    _rivr_wasm_node_init         \
    _rivr_wasm_flood_forward     \
    _rivr_wasm_get_ttl           \
    _rivr_wasm_get_hop           \
    _rivr_wasm_get_flags         \
    _rivr_wasm_get_drop_counts   \
    _rivr_wasm_jitter_ms         \
    _rivr_wasm_protocol_version  \
    _rivr_wasm_default_ttl       \
    _rivr_wasm_max_nodes

EXPORTED_FUNCTIONS := $(shell echo '$(EXPORTS)' | tr ' ' ',')

LDFLAGS :=                                              \
    -s WASM=1                                           \
    -s MODULARIZE=1                                     \
    -s EXPORT_NAME="createRivrModule"                   \
    -s EXPORTED_FUNCTIONS="[$(EXPORTED_FUNCTIONS)]"     \
    -s EXPORTED_RUNTIME_METHODS="['ccall','cwrap']"     \
    -s ALLOW_MEMORY_GROWTH=1                            \
    -s INITIAL_MEMORY=2097152                           \
    -s ENVIRONMENT=web                                  \
    --no-entry

TARGET := $(OUT)/rivr_routing.js

.PHONY: all clean copy

all: $(TARGET)

$(OUT):
	mkdir -p $(OUT)

$(TARGET): $(SRCS) | $(OUT)
	$(CC) $(CFLAGS) $(SRCS) $(LDFLAGS) -o $@
	@echo "✓ WASM build complete: $(OUT)/rivr_routing.{js,wasm}"

copy: all
	mkdir -p $(SITE)
	cp $(OUT)/rivr_routing.js   $(SITE)/
	cp $(OUT)/rivr_routing.wasm $(SITE)/
	@echo "✓ Copied to $(SITE)/"

clean:
	rm -rf $(OUT)
