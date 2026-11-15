// Benchmark: the server's CPU and memory, and how quickly callers get new
// audio, with N callers listening live. Everything runs on this machine: the
// server (pinned to one CPU core, like a one-core VM), a pretend church app
// streaming in real time, and N pretend Plivo calls.
//
//   node test/bench.mjs [--calls 10,50,100] [--seconds 60] [--no-pin]
//
// For each N it prints the server's CPU (% of one core), its memory, what it
// sends to the callers (WebSocket data; TCP and TLS add a little more), and
// the delay from the app sending a piece of audio to a caller receiving it.
// A live caller hears it about 1 s later than that: the server keeps 1 s queued
// at the phone service as a cushion (LEAD in listener.js).

import { spawn, execFile, execFileSync } from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { promisify } from 'node:util';

const args = Object.fromEntries(process.argv.slice(2).reduce((pairs, a, i, all) => (a.startsWith('--') ? [...pairs, [a.slice(2), all[i + 1] !== undefined && !all[i + 1].startsWith('--') ? all[i + 1] : 'true']] : pairs), []));
const sizes = String(args.calls || '10,50,100').split(',').map(Number).filter(n => n > 0);
const seconds = Number(args.seconds || 60);
const pin = !args['no-pin'];
const here = path.dirname(fileURLToPath(import.meta.url));
const KEYS = { INGEST_TOKEN: 'bench-ingest-token-0123456789', PLIVO_ANSWER_KEY: 'benchanswerkey0123456789', CALLER_SECRET: 'bench-caller-secret-0123456789' };
const RATE = 16000, PACKET = 320;              // the app's audio: 16 kHz, 20 ms packets
const ULAW_PER_PACKET = PACKET / 2;            // the server keeps 8 kHz mu-law: 160 bytes
const sleep = ms => new Promise(r => setTimeout(r, ms));
const now = () => performance.now();

// --- processes ---------------------------------------------------------------
function powershell(command) {
  return execFileSync('powershell', ['-NoProfile', '-Command', command], { encoding: 'utf8' }).trim();
}
// CPU seconds used so far and memory in MB of a process, and when that was
// read. Asynchronous: blocking here would stall the pretend app and callers.
async function usage(pid) {
  if (process.platform === 'win32') {
    const asked = now();
    const { stdout } = await promisify(execFile)('powershell', ['-NoProfile', '-Command',
      `$p = Get-Process -Id ${pid}; [string]::Format([Globalization.CultureInfo]::InvariantCulture, '{0} {1} {2}', $p.TotalProcessorTime.TotalSeconds, $p.WorkingSet64, $p.PeakWorkingSet64)`]);
    const [cpu, ws, peak] = stdout.trim().split(' ').map(Number);
    // PowerShell takes a moment to start; it read the numbers near the end.
    return { cpu, mb: ws / 1048576, peakMb: peak / 1048576, at: asked + (now() - asked) * 0.9 };
  }
  const at = now();
  const stat = fs.readFileSync(`/proc/${pid}/stat`, 'utf8');
  const f = stat.slice(stat.lastIndexOf(')') + 2).split(' ');
  const status = fs.readFileSync(`/proc/${pid}/status`, 'utf8');
  const kb = name => Number((new RegExp(`^${name}:\\s+(\\d+)`, 'm').exec(status) || [])[1] || 0);
  return { cpu: (Number(f[11]) + Number(f[12])) / 100, mb: kb('VmRSS') / 1024, peakMb: kb('VmHWM') / 1024, at };
}
function pinTo(pid, cpus) {
  if (process.platform === 'win32') powershell(`(Get-Process -Id ${pid}).ProcessorAffinity = [IntPtr]${cpus.reduce((m, c) => m + 2 ** c, 0)}`);
  else execFileSync('taskset', ['-pc', cpus.join(','), String(pid)], { stdio: 'ignore' });
}

