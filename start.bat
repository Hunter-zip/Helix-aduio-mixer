@echo off
rem ==========================================================================
rem  Helix Audio Mixer - uruchomienie jednym kliknieciem (Windows)
rem
rem  Buduje silnik, instaluje zaleznosci interfejsu i odpala aplikacje.
rem  Kolejne uruchomienia sa szybkie - przebudowuje sie tylko to, co sie zmienilo.
rem ==========================================================================

setlocal enabledelayedexpansion
cd /d "%~dp0"

echo.
echo   HELIX AUDIO MIXER
echo   =================
echo.

rem ---- Sprawdzenie wymagan -------------------------------------------------

where cmake >nul 2>&1
if errorlevel 1 (
    echo   [BLAD] Nie znaleziono CMake.
    echo.
    echo   Zainstaluj: https://cmake.org/download/
    echo   Przy instalacji zaznacz "Add CMake to the system PATH".
    echo.
    goto :fail
)

where node >nul 2>&1
if errorlevel 1 (
    echo   [BLAD] Nie znaleziono Node.js.
    echo.
    echo   Zainstaluj wersje LTS: https://nodejs.org/
    echo.
    goto :fail
)

rem ---- Budowanie silnika ---------------------------------------------------

echo   [1/3] Budowanie silnika audio...
echo.

if not exist "build\CMakeCache.txt" (
    rem HELIX_WERROR=OFF: ostrzezenie kompilatora nie ma prawa zablokowac
    rem uruchomienia programu na maszynie uzytkownika.
    cmake -S engine -B build -DCMAKE_BUILD_TYPE=Release -DHELIX_WERROR=OFF -DHELIX_BUILD_TESTS=OFF
    if errorlevel 1 (
        echo.
        echo   [BLAD] Konfiguracja CMake nie powiodla sie.
        echo   Najczestsza przyczyna: brak kompilatora C++.
        echo   Zainstaluj "Visual Studio 2022 Build Tools" z komponentem
        echo   "Desktop development with C++": https://visualstudio.microsoft.com/downloads/
        echo.
        goto :fail
    )
)

cmake --build build --config Release --parallel
if errorlevel 1 (
    echo.
    echo   [BLAD] Budowanie silnika nie powiodlo sie.
    goto :fail
)

rem ---- Zaleznosci interfejsu ----------------------------------------------

echo.
echo   [2/3] Przygotowanie interfejsu...
echo.

if not exist "ui\node_modules" (
    echo   Pierwsze uruchomienie - pobieram Electron ^(okolo 100 MB^)...
    echo.
    pushd ui
    call npm install
    if errorlevel 1 (
        popd
        echo.
        echo   [BLAD] npm install nie powiodl sie. Sprawdz polaczenie z internetem.
        goto :fail
    )
    popd
)

rem ---- Start ---------------------------------------------------------------

echo.
echo   [3/3] Uruchamianie...
echo.

pushd ui
call npm start
popd

endlocal
exit /b 0

:fail
echo.
pause
endlocal
exit /b 1
