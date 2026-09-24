// Drobne pomocniki interfejsu.

export function toast(message, kind = 'info', timeoutMs = 4000) {
  const stack = document.getElementById('toasts');
  if (!stack) return;

  const element = document.createElement('div');
  element.className = `toast${kind === 'error' ? ' is-error' : kind === 'ok' ? ' is-ok' : ''}`;
  element.textContent = message;
  stack.appendChild(element);

  setTimeout(() => element.remove(), timeoutMs);
}

export function formatDb(linear) {
  if (!linear || linear <= 0) return '−∞';
  const db = 20 * Math.log10(linear);
  return `${db >= 0 ? '+' : ''}${db.toFixed(1)} dB`;
}

/** Tworzy element z klasą i opcjonalną treścią. */
export function element(tag, className, text) {
  const node = document.createElement(tag);
  if (className) node.className = className;
  if (text !== undefined) node.textContent = text;
  return node;
}
