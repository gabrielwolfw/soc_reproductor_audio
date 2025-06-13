@echo off
setlocal

echo === HPS Audio Loader - Build and Deploy ===

REM Configurar letra de SD (cambiar si es necesario)
set SD_DRIVE=D:

REM Cambiar a directorio fuente
cd /d "%~dp0\..\hps_src"

echo === Compiling HPS Audio Loader ===

REM Limpiar archivos anteriores
if exist "hps_audio_loader" del "hps_audio_loader"

REM Compilar directamente con arm-linux-gnueabihf-gcc
echo Compiling with arm-linux-gnueabihf-gcc...
arm-linux-gnueabihf-gcc -Wall -O2 -std=c99 -march=armv7-a -mtune=cortex-a9 -mfpu=neon -mfloat-abi=hard -static -o hps_audio_loader hps_audio_loader.c

if not exist "hps_audio_loader" (
    echo.
    echo ERROR: Compilation failed
    echo.
    echo Make sure you have installed ARM cross-compiler:
    echo Download from: https://developer.arm.com/tools-and-software/open-source-software/developer-tools/gnu-toolchain/gnu-a/downloads
    echo Or install via: sudo apt install gcc-arm-linux-gnueabihf
    echo.
    pause
    exit /b 1
)

echo Compilation successful!
echo Binary size:
dir hps_audio_loader

echo.
echo === Deploying to SD Card ===

REM Verificar que existe la SD
if not exist %SD_DRIVE%\ (
    echo ERROR: SD Card not found at %SD_DRIVE%
    echo Change SD_DRIVE variable if needed
    pause
    exit /b 1
)

echo SD Card found at %SD_DRIVE%

REM Crear directorios necesarios
echo Creating directories...
mkdir "%SD_DRIVE%\home\root" 2>nul
mkdir "%SD_DRIVE%\media\sd\songs" 2>nul

REM Copiar archivos principales
echo Copying files...
copy "hps_audio_loader" "%SD_DRIVE%\home\root\" >nul
copy "install.sh" "%SD_DRIVE%\home\root\" >nul

REM Crear script de inicio automático
echo Creating auto-start script...
(
echo #!/bin/bash
echo cd /home/root
echo chmod +x hps_audio_loader
echo chmod +x install.sh
echo echo "Installing HPS Audio Loader..."
echo ./install.sh
) > "%SD_DRIVE%\home\root\setup.sh"

REM Crear archivos WAV de ejemplo vacíos
echo Creating sample WAV files...
fsutil file createnew "%SD_DRIVE%\media\sd\songs\song1.wav" 1024 >nul 2>&1
fsutil file createnew "%SD_DRIVE%\media\sd\songs\song2.wav" 1024 >nul 2>&1
fsutil file createnew "%SD_DRIVE%\media\sd\songs\song3.wav" 1024 >nul 2>&1

REM Crear README
echo Creating instructions...
(
echo HPS Audio Loader
echo ================
echo.
echo Files copied to SD:
echo - /home/root/hps_audio_loader  ^(main program^)
echo - /home/root/install.sh        ^(installer^)
echo - /home/root/setup.sh          ^(auto setup^)
echo - /media/sd/songs/song*.wav    ^(sample files^)
echo.
echo To install on HPS Linux:
echo 1. Boot DE1-SoC with this SD
echo 2. ssh root@192.168.1.10
echo 3. cd /home/root
echo 4. sudo ./setup.sh
echo.
echo Replace sample WAV files with real audio files
echo Files must be: 48kHz, 16-bit, stereo WAV format
) > "%SD_DRIVE%\README.txt"

echo.
echo === Build and Deploy Complete ===
echo Binary: %CD%\hps_audio_loader
echo SD Card: %SD_DRIVE%
echo.
echo Next steps:
echo 1. Replace sample WAV files with real ones in %SD_DRIVE%\media\sd\songs\
echo 2. Insert SD in DE1-SoC
echo 3. Boot Linux and run: sudo /home/root/setup.sh
echo.
pause