// Profile, przypisania aplikacji, skróty i automatyzacja (spec §15–§17).

import { command } from '../api.js';
import { state } from '../state.js';
import { element, toast } from '../ui.js';

function renderProfiles(snapshot) {
  const container = document.getElementById('profile-list');
  container.innerHTML = '';

  const profiles = snapshot.profiles || [];
  if (profiles.length === 0) {
    container.appendChild(element('div', 'hint', 'Brak zapisanych profili.'));
    return;
  }

  for (const profile of profiles) {
    const row = element('div', `profile-row${profile.active ? ' is-active' : ''}`);
    row.appendChild(element('span', 'name', profile.name));
    row.appendChild(element('span', 'desc', profile.description || ''));

    const load = element('button', 'btn btn-small', 'Wczytaj');
    load.addEventListener('click', () => {
      command('profile.load', { name: profile.name })
        .then(() => toast(`Profil: ${profile.name}`, 'ok'))
        .catch((error) => toast(error.message, 'error'));
    });

    const overwrite = element('button', 'btn btn-small', 'Nadpisz');
    overwrite.addEventListener('click', () => {
      command('profile.save', { name: profile.name, description: profile.description })
        .then(() => toast(`Zapisano: ${profile.name}`, 'ok'))
        .catch((error) => toast(error.message, 'error'));
    });

    const remove = element('button', 'btn btn-small btn-danger', 'Usuń');
    remove.addEventListener('click', () => {
      command('profile.delete', { name: profile.name })
        .then(() => toast(`Usunięto: ${profile.name}`, 'ok'))
        .catch((error) => toast(error.message, 'error'));
    });

    row.appendChild(load);
    row.appendChild(overwrite);
    row.appendChild(remove);
    container.appendChild(row);
  }
}

function renderApplications(snapshot) {
  const container = document.getElementById('application-list');
  container.innerHTML = '';

  const applications = snapshot.applications || [];
  const assignments = snapshot.assignments || {};

  // Aplikacje wykryte teraz plus te, które mają zapamiętane przypisanie.
  const seen = new Set();
  const rows = [];

  for (const application of applications) {
    seen.add(application.executable.toLowerCase());
    rows.push({
      executable: application.executable,
      channel: application.channel || '',
      running: true,
      active: application.active,
    });
  }
  for (const [executable, channel] of Object.entries(assignments)) {
    if (seen.has(executable.toLowerCase())) continue;
    rows.push({ executable, channel, running: false, active: false });
  }

  if (rows.length === 0) {
    container.appendChild(element('div', 'hint', 'Nie wykryto aplikacji odtwarzających dźwięk.'));
    return;
  }

  for (const row of rows) {
    const line = element('div', 'device-row');
    line.appendChild(element('span',
      `status-dot${row.active ? ' is-ok' : row.running ? '' : ''}`));
    line.appendChild(element('span', 'name', row.executable));

    const select = document.createElement('select');
    const none = document.createElement('option');
    none.value = '';
    none.textContent = '— bez przypisania —';
    select.appendChild(none);

    for (const channel of snapshot.channels || []) {
      const option = document.createElement('option');
      option.value = channel.name;
      option.textContent = channel.name;
      select.appendChild(option);
    }
    select.value = row.channel;

    select.addEventListener('change', () => {
      const request = select.value
        ? command('app.assign', { executable: row.executable, channel: select.value })
        : command('app.unassign', { executable: row.executable });
      request.catch((error) => toast(error.message, 'error'));
    });

    line.appendChild(select);
    if (!row.running) line.appendChild(element('span', 'meta', 'nieuruchomiona'));
    container.appendChild(line);
  }
}

function renderHotkeys(snapshot) {
  const container = document.getElementById('hotkey-list');
  container.innerHTML = '';

  if (!snapshot.hotkeysGlobal) {
    container.appendChild(element('div', 'hint',
      'Globalne przechwytywanie skrótów nie jest dostępne na tej platformie — ' +
      'skróty można wyzwalać z poziomu aplikacji.'));
  }

  for (const hotkey of snapshot.hotkeys || []) {
    const row = element('div', 'device-row');
    row.appendChild(element('span',
      `status-dot${hotkey.registered ? ' is-ok' : hotkey.enabled ? ' is-error' : ''}`));
    row.appendChild(element('span', 'name', hotkey.combo));
    row.appendChild(element('span', 'meta', `${hotkey.action}`));

    const trigger = element('button', 'btn btn-small', 'Wyzwól');
    trigger.addEventListener('click', () => {
      command('hotkey.trigger', { id: hotkey.id })
        .catch((error) => toast(error.message, 'error'));
    });
    row.appendChild(trigger);

    if (hotkey.error) row.appendChild(element('span', 'meta', hotkey.error));
    container.appendChild(row);
  }
}

function renderAutomation(snapshot) {
  const container = document.getElementById('automation-list');
  container.innerHTML = '';

  const rules = snapshot.automation || [];
  if (rules.length === 0) {
    container.appendChild(element('div', 'hint', 'Brak reguł.'));
    return;
  }

  for (const rule of rules) {
    const row = element('div', 'device-row');
    row.appendChild(element('span', `status-dot${rule.enabled ? ' is-ok' : ''}`));
    row.appendChild(element('span', 'name', rule.name || rule.id));
    row.appendChild(element('span', 'meta', rule.trigger));

    const toggle = element('button', `chip${rule.enabled ? ' is-on-accent' : ''}`,
                           rule.enabled ? 'WŁ.' : 'WYŁ.');
    toggle.addEventListener('click', () => {
      command('automation.setEnabled', { id: rule.id, value: !rule.enabled })
        .catch((error) => toast(error.message, 'error'));
    });
    row.appendChild(toggle);

    container.appendChild(row);
  }
}

export function renderProfilesView() {
  const snapshot = state.snapshot;
  if (!snapshot) return;

  renderProfiles(snapshot);
  renderApplications(snapshot);
  renderHotkeys(snapshot);
  renderAutomation(snapshot);
}

export function initProfiles() {
  document.getElementById('profile-save').addEventListener('click', () => {
    const input = document.getElementById('profile-name');
    const name = input.value.trim();
    if (!name) { toast('Podaj nazwę profilu', 'error'); return; }

    command('profile.save', { name })
      .then(() => {
        input.value = '';
        toast(`Zapisano profil: ${name}`, 'ok');
      })
      .catch((error) => toast(error.message, 'error'));
  });
}
