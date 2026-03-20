@echo off
title PE Operator Software — UltraLogic R1
cd /d "%~dp0"
python main.py
if %errorlevel% neq 0 (
    echo.
    echo [ERROR] Failed to launch. Make sure Python and dependencies are installed:
    echo   pip install PySide6 pyqtgraph pyserial numpy
    echo.
    pause
)
