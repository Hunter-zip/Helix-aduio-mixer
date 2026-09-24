#!/usr/bin/env bash
# ===========================================================================
#  Helix Audio Mixer — uruchomienie jedną komendą (Linux / macOS)
#
#  Poza Windows silnik pracuje na backendzie programowym: symuluje urządzenia
#  i pozwala rozwijać DSP, routing, profile i interfejs bez sprzętu Windows.
# ===========================================================================

set -euo pipefail
cd "$(dirname "$0")"

printf '\n  HELIX AUDIO MIXER\n  =================\n\n'

require() {
    if ! command -v "$1" >/dev/null 2>&1; then
        printf '  [BŁĄD] Nie znaleziono: %s\n\n  %s\n\n' "$1" "$2"
        exit 1
    fi
}

require cmake "Zainstaluj CMake: sudo apt install cmake  (lub https://cmake.org/download/)"
require node  "Zainstaluj Node.js LTS: https://nodejs.org/"

printf '  [1/3] Budowanie silnika audio...\n\n'
# HELIX_WERROR=OFF: ostrzeżenie kompilatora nie może zablokować uruchomienia.
cmake -S engine -B build -DCMAKE_BUILD_TYPE=Release -DHELIX_WERROR=OFF -DHELIX_BUILD_TESTS=OFF
cmake --build build --parallel

printf '\n  [2/3] Przygotowanie interfejsu...\n\n'
if [ ! -d ui/node_modules ]; then
    printf '  Pierwsze uruchomienie — pobieram Electron (około 100 MB)...\n\n'
    (cd ui && npm install)
fi

printf '\n  [3/3] Uruchamianie...\n\n'
cd ui && npm start
