// Panel urządzeń (spec §20) oraz wirtualnych punktów końcowych (spec §7).

import { command, appApi } from '../api.js';
import { state } from '../state.js';
import { element, toast } from '../ui.js';

function deviceSelect(devices, selected, onChange, includeNone = true) {
  const select = document.createElement('select');
  if (includeNone) {
    const none = document.createElement('option');
    none.value = '';
    none.textContent = '— brak —';
    select.appendChild(none);
  }
  for (const device of devices) {
    const option = document.createElement('option');
    option.value = device.id;
    option.textContent = device.default ? `${device.name} (domyślne)` : device.name;
    select.appendChild(option);
  }
  select.value = selected || '';
  select.addEventListener('change', () => onChange(select.value));
  return select;
}

function renderSettings(snapshot) {
  const settings = (snapshot.devices && snapshot.devices.settings) || {};
  document.getElementById('setting-samplerate').value = String(Math.round(settings.sampleRate || 48000));
  document.getElementById('setting-block').value = String(settings.blockFrames || 192);
  document.getElementById('setting-channels').value = String(settings.channels || 2);
  document.getElementById('setting-exclusive').checked = Boolean(settings.exclusive);

  const stats = snapshot.stats || {};
  const latency = document.getElementById('settings-latency');
  latency.textContent =
    `Latencja: ${(stats.totalLatencyMs || 0).toFixed(1)} ms ` +
    `(urządzenie ${(stats.deviceLatencyMs || 0).toFixed(1)} ms) · ` +
    `xruny: ${stats.xruns || 0} · backend: ${(snapshot.devices && snapshot.devices.backend) || '—'}`;
}

function renderBuses(snapshot) {
  const container = document.getElementById('bus-devices');
  container.innerHTML = '';

  const renderDevices = (snapshot.devices && snapshot.devices.render) || [];

  for (const bus of snapshot.buses || []) {
    const row = element('div', 'device-row');

    const dot = element('span', `status-dot${bus.deviceReady ? ' is-ok' : bus.deviceId ? ' is-error' : ''}`);
    row.appendChild(dot);
    row.appendChild(element('span', 'name', `${bus.label} · ${bus.name}`));

    row.appendChild(deviceSelect(renderDevices, bus.deviceId, (deviceId) => {
      command('bus.setDevice', { bus: bus.id, deviceId })
        .catch((error) => toast(error.message, 'error'));
    }));

    const test = element('button', 'btn btn-small', 'Test');
    test.title = 'Odtwórz krótki sygnał testowy';
    test.addEventListener('click', () => {
      if (!bus.deviceId) { toast('Najpierw wybierz urządzenie', 'error'); return; }
      command('device.test', { deviceId: bus.deviceId })
        .then(() => toast(`Sygnał testowy: ${bus.deviceName || bus.deviceId}`, 'ok'))
        .catch((error) => toast(error.message, 'error'));
    });
    row.appendChild(test);

    const primary = element('button', `chip${bus.primary ? ' is-on-accent' : ''}`, 'ZEGAR');
    primary.title = 'Magistrala napędzająca zegar silnika';
    primary.addEventListener('click', () => {
      command('bus.setPrimary', { bus: bus.id })
        .then(() => toast(`${bus.label} steruje zegarem`, 'ok'))
        .catch((error) => toast(error.message, 'error'));
    });
    row.appendChild(primary);

    if (bus.latencyMs) row.appendChild(element('span', 'meta', `${bus.latencyMs.toFixed(1)} ms`));
    if (bus.error) row.appendChild(element('span', 'meta', bus.error));

    container.appendChild(row);
  }
}

function renderSources(snapshot) {
  const container = document.getElementById('source-devices');
  container.innerHTML = '';

  const captureDevices = (snapshot.devices && snapshot.devices.capture) || [];
  const renderDevices = (snapshot.devices && snapshot.devices.render) || [];

  for (const source of snapshot.sources || []) {
    const row = element('div', 'device-row');
    row.appendChild(element('span', `status-dot${source.streaming ? ' is-ok' : source.active ? ' is-error' : ''}`));
    row.appendChild(element('span', 'name', source.name));

    if (source.kind === 'application') {
      row.appendChild(element('span', 'meta',
        `aplikacja: ${source.processName || '—'}${source.processId ? ` (PID ${source.processId})` : ''}`));
    } else {
      const isLoopback = source.kind === 'loopback';
      row.appendChild(deviceSelect(isLoopback ? renderDevices : captureDevices, source.deviceId,
        (deviceId) => {
          command('source.bindDevice', { source: source.id, deviceId, loopback: isLoopback })
            .catch((error) => toast(error.message, 'error'));
        }));
    }

    const channelSelect = document.createElement('select');
    const none = document.createElement('option');
    none.value = '0';
    none.textContent = '— brak kanału —';
    channelSelect.appendChild(none);
    for (const channel of snapshot.channels || []) {
      const option = document.createElement('option');
      option.value = String(channel.id);
      option.textContent = channel.name;
      channelSelect.appendChild(option);
    }
    channelSelect.value = String(source.channel || 0);
    channelSelect.addEventListener('change', () => {
      command('source.assign', { source: source.id, channel: Number(channelSelect.value) })
        .catch((error) => toast(error.message, 'error'));
    });
    row.appendChild(channelSelect);

    if (source.underruns) row.appendChild(element('span', 'meta', `under: ${source.underruns}`));

    container.appendChild(row);
  }
}

