#!/usr/bin/env bash
# Flash Rivr firmware – client_heltec_t114_ble (nRF52840)
# Option A: adafruit-nrfutil over USB serial bootloader (DFU zip)
# Usage: ./flash.sh [PORT]   (default: /dev/ttyACM0)
set -e
PORT="${1:-/dev/ttyACM0}"

if command -v adafruit-nrfutil &>/dev/null; then
  echo "Flashing via adafruit-nrfutil to $PORT …"
  adafruit-nrfutil dfu serial -pkg rivr_client_heltec_t114_ble_dfu.zip -p "$PORT" -b 115200
elif command -v nrfjprog &>/dev/null; then
  echo "Flashing via nrfjprog (J-Link) …"
  nrfjprog --program rivr_client_heltec_t114_ble.hex --chiperase --reset
else
  echo "Neither adafruit-nrfutil nor nrfjprog found."
  echo "  adafruit-nrfutil: pip install adafruit-nrfutil"
  echo "  nrfjprog: https://www.nordicsemi.com/Products/Development-tools/nRF-Command-Line-Tools"
  exit 1
fi
