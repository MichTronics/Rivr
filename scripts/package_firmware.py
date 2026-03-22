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
        rivr_<artifact_name>.json
        flash.sh
        flash.bat
        README.txt
    rivr_<artifact_name>.zip
"""

import json
import sys
import shutil
import subprocess
import zipfile
from pathlib import Path


DEFAULT_FLASH_CONFIG = {
    "chip": "esp32",
    "flash_mode": "dio",
    "flash_size": "4MB",
    "flash_freq": "80m",
    "segments": [
        {"address": "0x1000", "file": "bootloader"},
        {"address": "0x9000", "file": "partitions"},
        {"address": "0x20000", "file": "app"},
    ],
}


def load_flash_config(build_dir: Path) -> dict:
    """Read the exact flash layout from PlatformIO's flasher_args.json."""
    args_path = build_dir / "flasher_args.json"
    if not args_path.exists():
        return json.loads(json.dumps(DEFAULT_FLASH_CONFIG))

    with args_path.open("r", encoding="utf-8") as fp:
        raw = json.load(fp)

    flash_settings = raw.get("flash_settings", {})
    segments = [
        {
            "address": raw.get("bootloader", {}).get("offset", DEFAULT_FLASH_CONFIG["segments"][0]["address"]),
            "file": "bootloader",
        },
        {
            "address": raw.get("partition-table", {}).get("offset", DEFAULT_FLASH_CONFIG["segments"][1]["address"]),
            "file": "partitions",
        },
        {
            "address": raw.get("app", {}).get("offset", DEFAULT_FLASH_CONFIG["segments"][2]["address"]),
            "file": "app",
        },
    ]

    return {
        "chip": raw.get("extra_esptool_args", {}).get("chip", DEFAULT_FLASH_CONFIG["chip"]),
        "flash_mode": flash_settings.get("flash_mode", DEFAULT_FLASH_CONFIG["flash_mode"]),
        "flash_size": flash_settings.get("flash_size", DEFAULT_FLASH_CONFIG["flash_size"]),
        "flash_freq": flash_settings.get("flash_freq", DEFAULT_FLASH_CONFIG["flash_freq"]),
        "segments": segments,
    }


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


def merge_image(pkg_dir: Path, variant: str, files: dict, flash_config: dict) -> Path:
    """Run esptool merge-bin to produce a single flashable image."""
    full_bin = pkg_dir / f"rivr_{variant}_full.bin"
    segment_map = {segment["file"]: segment["address"] for segment in flash_config["segments"]}
    subprocess.run(
        [
            sys.executable, "-m", "esptool",
            "--chip", flash_config["chip"],
            "merge-bin",
            "--flash_mode", flash_config["flash_mode"],
            "--flash_size", flash_config["flash_size"],
            "--flash_freq", flash_config["flash_freq"],
            "-o", str(full_bin),
            segment_map["bootloader"], str(files["bootloader.bin"]),
            segment_map["partitions"], str(files["partitions.bin"]),
            segment_map["app"],        str(files["firmware"]),
        ],
        check=True,
    )
    print(f"  merged  →  {full_bin}  ({full_bin.stat().st_size // 1024} KB)")
    return full_bin


def write_manifest_json(pkg_dir: Path, variant: str, flash_config: dict) -> Path:
    """Write a sidecar manifest that matches the website flasher format."""
    manifest_path = pkg_dir / f"rivr_{variant}.json"
    manifest_path.write_text(json.dumps(flash_config, indent=2) + "\n", encoding="utf-8")
    print(f"  created {manifest_path}")
    return manifest_path


def write_flash_sh(pkg_dir: Path, variant: str, flash_config: dict) -> None:
    """Write a Linux/macOS bash flash script."""
    segment_map = {segment["file"]: segment["address"] for segment in flash_config["segments"]}
    content = (
        "#!/usr/bin/env bash\n"
        f"# Flash Rivr firmware – {variant}\n"
        "# Usage: ./flash.sh [PORT]   (default: /dev/ttyUSB0)\n"
        "set -e\n"
        'PORT="${1:-/dev/ttyUSB0}"\n'
        'echo "Flashing to $PORT …"\n'
        f"python -m esptool --chip {flash_config['chip']} --port \"$PORT\" --baud 921600 write_flash \\\n"
        f"  --flash_mode {flash_config['flash_mode']} \\\n"
        f"  --flash_freq {flash_config['flash_freq']} \\\n"
        f"  --flash_size {flash_config['flash_size']} \\\n"
        f"  {segment_map['bootloader']}  bootloader.bin \\\n"
        f"  {segment_map['partitions']}  partitions.bin \\\n"
        f"  {segment_map['app']} rivr_{variant}.bin\n"
    )
    p = pkg_dir / "flash.sh"
    p.write_text(content, encoding="utf-8")
    p.chmod(0o755)
    print(f"  created {p}")