function renderVirtual(snapshot) {
  const container = document.getElementById('virtual-devices');
  container.innerHTML = '';

  for (const endpoint of snapshot.virtualDevices || []) {
    const row = element('div', 'device-row');
    row.appendChild(element('span',
      `status-dot${endpoint.clientConnected ? ' is-ok' : endpoint.transportReady ? '' : ' is-error'}`));
    row.appendChild(element('span', 'name', endpoint.name));
    row.appendChild(element('span', 'meta', endpoint.kind === 'output' ? 'wyjście' : 'wejście'));

    if (endpoint.kind === 'output') {
      const select = document.createElement('select');
      const none = document.createElement('option');
      none.value = '0';
      none.textContent = '— brak —';
      select.appendChild(none);
      for (const bus of snapshot.buses || []) {
        const option = document.createElement('option');
        option.value = String(bus.id);
        option.textContent = `${bus.label} · ${bus.name}`;
        select.appendChild(option);
      }
      select.value = String(endpoint.bus || 0);
      select.addEventListener('change', () => {
        command('virtual.bindOutput', { id: endpoint.id, bus: Number(select.value) })
          .catch((error) => toast(error.message, 'error'));
      });
      row.appendChild(select);
    } else {
      const select = document.createElement('select');
      const none = document.createElement('option');
      none.value = '0';
      none.textContent = '— brak —';
      select.appendChild(none);
      for (const source of snapshot.sources || []) {
        const option = document.createElement('option');
        option.value = String(source.id);
        option.textContent = source.name;
        select.appendChild(option);
      }
      select.value = String(endpoint.source || 0);
      select.addEventListener('change', () => {
        command('virtual.bindInput', { id: endpoint.id, source: Number(select.value) })
          .catch((error) => toast(error.message, 'error'));
      });
      row.appendChild(select);
    }

    const toggle = element('button', `chip${endpoint.enabled ? ' is-on-accent' : ''}`,
                           endpoint.enabled ? 'WŁ.' : 'WYŁ.');
    toggle.addEventListener('click', () => {
      command('virtual.setEnabled', { id: endpoint.id, enabled: !endpoint.enabled })
        .catch((error) => toast(error.message, 'error'));
    });
    row.appendChild(toggle);

    container.appendChild(row);
  }

  const driver = snapshot.driver || {};
  const box = document.getElementById('driver-box');
  box.innerHTML = '';

  const labels = {
    'installed': 'zainstalowany',
    'not-installed': 'niezainstalowany',
    'needs-update': 'wymaga aktualizacji',
    'error': 'błąd',
  };

  box.appendChild(element('div', null,
    `Sterownik: ${labels[driver.state] || driver.state || '—'}` +
    `${driver.installedVersion ? ` (wersja ${driver.installedVersion})` : ''}`));
  if (driver.message) box.appendChild(element('div', null, driver.message));

  if (driver.state !== 'installed') {
    const actions = element('div', 'form-actions');
    const install = element('button', 'btn btn-small', 'Zainstaluj pakiet…');
    install.addEventListener('click', async () => {
      const path = await appApi.pickDriverPackage();
      if (!path) return;
      command('virtual.install', { package: path })
        .then(() => toast('Sterownik zainstalowany', 'ok'))
        .catch((error) => toast(error.message, 'error'));
    });
    actions.appendChild(install);
    box.appendChild(actions);
  } else {
    const actions = element('div', 'form-actions');
    const remove = element('button', 'btn btn-small btn-danger', 'Usuń sterownik');
    remove.addEventListener('click', () => {
      command('virtual.uninstall', {})
        .then(() => toast('Sterownik usunięty', 'ok'))
        .catch((error) => toast(error.message, 'error'));
    });
    actions.appendChild(remove);
    box.appendChild(actions);
  }
}

export function renderDevices() {
  const snapshot = state.snapshot;
  if (!snapshot) return;

  renderSettings(snapshot);
  renderBuses(snapshot);
  renderSources(snapshot);
  renderVirtual(snapshot);
}

export function initDevices() {
  document.getElementById('apply-settings').addEventListener('click', () => {
    command('device.apply', {
      sampleRate: Number(document.getElementById('setting-samplerate').value),
      blockFrames: Number(document.getElementById('setting-block').value),
      channels: Number(document.getElementById('setting-channels').value),
      exclusive: document.getElementById('setting-exclusive').checked,
    })
      .then(() => toast('Format zastosowany', 'ok'))
      .catch((error) => toast(error.message, 'error'));
  });
}
