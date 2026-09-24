'use strict';

// Proces główny Electrona: okno, tray, nadzór nad silnikiem i most IPC.
//
// Wątek audio żyje w osobnym procesie (helix-engine). Ten proces może zostać
// zamknięty, przeładowany albo się zawiesić — dźwięk leci dalej (spec §28).

const { app, BrowserWindow, Tray, Menu, ipcMain, nativeImage, shell, dialog } = require('electron');
const path = require('node:path');
const os = require('node:os');
const { deflateSync } = require('node:zlib');

const { EngineClient } = require('./src/engine-client');
const { EngineProcess } = require('./src/engine-process');

const WINDOW_WIDTH = 1180;
const WINDOW_HEIGHT = 760;

let mainWindow = null;
let tray = null;
let engineClient = null;
let engineProcess = null;
let isQuitting = false;
let lastSnapshot = null;
let micMuted = false;

// ── Katalog konfiguracji (ten sam, którego używa silnik) ────────────────────

function configDirectory() {
  if (process.env.HELIX_CONFIG_DIR) return process.env.HELIX_CONFIG_DIR;
  if (process.platform === 'win32') {
    const appData = process.env.APPDATA || path.join(os.homedir(), 'AppData', 'Roaming');
    return path.join(appData, 'Helix', 'AudioMixer');
  }
  const configHome = process.env.XDG_CONFIG_HOME || path.join(os.homedir(), '.config');
  return path.join(configHome, 'helix-audio-mixer');
}

// ── Ikona tray generowana w locie (brak zewnętrznych zależności) ────────────

function trayIconBuffer() {
  const crc32 = (buffer) => {
    let c = 0xffffffff;
    for (const byte of buffer) {
      c ^= byte;
      for (let i = 0; i < 8; i += 1) c = (c >>> 1) ^ (0xedb88320 * (c & 1));
    }
    return (c ^ 0xffffffff) >>> 0;
  };

  const chunk = (type, data) => {
    const length = Buffer.allocUnsafe(4);
    length.writeUInt32BE(data.length);
    const tag = Buffer.from(type);
    const crc = Buffer.allocUnsafe(4);
    crc.writeUInt32BE(crc32(Buffer.concat([tag, data])));
    return Buffer.concat([length, tag, data, crc]);
  };

  const size = 16;
  const header = Buffer.alloc(13);
  header.writeUInt32BE(size, 0);
  header.writeUInt32BE(size, 4);
  header[8] = 8;
  header[9] = 6; // RGBA

  const pixels = Buffer.alloc(size * (1 + size * 4), 0);
  const set = (x, y, r, g, b, a = 255) => {
    if (x < 0 || x >= size || y < 0 || y >= size) return;
    const offset = y * (1 + size * 4) + 1 + x * 4;
    pixels[offset] = r;
    pixels[offset + 1] = g;
    pixels[offset + 2] = b;
    pixels[offset + 3] = a;
  };

  for (let y = 0; y < size; y += 1) {
    pixels[y * (1 + size * 4)] = 0;
    for (let x = 0; x < size; x += 1) set(x, y, 18, 18, 20);
  }

  // Trzy suwaki — czytelna sylwetka miksera nawet w 16 px.
  const bars = [
    { x: 4, top: 3, bottom: 13, knob: 6 },
    { x: 8, top: 3, bottom: 13, knob: 9 },
    { x: 12, top: 3, bottom: 13, knob: 4 },
  ];
  for (const bar of bars) {
    for (let y = bar.top; y <= bar.bottom; y += 1) set(bar.x, y, 90, 90, 96);
    for (let dx = -1; dx <= 1; dx += 1) {
      set(bar.x + dx, bar.knob, 235, 235, 235);
      set(bar.x + dx, bar.knob + 1, 235, 235, 235);
    }
  }

  return Buffer.concat([
    Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]),
    chunk('IHDR', header),
    chunk('IDAT', deflateSync(pixels)),
    chunk('IEND', Buffer.alloc(0)),
  ]);
}

// ── Okno ────────────────────────────────────────────────────────────────────

