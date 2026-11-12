@echo off
cd /d "%~dp0"
python "%~dp0aruco_ble_sender.py" --scan --config "%~dp0ble_config.json"
pause
