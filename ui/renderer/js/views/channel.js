// Panel szczegółów kanału (spec §19): wejście, gain, efekty, wyjścia.

import { command } from '../api.js';
import { state, channelById } from '../state.js';
import { drawEqCurve } from '../components/eqcurve.js';
import { element, toast } from '../ui.js';

const openEffects = new Set();

function parameterControl(channel, effect, parameter) {
  const row = element('div', 'field');
  row.appendChild(element('label', null, parameter.name));

  if (parameter.scale === 'bool') {
    const toggle = element('button', `chip${parameter.value >= 0.5 ? ' is-on-accent' : ''}`,
                           parameter.value >= 0.5 ? 'WŁ.' : 'WYŁ.');
    toggle.addEventListener('click', () => {
      command('effect.setParam', {
        channel: channel.id, effect: effect.id, param: parameter.id,
        value: parameter.value >= 0.5 ? 0 : 1,
      }).catch((error) => toast(error.message, 'error'));
    });
    row.appendChild(toggle);
    return row;
  }

  if (parameter.scale === 'choice' && Array.isArray(parameter.choices)) {
    const select = document.createElement('select');
    parameter.choices.forEach((choice, index) => {
      const option = document.createElement('option');
      option.value = String(index);
      option.textContent = choice;
      select.appendChild(option);
    });
    select.value = String(Math.round(parameter.value));
    select.addEventListener('change', () => {
      command('effect.setParam', {
        channel: channel.id, effect: effect.id, param: parameter.id,
        value: Number(select.value),
      }).catch((error) => toast(error.message, 'error'));
    });
    row.appendChild(select);
    return row;
  }

  const slider = document.createElement('input');
  slider.type = 'range';
  slider.min = '0';
  slider.max = '1000';

  const logarithmic = parameter.scale === 'log' && parameter.min > 0;
  const toSlider = (value) => {
    if (logarithmic) {
      const ratio = Math.log(value / parameter.min) / Math.log(parameter.max / parameter.min);
      return Math.round(ratio * 1000);
    }
    return Math.round(((value - parameter.min) / (parameter.max - parameter.min)) * 1000);
  };
  const fromSlider = (position) => {
    const ratio = position / 1000;
    if (logarithmic) return parameter.min * Math.pow(parameter.max / parameter.min, ratio);
    return parameter.min + ratio * (parameter.max - parameter.min);
  };

  slider.value = String(toSlider(parameter.value));

  const readout = element('span', 'value', formatParameter(parameter, parameter.value));
  slider.addEventListener('input', () => {
    const value = fromSlider(Number(slider.value));
    readout.textContent = formatParameter(parameter, value);
    command('effect.setParam', {
      channel: channel.id, effect: effect.id, param: parameter.id, value,
    }).catch((error) => toast(error.message, 'error'));
  });

  row.appendChild(slider);
  row.appendChild(readout);
  return row;
}

function formatParameter(parameter, value) {
  const unit = parameter.unit ? ` ${parameter.unit}` : '';
  if (parameter.unit === 'Hz') {
    return value >= 1000 ? `${(value / 1000).toFixed(2)} kHz` : `${Math.round(value)} Hz`;
  }
  const digits = Math.abs(value) >= 100 ? 0 : Math.abs(value) >= 10 ? 1 : 2;
  return `${value.toFixed(digits)}${unit}`;
}

async function refreshEqCurve(canvas, channelId, effectId) {
  try {
    const response = await command('effect.eqResponse', {
      channel: channelId, effect: effectId, points: 160,
    });
    drawEqCurve(canvas, response.frequencies, response.gainDb);
  } catch {
    drawEqCurve(canvas, [], []);
  }
}

function buildEffect(channel, effect) {
  const box = element('div', 'effect');
  if (!effect.enabled) box.classList.add('is-bypassed');
  if (openEffects.has(effect.id)) box.classList.add('is-open');

  const head = element('div', 'effect-head');

  const toggle = element('div', `effect-toggle${effect.enabled ? ' is-on' : ''}`);
  toggle.title = effect.enabled ? 'Wyłącz efekt' : 'Włącz efekt';
  toggle.addEventListener('click', (event) => {
    event.stopPropagation();
    command('effect.setEnabled', { channel: channel.id, effect: effect.id, value: !effect.enabled })
      .catch((error) => toast(error.message, 'error'));
  });

  head.appendChild(toggle);
  head.appendChild(element('span', 'effect-title', effect.name));

  if (effect.latencyFrames > 0)
    head.appendChild(element('span', 'effect-latency', `${effect.latencyFrames} pr.`));

  head.addEventListener('click', () => {
    if (openEffects.has(effect.id)) openEffects.delete(effect.id);
    else openEffects.add(effect.id);
    box.classList.toggle('is-open');
  });

  box.appendChild(head);

  const body = element('div', 'effect-body');

  if (effect.type === 'equalizer') {
    const canvas = document.createElement('canvas');
    canvas.className = 'eq-canvas';
    canvas.width = 320;
    canvas.height = 112;
    body.appendChild(canvas);
    refreshEqCurve(canvas, channel.id, effect.id);
  }

  for (const parameter of effect.parameters || []) {
    // Pasma EQ są liczne — pokazujemy je dopiero po rozwinięciu efektu.
    body.appendChild(parameterControl(channel, effect, parameter));
  }

  box.appendChild(body);
  return box;
}

