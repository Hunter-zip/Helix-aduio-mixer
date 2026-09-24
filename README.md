# Helix Audio Mixer

Wirtualny mikser audio dla Windows 10/11. Przechwytuje dźwięk z aplikacji,
grupuje go w kanały, przetwarza w czasie rzeczywistym i kieruje na dowolną
liczbę wyjść jednocześnie.

Referencją funkcjonalną są SteelSeries Sonar i Voicemeeter — nie jako wzorzec
do kopiowania interfejsu czy nazewnictwa, tylko jako punkt odniesienia dla
zakresu funkcji.

```
┌──────────────────────────────────────────────────────────────────────┐
│ HELIX          Mikser  Routing  Urządzenia  Profile      Gaming ▼    │
├──────────────────────────────────────────────────────────────────────┤
│  GAME     CHAT     MUSIC    MEDIA    MICROPHONE   SYSTEM    AUX 1    │
│   ▓▓       ▓▓       ▓▓       ▓▓         ▓▓         ▓▓        ▓▓      │
│   ▓▓       ▓▓       ▓▓       ▓▓         ▓▓         ▓▓        ▓▓      │
│   ▓▓       ▓▓       ▓▓       ▓▓         ▓▓         ▓▓        ▓▓      │
│  ──●──    ──●──    ──●──    ──●──      ──●──      ──●──     ──●──    │
│   80%      65%      70%      50%        75%        40%       60%     │
│ MUTE SOLO  MUTE SOLO ...                                             │
├──────────────────────────────────────────────────────────────────────┤
│ MASTER  ▓▓▓▓▓▓▓▓░░░░  ────●────  85%  MUTE  LIMITER   Słuchawki ▼    │
└──────────────────────────────────────────────────────────────────────┘
```

## Co potrafi

* **Przechwytywanie per aplikacja** — Discord na kanał Chat, gra na Game,
  Spotify na Music. Przypisanie przeżywa restart aplikacji.
* **Routing jeden-do-wielu** — każdy kanał na dowolną liczbę wyjść naraz,
  przełączanie w czasie rzeczywistym, bez przerwy w dźwięku.
* **Pełny łańcuch DSP na kanał** — Gain → Noise Gate → Noise Suppression →
  EQ → Compressor → De-Esser → Limiter, z możliwością zmiany kolejności.
* **Equalizer 10-pasmowy** z wykresem charakterystyki liczonym przez silnik.
* **Mierniki RMS/Peak/Clip** dla każdego kanału, magistrali i Mastera.
* **Wirtualne urządzenia** — transport audio do innych aplikacji przez
  pamięć współdzieloną, plus moduł zarządzania sterownikiem systemowym.
* **Profile** (Gaming, Streaming, Music, Work, Movie) przełączane bez restartu.
* **Automatyzacja** — reguły „jeśli uruchomiono X, zrób Y”.
* **Globalne skróty klawiszowe** działające, gdy aplikacja jest w tle.

## Architektura w jednym zdaniu

Silnik audio jest osobnym procesem — GUI może się zawiesić, zrestartować albo
w ogóle nie istnieć, a dźwięk leci dalej.

```
helix-engine  ◄── TCP / JSON-lines ──►  GUI (Electron) albo helix-cli
```

Wątek audio nie alokuje pamięci, nie bierze blokad i nie robi I/O. Pilnuje tego
test, który podmienia globalny `operator new` i liczy alokacje podczas 700
bloków pełnego przetwarzania.

Szczegóły: [`docs/architecture.md`](docs/architecture.md).

## Uruchomienie

### Windows — dwa kliknięcia

1. Pobierz repozytorium (**Code → Download ZIP** albo `git clone`) i rozpakuj.
2. Kliknij dwukrotnie **`start.bat`**.

Skrypt sam sprawdzi wymagania, zbuduje silnik, pobierze zależności interfejsu
i uruchomi aplikację. Pierwsze uruchomienie trwa kilka minut (kompilacja +
pobranie Electrona), kolejne są natychmiastowe.

