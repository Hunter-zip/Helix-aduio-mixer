// Cienka warstwa nad mostem IPC: rzuca wyjątkiem przy błędzie silnika,
// dzięki czemu widoki mogą pisać zwykłe `await`.

const bridge = window.helix;

export async function command(name, args = {}) {
  const response = await bridge.engine.command(name, args);
  if (!response.ok) throw new Error(response.error || 'Błąd silnika');
  return response.result;
}

export async function snapshot() {
  const response = await bridge.engine.snapshot();
  if (!response.ok) throw new Error(response.error || 'Nie udało się pobrać stanu');
  return response.result;
}

export const restartEngine = () => bridge.engine.restart();

export const onSnapshot = (callback) => bridge.engine.onSnapshot(callback);
export const onMeters   = (callback) => bridge.engine.onMeters(callback);
export const onStatus   = (callback) => bridge.engine.onStatus(callback);
export const onEvent    = (callback) => bridge.engine.onEvent(callback);
export const onLog      = (callback) => bridge.engine.onLog(callback);

export const windowApi = bridge.window;
export const appApi = bridge.app;