function createWindow() {
  mainWindow = new BrowserWindow({
    width: WINDOW_WIDTH,
    height: WINDOW_HEIGHT,
    minWidth: 900,
    minHeight: 600,
    frame: false,
    backgroundColor: '#0f0f11',
    show: false,
    webPreferences: {
      preload: path.join(__dirname, 'preload.js'),
      contextIsolation: true,
      nodeIntegration: false,
      sandbox: false,
    },
  });

  mainWindow.loadFile(path.join(__dirname, 'renderer', 'index.html'));
  mainWindow.once('ready-to-show', () => mainWindow.show());

  mainWindow.on('close', (event) => {
    if (isQuitting) return;
    event.preventDefault();
    mainWindow.hide();
  });

  if (process.env.HELIX_DEV) mainWindow.webContents.openDevTools({ mode: 'detach' });
}

// ── Tray ────────────────────────────────────────────────────────────────────

function buildTrayMenu() {
  return Menu.buildFromTemplate([
    { label: `Mikrofon: ${micMuted ? 'wyciszony' : 'aktywny'}`, enabled: false },
    { type: 'separator' },
    {
      label: 'Pokaż Helix',
      click: () => {
        if (!mainWindow) createWindow();
        mainWindow.show();
        mainWindow.focus();
      },
    },
    {
      label: 'Wycisz mikrofon',
      type: 'checkbox',
      checked: micMuted,
      click: () => sendCommand('channel.toggleMute', { channel: 'Microphone' }),
    },
    { type: 'separator' },
    {
      label: 'Uruchamiaj przy starcie systemu',
      type: 'checkbox',
      checked: app.getLoginItemSettings().openAtLogin,
      click: (item) => app.setLoginItemSettings({ openAtLogin: item.checked }),
    },
    { type: 'separator' },
    {
      label: 'Zamknij Helix',
      click: () => {
        isQuitting = true;
        app.quit();
      },
    },
  ]);
}

function createTray() {
  tray = new Tray(nativeImage.createFromBuffer(trayIconBuffer()));
  tray.setToolTip('Helix Audio Mixer');
  tray.setContextMenu(buildTrayMenu());
  tray.on('double-click', () => {
    if (!mainWindow) createWindow();
    if (mainWindow.isVisible()) mainWindow.hide();
    else mainWindow.show();
  });
}

function refreshTray() {
  if (!tray) return;
  tray.setContextMenu(buildTrayMenu());
  tray.setToolTip(`Helix — mikrofon ${micMuted ? 'wyciszony' : 'aktywny'}`);
}

// ── Silnik ──────────────────────────────────────────────────────────────────

function sendToRenderer(channel, payload) {
  if (mainWindow && !mainWindow.isDestroyed()) mainWindow.webContents.send(channel, payload);
}

async function sendCommand(command, args) {
  if (!engineClient) throw new Error('Klient silnika nie został utworzony');
  return engineClient.send(command, args);
}

function trackMicState(snapshot) {
  if (!snapshot || !Array.isArray(snapshot.channels)) return;
  const microphone = snapshot.channels.find((channel) => channel.name === 'Microphone');
  if (!microphone) return;
  if (microphone.muted !== micMuted) {
    micMuted = microphone.muted;
    refreshTray();
  }
}

function setupEngine() {
  const configDir = configDirectory();

  engineProcess = new EngineProcess({ configDir });
  engineClient = new EngineClient({ configDir });

  engineProcess.on('started', (binary) => sendToRenderer('engine:log', `Silnik uruchomiony: ${binary}`));
  engineProcess.on('log', (line) => sendToRenderer('engine:log', line));
  engineProcess.on('exit', ({ code }) => sendToRenderer('engine:log', `Silnik zakończył pracę (kod ${code})`));

  engineClient.on('status', (status) => sendToRenderer('engine:status', status));

  engineClient.on('ready', async () => {
    try {
      lastSnapshot = await engineClient.send('engine.status', {});
      trackMicState(lastSnapshot);
      sendToRenderer('engine:snapshot', lastSnapshot);
    } catch (error) {
      sendToRenderer('engine:log', `Nie udało się pobrać stanu: ${error.message}`);
    }
  });

  engineClient.on('event', (event, data) => {
    if (event === 'meters') sendToRenderer('engine:meters', data);
    else sendToRenderer('engine:event', { event, data });
  });

  // Najpierw próbujemy dołączyć do działającego silnika; dopiero potem go startujemy.
  engineClient.connect();
  setTimeout(() => {
    if (!engineClient.connected && engineProcess.available) engineProcess.start();
    else if (!engineClient.connected) {
      sendToRenderer('engine:log',
        'Nie znaleziono binarki helix-engine. Zbuduj silnik (cmake --build build) lub ustaw HELIX_ENGINE_PATH.');
    }
  }, 700);
}

