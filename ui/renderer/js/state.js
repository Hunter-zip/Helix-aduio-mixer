// Wspólny stan renderera. Widoki subskrybują zmiany zamiast odpytywać silnik.

const listeners = new Set();

export const state = {
  snapshot: null,
  meters: null,
  selectedChannel: null,
  connection: { connected: false, authenticated: false, error: '' },
  view: 'mixer',
};

export function subscribe(callback) {
  listeners.add(callback);
  return () => listeners.delete(callback);
}

export function notify(reason) {
  for (const callback of listeners) callback(reason);
}

export function setSnapshot(snapshot) {
  state.snapshot = snapshot;

  // Zaznaczony kanał mógł zniknąć (np. po wczytaniu innego profilu).
  if (state.selectedChannel !== null && snapshot && Array.isArray(snapshot.channels)) {
    const stillThere = snapshot.channels.some((channel) => channel.id === state.selectedChannel);
    if (!stillThere) state.selectedChannel = null;
  }
  if (state.selectedChannel === null && snapshot && snapshot.channels && snapshot.channels.length > 0)
    state.selectedChannel = snapshot.channels[0].id;

  notify('snapshot');
}

export function setMeters(meters) {
  state.meters = meters;
  // Pomiary odświeżają się 30×/s — nie wywołujemy pełnego renderu,
  // rysowaniem zajmuje się osobna pętla animacji.
}

export function setConnection(status) {
  state.connection = { ...state.connection, ...status };
  notify('connection');
}

export function selectChannel(channelId) {
  state.selectedChannel = channelId;
  notify('selection');
}

export function setView(view) {
  state.view = view;
  notify('view');
}

/** Kanał po identyfikatorze z bieżącej migawki. */
export function channelById(channelId) {
  if (!state.snapshot || !Array.isArray(state.snapshot.channels)) return null;
  return state.snapshot.channels.find((channel) => channel.id === channelId) || null;
}

/** Pomiary konkretnego kanału z ostatniej ramki. */
export function channelMeter(channelId) {
  if (!state.meters || !Array.isArray(state.meters.channels)) return null;
  const entry = state.meters.channels.find((item) => item.id === channelId);
  return entry ? entry.output : null;
}

export function busMeter(busId) {
  if (!state.meters || !Array.isArray(state.meters.buses)) return null;
  const entry = state.meters.buses.find((item) => item.id === busId);
  return entry ? entry.meter : null;
}
