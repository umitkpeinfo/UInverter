"""
PE Operator Software — SVPWM Monitor
======================================

Company : PE Info
Author  : Umit Kayacik
Date    : 2026
Version : 5.0.0

USB CDC serial monitor for 6-channel SVPWM inverter (UltraLogic R1).

Firmware command set:
  SET:FREQ:<hz>              output frequency 1-400 Hz
  SET:SWF:<hz>               switching frequency 1000-16000 Hz
  SET:MOD:<0-1155>           modulation index per-mille
  SET:SVPWM:0                emergency stop (disable PWM, open relay, stop drive)
  SET:CHG:STOP|CLEAR         stop drive / full drive reset
  SET:DRV:START|RUN|STOP|RESET   drive state machine
  SET:DISP:<text>            send text to front-panel display
  GET:REG                    read TIM1 registers
  GET:DRV                    read drive state + measurements + diag code
  GET:BTN                    read front-panel button state
  GET:DIAG                   read diagnostic code + fault history

Firmware telemetry (periodic, no request needed):
  $DRV,S:<state>,F:<hex>,V:<volts>,IU:<amps>,IW:<amps>,
       S1:<raw>,S3:<raw>,BDTR:<hex>,MOE:<0|1>,CT:<0|1>,DC:<code>
  $VBUS,RAW:<raw>,V:<volts>

Dependencies: pip install PySide6 pyserial

Copyright (c) 2026 PE Info.  All rights reserved.
"""

from __future__ import annotations

import sys
from datetime import datetime
from pathlib import Path
from typing import Optional

from PySide6.QtWidgets import (
    QApplication, QMainWindow, QWidget, QVBoxLayout, QHBoxLayout,
    QLabel, QPushButton, QComboBox, QFrame, QTextEdit, QSizePolicy,
    QSpinBox, QLineEdit, QGroupBox, QGridLayout,
)
from PySide6.QtCore import Qt, QThread, Signal, QRect
from PySide6.QtGui import (
    QFont, QColor, QPainter, QPen, QBrush, QPixmap, QIcon,
    QRadialGradient, QKeySequence, QShortcut, QTextCharFormat,
)
import serial
import serial.tools.list_ports


# ═══════════════════════════════════════════════════════════════════════════
#  Theme
# ═══════════════════════════════════════════════════════════════════════════

PE_BLUE_DARK   = "#0A3D6B"
PE_BLUE        = "#0D6EBF"
PE_BLUE_LIGHT  = "#4DA3E8"

BG_PRIMARY     = "#1E2430"
BG_SECONDARY   = "#263040"
BG_INPUT       = "#354558"
BG_HOVER       = "#3E5068"

FG_PRIMARY     = "#E6EDF3"
FG_DIM         = "#6B7785"

ACCENT_GREEN   = "#3FB950"
ACCENT_RED     = "#F85149"
ACCENT_YELLOW  = "#E3B341"
ACCENT_CYAN    = "#39D2C0"
BORDER_COLOR   = "#445060"
BORDER_FOCUS   = "#58A6FF"

MONO = "font-family: 'Consolas', monospace;"

# ═══════════════════════════════════════════════════════════════════════════
#  Constants
# ═══════════════════════════════════════════════════════════════════════════

APP_NAME    = "SVPWM Monitor"
APP_VERSION = "5.0.0"
APP_ORG     = "PE Info"

VBUS_OV_WARN_V  = 380.0
VBUS_CHARGED_V  = 50.0

OUT_FREQ_MIN, OUT_FREQ_MAX, OUT_FREQ_DEF, OUT_FREQ_STEP = 1, 400, 60, 5
SW_FREQ_MIN, SW_FREQ_MAX, SW_FREQ_DEF, SW_FREQ_STEP     = 1000, 16000, 5000, 500
MOD_IDX_MIN, MOD_IDX_MAX, MOD_IDX_DEF, MOD_IDX_STEP     = 0, 1155, 850, 10

FAULT_NAMES = {
    0x0001: "OVERCURRENT",
    0x0002: "OVERVOLTAGE",
    0x0004: "UNDERVOLTAGE",
    0x0008: "BUS COLLAPSE",
    0x0010: "PRECHARGE",
}

DIAG_CODES: dict[int, tuple[str, str]] = {
    0:  ("---",       "No fault"),
    1:  ("F-01 OC/HW", "Overcurrent — BKIN hardware trip on PE15"),
    2:  ("F-02 OC/SW", "Overcurrent — CUR_TRIP stuck LOW on PD10"),
    3:  ("F-03 OV",    "Overvoltage — bus exceeded 420 V (3 readings)"),
    4:  ("F-04 UV",    "Undervoltage — bus below 30 V during RUN"),
    5:  ("F-05 BUS",   "Bus collapse — bus below 30 V during READY"),
    6:  ("F-06 PCHG",  "Precharge timeout — sequence exceeded 5 s"),
    7:  ("F-07 PCOL",  "Precharge collapse — bus dropped after relay close"),
    8:  ("F-08 NCHG",  "No charge — no voltage rise during precharge"),
    9:  ("F-09 ADC",   "ADC init failure — injected channel setup failed"),
    10: ("F-10 POV",   "Precharge overvoltage — bus exceeded limit"),
    50: ("d-50 REGN",  "Regen active — brake chopper engaged"),
}

