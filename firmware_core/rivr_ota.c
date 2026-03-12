/*
 * rivr_ota.c — LEGACY FILE: no longer compiled.
 *
 * The OTA implementation has been split into two translation units:
 *
 *   firmware_core/rivr_ota_core.c      — pure logic, host-safe, no ESP-IDF
 *   firmware_core/rivr_ota_platform.c  — ESP-IDF NVS backend
 *
 * Update firmware_core/CMakeLists.txt references both new files.
 * Host unit tests (tests/test_ota.c) link only rivr_ota_core.c and provide
 * platform-interface stubs directly.
 *
 * See firmware_core/rivr_ota_platform.h for the platform interface design.
 *
 * This file is intentionally left as a comment-only marker so that any
 * out-of-tree branch that still references it gets a clear error message
 * rather than a silent ODR violation.
 */

/*
 * This file is intentionally empty of compilable code.
 * Attempting to add it to a build will produce duplicate-symbol linker errors
 * against rivr_ota_core.c — which is the intended safety net.
 *
 * If you are porting an older branch: delete this file and add
 * rivr_ota_core.c + rivr_ota_platform.c to your CMakeLists instead.
 */
