@echo off
setlocal
pushd "%~dp0"

if exist "%USERPROFILE%\Documents\Portable Apps\_SCRIPTS\cmake.bat" call "%USERPROFILE%\Documents\Portable Apps\_SCRIPTS\cmake.bat"
if exist "%USERPROFILE%\Documents\Portable Apps\_SCRIPTS\ninja.bat" call "%USERPROFILE%\Documents\Portable Apps\_SCRIPTS\ninja.bat"
if exist "%USERPROFILE%\Documents\Portable Apps\_SCRIPTS\rust.bat" call "%USERPROFILE%\Documents\Portable Apps\_SCRIPTS\rust.bat"

if "%USYSEEK_PYTHON%"=="" (
    if exist "%LOCALAPPDATA%\Programs\Python\Python312\python.exe" set "USYSEEK_PYTHON=%LOCALAPPDATA%\Programs\Python\Python312"
)

set "PY_ARG="
if not "%USYSEEK_PYTHON%"=="" set "PY_ARG=-DPython_ROOT_DIR=%USYSEEK_PYTHON%"

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl -DCMAKE_EXPORT_COMPILE_COMMANDS=ON %PY_ARG%
if errorlevel 1 goto :fail
cmake --build build --parallel %*
if errorlevel 1 goto :fail

popd
endlocal
exit /b 0

:fail
popd
endlocal
exit /b 1