_DRV_STATE_STYLE = {
    "IDLE":  (FG_DIM,       "IDLE"),
    "PRCHG": (ACCENT_YELLOW, "PRE-CHARGE"),
    "READY": (ACCENT_CYAN,   "READY"),
    "RUN":   (ACCENT_GREEN,  "RUNNING"),
    "STOP":  (ACCENT_YELLOW, "STOPPING"),
    "FAULT": (ACCENT_RED,    "FAULT"),
}

GLOBAL_STYLE = f"""
QMainWindow, QWidget {{
    background-color: {BG_PRIMARY}; color: {FG_PRIMARY};
    font-family: "Segoe UI", "Roboto", "Arial", sans-serif; font-size: 12pt;
}}
QPushButton {{
    background-color: {BG_INPUT}; color: {FG_PRIMARY};
    border: 1px solid {BORDER_COLOR}; border-radius: 6px;
    padding: 10px 20px; font-weight: bold; font-size: 11pt;
}}
QPushButton:hover {{ background-color: {BG_HOVER}; border-color: {FG_DIM}; }}
QPushButton:pressed {{ background-color: {PE_BLUE_DARK}; }}
QPushButton#primaryBtn {{
    background-color: {PE_BLUE}; color: white; border: none;
    padding: 12px 26px; font-size: 12pt;
}}
QPushButton#primaryBtn:hover {{ background-color: {PE_BLUE_LIGHT}; }}
QComboBox {{
    background-color: {BG_INPUT}; color: {FG_PRIMARY};
    border: 1px solid {BORDER_COLOR}; border-radius: 6px;
    padding: 8px 14px; font-size: 12pt; min-width: 140px;
}}
QComboBox QAbstractItemView {{
    background-color: {BG_SECONDARY}; color: {FG_PRIMARY};
    border: 1px solid {BORDER_COLOR}; selection-background-color: {PE_BLUE};
}}
QSpinBox {{
    background-color: {BG_INPUT}; color: {FG_PRIMARY};
    border: 1px solid {BORDER_COLOR}; border-radius: 6px;
    padding: 6px 10px; {MONO} font-size: 13pt; font-weight: bold;
    min-width: 90px;
}}
QSpinBox:focus {{ border-color: {BORDER_FOCUS}; }}
QSpinBox::up-button, QSpinBox::down-button {{
    width: 20px; border: none; background: {BG_HOVER};
}}
QTextEdit {{
    background-color: {BG_INPUT}; color: {FG_DIM};
    border: 1px solid {BORDER_COLOR}; border-radius: 6px;
    padding: 8px; {MONO} font-size: 10pt;
}}
QGroupBox {{
    background-color: {BG_SECONDARY}; color: {FG_PRIMARY};
    border: 1px solid {BORDER_COLOR}; border-radius: 8px;
    padding: 12px; margin-top: 14px; font-weight: bold; font-size: 10pt;
}}
QGroupBox::title {{
    subcontrol-origin: margin; subcontrol-position: top left;
    padding: 0 6px; color: {PE_BLUE_LIGHT};
}}
QStatusBar {{
    background-color: {PE_BLUE_DARK}; color: {FG_PRIMARY};
    font-size: 11pt; border-top: 1px solid {BORDER_COLOR};
}}
"""

# ═══════════════════════════════════════════════════════════════════════════
#  Logo
# ═══════════════════════════════════════════════════════════════════════════

_LOGO_FILE = Path(__file__).parent / "PE_logo_MHI_Microsite1-002.png"
_logo_cache: Optional[QPixmap] = None


def _load_logo() -> Optional[QPixmap]:
    global _logo_cache
    if _logo_cache is not None:
        return _logo_cache
    if _LOGO_FILE.exists():
        pm = QPixmap(str(_LOGO_FILE))
        if not pm.isNull():
            _logo_cache = pm
            return pm
    return None


def _make_logo(size: int = 128) -> QPixmap:
    src = _load_logo()
    if src is not None:
        return src.scaledToHeight(size, Qt.SmoothTransformation)
    px = QPixmap(size, size)
    px.fill(Qt.transparent)
    p = QPainter(px)
    p.setRenderHint(QPainter.Antialiasing, True)
    g = QRadialGradient(size / 2, size / 2, size / 2)
    g.setColorAt(0.0, QColor(PE_BLUE))
    g.setColorAt(1.0, QColor(PE_BLUE_DARK))
    p.setBrush(QBrush(g))
    p.setPen(QPen(QColor(PE_BLUE_LIGHT), 2))
    m = 4
    p.drawRoundedRect(m, m, size - 2*m, size - 2*m, size*0.22, size*0.22)
    p.setFont(QFont("Segoe UI", int(size * 0.32), QFont.Bold))
    p.setPen(QColor("#FFFFFF"))
    p.drawText(QRect(0, 0, size, size), Qt.AlignCenter, "PE")
    p.end()
    return px


def _app_icon() -> QIcon:
    icon = QIcon()
    src = _load_logo()
    for sz in (16, 32, 48, 64, 128):
        pm = src.scaledToHeight(sz, Qt.SmoothTransformation) if src else _make_logo(sz)
        icon.addPixmap(pm)
    return icon


# ═══════════════════════════════════════════════════════════════════════════
#  Helpers
# ═══════════════════════════════════════════════════════════════════════════

