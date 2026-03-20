"""
PE Operator Software — SVPWM Monitor
======================================

Company : PE Info
Author  : Umit Kayacik
Date    : 2026
Version : 3.2.0

Minimal USB CDC serial monitor for 6-channel SVPWM inverter.

Dependencies: pip install PySide6 pyserial

Copyright (c) 2026 PE Info.  All rights reserved.
"""

from __future__ import annotations
import sys
from pathlib import Path
from typing import Optional

from PySide6.QtWidgets import (
    QApplication, QMainWindow, QWidget, QVBoxLayout, QHBoxLayout,
    QLabel, QPushButton, QComboBox, QFrame, QTextEdit, QSizePolicy,
    QSpinBox, QCheckBox, QLineEdit,
)
from PySide6.QtCore import Qt, QThread, Signal, QRect
from PySide6.QtGui import (
    QFont, QColor, QPainter, QPen, QBrush, QPixmap, QIcon,
    QRadialGradient,
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

ACCENT_GREEN_L = "#3FB950"
ACCENT_RED_L   = "#F85149"
ACCENT_YELLOW_L= "#E3B341"
ACCENT_CYAN    = "#39D2C0"
BORDER_COLOR   = "#445060"
BORDER_FOCUS   = "#58A6FF"

MONO = "font-family: 'Consolas', monospace;"

# ═══════════════════════════════════════════════════════════════════════════
#  App Constants
# ═══════════════════════════════════════════════════════════════════════════

APP_NAME    = "SVPWM Monitor"
APP_VERSION = "3.2.0"
APP_ORG     = "PE Info"

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
    padding: 8px; {MONO} font-size: 11pt;
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
        self.setMinimumSize(700, 400)
        self.resize(900, 500)

        self._worker: Optional[SerialWorker] = None

        self._build_toolbar()
        self._build_central()
        self._build_statusbar()

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

        info = QLabel("6-ch SVPWM  ·  60 Hz  ·  5 kHz  ·  2-Level VSI")
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

        self._conn_dot.set_color(ACCENT_GREEN_L)
        self._conn_label.setText(f"  Connected: {port}")
        self._conn_label.setStyleSheet(f"color: {ACCENT_GREEN_L}; font-size: 11pt;")
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

    def _on_line(self, line: str) -> None:
        self._log.append(line)
        sb = self._log.verticalScrollBar()
        sb.setValue(sb.maximum())

        if line.startswith("$VBUS,"):
            self._parse_vbus(line)
        elif line.startswith("$CHG,"):
            self._parse_chg(line)

    def _parse_vbus(self, line: str) -> None:
        try:
            parts = {k: v for k, v in
                     (p.split(":") for p in line[6:].split(",") if ":" in p)}
            raw = int(parts.get("RAW", "0"))
            volts = float(parts.get("V", "0"))
        except (ValueError, KeyError):
            return
        self._vbus_value.setText(f"{volts:.0f}")
        self._vbus_raw.setText(f"RAW {raw}")
        if volts > 380:
            color = ACCENT_RED_L
        elif volts > 50:
            color = ACCENT_GREEN_L
        else:
            color = FG_DIM
        self._vbus_value.setStyleSheet(
            f"color: {color}; font-size: 22pt; font-weight: bold; {MONO}")

    _CHG_STATE_STYLE = {
        "IDLE":  (FG_DIM,         "IDLE"),
        "PRCHG": (ACCENT_YELLOW_L, "PRE-CHARGE"),
        "VRFY":  (ACCENT_CYAN,    "VERIFY"),
        "RUN":   (ACCENT_GREEN_L, "RUNNING"),
        "FAULT": (ACCENT_RED_L,   "FAULT"),
    }

    def _parse_chg(self, line: str) -> None:
        try:
            parts = {k: v for k, v in
                     (p.split(":") for p in line[5:].split(",") if ":" in p)}
            state = parts.get("S", "IDLE")
            fault = parts.get("F", "NONE")
        except (ValueError, KeyError):
            return
        color, label = self._CHG_STATE_STYLE.get(state, (FG_DIM, state))
        if state == "FAULT" and fault != "NONE":
            label = f"FAULT: {fault}"
        self._chg_state_lbl.setText(label)
        self._chg_state_lbl.setStyleSheet(
            f"color: {color}; font-size: 10pt; font-weight: bold; {MONO}")

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

    def _on_charge_auto(self) -> None:
        if not self._worker:
            self.statusBar().showMessage("  Not connected", 3000)
            return
        self._worker.send_command("SET:CHG:AUTO")
        self.statusBar().showMessage("  Charge: auto pre-charge started", 3000)

    def _on_charge_stop(self) -> None:
        if not self._worker:
            self.statusBar().showMessage("  Not connected", 3000)
            return
        self._worker.send_command("SET:CHG:STOP")
        self._chg_check.setChecked(False)
        self.statusBar().showMessage("  Charge: STOPPED — relay open", 3000)

    def _on_charge_changed(self, _state: int) -> None:
        if not self._worker:
            self.statusBar().showMessage("  Not connected", 3000)
            return
        val = 1 if self._chg_check.isChecked() else 0
        self._worker.send_command(f"SET:CHG:{val}")
        self.statusBar().showMessage(
            f"  Charge relay: {'CLOSED (manual)' if val else 'OPEN'}", 3000)

    def _on_svpwm_start(self) -> None:
        if not self._worker:
            self.statusBar().showMessage("  Not connected", 3000)
            return
        self._worker.send_command("SET:SVPWM:1")
        self._svpwm_running = True
        self._svpwm_state_lbl.setText("RUNNING")
        self._svpwm_state_lbl.setStyleSheet(
            f"color: {ACCENT_GREEN_L}; font-size: 12pt; font-weight: bold; {MONO}")
        self.statusBar().showMessage("  SVPWM: STARTED — outputs enabled", 3000)

    def _on_svpwm_stop(self) -> None:
        if not self._worker:
            self.statusBar().showMessage("  Not connected", 3000)
            return
        self._worker.send_command("SET:SVPWM:0")
        self._svpwm_running = False
        self._svpwm_state_lbl.setText("STOPPED")
        self._svpwm_state_lbl.setStyleSheet(
            f"color: {ACCENT_RED_L}; font-size: 12pt; font-weight: bold; {MONO}")
        self.statusBar().showMessage("  SVPWM: STOPPED — outputs disabled", 3000)

    def _on_test_mode_changed(self, _state: int) -> None:
        if not self._worker:
            self.statusBar().showMessage("  Not connected", 3000)
            return
        val = 1 if self._test_check.isChecked() else 0
        self._worker.send_command(f"SET:TEST:{val}")
        self.statusBar().showMessage(
            f"  Test mode: {'ON' if val else 'OFF'} (SET:TEST:{val})", 3000)

    def _send_raw_cmd(self) -> None:
        cmd = self._cmd_input.text().strip()
        if not cmd:
            return
        if not self._worker:
            self.statusBar().showMessage("  Not connected", 3000)
            return
        self._worker.send_command(cmd)
        self._log.append(f">>> {cmd}")
        self._cmd_input.clear()
        self.statusBar().showMessage(f"  Sent: {cmd}", 3000)

    def _on_error(self, msg: str) -> None:
        self.statusBar().showMessage(f"  Serial error: {msg}", 5000)
        self._disconnect()

    # ── Central ──────────────────────────────────────────────────────

    def _build_central(self) -> None:
        central = QWidget()
        self.setCentralWidget(central)
        lo = QVBoxLayout(central)
        lo.setContentsMargins(12, 8, 12, 8)
        lo.setSpacing(10)

        header = QLabel("  SVPWM  ·  3 Complementary Pairs  ·  TIM1  ·  PE8-PE13")
        header.setStyleSheet(
            f"background: {BG_SECONDARY}; color: {PE_BLUE_LIGHT};"
            f"font-size: 11pt; font-weight: bold; padding: 8px 12px;"
            f"border-radius: 4px; {MONO}")
        header.setFixedHeight(32)
        lo.addWidget(header)

        ctrl_frame = QFrame()
        ctrl_frame.setStyleSheet(
            f"background: {BG_SECONDARY}; border: 1px solid {BORDER_COLOR};"
            f"border-radius: 8px;")
        ctrl_lo = QHBoxLayout(ctrl_frame)
        ctrl_lo.setContentsMargins(16, 12, 16, 12)
        ctrl_lo.setSpacing(20)

        def _make_param(label: str, unit: str, color: str,
                        lo_val: int, hi_val: int, default: int,
                        step: int = 1) -> QSpinBox:
            col = QVBoxLayout()
            col.setSpacing(4)
            lbl = QLabel(label)
            lbl.setStyleSheet(
                f"color: {FG_DIM}; font-size: 9pt; font-weight: bold; {MONO}")
            col.addWidget(lbl)
            row = QHBoxLayout()
            row.setSpacing(6)
            sb = QSpinBox()
            sb.setRange(lo_val, hi_val)
            sb.setValue(default)
            sb.setSingleStep(step)
            sb.setSuffix(f"  {unit}")
            sb.setStyleSheet(
                sb.styleSheet() + f"QSpinBox {{ color: {color}; }}")
            row.addWidget(sb)
            col.addLayout(row)
            ctrl_lo.addLayout(col)
            return sb

        self._spin_freq = _make_param(
            "OUTPUT FREQ", "Hz", ACCENT_GREEN_L, 1, 400, 60, 5)
        self._spin_swf = _make_param(
            "SWITCHING FREQ", "Hz", PE_BLUE_LIGHT, 1000, 20000, 5000, 500)
        self._spin_mod = _make_param(
            "MOD INDEX", "\u2030", ACCENT_YELLOW_L, 0, 1155, 850, 10)

        chg_col = QVBoxLayout()
        chg_col.setSpacing(4)
        chg_lbl = QLabel("DC BUS")
        chg_lbl.setStyleSheet(
            f"color: {FG_DIM}; font-size: 9pt; font-weight: bold; {MONO}")
        chg_col.addWidget(chg_lbl)

        vbus_row = QHBoxLayout()
        vbus_row.setSpacing(4)
        self._vbus_value = QLabel("---")
        self._vbus_value.setStyleSheet(
            f"color: {FG_DIM}; font-size: 22pt; font-weight: bold; {MONO}")
        vbus_row.addWidget(self._vbus_value)
        vbus_unit = QLabel("V")
        vbus_unit.setStyleSheet(
            f"color: {FG_DIM}; font-size: 12pt; {MONO}")
        vbus_unit.setAlignment(Qt.AlignBottom)
        vbus_row.addWidget(vbus_unit)
        chg_col.addLayout(vbus_row)

        self._vbus_raw = QLabel("RAW ---")
        self._vbus_raw.setStyleSheet(
            f"color: {FG_DIM}; font-size: 8pt; {MONO}")
        chg_col.addWidget(self._vbus_raw)

        self._chg_state_lbl = QLabel("IDLE")
        self._chg_state_lbl.setStyleSheet(
            f"color: {FG_DIM}; font-size: 10pt; font-weight: bold; {MONO}")
        chg_col.addWidget(self._chg_state_lbl)

        chg_btn_row = QHBoxLayout()
        chg_btn_row.setSpacing(4)
        self._chg_auto_btn = QPushButton("Auto")
        self._chg_auto_btn.setFixedSize(60, 28)
        self._chg_auto_btn.setCursor(Qt.PointingHandCursor)
        self._chg_auto_btn.setStyleSheet(
            f"font-size: 9pt; padding: 2px 6px; {MONO}")
        self._chg_auto_btn.clicked.connect(self._on_charge_auto)
        chg_btn_row.addWidget(self._chg_auto_btn)

        self._chg_stop_btn = QPushButton("Stop")
        self._chg_stop_btn.setFixedSize(60, 28)
        self._chg_stop_btn.setCursor(Qt.PointingHandCursor)
        self._chg_stop_btn.setStyleSheet(
            f"font-size: 9pt; padding: 2px 6px; color: {ACCENT_RED_L}; {MONO}")
        self._chg_stop_btn.clicked.connect(self._on_charge_stop)
        chg_btn_row.addWidget(self._chg_stop_btn)
        chg_col.addLayout(chg_btn_row)

        self._chg_check = QCheckBox("Manual relay")
        self._chg_check.setStyleSheet(
            f"color: {FG_DIM}; font-size: 9pt; {MONO}")
        self._chg_check.setChecked(False)
        self._chg_check.stateChanged.connect(self._on_charge_changed)
        chg_col.addWidget(self._chg_check)
        ctrl_lo.addLayout(chg_col)

        svpwm_col = QVBoxLayout()
        svpwm_col.setSpacing(4)
        svpwm_lbl = QLabel("SVPWM OUTPUT")
        svpwm_lbl.setStyleSheet(
            f"color: {FG_DIM}; font-size: 9pt; font-weight: bold; {MONO}")
        svpwm_col.addWidget(svpwm_lbl)

        self._svpwm_state_lbl = QLabel("RUNNING")
        self._svpwm_state_lbl.setStyleSheet(
            f"color: {ACCENT_GREEN_L}; font-size: 12pt; font-weight: bold; {MONO}")
        svpwm_col.addWidget(self._svpwm_state_lbl)
        self._svpwm_running = True

        svpwm_btn_row = QHBoxLayout()
        svpwm_btn_row.setSpacing(4)
        self._svpwm_start_btn = QPushButton("Start")
        self._svpwm_start_btn.setFixedSize(70, 32)
        self._svpwm_start_btn.setCursor(Qt.PointingHandCursor)
        self._svpwm_start_btn.setStyleSheet(
            f"font-size: 10pt; padding: 4px 8px;"
            f"color: {ACCENT_GREEN_L}; font-weight: bold; {MONO}")
        self._svpwm_start_btn.clicked.connect(self._on_svpwm_start)
        svpwm_btn_row.addWidget(self._svpwm_start_btn)

        self._svpwm_stop_btn = QPushButton("Stop")
        self._svpwm_stop_btn.setFixedSize(70, 32)
        self._svpwm_stop_btn.setCursor(Qt.PointingHandCursor)
        self._svpwm_stop_btn.setStyleSheet(
            f"font-size: 10pt; padding: 4px 8px;"
            f"color: {ACCENT_RED_L}; font-weight: bold; {MONO}")
        self._svpwm_stop_btn.clicked.connect(self._on_svpwm_stop)
        svpwm_btn_row.addWidget(self._svpwm_stop_btn)
        svpwm_col.addLayout(svpwm_btn_row)

        self._test_check = QCheckBox("Test 50% (CH1)")
        self._test_check.setStyleSheet(
            f"color: {FG_DIM}; font-size: 9pt; {MONO}")
        self._test_check.setChecked(False)
        self._test_check.stateChanged.connect(self._on_test_mode_changed)
        svpwm_col.addWidget(self._test_check)
        ctrl_lo.addLayout(svpwm_col)

        send_btn = QPushButton("Apply")
        send_btn.setObjectName("primaryBtn")
        send_btn.setCursor(Qt.PointingHandCursor)
        send_btn.setFixedHeight(48)
        send_btn.clicked.connect(self._send_params)
        ctrl_lo.addWidget(send_btn, alignment=Qt.AlignBottom)

        sep = QFrame()
        sep.setFrameShape(QFrame.VLine)
        sep.setStyleSheet(f"color: {BORDER_COLOR};")
        ctrl_lo.addWidget(sep)

        pins_info = [
            ("U  PE8/PE9",   "CH1/CH1N", ACCENT_RED_L),
            ("V  PE10/PE11", "CH2/CH2N", ACCENT_YELLOW_L),
            ("W  PE12/PE13", "CH3/CH3N", ACCENT_CYAN),
        ]
        for title, value, color in pins_info:
            col = QVBoxLayout()
            col.setSpacing(2)
            lbl_t = QLabel(title)
            lbl_t.setStyleSheet(
                f"color: {FG_DIM}; font-size: 9pt; font-weight: bold; {MONO}")
            col.addWidget(lbl_t)
            lbl_v = QLabel(value)
            lbl_v.setStyleSheet(
                f"color: {color}; font-size: 14pt; font-weight: bold; {MONO}")
            col.addWidget(lbl_v)
            ctrl_lo.addLayout(col)

        ctrl_lo.addStretch()
        lo.addWidget(ctrl_frame)

        self._log = QTextEdit()
        self._log.setReadOnly(True)
        self._log.setPlaceholderText("Serial log — connect to see data")
        self._log.document().setMaximumBlockCount(500)
        lo.addWidget(self._log)

        cmd_row = QHBoxLayout()
        cmd_row.setSpacing(6)
        cmd_lbl = QLabel("CMD")
        cmd_lbl.setStyleSheet(
            f"color: {FG_DIM}; font-size: 9pt; font-weight: bold; {MONO}")
        cmd_row.addWidget(cmd_lbl)
        self._cmd_input = QLineEdit()
        self._cmd_input.setPlaceholderText(
            "Raw command (e.g. GET:REG, SET:TEST:1)")
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
        lo.addLayout(cmd_row)

    def _build_statusbar(self) -> None:
        lbl = QLabel(f"  App v{APP_VERSION}")
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
