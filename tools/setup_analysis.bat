@echo off
rem Optional: installs the tools behind "Metronome" (automatic beat detection) and
rem "Remove song drums" (Demucs). Everything else in DrumVstHost works without this.
rem
rem Needs Python 3.10-3.13 (https://www.python.org/downloads/ - tick "Add python.exe to PATH").
rem Installs into <app folder>\.venv (about 1.5 GB: PyTorch CPU + Demucs) and downloads
rem the htdemucs separation model once (about 100 MB). Nothing is installed system-wide.
setlocal
set "APP=%~dp0.."
set "VENV=%APP%\.venv"

where python >nul 2>&1
if errorlevel 1 (
    echo Python was not found. Install Python 3.10-3.13 from https://www.python.org/downloads/
    echo and tick "Add python.exe to PATH", then run this file again.
    pause
    exit /b 1
)

echo [1/4] Creating the Python environment in "%VENV%" ...
if not exist "%VENV%\Scripts\python.exe" python -m venv "%VENV%"
if errorlevel 1 goto failed

echo [2/4] Installing beat detection (numpy, scipy, soundfile) ...
"%VENV%\Scripts\python.exe" -m pip install --upgrade pip
"%VENV%\Scripts\python.exe" -m pip install numpy scipy soundfile
if errorlevel 1 goto failed

echo [3/4] Installing drum separation (PyTorch CPU + Demucs). This is the large download ...
"%VENV%\Scripts\python.exe" -m pip install torch --index-url https://download.pytorch.org/whl/cpu
if errorlevel 1 goto failed
"%VENV%\Scripts\python.exe" -m pip install "demucs>=4.1.0"
if errorlevel 1 goto failed

echo [4/4] Downloading the separation model (htdemucs) ...
"%VENV%\Scripts\python.exe" -c "from demucs.api import Separator; Separator(model='hf://htdemucs', device='cpu'); print('model ready')"
if errorlevel 1 goto failed

echo.
echo Done. Restart DrumVstHost: Metronome and "Remove song drums" are now available.
pause
exit /b 0

:failed
echo.
echo Setup did not finish. Check the message above (internet connection, disk space, Python version) and run this file again.
pause
exit /b 1
