'use strict';
// Recordings and where each caller is. One service at a time: starting a new
// one deletes the previous recording, so the disk holds at most one service
// (about 29 MB an hour in phone format). Caller numbers are never stored;
// positions are kept under a keyed hash of the number.

const fs = require('node:fs');
const path = require('node:path');
const crypto = require('node:crypto');
const { EventEmitter } = require('node:events');

const BYTES_PER_SECOND = 8000;

function atomicJson(file, value) {
  const tmp = `${file}.${process.pid}.tmp`;
  fs.writeFileSync(tmp, JSON.stringify(value, null, 2));
  fs.renameSync(tmp, file);
}

function serviceId(now = new Date()) {
  const p = n => String(n).padStart(2, '0');
  return `${now.getUTCFullYear()}${p(now.getUTCMonth() + 1)}${p(now.getUTCDate())}-` +
    `${p(now.getUTCHours())}${p(now.getUTCMinutes())}${p(now.getUTCSeconds())}-` +
    crypto.randomBytes(3).toString('hex');
}

class Store extends EventEmitter {
  constructor({ dataDir, log = () => {} }) {
    super();
    this.dir = path.join(dataDir, 'services');
    this.log = log;
    this.current = null;
    fs.mkdirSync(this.dir, { recursive: true, mode: 0o750 });
    this.load();
    this.timer = setInterval(() => this.flush(), 5000);
    this.timer.unref();
  }

  load() {
    const ids = fs.readdirSync(this.dir, { withFileTypes: true })
      .filter(e => e.isDirectory()).map(e => e.name).sort();
    for (const id of ids) {
      const dir = path.join(this.dir, id);
      try {
        const meta = JSON.parse(fs.readFileSync(path.join(dir, 'meta.json'), 'utf8'));
        const audio = path.join(dir, 'audio.ulaw');
        const listenersFile = path.join(dir, 'listeners.json');
        const listeners = fs.existsSync(listenersFile)
          ? new Map(Object.entries(JSON.parse(fs.readFileSync(listenersFile, 'utf8')))) : new Map();
        if (this.current) this.closeFd(this.current);
        this.current = {
          id, dir, fd: fs.openSync(audio, 'a+'), bytes: fs.statSync(audio).size,
          startedAt: meta.startedAt, endedAt: meta.endedAt || null, stopReason: meta.stopReason || null,
          // Two 16 kHz input samples make one stored byte; the file is more
          // current than meta.json, which is saved every few seconds.
          inputSamples: fs.statSync(audio).size * 2, listeners, dirty: false
        };
      } catch (error) {
        this.log(`skipped damaged service ${id}: ${error.message}`);
      }
    }
    if (this.current) {
      // Only the newest recording is kept.
      for (const id of ids) if (id !== this.current.id) this.remove(id);
      this.log(`recording ${this.current.id}: ${this.seconds().toFixed(0)} s, ${this.current.endedAt ? 'ended' : 'open'}`);
    }
  }

  closeFd(service) { try { fs.closeSync(service.fd); } catch { /* already closed */ } }

  remove(id) {
    fs.rmSync(path.join(this.dir, id), { recursive: true, force: true });
    this.log(`deleted recording ${id}`);
  }

  info(service = this.current) {
    if (!service) return null;
    return {
      id: service.id, startedAt: service.startedAt, endedAt: service.endedAt,
      stopReason: service.stopReason, seconds: Math.floor(service.bytes / BYTES_PER_SECOND),
      inputSamples: service.inputSamples
    };
  }

  seconds() { return this.current ? this.current.bytes / BYTES_PER_SECOND : 0; }
  live() { return !!this.current && !this.current.endedAt; }

  startService() {
    const previous = this.current;
    if (previous && !previous.endedAt) this.endService('a new service started');
    const id = serviceId();
    const dir = path.join(this.dir, id);
    fs.mkdirSync(dir, { mode: 0o750 });
    const fd = fs.openSync(path.join(dir, 'audio.ulaw'), 'a+');
    this.current = {
      id, dir, fd, bytes: 0, startedAt: new Date().toISOString(), endedAt: null, stopReason: null,
      inputSamples: 0, listeners: new Map(), dirty: true
    };
    this.flush();
    if (previous) {
      this.closeFd(previous);
      this.remove(previous.id);
    }
    this.log(`service ${id} started`);
    this.emit('service', this.info());
    return this.current;
  }

  endService(reason) {
    const service = this.current;
    if (!service || service.endedAt) return;
    service.endedAt = new Date().toISOString();
    service.stopReason = reason;
    service.dirty = true;
    this.flush();
    this.log(`service ${service.id} ended (${reason}) after ${this.seconds().toFixed(0)} s`);
    this.emit('service', this.info());
    this.emit('data');
  }

  append(ulaw, inputSamples) {
    const service = this.current;
    if (!service || service.endedAt || !ulaw.length) return;
    fs.writeSync(service.fd, ulaw, 0, ulaw.length, service.bytes);
    service.bytes += ulaw.length;
    service.inputSamples = inputSamples;
    service.dirty = true;
    this.emit('data');
  }

  read(offset, length) {
    const service = this.current;
    if (!service || offset >= service.bytes || length <= 0) return Buffer.alloc(0);
    const n = Math.min(length, service.bytes - offset);
    const out = Buffer.allocUnsafe(n);
    const got = fs.readSync(service.fd, out, 0, n, offset);
    return out.subarray(0, got);
  }

  position(key) {
    const entry = this.current && this.current.listeners.get(key);
    return entry ? entry.position : null;
  }

  savePosition(key, position) {
    const service = this.current;
    if (!service || !key) return;
    service.listeners.set(key, { position, updatedAt: new Date().toISOString() });
    service.dirty = true;
  }

  flush() {
    const service = this.current;
    if (!service || !service.dirty) return;
    service.dirty = false;
    try {
      atomicJson(path.join(service.dir, 'meta.json'), {
        id: service.id, startedAt: service.startedAt, endedAt: service.endedAt,
        stopReason: service.stopReason, inputSamples: service.inputSamples, bytes: service.bytes
      });
      atomicJson(path.join(service.dir, 'listeners.json'), Object.fromEntries(service.listeners));
    } catch (error) {
      service.dirty = true;
      this.log(`could not save ${service.id}: ${error.message}`);
    }
  }

  close() {
    clearInterval(this.timer);
    this.flush();
    if (this.current) this.closeFd(this.current);
  }
}

module.exports = { Store, BYTES_PER_SECOND };
