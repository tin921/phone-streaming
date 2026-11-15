// End-to-end test of the phone server on this machine: it starts the server
// with a temporary data folder, plays the church app (pushing a stepped tone)
// and Plivo (calls, the audio stream, key presses), and checks what callers
// hear by decoding the mu-law and measuring the tone.
//
//   node test/run.mjs
//
// The tone steps every second: 400, 600, ... 2200 Hz, then repeats; so the
// pitch a caller hears tells where in the recording they are.

import { spawn } from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { createRequire } from 'node:module';

const require = createRequire(import.meta.url);
const { ULAW_TO_LINEAR } = require('../audio.js');
const here = path.dirname(fileURLToPath(import.meta.url));
const PORT = 18000 + Math.floor(Math.random() * 1000);
const BASE = `http://127.0.0.1:${PORT}`;
const WSBASE = `ws://127.0.0.1:${PORT}`;
const KEYS = { INGEST_TOKEN: 'test-ingest-token-0123456789', PLIVO_ANSWER_KEY: 'answerkey0123456789abcdef', CALLER_SECRET: 'caller-secret-0123456789', LISTEN_KEY: 'listenkey0123456789abc' };
const dataDir = fs.mkdtempSync(path.join(os.tmpdir(), 'phone-stream-test-'));

let failures = 0;
const results = [];
function check(ok, what, detail = '') {
  results.push(`${ok ? 'ok  ' : 'FAIL'}  ${what}${detail ? '  (' + detail + ')' : ''}`);
  console.log(results[results.length - 1]);
  if (!ok) failures++;
}
const sleep = ms => new Promise(r => setTimeout(r, ms));

// --- the tone ---------------------------------------------------------------
const RATE = 16000;
const freqAt = second => 400 + 200 * (Math.floor(second) % 10);
let phase = 0;
function tone(startSample, count) {
  const pcm = Buffer.alloc(count * 2);
  for (let i = 0; i < count; i++) {
    const f = freqAt((startSample + i) / RATE);
    phase += 2 * Math.PI * f / RATE;
    pcm.writeInt16LE(Math.round(Math.sin(phase) * 0.3 * 32767), i * 2);
  }
  return pcm;
}
function packet(startSample, pcm) {
  const head = Buffer.alloc(8);
  head.writeBigUInt64LE(BigInt(startSample));
  return Buffer.concat([head, pcm]);
}
// Strongest of the tone's ten pitches in 8 kHz mu-law, by Goertzel.
function pitch(ulaw) {
  let best = 0, bestPower = -1, total = 0;
  for (let k = 0; k < 10; k++) {
    const f = 400 + 200 * k;
    const w = 2 * Math.cos(2 * Math.PI * f / 8000);
    let s1 = 0, s2 = 0;
    for (const u of ulaw) { const s0 = ULAW_TO_LINEAR[u] + w * s1 - s2; s2 = s1; s1 = s0; }
    const power = s1 * s1 + s2 * s2 - w * s1 * s2;
    total += power;
    if (power > bestPower) { bestPower = power; best = f; }
  }
  return { freq: best, share: bestPower / (total || 1) };
}

// --- the server -------------------------------------------------------------
const server = spawn(process.execPath, [path.join(here, '..', 'main.js')], {
  env: { ...process.env, ...KEYS, PORT: String(PORT), DATA_DIR: dataDir, PUBLIC_WS_BASE: WSBASE },
  stdio: ['ignore', 'pipe', 'pipe']
});
const serverLog = [];
server.stdout.on('data', d => serverLog.push(String(d)));
server.stderr.on('data', d => serverLog.push(String(d)));

async function waitUp() {
  for (let i = 0; i < 50; i++) {
    try { const r = await fetch(`${BASE}/health`); if (r.ok) return; } catch { /* not yet */ }
    await sleep(100);
  }
  throw new Error('server did not start:\n' + serverLog.join(''));
}

