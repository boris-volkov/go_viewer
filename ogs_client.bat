@echo off
cd /d "%~dp0"
if not exist "apps\ogs_client\ogs_client.exe" (
    echo ogs_client.exe not found -- build it first ^(see README.md^).
    pause
    exit /b 1
)
start "" "apps\ogs_client\ogs_client.exe"
