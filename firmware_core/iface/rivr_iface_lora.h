/**
 * @file  rivr_iface_lora.h
 * @brief LoRa transport adapter for the Rivr packet bus.
 *
 * Wraps the rf_tx_queue push path so the bus can send frames over LoRa
 * without knowing the internal queue mechanics.
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/**
 * @brief Push a frame onto the LoRa TX queue.
 *
 * Computes time-on-air via RF_TOA_APPROX_US, then calls rb_try_push on
 * rf_tx_queue.  This function does NOT apply duty-cycle or airtime-scheduler
 * checks — those are enforced in tx_drain_loop() in main.c.
 *
 * @param data  Encoded Rivr frame bytes
 * @param len   Frame length (1–255 bytes)
 * @return true  if the frame was queued successfully
 * @return false if rf_tx_queue is full (frame dropped)
 */
bool rivr_iface_lora_send(const uint8_t *data, size_t len);