// --- the church app -----------------------------------------------------------
class App {
  constructor(token = KEYS.INGEST_TOKEN) {
    this.ws = new WebSocket(`${WSBASE}/ingest`);
    this.messages = [];
    this.waiters = [];
    this.closed = null;
    this.ws.addEventListener('message', e => {
      const m = JSON.parse(e.data);
      this.messages.push(m);
      this.waiters = this.waiters.filter(w => (w.type === m.type ? (w.resolve(m), false) : true));
    });
    this.ws.addEventListener('close', e => { this.closed = e.code; });
    this.opened = new Promise(r => this.ws.addEventListener('open', r));
    this.token = token;
  }
  async hello() {
    await this.opened;
    this.ws.send(JSON.stringify({ type: 'hello', token: this.token, client: 'test', format: 's16le', rate: 16000, channels: 1 }));
  }
  next(type, timeout = 3000) {
    return new Promise((resolve, reject) => {
      this.waiters.push({ type, resolve });
      setTimeout(() => reject(new Error(`no ${type}`)), timeout);
    });
  }
  send(m) { this.ws.send(JSON.stringify(m)); }
  push(start, pcm) { this.ws.send(packet(start, pcm)); }
}

// --- Plivo ------------------------------------------------------------------
async function answer(from, key = KEYS.PLIVO_ANSWER_KEY, headers = {}) {
  const r = await fetch(`${BASE}/plivo/answer/${key}`, {
    method: 'POST', headers: { 'Content-Type': 'application/x-www-form-urlencoded', ...headers },
    body: new URLSearchParams({ CallUUID: 'call-' + Math.random(), From: from || '', To: '15550000000' })
  });
  return { status: r.status, xml: await r.text() };
}

class Call {
  constructor(url) {
    this.ws = new WebSocket(url);
    this.audio = [];          // { at: ms since start, bytes }
    this.events = [];
    this.closedCode = null;
    this.t0 = Date.now();
    this.ws.addEventListener('message', e => {
      const m = JSON.parse(e.data);
      this.events.push({ at: Date.now() - this.t0, event: m.event, name: m.name });
      if (m.event === 'playAudio') this.audio.push({ at: Date.now() - this.t0, bytes: Buffer.from(m.media.payload, 'base64'), format: m.media });
      // Plivo reports a checkpoint once the audio before it has played.
      if (m.event === 'checkpoint') setTimeout(() => this.send({ event: 'playedStream', streamId: 'st', name: m.name }), 600);
      if (m.event === 'clearAudio') this.send({ event: 'clearedAudio', streamId: 'st' });
    });
    this.closed = new Promise(r => this.ws.addEventListener('close', e => { this.closedCode = e.code; r(e.code); }));
    this.opened = new Promise(r => this.ws.addEventListener('open', r));
  }
  async start() { await this.opened; this.send({ event: 'start', sequenceNumber: 1, start: { callId: 'c', streamId: 'st', accountId: 'a', tracks: ['inbound'], mediaFormat: { encoding: 'audio/x-mulaw', sampleRate: 8000 } }, extra_headers: '' }); }
  send(m) { if (this.ws.readyState === 1) this.ws.send(JSON.stringify(m)); }
  press(digit) { this.send({ event: 'dtmf', sequenceNumber: 9, streamId: 'st', dtmf: { track: 'inbound', digit, timestamp: String(Date.now()) } }); }
  bytesSince(ms) { return Buffer.concat(this.audio.filter(a => a.at >= ms).map(a => a.bytes)); }
  allBytes() { return Buffer.concat(this.audio.map(a => a.bytes)); }
  hangUp() { this.ws.close(); }
}
const streamUrl = xml => (/<Stream[^>]*>([^<]+)<\/Stream>/.exec(xml) || [])[1];

