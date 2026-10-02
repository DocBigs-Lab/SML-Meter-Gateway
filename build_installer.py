Import("env")
import json
import os
import re
import shutil

# Legt nach jedem Build die Flash-Teile fuer den Web-Installer
# (docs/index.html, ESP Web Tools) unter docs/firmware/ ab und schreibt
# docs/manifest.json mit der Version aus FW_VERSION in src/main.cpp neu —
# so laufen Firmware- und Manifest-Version nicht auseinander.
#
# Bewusst KEIN gemergtes Einzel-Image: esptool merge_bin fuellt die Luecke
# zwischen Partitionstabelle (0x8000) und boot_app0 (0xe000) mit 0xFF auf,
# und genau dort liegt die NVS-Partition — jedes Update ueber den Installer
# wuerde sonst WLAN/MQTT-Einstellungen loeschen. Als Einzelteile bleibt NVS
# unberuehrt; komplett loeschen geht weiterhin per Installer-Abfrage.

NAME = "SML-Meter-Gateway"


def read_fw_version(project_dir):
    try:
        with open(os.path.join(project_dir, "src", "main.cpp"), encoding="utf-8") as f:
            m = re.search(r'#define\s+FW_VERSION\s+"([^"]+)"', f.read())
            return m.group(1) if m else "dev"
    except Exception:
        return "dev"


def post_firmware(source, target, env):
    build_dir   = env.subst("$BUILD_DIR")
    project_dir = env.subst("$PROJECT_DIR")
    docs_dir    = os.path.join(project_dir, "docs")
    os.makedirs(docs_dir, exist_ok=True)

    bootloader = os.path.join(build_dir, "bootloader.bin")
    partitions = os.path.join(build_dir, "partitions.bin")
    firmware   = os.path.join(build_dir, "firmware.bin")
    boot_app0  = os.path.join(
        env.subst("$PROJECT_PACKAGES_DIR"),
        "framework-arduinoespressif32", "tools", "partitions", "boot_app0.bin"
    )

    if not all(os.path.exists(f) for f in [bootloader, partitions, firmware, boot_app0]):
        print("installer: binaries not ready – skipping")
        return

    # ESP32-C3: Bootloader liegt bei 0x0 (nicht 0x1000 wie beim klassischen
    # ESP32). Der Bootloader-Header enthaelt bereits Flash-Mode/-Groesse aus
    # dem Build (dio, 4 MB).
    fw_dir = os.path.join(docs_dir, "firmware")
    os.makedirs(fw_dir, exist_ok=True)
    parts = []
    for src, name, offset in [
        (bootloader, "bootloader.bin", 0x0),
        (partitions, "partitions.bin", 0x8000),
        (boot_app0,  "boot_app0.bin",  0xE000),
        (firmware,   "firmware.bin",   0x10000),
    ]:
        shutil.copyfile(src, os.path.join(fw_dir, name))
        parts.append({"path": f"firmware/{name}", "offset": offset})
    print(f"installer: Flash-Teile nach {fw_dir} kopiert "
          f"(firmware.bin {os.path.getsize(firmware):,} bytes)")

    version = read_fw_version(project_dir)
    manifest = {
        "name": NAME,
        "version": version,
        "new_install_prompt_erase": True,
        "builds": [{"chipFamily": "ESP32-C3", "parts": parts}],
    }
    with open(os.path.join(docs_dir, "manifest.json"), "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=2)
        f.write("\n")
    print(f"manifest.json aktualisiert: {NAME} {version}")


env.AddPostAction("$BUILD_DIR/firmware.bin", post_firmware)
