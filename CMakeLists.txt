cmake_minimum_required(VERSION 3.20)

# ─────────────────────────────────────────────────────────────────────────────
# RIVR Node — ESP-IDF top-level CMakeLists
#
# BUILD INSTRUCTIONS
# ──────────────────
# 1. Install ESP-IDF 5.x and run `. $IDF_PATH/export.sh` (Linux/Mac) or
#    the equivalent export.bat (Windows).
# 2. Build the Rust static library first:
#      cargo +esp build -p rivr_core \
#        --target xtensa-esp32-espidf --features ffi          # debug
#      cargo +esp build -p rivr_core \
#        --target xtensa-esp32-espidf --features ffi --release  # release (for flash)
# 3. Configure and build firmware:
#      idf.py set-target esp32
#      idf.py build
# 4. Flash and monitor:
#      idf.py -p COM3 flash monitor        # Windows
#      idf.py -p /dev/ttyUSB0 flash monitor  # Linux
#
# SIMULATION (no real SX1262 hardware needed):
#   Add -DRIVR_SIM_MODE=1 to IDF_CMAKE_ADD_COMPILE_FLAGS, or use PlatformIO:
#      pio run -e esp32_sim
# ─────────────────────────────────────────────────────────────────────────────

# firmware_core/ is the real component (all SRCS, REQUIRES live there).
# main/ is an empty stub kept for idf.py compatibility.
list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_SOURCE_DIR}/firmware_core")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(rivr_node)
