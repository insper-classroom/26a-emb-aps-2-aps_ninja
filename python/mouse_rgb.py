#!/usr/bin/env python3
"""
GUI para controlar o mouse wireless via Bluetooth (HC-06) e
configurar o LED RGB na Pico.

Protocolo:
  Pico -> PC (mouse): axis | val_lo | val_hi | 0xFF  (4 bytes, sync = 0xFF)
  PC   -> Pico (RGB): 'R'/'G'/'B' + valor (0-255)    (2 bytes)
"""

import sys
import glob
import threading

import serial
import serial.tools.list_ports
import pyautogui
pyautogui.PAUSE = 0
pyautogui.FAILSAFE = False

import tkinter as tk
from tkinter import ttk, messagebox


# ---------------------------------------------------------------------------
# Protocolo serial
# ---------------------------------------------------------------------------
SYNC_BYTE = 0xFF


def parse_axis(axis_byte, value_bytes):
    """Decodifica 1 byte de eixo + 2 bytes de valor (little-endian, signed)."""
    axis = axis_byte
    value = int.from_bytes(value_bytes, byteorder='little', signed=True)
    return axis, value


def move_mouse(axis, value):
    if axis == 0:
        pyautogui.moveRel(value, 0)
    elif axis == 1:
        pyautogui.moveRel(0, value)


def read_loop(ser, status_cb):
    """Lê pacotes do HC-06 e move o mouse."""
    while True:
        try:
            sync = ser.read(1)
            if not sync:
                continue
            if sync[0] != SYNC_BYTE:
                continue
            data = ser.read(3)
            if len(data) < 3:
                continue
            axis, value = parse_axis(data[0], data[1:3])
            if axis in (0, 1):
                move_mouse(axis, value)
        except (serial.SerialException, OSError) as e:
            status_cb(f"Conexão perdida: {e}", "red")
            return


# ---------------------------------------------------------------------------
# Portas seriais
# ---------------------------------------------------------------------------
def serial_ports():
    return [p.device for p in serial.tools.list_ports.comports()]


