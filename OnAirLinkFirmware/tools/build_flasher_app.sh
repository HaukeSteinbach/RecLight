#!/usr/bin/env bash
#
# build_flasher_app.sh -- builds "RecLight Flasher.app", a self-contained macOS
# app bundle of the board flasher. INTERNAL TOOL (production/board flashing);
# it is deliberately NOT part of the customer installer.
#
# The bundle carries its own Python, Tk, esptool and a copy of the merged
# firmware image, so the machine running it needs neither Homebrew nor
# ESP-IDF nor a checkout of this repo.
#
# Usage:
#   ./build_flasher_app.sh              # build into tools/dist/
#   ./build_flasher_app.sh --install    # build, then install to /Applications
#
set -euo pipefail

APP_NAME="RecLight Flasher"
BUNDLE_ID="com.steinbach-audio.reclight.flasher"
VERSION="1.0.0"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "${SCRIPT_DIR}"

VENV_PY="${SCRIPT_DIR}/.venv/bin/python3"
IMAGE="${SCRIPT_DIR}/../dist/reclight_link_merged.bin"
BUILD_DIR="${SCRIPT_DIR}/build"
DIST_DIR="${SCRIPT_DIR}/dist"
ICON="${BUILD_DIR}/RecLight.icns"

INSTALL=0
[[ "${1:-}" == "--install" ]] && INSTALL=1

# --- Toolchain --------------------------------------------------------------
# Homebrew's python-tk@3.13 is required at BUILD time, not at run time: it is
# the only easily available Python on macOS with a working modern Tcl/Tk
# (the system Python still ships Tk 8.5, where the window renders but buttons
# don't respond to clicks). PyInstaller copies that Tk into the bundle.
if [[ ! -x "${VENV_PY}" ]]; then
    echo "==> Creating the flasher build environment (one-time)"
    BREW_PY="$(brew --prefix 2>/dev/null)/opt/python@3.13/bin/python3.13"
    if [[ ! -x "${BREW_PY}" ]]; then
        echo "Installing Homebrew python-tk@3.13 (provides a working modern Tk)..."
        brew install python-tk@3.13
        BREW_PY="$(brew --prefix)/opt/python@3.13/bin/python3.13"
    fi
    "${BREW_PY}" -m venv "${SCRIPT_DIR}/.venv"
fi
# esptool >= 5 for the modern command spelling used in flash_gui.py.
"${VENV_PY}" -m pip install --quiet --upgrade pip "esptool>=5" pyinstaller

if [[ ! -f "${IMAGE}" ]]; then
    echo "ERROR: firmware image not found: ${IMAGE}" >&2
    echo "Build it first:  ../build_firmware.sh" >&2
    exit 1
fi

# --- Icon -------------------------------------------------------------------
mkdir -p "${BUILD_DIR}"
echo "==> Generating icon"
"${VENV_PY}" make_icon.py "${ICON}" >/dev/null

# --- Bundle -----------------------------------------------------------------
# --collect-all, not --hidden-import: esptool ships the flasher stubs as JSON
# DATA files (esptool/targets/stub_flasher/*/*.json). A module-level import
# hint pulls in the code but not those, and the app then fails at the moment
# it connects to a board with "Flasher stub data is missing for ESP32-C3".
echo "==> Building ${APP_NAME}.app"
rm -rf "${DIST_DIR}/${APP_NAME}.app"
"${VENV_PY}" -m PyInstaller \
    --noconfirm \
    --clean \
    --windowed \
    --name "${APP_NAME}" \
    --icon "${ICON}" \
    --osx-bundle-identifier "${BUNDLE_ID}" \
    --add-data "${IMAGE}:." \
    --collect-all esptool \
    --distpath "${DIST_DIR}" \
    --workpath "${BUILD_DIR}/pyinstaller" \
    --specpath "${BUILD_DIR}" \
    flash_gui.py

# PyInstaller writes CFBundleShortVersionString as "0.0.0" unless told
# otherwise; set it so the Finder's Get Info panel isn't nonsense.
PLIST="${DIST_DIR}/${APP_NAME}.app/Contents/Info.plist"
/usr/libexec/PlistBuddy -c "Set :CFBundleShortVersionString ${VERSION}" "${PLIST}" 2>/dev/null || true
/usr/libexec/PlistBuddy -c "Set :CFBundleVersion ${VERSION}" "${PLIST}" 2>/dev/null || true

# Ad-hoc signature: unsigned bundles built this way are killed on launch by
# Gatekeeper on Apple silicon. This is not notarization -- a first launch
# still needs right-click > Open on another Mac -- but it makes the app run
# on the machine that built it.
codesign --force --deep --sign - "${DIST_DIR}/${APP_NAME}.app" 2>/dev/null \
    || echo "WARNING: ad-hoc codesign failed -- the app may refuse to launch"

echo ""
echo "Built: ${DIST_DIR}/${APP_NAME}.app"

# --- Install ----------------------------------------------------------------
if [[ ${INSTALL} -eq 1 ]]; then
    echo "==> Installing to /Applications"
    rm -rf "/Applications/${APP_NAME}.app"
    cp -R "${DIST_DIR}/${APP_NAME}.app" "/Applications/${APP_NAME}.app"
    echo "Installed: /Applications/${APP_NAME}.app"
fi
