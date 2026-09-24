@echo off
rem  Helix Audio Mixer - uruchomienie zestawu testow (Windows)

setlocal
cd /d "%~dp0"

echo.
echo   Budowanie z testami...
echo.

cmake -S engine -B build-test -DCMAKE_BUILD_TYPE=Release -DHELIX_WERROR=OFF
if errorlevel 1 goto :fail

cmake --build build-test --config Release --parallel
if errorlevel 1 goto :fail

echo.
echo   Testy...
echo.

ctest --test-dir build-test -C Release --output-on-failure
if errorlevel 1 goto :fail

echo.
echo   Wszystko przeszlo.
echo.
pause
endlocal
exit /b 0

:fail
echo.
echo   Cos poszlo nie tak - zobacz komunikaty powyzej.
echo.
pause
endlocal
exit /b 1