async function startServer(maxCallers) {
  const port = 19000 + Math.floor(Math.random() * 1000);
  const dataDir = fs.mkdtempSync(path.join(os.tmpdir(), 'phone-stream-bench-'));
  const child = spawn(process.execPath, [path.join(here, '..', 'main.js')], {
    env: { ...process.env, ...KEYS, PORT: String(port), DATA_DIR: dataDir, PUBLIC_WS_BASE: `ws://127.0.0.1:${port}`, NEW_CALLER_START: 'live', MAX_CALLERS: String(maxCallers) },
    stdio: ['ignore', 'pipe', 'pipe']
  });
  const log = [];
  child.stdout.on('data', d => log.push(String(d)));
  child.stderr.on('data', d => log.push(String(d)));
  for (let i = 0; i < 50; i++) {
    try { if ((await fetch(`http://127.0.0.1:${port}/health`)).ok) return { child, port, dataDir, log }; } catch { /* not yet */ }
    await sleep(100);
  }
  child.kill();
  throw new Error('server did not start:\n' + log.join(''));
}

// --- the church app: streams a tone in real time -------------------------------
const second = Buffer.alloc(RATE * 2);
for (let i = 0; i < RATE; i++) second.writeInt16LE(Math.round(Math.sin(2 * Math.PI * 440 * i / RATE) * (0.2 + 0.15 * Math.sin(2 * Math.PI * i / RATE)) * 32767), i * 2);

async function startApp(port) {
  const ws = new WebSocket(`ws://127.0.0.1:${port}/ingest`);
  const waiting = new Map();
  ws.addEventListener('message', e => { if (typeof e.data === 'string') { const m = JSON.parse(e.data); const w = waiting.get(m.type); if (w) { waiting.delete(m.type); w(m); } } });
  const next = type => new Promise(r => waiting.set(type, r));
  await new Promise(r => ws.addEventListener('open', r));
  const welcome = next('welcome');
  ws.send(JSON.stringify({ type: 'hello', token: KEYS.INGEST_TOKEN, client: 'bench', format: 's16le', rate: RATE, channels: 1 }));
  await welcome;
  const service = next('service');
  ws.send(JSON.stringify({ type: 'start' }));
  await service;
  const pushedAt = [];                         // per packet: when it was sent
  const t0 = now();
  const timer = setInterval(() => {
    const due = Math.floor((now() - t0) * RATE / 1000);
    while ((pushedAt.length + 1) * PACKET <= due) {
      const k = pushedAt.length, start = k * PACKET;
      const head = Buffer.alloc(8);
      head.writeBigUInt64LE(BigInt(start));
      const off = (start % RATE) * 2;
      ws.send(Buffer.concat([head, second.subarray(off, off + PACKET * 2)]));
      pushedAt.push(now());
    }
  }, 10);
  return { pushedAt, stop: () => { clearInterval(timer); ws.close(); } };
}

// --- one pretend Plivo call, listening live -------------------------------------
async function caller(n, port, app, window) {
  const r = await fetch(`http://127.0.0.1:${port}/plivo/answer/${KEYS.PLIVO_ANSWER_KEY}`, {
    method: 'POST', headers: { 'Content-Type': 'application/x-www-form-urlencoded', 'X-Plivo-Signature-V3': 'bench' },
    body: new URLSearchParams({ CallUUID: `bench-${n}`, From: `+1555777${String(n).padStart(4, '0')}`, To: '+15550000000', Direction: 'inbound' })
  });
  const url = (/<Stream[^>]*>([^<]+)<\/Stream>/.exec(await r.text()) || [])[1];
  if (!url) throw new Error(`call ${n} got no stream`);
  const c = { bytes: 0, wire: 0, messages: 0, delays: [], gaps: 0, lastAt: 0, firstMs: null, closed: null };
  const ws = new WebSocket(url);
  let sentStart = 0;
  ws.addEventListener('message', e => {
    const t = now();
    const m = JSON.parse(e.data);
    const measuring = t >= window.from && t <= window.to;
    if (measuring) { c.wire += e.data.length + 4; c.messages++; }      // + WebSocket frame header
    if (m.event === 'playAudio') {
      if (c.firstMs === null) c.firstMs = t - sentStart;
      if (measuring) {
        c.bytes += Buffer.byteLength(m.media.payload, 'base64');
        if (c.lastAt && t - c.lastAt > 600) c.gaps++;
      }
      c.lastAt = t;
    } else if (m.event === 'checkpoint') {
      const position = Number(String(m.name).replace(/^\D+/, ''));
      const sent = app.pushedAt[Math.floor((position - 1) / ULAW_PER_PACKET)];
      if (measuring && sent !== undefined) c.delays.push(t - sent);
      setTimeout(() => ws.readyState === 1 && ws.send(JSON.stringify({ event: 'playedStream', streamId: 'st', name: m.name })), 1000);
    }
  });
  ws.addEventListener('close', e => { c.closed = e.code; });
  await new Promise((res, rej) => { ws.addEventListener('open', res); ws.addEventListener('error', () => rej(new Error(`call ${n}: stream did not open`))); });
  sentStart = now();
  ws.send(JSON.stringify({ event: 'start', sequenceNumber: 1, start: { callId: `c${n}`, streamId: 'st', accountId: 'a', tracks: ['inbound'], mediaFormat: { encoding: 'audio/x-mulaw', sampleRate: 8000 } }, extra_headers: '' }));
  c.hangUp = () => ws.close();
  return c;
}

