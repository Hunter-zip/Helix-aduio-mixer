// Punkt wejścia renderera: spina widoki, stan i most do silnika.

import { snapshot as fetchSnapshot, onSnapshot, onMeters, onStatus, onEvent, onLog,
         command, windowApi } from './api.js';
import { state, setSnapshot, setMeters, setConnection, setView, subscribe, notify } from './state.js';
import { renderMixer, initMixer, paintMeters } from './views/mixer.js';
import { renderInspector } from './views/channel.js';
import { renderRouting } from './views/routing.js';
import { renderDevices, initDevices } from './views/devices.js';
import { renderProfilesView, initProfiles } from './views/profiles.js';
import { toast } from './ui.js';

// ── Nawigacja ───────────────────────────────────────────────

function initTabs() {
  const tabs = document.getElementById('tabs');
  tabs.addEventListener('click', (event) => {
    const button = event.target.closest('.tab');
    if (!button) return;

    for (const tab of tabs.querySelectorAll('.tab')) tab.classList.remove('is-active');
    button.classList.add('is-active');

    const view = button.dataset.view;
    for (const section of document.querySelectorAll('.view'))
      section.classList.toggle('is-active', section.dataset.view === view);

    setView(view);
  });
}

// ── Kontrolki okna ──────────────────────────────────────────

function initWindowControls() {
  document.getElementById('btn-minimize').addEventListener('click', () => windowApi.minimize());
  document.getElementById('btn-hide').addEventListener('click', () => windowApi.hide());
  document.getElementById('btn-close').addEventListener('click', () => windowApi.close());

  const pin = document.getElementById('btn-pin');
  pin.addEventListener('click', async () => {
    const value = await windowApi.toggleAlwaysOnTop();
    pin.classList.toggle('is-active', value);
  });
}

// ── Profile w pasku tytułu ──────────────────────────────────

function initProfileSelect() {
  const select = document.getElementById('profile-select');
  select.addEventListener('change', () => {
    if (!select.value) return;
    command('profile.load', { name: select.value })
      .then(() => toast(`Profil: ${select.value}`, 'ok'))
      .catch((error) => toast(error.message, 'error'));
  });
}

function renderProfileSelect() {
  const select = document.getElementById('profile-select');
  const snapshot = state.snapshot;
  const active = snapshot ? snapshot.profile : '';

  select.innerHTML = '';
  const none = document.createElement('option');
  none.value = '';
  none.textContent = active ? active : '— brak —';
  select.appendChild(none);

  for (const profile of (snapshot && snapshot.profiles) || []) {
    const option = document.createElement('option');
    option.value = profile.name;
    option.textContent = profile.name;
    select.appendChild(option);
  }
  select.value = active || '';
}

// ── Pasek stanu ─────────────────────────────────────────────

function renderStatusBar() {
  const snapshot = state.snapshot;
  const connection = state.connection;

  const format = document.getElementById('pill-format');
  const latency = document.getElementById('pill-latency');
  const cpu = document.getElementById('pill-cpu');
  const link = document.getElementById('pill-connection');

  if (snapshot && snapshot.stats) {
    const stats = snapshot.stats;
    format.textContent = `${Math.round(stats.sampleRate / 100) / 10} kHz · ${stats.blockFrames}`;
    latency.textContent = `${(stats.totalLatencyMs || 0).toFixed(1)} ms`;
    latency.classList.toggle('is-warn', (stats.totalLatencyMs || 0) > 15);
  }

  if (state.meters) {
    const load = Math.round((state.meters.cpuLoad || 0) * 100);
    cpu.textContent = `CPU ${load}%`;
    cpu.classList.toggle('is-warn', load > 70);
  }

  link.classList.remove('is-ok', 'is-error');
  if (connection.authenticated) {
    link.textContent = 'połączono';
    link.classList.add('is-ok');
  } else if (connection.connected) {
    link.textContent = 'uwierzytelnianie…';
  } else {
    link.textContent = connection.error ? 'brak silnika' : 'łączenie…';
    link.classList.add('is-error');
  }
}

// ── Render całości ──────────────────────────────────────────

function renderAll(reason) {
  renderStatusBar();

  if (reason === 'connection') return;

  if (reason === 'selection') {
    // Przy samej zmianie zaznaczenia wystarczy odświeżyć inspektor i podświetlenie.
    for (const strip of document.querySelectorAll('.strip'))
      strip.classList.toggle('is-selected', Number(strip.dataset.channel) === state.selectedChannel);
    renderInspector();
    return;
  }

  renderProfileSelect();
  renderMixer();
  renderRouting();
  renderDevices();
  renderProfilesView();
}

// ── Pętla rysowania mierników (30–60 FPS, spec §13) ─────────

function startMeterLoop() {
  const paint = () => {
    paintMeters();
    requestAnimationFrame(paint);
  };
  requestAnimationFrame(paint);
}

// ── Start ───────────────────────────────────────────────────

async function refresh() {
  try {
    const data = await fetchSnapshot();
    setSnapshot(data);
  } catch (error) {
    setConnection({ error: error.message });
  }
}

function boot() {
  initTabs();
  initWindowControls();
  initProfileSelect();
  initMixer();
  initDevices();
  initProfiles();

  subscribe(renderAll);

  onSnapshot((data) => setSnapshot(data));
  onMeters((data) => {
    setMeters(data);
    renderStatusBar();
  });
  onStatus((status) => {
    setConnection(status);
    if (status.authenticated) refresh();
  });
  onEvent(() => refresh());
  onLog((line) => {
    const text = String(line).trim();
    if (text.includes('[ERROR]') || text.includes('[WARN]')) toast(text, 'error', 6000);
  });

  startMeterLoop();
  refresh();

  // Stan struktury odświeżamy rzadko — pomiary i tak przychodzą zdarzeniami.
  setInterval(() => {
    if (state.connection.authenticated) refresh();
  }, 3000);
}

document.addEventListener('DOMContentLoaded', boot);
