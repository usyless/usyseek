@echo off
setlocal enabledelayedexpansion
pushd "%~dp0"

if /i "%~1"=="--build" (
    call build.bat
    if errorlevel 1 goto :fail
)

set "BIN=build\bin"
set "DIST=dist"
set "STAGE=%DIST%\stage"

for %%F in (usyseek.exe usyseek-ui.exe usyseek-indexer.exe onnxruntime.dll python\usyseek_vision.py python\requirements.txt) do (
    if not exist "%BIN%\%%F" (
        echo [package-release] Missing %BIN%\%%F. Run build.bat first or pass --build.
        goto :fail
    )
)

where 7z >nul 2>nul
if errorlevel 1 (
    echo [package-release] 7z.exe not found in PATH. Install 7-Zip to build usyseek.7z and usyseek.zip.
    goto :fail
)

if exist "%STAGE%" rmdir /s /q "%STAGE%"
if not exist "%DIST%" mkdir "%DIST%"
mkdir "%STAGE%"
mkdir "%STAGE%\python"

copy /y "%BIN%\usyseek.exe" "%STAGE%\usyseek.exe" >nul || goto :fail
copy /y "%BIN%\usyseek-ui.exe" "%STAGE%\usyseek-ui.exe" >nul || goto :fail
copy /y "%BIN%\usyseek-indexer.exe" "%STAGE%\usyseek-indexer.exe" >nul || goto :fail
copy /y "%BIN%\onnxruntime.dll" "%STAGE%\onnxruntime.dll" >nul || goto :fail
copy /y "%BIN%\python\usyseek_vision.py" "%STAGE%\python\usyseek_vision.py" >nul || goto :fail
copy /y "%BIN%\python\requirements.txt" "%STAGE%\python\requirements.txt" >nul || goto :fail
if exist "LICENSE" copy /y "LICENSE" "%STAGE%\LICENSE" >nul
if exist "licenses" xcopy /e /i /y /q "licenses" "%STAGE%\licenses" >nul

if exist "%DIST%\usyseek.7z" del /f /q "%DIST%\usyseek.7z"
if exist "%DIST%\usyseek.zip" del /f /q "%DIST%\usyseek.zip"

pushd "%STAGE%"
echo [package-release] Creating %DIST%\usyseek.7z...
7z a -t7z -mx=9 -ms=on "..\usyseek.7z" * >nul
if errorlevel 1 (
    popd
    goto :fail
)

echo [package-release] Creating %DIST%\usyseek.zip...
7z a -tzip -mx=9 "..\usyseek.zip" * >nul
if errorlevel 1 (
    popd
    goto :fail
)
popd

rmdir /s /q "%STAGE%"

echo.
echo [package-release] Done:
for %%A in ("%DIST%\usyseek.7z" "%DIST%\usyseek.zip") do (
    echo   %%~nxA  ^(%%~zA bytes^)
    for /f "skip=1 tokens=1" %%H in ('certutil -hashfile "%%~A" SHA256 ^| findstr /v "CertUtil"') do (
        echo     SHA256: %%H
    )
)

popd
endlocal
exit /b 0

:fail
if exist "%STAGE%" rmdir /s /q "%STAGE%"
popd
endlocal
exit /b 1
