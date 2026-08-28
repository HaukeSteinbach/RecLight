#!/usr/bin/env python3
"""
RecLight Link -- one-click board flasher.

Small GUI for production use: flashes the pre-built merged firmware image
(dist/reclight_link_merged.bin) onto an ESP32-C3 board and resets it,
so a whole batch of boards can be flashed back-to-back.

- "Flash & Reset" button flashes the currently selected serial port.
- "Auto-flash newly connected boards" checkbox (on by default): as soon as a
  new board is plugged in, it gets flashed and reset automatically -- just
  plug in the next board when the green "DONE" banner appears.

Requires only the Python standard library + esptool 5 or newer
(``pip3 install "esptool>=5"``).
Does NOT require the full ESP-IDF toolchain -- it flashes the already-built
merged binary, same as flash_esp.sh.

Runs both from source and as the bundled "RecLight Flasher.app" produced by
build_flasher_app.sh; see FROZEN / IMAGE_PATH / esptool_command() below for
the two places where those differ.
"""

import glob
import queue
import subprocess
import sys
import threading
import tkinter as tk
from pathlib import Path
from tkinter import ttk

# True when running inside the PyInstaller .app bundle rather than from source.
FROZEN = getattr(sys, "frozen", False)

SCRIPT_DIR = Path(__file__).resolve().parent


def _image_path():
    """Where the merged firmware image lives.

    From source it sits next to the firmware tree; inside the .app bundle
    PyInstaller unpacks it into sys._MEIPASS, so the app carries the exact
    image it was built with and never depends on the repo being present.
    """
    if FROZEN:
        return Path(getattr(sys, "_MEIPASS", SCRIPT_DIR)) / "reclight_link_merged.bin"
    return SCRIPT_DIR.parent / "dist" / "reclight_link_merged.bin"


IMAGE_PATH = _image_path()

# Marker argument used to re-enter this same binary as a plain esptool run.
# A frozen app has no "python -m esptool" to call -- sys.executable IS the
# app -- so the app relaunches itself with this flag and the branch at the top
# of main() hands straight over to esptool, before any Tk window exists.
ESPTOOL_WORKER_FLAG = "--esptool-worker"


def esptool_command(args):
    if FROZEN:
        return [sys.executable, ESPTOOL_WORKER_FLAG] + args
    return [sys.executable, "-m", "esptool"] + args

PORT_GLOBS = [
    "/dev/cu.usbmodem*",
    "/dev/cu.wchusbserial*",
    "/dev/cu.SLAB_USBtoUART*",
]

CHIP = "esp32c3"
BAUD = "460800"


def list_ports():
    ports = []
    for pattern in PORT_GLOBS:
        ports.extend(glob.glob(pattern))
    return sorted(set(ports))


