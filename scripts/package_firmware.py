#!/usr/bin/env python3
"""
scripts/package_firmware.py
============================
Package a Rivr firmware variant into a user-friendly ZIP archive.

Usage:
    python scripts/package_firmware.py <pio_env> <artifact_name>

Arguments:
    pio_env        PlatformIO environment name (e.g. client_esp32devkit_e22_900)
    artifact_name  Release artifact stem      (e.g. client_esp32_e22_900)

Inputs (from PlatformIO build output):
    .pio/build/<pio_env>/bootloader.bin
    .pio/build/<pio_env>/partitions.bin
    .pio/build/<pio_env>/firmware.bin

Outputs:
    release/<artifact_name>/
        bootloader.bin
        partitions.bin
        rivr_<artifact_name>.bin
        rivr_<artifact_name>_full.bin
        flash.sh
        flash.bat
        README.txt
    rivr_<artifact_name>.zip
"""

import sys
import shutil
import subprocess
import zipfile
from pathlib import Path


# ── ESP32 flash offset map ────────────────────────────────────────────────────
BOOTLOADER_ADDR = "0x1000"
PARTITIONS_ADDR = "0x8000"
APP_ADDR        = "0x10000"


def copy_binaries(build_dir: Path, pkg_dir: Path, variant: str) -> dict:
    """Copy build artefacts into the package folder; return a name→path map."""
    files = {}

    for name in ("bootloader.bin", "partitions.bin"):
        src = build_dir / name
        dst = pkg_dir / name
        shutil.copy(src, dst)
        print(f"  copied  {src}  →  {dst}")
        files[name] = dst

    fw_dst = pkg_dir / f"rivr_{variant}.bin"
    shutil.copy(build_dir / "firmware.bin", fw_dst)
    print(f"  copied  firmware.bin  →  {fw_dst}")
    files["firmware"] = fw_dst

    return files


def merge_image(pkg_dir: Path, variant: str, files: dict) -> Path:
    """Run esptool merge-bin to produce a single flashable image."""
    full_bin = pkg_dir / f"rivr_{variant}_full.bin"
    subprocess.run(
        [
            sys.executable, "-m", "esptool",
            "--chip", "esp32",
            "merge-bin",
            "-o", str(full_bin),
            BOOTLOADER_ADDR, str(files["bootloader.bin"]),
            PARTITIONS_ADDR, str(files["partitions.bin"]),
            APP_ADDR,        str(files["firmware"]),
        ],
        check=True,
    )
    print(f"  merged  →  {full_bin}  ({full_bin.stat().st_size // 1024} KB)")
    return full_bin


def write_flash_sh(pkg_dir: Path, variant: str) -> None:
    """Write a Linux/macOS bash flash script."""
    content = (
        "#!/usr/bin/env bash\n"
        f"# Flash Rivr firmware – {variant}\n"
        "# Usage: ./flash.sh [PORT]   (default: /dev/ttyUSB0)\n"
        "set -e\n"
        'PORT="${1:-/dev/ttyUSB0}"\n'
        'echo "Flashing to $PORT …"\n'
        "python -m esptool --chip esp32 --port \"$PORT\" --baud 921600 write_flash \\\n"
        f"  {BOOTLOADER_ADDR}  bootloader.bin \\\n"
        f"  {PARTITIONS_ADDR}  partitions.bin \\\n"
        f"  {APP_ADDR} rivr_{variant}.bin\n"
    )
    p = pkg_dir / "flash.sh"
    p.write_text(content, encoding="utf-8")
    p.chmod(0o755)
    print(f"  created {p}")


def write_flash_bat(pkg_dir: Path, variant: str) -> None:
    """Write a Windows batch flash script (CRLF line endings)."""
    lines = [
        "@echo off",
        f"REM Flash Rivr firmware – {variant}",
        "REM Usage: flash.bat [COM_PORT]   (default: COM3)",
        "SET PORT=%1",
        'IF "%PORT%"=="" SET PORT=COM3',
        'echo Flashing to %PORT% ...',
        "python -m esptool --chip esp32 --port %PORT% --baud 921600 write_flash ^",
        f"  {BOOTLOADER_ADDR}  bootloader.bin ^",
        f"  {PARTITIONS_ADDR}  partitions.bin ^",
        f"  {APP_ADDR} rivr_{variant}.bin",
    ]
    p = pkg_dir / "flash.bat"
    p.write_bytes("\r\n".join(lines).encode("utf-8"))
    print(f"  created {p}")