# ---------------------------------------------------------------------------
# GUI
# ---------------------------------------------------------------------------
class App:
    BG = "#1e1e1e"
    FG = "#d4d4d4"
    ACCENT = "#007acc"
    BG_ENTRY = "#2a2a2a"

    def __init__(self, root):
        self.root = root
        self.ser = None
        self.reader = None

        root.title("Mouse Wireless + RGB")
        root.geometry("420x420")
        root.configure(bg=self.BG)
        root.resizable(False, False)

        self._build_ui()

    def _build_ui(self):
        style = ttk.Style(self.root)
        style.theme_use("clam")
        style.configure("TLabel", background=self.BG, foreground=self.FG)
        style.configure("TFrame", background=self.BG)
        style.configure("TButton", background="#444", foreground=self.FG,
                        font=("Segoe UI", 10, "bold"), borderwidth=0)
        style.map("TButton", background=[("active", "#555")])
        style.configure("Accent.TButton", background=self.ACCENT, foreground="white",
                        font=("Segoe UI", 11, "bold"), padding=6)
        style.map("Accent.TButton", background=[("active", "#005f9e")])
        style.configure("TCombobox", fieldbackground=self.BG_ENTRY,
                        background=self.BG_ENTRY, foreground=self.FG)

        title = ttk.Label(self.root, text="Mouse Wireless + RGB",
                          font=("Segoe UI", 14, "bold"))
        title.pack(pady=(12, 6))

        # --- conexão ---
        conn = ttk.Frame(self.root, padding=10)
        conn.pack(fill="x", padx=12)

        ttk.Label(conn, text="Porta COM:").grid(row=0, column=0, sticky="w", pady=4)
        self.port_var = tk.StringVar()
        self.port_cb = ttk.Combobox(conn, textvariable=self.port_var,
                                    values=serial_ports(), state="readonly",
                                    width=18)
        self.port_cb.grid(row=0, column=1, padx=4, pady=4)
        ttk.Button(conn, text="↻", width=3, command=self._refresh_ports).grid(
            row=0, column=2, padx=4)

        self.btn_connect = ttk.Button(conn, text="Conectar",
                                      style="Accent.TButton",
                                      command=self._toggle_connection)
        self.btn_connect.grid(row=0, column=3, padx=8)

        self.status_var = tk.StringVar(value="Desconectado")
        self.status_label = ttk.Label(self.root, textvariable=self.status_var,
                                      foreground="#ef5350")
        self.status_label.pack(pady=4)

        # --- RGB ---
        rgb_frame = ttk.LabelFrame(self.root, text=" LED RGB ", padding=12)
        rgb_frame.pack(fill="x", padx=12, pady=10)

        self.rgb_vars = {}
        for i, (cmd, label, color) in enumerate([
            ('R', "Vermelho", "#ff5252"),
            ('G', "Verde",    "#69f0ae"),
            ('B', "Azul",     "#448aff"),
        ]):
            ttk.Label(rgb_frame, text=label, foreground=color,
                      font=("Segoe UI", 10, "bold")).grid(
                row=i, column=0, sticky="w", padx=4, pady=4)

            var = tk.IntVar(value=0)
            self.rgb_vars[cmd] = var

            scale = tk.Scale(rgb_frame, from_=0, to=255, orient="horizontal",
                             length=220, bg=self.BG, fg=self.FG,
                             troughcolor="#333", highlightthickness=0,
                             activebackground=color, sliderlength=18,
                             showvalue=False, variable=var,
                             command=lambda v, c=cmd: self._send_rgb(c, int(float(v))))
            scale.grid(row=i, column=1, padx=4, pady=4)

            val_label = ttk.Label(rgb_frame, textvariable=var, width=4)
            val_label.grid(row=i, column=2, padx=4)

        ttk.Button(rgb_frame, text="Apagar tudo",
                   command=self._all_off).grid(row=3, column=0,
                                                columnspan=3, pady=(8, 0))

        # --- info ---
        info = ttk.Label(self.root,
                         text="Mova o joystick para controlar o mouse.\n"
                              "Use os sliders para alterar a cor do LED.",
                         justify="center")
        info.pack(pady=(4, 0))

    # ------------------------------------------------------------------
    # Conexão
    # ------------------------------------------------------------------
    def _refresh_ports(self):
        ports = serial_ports()
        self.port_cb["values"] = ports
        if ports and not self.port_var.get():
            self.port_var.set(ports[0])

    def _toggle_connection(self):
        if self.ser and self.ser.is_open:
            self._disconnect()
        else:
            self._connect()

    def _connect(self):
        port = self.port_var.get()
        if not port:
            messagebox.showwarning("Aviso", "Selecione uma porta serial.")
            return
        try:
            self.ser = serial.Serial(port, 115200, timeout=0.1)
            self._set_status(f"Conectado em {port}", "#4caf50")
            self.btn_connect.config(text="Desconectar")
            self.reader = threading.Thread(target=read_loop,
                                           args=(self.ser, self._set_status),
                                           daemon=True)
            self.reader.start()
        except Exception as e:
            messagebox.showerror("Erro", f"Falha ao conectar: {e}")
            self._set_status("Erro de conexão", "#ef5350")

    def _disconnect(self):
        if self.ser:
            try:
                self.ser.close()
            except Exception:
                pass
            self.ser = None
        self._set_status("Desconectado", "#ef5350")
        self.btn_connect.config(text="Conectar")

    def _set_status(self, text, color):
        def _do():
            self.status_var.set(text)
            self.status_label.config(foreground=color)
        # Thread-safe atualização da UI
        self.root.after(0, _do)

    # ------------------------------------------------------------------
    # Envio RGB
    # ------------------------------------------------------------------
    def _send_rgb(self, cmd, value):
        if not self.ser or not self.ser.is_open:
            return
        try:
            self.ser.write(bytes([ord(cmd), value & 0xFF]))
        except serial.SerialException as e:
            self._set_status(f"Erro envio: {e}", "#ef5350")

    def _all_off(self):
        for cmd, var in self.rgb_vars.items():
            var.set(0)
            self._send_rgb(cmd, 0)


def main():
    root = tk.Tk()
    app = App(root)
    root.protocol("WM_DELETE_WINDOW", lambda: (app._disconnect(), root.destroy()))
    root.mainloop()


if __name__ == "__main__":
    main()
