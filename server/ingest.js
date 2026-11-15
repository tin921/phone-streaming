'use strict';
// The church app's connection. Any number of authenticated apps may be
// connected to watch (status, callers); one at a time streams the service.
//
// App -> server, text (JSON):
//   {type:'hello', token, client, format:'s16le', rate:16000, channels:1}
//   {type:'start'}            begin a new service (the previous recording is deleted)
//   {type:'attach', id}       carry on with service id after a reconnect
//   {type:'stop', id}         end the service
//   {type:'ping', t}
//   'start' and 'attach' make this connection the one that streams; an
//   earlier streaming connection is closed (4002).
// App -> server, binary: 8-byte little-endian sample index, then s16le PCM.
//   The index is the position of the first sample since the service began, so
//   audio resent after a reconnect is not stored twice and a gap becomes silence.
// Server -> app:
//   welcome {service, live, links:{status, listen}}, then calls
//   service | no-service | stopped {service}, pong {t}, error {message}
//   calls {serviceId, calls:[...], levels:[...]}   the whole call list (on connect, new service)
//   call {call}                                     one call added or changed
//   status every second: {now, service, live, streaming, otherStreaming, callers,
//     browsers, positions:[{id, heard}], apps:[{ip, streaming, you, since}],
//     receivedKbps, level, levelsFrom, levels}
//   levels: the peak level (dBFS, whole numbers, -99 = silence) of each second
//   of the recording; status carries the seconds added since the last one.

const crypto = require('node:crypto');
const { PhoneEncoder, pcmLevels } = require('./audio');

const RATE = 16000;
const BYTES_PER_SECOND = 8000;
const MAX_GAP_SAMPLES = RATE * 600;

function sameSecret(a, b) {
  const x = crypto.createHash('sha256').update(String(a)).digest();
  const y = crypto.createHash('sha256').update(String(b)).digest();
  return crypto.timingSafeEqual(x, y);
}

class Ingest {
  // audience(): {callers, browsers, positions} of who is listening now.
  constructor({ store, token, log, audience, callLog, links }) {
    this.store = store;
    this.token = token;
    this.log = log;
    this.audience = audience;
    this.callLog = callLog;
    this.links = links;
    this.clients = new Set();
    this.active = null;          // the connection that streams
    this.encoder = null;
    this.encoderFor = null;
    this.levels = [];
    this.levelsFor = store.current ? store.current.id : null;
    callLog.on('call', call => { for (const c of this.clients) this.send(c, { type: 'call', call }); });
    callLog.on('reset', () => { for (const c of this.clients) this.sendCalls(c); });
  }

  accept(ws, remote) {
    const state = { ws, remote, authed: false, attached: null, bytesWindow: 0, level: null, lastAudio: 0, levelsSent: 0, since: Date.now() };
    const helloTimer = setTimeout(() => { if (!state.authed) ws.close(4001, 'no hello'); }, 5000);
    ws.on('text', text => this.onText(state, text, helloTimer));
    ws.on('binary', data => this.onAudio(state, data));
    ws.on('close', (code, reason) => {
      clearTimeout(helloTimer);
      clearInterval(state.statusTimer);
      this.clients.delete(state);
      if (this.active === state) {
        this.active = null;
        this.log(`streaming app disconnected (${code}${reason ? ' ' + reason : ''})`);
      } else if (state.authed) {
        this.log(`app disconnected from ${state.remote} (${code})`);
      }
    });
  }

  send(state, value) { if (!state.ws.closed) state.ws.sendJson(value); }

  serviceLevels() {
    const id = this.store.current ? this.store.current.id : null;
    if (id !== this.levelsFor) {
      this.levelsFor = id;
      this.levels = [];
      for (const c of this.clients) c.levelsSent = 0;
    }
    return this.levels;
  }

  sendCalls(state) {
    const levels = this.serviceLevels();
    state.levelsSent = Math.max(0, levels.length - 1);
    this.send(state, { type: 'calls', serviceId: this.callLog.serviceId, calls: this.callLog.list(), levels });
  }

  // Makes this connection the one that streams.
  takeOver(state) {
    if (this.active && this.active !== state) {
      this.log('another app took over streaming');
      this.active.attached = null;
      this.active.ws.close(4002, 'another app is streaming now');
    }
    this.active = state;
  }

