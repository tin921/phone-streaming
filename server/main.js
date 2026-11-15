'use strict';
// Phone stream server: takes the church app's audio over a WebSocket, keeps
// the current service's recording, and plays it to phone callers through
// Plivo, each caller at their own position (press 1 for live).
//
// Runs behind Apache (TLS on phone.<domain>), listening on 127.0.0.1 only.
// Settings come from environment variables; see README.md.

const http = require('node:http');
const path = require('node:path');
const crypto = require('node:crypto');
const ws = require('./ws');
const { Store } = require('./store');
const { Ingest } = require('./ingest');
const { Plivo } = require('./plivo');
const { CallLog } = require('./calls');
const { ULAW_TO_LINEAR } = require('./audio');

const env = process.env;
const num = (name, fallback) => (env[name] !== undefined && env[name] !== '' ? Number(env[name]) : fallback);
const config = {
  host: env.HOST || '127.0.0.1',
  port: num('PORT', 8090),
  dataDir: path.resolve(env.DATA_DIR || path.join(__dirname, 'data')),
  publicWsBase: env.PUBLIC_WS_BASE || `wss://${env.PUBLIC_HOST || 'phone.example.org'}`,
  publicUrl: (env.PUBLIC_URL || `https://${env.PUBLIC_HOST || 'phone.example.org'}`).replace(/\/+$/, ''),
  ingestToken: env.INGEST_TOKEN || '',
  plivoAnswerKey: env.PLIVO_ANSWER_KEY || '',
  callerSecret: env.CALLER_SECRET || '',
  listenKey: env.LISTEN_KEY && env.LISTEN_KEY.length >= 16 ? env.LISTEN_KEY : '',
  newCallerStart: env.NEW_CALLER_START === 'live' ? 'live' : 'beginning',
  rewindSeconds: num('REWIND_SECONDS', 3),
  maxCallers: num('MAX_CALLERS', 100),
  streamTimeoutSeconds: num('STREAM_TIMEOUT_SECONDS', 14400),
  autoEndMinutes: num('AUTO_END_MINUTES', 180),
  speakLanguage: env.SPEAK_LANGUAGE || 'en-US',
  speakVoice: env.SPEAK_VOICE || 'WOMAN',
  texts: {
    welcomeBeginning: env.TEXT_WELCOME_BEGINNING ?? 'Welcome. The service will play from the beginning. Press 1 at any time to go to the live service.',
    welcomeLive: env.TEXT_WELCOME_LIVE ?? 'Welcome. You are joining the live service.',
    welcomeBackLive: env.TEXT_WELCOME_BACK_LIVE ?? 'Welcome back. Continuing where you left off. Press 1 at any time to go to the live service.',
    welcomeRecording: env.TEXT_WELCOME_RECORDING ?? 'Welcome. This is the recording of the latest service.',
    welcomeBackRecording: env.TEXT_WELCOME_BACK_RECORDING ?? 'Welcome back. Continuing the recording where you left off.',
    noService: env.TEXT_NO_SERVICE ?? 'There is no service at the moment. Please call again later. Goodbye.',
    ended: env.TEXT_ENDED ?? 'The service has ended. Thank you for listening. Goodbye.',
    busy: env.TEXT_BUSY ?? 'Sorry, the line is full right now. Please try again later.'
  }
};

for (const name of ['ingestToken', 'plivoAnswerKey', 'callerSecret']) {
  if (config[name].length < 16) {
    console.error(`Setting ${name.replace(/[A-Z]/g, c => '_' + c).toUpperCase()} is missing or shorter than 16 characters.`);
    process.exit(2);
  }
}

const log = message => console.log(`${new Date().toISOString()} ${message}`);
const store = new Store({ dataDir: config.dataDir, log });
const callLog = new CallLog({ store });
const plivo = new Plivo({ store, config, log, callLog });
const browsers = new Set();      // test player listeners: { record, cursor }
const ingest = new Ingest({
  store, token: config.ingestToken, log, callLog,
  links: { status: `${config.publicUrl}/`, listen: config.listenKey ? `${config.publicUrl}/listen/${config.listenKey}` : null },
  audience: () => ({
    callers: plivo.count(), browsers: browsers.size,
    positions: [...plivo.positions(), ...[...browsers].map(b => ({ id: b.record.id, heard: Math.round(b.cursor / 8000) }))]
  })
});
const sockets = new Set();
let lastAudioAt = Date.now();
store.on('data', () => { lastAudioAt = Date.now(); });

function sameText(a, b) {
  const x = crypto.createHash('sha256').update(String(a)).digest();
  const y = crypto.createHash('sha256').update(String(b)).digest();
  return crypto.timingSafeEqual(x, y);
}

