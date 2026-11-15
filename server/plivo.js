'use strict';
// Plivo: the number's Answer URL is https://<host>/plivo/answer/<PLIVO_ANSWER_KEY>.
// It answers with XML: a short spoken greeting, then a bidirectional <Stream>
// to /plivo/stream/<one-time token>, over which the server sends the caller's
// audio (8 kHz mu-law, 'playAudio') and receives key presses ('dtmf').
// When the stream closes, Plivo carries on with the XML after it: a goodbye
// and a hang-up. Plivo's stream doesn't carry the caller's number, so the
// answer step hands each call a token that names it. Every call goes into the
// call log (calls.js) that the church app shows.

const crypto = require('node:crypto');
const { Listener } = require('./listener');
const { BYTES_PER_SECOND } = require('./store');

const field = (params, name) => String(params[name] || '').slice(0, 64);

function xmlEscape(text) {
  return String(text).replace(/[<>&'"]/g, c => ({ '<': '&lt;', '>': '&gt;', '&': '&amp;', "'": '&apos;', '"': '&quot;' }[c]));
}

class Plivo {
  constructor({ store, config, log, callLog }) {
    this.store = store;
    this.config = config;
    this.log = log;
    this.callLog = callLog;
    this.pending = new Map();   // token -> { key, created, record }
    this.listeners = new Set();
    setInterval(() => {
      const old = Date.now() - 120000;
      for (const [token, call] of this.pending) {
        if (call.created >= old) continue;
        this.pending.delete(token);
        this.callLog.update(call.record, { endedAt: Date.now(), end: 'no audio stream' });
      }
    }, 30000).unref();
  }

  callerKey(from) {
    const digits = String(from || '').replace(/[^0-9]/g, '');
    if (digits.length < 7) return null;      // withheld, anonymous or unusable: no resume
    return crypto.createHmac('sha256', this.config.callerSecret).update(digits).digest('hex').slice(0, 32);
  }

  speak(text) {
    const c = this.config;
    return text ? `<Speak language="${xmlEscape(c.speakLanguage)}" voice="${xmlEscape(c.speakVoice)}">${xmlEscape(text)}</Speak>` : '';
  }

  // meta: {ip, via}: who asked (Plivo's server for a real call) and 'phone'
  // for a request Plivo signed, 'test' for any other.
  answer(params, meta = {}) {
    const c = this.config;
    const t = c.texts;
    const xml = body => `<?xml version="1.0" encoding="UTF-8"?>\n<Response>${body}</Response>\n`;
    const service = this.store.current;
    const who = { from: field(params, 'From'), name: field(params, 'CallerName'), ip: meta.ip || '', via: meta.via || 'phone' };
    if (!service || service.bytes < BYTES_PER_SECOND) {
      this.log('call answered: no service to play');
      this.callLog.add({ ...who, endedAt: Date.now(), end: 'no service' });
      return xml(this.speak(t.noService) + '<Hangup/>');
    }
    if (this.listeners.size + this.pending.size >= c.maxCallers) {
      this.log('call refused: the line is full');
      this.callLog.add({ ...who, endedAt: Date.now(), end: 'line full' });
      return xml(this.speak(t.busy) + '<Hangup/>');
    }
    const key = this.callerKey(params.From);
    const saved = key ? this.store.position(key) : null;
    const live = !service.endedAt;
    // Matches startPosition(): a returning caller carries on (at live if they
    // had caught up); after the service, one who heard it all starts again.
    const unfinished = saved !== null && saved < service.bytes - 5 * BYTES_PER_SECOND;
    let greeting;
    if (live) greeting = saved !== null ? t.welcomeBackLive : (c.newCallerStart === 'live' ? t.welcomeLive : t.welcomeBeginning);
    else greeting = unfinished ? t.welcomeBackRecording : t.welcomeRecording;
    const token = crypto.randomBytes(16).toString('hex');
    this.pending.set(token, { key, created: Date.now(), record: this.callLog.add(who) });
    this.log(`call answered (${key ? 'caller ' + key.slice(0, 6) : 'no caller ID'}, ${saved !== null ? 'returning' : 'new'})`);
    const url = `${c.publicWsBase}/plivo/stream/${token}`;
    return xml(this.speak(greeting) +
      `<Stream bidirectional="true" keepCallAlive="true" audioTrack="inbound" contentType="audio/x-mulaw;rate=8000" ` +
      `streamTimeout="${c.streamTimeoutSeconds}" extraHeaders="callToken=${token}">${xmlEscape(url)}</Stream>` +
      this.speak(t.ended) + '<Hangup/>');
  }

  // Where a call starts: a returning caller a little before where they
  // stopped, a new one at the beginning or at live.
  startPosition(key) {
    const service = this.store.current;
    const saved = key ? this.store.position(key) : null;
    if (saved !== null && saved < service.bytes - 5 * BYTES_PER_SECOND) {
      return Math.max(0, saved - this.config.rewindSeconds * BYTES_PER_SECOND);
    }
    // A caller who already heard everything so far: live while the service
    // runs, the beginning once it is only a recording.
    if (!service.endedAt && (saved !== null || this.config.newCallerStart === 'live')) {
      return Math.max(0, service.bytes - BYTES_PER_SECOND);
    }
    return 0;
  }

  stream(ws, token) {
    const call = this.pending.get(token);
    if (!call) {
      this.log('stream refused: unknown or used call token');
      return ws.close(4004, 'unknown call');
    }
    this.pending.delete(token);
    let listener = null;
    let streamId = null;
    const sink = {
      sendAudio: ulaw => ws.sendJson({ event: 'playAudio', media: { contentType: 'audio/x-mulaw', sampleRate: 8000, payload: ulaw.toString('base64') } }),
      checkpoint: name => ws.sendJson({ event: 'checkpoint', streamId, name }),
      clear: () => ws.sendJson({ event: 'clearAudio', streamId }),
      end: () => ws.close(1000, 'done')
    };
    const timeout = setTimeout(() => { if (!listener) ws.close(4005, 'no start event'); }, 15000);
    ws.on('text', text => {
      let m;
      try { m = JSON.parse(text); } catch { return; }
      if (m.event === 'start' && !listener) {
        clearTimeout(timeout);
        streamId = (m.start && m.start.streamId) || m.streamId || null;
        const service = this.store.current;
        if (!service) {
          this.callLog.update(call.record, { endedAt: Date.now(), end: 'no service' });
          return ws.close(1000, 'no service');
        }
        const start = this.startPosition(call.key);
        listener = new Listener({
          store: this.store, sink, key: call.key, start,
          log: this.log, onEnd: () => this.listeners.delete(listener)
        });
        this.listeners.add(listener);
        listener.record = call.record;
        const kind = start === 0 ? 'beginning' : (!service.endedAt && start >= service.bytes - 2 * BYTES_PER_SECOND ? 'live' : 'resumed');
        this.callLog.update(call.record, { connectedAt: Date.now(), start: kind, startSeconds: Math.round(start / BYTES_PER_SECOND) });
        this.log(`stream started at ${(listener.cursor / BYTES_PER_SECOND).toFixed(0)} s (${this.listeners.size} listening)`);
      } else if (m.event === 'dtmf' && listener) {
        const digit = m.dtmf && m.dtmf.digit;
        if (digit === '1') {
          listener.goLive();
          this.callLog.update(call.record, { liveAt: Date.now(), livePresses: call.record.livePresses + 1 });
          this.log('caller pressed 1: live');
        }
      } else if (m.event === 'playedStream' && listener) {
        listener.played(m.name);
      }
    });
    ws.on('close', () => {
      clearTimeout(timeout);
      if (listener) {
        listener.stop();
        this.log(`stream closed (${this.listeners.size} listening)`);
      }
      if (!call.record.endedAt) {
        const end = listener ? (listener.endReason || 'hung up') : 'no audio stream';
        this.callLog.update(call.record, { endedAt: Date.now(), end });
      }
    });
  }

  count() { return this.listeners.size; }

  // Where each caller on the line is in the recording, in seconds.
  positions() {
    return [...this.listeners].map(l => ({ id: l.record.id, heard: Math.round(l.heard() / BYTES_PER_SECOND) }));
  }
}

module.exports = { Plivo, xmlEscape };
