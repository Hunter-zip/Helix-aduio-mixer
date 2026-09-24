'use strict';

// Nadzorca procesu silnika.
//
// GUI może działać bez silnika (pokaże stan „rozłączony”), a silnik bez GUI —
// to jest właśnie sedno wymagania z §28 specyfikacji. Ten moduł tylko ułatwia
// życie: jeśli silnik nie działa, uruchamia go i pilnuje restartu.

const { spawn } = require('node:child_process');
const fs = require('node:fs');
const path = require('node:path');
const { EventEmitter } = require('node:events');

const RESTART_DELAY_MS = 1500;
const MAX_RESTARTS = 5;

function executableName() {
  return process.platform === 'win32' ? 'helix-engine.exe' : 'helix-engine';
}

/** Szuka binarki silnika w typowych miejscach (instalacja i drzewo dewelopera). */
function findEngineBinary(explicitPath) {
  const candidates = [];
  if (explicitPath) candidates.push(explicitPath);
  if (process.env.HELIX_ENGINE_PATH) candidates.push(process.env.HELIX_ENGINE_PATH);
  if (process.resourcesPath) candidates.push(path.join(process.resourcesPath, 'engine', executableName()));

  const root = path.resolve(__dirname, '..', '..');
  candidates.push(
    path.join(root, 'build', executableName()),
    path.join(root, 'build', 'Release', executableName()),
    path.join(root, 'build', 'RelWithDebInfo', executableName()),
  );

  for (const candidate of candidates) {
    try {
      if (candidate && fs.existsSync(candidate)) return candidate;
    } catch {
      // ignorujemy niedostępne ścieżki
    }
  }
  return null;
}

class EngineProcess extends EventEmitter {
  constructor(options = {}) {
    super();
    this.configDir = options.configDir;
    this.binary = findEngineBinary(options.binaryPath);
    this.child = null;
    this.restarts = 0;
    this.stopping = false;
  }

  get available() {
    return Boolean(this.binary);
  }

  start() {
    if (this.child || !this.binary) return false;

    const args = [];
    if (this.configDir) args.push('--config-dir', this.configDir);

    this.stopping = false;
    this.child = spawn(this.binary, args, { stdio: ['ignore', 'pipe', 'pipe'] });

    this.child.stdout.on('data', (data) => this.emit('log', String(data)));
    this.child.stderr.on('data', (data) => this.emit('log', String(data)));

    this.child.on('exit', (code, signal) => {
      this.child = null;
      this.emit('exit', { code, signal });

      if (this.stopping || this.restarts >= MAX_RESTARTS) return;
      this.restarts += 1;
      // Silnik nie powinien padać — jeśli padnie, podnosimy go i mówimy o tym.
      setTimeout(() => this.start(), RESTART_DELAY_MS);
    });

    this.emit('started', this.binary);
    return true;
  }

  stop() {
    this.stopping = true;
    if (!this.child) return;
    this.child.kill();
    this.child = null;
  }
}

module.exports = { EngineProcess, findEngineBinary };
