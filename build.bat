@echo off
set "PATH=d:\Picoprojects\tools\python;d:\Picoprojects\tools\cmake\bin;d:\Picoprojects\tools\gcc-arm\bin;d:\Picoprojects\tools;%PATH%"
set "PICO_SDK_PATH=d:\Picoprojects\tools\pico-sdk"

cd /d d:\Picoprojects\pico-universal-programmer

if not exist build mkdir build
cd build

echo [*] Configuring CMake with prebuilt host tools...
cmake -G Ninja -DCMAKE_BUILD_TYPE=Release ^
  -DPython3_EXECUTABLE=d:/Picoprojects/tools/python/python.exe ^
  -DPICO_ELF2UF2_EXECUTABLE=d:/Picoprojects/tools/elf2uf2.exe ^
  -DPICO_PIOASM_EXECUTABLE=d:/Picoprojects/tools/pioasm.exe ^
  ..

if %errorlevel% neq 0 (
    echo [!] CMake configuration failed.
    exit /b %errorlevel%
)

echo [*] Building firmware...
ninja
if %errorlevel% neq 0 (
    echo [!] Build failed.
    exit /b %errorlevel%
)

echo [SUCCESS] Firmware compiled successfully!
dir /b *.uf2
