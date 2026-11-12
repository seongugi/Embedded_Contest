@echo off
cd /d "%~dp0"
python "%~dp0check_ble_connection.py" %*
pause