const pct = (list, p) => { if (!list.length) return NaN; const s = [...list].sort((a, b) => a - b); return s[Math.min(s.length - 1, Math.floor(p / 100 * s.length))]; };

async function run(n) {
  const server = await startServer(n + 10);
  let pinned = false;
  if (pin) { try { pinTo(server.child.pid, [0]); pinned = true; } catch { /* not allowed here */ } }
  const app = await startApp(server.port);
  await sleep(2000);                           // calls need at least 1 s recorded
  const window = { from: Infinity, to: Infinity };
  const calls = [];
  for (let i = 1; i <= n; i++) { calls.push(caller(i, server.port, app, window)); await sleep(30); }
  const list = await Promise.all(calls);
  await sleep(3000);
  window.from = now();
  window.to = window.from + seconds * 1000;
  const before = await usage(server.child.pid);
  await sleep(Math.max(0, window.to - now()));
  const after = await usage(server.child.pid);
  const wall = (window.to - window.from) / 1000;
  const cpuWall = (after.at - before.at) / 1000;
  for (const c of list) c.dropped = c.closed !== null;      // the server ended it during the run
  for (const c of list) c.hangUp();
  app.stop();
  await sleep(500);
  server.child.kill();
  await sleep(300);
  fs.rmSync(server.dataDir, { recursive: true, force: true });
  const delays = list.flatMap(c => c.delays);
  const ok = list.filter(c => !c.dropped && c.gaps === 0 && c.bytes >= (seconds - 0.5) * 8000).length;
  return {
    n, ok, pinned,
    cpu: (after.cpu - before.cpu) / cpuWall * 100,
    mb: after.mb, peakMb: after.peakMb,
    mbps: list.reduce((s, c) => s + c.wire, 0) * 8 / wall / 1e6,
    messagesPerSecond: list.reduce((s, c) => s + c.messages, 0) / wall,
    delay50: pct(delays, 50), delay95: pct(delays, 95), delayMax: Math.max(...delays),
    first50: pct(list.map(c => c.firstMs), 50)
  };
}

const cpus = os.cpus();
console.log(`${cpus[0].model.trim()}, ${cpus.length} logical CPUs, ${os.platform()}, Node ${process.version}; ${seconds} s per run`);
if (pin && cpus.length >= 4) { try { pinTo(process.pid, cpus.map((_, i) => i).slice(2)); } catch { /* fine */ } }
const results = [];
for (const n of sizes) {
  const r = await run(n);
  results.push(r);
  console.log(`${String(n).padStart(4)} callers: ${r.ok === n ? 'all' : `${r.ok} of ${n}`} kept real-time pace; server CPU ${r.cpu.toFixed(1)}% of one core${r.pinned ? '' : ' (not pinned)'}, ` +
    `memory ${r.mb.toFixed(0)} MB (peak ${r.peakMb.toFixed(0)}); sending ${r.mbps.toFixed(2)} Mbps; ` +
    `delay to callers median ${r.delay50.toFixed(0)} ms, 95% ${r.delay95.toFixed(0)} ms, max ${r.delayMax.toFixed(0)} ms; first audio after ${r.first50.toFixed(0)} ms`);
}
console.log('\n| Callers | Kept real-time | Server CPU (one core) | Memory | Sent to callers | Delay median / 95% / max |');
console.log('| ---: | :---: | ---: | ---: | ---: | ---: |');
for (const r of results) {
  console.log(`| ${r.n} | ${r.ok}/${r.n} | ${r.cpu.toFixed(1)}% | ${r.mb.toFixed(0)} MB | ${r.mbps.toFixed(1)} Mbps | ${r.delay50.toFixed(0)} / ${r.delay95.toFixed(0)} / ${r.delayMax.toFixed(0)} ms |`);
}
