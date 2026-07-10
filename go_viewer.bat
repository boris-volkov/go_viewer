@echo off
cd /d "%~dp0"
if not exist "apps\go_viewer\go_viewer.exe" (
    echo go_viewer.exe not found -- build it first ^(see README.md^).
    pause
    exit /b 1
)
start "" "apps\go_viewer\go_viewer.exe"
