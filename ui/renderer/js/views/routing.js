// Macierz routingu kanał × magistrala (spec §6).

import { command } from '../api.js';
import { state } from '../state.js';
import { toast } from '../ui.js';

export function renderRouting() {
  const table = document.getElementById('routing-matrix');
  const snapshot = state.snapshot;

  table.innerHTML = '';
  if (!snapshot || !Array.isArray(snapshot.routing) || snapshot.routing.length === 0) {
    table.innerHTML = '<tr><td class="hint">Brak danych routingu</td></tr>';
    return;
  }

  const header = document.createElement('tr');
  const corner = document.createElement('th');
  corner.className = 'row-head';
  corner.textContent = 'Kanał → wyjście';
  header.appendChild(corner);

  const buses = snapshot.buses || [];
  for (const bus of buses) {
    const cell = document.createElement('th');
    cell.textContent = bus.label;
    cell.title = `${bus.name}${bus.deviceName ? ` — ${bus.deviceName}` : ''}`;
    header.appendChild(cell);
  }
  table.appendChild(header);

  for (const row of snapshot.routing) {
    const tr = document.createElement('tr');
    const name = document.createElement('td');
    name.className = 'row-head';
    name.textContent = row.name;
    tr.appendChild(name);

    for (const cell of row.buses || []) {
      const td = document.createElement('td');
      const toggle = document.createElement('button');
      toggle.className = `cell-toggle${cell.enabled ? ' is-on' : ''}`;
      toggle.textContent = cell.enabled ? 'ON' : '—';
      toggle.title = `${row.name} → ${cell.label}`;
      toggle.addEventListener('click', () => {
        command('routing.set', { channel: row.channel, bus: cell.bus, value: !cell.enabled })
          .catch((error) => toast(error.message, 'error'));
      });
      td.appendChild(toggle);
      tr.appendChild(td);
    }

    table.appendChild(tr);
  }
}
