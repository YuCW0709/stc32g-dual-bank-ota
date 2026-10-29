@echo off
REM ============================================================
REM  serve_fw.bat -- expose build\fw_min_v0.2.0_ota.bin on the
REM  public internet so the 4G module can download it.
REM
REM  Why this script exists:
REM    file.pizza / Lansou / WenShuShu are P2P or JS pages.
REM    Their server has no static file endpoint, so the AT stack
REM    can NEVER fetch the file. This script serves a real file
REM    over plain HTTP(S) through an SSH tunnel.
REM
REM  Requirements: python (in PATH) + ssh (Windows 10/11 built-in)
REM  Keep this window OPEN while the board is downloading.
REM ============================================================
setlocal
cd /d "%~dp0.."

set PORT=8000
set FILE=fw_min_v0.2.0_ota.bin

echo ============================================================
echo   OTA firmware tunnel
echo     local  : http://127.0.0.1:%PORT%/%FILE%
echo     public : see the https:// line printed below
echo     verify : python tools\check_fw_url.py ^<public^>/%FILE%
echo ============================================================
echo.

if not exist "build\%FILE%" (
    echo [X] build\%FILE% not found. Build it first.
    pause
    exit /b 1
)

start "fw-http" /min python -m http.server %PORT% --bind 127.0.0.1 --directory build
timeout /t 2 >nul

echo [tunnel] connecting to serveo.net (no account needed) ...
echo [tunnel] the public URL appears on the next line
echo.
ssh -T -o StrictHostKeyChecking=no -o UserKnownHostsFile=NUL -o ServerAliveInterval=20 -R 80:localhost:%PORT% serveo.net

echo.
echo [tunnel] closed. If the board already downloaded, this is fine.
pause
