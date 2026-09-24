# Stan realizacji względem specyfikacji

Kolejność etapów zgodna z §27.

## Etap 1 — Audio Engine ✔
* inicjalizacja urządzeń, wejścia/wyjścia, buforowanie — `DeviceManager`, `AudioBackend`
* routing — `RoutingMatrix` (kanał × magistrala, rampowana)
* Master — `MasterBus`, bez dodatkowego buforowania
* VU/Peak meter — `LevelMeter`, wartości liczone w silniku

## Etap 2 — Virtual Audio ◐
* transport między procesami — `SharedRingBuffer` ✔
* zarządzanie punktami końcowymi — `VirtualDeviceManager` ✔
* moduł instalacji sterownika — `DriverInterface` + wariant Windows ✔
* **pakiet sterownika jądra — poza tym repozytorium** (wymaga podpisu EV/WHQL,
  szczegóły w `virtual-audio.md`)

## Etap 3 — Application Routing ✔ (Windows) / ◐ (poza Windows)
* wykrywanie aplikacji — `WindowsAppDetector` (IAudioSessionManager2)
* przechwytywanie strumienia aplikacji — process loopback (Windows 10 20H1+)
* przypisania aplikacja → kanał, trwałe po restarcie aplikacji — `AppAssignments`
* poza Windows: `ManualAppDetector` (sterowany programowo, używany w testach)

## Etap 4 — DSP ✔
Gain, Noise Gate, Noise Suppression (STFT + minimum statistics),
Equalizer (10 pasm, 8 typów filtrów), Compressor (miękkie kolano, peak/RMS),
De-Esser (zwrotnica LR2), Limiter (look-ahead, brickwall).
Kolejność efektów zmienialna w locie, każdy z osobnym bypassem.

## Etap 5 — GUI ✔
Mikser, panel kanału, macierz routingu, konfiguracja urządzeń, profile.
Interfejs w osobnym procesie, komunikacja przez protokół sterujący.

## Etap 6 — Profile i automatyzacja ✔
Profile JSON z wersjonowaniem i migracją, przypisania aplikacji,
reguły automatyzacji (wyzwalacz → warunek → akcje), globalne skróty klawiszowe.

## Etap 7 — Optymalizacja ◐
* kontrakt RT zweryfikowany testem liczącym alokacje ✔
* kompensacja dryftu zegarów ✔
* hot-plug i zmiana urządzenia domyślnego ✔
* testy obciążeniowe na realnym sprzęcie Windows — do wykonania
* strojenie pod tryb wyłączny WASAPI < 5 ms — do wykonania

## Czego świadomie nie ma

| Element | Powód |
|---|---|
| Pakiet sterownika (.sys/.inf/.cat) | wymaga certyfikatu EV i atestacji WHQL — osobny artefakt wydawniczy |
| Backend AI do redukcji szumów | interfejs `NoiseSuppressorBackend` jest gotowy; model to osobna zależność |
| Hosting pluginów VST3/CLAP | `AudioPlugin` jest przygotowany pod adapter, samego adaptera brak |
| Testy na realnym sprzęcie Windows | kod WASAPI jest zweryfikowany kompilacyjnie (mingw-w64), nie uruchomieniowo |
