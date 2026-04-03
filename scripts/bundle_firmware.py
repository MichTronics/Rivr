#!/usr/bin/env python3
"""
scripts/bundle_firmware.py
===========================
Assemble per-board "bundle" ZIPs that combine client, BLE client, and
repeater firmware into a single downloadable archive.

Used on the download page so users grab one ZIP for their hardware and
find all role variants inside.

Usage:
    python scripts/bundle_firmware.py <zips_dir> <bundles_json> [out_dir]

Arguments:
    zips_dir      Directory containing the per-variant  rivr_<artifact>.zip files
                  produced by package_firmware.py.
    bundles_json  Path to .github/firmware-bundles.json (bundle definitions).
    out_dir       Output directory for bundle ZIPs (default: same as zips_dir).

Outputs (one ZIP per entry in bundles_json):
    rivr_<bundle>.zip
        <bundle>/README.txt
        <bundle>/client/<variant files …>
        <bundle>/client_ble/<variant files …>   (if present)
        <bundle>/repeater/<variant files …>
"""

import json
import sys
import zipfile
from pathlib import Path


def role_folder(artifact: str) -> str:
    """Derive a short role folder name from an artifact stem."""
    if artifact.startswith("relay_"):
        return "repeater"
    if artifact.endswith("_ble"):
        return "client_ble"
    return "client"


def create_bundle(bundle_def: dict, zips_dir: Path, out_dir: Path) -> Path:
    """Create one board bundle ZIP from its constituent per-variant ZIPs."""
    bundle_name = bundle_def["bundle"]
    board_label = bundle_def["board"]
    artifacts   = bundle_def["artifacts"]

    out_path = out_dir / f"rivr_{bundle_name}_bundle.zip"

    roles_present = [role_folder(a) for a in artifacts]

    readme_lines = [
        f"Rivr Firmware Bundle \u2013 {board_label}",
        "=" * (22 + len(board_label)),
        "",
        "This archive contains firmware images for the " + board_label + ".",
        "Choose the sub-folder that matches the role you want to flash:",
        "",
    ]
    for artifact in artifacts:
        role = role_folder(artifact)
        readme_lines.append(f"  {role:<14}  \u2192  {artifact}")

    readme_lines += [
        "",
        "Flashing",
        "--------",
        "  Open the sub-folder for the role you want and follow README.txt inside.",
        "",
        "Roles explained",
        "---------------",
        "  client      Chat client (USB serial shell, no BLE).",
        "  client_ble  Chat client + BLE companion app support.",
        "  repeater    Fabric repeater / routing node, no local UI.",
        "",
        "Support",
        "-------",
        "  https://github.com/MichTronics/Rivr",
        "",
    ]

    with zipfile.ZipFile(out_path, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as out_zf:
        out_zf.writestr(f"{bundle_name}/README.txt", "\n".join(readme_lines))

        for artifact in artifacts:
            src_zip = zips_dir / f"rivr_{artifact}.zip"
            if not src_zip.exists():
                print(
                    f"ERROR: expected artifact ZIP not found: {src_zip}",
                    file=sys.stderr,
                )
                sys.exit(1)

            role = role_folder(artifact)

            with zipfile.ZipFile(src_zip, "r") as in_zf:
                for entry in in_zf.infolist():
                    if entry.is_dir():
                        continue
                    # Original layout inside per-variant ZIPs:
                    #   <artifact>/filename
                    # Rewrite to:
                    #   <bundle_name>/<role>/filename
                    parts = Path(entry.filename).parts
                    file_part = "/".join(parts[1:]) if len(parts) >= 2 else entry.filename
                    new_name = f"{bundle_name}/{role}/{file_part}"
                    data = in_zf.read(entry.filename)
                    info = zipfile.ZipInfo(new_name)
                    info.compress_type = zipfile.ZIP_DEFLATED
                    # Preserve executable bit for flash.sh
                    if entry.external_attr >> 16:
                        info.external_attr = entry.external_attr
                    out_zf.writestr(info, data)

    size_kb = out_path.stat().st_size // 1024
    print(f"  bundle  {out_path.name}  ({size_kb} KB)  [{', '.join(roles_present)}]")
    return out_path


def main() -> None:
    if len(sys.argv) < 3:
        print(
            f"Usage: {sys.argv[0]} <zips_dir> <bundles_json> [out_dir]",
            file=sys.stderr,
        )
        sys.exit(1)

    zips_dir     = Path(sys.argv[1])
    bundles_json = Path(sys.argv[2])
    out_dir      = Path(sys.argv[3]) if len(sys.argv) > 3 else zips_dir

    if not zips_dir.is_dir():
        print(f"ERROR: zips_dir not found: {zips_dir}", file=sys.stderr)
        sys.exit(1)

    if not bundles_json.exists():
        print(f"ERROR: bundles_json not found: {bundles_json}", file=sys.stderr)
        sys.exit(1)

    bundles = json.loads(bundles_json.read_text(encoding="utf-8"))

    out_dir.mkdir(parents=True, exist_ok=True)

    print(f"\n\u2500\u2500 Creating firmware bundles \u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500")
    for bundle_def in bundles:
        create_bundle(bundle_def, zips_dir, out_dir)

    print(f"\n\u2713 {len(bundles)} bundle(s) created in {out_dir}\n")


if __name__ == "__main__":
    main()
