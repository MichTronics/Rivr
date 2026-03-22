Rivr Firmware – client_heltec_t114_ble (nRF52840)
========================================

Files in this package
---------------------
  rivr_client_heltec_t114_ble.hex          Intel HEX for J-Link / nrfjprog
  rivr_client_heltec_t114_ble_dfu.zip      adafruit-nrfutil DFU package (serial bootloader)
  rivr_client_heltec_t114_ble.json         Metadata
  flash.sh                    Linux/macOS flash script

Flashing – adafruit-nrfutil / serial bootloader (easiest)
----------------------------------------------------------
  1. Install: pip install adafruit-nrfutil
  2. Double-click RESET to enter the serial DFU bootloader.
  3. chmod +x flash.sh && ./flash.sh /dev/ttyACM0

Flashing – J-Link / nrfjprog
-----------------------------
  nrfjprog --program rivr_{variant}.hex --chiperase --reset

Support
-------
  https://github.com/MichTronics/Rivr
