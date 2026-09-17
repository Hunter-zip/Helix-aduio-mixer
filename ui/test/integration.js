#!/usr/bin/env node
'use strict';

// Test integracyjny GUI ↔ silnik.
//
// Uruchamia helix-engine na backendzie programowym i przechodzi przez ten sam
// protokół, którego używa interfejs. Dzięki temu zmiana w API silnika od razu
// wychodzi na jaw, zanim ktokolwiek odpali Electrona.
//
// Użycie: node test/integration.js [ścieżka-do-helix-engine]

const { spawn } = require('node:child_process');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');

const { EngineClient } = require('../src/engine-client');
const { findEngineBinary } = require('../src/engine-process');

const PORT = 47871;

let failures = 0;
function check(condition, label) {
  process.stdout.write(`${condition ? '  \x1b[32mOK\x1b[0m  ' : ' \x1b[31mFAIL\x1b[0m '} ${label}\n`);
  if (!condition) failures += 1;
}

async function main() {
  const binary = findEngineBinary(process.argv[2]);
  if (!binary) {
    console.error('Nie znaleziono helix-engine. Zbuduj silnik albo podaj ścieżkę jako argument.');
    process.exit(2);
  }

  const configDir = fs.mkdtempSync(path.join(os.tmpdir(), 'helix-ui-test-'));
  const engine = spawn(binary, [
    '--config-dir', configDir,
    '--null-backend',
    '--port', String(PORT),
    '--log-level', 'warn',
  ], { stdio: ['ignore', 'pipe', 'pipe'] });

  let engineLog = '';
  engine.stderr.on('data', (data) => { engineLog += String(data); });

  const cleanup = () => {
    engine.kill();
    fs.rmSync(configDir, { recursive: true, force: true });
  };

  // Silnik zapisuje plik endpointu dopiero po starcie serwera sterującego.
  await waitFor(() => fs.existsSync(path.join(configDir, 'control-endpoint.json')), 8000);

  const client = new EngineClient({ configDir });
  const ready = new Promise((resolve, reject) => {
    client.once('ready', resolve);
    setTimeout(() => reject(new Error('silnik nie odpowiedział')), 8000);
  });

  client.connect();
  await ready;

  try {
    const snapshot = await client.send('engine.status', {});
    check(Array.isArray(snapshot.channels) && snapshot.channels.length === 8,
          'migawka zawiera domyślne osiem kanałów');
    check(Array.isArray(snapshot.buses) && snapshot.buses.length === 4,
          'migawka zawiera cztery magistrale');
    check(typeof snapshot.stats.sampleRate === 'number', 'statystyki silnika są dostępne');
    check(Array.isArray(snapshot.plugins) && snapshot.plugins.length >= 7,
          'lista dostępnych efektów jest kompletna');

    const volume = await client.send('channel.setVolume', { channel: 'Game', value: 0.37 });
    check(Math.abs(volume.volume - 0.37) < 1e-5, 'ustawienie głośności kanału');

    const muted = await client.send('channel.toggleMute', { channel: 'Chat' });
    check(muted.muted === true, 'przełączenie wyciszenia');

    const routing = await client.send('routing.set', { channel: 'Music', bus: 'B2', value: true });
    const musicRow = routing.find((row) => row.name === 'Music');
    check(musicRow.buses.find((cell) => cell.label === 'B2').enabled === true,
          'zmiana routingu wchodzi natychmiast');

    const effects = await client.send('effect.list', { channel: 'Microphone' });
    check(effects.length === 7, 'mikrofon ma pełny łańcuch DSP ze specyfikacji');

    const equalizer = effects.find((effect) => effect.type === 'equalizer');
    await client.send('effect.setEnabled', { channel: 'Microphone', effect: equalizer.id, value: true });
    await client.send('effect.setParam',
                      { channel: 'Microphone', effect: equalizer.id, param: 'band4.enabled', value: 1 });
    await client.send('effect.setParam',
                      { channel: 'Microphone', effect: equalizer.id, param: 'band4.gain', value: 8 });

    const curve = await client.send('effect.eqResponse',
                                    { channel: 'Microphone', effect: equalizer.id, points: 64 });
    check(curve.gainDb.length === 64, 'krzywa EQ ma żądaną liczbę punktów');
    check(Math.max(...curve.gainDb) > 7, 'krzywa EQ pokazuje podbicie pasma');

    await client.send('profile.save', { name: 'UITest' });
    const profiles = await client.send('profile.list', {});
    check(profiles.some((profile) => profile.name === 'UITest'), 'zapis profilu');

    await client.send('channel.setVolume', { channel: 'Game', value: 0.9 });
    const restored = await client.send('profile.load', { name: 'UITest' });
    const game = restored.channels.find((channel) => channel.name === 'Game');
    check(Math.abs(game.volume - 0.37) < 1e-4, 'wczytanie profilu przywraca stan bez restartu');

    const meters = await client.send('engine.meters', {});
    check(Array.isArray(meters.channels) && meters.channels[0].output.peakL !== undefined,
          'pomiary mają strukturę oczekiwaną przez GUI');

    let sawMeterEvent = false;
    client.on('event', (event) => { if (event === 'meters') sawMeterEvent = true; });
    await delay(400);
    check(sawMeterEvent, 'silnik rozsyła zdarzenia pomiarowe');

    let rejected = false;
    try {
      await client.send('nie.ma.takiej.komendy', {});
    } catch {
      rejected = true;
    }
    check(rejected, 'nieznana komenda kończy się błędem, a nie ciszą');
  } catch (error) {
    check(false, `wyjątek: ${error.message}`);
  }

  client.disconnect();
  cleanup();

  if (failures > 0 && engineLog) console.error('\n--- log silnika ---\n' + engineLog);
  console.log(failures === 0 ? '\nGUI ↔ silnik: wszystko działa' : `\nGUI ↔ silnik: ${failures} błędów`);
  process.exit(failures === 0 ? 0 : 1);
}

function delay(ms) {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

async function waitFor(predicate, timeoutMs) {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    if (predicate()) return;
    await delay(50);
  }
  throw new Error('przekroczono czas oczekiwania na start silnika');
}

main().catch((error) => {
  console.error('Błąd testu:', error.message);
  process.exit(1);
});
