// Pretends to be Plivo making one call to a running phone server: asks the
// Answer URL, opens the audio stream, records what the caller would hear to a
// WAV file and prints its level. Works against the local test server or the
// real one, before any phone service is set up.
//
//   node test/call.mjs --answer https://phone.example.org/plivo/answer/<KEY> [--from +15551230001]
//                      [--seconds 10] [--press1 5] [--out call.wav] [--signed]
// --signed adds a (dummy) Plivo signature header, so the call is listed as a
// phone call instead of a test.

import fs from 'node:fs';
import { createRequire } from 'node:module';
const require = createRequire(import.meta.url);
const { ULAW_TO_LINEAR } = require('../audio.js');

const args = Object.fromEntries(process.argv.slice(2).reduce((pairs, a, i, all) => (a.startsWith('--') ? [...pairs, [a.slice(2), all[i + 1] !== undefined && !all[i + 1].startsWith('--') ? all[i + 1] : 'true']] : pairs), []));
if (!args.answer) { console.log('usage: node test/call.mjs --answer <Answer URL> [--from N] [--seconds S] [--press1 S] [--out file.wav]'); process.exit(2); }
const seconds = Number(args.seconds || 10);

const r = await fetch(args.answer, { method: 'POST', headers: { 'Content-Type': 'application/x-www-form-urlencoded', ...(args.signed ? { 'X-Plivo-Signature-V3': 'test' } : {}) },
  body: new URLSearchParams({ CallUUID: 'test-' + Date.now(), From: args.from || '+15551230001', To: '+15550000000', Direction: 'inbound' }) });
const xml = await r.text();
console.log(`answer: HTTP ${r.status}`);
const speak = [...xml.matchAll(/<Speak[^>]*>([^<]*)<\/Speak>/g)].map(m => m[1]);
if (speak.length) console.log(`spoken: "${speak[0]}"${speak[1] ? `, at the end: "${speak[1]}"` : ''}`);
const url = (/<Stream[^>]*>([^<]+)<\/Stream>/.exec(xml) || [])[1];
if (!url) { console.log('no audio stream in the answer (nothing to play)'); process.exit(r.ok ? 0 : 1); }

const ws = new WebSocket(url);
const chunks = [];
let closedCode = null;
ws.addEventListener('message', e => {
  const m = JSON.parse(e.data);
  if (m.event === 'playAudio') chunks.push(Buffer.from(m.media.payload, 'base64'));
  if (m.event === 'checkpoint') setTimeout(() => ws.readyState === 1 && ws.send(JSON.stringify({ event: 'playedStream', streamId: 'st', name: m.name })), 1000);
  if (m.event === 'clearAudio') ws.send(JSON.stringify({ event: 'clearedAudio', streamId: 'st' }));
});
const closed = new Promise(res => ws.addEventListener('close', e => { closedCode = e.code; res(); }));
await new Promise((res, rej) => { ws.addEventListener('open', res); ws.addEventListener('error', () => rej(new Error('stream did not open'))); });
ws.send(JSON.stringify({ event: 'start', sequenceNumber: 1, start: { callId: 'c', streamId: 'st', accountId: 'a', tracks: ['inbound'], mediaFormat: { encoding: 'audio/x-mulaw', sampleRate: 8000 } }, extra_headers: '' }));
if (args.press1) setTimeout(() => { ws.send(JSON.stringify({ event: 'dtmf', sequenceNumber: 2, streamId: 'st', dtmf: { track: 'inbound', digit: '1', timestamp: String(Date.now()) } })); console.log('pressed 1'); }, Number(args.press1) * 1000);
await Promise.race([closed, new Promise(res => setTimeout(res, seconds * 1000))]);
if (ws.readyState === 1) ws.close();

const ulaw = Buffer.concat(chunks);
const pcm = Buffer.alloc(ulaw.length * 2);
let peak = 0, sum = 0;
for (let i = 0; i < ulaw.length; i++) {
  const v = ULAW_TO_LINEAR[ulaw[i]];
  pcm.writeInt16LE(v, i * 2);
  peak = Math.max(peak, Math.abs(v));
  sum += v * v;
}
const db = x => (x > 0 ? (20 * Math.log10(x / 32768)).toFixed(1) : '-inf');
console.log(`received ${(ulaw.length / 8000).toFixed(1)} s of audio; peak ${db(peak)} dBFS, average ${db(Math.sqrt(sum / (ulaw.length || 1)))} dBFS` +
  (closedCode !== null ? `; the server ended the stream (${closedCode})` : ''));
if (args.out) {
  const h = Buffer.alloc(44);
  h.write('RIFF', 0); h.writeUInt32LE(36 + pcm.length, 4); h.write('WAVE', 8); h.write('fmt ', 12);
  h.writeUInt32LE(16, 16); h.writeUInt16LE(1, 20); h.writeUInt16LE(1, 22); h.writeUInt32LE(8000, 24);
  h.writeUInt32LE(16000, 28); h.writeUInt16LE(2, 32); h.writeUInt16LE(16, 34); h.write('data', 36); h.writeUInt32LE(pcm.length, 40);
  fs.writeFileSync(args.out, Buffer.concat([h, pcm]));
  console.log(`saved ${args.out}`);
}
