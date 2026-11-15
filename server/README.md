# Phone stream server

Runs on a small Linux VM behind a web server that provides TLS (the examples
use Apache and `phone.example.org`). The church app pushes the service audio
to it; callers dial a Plivo number and hear the service, each at their own
position:

- a new caller starts at the beginning (or at live: `NEW_CALLER_START=live`);
- a caller who hangs up and calls again carries on a few seconds before where
  they stopped (positions are kept per service, under an anonymous key made
  from the caller's number);
- pressing **1** goes to live;
- after the service, callers hear the recording; at its end they hear a
  goodbye and the call hangs up;
- starting the next service deletes the previous recording.

The church app shows every call of the current service: the caller's number,
when they called and hung up, where they started, when they pressed 1 and how
far behind live they are; browser test-player listeners too, and the IP
address each request came from (for a real call that is Plivo's server; a
request without Plivo's signature header is listed as a test). The call list
is kept in the server's memory only: never written to disk, cleared when the
next service starts, and forgotten when the server restarts. Several copies of
the app may be connected to watch; the one that presses Start streams.

No npm packages: Node.js 20 or later only.

## Addresses

| Address | What |
| --- | --- |
| `https://phone.example.org/` | Status page: server up, service live, app connected, callers |
| `https://phone.example.org/health` | The same as JSON |
| `https://phone.example.org/plivo/answer/<PLIVO_ANSWER_KEY>` | Plivo's Answer URL (POST) |
| `wss://phone.example.org/plivo/stream/<one-time token>` | Plivo's audio stream for one call (handed out by the answer) |
| `wss://phone.example.org/ingest` | The church app's uplink (its key is `INGEST_TOKEN`) |
| `https://phone.example.org/listen/<LISTEN_KEY>` | Test player for a browser or phone, from about live (only while `LISTEN_KEY` is set); the app shows it with a QR code |
| `https://phone.example.org/listen/<LISTEN_KEY>/audio.wav` | The player's audio: an endless 8 kHz WAV |

The server listens on `127.0.0.1:8090` only; the web server passes the name
to it, WebSockets included. With Apache 2.4.47 or later (`a2enmod proxy
proxy_http ssl`), inside the `phone.example.org` HTTPS virtual host (for
example one made by certbot):

```apache
ProxyPreserveHost On
ProxyPass        / http://127.0.0.1:8090/ upgrade=websocket
ProxyPassReverse / http://127.0.0.1:8090/
```

The server trusts the last `X-Forwarded-For` entry as the client's address,
which is what Apache adds; don't expose port 8090 directly.

## Install or update (on the VM, as root)

Without copying files: on the PC, run `make-vm-install.ps1` (in the folder
above this one); it writes `vm-install.sh`, the server packed into one block
of text with the app's key and the server's name (from `app/ingest-key.txt`
and `app/server-address.txt`). Paste all of it into the VM's root terminal.
`vm-install.sh` contains the key: keep it private.

With the folder on the VM, from inside it:

```bash
PUBLIC_HOST=phone.example.org INGEST_TOKEN=<the app's key> bash deploy/install.sh
```

It creates the `phonestream` user, puts the program in `/opt/phone-stream`,
the recordings in `/var/lib/phone-stream`, the settings with new random keys
in `/etc/phone-stream/phone-stream.env` (first run only; `INGEST_TOKEN` and
`PUBLIC_HOST` given on a later run replace the ones there), and starts the
`phone-stream` service, which starts again by itself after a crash or a
reboot. At the end it prints the status page, the Plivo Answer URL and the
browser test player.

Useful commands:

```bash
systemctl status phone-stream
journalctl -u phone-stream -f        # the log, live
systemctl restart phone-stream       # after changing the settings
```

## Settings (`/etc/phone-stream/phone-stream.env`)

| Setting | Default | Meaning |
| --- | --- | --- |
| `INGEST_TOKEN`, `PLIVO_ANSWER_KEY`, `CALLER_SECRET` | (random) | Keys; at least 16 characters |
| `LISTEN_KEY` | (random) | Turns on the browser test player; delete the line to turn it off |
| `NEW_CALLER_START` | `beginning` | `beginning` or `live` |
| `REWIND_SECONDS` | `3` | How far before their last position a returning caller resumes |
| `MAX_CALLERS` | `100` | Callers at once; more hear "the line is full" |
| `AUTO_END_MINUTES` | `180` | A service with no audio and no app this long is ended |
| `PUBLIC_HOST` / `PUBLIC_URL` | (from the install) / `https://<PUBLIC_HOST>` | The addresses handed to Plivo and the app |
| `SPEAK_LANGUAGE`, `SPEAK_VOICE` | `en-US`, `WOMAN` | Plivo's voice for the spoken messages |
| `TEXT_WELCOME_BEGINNING`, `TEXT_WELCOME_LIVE`, `TEXT_WELCOME_BACK_LIVE`, `TEXT_WELCOME_RECORDING`, `TEXT_WELCOME_BACK_RECORDING`, `TEXT_NO_SERVICE`, `TEXT_ENDED`, `TEXT_BUSY` | English | The spoken messages |

## Tests (on any PC with Node.js)

```bash
node test/run.mjs
```

It starts the server on a random local port with a temporary data folder,
plays the church app (a tone that changes pitch every second) and Plivo
(calls, the stream, key presses, checkpoints), and checks what each caller
hears by measuring the pitch, and what the app is told about the calls.

One pretend call against a running server (the local one or the real one),
before any phone service is set up:

```bash
node test/call.mjs --answer https://phone.example.org/plivo/answer/<PLIVO_ANSWER_KEY> --from +15551230001 --seconds 20 --press1 10 --out call.wav
```

It prints the greeting, how much audio arrived and how loud, and saves what
the caller would hear. `--signed` lists it as a phone call instead of a test.

Many pretend calls at once against a running server, to see that each one
keeps real-time pace (a service must exist; the calls play it):

```bash
node test/load.mjs --answer https://phone.example.org/plivo/answer/<PLIVO_ANSWER_KEY> --calls 50 --seconds 60
```

The server's CPU, memory and delay with 10, 50 and 100 live callers, all on
this machine (the server pinned to one CPU core):

```bash
node test/bench.mjs [--calls 10,50,100] [--seconds 60]
```

## The app's protocol

WebSocket text messages are JSON; audio is binary: an 8-byte little-endian
sample index followed by 16 kHz mono 16-bit PCM. See the comment at the top
of `ingest.js`.
