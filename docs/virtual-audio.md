# Wirtualne urządzenia audio

Specyfikacja §7 wymaga, żeby Helix udostępniał własne urządzenia audio widoczne
dla innych aplikacji Windows. Problem dzieli się na dwie niezależne części —
i tak też jest zaimplementowany.

## 1. Transport danych (gotowy, działa)

Każdy wirtualny punkt końcowy to nazwany segment pamięci współdzielonej
z bezblokadowym buforem pierścieniowym (`virtualaudio::SharedRingBuffer`):

```
┌──────────────────────┐        pamięć współdzielona        ┌────────────────────┐
│ silnik Helix         │  ── nagłówek + bufor pierścieniowy ►│ klient             │
│ magistrala B1        │                                     │ (sterownik / app)  │
└──────────────────────┘                                     └────────────────────┘
```

Układ segmentu:

| Pole | Typ | Znaczenie |
|---|---|---|
| `magic` | `uint32` | `"HLXR"` |
| `version` | `uint32` | wersja ABI (obecnie 1) |
| `channels`, `sampleRate`, `capacityFrames` | `uint32` | format transportu |
| `writeIndex`, `readIndex` | `atomic<uint64>` | pozycje SPSC |
| `overruns`, `underruns` | `atomic<uint64>` | diagnostyka |
| `producerAlive`, `consumerAlive` | `atomic<uint32>` | wykrywanie zerwania |
| dane | `float32` przeplot | `capacityFrames × channels` |

Nazwy segmentów: `Local\helix.<id>` na Windows, `/helix.<id>` na POSIX.
Identyfikatory domyślnych punktów końcowych:
`virtual-mixer-input`, `virtual-mixer-output`, `game-output`, `chat-output`,
`stream-output`, `microphone-output`.

Ta część jest w pełni zaimplementowana i pokryta testami
(`virtual/przepływ z magistrali do klienta`, `virtual/przepływ od klienta do źródła`).
Dowolna aplikacja, która zmapuje segment i będzie respektować nagłówek, dostanie
strumień audio z miksera — bez sterownika jądra.

## 2. Widoczność w systemie (wymaga osobnego pakietu sterownika)

Żeby Windows pokazał urządzenie na liście „Dźwięk”, potrzebny jest sterownik
trybu jądra. Tego **nie da się zrobić z poziomu aplikacji użytkownika** —
wymaga pakietu `.inf` + `.sys` + `.cat` podpisanego certyfikatem EV
i przepuszczonego przez atestację WHQL.

Helix zawiera moduł zarządzający takim pakietem (`DriverInterface`), a nie sam
pakiet. Moduł potrafi:

* sprawdzić stan instalacji (rejestr + enumeracja punktów końcowych),
* zainstalować i zaktualizować pakiet (`pnputil /add-driver … /install`),
* usunąć urządzenie i pakiet,
* zapisać żądaną liczbę wejść i wyjść.

Wszystkie operacje wymagają uprawnień administratora; moduł sam to sprawdza
i zwraca czytelny komunikat zamiast cicho zawodzić.

### Czego potrzebuje pakiet sterownika

| Element | Wymaganie |
|---|---|
| Model | AVStream (`portcls`) lub Audio Driver Sample z Windows Driver Samples |
| Identyfikator sprzętowy | `ROOT\HelixVirtualAudio` |
| Nazwy punktów końcowych | muszą zawierać `Helix` (po tym moduł je rozpoznaje) |
| Klucz konfiguracji | `HKLM\SOFTWARE\Helix\AudioMixer\Driver` (`Version`, `PackagePath`, `InputCount`, `OutputCount`) |
| Transport do miksera | komponent user-mode mapujący segmenty opisane wyżej |
| Podpis | certyfikat EV + atestacja WHQL (inaczej Windows go nie załaduje) |

### Stan bez sterownika

Bez zainstalowanego pakietu Helix nadal działa:

* miksuje, routuje i przetwarza dźwięk,
* udostępnia wirtualne wyjścia przez pamięć współdzieloną,
* raportuje w GUI stan `not-installed` wraz z wyjaśnieniem.

Nie podszywa się pod zainstalowany sterownik i nie udaje, że urządzenia są
widoczne w systemie, kiedy nie są.

## Integracja z OBS i innymi aplikacjami

Do czasu wydania pakietu sterownika najprostsza droga dla strumieniowania:

1. skieruj kanały na magistralę `B1` („Stream Output”),
2. włącz punkt końcowy `stream-output` w zakładce *Urządzenia*,
3. po stronie odbiornika zmapuj segment `Local\helix.stream-output`
   i czytaj z niego ramki float32 (nagłówek opisuje format).

Nagłówek i semantyka są stabilne w obrębie `version = 1`; zmiana układu pola
wymusi podbicie wersji, a klient o starszej wersji dostanie czytelny błąd
zamiast losowych próbek.
