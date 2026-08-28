#!/usr/bin/env bash
#
# flash_esp.sh -- flash the current RecLight firmware onto an ESP32-C3.
# INTERNAL TOOL (not for customers).
#
# Flashes the pre-merged image dist/reclight_link_merged.bin, so it does NOT
# recompile and does NOT require the full ESP-IDF toolchain -- only esptool.
#
# Usage:
#   ./flash_esp.sh                 # auto-detect the serial port
#   ./flash_esp.sh /dev/cu.usbmodem2101
#   ./flash_esp.sh --update [port] # keep the device's WiFi settings
#
# NOTE: a normal flash FACTORY-RESETS the device. The merged image is one
# contiguous blob from 0x0, so it necessarily covers the NVS partition at
# 0x9000 and fills it with 0xFF -- the saved WiFi credentials and lamp
# settings go with it, and the device comes back at setup step 1. That is what
# you want when flashing new boards.
#
# --update writes only the app partition at 0x10000, leaving NVS alone, which
# is what you want when putting new firmware on a device that is already set
# up in a studio.
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
IMAGE="${SCRIPT_DIR}/dist/reclight_link_merged.bin"

UPDATE=0
OFFSET="0x0"
if [[ "${1:-}" == "--update" ]]; then
    UPDATE=1
    shift
    IMAGE="${SCRIPT_DIR}/build/reclight_link.bin"   # the app alone
    OFFSET="0x10000"                                # app partition, NVS untouched
    if [[ ! -f "${IMAGE}" ]]; then
        echo "ERROR: app image not found: ${IMAGE}" >&2
        echo "Build it first with:  ./build_firmware.sh" >&2
        exit 1
    fi
fi

if [[ ! -f "${IMAGE}" ]]; then
    echo "ERROR: firmware image not found: ${IMAGE}" >&2
    echo "Build it first with:  ./build_firmware.sh" >&2
    exit 1
fi

# --- Find a serial port -----------------------------------------------------
PORT="${1:-}"
if [[ -z "${PORT}" ]]; then
    PORT="$(ls /dev/cu.usbmodem* /dev/cu.wchusbserial* /dev/cu.SLAB_USBtoUART* 2>/dev/null | head -n1 || true)"
    if [[ -z "${PORT}" ]]; then
        echo "ERROR: no serial port found. Plug in the ESP32-C3 or pass the port explicitly:" >&2
        echo "  ./flash_esp.sh /dev/cu.usbmodemXXXX" >&2
        exit 1
    fi
    echo "==> Auto-detected port: ${PORT}"
fi

# --- Find esptool -----------------------------------------------------------
ESPTOOL=""
if command -v esptool.py >/dev/null 2>&1; then
    ESPTOOL="esptool.py"
elif python3 -c "import esptool" >/dev/null 2>&1; then
    ESPTOOL="python3 -m esptool"
elif [[ -f "${HOME}/esp/esp-idf/export.sh" ]]; then
    # Fall back to the IDF environment (provides esptool).
    # shellcheck disable=SC1091
    source "${HOME}/esp/esp-idf/export.sh" >/dev/null 2>&1
    ESPTOOL="esptool.py"
else
    echo "ERROR: esptool not found. Install it with:  pip3 install esptool" >&2
    exit 1
fi

# --- Flash ------------------------------------------------------------------
echo "==> Flashing ${IMAGE##*/} to ${PORT}"
${ESPTOOL} --chip esp32c3 -p "${PORT}" -b 460800 \
    --before default_reset --after hard_reset \
    write_flash --flash_mode dio --flash_freq 80m --flash_size 2MB \
    "${OFFSET}" "${IMAGE}"

echo ""
echo "Done. The ESP has been reset and is running the RecLight Link firmware."
