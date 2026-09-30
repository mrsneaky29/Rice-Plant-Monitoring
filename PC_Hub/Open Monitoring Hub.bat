@echo off
setlocal
cd /d "%~dp0"
if exist Rice_Plant_Monitoring_Hub.exe (
  start "Rice Plant Monitoring Hub" Rice_Plant_Monitoring_Hub.exe --open-browser
) else (
  py -3 Monitoring_Hub.py --open-browser
)
