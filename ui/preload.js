'use strict';

// Most między procesem głównym a rendererem. Renderer nie ma dostępu do Node,
// do gniazd ani do plików — tylko do tego, co jest tu wystawione.

const { contextBridge, ipcRenderer } = require('electron');

const listeners = new Map();

function subscribe(channel, callback) {
  const handler = (_event, payload) => callback(payload);
  ipcRenderer.on(channel, handler);
  listeners.set(callback, { channel, handler });
  return () => {
    const entry = listeners.get(callback);
    if (!entry) return;
    ipcRenderer.removeListener(entry.channel, entry.handler);
    listeners.delete(callback);
  };
}

contextBridge.exposeInMainWorld('helix', {
  engine: {
    /** Wysyła komendę do silnika. Zwraca { ok, result } albo { ok:false, error }. */
    command: (command, args) => ipcRenderer.invoke('engine:command', { command, args }),
    snapshot: () => ipcRenderer.invoke('engine:snapshot'),
    restart: () => ipcRenderer.invoke('engine:restart'),

    onSnapshot: (callback) => subscribe('engine:snapshot', callback),
    onMeters: (callback) => subscribe('engine:meters', callback),
    onStatus: (callback) => subscribe('engine:status', callback),
    onEvent: (callback) => subscribe('engine:event', callback),
    onLog: (callback) => subscribe('engine:log', callback),
  },

  window: {
    minimize: () => ipcRenderer.invoke('window:minimize'),
    hide: () => ipcRenderer.invoke('window:hide'),
    close: () => ipcRenderer.invoke('window:close'),
    toggleAlwaysOnTop: () => ipcRenderer.invoke('window:toggleAlwaysOnTop'),
    isAlwaysOnTop: () => ipcRenderer.invoke('window:isAlwaysOnTop'),
  },

  app: {
    version: () => ipcRenderer.invoke('app:version'),
    autostart: (enable) => ipcRenderer.invoke('app:autostart', enable),
    configDir: () => ipcRenderer.invoke('app:configDir'),
    openConfigDir: () => ipcRenderer.invoke('app:openConfigDir'),
    pickDriverPackage: () => ipcRenderer.invoke('app:pickDriverPackage'),
  },
});