// The client's address. Behind Apache it is the last X-Forwarded-For entry
// (Apache appends the address it saw; earlier entries come from the client
// and can be anything).
function remoteOf(req) {
  const forwarded = String(req.headers['x-forwarded-for'] || '').split(',').map(s => s.trim()).filter(Boolean);
  return (forwarded.length ? forwarded[forwarded.length - 1] : String(req.socket.remoteAddress || '')).replace(/^::ffff:/, '');
}

async function readBody(req, limit = 16384) {
  let body = '';
  for await (const chunk of req) {
    body += chunk;
    if (body.length > limit) throw new Error('request body too large');
  }
  return body;
}

function send(res, status, type, body) {
  res.writeHead(status, { 'Content-Type': type, 'Content-Length': Buffer.byteLength(body), 'Cache-Control': 'no-store' });
  res.end(body);
}

const STATUS_PAGE = `<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Phone stream</title>
<style>
:root{color-scheme:light dark;--bg:#f6f7f9;--card:#fff;--ink:#1d2430;--muted:#5d6878;--ok:#1d7a46;--off:#8a5a00}
@media (prefers-color-scheme:dark){:root{--bg:#14181e;--card:#1d232b;--ink:#e6eaf0;--muted:#9aa5b4;--ok:#5fd18f;--off:#e3b04b}}
body{margin:0;background:var(--bg);color:var(--ink);font:16px/1.5 system-ui,sans-serif}
main{max-width:520px;margin:48px auto;padding:0 16px}
.card{background:var(--card);border-radius:12px;padding:20px 24px;box-shadow:0 1px 3px #0002}
h1{font-size:20px;margin:0 0 12px}dl{display:grid;grid-template-columns:auto 1fr;gap:6px 16px;margin:0}
dt{color:var(--muted)}dd{margin:0;font-weight:600}.ok{color:var(--ok)}.off{color:var(--off)}
p{color:var(--muted);font-size:14px;margin:14px 0 0}
</style></head><body><main><div class="card">
<h1>Phone stream server</h1>
<dl><dt>Server</dt><dd id="server">checking…</dd>
<dt>Service</dt><dd id="live">–</dd>
<dt>Church app</dt><dd id="app">–</dd>
<dt>Callers</dt><dd id="callers">–</dd></dl>
<p>Updates every 5 seconds.</p></div></main>
<script>
const set=(id,text,cls)=>{const e=document.getElementById(id);e.textContent=text;e.className=cls||''};
async function poll(){try{const r=await fetch('/health',{cache:'no-store'});const h=await r.json();
set('server','up','ok');set('live',h.live?'live':'not live',h.live?'ok':'off');
set('app',h.appConnected?'connected':'not connected',h.appConnected?'ok':'off');set('callers',String(h.callers))}
catch(e){set('server','not reachable','off')}}
poll();setInterval(poll,5000);
</script></body></html>
`;

async function route(req, res) {
  const url = new URL(req.url, 'http://localhost');
  if (url.pathname === '/' && req.method === 'GET') return send(res, 200, 'text/html; charset=utf-8', STATUS_PAGE);
  if (url.pathname === '/health' && req.method === 'GET') {
    return send(res, 200, 'application/json', JSON.stringify({
      ok: true, live: store.live(), appConnected: ingest.connected(), callers: plivo.count()
    }));
  }
  const answer = /^\/plivo\/answer\/([A-Za-z0-9_-]+)$/.exec(url.pathname);
  if (answer && (req.method === 'POST' || req.method === 'GET')) {
    if (!sameText(answer[1], config.plivoAnswerKey)) return send(res, 404, 'text/plain', 'not found');
    const params = Object.fromEntries(url.searchParams);
    if (req.method === 'POST') Object.assign(params, Object.fromEntries(new URLSearchParams(await readBody(req))));
    const signed = req.headers['x-plivo-signature-v3'] || req.headers['x-plivo-signature-v2'];
    return send(res, 200, 'application/xml', plivo.answer(params, { ip: remoteOf(req), via: signed ? 'phone' : 'test' }));
  }
  const listen = /^\/listen\/([A-Za-z0-9_-]+)(\/audio\.wav)?$/.exec(url.pathname);
  if (listen && req.method === 'GET' && config.listenKey && sameText(listen[1], config.listenKey)) {
    return listen[2] ? listenInBrowser(req, res) : send(res, 200, 'text/html; charset=utf-8', LISTEN_PAGE);
  }
  send(res, 404, 'text/plain', 'not found');
}