def write_flash_bat(pkg_dir: Path, variant: str, flash_config: dict) -> None:
    """Write a Windows batch flash script (CRLF line endings)."""
    segment_map = {segment["file"]: segment["address"] for segment in flash_config["segments"]}
    lines = [
        "@echo off",
        f"REM Flash Rivr firmware – {variant}",
        "REM Usage: flash.bat [COM_PORT]   (default: COM3)",
        "SET PORT=%1",
        'IF "%PORT%"=="" SET PORT=COM3',
        'echo Flashing to %PORT% ...',
        f"python -m esptool --chip {flash_config['chip']} --port %PORT% --baud 921600 write_flash ^",
        f"  --flash_mode {flash_config['flash_mode']} ^",
        f"  --flash_freq {flash_config['flash_freq']} ^",
        f"  --flash_size {flash_config['flash_size']} ^",
        f"  {segment_map['bootloader']}  bootloader.bin ^",
        f"  {segment_map['partitions']}  partitions.bin ^",
        f"  {segment_map['app']} rivr_{variant}.bin",
    ]
    p = pkg_dir / "flash.bat"
    p.write_bytes("\r\n".join(lines).encode("utf-8"))
    print(f"  created {p}")


def write_readme(pkg_dir: Path, variant: str, flash_config: dict) -> None:
    """Write a plain-text flashing guide."""
    sep = "=" * (18 + len(variant))
    segment_map = {segment["file"]: segment["address"] for segment in flash_config["segments"]}
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
        f"  rivr_{variant}.json          Exact webflasher / esptool segment metadata\n"
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
        f"    python -m esptool --chip {flash_config['chip']} --baud 921600 write_flash \\\n"
        f"      0x0 rivr_{variant}_full.bin\n"
        "\n"
        "Exact segment layout\n"
        "--------------------\n"
        f"  chip         {flash_config['chip']}\n"
        f"  flash_mode   {flash_config['flash_mode']}\n"
        f"  flash_freq   {flash_config['flash_freq']}\n"
        f"  flash_size   {flash_config['flash_size']}\n"
        f"  bootloader   {segment_map['bootloader']}\n"
        f"  partitions   {segment_map['partitions']}\n"
        f"  app          {segment_map['app']}\n"
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


def is_nrf52_env(pio_env: str) -> bool:
    """Return True for nRF52 PlatformIO environments (flat .bin, no bootloader)."""
    return "_t114" in pio_env or "_t1000_e" in pio_env or "_nrf52" in pio_env


