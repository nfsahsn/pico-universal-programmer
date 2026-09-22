@echo off
setlocal
cd /d "%~dp0"
if not defined PICO_SDK_PATH (
    echo Set PICO_SDK_PATH to the pinned Pico SDK 2.2.0 checkout.
    exit /b 1
)
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DPICO_NO_PICOTOOL=1
if errorlevel 1 exit /b 1
cmake --build build
if errorlevel 1 exit /b 1
echo Supervisor built. Combined firmware requires all three slot-linked probe builds.