// The browser test player, at https://<host>/listen/<LISTEN_KEY> (only when
// LISTEN_KEY is set). It plays the endless WAV below through the Web Audio
// API, which phones handle better than an <audio> element on a stream that
// never ends; iPhones need the ring/silent switch off for Web Audio before iOS 17.
const LISTEN_PAGE = `<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Phone stream test player</title>
<style>
:root{color-scheme:light dark;--bg:#f6f7f9;--card:#fff;--ink:#1d2430;--muted:#5d6878;--accent:#1d6fd8;--ok:#1d7a46;--off:#8a5a00}
@media (prefers-color-scheme:dark){:root{--bg:#14181e;--card:#1d232b;--ink:#e6eaf0;--muted:#9aa5b4;--accent:#5b9dff;--ok:#5fd18f;--off:#e3b04b}}
body{margin:0;background:var(--bg);color:var(--ink);font:17px/1.5 system-ui,sans-serif}
main{max-width:460px;margin:40px auto;padding:0 16px}
.card{background:var(--card);border-radius:14px;padding:22px 24px;box-shadow:0 1px 3px #0002;text-align:center}
h1{font-size:20px;margin:0 0 4px}p{color:var(--muted);font-size:14px;margin:8px 0 0}
button{margin:18px 0 6px;width:100%;padding:16px;font:600 20px system-ui,sans-serif;border:0;border-radius:12px;background:var(--accent);color:#fff}
#state{font-weight:600}.ok{color:var(--ok)}.off{color:var(--off)}
</style></head><body><main><div class="card">
<h1>Phone stream test player</h1>
<p>What phone callers hear, a few seconds behind live.</p>
<button id="play">&#9654; Listen</button>
<div id="state">Stopped</div>
<p>On an iPhone, turn off silent mode if you hear nothing.</p>
</div></main>
<script>
const btn = document.getElementById('play'), stateEl = document.getElementById('state');
const show = (text, cls) => { stateEl.textContent = text; stateEl.className = cls || ''; };
let ctx = null, abort = null;
btn.onclick = () => (abort ? stop('Stopped') : play());
function stop(text, cls) {
  if (abort) abort.abort();
  abort = null;
  if (ctx) { ctx.close(); ctx = null; }
  btn.innerHTML = '&#9654; Listen';
  show(text, cls);
}
async function play() {
  if (navigator.audioSession) navigator.audioSession.type = 'playback';
  ctx = new (window.AudioContext || window.webkitAudioContext)();
  await ctx.resume();
  abort = new AbortController();
  btn.innerHTML = '&#9632; Stop';
  show('Connecting…');
  try {
    const r = await fetch(location.pathname.replace(/\\/$/, '') + '/audio.wav', { signal: abort.signal, cache: 'no-store' });
    if (!r.ok) return stop(r.status === 404 ? 'No service on the server yet' : 'Server answered ' + r.status, 'off');
    const reader = r.body.getReader();
    const step = 8000 / ctx.sampleRate;
    let skip = 44, odd = null, prev = 0, t = 0, next = 0;
    show('Playing', 'ok');
    for (;;) {
      const { value, done } = await reader.read();
      if (done) break;
      let bytes = value;
      if (skip) { const n = Math.min(skip, bytes.length); bytes = bytes.subarray(n); skip -= n; }
      if (odd !== null) { const b = new Uint8Array(bytes.length + 1); b[0] = odd; b.set(bytes, 1); bytes = b; odd = null; }
      if (bytes.length & 1) { odd = bytes[bytes.length - 1]; bytes = bytes.subarray(0, bytes.length - 1); }
      const n = bytes.length / 2;
      if (!n || !ctx) continue;
      const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.length);
      const input = new Float32Array(n);
      for (let i = 0; i < n; i++) input[i] = view.getInt16(i * 2, true) / 32768;
      // 8 kHz to the device rate, linear, carried across chunks.
      const out = new Float32Array(Math.ceil((n - t) / step) + 1);
      let k = 0;
      while (t < n - 1) { const i = Math.floor(t), f = t - i, a = i < 0 ? prev : input[i]; out[k++] = a + (input[i + 1] - a) * f; t += step; }
      t -= n; prev = input[n - 1];
      if (!k) continue;
      const buffer = ctx.createBuffer(1, k, ctx.sampleRate);
      buffer.getChannelData(0).set(out.subarray(0, k));
      const now = ctx.currentTime;
      if (next < now + 0.05) next = now + 0.3;
      if (next > now + 6) continue;             // far ahead: drop to stay near live
      const src = ctx.createBufferSource();
      src.buffer = buffer;
      src.connect(ctx.destination);
      src.start(next);
      next += buffer.duration;
    }
    stop('The stream ended (the service ended or a new one started)', 'off');
  } catch (e) {
    if (abort) stop('Connection lost: ' + e.message, 'off');
  }
}
</script></body></html>
`;