def _parse_kv(payload: str) -> dict[str, str]:
    """Parse 'K:V,K:V,...' telemetry into a dict."""
    result: dict[str, str] = {}
    for pair in payload.split(","):
        if ":" in pair:
            k, v = pair.split(":", 1)
            result[k] = v
    return result


def _decode_faults(fault_val: int) -> str:
    """Return comma-separated fault names for a fault bitmask."""
    if fault_val == 0:
        return ""
    names = [name for bit, name in FAULT_NAMES.items() if fault_val & bit]
    if not names:
        return f"0x{fault_val:04X}"
    return ", ".join(names)


def _vbus_color(volts: float) -> str:
    if volts > VBUS_OV_WARN_V:
        return ACCENT_RED
    if volts > VBUS_CHARGED_V:
        return ACCENT_GREEN
    return FG_DIM


# ═══════════════════════════════════════════════════════════════════════════
#  Serial Worker
# ═══════════════════════════════════════════════════════════════════════════

class SerialWorker(QThread):
    line_received   = Signal(str)
    error_occurred  = Signal(str)
    connection_lost = Signal()

    def __init__(self, port: str, baudrate: int = 115200) -> None:
        super().__init__()
        self.port_name = port
        self.baudrate = baudrate
        self._running = False
        self.serial_port: Optional[serial.Serial] = None

    def run(self) -> None:
        try:
            self.serial_port = serial.Serial(
                self.port_name, self.baudrate, timeout=0.1)
            self._running = True
        except serial.SerialException as e:
            self.error_occurred.emit(str(e))
            return

        buf = ""
        while self._running:
            try:
                raw = self.serial_port.read(self.serial_port.in_waiting or 1)
                if not raw:
                    continue
                buf += raw.decode("utf-8", errors="ignore")
                while "\n" in buf:
                    line, buf = buf.split("\n", 1)
                    line = line.strip()
                    if line:
                        self.line_received.emit(line)
            except serial.SerialException:
                if self._running:
                    self.connection_lost.emit()
                break
            except Exception:
                continue

        if self.serial_port and self.serial_port.is_open:
            self.serial_port.close()

    def send_command(self, cmd: str) -> None:
        if self.serial_port and self.serial_port.is_open:
            try:
                self.serial_port.write((cmd + "\r\n").encode())
            except serial.SerialException:
                pass

    def stop(self) -> None:
        self._running = False
        self.wait(2000)


# ═══════════════════════════════════════════════════════════════════════════
#  Widgets
# ═══════════════════════════════════════════════════════════════════════════

class StatusDot(QWidget):
    def __init__(self, color: str = FG_DIM, size: int = 12,
                 parent: Optional[QWidget] = None) -> None:
        super().__init__(parent)
        self._color = QColor(color)
        self._sz = size
        self.setFixedSize(size, size)

    def set_color(self, color: str) -> None:
        self._color = QColor(color)
        self.update()

    def paintEvent(self, _) -> None:
        p = QPainter(self)
        p.setRenderHint(QPainter.Antialiasing)
        p.setPen(Qt.NoPen)
        p.setBrush(self._color)
        p.drawEllipse(1, 1, self._sz - 2, self._sz - 2)
        p.end()


# ═══════════════════════════════════════════════════════════════════════════
#  Main Window
# ═══════════════════════════════════════════════════════════════════════════

