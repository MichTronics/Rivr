# RIVR Node – firmware_core ESP-IDF component
#
# This is the real build component for both PlatformIO and native idf.py builds.
# Sources are in firmware_core/ (hardware layer) and rivr_layer/ (RIVR glue).
# Include dirs are anchored at the project root: #include "firmware_core/xxx.h"

# Select radio driver at CMake configure time.
# Pass  -DRIVR_RADIO_SX1276=1  via board_build.cmake_extra_args in platformio.ini
# to compile radio_sx1276.c instead of the default radio_sx1262.c.
if(RIVR_RADIO_SX1276)
    set(_RADIO_SRC "radio_sx1276.c")
    message(STATUS "RIVR: using SX1276 driver (radio_sx1276.c)")
else()
    set(_RADIO_SRC "radio_sx1262.c")
    message(STATUS "RIVR: using SX1262 driver (radio_sx1262.c)")
endif()

idf_component_register(
    SRCS
        "main.c"
        "platform_esp32.c"
        "display/display.c"
        ${_RADIO_SRC}
        "timebase.c"
        "dutycycle.c"
        "airtime_sched.c"
        "protocol.c"
        "routing.c"
        "route_cache.c"
        "pending_queue.c"
        "rivr_fabric.c"
        "rivr_metrics.c"
        "rivr_log.c"
        "retry_table.c"
        "build_info.c"
        # P2: signed OTA + policy engine
        "ed25519_verify.c"
        "rivr_ota_core.c"      # platform-agnostic logic
        "rivr_ota_platform.c" # ESP-IDF NVS backend (never linked in host tests)
        "policy.c"
        # HAL abstraction layer (additive — does not change existing driver code)
        "hal/radio_if.c"
        # Watchdog / panic marker
        "rivr_panic.c"
        # Runtime-adjustable policy parameters
        "rivr_policy.c"
        # @PARAMS HMAC-SHA-256 signing support
        "crypto/hmac_sha256.c"
        # RIVR glue layer (relative to firmware_core/)
        "../rivr_layer/rivr_embed.c"
        "../rivr_layer/rivr_sinks.c"
        "../rivr_layer/rivr_sources.c"
        "../rivr_layer/rivr_cli.c"

    INCLUDE_DIRS
        ".."    # project root – enables #include "firmware_core/xxx.h"

    REQUIRES
        esp_driver_spi
        esp_driver_gpio
        esp_driver_uart
        esp_driver_i2c
        esp_timer
        freertos
        log
        spi_flash
        nvs_flash  # P2: signed OTA anti-replay via NVS
        esp_system  # rivr_panic: esp_reset_reason(), RTC_DATA_ATTR
)

# ── Link the pre-built Rust rivr_core static library ─────────────────────────
# After idf_component_register(), the __idf_firmware_core target exists and we
# can link the pre-built Rust .a directly to it.
# Search order: workspace cargo target/ tree first, rivr_core/target/ fallback.
set(_RIVR_ROOTS
    "${CMAKE_SOURCE_DIR}/target/xtensa-esp32-espidf/release"
    "${CMAKE_SOURCE_DIR}/target/xtensa-esp32-espidf/debug"
    "${CMAKE_SOURCE_DIR}/rivr_core/target/release"
    "${CMAKE_SOURCE_DIR}/rivr_core/target/debug"
)
foreach(_root IN LISTS _RIVR_ROOTS)
    if(EXISTS "${_root}/librivr_core.a")
        set(RIVR_CORE_LIB "${_root}/librivr_core.a")
        break()
    endif()
endforeach()

if(NOT DEFINED RIVR_CORE_LIB)
    message(FATAL_ERROR
        "librivr_core.a not found. Build it first: "
        "cargo +esp build -p rivr_core "
        "--target xtensa-esp32-espidf --features ffi [--release]")
endif()

message(STATUS "Using rivr_core static library: ${RIVR_CORE_LIB}")

# Use idf_component_get_property to get the CMake library target for THIS
# component (firmware_core), then attach the Rust .a to it.
idf_component_get_property(firmware_core_lib ${COMPONENT_NAME} COMPONENT_LIB)
target_link_libraries(${firmware_core_lib} INTERFACE "${RIVR_CORE_LIB}")
