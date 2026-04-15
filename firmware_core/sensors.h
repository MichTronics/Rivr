/**
 * @file  sensors.h
 * @brief Sensor subsystem — DS18B20 + AM2302 integration into the RIVR main loop.
 *
 * Calls sensors_init() once at startup and sensors_tick() every loop iteration.
 * Readings are automatically encoded as PKT_TELEMETRY frames and pushed to the
 * LoRa TX queue at RIVR_SENSOR_TX_MS intervals (default 60 s).
 *
 * Sensor IDs:
 *   1 — DS18B20 temperature       (UNIT_CELSIUS,    sensor 0x0001)
 *   2 — AM2302 relative humidity  (UNIT_PERCENT_RH, sensor 0x0002)
 *   3 — AM2302 temperature        (UNIT_CELSIUS,    sensor 0x0003)
 *   4 — Battery voltage           (UNIT_MILLIVOLTS, sensor 0x0004)
 *
 * Both sensors compile to no-ops when their respective feature flags are
 * disabled (RIVR_FEATURE_DS18B20=0 and/or RIVR_FEATURE_AM2302=0).
 */

#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialise all enabled sensors.
 *
 * Configures GPIO pins and prepares internal state.
 * Must be called once, after the hardware GPIO driver is ready.
 */
void sensors_init(void);

/**
 * @brief Drive sensor read/transmit state machines.
 *
 * Call from the main loop on every tick (~10 ms).  Non-blocking except for a
 * brief AM2302 data-read burst (~5 ms) that occurs at most once per TX interval.
 *
 * @param now_ms   Current monotonic time in milliseconds.
 * @param node_id  This node's 32-bit ID (used as PKT_TELEMETRY src_id).
 * @param net_id   This node's 16-bit network ID.
 */
void sensors_tick(uint32_t now_ms, uint32_t node_id, uint16_t net_id);

#ifdef __cplusplus
}
#endif