def package_nrf52(build_dir: Path, pkg_dir: Path, variant: str) -> None:
    """Package an nRF52 variant: flat firmware.bin only, with UF2/drag-and-drop instructions."""
    fw_src = build_dir / "firmware.bin"
    if not fw_src.exists():
        print(f"ERROR: expected build artefact not found: {fw_src}", file=sys.stderr)
        sys.exit(1)

    pkg_dir.mkdir(parents=True, exist_ok=True)
    print(f"\n── Packaging {variant} (nRF52840) ───────────────────────────────")

    # Copy flat firmware binary
    fw_dst = pkg_dir / f"rivr_{variant}.bin"
    shutil.copy(fw_src, fw_dst)
    print(f"  copied  firmware.bin  →  {fw_dst}")

    # Copy UF2 if present (Adafruit BSP produces it alongside firmware.bin)
    uf2_src = build_dir / "firmware.uf2"
    if uf2_src.exists():
        uf2_dst = pkg_dir / f"rivr_{variant}.uf2"
        shutil.copy(uf2_src, uf2_dst)
        print(f"  copied  firmware.uf2  →  {uf2_dst}")

    # Sidecar JSON (minimal — no ESP flash segments)
    manifest = {
        "chip": "nrf52840",
        "platform": "nrf52",
        "segments": [{"address": "0x0000", "file": "firmware"}],
    }
    manifest_path = pkg_dir / f"rivr_{variant}.json"
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(f"  created {manifest_path}")

    # flash.sh for nRF52 via nrfjprog or adafruit-nrfutil
    sh_content = (
        "#!/usr/bin/env bash\n"
        f"# Flash Rivr firmware – {variant} (nRF52840)\n"
        "# Option A: adafruit-nrfutil over USB serial bootloader\n"
        "# Usage: ./flash.sh [PORT]   (default: /dev/ttyACM0)\n"
        "set -e\n"
        'PORT="${1:-/dev/ttyACM0}"\n'
        "\n"
        "if command -v adafruit-nrfutil &>/dev/null; then\n"
        '  echo "Flashing via adafruit-nrfutil to $PORT …"\n'
        f"  adafruit-nrfutil dfu serial -pkg rivr_{variant}.bin -p \"$PORT\" -b 115200\n"
        "else\n"
        '  echo "adafruit-nrfutil not found. Install with: pip install adafruit-nrfutil"\n'
        '  echo "Or drag rivr_{variant}.uf2 to the T114 UF2 bootloader drive."\n'
        "  exit 1\n"
        "fi\n"
    )
    sh_path = pkg_dir / "flash.sh"
    sh_path.write_text(sh_content, encoding="utf-8")
    sh_path.chmod(0o755)
    print(f"  created {sh_path}")

    # README
    sep = "=" * (18 + len(variant))
    readme_content = (
        f"Rivr Firmware – {variant} (nRF52840)\n"
        f"{sep}\n"
        "\n"
        "Files in this package\n"
        "---------------------\n"
        f"  rivr_{variant}.bin      Firmware binary\n"
        f"  rivr_{variant}.uf2      UF2 drag-and-drop image (if present)\n"
        f"  rivr_{variant}.json     Metadata\n"
        f"  flash.sh                Linux/macOS flash script\n"
        "\n"
        "Flashing – drag and drop (easiest)\n"
        "-----------------------------------\n"
        "  1. Double-click the RESET button on the board to enter UF2 bootloader.\n"
        "     A USB drive called 'T114Boot' (or similar) will appear.\n"
        f"  2. Copy rivr_{variant}.uf2 onto that drive.\n"
        "  3. The board will reboot and run the new firmware automatically.\n"
        "\n"
        "Flashing – adafruit-nrfutil (Linux/macOS)\n"
        "-----------------------------------------\n"
        "  1. Install: pip install adafruit-nrfutil\n"
        "  2. chmod +x flash.sh\n"
        "  3. ./flash.sh /dev/ttyACM0   (replace with your port)\n"
        "\n"
        "Support\n"
        "-------\n"
        "  https://github.com/MichTronics/Rivr\n"
    )
    readme_path = pkg_dir / "README.txt"
    readme_path.write_text(readme_content, encoding="utf-8")
    print(f"  created {readme_path}")

    zip_path = create_zip(pkg_dir, variant)
    print(f"\n✓ Package ready: {zip_path}\n")


def main() -> None:
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} <pio_env> <artifact_name>", file=sys.stderr)
        sys.exit(1)

    pio_env  = sys.argv[1]
    variant  = sys.argv[2]

    build_dir = Path(f".pio/build/{pio_env}")
    pkg_dir   = Path(f"release/{variant}")

    # nRF52 variants: flat bin only, no bootloader/partitions
    if is_nrf52_env(pio_env):
        package_nrf52(build_dir, pkg_dir, variant)
        return

    flash_config = load_flash_config(build_dir)

    # Validate inputs exist before doing any work.
    for required in ("bootloader.bin", "partitions.bin", "firmware.bin"):
        p = build_dir / required
        if not p.exists():
            print(f"ERROR: expected build artefact not found: {p}", file=sys.stderr)
            sys.exit(1)

    pkg_dir.mkdir(parents=True, exist_ok=True)
    print(f"\n── Packaging {variant} ───────────────────────────────────────────")

    files    = copy_binaries(build_dir, pkg_dir, variant)
    _        = merge_image(pkg_dir, variant, files, flash_config)
    _        = write_manifest_json(pkg_dir, variant, flash_config)
    write_flash_sh(pkg_dir, variant, flash_config)
    write_flash_bat(pkg_dir, variant, flash_config)
    write_readme(pkg_dir, variant, flash_config)
    zip_path = create_zip(pkg_dir, variant)

    print(f"\n✓ Package ready: {zip_path}\n")


if __name__ == "__main__":
    main()