Potrzebujesz jednorazowo dwóch rzeczy — jeśli ich nie masz, skrypt powie
którą i poda link:

| Wymaganie | Skąd |
|---|---|
| CMake (z opcją „Add to PATH") | [cmake.org/download](https://cmake.org/download/) |
| Visual Studio 2022 Build Tools, komponent „Desktop development with C++" | [visualstudio.microsoft.com/downloads](https://visualstudio.microsoft.com/downloads/) |
| Node.js LTS | [nodejs.org](https://nodejs.org/) |

Testy: **`test.bat`**.

### Bez budowania — gotowe binarki

Zakładka **Actions** → dowolny zielony przebieg → artefakt
`helix-engine-windows-x64`. W środku `helix-engine.exe` i `helix-cli.exe`
gotowe do uruchomienia.

### Linux / macOS

```bash
./start.sh
```

Silnik pracuje wtedy na backendzie programowym — symuluje urządzenia, więc
DSP, routing, profile i interfejs działają bez sprzętu Windows.

### Ręcznie

```bash
cmake -S engine -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure

cd ui && npm install && npm start
```

### Sam silnik, bez GUI

```bash
./build/helix-engine
./build/helix-cli engine.status
./build/helix-cli channel.setVolume channel=Game value=0.5
./build/helix-cli --watch
```

Więcej: [`docs/building.md`](docs/building.md).

## Stan projektu

| Obszar | Stan |
|---|---|
| Silnik audio, routing, Master, mierniki | gotowe |
| DSP (7 efektów + EQ 10-pasmowy) | gotowe |
| Profile, automatyzacja, skróty | gotowe |
| Protokół sterujący i GUI | gotowe |
| WASAPI, wykrywanie aplikacji, process loopback | kod gotowy, zweryfikowany kompilacyjnie |
| Pakiet sterownika wirtualnego audio (.sys/.inf) | poza repozytorium — wymaga podpisu EV/WHQL |

Pełne zestawienie względem specyfikacji: [`docs/roadmap.md`](docs/roadmap.md).

**Uczciwie o ograniczeniach:** kod dla Windows (WASAPI, sesje audio, skróty,
instalacja sterownika) kompiluje się bez ostrzeżeń pod MSVC i mingw-w64, ale
nie był uruchamiany na realnym sprzęcie Windows w tym środowisku. Wszystko, co
niezależne od platformy — DSP, routing, profile, protokół, transport wirtualny —
jest pokryte testami, które przechodzą.

## Testy

```bash
ctest --test-dir build --output-on-failure   # 91 testów jednostkowych + kontrakt RT
cd ui && npm test                            # test integracyjny GUI ↔ silnik
```

Zakres: JSON (parser, escape'y, unicode, błędy, zapis atomowy), DSP (FFT, filtry,
kompresor, limiter, bramka, de-esser, redukcja szumów, resampler), silnik
(routing, mute/solo, Master, mierniki, hot-plug, zmiana formatu w locie,
modyfikacja struktury równolegle z przetwarzaniem), współbieżność (SPSC,
bufory pierścieniowe pod obciążeniem dwuwątkowym), profile (migracja schematu),
protokół (uwierzytelnianie, dyspozytor komend), transport wirtualny.

## Dokumentacja

| Plik | Zawartość |
|---|---|
| [`docs/architecture.md`](docs/architecture.md) | procesy, wątki, kontrakt RT, wymiana grafu |
| [`docs/control-protocol.md`](docs/control-protocol.md) | pełne API sterujące |
| [`docs/virtual-audio.md`](docs/virtual-audio.md) | wirtualne urządzenia, wymagania sterownika |
| [`docs/building.md`](docs/building.md) | budowanie, uruchamianie, cross-kompilacja |
| [`docs/roadmap.md`](docs/roadmap.md) | stan względem specyfikacji |

## Licencja

MIT — patrz [`LICENSE`](LICENSE).
