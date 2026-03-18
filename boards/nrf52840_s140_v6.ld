/* Linker script for Heltec T114 (nRF52840 + S140 v6.1.1 softdevice)
 * Based on MeshCore project — https://github.com/meshcore-dev/MeshCore
 *
 * FLASH: starts after S140 softdevice at 0x26000
 * RAM:   starts after S140 RAM reservation at 0x20006000
 */

SEARCH_DIR(.)
GROUP(-lgcc -lc -lnosys)

MEMORY
{
  FLASH (rx)  : ORIGIN = 0x26000, LENGTH = 0xED000 - 0x26000
  RAM   (rwx) : ORIGIN = 0x20006000, LENGTH = 0x20040000 - 0x20006000
}

SECTIONS
{
  . = ALIGN(4);
  .svc_data :
  {
    PROVIDE(__start_svc_data = .);
    KEEP(*(.svc_data))
    PROVIDE(__stop_svc_data = .);
  } > RAM

  .fs_data :
  {
    PROVIDE(__start_fs_data = .);
    KEEP(*(.fs_data))
    PROVIDE(__stop_fs_data = .);
  } > RAM
} INSERT AFTER .data;

INCLUDE "nrf52_common.ld"