def write_readme(pkg_dir: Path, variant: str) -> None:
    """Write a plain-text flashing guide."""
    sep = "=" * (18 + len(variant))
    content = (
        f"Rivr Firmware – {variant}\n"
        f"{sep}\n"
        "\n"
        "Files in this package\n"
        "---------------------\n"
        f"  bootloader.bin               ESP32 bootloader\n"
        f"  partitions.bin               Partition table\n"
        f"  rivr_{variant}.bin           Application firmware\n"
        f"  rivr_{variant}_full.bin      Merged single-file image (all three above)\n"
        f"  flash.sh                     Linux/macOS flash script\n"
        f"  flash.bat                    Windows flash script\n"
        "\n"
        "Requirements\n"
        "------------\n"
        "  Python 3 and esptool must be installed:\n"
        "    pip install esptool\n"
        "\n"
        "Flashing – Linux / macOS\n"
        "------------------------\n"
        "  1. Connect your board via USB.\n"
        "  2. chmod +x flash.sh\n"
        "  3. ./flash.sh /dev/ttyUSB0\n"
        "     (Replace /dev/ttyUSB0 with your actual port, e.g. /dev/ttyACM0)\n"
        "\n"
        "Flashing – Windows\n"
        "------------------\n"
        "  1. Connect your board via USB.\n"
        "  2. Open a Command Prompt in this folder.\n"
        "  3. flash.bat COM3\n"
        "     (Replace COM3 with your actual COM port)\n"
        "\n"
        "Flashing – single merged image (any OS)\n"
        "---------------------------------------\n"
        "  This method writes everything in one command:\n"
        f"    python -m esptool --chip esp32 --baud 921600 write_flash \\\n"
        f"      0x0 rivr_{variant}_full.bin\n"
        "\n"
        "Finding your COM port\n"
        "---------------------\n"
        "  Linux:   ls /dev/tty{USB,ACM}*\n"
        "  macOS:   ls /dev/cu.*\n"
        "  Windows: Device Manager → Ports (COM & LPT)\n"
        "\n"
        "Support\n"
        "-------\n"
        "  https://github.com/MichTronics/Rivr\n"
    )
    p = pkg_dir / "README.txt"
    p.write_text(content, encoding="utf-8")
    print(f"  created {p}")


def create_zip(pkg_dir: Path, variant: str) -> Path:
    """Zip the package folder; paths inside start at <variant>/."""
    zip_path = Path(f"rivr_{variant}.zip")
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as zf:
        for f in sorted(pkg_dir.rglob("*")):
            if f.is_file():
                # archive name: <variant>/filename  (no leading "release/")
                arcname = Path(variant) / f.relative_to(pkg_dir)
                zf.write(f, arcname)
    print(f"  zipped  →  {zip_path}  ({zip_path.stat().st_size // 1024} KB)")
    return zip_path


def main() -> None:
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} <pio_env> <artifact_name>", file=sys.stderr)
        sys.exit(1)

    pio_env  = sys.argv[1]
    variant  = sys.argv[2]

    build_dir = Path(f".pio/build/{pio_env}")
    pkg_dir   = Path(f"release/{variant}")

    # Validate inputs exist before doing any work.
    for required in ("bootloader.bin", "partitions.bin", "firmware.bin"):
        p = build_dir / required
        if not p.exists():
            print(f"ERROR: expected build artefact not found: {p}", file=sys.stderr)
            sys.exit(1)

    pkg_dir.mkdir(parents=True, exist_ok=True)
    print(f"\n── Packaging {variant} ───────────────────────────────────────────")

    files    = copy_binaries(build_dir, pkg_dir, variant)
    _        = merge_image(pkg_dir, variant, files)
    write_flash_sh(pkg_dir, variant)
    write_flash_bat(pkg_dir, variant)
    write_readme(pkg_dir, variant)
    zip_path = create_zip(pkg_dir, variant)

    print(f"\n✓ Package ready: {zip_path}\n")


if __name__ == "__main__":
    main()
