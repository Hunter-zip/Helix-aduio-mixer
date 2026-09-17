# Protokół sterujący

Silnik nasłuchuje na `127.0.0.1` (domyślnie port `47811`) i wymienia z klientem
linie JSON zakończone `\n`.

Po starcie silnik zapisuje w katalogu konfiguracji plik
`control-endpoint.json` z adresem, portem i tokenem — klient nie musi niczego
zgadywać.

## Ramki

```jsonc
// żądanie
{"id": 1, "cmd": "channel.setVolume", "args": {"channel": "Game", "value": 0.5}}

// odpowiedź
{"id": 1, "ok": true,  "result": { /* … */ }}
{"id": 1, "ok": false, "error": "Nieznany kanał"}

// zdarzenie (bez pola id)
{"event": "meters", "data": { /* … */ }}
{"event": "hello",  "data": {"protocol": 1, "requiresAuth": true}}
```

## Uwierzytelnianie

Po połączeniu serwer wysyła `hello`. Jeśli `requiresAuth` jest `true`, pierwszą
komendą musi być `auth`:

```json
{"id": 0, "cmd": "auth", "args": {"token": "<token z control-token>"}}
```

Token jest generowany przy każdym starcie silnika i zapisywany z prawami tylko
dla właściciela. Porównanie jest odporne na pomiar czasu. Bez uwierzytelnienia
każda inna komenda kończy się błędem.

## Odwołania do obiektów

Kanały, magistrale i źródła można wskazywać liczbą (identyfikator) albo tekstem
(nazwa kanału/źródła, etykieta lub nazwa magistrali):

```json
{"cmd": "routing.set", "args": {"channel": "Music", "bus": "B1", "value": true}}
{"cmd": "routing.set", "args": {"channel": 3, "bus": 5, "value": true}}
```

## Komendy

### Silnik
| Komenda | Argumenty | Wynik |
|---|---|---|
| `engine.status` / `engine.snapshot` | — | pełna migawka stanu |
| `engine.meters` | — | pomiary wszystkich kanałów, magistral i Mastera |
| `engine.start` / `engine.stop` | — | start/stop strumieni urządzeń |
| `engine.resetStats` | — | zerowanie liczników xrunów i CPU |
| `engine.commands` | — | lista obsługiwanych komend |

### Urządzenia (§20)
| Komenda | Argumenty |
|---|---|
| `device.list` | — |
| `device.apply` | `sampleRate`, `blockFrames`, `channels`, `exclusive`, `followSystemDefault` |
| `device.test` | `deviceId`, `duration` |

### Kanały (§4)
`channel.list`, `channel.add` (`name`, `icon`, `defaultChain`), `channel.remove`,
`channel.rename` (`name`), `channel.setIcon` (`icon`), `channel.setVolume` (`value` 0–2),
`channel.setMute` / `channel.toggleMute`, `channel.setSolo`, `channel.setPan` (−1…1),
`channel.setGain` (dB), `channel.setEnabled`, `channel.clearClip`.

### Efekty (§8, §9, §25)
| Komenda | Argumenty |
|---|---|
| `effect.list` | `channel` |
| `effect.add` | `channel`, `type`, `position` |
| `effect.remove` | `channel`, `effect` |
| `effect.reorder` | `channel`, `order` (tablica identyfikatorów) |
| `effect.setEnabled` | `channel`, `effect`, `value` |
| `effect.setParam` | `channel`, `effect`, `param`, `value` |
| `effect.eqResponse` | `channel`, `effect`, `points` lub `frequencies` |

### Magistrale i routing (§6, §14)
`bus.list`, `bus.add` (`label`, `name`, `kind`), `bus.remove`, `bus.setVolume`,
`bus.setMute`, `bus.setLimiter` (`value`, `ceilingDb`), `bus.setDevice` (`deviceId`),
`bus.setFollowsMaster`, `bus.setPrimary`,
`routing.get`, `routing.set` (`channel`, `bus`, `value`), `routing.setGain`,
`master.get`, `master.setVolume`, `master.setMute`, `master.setLimiter`.

### Źródła i aplikacje (§5)
`source.list`, `source.add` (`name`, `kind`), `source.remove`, `source.assign` (`channel`),
`source.setGain`, `source.bindDevice` (`deviceId`, `loopback`),
`source.bindProcess` (`processId`, `executable`), `source.unbind`,
`app.list`, `app.assign` (`executable`, `channel`), `app.unassign`, `app.assignments`.

### Profile, skróty, automatyzacja (§15–§17)
`profile.list`, `profile.save` (`name`, `description`), `profile.load` (`name`),
`profile.delete`, `profile.rename` (`from`, `to`), `profile.duplicate`, `profile.active`,
`hotkey.list`, `hotkey.bind` (`id`, `combo`, `action`, `args`), `hotkey.unbind`, `hotkey.trigger`,
`automation.list`, `automation.add` (`rule`), `automation.remove`, `automation.setEnabled`.

### Wirtualne urządzenia (§7)
`virtual.list`, `virtual.setEnabled` (`id`, `enabled`), `virtual.bindOutput` (`id`, `bus`),
`virtual.bindInput` (`id`, `source`), `virtual.driverStatus`,
`virtual.install` (`package`), `virtual.update`, `virtual.uninstall`.

### Pozostałe
`plugins.list` — dostępne typy efektów.

## Zdarzenia

| Zdarzenie | Treść |
|---|---|
| `hello` | wersja protokołu, czy wymagane uwierzytelnienie |
| `meters` | pomiary (domyślnie 30 razy na sekundę, tylko gdy ktoś słucha) |

## Przykład z linii poleceń

```bash
helix-cli engine.status
helix-cli channel.setVolume channel=Game value=0.5
helix-cli routing.set channel=Microphone bus=B1 value=true
helix-cli effect.setParam channel=Microphone effect=12 param=threshold value=-30
helix-cli --watch          # podgląd zdarzeń pomiarowych
```
