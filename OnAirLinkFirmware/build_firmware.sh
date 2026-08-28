#!/usr/bin/env bash
#
# build_firmware.sh -- build the RecLight Ableton Link firmware and refresh the
# merged flash image (dist/reclight_link_merged.bin). INTERNAL TOOL.
#
# Requires ESP-IDF v5.5 installed at ~/esp/esp-idf.
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "${SCRIPT_DIR}"

if [[ ! -f "${HOME}/esp/esp-idf/export.sh" ]]; then
    echo "ERROR: ESP-IDF not found at ~/esp/esp-idf" >&2
    exit 1
fi
# shellcheck disable=SC1091
source "${HOME}/esp/esp-idf/export.sh" >/dev/null 2>&1

idf.py set-target esp32c3 >/dev/null
idf.py build

mkdir -p dist
( cd build && esptool.py --chip esp32c3 merge_bin \
    -o ../dist/reclight_link_merged.bin \
    --flash_mode dio --flash_freq 80m --flash_size 2MB \
    0x0 bootloader/bootloader.bin \
    0x8000 partition_table/partition-table.bin \
    0x10000 reclight_link.bin )

echo ""
echo "Done. Merged image: dist/reclight_link_merged.bin"
echo "Flash it onto ESPs with:  ./flash_esp.sh [port]"
