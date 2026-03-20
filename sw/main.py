"""
PE Operator Software — SVPWM Monitor
======================================

Company : PE Info
Author  : Umit Kayacik
Date    : 2026
Version : 4.0.0

USB CDC serial monitor for 6-channel SVPWM inverter (UltraLogic R1).

Firmware command set:
  SET:FREQ:<hz>              output frequency 1-400 Hz
  SET:SWF:<hz>               switching frequency 1000-16000 Hz
  SET:MOD:<0-1155>           modulation index per-mille
  SET:SVPWM:0                disable PWM outputs
  SET:CHG:STOP|CLEAR         stop precharge / clear charge fault
  SET:DRV:START|RUN|STOP|RESET   drive state machine
  SET:DISP:<text>            send text to front-panel display
  GET:REG                    read TIM1 registers
  GET:DRV                    read drive state + measurements
  GET:BTN                    read front-panel button state

Firmware telemetry (periodic, no request needed):
  $DRV,S:<state>,F:<hex>,V:<volts>,IU:<amps>,IW:<amps>,S1:<raw>,S3:<raw>,BDTR:<hex>,MOE:<0|1>,CT:<0|1>
  $VBUS,RAW:<raw>,V:<volts>

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
    QSpinBox, QLineEdit, QGroupBox, QGridLayout,
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
APP_VERSION = "4.0.0"
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

_DRV_STATE_STYLE = {
    "IDLE":  (FG_DIM,          "IDLE"),
    "PRCHG": (ACCENT_YELLOW_L, "PRE-CHARGE"),
    "READY": (ACCENT_CYAN,     "READY"),
    "RUN":   (ACCENT_GREEN_L,  "RUNNING"),
    "STOP":  (ACCENT_YELLOW_L, "STOPPING"),
    "FAULT": (ACCENT_RED_L,    "FAULT"),
}


class MainWindow(QMainWindow):

    def __init__(self) -> None:
        super().__init__()
        self.setWindowTitle(f"{APP_NAME}  —  UltraLogic R1")
        self.setWindowIcon(_app_icon())
        self.setMinimumSize(800, 520)
        self.resize(1050, 620)

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

    # ── Serial Data Handling ─────────────────────────────────────────

    def _on_line(self, line: str) -> None:
        self._log.append(line)
        sb = self._log.verticalScrollBar()
        sb.setValue(sb.maximum())

        if line.startswith("$VBUS,"):
            self._parse_vbus(line)
        elif line.startswith("$DRV,"):
            self._parse_drv(line)
        elif line.startswith("$BTN,"):
            self._parse_btn(line)

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

    def _parse_drv(self, line: str) -> None:
        try:
            parts = {k: v for k, v in
                     (p.split(":", 1) for p in line[5:].split(",") if ":" in p)}
            state = parts.get("S", "IDLE")
            fault_hex = parts.get("F", "0000")
            volts = float(parts.get("V", "0"))
            i_u = float(parts.get("IU", "0"))
            i_w = float(parts.get("IW", "0"))
            moe = parts.get("MOE", "")
            cur_trip = parts.get("CT", "0")
            fault_val = int(fault_hex, 16) if fault_hex else 0
        except (ValueError, KeyError):
            return

        color, label = _DRV_STATE_STYLE.get(state, (FG_DIM, state))
        if fault_val != 0:
            label = f"FAULT: 0x{fault_hex}"
            color = ACCENT_RED_L
        self._drv_state_lbl.setText(label)
        self._drv_state_lbl.setStyleSheet(
            f"color: {color}; font-size: 12pt; font-weight: bold; {MONO}")

        moe_txt = "ON" if moe == "1" else "OFF"
        moe_color = ACCENT_GREEN_L if moe == "1" else FG_DIM
        self._moe_lbl.setText(f"MOE: {moe_txt}")
        self._moe_lbl.setStyleSheet(
            f"color: {moe_color}; font-size: 10pt; font-weight: bold; {MONO}")

        ct_str = " CUR_TRIP!" if cur_trip == "1" else ""
        self._shunt_lbl.setText(f"IU:{i_u:+.1f}A  IW:{i_w:+.1f}A{ct_str}")

        self._vbus_value.setText(f"{volts:.0f}")
        vbus_color = ACCENT_RED_L if volts > 380 else (
            ACCENT_GREEN_L if volts > 50 else FG_DIM)
        self._vbus_value.setStyleSheet(
            f"color: {vbus_color}; font-size: 22pt; font-weight: bold; {MONO}")

    def _parse_btn(self, line: str) -> None:
        try:
            parts = {k: v for k, v in
                     (p.split(":") for p in line[5:].split(",") if ":" in p)}
            raw = parts.get("RAW", "00")
            scr = parts.get("SCR", "0")
            inc = parts.get("INC", "0")
            dec = parts.get("DEC", "0")
        except (ValueError, KeyError):
            return
        self._btn_lbl.setText(f"RAW:0x{raw}  SCR:{scr}  INC:{inc}  DEC:{dec}")

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

        panels = QHBoxLayout()
        panels.setSpacing(10)

        # ── Left: Drive Control + DC Bus ─────────────────────────────
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

        self._moe_lbl = QLabel("MOE: OFF")
        self._moe_lbl.setStyleSheet(
            f"color: {FG_DIM}; font-size: 10pt; font-weight: bold; {MONO}")
        self._moe_lbl.setAlignment(Qt.AlignCenter)
        drv_lo.addWidget(self._moe_lbl)

        self._shunt_lbl = QLabel("IU:---  IW:---")
        self._shunt_lbl.setStyleSheet(
            f"color: {FG_DIM}; font-size: 9pt; {MONO}")
        self._shunt_lbl.setAlignment(Qt.AlignCenter)
        drv_lo.addWidget(self._shunt_lbl)

        drv_btns = QGridLayout()
        drv_btns.setSpacing(6)

        btn_start = QPushButton("Start")
        btn_start.setCursor(Qt.PointingHandCursor)
        btn_start.setStyleSheet(
            f"font-size: 10pt; padding: 6px; color: {ACCENT_GREEN_L}; {MONO}")
        btn_start.setToolTip("SET:DRV:START — begins precharge sequence")
        btn_start.clicked.connect(lambda: self._send_cmd("SET:DRV:START"))
        drv_btns.addWidget(btn_start, 0, 0)

        btn_run = QPushButton("Run")
        btn_run.setCursor(Qt.PointingHandCursor)
        btn_run.setStyleSheet(
            f"font-size: 10pt; padding: 6px; color: {ACCENT_GREEN_L};"
            f"font-weight: bold; {MONO}")
        btn_run.setToolTip("SET:DRV:RUN — enable SVPWM outputs (after precharge)")
        btn_run.clicked.connect(lambda: self._send_cmd("SET:DRV:RUN"))
        drv_btns.addWidget(btn_run, 0, 1)

        btn_stop = QPushButton("Stop")
        btn_stop.setCursor(Qt.PointingHandCursor)
        btn_stop.setStyleSheet(
            f"font-size: 10pt; padding: 6px; color: {ACCENT_RED_L};"
            f"font-weight: bold; {MONO}")
        btn_stop.setToolTip("SET:DRV:STOP — disable outputs, open relay")
        btn_stop.clicked.connect(lambda: self._send_cmd("SET:DRV:STOP"))
        drv_btns.addWidget(btn_stop, 1, 0)

        btn_reset = QPushButton("Reset")
        btn_reset.setCursor(Qt.PointingHandCursor)
        btn_reset.setStyleSheet(
            f"font-size: 10pt; padding: 6px; color: {ACCENT_YELLOW_L}; {MONO}")
        btn_reset.setToolTip("SET:DRV:RESET — clear faults, return to IDLE")
        btn_reset.clicked.connect(lambda: self._send_cmd("SET:DRV:RESET"))
        drv_btns.addWidget(btn_reset, 1, 1)

        drv_lo.addLayout(drv_btns)

        e_stop = QPushButton("EMERGENCY STOP")
        e_stop.setCursor(Qt.PointingHandCursor)
        e_stop.setStyleSheet(
            f"font-size: 11pt; padding: 8px; color: white;"
            f"background: {ACCENT_RED_L}; font-weight: bold;"
            f"border: 2px solid #C03030; border-radius: 6px; {MONO}")
        e_stop.setToolTip("SET:SVPWM:0 — immediately disable PWM outputs")
        e_stop.clicked.connect(lambda: self._send_cmd("SET:SVPWM:0"))
        drv_lo.addWidget(e_stop)

        left.addWidget(drv_group)

        bus_group = QGroupBox("DC BUS")
        bus_lo = QVBoxLayout(bus_group)
        bus_lo.setSpacing(4)

        vbus_row = QHBoxLayout()
        vbus_row.setSpacing(4)
        self._vbus_value = QLabel("---")
        self._vbus_value.setStyleSheet(
            f"color: {FG_DIM}; font-size: 22pt; font-weight: bold; {MONO}")
        vbus_row.addWidget(self._vbus_value)
        vbus_unit = QLabel("V")
        vbus_unit.setStyleSheet(f"color: {FG_DIM}; font-size: 12pt; {MONO}")
        vbus_unit.setAlignment(Qt.AlignBottom)
        vbus_row.addWidget(vbus_unit)
        vbus_row.addStretch()
        bus_lo.addLayout(vbus_row)

        self._vbus_raw = QLabel("RAW ---")
        self._vbus_raw.setStyleSheet(f"color: {FG_DIM}; font-size: 8pt; {MONO}")
        bus_lo.addWidget(self._vbus_raw)

        chg_btns = QHBoxLayout()
        chg_btns.setSpacing(4)
        btn_chg_stop = QPushButton("Chg Stop")
        btn_chg_stop.setFixedHeight(28)
        btn_chg_stop.setCursor(Qt.PointingHandCursor)
        btn_chg_stop.setStyleSheet(f"font-size: 9pt; padding: 2px 6px; {MONO}")
        btn_chg_stop.setToolTip("SET:CHG:STOP — stop precharge, open relay")
        btn_chg_stop.clicked.connect(lambda: self._send_cmd("SET:CHG:STOP"))
        chg_btns.addWidget(btn_chg_stop)

        btn_chg_clear = QPushButton("Clear Fault")
        btn_chg_clear.setFixedHeight(28)
        btn_chg_clear.setCursor(Qt.PointingHandCursor)
        btn_chg_clear.setStyleSheet(
            f"font-size: 9pt; padding: 2px 6px; color: {ACCENT_YELLOW_L}; {MONO}")
        btn_chg_clear.setToolTip("SET:CHG:CLEAR — clear charge fault")
        btn_chg_clear.clicked.connect(lambda: self._send_cmd("SET:CHG:CLEAR"))
        chg_btns.addWidget(btn_chg_clear)
        bus_lo.addLayout(chg_btns)

        left.addWidget(bus_group)
        left.addStretch()
        panels.addLayout(left)

        # ── Center: Parameters ───────────────────────────────────────
        center = QVBoxLayout()
        center.setSpacing(10)

        param_group = QGroupBox("PARAMETERS")
        param_lo = QVBoxLayout(param_group)
        param_lo.setSpacing(8)

        def _make_param(label: str, unit: str, color: str,
                        lo_val: int, hi_val: int, default: int,
                        step: int = 1) -> QSpinBox:
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
            sb.setStyleSheet(
                sb.styleSheet() + f"QSpinBox {{ color: {color}; }}")
            row.addWidget(sb)
            param_lo.addLayout(row)
            return sb

        self._spin_freq = _make_param(
            "OUT FREQ", "Hz", ACCENT_GREEN_L, 1, 400, 60, 5)
        self._spin_swf = _make_param(
            "SW FREQ", "Hz", PE_BLUE_LIGHT, 1000, 16000, 5000, 500)
        self._spin_mod = _make_param(
            "MOD IDX", "\u2030", ACCENT_YELLOW_L, 0, 1155, 850, 10)

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
            ("U  PE8/PE9",   "CH1/CH1N", ACCENT_RED_L),
            ("V  PE10/PE11", "CH2/CH2N", ACCENT_YELLOW_L),
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
        panels.addLayout(center)

        # ── Right: Display + Buttons ─────────────────────────────────
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
        self._btn_lbl.setStyleSheet(
            f"color: {FG_DIM}; font-size: 9pt; {MONO}")
        self._btn_lbl.setAlignment(Qt.AlignCenter)
        disp_lo.addWidget(self._btn_lbl)

        right.addWidget(disp_group)

        reg_group = QGroupBox("REGISTERS")
        reg_lo = QVBoxLayout(reg_group)
        reg_lo.setSpacing(4)
        btn_get_reg = QPushButton("GET:REG")
        btn_get_reg.setCursor(Qt.PointingHandCursor)
        btn_get_reg.setFixedHeight(28)
        btn_get_reg.setStyleSheet(f"font-size: 9pt; padding: 2px 6px; {MONO}")
        btn_get_reg.clicked.connect(lambda: self._send_cmd("GET:REG"))
        reg_lo.addWidget(btn_get_reg)

        btn_get_drv = QPushButton("GET:DRV")
        btn_get_drv.setCursor(Qt.PointingHandCursor)
        btn_get_drv.setFixedHeight(28)
        btn_get_drv.setStyleSheet(f"font-size: 9pt; padding: 2px 6px; {MONO}")
        btn_get_drv.clicked.connect(lambda: self._send_cmd("GET:DRV"))
        reg_lo.addWidget(btn_get_drv)

        right.addWidget(reg_group)
        right.addStretch()
        panels.addLayout(right)

        lo.addLayout(panels)

        # ── Bottom: Log + Raw Command ────────────────────────────────
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
