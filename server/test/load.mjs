// Load test: many pretend Plivo calls at once from one process, against a
// running server. Each call answers, streams, and counts the audio it gets;
// at the end it prints how many kept real-time pace.
//
//   node test/load.mjs --answer https://phone.example.org/plivo/answer/<KEY> --calls 25 --seconds 60 [--ramp 10] [--first 1]
// --ramp: seconds over which the calls are started (default: all at once).
// --first: the first pretend number (+1555888NNNN). Numbers that called before
//          in the same service are returning callers and resume where they were.

const args = Object.fromEntries(process.argv.slice(2).reduce((pairs, a, i, all) => (a.startsWith('--') ? [...pairs, [a.slice(2), all[i + 1] !== undefined && !all[i + 1].startsWith('--') ? all[i + 1] : 'true']] : pairs), []));
if (!args.answer) { console.log('usage: node test/load.mjs --answer <Answer URL> [--calls N] [--seconds S] [--ramp S] [--first N]'); process.exit(2); }
const calls = Number(args.calls || 10), seconds = Number(args.seconds || 60), ramp = Number(args.ramp || 0), first = Number(args.first || 1);
const sleep = ms => new Promise(r => setTimeout(r, ms));

async function oneCall(n) {
  const from = `+1555888${String(first + n - 1).padStart(4, '0')}`;
  const r = await fetch(args.answer, { method: 'POST', headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
    body: new URLSearchParams({ CallUUID: `load-${Date.now()}-${n}`, From: from, To: '+15550000000', Direction: 'inbound' }) });
  const xml = await r.text();
  const url = (/<Stream[^>]*>([^<]+)<\/Stream>/.exec(xml) || [])[1];
  if (!url) return { n, error: /line is full/.test(xml) ? 'line full' : /no service/i.test(xml) ? 'no service' : `answer HTTP ${r.status}` };
  const ws = new WebSocket(url);
  let bytes = 0, firstAt = 0, closed = null, gaps = 0, lastAt = 0;
  ws.addEventListener('message', e => {
    const m = JSON.parse(e.data);
    if (m.event === 'playAudio') {
      const now = Date.now();
      if (!firstAt) firstAt = now;
      if (lastAt && now - lastAt > 600) gaps++;          // audio paused for over half a second
      lastAt = now;
      bytes += Buffer.from(m.media.payload, 'base64').length;
    }
    if (m.event === 'checkpoint') setTimeout(() => ws.readyState === 1 && ws.send(JSON.stringify({ event: 'playedStream', streamId: 'st', name: m.name })), 1000);
  });
  ws.addEventListener('close', e => { closed = e.code; });
  await new Promise((res, rej) => { ws.addEventListener('open', res); ws.addEventListener('error', e => rej(new Error(`stream did not open${e.error && e.error.message ? ': ' + e.error.message : ''}`))); });
  ws.send(JSON.stringify({ event: 'start', sequenceNumber: 1, start: { callId: `c${n}`, streamId: 'st', accountId: 'a', tracks: ['inbound'], mediaFormat: { encoding: 'audio/x-mulaw', sampleRate: 8000 } }, extra_headers: '' }));
  const t0 = Date.now();
  while (Date.now() - t0 < seconds * 1000 && closed === null) await sleep(200);
  if (ws.readyState === 1) ws.close();
  return { n, got: bytes / 8000, wall: (Date.now() - t0) / 1000, firstMs: firstAt ? firstAt - t0 : null, gaps, closed };
}

console.log(`${calls} calls, ${seconds} s each${ramp ? `, started over ${ramp} s` : ', all at once'}`);
const started = Date.now();
const results = await Promise.all(Array.from({ length: calls }, async (_, i) => {
  if (ramp) await sleep(i * ramp * 1000 / calls);
  try { return await oneCall(i + 1); } catch (error) { return { n: i + 1, error: error.message }; }
}));
const ok = results.filter(r => !r.error && r.got >= r.wall - 0.5 && r.gaps === 0);
for (const r of results) {
  if (r.error) console.log(`call ${String(r.n).padStart(3)}: ${r.error}`);
  else if (!ok.includes(r)) console.log(`call ${String(r.n).padStart(3)}: ${r.got.toFixed(1)} s audio in ${r.wall.toFixed(1)} s, ${r.gaps} pauses, closed ${r.closed}`);
}
const got = results.filter(r => !r.error).map(r => r.got);
console.log(`${ok.length} of ${calls} kept real-time pace with no pauses` +
  (got.length ? `; audio per call ${Math.min(...got).toFixed(1)}-${Math.max(...got).toFixed(1)} s; first audio within ${Math.max(...results.filter(r => r.firstMs !== null && r.firstMs !== undefined).map(r => r.firstMs))} ms` : '') +
  `; ${((Date.now() - started) / 1000).toFixed(0)} s in all`);
