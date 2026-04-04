# README.md

## Helix Audio Mixer

Nowoczesny mikser audio na Windowsa inspirowany SteelSeries GG, NVIDIA Broadcast i Voicemeeter Potato.

### 🔥 Co to robi?

Helix Audio Mixer daje ci pełną kontrolę nad dźwiękiem:

* sterujesz głośnością każdej aplikacji osobno
* ogarniasz routing audio (mikrofon, system, gry, Discord itd.)
* dodajesz efekty głosowe (EQ, compressor, noise gate)
* czyścisz mikrofon z szumów (RNNoise)
* integrujesz wszystko z OBS

To nie jest kolejny „panel głośności” — to centrum dowodzenia audio.

---

## 🚀 Status projektu

Aktualnie:

* ✔ Electron app działa
* ✔ UI + tray + autostart
* ✔ Overlay
* ⚠ Do poprawy: skalowanie tekstu (UI)

---

## 🧠 Stack technologiczny

* Electron
* Node.js
* Web Audio API
* WASAPI (planowane)
* RNNoise (WASM)

---

## 🗺 Roadmap

1. UI + Electron (✔ DONE)
2. Audio Routing
3. Native Audio Engine
4. Per-App Volume
5. Efekty głosowe
6. Noise Suppression
7. Makra
8. OBS Integration

---

## ⚙ Instalacja

```bash
npm install
npm start
```

---

## 💡 Wizja

Celem jest stworzenie czegoś lepszego niż Voicemeeter — prostszego w użyciu, ale potężniejszego pod maską.

Zero ograniczeń. Pełna kontrola.

---

## 🧨 Największe wyzwania

* niskie opóźnienia audio
* stabilność przy wielu źródłach
* integracja z Windows Audio API

---

## 🛠 Następne kroki

* naprawa skalowania UI
* start audio routingu

---

## 🤝 Wkład

Projekt w trakcie rozwoju — jeśli chcesz dorzucić coś od siebie, śmiało.

---

## 📜 Licencja

Do ustalenia
