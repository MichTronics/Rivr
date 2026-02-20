# RIVR Node – ESP-IDF main component
#
# Sources span three directories: firmware_core/, rivr_layer/, and this (main/).
# All include dirs are anchored at the project root so headers are reached as
# e.g. #include "firmware_core/timebase.h".
#
# Rust static library: build manually before CMake (see project README):
#   cd ../../rivr_core && cargo build --features ffi [--release]
# Then copy / symlink librivr_core.a into this build directory.

idf_component_register(
    SRCS
        # Firmware hardware layer
        "../firmware_core/main.c"
        "../firmware_core/platform_esp32.c"
        "../firmware_core/radio_sx1262.c"
        "../firmware_core/timebase.c"
        "../firmware_core/dutycycle.c"
        "../firmware_core/protocol.c"
        "../firmware_core/routing.c"
        # RIVR glue layer
        "../rivr_layer/rivr_embed.c"
        "../rivr_layer/rivr_sinks.c"
        "../rivr_layer/rivr_sources.c"

    INCLUDE_DIRS
        ".."        # project root – enables #include "firmware_core/xxx.h"

    REQUIRES
        esp_driver_spi
        esp_driver_gpio
        esp_driver_uart
        esp_timer
        freertos
        log
        spi_flash
)

# ── Link the pre-built Rust rivr_core static library ─────────────────────────
# ESP-IDF CMake passes extra link libraries via target_link_libraries after the
# component is registered.
#
# We look in two locations:
#   1. PROJECT_DIR/rivr_core/target/debug/librivr_core.a  (debug build)
#   2. PROJECT_DIR/rivr_core/target/release/librivr_core.a (release build)
set(RIVR_CORE_LIB_DEBUG   "${CMAKE_SOURCE_DIR}/rivr_core/target/debug/librivr_core.a")
set(RIVR_CORE_LIB_RELEASE "${CMAKE_SOURCE_DIR}/rivr_core/target/release/librivr_core.a")

if(EXISTS "${RIVR_CORE_LIB_RELEASE}")
    set(RIVR_CORE_LIB "${RIVR_CORE_LIB_RELEASE}")
elseif(EXISTS "${RIVR_CORE_LIB_DEBUG}")
    set(RIVR_CORE_LIB "${RIVR_CORE_LIB_DEBUG}")
else()
    message(FATAL_ERROR
        "librivr_core.a not found. Build it first:\n"
        "  cd rivr_core && cargo build --features ffi [--release]\n"
        "Expected path: ${RIVR_CORE_LIB_RELEASE}")
endif()

message(STATUS "Using rivr_core static library: ${RIVR_CORE_LIB}")

# Attach to the main component target (set via idf_component_get_property)
idf_component_get_property(main_lib __idf_main COMPONENT_LIB)
target_link_libraries(${main_lib} INTERFACE "${RIVR_CORE_LIB}")
