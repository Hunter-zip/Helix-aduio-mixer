// Rysowanie mierników. Wartości pochodzą z silnika (spec §13) — renderer
// jedynie je maluje, nie liczy niczego z sygnału.

const MIN_DB = -60;

/** Pozycja 0..1 na skali dB dla wartości liniowej. */
function toScale(linear) {
  if (!linear || linear <= 0) return 0;
  const db = 20 * Math.log10(linear);
  if (db <= MIN_DB) return 0;
  if (db >= 0) return 1;
  return (db - MIN_DB) / -MIN_DB;
}

function gradientFor(context, x, y, width, height, vertical) {
  const gradient = vertical
    ? context.createLinearGradient(0, y + height, 0, y)
    : context.createLinearGradient(x, 0, x + width, 0);
  gradient.addColorStop(0.0, '#2f9e5a');
  gradient.addColorStop(0.7, '#5ac8fa');
  gradient.addColorStop(0.88, '#f5a623');
  gradient.addColorStop(1.0, '#ff5a5a');
  return gradient;
}

/** Pionowy miernik stereo z peak-holdem i wskaźnikiem clippingu. */
export function drawVerticalMeter(canvas, meter) {
  const context = canvas.getContext('2d');
  const { width, height } = canvas;

  context.clearRect(0, 0, width, height);
  context.fillStyle = '#0a0a0c';
  context.fillRect(0, 0, width, height);

  // Podziałka co 6 dB.
  context.strokeStyle = 'rgba(255,255,255,0.05)';
  context.lineWidth = 1;
  for (let db = -6; db >= MIN_DB; db -= 6) {
    const y = Math.round(height - toScale(Math.pow(10, db / 20)) * height) + 0.5;
    context.beginPath();
    context.moveTo(0, y);
    context.lineTo(width, y);
    context.stroke();
  }

  if (!meter) return;

  const gap = 2;
  const barWidth = Math.floor((width - gap) / 2);
  const channels = [
    { rms: meter.rmsL, peak: meter.peakL, x: 0 },
    { rms: meter.rmsR, peak: meter.peakR, x: barWidth + gap },
  ];

  for (const channel of channels) {
    const rmsHeight = Math.round(toScale(channel.rms) * height);
    if (rmsHeight > 0) {
      context.fillStyle = gradientFor(context, channel.x, 0, barWidth, height, true);
      context.fillRect(channel.x, height - rmsHeight, barWidth, rmsHeight);
    }

    const peakY = Math.round(height - toScale(channel.peak) * height);
    if (channel.peak > 0) {
      context.fillStyle = channel.peak >= 0.999 ? '#ff5a5a' : '#e9e9ee';
      context.fillRect(channel.x, Math.max(0, peakY - 1), barWidth, 2);
    }
  }

  if (meter.clip) {
    context.fillStyle = '#ff5a5a';
    context.fillRect(0, 0, width, 3);
  }
}

/** Poziomy miernik stereo (sekcja Master). */
export function drawHorizontalMeter(canvas, meter) {
  const context = canvas.getContext('2d');
  const { width, height } = canvas;

  context.clearRect(0, 0, width, height);
  context.fillStyle = '#0a0a0c';
  context.fillRect(0, 0, width, height);

  if (!meter) return;

  const gap = 2;
  const barHeight = Math.floor((height - gap) / 2);
  const channels = [
    { rms: meter.rmsL, peak: meter.peakL, y: 0 },
    { rms: meter.rmsR, peak: meter.peakR, y: barHeight + gap },
  ];

  for (const channel of channels) {
    const rmsWidth = Math.round(toScale(channel.rms) * width);
    if (rmsWidth > 0) {
      context.fillStyle = gradientFor(context, 0, channel.y, width, barHeight, false);
      context.fillRect(0, channel.y, rmsWidth, barHeight);
    }

    const peakX = Math.round(toScale(channel.peak) * width);
    if (channel.peak > 0) {
      context.fillStyle = channel.peak >= 0.999 ? '#ff5a5a' : '#e9e9ee';
      context.fillRect(Math.max(0, peakX - 2), channel.y, 2, barHeight);
    }
  }

  if (meter.clip) {
    context.fillStyle = '#ff5a5a';
    context.fillRect(width - 3, 0, 3, height);
  }
}