class MainWindow(QMainWindow):

    def __init__(self) -> None:
        super().__init__()
        self.setWindowTitle(f"{APP_NAME}  —  UltraLogic R1")
        self.setWindowIcon(_app_icon())
        self.setMinimumSize(880, 560)
        self.resize(1100, 660)

        self._worker: Optional[SerialWorker] = None

        self._build_toolbar()
        self._build_central()
        self._build_statusbar()
        self._bind_shortcuts()

    # ── Keyboard Shortcuts ────────────────────────────────────────────

    def _bind_shortcuts(self) -> None:
        esc = QShortcut(QKeySequence(Qt.Key_Escape), self)
        esc.activated.connect(self._emergency_stop)

    def _emergency_stop(self) -> None:
        if self._worker:
            self._worker.send_command("SET:SVPWM:0")
            self.statusBar().showMessage("  EMERGENCY STOP sent (Esc)", 5000)

    # ── Toolbar ──────────────────────────────────────────────────────

    def _build_toolbar(self) -> None:
        tb = self.addToolBar("Connection")
        tb.setMovable(False)
        tb.setStyleSheet(
            f"QToolBar {{ background: {BG_SECONDARY};"
            f"border-bottom: 1px solid {BORDER_COLOR}; padding: 4px; }}")

        logo = QLabel()
        logo.setPixmap(_make_logo(36))
        logo.setStyleSheet("background: transparent; padding: 4px;")
        tb.addWidget(logo)
        tb.addWidget(QLabel("  "))

        self._port_combo = QComboBox()
        self._port_combo.setMinimumWidth(200)
        self._refresh_ports()
        tb.addWidget(self._port_combo)

        refresh_btn = QPushButton("Refresh")
        refresh_btn.setCursor(Qt.PointingHandCursor)
        refresh_btn.clicked.connect(self._refresh_ports)
        tb.addWidget(refresh_btn)

        self._connect_btn = QPushButton("Connect")
        self._connect_btn.setObjectName("primaryBtn")
        self._connect_btn.setCursor(Qt.PointingHandCursor)
        self._connect_btn.clicked.connect(self._toggle_connection)
        tb.addWidget(self._connect_btn)

        self._conn_dot = StatusDot(FG_DIM, 14)
        tb.addWidget(self._conn_dot)
        self._conn_label = QLabel("  Disconnected")
        self._conn_label.setStyleSheet(f"color: {FG_DIM}; font-size: 11pt;")
        tb.addWidget(self._conn_label)

        spacer = QWidget()
        spacer.setSizePolicy(QSizePolicy.Expanding, QSizePolicy.Preferred)
        tb.addWidget(spacer)

        info = QLabel("6-ch SVPWM  ·  TIM1  ·  PE8-PE13  ·  2-Level VSI")
        info.setStyleSheet(f"color: {FG_DIM}; font-size: 10pt; {MONO}")
        tb.addWidget(info)

    def _refresh_ports(self) -> None:
        self._port_combo.clear()
        ports = serial.tools.list_ports.comports()
        for p in sorted(ports, key=lambda x: x.device):
            self._port_combo.addItem(f"{p.device}  —  {p.description}", p.device)
        if not ports:
            self._port_combo.addItem("No ports found", "")

    # ── Connection ───────────────────────────────────────────────────

    def _toggle_connection(self) -> None:
        if self._worker:
            self._disconnect()
        else:
            self._connect()

    def _connect(self) -> None:
        port = self._port_combo.currentData()
        if not port:
            return
        w = SerialWorker(port)
        w.line_received.connect(self._on_line)
        w.error_occurred.connect(self._on_error)
        w.connection_lost.connect(self._disconnect)
        w.start()
        self._worker = w

        self._conn_dot.set_color(ACCENT_GREEN)
        self._conn_label.setText(f"  Connected: {port}")
        self._conn_label.setStyleSheet(f"color: {ACCENT_GREEN}; font-size: 11pt;")
        self._connect_btn.setText("Disconnect")
        self.statusBar().showMessage(f"  Connected to {port}", 3000)

    def _disconnect(self) -> None:
        if self._worker:
            self._worker.stop()
            self._worker = None
        self._conn_dot.set_color(FG_DIM)
        self._conn_label.setText("  Disconnected")
        self._conn_label.setStyleSheet(f"color: {FG_DIM}; font-size: 11pt;")
        self._connect_btn.setText("Connect")
        self.statusBar().showMessage("  Disconnected", 3000)

    # ── Serial Data Handling ─────────────────────────────────────────

    def _on_line(self, line: str) -> None:
        self._log_line(line)

        if line.startswith("$VBUS,"):
            self._parse_vbus(line)
        elif line.startswith("$DRV,"):
            self._parse_drv(line)
        elif line.startswith("$BTN,"):
            self._parse_btn(line)
        elif line.startswith("$DIAG,"):
            self._parse_diag(line)

    def _parse_vbus(self, line: str) -> None:
        try:
            parts = _parse_kv(line[6:])
            raw = int(parts.get("RAW", "0"))
            volts = float(parts.get("V", "0"))
        except (ValueError, KeyError):
            return
        self._update_vbus_display(volts, raw)

    def _parse_drv(self, line: str) -> None:
        try:
            parts = _parse_kv(line[5:])
            state = parts.get("S", "IDLE")
            fault_hex = parts.get("F", "0000")
            volts = float(parts.get("V", "0"))
            i_u = float(parts.get("IU", "0"))
            i_w = float(parts.get("IW", "0"))
            moe = parts.get("MOE", "")
            cur_trip = parts.get("CT", "0")
            diag_code = int(parts.get("DC", "0"))
            fault_val = int(fault_hex, 16) if fault_hex else 0
        except (ValueError, KeyError):
            return

        color, label = _DRV_STATE_STYLE.get(state, (FG_DIM, state))
        if fault_val != 0:
            label = f"FAULT: {_decode_faults(fault_val)}"
            color = ACCENT_RED
        self._drv_state_lbl.setText(label)
        self._drv_state_lbl.setStyleSheet(
            f"color: {color}; font-size: 12pt; font-weight: bold; {MONO}")

        short, desc = DIAG_CODES.get(diag_code, (f"F-{diag_code:02d}", "Unknown"))
        if diag_code != 0:
            self._diag_lbl.setText(f"{short}  —  {desc}")
            diag_color = ACCENT_RED if diag_code < 50 else ACCENT_YELLOW
            self._diag_lbl.setStyleSheet(
                f"color: {diag_color}; font-size: 9pt; {MONO}")
        else:
            self._diag_lbl.setText("")
            self._diag_lbl.setStyleSheet("")

        moe_on = moe == "1"
        self._moe_lbl.setText(f"MOE: {'ON' if moe_on else 'OFF'}")
        self._moe_lbl.setStyleSheet(
            f"color: {ACCENT_GREEN if moe_on else FG_DIM};"
            f"font-size: 10pt; font-weight: bold; {MONO}")

        tripped = cur_trip == "1"
        if tripped:
            self._ct_lbl.setText("CUR_TRIP ACTIVE")
            self._ct_lbl.setStyleSheet(
                f"color: white; background: {ACCENT_RED};"
                f"font-size: 9pt; font-weight: bold; {MONO}"
                f"padding: 2px 6px; border-radius: 3px;")
        else:
            self._ct_lbl.setText("")
            self._ct_lbl.setStyleSheet("")

        self._shunt_lbl.setText(f"IU: {i_u:+.2f} A    IW: {i_w:+.2f} A")
        shunt_color = ACCENT_GREEN if state == "RUN" else FG_DIM
        self._shunt_lbl.setStyleSheet(
            f"color: {shunt_color}; font-size: 10pt; {MONO}")

        self._update_vbus_display(volts)

    def _parse_btn(self, line: str) -> None:
        try:
            parts = _parse_kv(line[5:])
            raw = parts.get("RAW", "00")
            scr = parts.get("SCR", "0")
            inc = parts.get("INC", "0")
            dec = parts.get("DEC", "0")
        except (ValueError, KeyError):
            return
        self._btn_lbl.setText(f"RAW:0x{raw}  SCR:{scr}  INC:{inc}  DEC:{dec}")

    def _parse_diag(self, line: str) -> None:
        """Parse $DIAG response and display decoded fault history in the log."""
        try:
            parts = _parse_kv(line[6:])
            dc = int(parts.get("DC", "0"))
            count = int(parts.get("N", "0"))
        except (ValueError, KeyError):
            return

        short, desc = DIAG_CODES.get(dc, (f"F-{dc:02d}", "Unknown"))
        self._log_line(f"--- DIAGNOSTIC REPORT ---")
        self._log_line(f"Active: {short} — {desc}")
        self._log_line(f"History ({count} entries):")

        for i in range(count):
            key = f"H{i}"
            val = parts.get(key, "")
            if "@" in val:
                code_s, tick_s = val.split("@", 1)
                code = int(code_s)
                tick = int(tick_s)
                s, d = DIAG_CODES.get(code, (f"F-{code:02d}", "Unknown"))
                secs = tick / 1000.0
                self._log_line(f"  [{i}] {s} at {secs:.1f}s — {d}")

        self._log_line(f"--- END REPORT ---")

    def _update_vbus_display(self, volts: float, raw: Optional[int] = None) -> None:
        color = _vbus_color(volts)
        self._vbus_value.setText(f"{volts:.0f}")
        self._vbus_value.setStyleSheet(
            f"color: {color}; font-size: 24pt; font-weight: bold; {MONO}")
        if raw is not None:
            self._vbus_raw.setText(f"RAW {raw}")

    # ── Command Handlers ─────────────────────────────────────────────

    def _send_cmd(self, cmd: str) -> None:
        if not self._worker:
            self.statusBar().showMessage("  Not connected", 3000)
            return
        self._worker.send_command(cmd)
        self.statusBar().showMessage(f"  Sent: {cmd}", 3000)

    def _send_params(self) -> None:
        if not self._worker:
            self.statusBar().showMessage("  Not connected", 3000)
            return
        self._worker.send_command(f"SET:FREQ:{self._spin_freq.value()}")
        self._worker.send_command(f"SET:SWF:{self._spin_swf.value()}")
        self._worker.send_command(f"SET:MOD:{self._spin_mod.value()}")
        self.statusBar().showMessage(
            f"  Sent: {self._spin_freq.value()} Hz  /  "
            f"{self._spin_swf.value()} Hz  /  "
            f"{self._spin_mod.value()}\u2030", 3000)

    def _send_display_text(self) -> None:
        text = self._disp_input.text().strip()
        if text:
            self._send_cmd(f"SET:DISP:{text}")

    def _poll_buttons(self) -> None:
        self._send_cmd("GET:BTN")

    def _send_raw_cmd(self) -> None:
        cmd = self._cmd_input.text().strip()
        if not cmd:
            return
        if not self._worker:
            self.statusBar().showMessage("  Not connected", 3000)
            return
        self._worker.send_command(cmd)
        self._log_line(f">>> {cmd}")
        self._cmd_input.clear()
        self.statusBar().showMessage(f"  Sent: {cmd}", 3000)

    def _on_error(self, msg: str) -> None:
        self.statusBar().showMessage(f"  Serial error: {msg}", 5000)
        self._disconnect()

    # ── Log ───────────────────────────────────────────────────────────

    def _log_line(self, text: str) -> None:
        ts = datetime.now().strftime("%H:%M:%S.") + f"{datetime.now().microsecond // 1000:03d}"
        color = FG_DIM
        if text.startswith("$DRV,"):
            color = PE_BLUE_LIGHT
        elif text.startswith("$VBUS,"):
            color = ACCENT_CYAN
        elif text.startswith("$BTN,"):
            color = ACCENT_YELLOW
        elif text.startswith("---") or text.startswith("  ["):
            color = ACCENT_RED
        elif text.startswith("Active:") or text.startswith("History"):
            color = ACCENT_YELLOW
        elif text.startswith(">>>"):
            color = ACCENT_GREEN

        self._log.append(
            f'<span style="color:{FG_DIM}">{ts}</span>  '
            f'<span style="color:{color}">{text}</span>')
        sb = self._log.verticalScrollBar()
        sb.setValue(sb.maximum())

    # ── Central Widget ───────────────────────────────────────────────

    def _build_central(self) -> None:
        central = QWidget()
        self.setCentralWidget(central)
        lo = QVBoxLayout(central)
        lo.setContentsMargins(12, 8, 12, 8)
        lo.setSpacing(10)

        header = QLabel(
            "  SVPWM  ·  3 Complementary Pairs  ·  TIM1  ·  PE8-PE13"
            "    |    Esc = Emergency Stop")
        header.setStyleSheet(
            f"background: {BG_SECONDARY}; color: {PE_BLUE_LIGHT};"
            f"font-size: 11pt; font-weight: bold; padding: 8px 12px;"
            f"border-radius: 4px; {MONO}")
        header.setFixedHeight(32)
        lo.addWidget(header)

        panels = QHBoxLayout()
        panels.setSpacing(10)
        panels.addLayout(self._build_drive_panel(), 3)
        panels.addLayout(self._build_param_panel(), 3)
        panels.addLayout(self._build_aux_panel(), 2)
        lo.addLayout(panels)

        self._build_log_panel(lo)

    # ── Left Panel: Drive Control + DC Bus ────────────────────────────

    def _build_drive_panel(self) -> QVBoxLayout:
        left = QVBoxLayout()
        left.setSpacing(10)

        drv_group = QGroupBox("DRIVE STATE MACHINE")
        drv_lo = QVBoxLayout(drv_group)
        drv_lo.setSpacing(6)

        self._drv_state_lbl = QLabel("IDLE")
        self._drv_state_lbl.setStyleSheet(
            f"color: {FG_DIM}; font-size: 14pt; font-weight: bold; {MONO}")
        self._drv_state_lbl.setAlignment(Qt.AlignCenter)
        drv_lo.addWidget(self._drv_state_lbl)

        status_row = QHBoxLayout()
        status_row.setSpacing(8)
        self._moe_lbl = QLabel("MOE: OFF")
        self._moe_lbl.setStyleSheet(
            f"color: {FG_DIM}; font-size: 10pt; font-weight: bold; {MONO}")
        self._moe_lbl.setAlignment(Qt.AlignCenter)
        status_row.addWidget(self._moe_lbl)
        self._ct_lbl = QLabel("")
        self._ct_lbl.setAlignment(Qt.AlignCenter)
        status_row.addWidget(self._ct_lbl)
        drv_lo.addLayout(status_row)

        self._shunt_lbl = QLabel("IU: ---    IW: ---")
        self._shunt_lbl.setStyleSheet(f"color: {FG_DIM}; font-size: 10pt; {MONO}")
        self._shunt_lbl.setAlignment(Qt.AlignCenter)
        drv_lo.addWidget(self._shunt_lbl)

        self._diag_lbl = QLabel("")
        self._diag_lbl.setAlignment(Qt.AlignCenter)
        self._diag_lbl.setWordWrap(True)
        drv_lo.addWidget(self._diag_lbl)

        drv_btns = QGridLayout()
        drv_btns.setSpacing(6)

        btn_data = [
            ("Start", ACCENT_GREEN, False,
             "SET:DRV:START — begin precharge sequence", "SET:DRV:START", 0, 0),
            ("Run", ACCENT_GREEN, True,
             "SET:DRV:RUN — enable SVPWM outputs", "SET:DRV:RUN", 0, 1),
            ("Stop", ACCENT_RED, True,
             "SET:DRV:STOP — disable outputs, open relay", "SET:DRV:STOP", 1, 0),
            ("Reset", ACCENT_YELLOW, False,
             "SET:DRV:RESET — clear faults, return to IDLE", "SET:DRV:RESET", 1, 1),
        ]
        for label, color, bold, tooltip, cmd, row, col in btn_data:
            btn = QPushButton(label)
            btn.setCursor(Qt.PointingHandCursor)
            weight = "font-weight: bold;" if bold else ""
            btn.setStyleSheet(
                f"font-size: 10pt; padding: 6px; color: {color}; {weight} {MONO}")
            btn.setToolTip(tooltip)
            btn.clicked.connect(lambda _, c=cmd: self._send_cmd(c))
            drv_btns.addWidget(btn, row, col)

        drv_lo.addLayout(drv_btns)

        e_stop = QPushButton("EMERGENCY STOP  (Esc)")
        e_stop.setCursor(Qt.PointingHandCursor)
        e_stop.setStyleSheet(
            f"font-size: 11pt; padding: 10px; color: white;"
            f"background: {ACCENT_RED}; font-weight: bold;"
            f"border: 2px solid #C03030; border-radius: 6px; {MONO}")
        e_stop.setToolTip("SET:SVPWM:0 — emergency stop: disable PWM, open relay, stop drive")
        e_stop.clicked.connect(self._emergency_stop)
        drv_lo.addWidget(e_stop)

        left.addWidget(drv_group)

        bus_group = QGroupBox("DC BUS")
        bus_lo = QVBoxLayout(bus_group)
        bus_lo.setSpacing(4)

        vbus_row = QHBoxLayout()
        vbus_row.setSpacing(4)
        self._vbus_value = QLabel("---")
        self._vbus_value.setStyleSheet(
            f"color: {FG_DIM}; font-size: 24pt; font-weight: bold; {MONO}")
        vbus_row.addWidget(self._vbus_value)
        vbus_unit = QLabel("V dc")
        vbus_unit.setStyleSheet(f"color: {FG_DIM}; font-size: 11pt; {MONO}")
        vbus_unit.setAlignment(Qt.AlignBottom)
        vbus_row.addWidget(vbus_unit)
        vbus_row.addStretch()
        bus_lo.addLayout(vbus_row)

        self._vbus_raw = QLabel("RAW ---")
        self._vbus_raw.setStyleSheet(f"color: {FG_DIM}; font-size: 8pt; {MONO}")
        bus_lo.addWidget(self._vbus_raw)

        chg_btns = QHBoxLayout()
        chg_btns.setSpacing(4)

        btn_chg_stop = QPushButton("Drive Stop")
        btn_chg_stop.setFixedHeight(28)
        btn_chg_stop.setCursor(Qt.PointingHandCursor)
        btn_chg_stop.setStyleSheet(f"font-size: 9pt; padding: 2px 6px; {MONO}")
        btn_chg_stop.setToolTip("SET:CHG:STOP — stop drive, disable PWM, open relay")
        btn_chg_stop.clicked.connect(lambda: self._send_cmd("SET:CHG:STOP"))
        chg_btns.addWidget(btn_chg_stop)

        btn_chg_clear = QPushButton("Clear Faults")
        btn_chg_clear.setFixedHeight(28)
        btn_chg_clear.setCursor(Qt.PointingHandCursor)
        btn_chg_clear.setStyleSheet(
            f"font-size: 9pt; padding: 2px 6px; color: {ACCENT_YELLOW}; {MONO}")
        btn_chg_clear.setToolTip(
            "SET:CHG:CLEAR — full reset: clear faults, stop drive, return to IDLE")
        btn_chg_clear.clicked.connect(lambda: self._send_cmd("SET:CHG:CLEAR"))
        chg_btns.addWidget(btn_chg_clear)
        bus_lo.addLayout(chg_btns)

        left.addWidget(bus_group)
        left.addStretch()
        return left

    # ── Center Panel: Parameters + Pin Map ────────────────────────────

    def _build_param_panel(self) -> QVBoxLayout:
        center = QVBoxLayout()
        center.setSpacing(10)

        param_group = QGroupBox("PARAMETERS")
        param_lo = QVBoxLayout(param_group)
        param_lo.setSpacing(8)

        self._spin_freq = self._make_param_row(
            param_lo, "OUT FREQ", "Hz", ACCENT_GREEN,
            OUT_FREQ_MIN, OUT_FREQ_MAX, OUT_FREQ_DEF, OUT_FREQ_STEP)
        self._spin_swf = self._make_param_row(
            param_lo, "SW FREQ", "Hz", PE_BLUE_LIGHT,
            SW_FREQ_MIN, SW_FREQ_MAX, SW_FREQ_DEF, SW_FREQ_STEP)
        self._spin_mod = self._make_param_row(
            param_lo, "MOD IDX", "\u2030", ACCENT_YELLOW,
            MOD_IDX_MIN, MOD_IDX_MAX, MOD_IDX_DEF, MOD_IDX_STEP)

        send_btn = QPushButton("Apply")
        send_btn.setObjectName("primaryBtn")
        send_btn.setCursor(Qt.PointingHandCursor)
        send_btn.setFixedHeight(40)
        send_btn.clicked.connect(self._send_params)
        param_lo.addWidget(send_btn)

        center.addWidget(param_group)

        pins_group = QGroupBox("PIN MAP")
        pins_lo = QHBoxLayout(pins_group)
        pins_lo.setSpacing(12)
        pins_info = [
            ("U  PE8/PE9",   "CH1/CH1N", ACCENT_RED),
            ("V  PE10/PE11", "CH2/CH2N", ACCENT_YELLOW),
            ("W  PE12/PE13", "CH3/CH3N", ACCENT_CYAN),
        ]
        for title, value, color in pins_info:
            col = QVBoxLayout()
            col.setSpacing(2)
            lbl_t = QLabel(title)
            lbl_t.setStyleSheet(
                f"color: {FG_DIM}; font-size: 8pt; font-weight: bold; {MONO}")
            col.addWidget(lbl_t)
            lbl_v = QLabel(value)
            lbl_v.setStyleSheet(
                f"color: {color}; font-size: 11pt; font-weight: bold; {MONO}")
            col.addWidget(lbl_v)
            pins_lo.addLayout(col)
        center.addWidget(pins_group)

        center.addStretch()
        return center

    @staticmethod
    def _make_param_row(parent_lo: QVBoxLayout, label: str, unit: str,
                        color: str, lo_val: int, hi_val: int,
                        default: int, step: int) -> QSpinBox:
        row = QHBoxLayout()
        row.setSpacing(6)
        lbl = QLabel(label)
        lbl.setStyleSheet(
            f"color: {FG_DIM}; font-size: 9pt; font-weight: bold; {MONO}")
        lbl.setFixedWidth(80)
        row.addWidget(lbl)
        sb = QSpinBox()
        sb.setRange(lo_val, hi_val)
        sb.setValue(default)
        sb.setSingleStep(step)
        sb.setSuffix(f"  {unit}")
        sb.setStyleSheet(sb.styleSheet() + f"QSpinBox {{ color: {color}; }}")
        row.addWidget(sb)
        parent_lo.addLayout(row)
        return sb

    # ── Right Panel: Display + Registers ──────────────────────────────

    def _build_aux_panel(self) -> QVBoxLayout:
        right = QVBoxLayout()
        right.setSpacing(10)

        disp_group = QGroupBox("FRONT PANEL DISPLAY")
        disp_lo = QVBoxLayout(disp_group)
        disp_lo.setSpacing(6)

        disp_row = QHBoxLayout()
        disp_row.setSpacing(6)
        self._disp_input = QLineEdit()
        self._disp_input.setPlaceholderText("Display text (6 chars)")
        self._disp_input.setMaxLength(6)
        self._disp_input.setStyleSheet(
            f"background: {BG_INPUT}; color: {FG_PRIMARY};"
            f"border: 1px solid {BORDER_COLOR}; border-radius: 6px;"
            f"padding: 6px 10px; {MONO} font-size: 13pt; font-weight: bold;")
        self._disp_input.returnPressed.connect(self._send_display_text)
        disp_row.addWidget(self._disp_input)

        btn_disp = QPushButton("Send")
        btn_disp.setCursor(Qt.PointingHandCursor)
        btn_disp.setFixedSize(60, 32)
        btn_disp.setStyleSheet(f"font-size: 9pt; padding: 2px; {MONO}")
        btn_disp.clicked.connect(self._send_display_text)
        disp_row.addWidget(btn_disp)
        disp_lo.addLayout(disp_row)

        btn_poll = QPushButton("Poll Buttons")
        btn_poll.setCursor(Qt.PointingHandCursor)
        btn_poll.setFixedHeight(28)
        btn_poll.setStyleSheet(f"font-size: 9pt; padding: 2px 6px; {MONO}")
        btn_poll.setToolTip("GET:BTN — read front-panel button state")
        btn_poll.clicked.connect(self._poll_buttons)
        disp_lo.addWidget(btn_poll)

        self._btn_lbl = QLabel("RAW:--  SCR:--  INC:--  DEC:--")
        self._btn_lbl.setStyleSheet(f"color: {FG_DIM}; font-size: 9pt; {MONO}")
        self._btn_lbl.setAlignment(Qt.AlignCenter)
        disp_lo.addWidget(self._btn_lbl)

        right.addWidget(disp_group)

        query_group = QGroupBox("DIAGNOSTICS")
        query_lo = QVBoxLayout(query_group)
        query_lo.setSpacing(4)

        for label, cmd in [("GET:REG", "GET:REG"),
                           ("GET:DRV", "GET:DRV"),
                           ("GET:DIAG", "GET:DIAG"),
                           ("GET:HEAP", "GET:HEAP")]:
            btn = QPushButton(label)
            btn.setCursor(Qt.PointingHandCursor)
            btn.setFixedHeight(28)
            btn.setStyleSheet(f"font-size: 9pt; padding: 2px 6px; {MONO}")
            btn.clicked.connect(lambda _, c=cmd: self._send_cmd(c))
            query_lo.addWidget(btn)

        right.addWidget(query_group)
        right.addStretch()
        return right

    # ── Bottom: Log + Raw Command ─────────────────────────────────────

    def _build_log_panel(self, parent_lo: QVBoxLayout) -> None:
        self._log = QTextEdit()
        self._log.setReadOnly(True)
        self._log.setPlaceholderText("Serial log — connect to see data")
        self._log.document().setMaximumBlockCount(800)
        parent_lo.addWidget(self._log)

        cmd_row = QHBoxLayout()
        cmd_row.setSpacing(6)
        cmd_lbl = QLabel("CMD")
        cmd_lbl.setStyleSheet(
            f"color: {FG_DIM}; font-size: 9pt; font-weight: bold; {MONO}")
        cmd_row.addWidget(cmd_lbl)
        self._cmd_input = QLineEdit()
        self._cmd_input.setPlaceholderText(
            "Raw command (e.g. GET:REG, SET:DRV:START)")
        self._cmd_input.setStyleSheet(
            f"background: {BG_INPUT}; color: {FG_PRIMARY};"
            f"border: 1px solid {BORDER_COLOR}; border-radius: 6px;"
            f"padding: 6px 10px; {MONO} font-size: 11pt;")
        self._cmd_input.returnPressed.connect(self._send_raw_cmd)
        cmd_row.addWidget(self._cmd_input)
        send_raw = QPushButton("Send")
        send_raw.setCursor(Qt.PointingHandCursor)
        send_raw.setFixedWidth(80)
        send_raw.clicked.connect(self._send_raw_cmd)
        cmd_row.addWidget(send_raw)
        parent_lo.addLayout(cmd_row)

    # ── Status Bar ────────────────────────────────────────────────────

    def _build_statusbar(self) -> None:
        lbl = QLabel(f"  v{APP_VERSION}  ·  Esc = E-Stop")
        lbl.setStyleSheet(f"color: {FG_DIM}; font-size: 10pt;")
        self.statusBar().addPermanentWidget(lbl)

    def closeEvent(self, event) -> None:
        self._disconnect()
        event.accept()


# ═══════════════════════════════════════════════════════════════════════════
#  Entry Point
# ═══════════════════════════════════════════════════════════════════════════

def main() -> None:
    app = QApplication(sys.argv)
    app.setApplicationName(APP_NAME)
    app.setOrganizationName(APP_ORG)
    app.setWindowIcon(_app_icon())
    app.setStyleSheet(GLOBAL_STYLE)
    window = MainWindow()
    window.show()
    sys.exit(app.exec())


if __name__ == "__main__":
    main()