// ── IPC ─────────────────────────────────────────────────────────────────────

function setupIpc() {
  ipcMain.handle('engine:command', async (_event, { command, args }) => {
    try {
      const result = await sendCommand(command, args || {});
      if (command === 'engine.status' || command === 'profile.load') {
        lastSnapshot = result;
        trackMicState(result);
      }
      return { ok: true, result };
    } catch (error) {
      return { ok: false, error: error.message };
    }
  });

  ipcMain.handle('engine:snapshot', async () => {
    try {
      lastSnapshot = await sendCommand('engine.status', {});
      trackMicState(lastSnapshot);
      return { ok: true, result: lastSnapshot };
    } catch (error) {
      return { ok: false, error: error.message };
    }
  });

  ipcMain.handle('engine:restart', async () => {
    if (!engineProcess) return { ok: false, error: 'Brak nadzorcy procesu' };
    engineProcess.restarts = 0;
    engineProcess.stop();
    setTimeout(() => engineProcess.start(), 500);
    return { ok: true };
  });

  ipcMain.handle('window:minimize', () => mainWindow && mainWindow.minimize());
  ipcMain.handle('window:hide', () => mainWindow && mainWindow.hide());
  ipcMain.handle('window:close', () => {
    isQuitting = true;
    app.quit();
  });
  ipcMain.handle('window:toggleAlwaysOnTop', () => {
    if (!mainWindow) return false;
    const value = !mainWindow.isAlwaysOnTop();
    mainWindow.setAlwaysOnTop(value);
    return value;
  });
  ipcMain.handle('window:isAlwaysOnTop', () => (mainWindow ? mainWindow.isAlwaysOnTop() : false));

  ipcMain.handle('app:autostart', (_event, enable) => {
    if (typeof enable === 'boolean') {
      app.setLoginItemSettings({ openAtLogin: enable });
      refreshTray();
    }
    return app.getLoginItemSettings().openAtLogin;
  });

  ipcMain.handle('app:version', () => app.getVersion());
  ipcMain.handle('app:configDir', () => configDirectory());
  ipcMain.handle('app:openConfigDir', () => shell.openPath(configDirectory()));

  ipcMain.handle('app:pickDriverPackage', async () => {
    const result = await dialog.showOpenDialog(mainWindow, {
      title: 'Wskaż pakiet sterownika (.inf)',
      filters: [{ name: 'Pakiet sterownika', extensions: ['inf'] }],
      properties: ['openFile'],
    });
    return result.canceled ? null : result.filePaths[0];
  });
}

// ── Cykl życia ──────────────────────────────────────────────────────────────

if (!app.requestSingleInstanceLock()) {
  app.quit();
} else {
  app.on('second-instance', () => {
    if (!mainWindow) createWindow();
    mainWindow.show();
    mainWindow.focus();
  });

  app.whenReady().then(() => {
    createWindow();
    createTray();
    setupIpc();
    setupEngine();
  });

  app.on('window-all-closed', () => {
    // Nie wychodzimy — aplikacja żyje w tray.
  });

  app.on('activate', () => {
    if (!mainWindow) createWindow();
    else mainWindow.show();
  });

  app.on('before-quit', () => {
    isQuitting = true;
    if (engineClient) engineClient.disconnect();
    // Silnika NIE zatrzymujemy, jeśli działał przed startem GUI — mógłby ucichnąć
    // dźwięk, którego użytkownik słucha. Zatrzymujemy tylko proces, który sami uruchomiliśmy.
    if (engineProcess) engineProcess.stop();
  });
}
