'use strict';
// Audio for the phone side: the church app sends 16 kHz mono 16-bit PCM; the
// phone services take 8 kHz G.711 mu-law. One mu-law byte is one sample, so a
// byte offset in a recording is also its time (8000 bytes per second).

const BIAS = 0x84;
const CLIP = 32635;

function linearToUlaw(sample) {
  let s = sample;
  let sign = 0;
  if (s < 0) { s = -s; sign = 0x80; }
  if (s > CLIP) s = CLIP;
  s += BIAS;
  let exponent = 7;
  for (let mask = 0x4000; (s & mask) === 0 && exponent > 0; mask >>= 1) exponent--;
  const mantissa = (s >> (exponent + 3)) & 0x0f;
  return ~(sign | (exponent << 4) | mantissa) & 0xff;
}

const ULAW_TO_LINEAR = new Int16Array(256);
for (let i = 0; i < 256; i++) {
  const u = ~i & 0xff;
  const sign = u & 0x80;
  const exponent = (u >> 4) & 0x07;
  const mantissa = u & 0x0f;
  const magnitude = (((mantissa << 3) + BIAS) << exponent) - BIAS;
  ULAW_TO_LINEAR[i] = sign ? -magnitude : magnitude;
}
const ULAW_SILENCE = linearToUlaw(0);

// Low-pass FIR for 16 kHz -> 8 kHz: windowed sinc, cut-off 3.6 kHz.
const TAPS = (() => {
  const n = 31;
  const cutoff = 3600 / 16000;
  const taps = new Float64Array(n);
  let sum = 0;
  for (let i = 0; i < n; i++) {
    const m = i - (n - 1) / 2;
    const sinc = m === 0 ? 2 * cutoff : Math.sin(2 * Math.PI * cutoff * m) / (Math.PI * m);
    const window = 0.54 - 0.46 * Math.cos(2 * Math.PI * i / (n - 1));
    taps[i] = sinc * window;
    sum += taps[i];
  }
  for (let i = 0; i < n; i++) taps[i] /= sum;
  return taps;
})();

// Streams 16 kHz s16le PCM into 8 kHz mu-law, keeping filter state between
// calls; every two input samples give one output byte.
class PhoneEncoder {
  constructor() {
    this.history = new Float64Array(TAPS.length);
    this.position = 0;
    this.phase = 0;
  }

  encode(pcm) {
    const samples = Math.floor(pcm.length / 2);
    const out = Buffer.allocUnsafe(Math.ceil((samples + this.phase) / 2));
    let written = 0;
    for (let i = 0; i < samples; i++) {
      this.history[this.position] = pcm.readInt16LE(i * 2);
      this.position = (this.position + 1) % TAPS.length;
      this.phase ^= 1;
      if (this.phase === 0) {
        let acc = 0;
        for (let t = 0, p = this.position; t < TAPS.length; t++) {
          acc += TAPS[t] * this.history[p];
          p = p + 1 === TAPS.length ? 0 : p + 1;
        }
        out[written++] = linearToUlaw(Math.max(-32768, Math.min(32767, Math.round(acc))));
      }
    }
    return out.subarray(0, written);
  }
}

// Peak and RMS of 16-bit PCM, in dBFS.
function pcmLevels(pcm) {
  const samples = Math.floor(pcm.length / 2);
  if (!samples) return { peakDb: -120, rmsDb: -120 };
  let peak = 0;
  let sum = 0;
  for (let i = 0; i < samples; i++) {
    const v = pcm.readInt16LE(i * 2) / 32768;
    const a = Math.abs(v);
    if (a > peak) peak = a;
    sum += v * v;
  }
  const db = x => (x > 1e-6 ? 20 * Math.log10(x) : -120);
  return { peakDb: db(peak), rmsDb: db(Math.sqrt(sum / samples)) };
}

module.exports = { linearToUlaw, ULAW_TO_LINEAR, ULAW_SILENCE, PhoneEncoder, pcmLevels };
