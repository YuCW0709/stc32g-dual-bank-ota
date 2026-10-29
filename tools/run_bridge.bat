@echo off
rem ============================================================================
rem  serial_bridge launcher
rem
rem  NOTE: this file is intentionally ASCII-only. A UTF-8 .bat echoed by a
rem        cp936 cmd.exe garbles the text and can abort the script mid-way
rem        (which looks exactly like a crash-and-close). All Chinese text is
rem        printed by Python instead, where the encoding is handled properly.
rem ============================================================================
setlocal
cd /d "%~dp0.."
title serial bridge - OTA debug
chcp 65001 >nul 2>&1
rem Tell the script this window is already held by "pause" below, so it must
rem not add a second "press Enter" prompt of its own.
set "SB_LAUNCHER=1"

rem ---- 1) locate a usable Python -------------------------------------------
set "PY="
py -3 -c "import sys" >nul 2>&1
if %ERRORLEVEL%==0 set "PY=py -3"
if not defined PY (
    python -c "import sys" >nul 2>&1
    if not errorlevel 1 set "PY=python"
)

if not defined PY (
    echo.
    echo   [!] Python 3 not found in PATH.
    echo       Install it from python.org and tick
    echo       "Add python.exe to PATH" during setup.
    echo.
    pause
    exit /b 1
)

rem ---- 2) make sure pyserial is there --------------------------------------
%PY% -c "import serial" >nul 2>&1
if not errorlevel 1 goto :run

echo.
echo   [i] pyserial not installed, installing now ...
%PY% -m pip install pyserial
if errorlevel 1 (
    echo.
    echo   [!] pip install failed. Run manually:
    echo       %PY% -m pip install pyserial
    echo.
    pause
    exit /b 1
)

:run
rem ---- 3) run. With no arguments the script starts its own wizard. ---------
echo.
%PY% "%~dp0serial_bridge.py" %*
set "RC=%ERRORLEVEL%"
echo.
if not "%RC%"=="0" echo   [!] exit code %RC%
rem Only hold the window when launched by double-click (no arguments).
rem Keeps the summary readable, and never blocks a scripted call.
if "%~1"=="" pause
endlocal
exit /b %RC%