  onText(state, text, helloTimer) {
    let message;
    try { message = JSON.parse(text); } catch { return state.ws.close(4000, 'bad json'); }
    if (!state.authed) {
      if (message.type !== 'hello' || !this.token || !sameSecret(message.token, this.token)) {
        this.log(`app refused from ${state.remote}: wrong or missing token`);
        return state.ws.close(4001, 'unauthorized');
      }
      if (message.format !== 's16le' || message.rate !== RATE || message.channels !== 1) {
        return state.ws.close(4003, 'audio must be s16le, 16000 Hz, mono');
      }
      clearTimeout(helloTimer);
      state.authed = true;
      this.clients.add(state);
      this.log(`app connected from ${state.remote} (${message.client || 'unknown client'})`);
      state.statusTimer = setInterval(() => this.status(state), 1000);
      this.send(state, { type: 'welcome', service: this.store.info(), live: this.store.live(), links: this.links });
      return this.sendCalls(state);
    }
    switch (message.type) {
      case 'start': {
        this.takeOver(state);
        const service = this.store.startService();
        state.attached = service.id;
        return this.send(state, { type: 'service', service: this.store.info() });
      }
      case 'attach': {
        if (this.store.live() && this.store.current.id === message.id) {
          this.takeOver(state);
          state.attached = message.id;
          this.log(`app resumed service ${message.id} at ${this.store.current.inputSamples} samples`);
          return this.send(state, { type: 'service', service: this.store.info() });
        }
        return this.send(state, { type: 'no-service', service: this.store.info() });
      }
      case 'stop': {
        if (this.store.live() && (!message.id || this.store.current.id === message.id)) this.store.endService('stopped by the operator');
        state.attached = null;
        if (this.active === state) this.active = null;
        return this.send(state, { type: 'stopped', service: this.store.info() });
      }
      case 'ping':
        return this.send(state, { type: 'pong', t: message.t });
      default:
        return this.send(state, { type: 'error', message: `unknown message ${message.type}` });
    }
  }

  onAudio(state, data) {
    if (!state.authed) return state.ws.close(4001, 'unauthorized');
    const service = this.store.current;
    if (this.active !== state || !state.attached || !service || service.endedAt || service.id !== state.attached || data.length < 8) return;
    state.bytesWindow += data.length;
    const start = Number(data.readBigUInt64LE(0));
    let pcm = data.subarray(8, data.length - ((data.length - 8) % 2));
    if (this.encoderFor !== service.id) {
      this.encoder = new PhoneEncoder();
      this.encoderFor = service.id;
    }
    const levels = this.serviceLevels();
    let have = service.inputSamples;
    if (start > have) {
      const gap = Math.min(start - have, MAX_GAP_SAMPLES);
      this.store.append(this.encoder.encode(Buffer.alloc(gap * 2)), have + gap);
      if (start - have > gap) this.log(`gap of ${((start - have) / RATE).toFixed(1)} s shortened to 10 minutes`);
      else this.log(`gap of ${(gap / RATE).toFixed(1)} s filled with silence`);
      have = start;
    }
    const skip = Math.max(0, have - start) * 2;
    if (skip >= pcm.length) return;
    pcm = pcm.subarray(skip);
    state.level = pcmLevels(pcm);
    state.lastAudio = Date.now();
    this.store.append(this.encoder.encode(pcm), have + pcm.length / 2);
    // Per-second peaks for the app's graph (seconds of recording).
    const second = Math.max(0, Math.ceil(service.bytes / BYTES_PER_SECOND) - 1);
    const peak = Math.max(-99, Math.round(state.level.peakDb));
    while (levels.length <= second) levels.push(-99);
    levels[second] = Math.max(levels[second], peak);
  }

  status(state) {
    const kbps = state.bytesWindow * 8 / 1000;
    state.bytesWindow = 0;
    const levels = this.serviceLevels();
    const from = Math.min(state.levelsSent, levels.length);
    state.levelsSent = Math.max(0, levels.length - 1);
    const who = this.audience();
    this.send(state, {
      type: 'status', now: Date.now(), service: this.store.info(), live: this.store.live(),
      streaming: this.active === state && !!state.attached, otherStreaming: !!this.active && this.active !== state,
      callers: who.callers, browsers: who.browsers, positions: who.positions,
      apps: [...this.clients].map(c => ({ ip: c.remote, streaming: c === this.active, you: c === state, since: c.since })),
      receivedKbps: Math.round(kbps),
      level: state.lastAudio && Date.now() - state.lastAudio < 2000 ? state.level : null,
      levelsFrom: from, levels: levels.slice(from)
    });
  }

  connected() { return !!this.active; }
}

module.exports = { Ingest, sameSecret };
