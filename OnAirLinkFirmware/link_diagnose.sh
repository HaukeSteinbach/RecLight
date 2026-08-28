#!/usr/bin/env bash
#
# link_diagnose.sh -- reconfigure a RecLight device to a new WiFi network
# (e.g. a phone hotspot) in Ableton Link mode, then check whether Link peer
# discovery actually works on that network. INTERNAL DIAGNOSTIC TOOL.
#
# Runs entirely offline / standalone -- no chat connection needed while your
# Mac's only WiFi link is the "RecLight Setup" AP (which has no internet).
#
# BEFORE running this:
#   1. Join the "RecLight Setup" WiFi network on this Mac (WiFi menu bar icon,
#      open network, no password).
#
# Usage:
#   ./link_diagnose.sh <NEW_WIFI_SSID> <NEW_WIFI_PASSWORD> [STUDIO] [SERIAL_PORT]
#
# Example:
#   ./link_diagnose.sh "iPhone von Hauke" "correcthorsebattery" 2
#
# What it does:
#   1. Confirms the RecLight setup portal (192.168.4.1) is reachable.
#   2. Sends the new WiFi credentials + Ableton Link mode to the device.
#   3. Reconnects this Mac to the new WiFi network.
#   4. Waits for the device's IP announce broadcast (confirms it joined).
#   5. Reads live serial output to show Ableton Link peer count (peers=N).
#
# Nothing here is uploaded/sent anywhere except to the RecLight device itself
# on the local network (http://192.168.4.1) and to your own Mac's WiFi stack.

set -euo pipefail

SSID="${1:?Usage: $0 <new_wifi_ssid> <new_wifi_password> [studio] [serial_port]}"
PASS="${2:?Usage: $0 <new_wifi_ssid> <new_wifi_password> [studio] [serial_port]}"
STUDIO="${3:-2}"
SERIAL_PORT="${4:-}"
INTERFACE="en0"
PORTAL="http://192.168.4.1"

if [[ -z "${SERIAL_PORT}" ]]; then
    SERIAL_PORT="$(ls /dev/cu.usbmodem* /dev/cu.wchusbserial* /dev/cu.SLAB_USBtoUART* 2>/dev/null | head -n1 || true)"
fi

echo "== 1) Checking that the RecLight setup portal is reachable =="
if ! curl -s -m 5 -o /dev/null "${PORTAL}/"; then
    echo "ERROR: Can't reach ${PORTAL}." >&2
    echo "  -> Make sure this Mac is joined to the 'RecLight Setup' WiFi network first" >&2
    echo "     (WiFi menu bar icon, top right of the screen)." >&2
    exit 1
fi
echo "Portal reachable."

echo
echo "== 2) Sending new WiFi credentials + Ableton Link mode to the device =="
ENC_SSID="$(python3 -c 'import urllib.parse,sys; print(urllib.parse.quote(sys.argv[1]))' "${SSID}")"
ENC_PASS="$(python3 -c 'import urllib.parse,sys; print(urllib.parse.quote(sys.argv[1]))' "${PASS}")"
curl -s "${PORTAL}/configure?mode=2&ssid=${ENC_SSID}&pass=${ENC_PASS}&studio=${STUDIO}" -o /tmp/reclight_configure_result.html
echo "Sent. Device is saving + restarting now (~5s)."
sleep 8

echo
echo "== 3) Reconnecting this Mac to '${SSID}' =="
if networksetup -setairportnetwork "${INTERFACE}" "${SSID}" "${PASS}" 2>/tmp/reclight_wifi_join.log; then
    echo "Joined '${SSID}'."
else
    echo "Could not auto-join from this script (macOS WiFi scan permissions)." >&2
    echo "  -> Please join '${SSID}' manually now via the WiFi menu bar icon." >&2
    read -r -p "Press Enter once connected to '${SSID}'... " _dummy
fi

echo
echo "== 4) Waiting up to 30s for the device's IP announce (confirms it joined the new network) =="
python3 - <<'PYEOF'
import socket
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(('', 4211))
s.settimeout(30)
try:
    data, addr = s.recvfrom(256)
    print(f"Device announced from {addr[0]}: {data.decode(errors='replace')}")
except socket.timeout:
    print("No announce received within 30s -- device may still be joining WiFi, or the network blocks broadcast.")
PYEOF

if [[ -z "${SERIAL_PORT}" ]]; then
    echo
    echo "No serial port found/given -- skipping live Link peer check."
    echo "Plug in the device via USB and re-run with the port as the 4th argument to see live peers=N."
    exit 0
fi

echo
echo "== 5) Reading 20s of serial log from ${SERIAL_PORT} (watch the 'peers=' number) =="
python3 - <<PYEOF
import serial, time
ser = serial.Serial("${SERIAL_PORT}", 115200, timeout=1)
start = time.time()
while time.time() - start < 20:
    line = ser.readline()
    if line:
        print(line.decode('utf-8', errors='replace').rstrip())
ser.close()
PYEOF

echo
echo "== Done. =="
echo "If 'peers=0' the whole time above: open Ableton Live on a device on the"
echo "SAME network, turn on Link + Start Stop Sync, then just re-run this"
echo "script again (steps 1-3 are harmless to repeat) to see the peer count."
