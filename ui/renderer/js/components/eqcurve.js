// Wykres charakterystyki equalizera (spec §9).
// Punkty liczy silnik — tutaj tylko je rysujemy.

const MIN_DB = -18;
const MAX_DB = 18;

export function drawEqCurve(canvas, frequencies, gains) {
  const context = canvas.getContext('2d');
  const width = canvas.width;
  const height = canvas.height;

  context.clearRect(0, 0, width, height);
  context.fillStyle = '#0a0a0c';
  context.fillRect(0, 0, width, height);

  const toY = (db) => height - ((db - MIN_DB) / (MAX_DB - MIN_DB)) * height;
  const toX = (frequency) => {
    const min = Math.log10(20);
    const max = Math.log10(20000);
    return ((Math.log10(Math.max(frequency, 20)) - min) / (max - min)) * width;
  };

  // Siatka: dekady częstotliwości i poziomy co 6 dB.
  context.strokeStyle = 'rgba(255,255,255,0.06)';
  context.lineWidth = 1;
  for (const frequency of [100, 1000, 10000]) {
    const x = Math.round(toX(frequency)) + 0.5;
    context.beginPath();
    context.moveTo(x, 0);
    context.lineTo(x, height);
    context.stroke();
  }
  for (const db of [-12, -6, 6, 12]) {
    const y = Math.round(toY(db)) + 0.5;
    context.beginPath();
    context.moveTo(0, y);
    context.lineTo(width, y);
    context.stroke();
  }

  // Linia 0 dB.
  context.strokeStyle = 'rgba(255,255,255,0.18)';
  const zeroY = Math.round(toY(0)) + 0.5;
  context.beginPath();
  context.moveTo(0, zeroY);
  context.lineTo(width, zeroY);
  context.stroke();

  if (!Array.isArray(frequencies) || !Array.isArray(gains) || gains.length === 0) return;

  context.beginPath();
  for (let i = 0; i < gains.length; i += 1) {
    const x = toX(frequencies[i]);
    const y = toY(Math.max(MIN_DB, Math.min(MAX_DB, gains[i])));
    if (i === 0) context.moveTo(x, y);
    else context.lineTo(x, y);
  }

  context.strokeStyle = '#5ac8fa';
  context.lineWidth = 2;
  context.stroke();

  // Delikatne wypełnienie pod krzywą.
  context.lineTo(width, zeroY);
  context.lineTo(0, zeroY);
  context.closePath();
  context.fillStyle = 'rgba(90, 200, 250, 0.12)';
  context.fill();
}
