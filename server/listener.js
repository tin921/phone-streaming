'use strict';
// One caller listening to the recording at their own position. The phone
// service plays whatever it is sent, in order, so audio is paced to real time
// with about a second of lead: the position stays close to what the caller has
// heard, and "go to live" can clear what is queued. Provider adapters (Plivo,
// later Telnyx) supply the sink: sendAudio(ulaw), checkpoint(name), clear(), end().

const { BYTES_PER_SECOND } = require('./store');

const FRAME = 160;               // 20 ms of 8 kHz mu-law
const LEAD = BYTES_PER_SECOND;   // queued ahead of the caller's ear: 1 s
const CHUNK = 1600;              // largest single send: 200 ms
const CHECKPOINT_EVERY = BYTES_PER_SECOND;

class Listener {
  constructor({ store, sink, key, start, log, onEnd }) {
    this.store = store;
    this.sink = sink;
    this.key = key;
    this.log = log;
    this.onEnd = onEnd;
    this.serviceId = store.current.id;
    this.cursor = Math.max(0, Math.min(start, store.current.bytes));
    this.startedAt = this.cursor;
    this.confirmed = null;       // last position the phone service said it played
    this.credit = LEAD;
    this.lastTick = Date.now();
    this.nextCheckpoint = this.cursor + CHECKPOINT_EVERY;
    this.finishing = null;
    this.closed = false;
    this.timer = setInterval(() => this.pump(), 100);
  }

  // What the caller has heard, as well as can be told.
  heard() {
    if (this.confirmed !== null) return this.confirmed;
    return Math.max(this.startedAt, this.cursor - Math.max(0, LEAD - this.credit));
  }

  pump() {
    if (this.closed) return;
    const service = this.store.current;
    if (!service || service.id !== this.serviceId) return this.finish('the recording was replaced');
    const now = Date.now();
    this.credit = Math.min(LEAD, this.credit + (now - this.lastTick) * BYTES_PER_SECOND / 1000);
    this.lastTick = now;
    for (;;) {
      const available = service.bytes - this.cursor;
      const n = Math.floor(Math.min(this.credit, available, CHUNK) / FRAME) * FRAME;
      if (n <= 0) break;
      this.sink.sendAudio(this.store.read(this.cursor, n));
      this.cursor += n;
      this.credit -= n;
      if (this.cursor >= this.nextCheckpoint) {
        this.sink.checkpoint(`p${this.cursor}`);
        this.nextCheckpoint = this.cursor + CHECKPOINT_EVERY;
      }
    }
    if (service.endedAt && this.cursor >= service.bytes - FRAME) {
      if (!this.finishing) {
        // Everything is sent: wait for it to be played, then let the call go on.
        this.sink.checkpoint(`end${this.cursor}`);
        this.finishing = now + (LEAD / BYTES_PER_SECOND) * 1000 + 3000;
      } else if (now > this.finishing) {
        this.finish('the end of the recording');
      }
    }
  }

  played(name) {
    const match = /^(?:p|end)(\d+)$/.exec(name || '');
    if (!match) return;
    this.confirmed = Number(match[1]);
    if (name.startsWith('end')) this.finish('the end of the recording');
  }

  goLive() {
    const service = this.store.current;
    if (!service) return;
    this.sink.clear();
    this.cursor = Math.max(0, service.bytes - BYTES_PER_SECOND);
    this.cursor -= this.cursor % FRAME;
    this.confirmed = this.cursor;
    this.startedAt = this.cursor;
    this.credit = LEAD;
    this.nextCheckpoint = this.cursor + CHECKPOINT_EVERY;
    this.finishing = null;
  }

  finish(reason) {
    if (this.closed) return;
    this.endReason = reason;
    this.stop();
    this.log(`call ended: ${reason}`);
    this.sink.end();
  }

  // The call or stream is gone: remember where this caller was.
  stop() {
    if (this.closed) return;
    this.closed = true;
    clearInterval(this.timer);
    if (this.key && this.store.current && this.store.current.id === this.serviceId) {
      this.store.savePosition(this.key, this.heard());
    }
    if (this.onEnd) this.onEnd();
  }
}

module.exports = { Listener, FRAME, LEAD };
