# RIVR Node – ESP-IDF main component (stub)
#
# All actual sources and REQUIRES live in firmware_core/CMakeLists.txt.
# This stub exists so that idf.py builds (which always expect a 'main'
# component) continue to work.  PlatformIO uses firmware_core/ as its
# primary component (src_dir = firmware_core in platformio.ini).

idf_component_register()
