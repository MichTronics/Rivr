# =============================================================================
# variants/rpi_e22_900/Makefile — RIVR node for Raspberry Pi + E22-900M30S
# =============================================================================
#
# Must be invoked from the project root:
#   make -C variants/rpi_e22_900                  # client (default)
#   make -C variants/rpi_e22_900 ROLE=repeater    # repeater
#   make -C variants/rpi_e22_900 CALLSIGN=N0CALL  # with custom callsign
#   make -C variants/rpi_e22_900 clean
#
# Prerequisites:
#   sudo apt install gcc libgpiod-dev
#   # Enable SPI in /boot/firmware/config.txt:
#   #   dtparam=spi=on
#   # Then reboot.
#
# Run (gpio/spi group membership or sudo required):
#   sudo ./rivr          # client
#   sudo ./rivr-repeater # repeater
#
# Wiring (BCM numbers):
#   NSS=8   BUSY=24  NRST=17  DIO1=25  RXEN=23  TXEN=22
#   MISO=9  MOSI=10  SCK=11   → /dev/spidev0.0
#
# RF / LoRa profile and build identity are defined in:
#   variants/rpi_e22_900/config.h

# All paths are relative to the project root (enforced by -C invocation).
ROOT := ../..

CC       := gcc

# ── Build role ────────────────────────────────────────────────────────────────
# Override on the command line:
#   make -f Makefile.linux ROLE=repeater   → relay node (no chat CLI)
#   make -f Makefile.linux ROLE=client     → client node (default, with chat CLI)
ROLE ?= client

# Each role gets its own object directory so flags are never reused across roles.
OBJDIR   := $(ROOT)/build/linux/$(ROLE)

ifeq ($(ROLE),repeater)
  TARGET := rivr-repeater
  _ROLE_DEFS  := -DRIVR_ROLE_CLIENT=0
  _ROLE_DEFS += -DRIVR_FABRIC_REPEATER=1
  _ROLE_DEFS += -DRIVR_BUILD_REPEATER=1
  _ROLE_ENV   := linux_rpi_e22_900_repeater
else
  TARGET := rivr
  _ROLE_DEFS  := -DRIVR_ROLE_CLIENT=1
  _ROLE_DEFS += -DRIVR_FABRIC_REPEATER=0
  _ROLE_DEFS += -DRIVR_BUILD_REPEATER=0
  _ROLE_ENV   := linux_rpi_e22_900
endif

# ── Feature & platform flags ──────────────────────────────────────────────────
DEFS  := -DRIVR_PLATFORM_LINUX=1
DEFS += -DRIVR_SIM_MODE=0
DEFS += $(_ROLE_DEFS)
DEFS += -DRIVR_FEATURE_BLE=0
DEFS += -DRIVR_FEATURE_USB_BRIDGE=0
DEFS += -DRIVR_FEATURE_DISPLAY=0
DEFS += -DFEATURE_DISPLAY=0
DEFS += -DRIVR_RFSWITCH_ENABLE=1
DEFS += -DRIVR_LOG_LEVEL=2

# Build identity (radio params come from config.h via -include below)
DEFS += -DRIVR_BUILD_ENV=\"$(_ROLE_ENV)\"

# Node callsign — override on the command line: make -C variants/rpi_e22_900 CALLSIGN=N0CALL
ifdef CALLSIGN
DEFS += -DRIVR_CALLSIGN=\"$(CALLSIGN)\"
endif

# ── Include paths ─────────────────────────────────────────────────────────────
# All -I paths are relative to the project root (Makefile is invoked with -C).
INCS  := -I $(ROOT)
INCS += -I $(ROOT)/firmware_core
INCS += -I $(ROOT)/rivr_layer
INCS += -I $(ROOT)/firmware_core/linux/include
INCS += -include $(ROOT)/firmware_core/platform_linux.h
INCS += -include $(ROOT)/variants/rpi_e22_900/config.h

# ── Compiler flags ────────────────────────────────────────────────────────────
CFLAGS  := -std=gnu11 -Wall -Wextra -O2
CFLAGS += -Wno-unused-parameter -Wno-unused-function -Wno-unused-variable
CFLAGS += $(DEFS) $(INCS)

