'use strict';

// Klient protokołu sterującego silnika (JSON-lines po TCP).
//
// Żyje w procesie głównym Electrona. Renderer nigdy nie rozmawia z silnikiem
// bezpośrednio — dzięki temu awaria okna nie zrywa sesji sterującej.

const net = require('node:net');
const fs = require('node:fs');
const path = require('node:path');
const { EventEmitter } = require('node:events');

const DEFAULT_PORT = 47811;
const RECONNECT_DELAY_MS = 1000;
const REQUEST_TIMEOUT_MS = 8000;

class EngineClient extends EventEmitter {
  constructor(options = {}) {
    super();
    this.configDir = options.configDir;
    this.host = options.host || '127.0.0.1';
    this.port = options.port || DEFAULT_PORT;
    this.token = options.token || '';

    this.socket = null;
    this.buffer = '';
    this.nextId = 1;
    this.pending = new Map();
    this.connected = false;
    this.authenticated = false;
    this.shouldReconnect = true;
    this.reconnectTimer = null;
  }

  /** Odczytuje port i token zapisane przez silnik przy starcie. */
  loadEndpoint() {
    if (!this.configDir) return false;
    try {
      const file = path.join(this.configDir, 'control-endpoint.json');
      const data = JSON.parse(fs.readFileSync(file, 'utf8'));
      if (data.address) this.host = data.address;
      if (data.port) this.port = data.port;
      if (data.token) this.token = data.token;
      return true;
    } catch {
      return false;
    }
  }

  connect() {
    if (this.socket) return;
    this.shouldReconnect = true;
    this.loadEndpoint();

    const socket = net.createConnection({ host: this.host, port: this.port });
    this.socket = socket;
    socket.setNoDelay(true);
    socket.setEncoding('utf8');

    socket.on('connect', () => {
      this.connected = true;
      this.buffer = '';
      this.emit('status', { connected: true, authenticated: false });
    });

    socket.on('data', (chunk) => this._onData(chunk));

    socket.on('error', (error) => {
      this.emit('status', { connected: false, authenticated: false, error: error.message });
    });

    socket.on('close', () => {
      this.connected = false;
      this.authenticated = false;
      this.socket = null;
      this._rejectAll('Połączenie z silnikiem zostało zerwane');
      this.emit('status', { connected: false, authenticated: false });
      this._scheduleReconnect();
    });
  }

  disconnect() {
    this.shouldReconnect = false;
    if (this.reconnectTimer) {
      clearTimeout(this.reconnectTimer);
      this.reconnectTimer = null;
    }
    if (this.socket) {
      this.socket.destroy();
      this.socket = null;
    }
    this._rejectAll('Rozłączono');
  }

  _scheduleReconnect() {
    if (!this.shouldReconnect || this.reconnectTimer) return;
    this.reconnectTimer = setTimeout(() => {
      this.reconnectTimer = null;
      this.connect();
    }, RECONNECT_DELAY_MS);
  }

  _onData(chunk) {
    this.buffer += chunk;

    let newline;
    while ((newline = this.buffer.indexOf('\n')) >= 0) {
      const line = this.buffer.slice(0, newline).trim();
      this.buffer = this.buffer.slice(newline + 1);
      if (!line) continue;

      let message;
      try {
        message = JSON.parse(line);
      } catch {
        continue;
      }
      this._handleMessage(message);
    }
  }

  _handleMessage(message) {
    if (typeof message.event === 'string') {
      if (message.event === 'hello') {
        this._authenticate(message.data && message.data.requiresAuth);
      } else {
        this.emit('event', message.event, message.data);
      }
      return;
    }

    const pending = this.pending.get(message.id);
    if (!pending) return;

    this.pending.delete(message.id);
    clearTimeout(pending.timer);

    if (message.ok) pending.resolve(message.result);
    else pending.reject(new Error(message.error || 'Nieznany błąd silnika'));
  }

  async _authenticate(required) {
    if (!required) {
      this.authenticated = true;
      this.emit('status', { connected: true, authenticated: true });
      this.emit('ready');
      return;
    }

    try {
      await this.send('auth', { token: this.token });
      this.authenticated = true;
      this.emit('status', { connected: true, authenticated: true });
      this.emit('ready');
    } catch (error) {
      this.emit('status', { connected: true, authenticated: false, error: error.message });
    }
  }

  /** Wysyła komendę i czeka na odpowiedź. */
  send(command, args = {}) {
    return new Promise((resolve, reject) => {
      if (!this.socket || !this.connected) {
        reject(new Error('Silnik nie jest połączony'));
        return;
      }

      const id = this.nextId++;
      const timer = setTimeout(() => {
        this.pending.delete(id);
        reject(new Error(`Przekroczono czas odpowiedzi: ${command}`));
      }, REQUEST_TIMEOUT_MS);

      this.pending.set(id, { resolve, reject, timer });
      this.socket.write(`${JSON.stringify({ id, cmd: command, args })}\n`);
    });
  }

  _rejectAll(reason) {
    for (const [, pending] of this.pending) {
      clearTimeout(pending.timer);
      pending.reject(new Error(reason));
    }
    this.pending.clear();
  }
}

module.exports = { EngineClient, DEFAULT_PORT };
