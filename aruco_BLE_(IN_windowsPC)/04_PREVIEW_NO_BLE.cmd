@echo off
cd /d "%~dp0"
python "%~dp0track_aruco_4k.py"
if errorlevel 1 pause