export function renderInspector() {
  const inspector = document.getElementById('inspector');
  const channel = channelById(state.selectedChannel);

  inspector.innerHTML = '';

  if (!channel) {
    inspector.appendChild(element('div', 'inspector-empty',
                                  'Wybierz kanał, aby zobaczyć jego ustawienia'));
    return;
  }

  const title = element('h3', null, channel.name);
  inspector.appendChild(title);

  // ── Wejście ──────────────────────────────────────────────
  const snapshot = state.snapshot;
  const sources = (snapshot.sources || []).filter((source) => source.channel === channel.id);

  const inputSection = element('div', 'section');
  inputSection.appendChild(element('h4', null, 'Wejście'));

  if (sources.length === 0) {
    inputSection.appendChild(element('div', 'hint', 'Brak przypisanego źródła.'));
  } else {
    for (const source of sources) {
      const row = element('div', 'field');
      row.appendChild(element('label', null, source.name));

      const select = document.createElement('select');
      const none = document.createElement('option');
      none.value = '';
      none.textContent = '— brak —';
      select.appendChild(none);

      const isLoopback = source.kind === 'loopback';
      const devices = isLoopback
        ? (snapshot.devices && snapshot.devices.render) || []
        : (snapshot.devices && snapshot.devices.capture) || [];

      for (const device of devices) {
        const option = document.createElement('option');
        option.value = device.id;
        option.textContent = device.name;
        select.appendChild(option);
      }

      if (source.kind === 'application') {
        select.disabled = true;
        const option = document.createElement('option');
        option.value = 'app';
        option.textContent = `${source.processName} (PID ${source.processId || '—'})`;
        select.appendChild(option);
        select.value = 'app';
      } else {
        select.value = source.deviceId || '';
        select.addEventListener('change', () => {
          command('source.bindDevice', {
            source: source.id, deviceId: select.value, loopback: isLoopback,
          }).catch((error) => toast(error.message, 'error'));
        });
      }

      row.appendChild(select);
      inputSection.appendChild(row);
    }
  }
  inspector.appendChild(inputSection);

  // ── Gain i pan ───────────────────────────────────────────
  const toneSection = element('div', 'section');
  toneSection.appendChild(element('h4', null, 'Tor'));

  const gainRow = element('div', 'field');
  gainRow.appendChild(element('label', null, 'Gain'));
  const gainSlider = document.createElement('input');
  gainSlider.type = 'range';
  gainSlider.min = '-40';
  gainSlider.max = '40';
  gainSlider.step = '0.5';
  gainSlider.value = String(channel.gainDb);
  const gainValue = element('span', 'value', `${channel.gainDb.toFixed(1)} dB`);
  gainSlider.addEventListener('input', () => {
    gainValue.textContent = `${Number(gainSlider.value).toFixed(1)} dB`;
    command('channel.setGain', { channel: channel.id, value: Number(gainSlider.value) })
      .catch((error) => toast(error.message, 'error'));
  });
  gainRow.appendChild(gainSlider);
  gainRow.appendChild(gainValue);
  toneSection.appendChild(gainRow);

  const panRow = element('div', 'field');
  panRow.appendChild(element('label', null, 'Pan'));
  const panSlider = document.createElement('input');
  panSlider.type = 'range';
  panSlider.min = '-100';
  panSlider.max = '100';
  panSlider.value = String(Math.round(channel.pan * 100));
  const panValue = element('span', 'value', panLabel(channel.pan));
  panSlider.addEventListener('input', () => {
    const value = Number(panSlider.value) / 100;
    panValue.textContent = panLabel(value);
    command('channel.setPan', { channel: channel.id, value })
      .catch((error) => toast(error.message, 'error'));
  });
  panRow.appendChild(panSlider);
  panRow.appendChild(panValue);
  toneSection.appendChild(panRow);

  inspector.appendChild(toneSection);

  // ── Efekty ───────────────────────────────────────────────
  const effectsSection = element('div', 'section');
  effectsSection.appendChild(element('h4', null, 'Efekty'));
  for (const effect of channel.effects || [])
    effectsSection.appendChild(buildEffect(channel, effect));

  const addRow = element('div', 'form-actions');
  const addSelect = document.createElement('select');
  for (const plugin of snapshot.plugins || []) {
    const option = document.createElement('option');
    option.value = plugin.type;
    option.textContent = plugin.name;
    addSelect.appendChild(option);
  }
  const addButton = element('button', 'btn btn-small', 'Dodaj efekt');
  addButton.addEventListener('click', () => {
    command('effect.add', { channel: channel.id, type: addSelect.value })
      .catch((error) => toast(error.message, 'error'));
  });
  addRow.appendChild(addSelect);
  addRow.appendChild(addButton);
  effectsSection.appendChild(addRow);

  inspector.appendChild(effectsSection);

  // ── Wyjścia ──────────────────────────────────────────────
  const outputSection = element('div', 'section');
  outputSection.appendChild(element('h4', null, 'Wyjścia'));

  const toggles = element('div', 'output-toggles');
  const enabledBuses = new Set((channel.outputs || []).map((output) => output.bus));

  for (const bus of snapshot.buses || []) {
    const active = enabledBuses.has(bus.id);
    const toggle = element('button', `chip${active ? ' is-on-accent' : ''}`,
                           `${bus.label} · ${bus.name}`);
    toggle.addEventListener('click', () => {
      command('routing.set', { channel: channel.id, bus: bus.id, value: !active })
        .catch((error) => toast(error.message, 'error'));
    });
    toggles.appendChild(toggle);
  }

  outputSection.appendChild(toggles);
  inspector.appendChild(outputSection);
}

function panLabel(pan) {
  if (Math.abs(pan) < 0.01) return 'środek';
  return pan < 0 ? `L ${Math.round(-pan * 100)}` : `P ${Math.round(pan * 100)}`;
}
