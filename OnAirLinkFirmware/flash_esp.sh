#!/usr/bin/env bash
#
# flash_esp.sh -- flash the current RecLight Ableton Link firmware onto an ESP32-C3.
# INTERNAL TOOL (not for customers).
#
# Flashes the pre-merged image dist/reclight_link_merged.bin, so it does NOT
# recompile and does NOT require the full ESP-IDF toolchain -- only esptool.
#
# Usage:
#   ./flash_esp.sh                 # auto-detect the serial port
#   ./flash_esp.sh /dev/cu.usbmodem2101
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
IMAGE="${SCRIPT_DIR}/dist/reclight_link_merged.bin"

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
    0x0 "${IMAGE}"

echo ""
echo "Done. The ESP has been reset and is running the RecLight Link firmware."
