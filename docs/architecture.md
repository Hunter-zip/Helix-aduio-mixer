# Architektura

## Podział na procesy

```
┌────────────────────────┐        TCP 127.0.0.1 / JSON-lines        ┌──────────────────────┐
│  helix-engine          │ ◄────────────────────────────────────────►│  GUI (Electron)      │
│  (proces silnika)      │                                           │  lub helix-cli       │
│                        │                                           └──────────────────────┘
│  wątek renderujący ────┼── WASAPI ──► karta dźwiękowa
│  wątki przechwytujące ─┼── WASAPI ◄── mikrofon / loopback / aplikacje
│  wątek sterujący       │
│  wątek transportu VA ──┼── pamięć współdzielona ──► wirtualne urządzenia
└────────────────────────┘
```

GUI jest osobnym procesem i nigdy nie dotyka buforów audio. Zamknięcie,
zawieszenie albo przeładowanie interfejsu nie przerywa przetwarzania —
to realizacja zasady ze specyfikacji §28.

## Przepływ sygnału

```
Źródła (mikrofon, loopback systemu, loopback aplikacji, wejścia wirtualne)
  │  bufory pierścieniowe SPSC (bez blokad)
  ▼
Kanały ── pomiar wejścia ── łańcuch DSP ── gain/pan/mute ── pomiar wyjścia
  │
  ▼
Macierz routingu (kanał × magistrala, rampowana w obrębie bloku)
  │
  ▼
Magistrale A1..An (fizyczne) i B1..Bn (wirtualne)
  │  głośność → Master → limiter → pomiar
  ▼
Urządzenia wyjściowe / transport wirtualny
```

## Wątki i kontrakt czasu rzeczywistego

| Wątek | Zadanie | Wolno alokować? |
|---|---|---|
| renderujący (callback urządzenia) | cały graf audio | **nie** |
| przechwytujące (jeden na strumień) | konwersja + zapis do bufora źródła | **nie** |
| sterujący | komendy, profile, wykrywanie aplikacji, sprzątanie | tak |
| transportu wirtualnego | przenoszenie próbek do/od klientów | **nie** |
| serwera sterującego | protokół, rozsyłanie pomiarów | tak |

W wątku audio nie ma alokacji, blokad, I/O ani wyjątków. Pilnuje tego test
`helix_rt_tests`, który podmienia globalny `operator new` i liczy alokacje
podczas 700 bloków pełnego przetwarzania (8 kanałów × 7 efektów, 2 magistrale).

## Bezpieczna wymiana struktury grafu

Dodanie kanału, usunięcie efektu czy zmiana przypisania źródła nie mogą
zatrzymać wątku audio ani zwolnić pamięci spod jego nóg. Mechanizm:

1. wątek sterujący buduje nową migawkę grafu (`AudioEngine::Graph`),
2. publikuje ją atomowo (`std::atomic<const Graph*>`, release),
3. stara migawka trafia na listę obiektów odstawionych ze znacznikiem
   licznika bloków,
4. `collectGarbage()` w wątku sterującym zwalnia dopiero to, co jest starsze
   o co najmniej dwa pełne bloki — wtedy wątek audio na pewno już tego nie widzi.

Parametry zmieniane często (głośność, mute, pan, parametry efektów, komórki
macierzy routingu) nie przechodzą przez ten mechanizm — są atomikami czytanymi
bezpośrednio przez wątek audio i wygładzanymi rampą w obrębie bloku.

## Rampy i brak trzasków

Każda zmiana, która mogłaby wywołać skok sygnału, jest interpolowana liniowo
wewnątrz bloku:

* głośność kanału i Mastera — rampa 20–25 ms,
* mute i solo — 12 ms,
* pan — 20 ms,
* komórka macierzy routingu — pełna długość bloku.

Test `engine/zmiana routingu w locie nie tworzy skoku sygnału` sprawdza, że
kolejne próbki po włączeniu wysyłki różnią się o mniej niż 2% pełnej skali.

## Zegary i dryft

Magistrala podstawowa („zegar”) jest wpięta bezpośrednio w callback urządzenia
— jej sygnał trafia do karty bez dodatkowego bufora (spec §14). Pozostałe
magistrale i wszystkie wejścia mają własne bufory pierścieniowe oraz resampler
Catmull-Rom z regulatorem proporcjonalnym: gdy bufor odjeżdża od punktu
docelowego, tempo jest korygowane o ułamek procenta. Dzięki temu urządzenia
o niezależnych zegarach nie rozjeżdżają się w nieskończoność.

## Moduły

```
engine/include/helix/
  Types.h, AudioBuffer.h, RingBuffer.h, SpscQueue.h, Parameter.h, Meter.h, Denormal.h
  dsp/      AudioPlugin, Biquad, Fft, Resampler, EffectChain, PluginRegistry,
            Gain, NoiseGate, NoiseSuppression, Equalizer, Compressor, DeEsser, Limiter
  core/     AudioEngine, Channel, Bus, RoutingMatrix, MasterBus, InputSource,
            ChannelManager, DeviceManager, AudioBackend, NullBackend
  virtualaudio/ SharedRingBuffer, DriverInterface, VirtualDeviceManager
  app/      Json, ProfileManager, AppDetection, HotkeyManager, AutomationEngine,
            EngineController, ControlServer
  platform/ WasapiBackend, WindowsAppDetector, WindowsHotkeys, WindowsDriverInterface
```

Warstwa `platform/` jest jedynym miejscem, które zna Windows. Dołożenie kolejnej
platformy to implementacja `AudioBackend`, `AppDetector`, `HotkeyBackend`
i `DriverInterface` — reszta silnika zostaje bez zmian.

## Rozszerzanie

* **Nowy efekt** — zaimplementuj `dsp::AudioPlugin` i zarejestruj w
  `PluginRegistry`. GUI zbuduje kontrolki automatycznie z opisu parametrów.
* **Nowy algorytm redukcji szumów** — zaimplementuj `NoiseSuppressorBackend`
  i zarejestruj w `NoiseSuppressorRegistry`. Podmiana działa w locie, bez
  zwalniania pamięci w wątku audio.
* **Nowy wyzwalacz/akcja automatyzacji** — wyzwalacz dopisz do `TriggerType`,
  akcją może być dowolna istniejąca komenda sterująca.
* **Nowa komenda** — dopisz gałąź w `EngineController::dispatch` i nazwę w
  `commandNames()`.