class FlasherApp:
    def __init__(self, root):
        self.root = root
        root.title("RecLight Link -- Board Flasher")
        root.geometry("640x420")
        root.minsize(560, 360)

        self.flashing = False
        self.flashed_ports = set()  # ports flashed since they last appeared
        self.log_queue = queue.Queue()

        # --- Top: port selection -------------------------------------------------
        top = ttk.Frame(root, padding=10)
        top.pack(fill=tk.X)

        ttk.Label(top, text="Port:").pack(side=tk.LEFT)
        self.port_var = tk.StringVar()
        self.port_combo = ttk.Combobox(top, textvariable=self.port_var, width=28, state="readonly")
        self.port_combo.pack(side=tk.LEFT, padx=(4, 10))

        self.auto_var = tk.BooleanVar(value=True)
        ttk.Checkbutton(
            top, text="Auto-flash newly connected boards", variable=self.auto_var
        ).pack(side=tk.LEFT)

        # --- Status banner --------------------------------------------------------
        self.status_var = tk.StringVar(value="Plug in a board to begin.")
        self.status_label = tk.Label(
            root, textvariable=self.status_var, font=("Helvetica", 20, "bold"),
            bg="#444444", fg="white", pady=16,
        )
        self.status_label.pack(fill=tk.X)

        # --- Flash button -----------------------------------------------------
        btn_frame = ttk.Frame(root, padding=10)
        btn_frame.pack(fill=tk.X)
        self.flash_button = ttk.Button(btn_frame, text="Flash && Reset", command=self.on_flash_clicked)
        self.flash_button.pack(fill=tk.X, ipady=8)

        # --- Log ----------------------------------------------------------------
        log_frame = ttk.Frame(root, padding=(10, 0, 10, 10))
        log_frame.pack(fill=tk.BOTH, expand=True)
        self.log_text = tk.Text(log_frame, height=12, font=("Menlo", 11), state="disabled")
        self.log_text.pack(fill=tk.BOTH, expand=True, side=tk.LEFT)
        scroll = ttk.Scrollbar(log_frame, command=self.log_text.yview)
        scroll.pack(side=tk.RIGHT, fill=tk.Y)
        self.log_text.configure(yscrollcommand=scroll.set)

        if not IMAGE_PATH.exists():
            self.log(f"ERROR: firmware image not found: {IMAGE_PATH}")
            if FROZEN:
                # The image is baked into the bundle, so this can only mean a
                # broken build -- telling the user to run build_firmware.sh
                # would send them chasing a repo they may not even have.
                self.log("This copy of the app was built without a firmware image.")
                self.set_status("Broken app bundle -- rebuild the flasher", "#b00020")
            else:
                self.log("Build it first with: ./build_firmware.sh")
                self.set_status(f"Missing {IMAGE_PATH.name} -- build firmware first", "#b00020")
            self.flash_button.state(["disabled"])
        else:
            self.log(f"Using firmware image: {IMAGE_PATH}")

        self.refresh_ports(initial=True)
        self.root.after(200, self.poll_log_queue)

    # --- logging / status -----------------------------------------------------
    def log(self, line):
        self.log_text.configure(state="normal")
        self.log_text.insert(tk.END, line + "\n")
        self.log_text.see(tk.END)
        self.log_text.configure(state="disabled")

    def set_status(self, text, color):
        self.status_var.set(text)
        self.status_label.configure(bg=color)

    # --- port list / auto-detect ----------------------------------------------
    def refresh_ports(self, initial=False):
        current = list_ports()
        self.port_combo["values"] = current

        if current and self.port_var.get() not in current:
            self.port_var.set(current[-1])
        elif not current:
            self.port_var.set("")

        # Drop bookkeeping for ports that disappeared, so replugging the same
        # physical port path later is treated as "new" again.
        self.flashed_ports &= set(current)

        if not self.flashing:
            if not current:
                self.set_status("No board connected. Plug one in via USB.", "#444444")
            else:
                self.set_status("Ready.", "#444444")

        if self.auto_var.get() and not self.flashing:
            for port in current:
                if port not in self.flashed_ports:
                    self.port_var.set(port)
                    self.start_flash(port)
                    break

        self.root.after(700, self.refresh_ports)

    # --- flashing ---------------------------------------------------------------
    def on_flash_clicked(self):
        port = self.port_var.get()
        if not port:
            self.set_status("No port selected.", "#b00020")
            return
        self.start_flash(port)

    def start_flash(self, port):
        if self.flashing:
            return
        self.flashing = True
        self.flash_button.state(["disabled"])
        self.set_status(f"Flashing {port} ...", "#e0a800")
        self.log(f"\n==> Flashing {IMAGE_PATH.name} to {port}")
        threading.Thread(target=self._flash_worker, args=(port,), daemon=True).start()

    def _flash_worker(self, port):
        cmd = esptool_command([
            "--chip", CHIP,
            "-p", port,
            "-b", BAUD,
            # Dashed spellings: esptool 5 deprecated the underscore forms
            # and warns loudly about each one, which buried the actual
            # progress output in the log window.
            "--before", "default-reset",
            "--after", "hard-reset",
            "write-flash",
            "--flash-mode", "dio",
            "--flash-freq", "80m",
            "--flash-size", "2MB",
            "0x0", str(IMAGE_PATH),
        ])
        try:
            # Each flash runs in its own process, so a board that wedges
            # esptool mid-write can't take the GUI down with it -- that
            # matters when flashing a batch back to back.
            proc = subprocess.Popen(
                cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                text=True, bufsize=1,
            )
            for line in proc.stdout:
                self.log_queue.put(("line", line.rstrip()))
            code = proc.wait()
        except Exception as exc:  # esptool missing, permissions, etc.
            self.log_queue.put(("line", f"ERROR: {exc}"))
            code = 1

        self.log_queue.put(("done", (port, code)))

    def poll_log_queue(self):
        try:
            while True:
                kind, payload = self.log_queue.get_nowait()
                if kind == "line":
                    self.log(payload)
                elif kind == "done":
                    port, code = payload
                    self.flashed_ports.add(port)
                    self.flashing = False
                    self.flash_button.state(["!disabled"])
                    if code == 0:
                        self.log(f"==> DONE. {port} flashed and reset.\n")
                        self.set_status("DONE -- unplug and connect next board", "#2e7d32")
                    else:
                        self.log(f"==> FAILED (exit code {code}).\n")
                        self.set_status(f"FAILED (exit code {code}) -- see log", "#b00020")
        except queue.Empty:
            pass
        self.root.after(200, self.poll_log_queue)


def main():
    # Must come before anything touches Tk: in the frozen app this process is
    # a plain esptool run spawned by the GUI, not a second GUI.
    if len(sys.argv) > 1 and sys.argv[1] == ESPTOOL_WORKER_FLAG:
        import esptool
        sys.exit(esptool.main(sys.argv[2:]))

    root = tk.Tk()
    FlasherApp(root)
    root.mainloop()


if __name__ == "__main__":
    main()