# ── Source files (paths relative to project root) ────────────────────────────
# Linux-specific platform / radio / timing / panic
SRCS  := firmware_core/main_linux.c
SRCS += firmware_core/platform_linux.c
SRCS += firmware_core/radio_sx1262_linux.c
SRCS += firmware_core/timebase_linux.c
SRCS += firmware_core/rivr_panic_linux.c

# Core protocol & routing
SRCS += firmware_core/routing.c
SRCS += firmware_core/protocol.c
SRCS += firmware_core/dutycycle.c
SRCS += firmware_core/neighbor_table.c
SRCS += firmware_core/route_cache.c
SRCS += firmware_core/retry_table.c
SRCS += firmware_core/pending_queue.c

# Fabric, metrics, logging
SRCS += firmware_core/rivr_fabric.c
SRCS += firmware_core/rivr_metrics.c
SRCS += firmware_core/rivr_log.c

# Scheduling & timing
SRCS += firmware_core/airtime_sched.c
SRCS += firmware_core/beacon_sched.c

# Policy engine
SRCS += firmware_core/policy.c
SRCS += firmware_core/rivr_policy.c

# Build info & crypto
SRCS += firmware_core/build_info.c
SRCS += firmware_core/ed25519_verify.c
SRCS += firmware_core/crypto/hmac_sha256.c

# OTA (core logic + NVS-backed platform — NVS stub returns NOT_FOUND → default program)
SRCS += firmware_core/rivr_ota_core.c
SRCS += firmware_core/rivr_ota_platform.c

# Radio HAL vtable
SRCS += firmware_core/hal/radio_if.c

# BLE (compile-guards itself to empty stubs when RIVR_FEATURE_BLE=0)
SRCS += firmware_core/ble/rivr_ble.c
SRCS += firmware_core/ble/rivr_ble_service.c
SRCS += firmware_core/ble/rivr_ble_companion.c

# Display (compile-guards itself to empty stubs when FEATURE_DISPLAY=0)
SRCS += firmware_core/display/display.c

# Multi-transport packet bus
SRCS += firmware_core/rivr_bus/rivr_bus.c
SRCS += firmware_core/rivr_bus/rivr_dup_cache.c
SRCS += firmware_core/rivr_bus/rivr_dispatch.c

# Transport interface adapters
SRCS += firmware_core/iface/rivr_iface_lora.c
SRCS += firmware_core/iface/rivr_iface_ble.c
SRCS += firmware_core/iface/rivr_iface_usb.c

# RIVR glue layer
SRCS += rivr_layer/rivr_embed.c
SRCS += rivr_layer/rivr_sinks.c
SRCS += rivr_layer/rivr_sources.c
SRCS += rivr_layer/rivr_svc.c
SRCS += rivr_layer/rivr_cli.c

# ── Rust core library ─────────────────────────────────────────────────────────
RIVR_CORE_LIB := $(ROOT)/target/release/librivr_core.a

# ── Derived object list ───────────────────────────────────────────────────────
# Sources are relative to ROOT; objects mirror that tree under OBJDIR.
OBJS := $(patsubst %.c,$(OBJDIR)/%.o,$(SRCS))

# ── System libraries ──────────────────────────────────────────────────────────
LDLIBS := $(RIVR_CORE_LIB) -lgpiod -lpthread -lm

# ── Output binary location (project root, next to rivr) ──────────────────────
TARGET_BIN := $(ROOT)/$(TARGET)

# ── Default target ────────────────────────────────────────────────────────────
.PHONY: all clean rivr_core

all: rivr_core $(TARGET_BIN)

$(TARGET_BIN): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)
	@echo "Build complete: $@"

# ── Build Rust core library ───────────────────────────────────────────────────
rivr_core:
	cargo build --manifest-path $(ROOT)/rivr_core/Cargo.toml --release --features ffi

# ── Compile C files (sources relative to ROOT) ────────────────────────────────
$(OBJDIR)/%.o: $(ROOT)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

# ── Clean ─────────────────────────────────────────────────────────────────────
clean:
	rm -rf $(OBJDIR) $(TARGET_BIN)
	cargo clean --manifest-path $(ROOT)/rivr_core/Cargo.toml