// The audio for the test player: the current recording from about live, as
// an endless 8 kHz WAV. Each listener shows in the app's call list.
function listenInBrowser(req, res) {
  const service = store.current;
  if (!service) return send(res, 404, 'text/plain', 'no service');
  const start = Math.max(0, service.bytes - 2 * 8000);
  const entry = { cursor: start, record: callLog.add({ via: 'browser', ip: remoteOf(req), connectedAt: Date.now(), start: 'live', startSeconds: Math.round(start / 8000) }) };
  browsers.add(entry);
  log(`browser test player from ${entry.record.ip} (${browsers.size} in browsers)`);
  let endReason = 'stopped listening';
  const header = Buffer.alloc(44);
  header.write('RIFF', 0); header.writeUInt32LE(0xffffffff, 4); header.write('WAVE', 8);
  header.write('fmt ', 12); header.writeUInt32LE(16, 16); header.writeUInt16LE(1, 20); header.writeUInt16LE(1, 22);
  header.writeUInt32LE(8000, 24); header.writeUInt32LE(16000, 28); header.writeUInt16LE(2, 32); header.writeUInt16LE(16, 34);
  header.write('data', 36); header.writeUInt32LE(0xffffffff - 36, 40);
  res.writeHead(200, { 'Content-Type': 'audio/wav', 'Cache-Control': 'no-store' });
  res.write(header);
  const id = service.id;
  let waiting = false;
  const timer = setInterval(() => {
    const now = store.current;
    if (!now || now.id !== id) { endReason = 'the recording was replaced'; return res.end(); }
    if (waiting) return;
    const ulaw = store.read(entry.cursor, 8000);
    if (!ulaw.length) { if (now.endedAt) { endReason = 'the end of the recording'; res.end(); } return; }
    entry.cursor += ulaw.length;
    const pcm = Buffer.allocUnsafe(ulaw.length * 2);
    for (let i = 0; i < ulaw.length; i++) pcm.writeInt16LE(ULAW_TO_LINEAR[ulaw[i]], i * 2);
    if (!res.write(pcm)) { waiting = true; res.once('drain', () => { waiting = false; }); }
  }, 200);
  res.on('close', () => {
    clearInterval(timer);
    browsers.delete(entry);
    callLog.update(entry.record, { endedAt: Date.now(), end: endReason });
  });
}

const server = http.createServer((req, res) => {
  route(req, res).catch(error => {
    log(`request failed: ${error.message}`);
    if (!res.headersSent) send(res, 400, 'text/plain', 'bad request');
  });
});

server.on('upgrade', (req, socket, head) => {
  const url = new URL(req.url, 'http://localhost');
  const stream = /^\/plivo\/stream\/([a-f0-9]{32})$/.exec(url.pathname);
  if (url.pathname !== '/ingest' && !stream) return ws.reject(socket, 404, 'Not Found');
  const conn = ws.accept(req, socket, head, { maxMessage: url.pathname === '/ingest' ? 1 << 20 : 1 << 16 });
  if (!conn) return;
  sockets.add(conn);
  conn.on('close', () => sockets.delete(conn));
  if (stream) plivo.stream(conn, stream[1]);
  else ingest.accept(conn, remoteOf(req));
});

// Keep connections alive through Apache, and drop ones that went silent.
setInterval(() => {
  const now = Date.now();
  for (const conn of sockets) {
    if (now - conn.lastHeard > 60000) conn.close(4008, 'silent');
    else conn.ping();
  }
}, 20000).unref();

// A service left open with no audio for hours (the app crashed or the PC was
// switched off) is ended, so callers hear the recording instead of waiting.
setInterval(() => {
  if (store.live() && !ingest.connected() && Date.now() - lastAudioAt > config.autoEndMinutes * 60000) {
    store.endService(`no audio for ${config.autoEndMinutes} minutes`);
  }
}, 60000).unref();

server.listen(config.port, config.host, () => {
  log(`phone stream server on http://${config.host}:${config.port}, data in ${config.dataDir}`);
});

function shutdown() {
  log('stopping');
  for (const conn of sockets) conn.close(1001, 'server stopping');
  store.close();
  server.close(() => process.exit(0));
  setTimeout(() => process.exit(0), 3000).unref();
}
process.on('SIGINT', shutdown);
process.on('SIGTERM', shutdown);
