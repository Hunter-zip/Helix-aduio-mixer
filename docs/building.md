# Budowanie i uruchamianie

## Najkrótsza droga

| System | Komenda |
|---|---|
| Windows | dwuklik w `start.bat` (testy: `test.bat`) |
| Linux / macOS | `./start.sh` |

Skrypty sprawdzają wymagania, budują silnik, instalują zależności interfejsu
i uruchamiają aplikację. Budują z `HELIX_WERROR=OFF` — ostrzeżenie kompilatora
nie ma prawa zablokować uruchomienia programu na cudzej maszynie.

Reszta tego dokumentu opisuje, co te skrypty robią pod spodem, i jak zejść
niżej, gdy chcesz nad czymś zapanować ręcznie.

## Wymagania

| Element | Wersja |
|---|---|
| CMake | ≥ 3.20 |
| Kompilator C++ | MSVC 2022, GCC 12+ lub Clang 15+ (C++20) |
| Node.js | ≥ 18 (tylko dla GUI) |

## Silnik

```bash
cmake -S engine -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Powstają: `helix-engine` (proces silnika), `helix-cli` (klient diagnostyczny),
`helix_tests` i `helix_rt_tests`.

### Opcje

| Opcja | Domyślnie | Znaczenie |
|---|---|---|
| `HELIX_BUILD_TESTS` | `ON` | buduj testy |
| `HELIX_WERROR` | `ON` | ostrzeżenia jako błędy |

## GUI

```bash
cd ui
npm install
npm start
```

GUI szuka działającego silnika, a jeśli go nie znajdzie — uruchamia binarkę
z `build/`. Ścieżkę można wskazać zmienną `HELIX_ENGINE_PATH`.

Test integracyjny protokołu (nie wymaga Electrona):

```bash
cd ui && npm test
```

## Uruchomienie samego silnika

```bash
./build/helix-engine --config-dir ~/.config/helix-audio-mixer
./build/helix-cli engine.status
```

Najważniejsze przełączniki: `--sample-rate`, `--block`, `--exclusive`,
`--profile`, `--port`, `--null-backend`, `--log-level`, `--no-hotkeys`.
Pełna lista: `helix-engine --help`.

## Sanitizery

Zestaw testów przechodzi czysto pod ASan, UBSan i TSan:

```bash
cmake -S engine -B build-san -DHELIX_WERROR=OFF \
      -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -g -O1" \
      -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
cmake --build build-san -j --target helix_tests
./build-san/tests/helix_tests

# wyścigi danych
cmake -S engine -B build-tsan -DHELIX_WERROR=OFF \
      -DCMAKE_CXX_FLAGS="-fsanitize=thread -g -O1" \
      -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread"
cmake --build build-tsan -j --target helix_tests
./build-tsan/tests/helix_tests
```

`helix_rt_tests` podmienia globalny `operator new`, żeby policzyć alokacje
w callbacku audio — z sanitizerami nie da się tego sensownie łączyć, więc pod
nimi uruchamiamy tylko zestaw główny.

## Praca poza Windows

Silnik buduje się i działa na Linuksie z backendem programowym
(`NullBackend`) — symuluje urządzenia i napędza graf własnym zegarem.
To pozwala rozwijać DSP, routing, profile i protokół bez sprzętu Windows.

## Weryfikacja warstwy Windows spod Linuksa

```bash
sudo apt-get install g++-mingw-w64-x86-64
cmake -S engine -B build-win -DCMAKE_TOOLCHAIN_FILE=$PWD/engine/cmake/mingw-w64-x86_64.cmake
cmake --build build-win -j
```

Cały kod WASAPI, wykrywania aplikacji, skrótów i modułu sterownika przechodzi
wtedy kompilację z `-Werror`. To weryfikacja poprawności kodu, nie zamiennik
testów na realnym sprzęcie.
