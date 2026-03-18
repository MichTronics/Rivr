/*
 * variant.cpp — Heltec T114 (nRF52840)
 *
 * g_ADigitalPinMap maps "Arduino pin numbers" to nRF52 GPIO flat numbers.
 * Flat GPIO numbering: P0.x = x, P1.x = 32+x.
 * Pins 0 and 1 are reserved (0xff = invalid).
 *
 * Adapted from MeshCore project — MIT License
 */

#include "variant.h"
#include "wiring_constants.h"
#include "wiring_digital.h"

const uint32_t g_ADigitalPinMap[] = {
  // P0.00..P0.13 — 0 and 1 reserved
  0xff, 0xff, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13,
  // P0.14..P0.26
  14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26,
  // P0.27..P0.39
  27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39,
  // P1.08..P1.15 (flat 40..47)
  40, 41, 42, 43, 44, 45, 46, 47
};

void initVariant()
{
  pinMode(PIN_USER_BTN, INPUT);
}