// --- the run ------------------------------------------------------------------
try {
  await waitUp();
  const page = await (await fetch(`${BASE}/`)).text();
  check(page.includes('Phone stream server'), 'status page at /');

  let r = await answer('+15551230001');
  check(r.status === 200 && /There is no service/.test(r.xml) && /<Hangup\/>/.test(r.xml) && !/<Stream/.test(r.xml), 'a call with no service: told so, then hung up');
  r = await answer('+15551230001', 'wrongkey0123456789abcdef');
  check(r.status === 404, 'wrong answer key: 404');

  const intruder = new App('wrong-token-0123456789');
  await intruder.hello();
  await sleep(300);
  check(intruder.closed === 4001, 'app with a wrong token refused', `close ${intruder.closed}`);

  const app = new App();
  await app.hello();
  const welcomeCalls = app.next('calls');
  const welcome = await app.next('welcome');
  check(welcome.service === null, 'app welcomed, no service yet');
  check(welcome.links && welcome.links.listen === `https://phone.example.org/listen/${KEYS.LISTEN_KEY}` && welcome.links.status === 'https://phone.example.org/',
    'the welcome gives the app the status page and test player links', JSON.stringify(welcome.links));
  const early = (await welcomeCalls).calls;
  check(early.length === 1 && early[0].end === 'no service' && early[0].from === '+15551230001' && early[0].via === 'test' && early[0].ip === '127.0.0.1',
    'the app gets the call list: the call with no service, its number, test, IP', JSON.stringify(early[0]));
  const resetCalls = app.next('calls');
  app.send({ type: 'start' });
  const started = (await app.next('service')).service;
  check(!!started && started.seconds === 0, 'app started a service', started && started.id);
  check((await resetCalls).calls.length === 0, 'a new service starts an empty call list');
  const callsSeen = () => app.messages.filter(m => m.type === 'call').map(m => m.call);

  // 8 seconds at once, then real time.
  let pushed = 0;
  const pushSeconds = s => { const n = Math.round(s * RATE); app.push(pushed, tone(pushed, n)); pushed += n; };
  for (let i = 0; i < 80; i++) pushSeconds(0.1);
  const pushTimer = setInterval(() => pushSeconds(0.1), 100);
  await sleep(300);

  // A new caller starts at the beginning.
  r = await answer('+15551230001');
  check(/play from the beginning/.test(r.xml) && /bidirectional="true"/.test(r.xml) && /audio\/x-mulaw;rate=8000/.test(r.xml), 'new caller: greeting and a bidirectional mu-law stream');
  const a = new Call(streamUrl(r.xml));
  await a.start();
  await sleep(2500);
  const first = pitch(a.allBytes().subarray(0, 4000));
  check(first.freq === 400 && first.share > 0.9, 'new caller hears the beginning (400 Hz)', `${first.freq} Hz, ${(first.share * 100).toFixed(0)}%`);
  const sent25 = a.allBytes().length / 8000;
  check(sent25 >= 2.8 && sent25 <= 4.2, 'audio is paced to real time with about 1 s lead', `${sent25.toFixed(2)} s sent in 2.5 s`);
  check(a.audio.every(x => x.format.contentType === 'audio/x-mulaw' && x.format.sampleRate === 8000), 'playAudio messages say audio/x-mulaw, 8000');
  check(a.events.some(e => e.event === 'checkpoint'), 'checkpoints are sent');
  let seen = callsSeen().filter(c => c.from === '+15551230001');
  let last = seen[seen.length - 1];
  check(!!last && last.connectedAt && last.start === 'beginning' && last.startSeconds === 0 && !last.endedAt,
    'the app is told the call connected, from the beginning', JSON.stringify(last));

  // Press 1: live.
  const pressedAt = Date.now() - a.t0;
  a.press('1');
  await sleep(1200);
  seen = callsSeen().filter(c => c.from === '+15551230001');
  last = seen[seen.length - 1];
  check(!!last && last.liveAt && last.livePresses === 1, 'the app is told the caller pressed 1');
  check(a.events.some(e => e.event === 'clearAudio' && e.at >= pressedAt), 'pressing 1 clears what is queued');
  const liveSecond = pushed / RATE;
  const afterJump = pitch(a.bytesSince(pressedAt).subarray(0, 2400));
  const expected = [freqAt(liveSecond - 3), freqAt(liveSecond - 2), freqAt(liveSecond - 1)];
  check(expected.includes(afterJump.freq) && afterJump.freq !== 400, 'after pressing 1 the caller hears live', `${afterJump.freq} Hz, live is at ${liveSecond.toFixed(1)} s`);

  // Hang up and call back: carries on where they were.
  await sleep(1500);
  const heardUntil = pushed / RATE - 1;
  a.hangUp();
  await a.closed;
  await sleep(300);
  seen = callsSeen().filter(c => c.from === '+15551230001');
  last = seen[seen.length - 1];
  check(!!last && last.endedAt && last.end === 'hung up', 'the app is told the caller hung up', last && last.end);
  r = await answer('+15551230001', undefined, { 'X-Plivo-Signature-V3': 'sig' });
  check(/Welcome back/.test(r.xml), 'returning caller is welcomed back');
  seen = callsSeen();
  check(seen[seen.length - 1].via === 'phone', 'a request Plivo signed counts as a phone call');
  const a2 = new Call(streamUrl(r.xml));
  await a2.start();
  await sleep(1200);
  const resumed = pitch(a2.allBytes().subarray(0, 2400));
  const near = [];
  for (let s = heardUntil - 6; s <= heardUntil + 1; s++) near.push(freqAt(s));
  check(near.includes(resumed.freq) && resumed.freq !== 400, 'returning caller resumes a little before where they left', `${resumed.freq} Hz, they were at about ${heardUntil.toFixed(1)} s`);

  // Someone else starts at the beginning; no caller ID does too.
  r = await answer('+15559870002');
  const b = new Call(streamUrl(r.xml));
  await b.start();
  await sleep(800);
  check(pitch(b.allBytes().subarray(0, 2400)).freq === 400, 'a different caller starts at the beginning');
  b.hangUp();
  r = await answer('');
  check(/play from the beginning/.test(r.xml), 'a call without caller ID is treated as new');

  // The status the app sees.
  const status = await app.next('status', 2000);
  check(status.live && status.callers >= 1 && status.receivedKbps > 200 && status.level && status.level.peakDb > -20, 'app status: live, callers, receive rate, level',
    `callers ${status.callers}, ${status.receivedKbps} kbps, peak ${status.level && status.level.peakDb.toFixed(1)} dB`);
  check(status.streaming && status.positions.length === status.callers && status.positions.every(p => p.heard >= 0) &&
    status.apps.length === 1 && status.apps[0].you && status.apps[0].ip === '127.0.0.1',
    'app status: streaming, where each caller is, connected apps with IPs', JSON.stringify({ positions: status.positions, apps: status.apps }));
  const allLevels = [];
  for (const m of app.messages) if (m.type === 'status') m.levels.forEach((v, i) => { allLevels[m.levelsFrom + i] = v; });
  check(allLevels.length >= 8 && allLevels.slice(0, 8).every(v => v > -20), 'app status carries the level of each second', allLevels.slice(0, 10).join(' '));

  // A one-time stream token can't be used again.
  const reuse = new Call(streamUrl(r.xml));
  await reuse.start();
  const reuse2 = new Call(streamUrl(r.xml));
  await reuse2.start();
  await sleep(300);
  check(reuse2.closedCode === 4004, 'a stream token works once', `close ${reuse2.closedCode}`);
  reuse.hangUp();

  // The app reconnects: resent audio isn't stored twice, a gap becomes silence.
  clearInterval(pushTimer);
  await sleep(200);
  const beforeReconnect = (await app.next('status', 2000)).service.inputSamples;
  app.ws.close();
  await sleep(200);
  const app2 = new App();
  await app2.hello();
  const w2 = await app2.next('welcome');
  check(w2.live && w2.service.id === started.id, 'after a reconnect the app sees its service still live');
  app2.send({ type: 'attach', id: started.id });
  const att = (await app2.next('service')).service;
  check(att.inputSamples === beforeReconnect && att.inputSamples === pushed, 'attach reports how much the server has', `${att.inputSamples} samples`);
  app2.push(pushed - RATE, tone(pushed - RATE, RATE * 2));   // 1 s already stored, 1 s new
  pushed += RATE;
  await sleep(300);
  let s2 = (await app2.next('status', 2000)).service;
  check(s2.inputSamples === pushed, 'resent audio is not stored twice', `${s2.inputSamples} vs ${pushed}`);
  app2.push(pushed + RATE / 2, tone(pushed + RATE / 2, RATE / 2));   // half a second missing
  pushed += RATE;
  await sleep(300);
  s2 = (await app2.next('status', 2000)).service;
  check(s2.inputSamples === pushed, 'a gap is filled with silence', `${s2.inputSamples} vs ${pushed}`);

  // A second app can watch; it takes over streaming only when it attaches.
  const app3 = new App();
  await app3.hello();
  await app3.next('welcome');
  await sleep(200);
  check(app2.closed === null, 'a second app connection can watch without taking over', `close ${app2.closed}`);
  const watching = await app3.next('status', 2000);
  check(!watching.streaming && watching.otherStreaming && watching.apps.length === 2, 'the watching app sees another app streaming');
  app3.send({ type: 'attach', id: started.id });
  await app3.next('service');
  await sleep(200);
  check(app2.closed === 4002, 'attaching takes streaming over from the older connection', `close ${app2.closed}`);

  // The browser test player: a page, and the audio as a WAV stream.
  const playerPage = await fetch(`${BASE}/listen/${KEYS.LISTEN_KEY}`);
  check(playerPage.status === 200 && (await playerPage.text()).includes('audio.wav'), 'browser test player page');
  const listen = await fetch(`${BASE}/listen/${KEYS.LISTEN_KEY}/audio.wav`);
  const reader = listen.body.getReader();
  const firstChunk = (await reader.read()).value;
  await sleep(300);
  reader.cancel();
  check(listen.status === 200 && Buffer.from(firstChunk).subarray(0, 4).toString() === 'RIFF', 'browser test player sends a WAV stream');
  check((await fetch(`${BASE}/listen/wrong-listen-key-0123456789`)).status === 404, 'browser test player needs its key');
  await sleep(300);
  const browserCalls = app3.messages.filter(m => m.type === 'call' && m.call.via === 'browser').map(m => m.call);
  check(browserCalls.length >= 2 && browserCalls[0].ip === '127.0.0.1' && browserCalls[browserCalls.length - 1].endedAt,
    'a browser listener shows in the call list with its IP, and when it stopped', JSON.stringify(browserCalls[browserCalls.length - 1]));

  // Stop: the recording ends; a caller near the end hears the rest and the call ends.
  app3.send({ type: 'stop' });
  const stopped = (await app3.next('stopped')).service;
  check(!!stopped.endedAt, 'stop ends the service', stopped.stopReason);
  r = await answer('+15551230001');
  check(/Continuing the recording/.test(r.xml), 'after the service, a returning caller continues the recording');
  const c = new Call(streamUrl(r.xml));
  await c.start();
  const code = await Promise.race([c.closed, sleep(20000).then(() => 'timeout')]);
  const lastSeconds = c.allBytes().length / 8000;
  check(code === 1000, 'at the end of the recording the stream closes, so Plivo says goodbye and hangs up', `close ${code} after ${lastSeconds.toFixed(1)} s of audio`);
  check(c.events.some(e => /^end\d+$/.test(e.name || '')), 'an end checkpoint is sent before closing');

  // A new service replaces the old recording on disk.
  app3.send({ type: 'start' });
  const next = (await app3.next('service')).service;
  await sleep(200);
  const dirs = fs.readdirSync(path.join(dataDir, 'services'));
  check(next.id !== started.id && dirs.length === 1 && dirs[0] === next.id, 'a new service deletes the previous recording', dirs.join(', '));
  r = await answer('+15551230001');
  check(/Welcome\. The service will play from the beginning|There is no service/.test(r.xml), 'positions start afresh with the new service');

  const health = await (await fetch(`${BASE}/health`)).json();
  check(health.ok && health.live && health.appConnected, '/health reports up, live, app connected');
  app3.ws.close();
} catch (error) {
  check(false, 'test run', error.stack || String(error));
} finally {
  server.kill();
  await sleep(300);
  fs.rmSync(dataDir, { recursive: true, force: true });
}
if (failures) console.log('\n--- server log ---\n' + serverLog.join(''));
console.log(`\n${results.length - failures} passed, ${failures} failed`);
process.exit(failures ? 1 : 0);
