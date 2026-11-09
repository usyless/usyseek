@echo off
setlocal
pushd "%~dp0"

call "C:\Users\usy\Documents\Portable Apps\_SCRIPTS\cmake.bat"
call "C:\Users\usy\Documents\Portable Apps\_SCRIPTS\ninja.bat"
call "C:\Users\usy\Documents\Portable Apps\_SCRIPTS\rust.bat"

if "%USYSEEK_PYTHON%"=="" set "USYSEEK_PYTHON=C:\Users\usy\AppData\Local\Programs\Python\Python312"

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl -DCMAKE_EXPORT_COMPILE_COMMANDS=ON "-DPython_ROOT_DIR=%USYSEEK_PYTHON%"
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
