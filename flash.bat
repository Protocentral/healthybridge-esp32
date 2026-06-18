@echo off
REM Flash prebuilt HealthyBridge Lite firmware to a HealthyPi 5 ESP32-C3 (Windows).
REM
REM Requires esptool:  pip install esptool
REM Connect the ESP32-C3 USB Type-C port and find its COM port in Device Manager.
REM
REM Usage:
REM   flash.bat COM5                FULL install (merged image @ 0x0) - erases Wi-Fi/settings
REM   flash.bat COM5 --app-only     UPDATE app only (@ 0x10000)       - keeps Wi-Fi/settings
REM
REM Download the binaries from the GitHub release into this folder:
REM   healthypi5_next_esp32-merged.bin  and/or  healthypi5_next_esp32-app.bin
setlocal
set PORT=%1
set MODE=%2
set CHIP=esp32c3
set BAUD=460800

if "%PORT%"=="" (
  echo usage: flash.bat ^<PORT^> [--app-only]
  exit /b 1
)

if "%MODE%"=="--app-only" (
  echo ^>^> Updating app only @ 0x10000 ^(stored Wi-Fi / settings preserved^)
  python -m esptool --chip %CHIP% -p %PORT% -b %BAUD% write_flash 0x10000 healthypi5_next_esp32-app.bin
) else (
  echo ^>^> Full install @ 0x0 ^(this ERASES stored Wi-Fi / settings - re-provision after^)
  python -m esptool --chip %CHIP% -p %PORT% -b %BAUD% write_flash 0x0 healthypi5_next_esp32-merged.bin
)

echo ^>^> Done. Reset the board to run the new firmware.
endlocal
