@echo off
setlocal
set "VCVARS=E:\VisualStudio\VS2026\VC\Auxiliary\Build\vcvars64.bat"
set "MOSQUITTO_DIR=C:\Program Files\mosquitto"
if not defined OUTPUT_EXE set "OUTPUT_EXE=zigbee_reader.exe"

if not exist "%VCVARS%" (
    echo Khong tim thay MSVC: %VCVARS%
    exit /b 1
)
if not exist "%MOSQUITTO_DIR%\devel\mosquitto.lib" (
    echo Khong tim thay mosquitto.lib
    exit /b 1
)

call "%VCVARS%" >nul
cl /nologo /W4 /O2 /std:c11 main.c dashboard.c /I"%MOSQUITTO_DIR%\devel" /Fe:"%OUTPUT_EXE%" /link /LIBPATH:"%MOSQUITTO_DIR%\devel" mosquitto.lib ws2_32.lib
if errorlevel 1 exit /b %errorlevel%

echo Build thanh cong: %OUTPUT_EXE%
