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
#include <stdbool.h>

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

/**
 * @brief Runtime sensor transmit policy — all fields configurable at runtime.
 *
 * Compile-time defaults are loaded first; NVS overrides on boot; BLE command
 * RIVR_CP_CMD_SET_SENSOR_CONFIG (0x08) can update fields at any time and
 * optionally persist them to NVS.
 */
typedef struct {
    uint32_t tx_ms;          /**< Heartbeat interval (ms).         Default 300000. */
    uint32_t min_delta_ms;   /**< Min gap between delta TXes (ms). Default 30000.  */
    uint16_t delta_temp;     /**< Temp change to trigger (°C×100). Default 50.     */
    uint16_t delta_rh;       /**< RH change to trigger (%×100).    Default 100.    */
    uint16_t delta_vbat;     /**< VBAT change to trigger (mV).     Default 100.    */
} sensors_config_t;

/**
 * @brief Load sensor config from NVS (if available) or apply compile-time defaults.
 * Call once, after NVS is initialised, before sensors_init().
 */
void sensors_nvs_load(void);

/**
 * @brief Persist current sensor config to NVS.
 * @return true on success, false if NVS write fails.
 */
bool sensors_nvs_save(void);

/**
 * @brief Read current sensor transmit policy.
 */
sensors_config_t sensors_get_config(void);

/**
 * @brief Apply new sensor transmit policy at runtime.
 * Changes take effect on the next sensors_tick() call.
 * @param cfg  New policy to apply (all fields validated).
 */
void sensors_set_config(const sensors_config_t *cfg);

/**
 * @brief Last-known readings from all enabled sensors.
 *
 * Fields are INT32_MIN when no reading has been taken yet (e.g. DS18B20
 * conversion not yet complete, sensor not enabled at compile time).
 */
typedef struct {
    int32_t ds18b20_temp_x100;  /**< DS18B20 temperature  (°C × 100)  */
    int32_t am2302_rh_x100;     /**< AM2302 humidity      (% RH × 100) */
    int32_t am2302_temp_x100;   /**< AM2302 temperature   (°C × 100)  */
    int32_t vbat_mv;            /**< Battery voltage      (mV)         */
} sensors_last_t;

/**
 * @brief Return the most recent readings from all enabled sensors.
 *
 * Any field that has not yet received a reading is set to INT32_MIN.
 * Safe to call from the main loop on every iteration — no I/O, pure state read.
 */
sensors_last_t sensors_get_last(void);

#ifdef __cplusplus
}
#endif
