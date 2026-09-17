// Widok miksera: paski kanałów + sekcja Master (spec §18).

import { command } from '../api.js';
import { state, channelById, selectChannel } from '../state.js';
import { drawVerticalMeter, drawHorizontalMeter } from '../components/meter.js';
import { renderInspector } from './channel.js';
import { toast } from '../ui.js';

const ICONS = {
  gamepad: '🎮', chat: '💬', music: '🎵', media: '🎬',
  mic: '🎙️', system: '🖥️', aux: '🎚️', channel: '🎚️',
};

const stripCanvases = new Map();
let masterCanvas = null;

/** Tworzy pasek kanału. */
function buildStrip(channel) {
  const strip = document.createElement('div');
  strip.className = 'strip';
  strip.dataset.channel = String(channel.id);
  if (channel.muted) strip.classList.add('is-muted');
  if (state.selectedChannel === channel.id) strip.classList.add('is-selected');

  const head = document.createElement('div');
  head.className = 'strip-head';
  head.innerHTML =
    `<span class="strip-icon">${ICONS[channel.icon] || ICONS.channel}</span>` +
    `<span class="strip-name" title="${channel.name}"></span>`;
  head.querySelector('.strip-name').textContent = channel.name;
  head.addEventListener('click', () => selectChannel(channel.id));
  strip.appendChild(head);

  const meterBox = document.createElement('div');
  meterBox.className = 'strip-meter';
  const canvas = document.createElement('canvas');
  canvas.width = 28;
  canvas.height = 140;
  canvas.style.height = '100%';
  meterBox.appendChild(canvas);
  strip.appendChild(meterBox);
  stripCanvases.set(channel.id, canvas);

  const fader = document.createElement('input');
  fader.type = 'range';
  fader.className = 'strip-fader';
  fader.min = '0';
  fader.max = '100';
  fader.value = String(Math.round(channel.volume * 100));

  const value = document.createElement('span');
  value.className = 'strip-value';
  value.textContent = `${Math.round(channel.volume * 100)}%`;

  fader.addEventListener('input', () => {
    value.textContent = `${fader.value}%`;
    command('channel.setVolume', { channel: channel.id, value: Number(fader.value) / 100 })
      .catch((error) => toast(error.message, 'error'));
  });

  strip.appendChild(fader);
  strip.appendChild(value);

  const buttons = document.createElement('div');
  buttons.className = 'strip-buttons';

  const mute = document.createElement('button');
  mute.className = `chip${channel.muted ? ' is-on' : ''}`;
  mute.textContent = 'MUTE';
  mute.addEventListener('click', () => {
    command('channel.setMute', { channel: channel.id, value: !channel.muted })
      .catch((error) => toast(error.message, 'error'));
  });

  const solo = document.createElement('button');
  solo.className = `chip${channel.solo ? ' is-on-warn' : ''}`;
  solo.textContent = 'SOLO';
  solo.addEventListener('click', () => {
    command('channel.setSolo', { channel: channel.id, value: !channel.solo })
      .catch((error) => toast(error.message, 'error'));
  });

  buttons.appendChild(mute);
  buttons.appendChild(solo);
  strip.appendChild(buttons);

  return strip;
}

export function renderMixer() {
  const container = document.getElementById('strips');
  const snapshot = state.snapshot;

  stripCanvases.clear();
  container.innerHTML = '';

  if (!snapshot || !Array.isArray(snapshot.channels)) {
    container.innerHTML = '<div class="inspector-empty">Brak połączenia z silnikiem</div>';
    return;
  }

  for (const channel of snapshot.channels) container.appendChild(buildStrip(channel));

  renderMaster();
  renderInspector();
}

function renderMaster() {
  const snapshot = state.snapshot;
  if (!snapshot || !snapshot.master) return;

  const master = snapshot.master;
  const fader = document.getElementById('master-volume');
  const value = document.getElementById('master-value');
  const mute = document.getElementById('master-mute');
  const limiter = document.getElementById('master-limiter');
  const deviceSelect = document.getElementById('master-device');

  fader.value = String(Math.round(master.volume * 100));
  value.textContent = `${Math.round(master.volume * 100)}%`;
  mute.classList.toggle('is-on', Boolean(master.muted));
  limiter.classList.toggle('is-on-accent', Boolean(master.limiter));

  masterCanvas = document.getElementById('master-meter');

  // Wybór urządzenia magistrali podstawowej.
  const primaryBus = (snapshot.buses || []).find((bus) => bus.primary);
  const renderDevices = (snapshot.devices && snapshot.devices.render) || [];

  deviceSelect.innerHTML = '';
  const none = document.createElement('option');
  none.value = '';
  none.textContent = '— brak —';
  deviceSelect.appendChild(none);

  for (const device of renderDevices) {
    const option = document.createElement('option');
    option.value = device.id;
    option.textContent = device.default ? `${device.name} (domyślne)` : device.name;
    deviceSelect.appendChild(option);
  }

  if (primaryBus) deviceSelect.value = primaryBus.deviceId || '';
  deviceSelect.dataset.bus = primaryBus ? String(primaryBus.id) : '';
}

/** Jednorazowa inicjalizacja zdarzeń sekcji Master. */
export function initMixer() {
  const fader = document.getElementById('master-volume');
  const value = document.getElementById('master-value');

  fader.addEventListener('input', () => {
    value.textContent = `${fader.value}%`;
    command('master.setVolume', { value: Number(fader.value) / 100 })
      .catch((error) => toast(error.message, 'error'));
  });

  document.getElementById('master-mute').addEventListener('click', () => {
    const muted = state.snapshot && state.snapshot.master ? state.snapshot.master.muted : false;
    command('master.setMute', { value: !muted }).catch((error) => toast(error.message, 'error'));
  });

  document.getElementById('master-limiter').addEventListener('click', () => {
    const enabled = state.snapshot && state.snapshot.master ? state.snapshot.master.limiter : true;
    command('master.setLimiter', { value: !enabled }).catch((error) => toast(error.message, 'error'));
  });

  document.getElementById('master-device').addEventListener('change', (event) => {
    const busId = event.target.dataset.bus;
    if (!busId) return;
    command('bus.setDevice', { bus: Number(busId), deviceId: event.target.value })
      .catch((error) => toast(error.message, 'error'));
  });
}

/** Rysowanie mierników — wołane z pętli animacji, nie przebudowuje DOM. */
export function paintMeters() {
  const meters = state.meters;
  if (!meters) return;

  for (const entry of meters.channels || []) {
    const canvas = stripCanvases.get(entry.id);
    if (canvas) drawVerticalMeter(canvas, entry.output);
  }

  if (masterCanvas) drawHorizontalMeter(masterCanvas, meters.master);
}
