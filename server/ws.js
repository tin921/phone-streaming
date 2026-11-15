'use strict';
// A small WebSocket server (RFC 6455) for the phone server: no extensions, no
// subprotocols. Clients must mask their frames, as the RFC requires; the
// server sends unmasked frames. Enough for Plivo/Telnyx audio streams and the
// church app's uplink, without an npm dependency on the VM.

const crypto = require('node:crypto');
const { EventEmitter } = require('node:events');

const GUID = '258EAFA5-E914-47DA-95CA-C5AB0DC85B11';
const OP = { CONT: 0, TEXT: 1, BINARY: 2, CLOSE: 8, PING: 9, PONG: 10 };

class WebSocketConnection extends EventEmitter {
  constructor(socket, head, options = {}) {
    super();
    this.socket = socket;
    this.maxMessage = options.maxMessage || 1 << 20;
    this.closed = false;
    this.buffer = head && head.length ? Buffer.from(head) : Buffer.alloc(0);
    this.fragments = null;
    this.fragmentOpcode = 0;
    this.fragmentSize = 0;
    this.lastHeard = Date.now();
    socket.setNoDelay(true);
    socket.on('data', chunk => this.onData(chunk));
    socket.on('error', () => this.finish(1006, 'socket error'));
    socket.on('close', () => this.finish(1006, 'socket closed'));
    if (this.buffer.length) setImmediate(() => this.parse());
  }

  get bufferedAmount() { return this.socket.writableLength; }

  onData(chunk) {
    this.lastHeard = Date.now();
    this.buffer = this.buffer.length ? Buffer.concat([this.buffer, chunk]) : chunk;
    this.parse();
  }

  parse() {
    while (!this.closed) {
      const b = this.buffer;
      if (b.length < 2) return;
      const fin = (b[0] & 0x80) !== 0;
      const opcode = b[0] & 0x0f;
      const masked = (b[1] & 0x80) !== 0;
      let length = b[1] & 0x7f;
      let at = 2;
      if (length === 126) {
        if (b.length < 4) return;
        length = b.readUInt16BE(2);
        at = 4;
      } else if (length === 127) {
        if (b.length < 10) return;
        const high = b.readUInt32BE(2);
        if (high !== 0) return this.fail(1009, 'frame too large');
        length = b.readUInt32BE(6);
        at = 10;
      }
      if (!masked) return this.fail(1002, 'client frames must be masked');
      if (length > this.maxMessage) return this.fail(1009, 'frame too large');
      if (b.length < at + 4 + length) return;
      const mask = b.subarray(at, at + 4);
      const payload = Buffer.allocUnsafe(length);
      for (let i = 0; i < length; i++) payload[i] = b[at + 4 + i] ^ mask[i & 3];
      this.buffer = b.subarray(at + 4 + length);
      this.frame(fin, opcode, payload);
    }
  }

  frame(fin, opcode, payload) {
    if (opcode >= 8) {
      if (!fin || payload.length > 125) return this.fail(1002, 'bad control frame');
      if (opcode === OP.PING) return this.sendFrame(OP.PONG, payload);
      if (opcode === OP.PONG) return this.emit('pong', payload);
      if (opcode === OP.CLOSE) {
        const code = payload.length >= 2 ? payload.readUInt16BE(0) : 1005;
        const reason = payload.length > 2 ? payload.subarray(2).toString('utf8') : '';
        if (!this.closing) this.sendFrame(OP.CLOSE, payload.subarray(0, 2));
        return this.finish(code, reason);
      }
      return this.fail(1002, 'unknown control frame');
    }
    if (opcode === OP.CONT) {
      if (!this.fragments) return this.fail(1002, 'unexpected continuation');
      this.fragments.push(payload);
      this.fragmentSize += payload.length;
      if (this.fragmentSize > this.maxMessage) return this.fail(1009, 'message too large');
      if (!fin) return;
      const whole = Buffer.concat(this.fragments);
      const op = this.fragmentOpcode;
      this.fragments = null;
      return this.deliver(op, whole);
    }
    if (opcode !== OP.TEXT && opcode !== OP.BINARY) return this.fail(1002, 'unknown opcode');
    if (this.fragments) return this.fail(1002, 'interleaved message');
    if (!fin) {
      this.fragments = [payload];
      this.fragmentOpcode = opcode;
      this.fragmentSize = payload.length;
      return;
    }
    this.deliver(opcode, payload);
  }

  deliver(opcode, payload) {
    if (opcode === OP.TEXT) this.emit('text', payload.toString('utf8'));
    else this.emit('binary', payload);
  }

  sendFrame(opcode, payload) {
    if (this.closed || this.socket.destroyed) return false;
    const length = payload.length;
    let header;
    if (length < 126) {
      header = Buffer.from([0x80 | opcode, length]);
    } else if (length < 65536) {
      header = Buffer.from([0x80 | opcode, 126, length >> 8, length & 0xff]);
    } else {
      header = Buffer.alloc(10);
      header[0] = 0x80 | opcode;
      header[1] = 127;
      header.writeUInt32BE(0, 2);
      header.writeUInt32BE(length, 6);
    }
    this.socket.write(header);
    return this.socket.write(payload);
  }

  sendText(text) { return this.sendFrame(OP.TEXT, Buffer.from(text, 'utf8')); }
  sendJson(value) { return this.sendText(JSON.stringify(value)); }
  sendBinary(data) { return this.sendFrame(OP.BINARY, data); }
  ping() { return this.sendFrame(OP.PING, Buffer.alloc(0)); }

  close(code = 1000, reason = '') {
    if (this.closed || this.closing) return;
    this.closing = true;
    const body = Buffer.alloc(2 + Buffer.byteLength(reason));
    body.writeUInt16BE(code, 0);
    body.write(reason, 2);
    this.sendFrame(OP.CLOSE, body.subarray(0, Math.min(body.length, 125)));
    // Give the peer a moment to answer the close, then drop the socket.
    setTimeout(() => this.finish(code, reason), 1000).unref();
  }

  fail(code, reason) {
    this.close(code, reason);
    this.finish(code, reason);
  }

  finish(code, reason) {
    if (this.closed) return;
    this.closed = true;
    this.socket.end();
    setTimeout(() => this.socket.destroy(), 2000).unref();
    this.emit('close', code, reason);
  }
}

// Completes the HTTP upgrade. Returns the connection, or null after answering
// the request with an error.
function accept(req, socket, head, options) {
  const key = req.headers['sec-websocket-key'];
  const upgrade = String(req.headers.upgrade || '').toLowerCase();
  if (upgrade !== 'websocket' || !key || req.headers['sec-websocket-version'] !== '13') {
    socket.end('HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n');
    return null;
  }
  const acceptKey = crypto.createHash('sha1').update(key + GUID).digest('base64');
  socket.write('HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n' +
    `Sec-WebSocket-Accept: ${acceptKey}\r\n\r\n`);
  return new WebSocketConnection(socket, head, options);
}

function reject(socket, status, text) {
  socket.end(`HTTP/1.1 ${status} ${text}\r\nConnection: close\r\nContent-Length: 0\r\n\r\n`);
}

module.exports = { accept, reject, WebSocketConnection };
