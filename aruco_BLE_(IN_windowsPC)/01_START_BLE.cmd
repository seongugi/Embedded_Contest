@echo off
cd /d "%~dp0"
python "%~dp0track_aruco_4k.py" --ble --ble-config "%~dp0ble_config.json"
if errorlevel 1 pause
