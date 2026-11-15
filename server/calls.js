'use strict';
// The calls of the current service, for the church app's call list and
// timeline: the caller's number as the phone service gives it, when the call
// came in, connected and ended, where it started and when the caller pressed 1.
// Kept in memory only (never written to disk; a server restart forgets it)
// and cleared when a new service starts.
// Events: 'call' (a call was added or changed), 'reset' (a new service).

const { EventEmitter } = require('node:events');

const MAX_CALLS = 5000;

class CallLog extends EventEmitter {
  constructor({ store }) {
    super();
    this.serviceId = store.current ? store.current.id : null;
    this.calls = [];
    this.nextId = 1;
    store.on('service', info => {
      if (info && info.id !== this.serviceId) {
        this.serviceId = info.id;
        this.calls = [];
        this.emit('reset');
      }
    });
  }

  // fields: from, name, ip, via ('phone', 'test' or 'browser'), and for a
  // call that was not played: endedAt, end.
  add(fields) {
    const call = {
      id: this.nextId++, at: Date.now(), from: '', name: '', ip: '', via: 'phone',
      connectedAt: null, endedAt: null, end: null,
      start: null, startSeconds: null, liveAt: null, livePresses: 0,
      ...fields
    };
    this.calls.push(call);
    if (this.calls.length > MAX_CALLS) this.calls.shift();
    this.emit('call', call);
    return call;
  }

  update(call, changes) {
    Object.assign(call, changes);
    this.emit('call', call);
  }

  list() { return this.calls; }
}

module.exports = { CallLog };
