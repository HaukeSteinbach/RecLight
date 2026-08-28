#!/usr/bin/env bash
# Double-click launcher for the RecLight Link board-flasher GUI, run straight
# from this checkout.
#
# For flashing boards in production, prefer the standalone app instead:
#   ./build_flasher_app.sh --install   ->  /Applications/RecLight Flasher.app
# That bundle needs neither Homebrew nor this repo on the machine using it.
#
# Uses a dedicated venv built on Homebrew's python-tk (modern Tcl/Tk 9).
# macOS's own /usr/bin/python3 ships an ancient, broken Tk 8.5 where windows
# render but buttons don't respond to clicks -- this venv avoids that.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "${SCRIPT_DIR}"

VENV_PY="${SCRIPT_DIR}/.venv/bin/python3"

if [[ ! -x "${VENV_PY}" ]]; then
    echo "Setting up the flasher tool's Python environment (one-time)..."
    BREW_PY="$(brew --prefix python-tk@3.13 2>/dev/null)/../python@3.13/bin/python3.13"
    if [[ ! -x "${BREW_PY}" ]]; then
        BREW_PY="$(brew --prefix)/opt/python@3.13/bin/python3.13"
    fi
    if [[ ! -x "${BREW_PY}" ]]; then
        echo "Installing Homebrew python-tk@3.13 (provides a working modern Tk)..."
        brew install python-tk@3.13
        BREW_PY="$(brew --prefix)/opt/python@3.13/bin/python3.13"
    fi
    "${BREW_PY}" -m venv "${SCRIPT_DIR}/.venv"
    "${VENV_PY}" -m pip install --quiet --upgrade pip esptool
fi

exec "${VENV_PY}" flash_gui.py
