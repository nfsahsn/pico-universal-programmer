@echo off
setlocal enabledelayedexpansion

echo =====================================================================
echo  Pico Universal Programmer - Arduino IDE Support Installer
echo =====================================================================
echo.

set "ARDUINO15=%LOCALAPPDATA%\Arduino15\packages"

if not exist "%ARDUINO15%" (
    echo [ERROR] Arduino15 packages directory not found at:
    echo         %ARDUINO15%
    echo Please install your board cores in Arduino IDE first.
    echo.
    pause
    exit /b 1
)

set "SCRIPT_DIR=%~dp0"
set /a INSTALLED=0

:: 1. WCH CH32V Core
echo [1/3] Checking for WCH CH32V Core...
set "WCH_DIR=%ARDUINO15%\WCH\hardware\ch32v"
if exist "%WCH_DIR%" (
    for /d %%D in ("%WCH_DIR%\*") do (
        echo       Found CH32V version: %%~nxD
        copy /Y "%SCRIPT_DIR%ch32v\programmers.local.txt" "%%D\" >nul
        copy /Y "%SCRIPT_DIR%ch32v\platform.local.txt" "%%D\" >nul
        echo       -- Installed PicoRVD and CMSIS-DAP recipes successfully.
        set /a INSTALLED+=1
    )
) else (
    echo       [SKIP] WCH core not found. (Install via Boards Manager if needed)
)
echo.

:: 2. STM32duino Core
echo [2/3] Checking for STM32duino Core...
set "STM32_DIR=%ARDUINO15%\STMicroelectronics\hardware\stm32"
if exist "%STM32_DIR%" (
    for /d %%D in ("%STM32_DIR%\*") do (
        echo       Found STM32 version: %%~nxD
        copy /Y "%SCRIPT_DIR%stm32\programmers.local.txt" "%%D\" >nul
        echo       -- Installed CMSIS-DAP recipe successfully.
        set /a INSTALLED+=1
    )
) else (
    echo       [SKIP] STM32 core not found. (Install via Boards Manager if needed)
)
echo.

:: 3. RP2040 Core
echo [3/3] Checking for Raspberry Pi RP2040 Core...
set "RP2040_DIR=%ARDUINO15%\rp2040\hardware\rp2040"
if exist "%RP2040_DIR%" (
    for /d %%D in ("%RP2040_DIR%\*") do (
        echo       Found RP2040 version: %%~nxD
        copy /Y "%SCRIPT_DIR%rp2040\programmers.local.txt" "%%D\" >nul
        echo       -- Installed Picoprobe recipe successfully.
        set /a INSTALLED+=1
    )
) else (
    echo       [SKIP] RP2040 core not found. (Install via Boards Manager if needed)
)
echo.

echo =====================================================================
if %INSTALLED% GTR 0 (
    echo [SUCCESS] Integration files installed successfully into %INSTALLED% core(s)!
    echo           Please RESTART Arduino IDE for changes to take effect.
) else (
    echo [NOTE] No matching cores were found in your Arduino15 folder.
    echo        Install your cores in Arduino IDE first, then re-run this script.
)
echo =====================================================================
echo.
pause
