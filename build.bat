@echo off
rem Build DrumVstHost (Release) and copy the exe next to this file.
rem CMake crashes when the working directory contains non-ASCII characters,
rem so everything runs from E:\DevTools through an ASCII junction to this folder.
setlocal
set "HERE=%~dp0"
set "SRC=E:\DevTools\src\drum-vst-host"
set "BUILD=E:\DevTools\build\drum-vst-host"

if not exist "E:\DevTools\src" mkdir "E:\DevTools\src"
if not exist "%SRC%\CMakeLists.txt" mklink /J "%SRC%" "%HERE:~0,-1%" >nul

cd /d E:\DevTools
call "E:\DevTools\VSBuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
where cl >nul 2>&1
if errorlevel 1 (
    echo MSVC compiler not found. Check E:\DevTools\VSBuildTools.
    exit /b 1
)

cmake -S "%SRC%" -B "%BUILD%" -G Ninja -DCMAKE_BUILD_TYPE=Release
if errorlevel 1 exit /b 1

cmake --build "%BUILD%" --target DrumVstHost
if errorlevel 1 exit /b 1

copy /y "%BUILD%\DrumVstHost_artefacts\Release\DrumVstHost.exe" "%HERE%DrumVstHost.exe" >nul
if errorlevel 1 exit /b 1
if exist "%BUILD%\DrumVstHost_artefacts\Release\DrumVstHost.pdb" copy /y "%BUILD%\DrumVstHost_artefacts\Release\DrumVstHost.pdb" "%HERE%DrumVstHost.pdb" >nul

echo BUILD OK: %HERE%DrumVstHost.exe