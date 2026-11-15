# Church Phone Stream

Church Phone Stream lets people listen to a church service by calling an
ordinary phone number. A small Windows app sends whatever the church PC is
playing to a server you run, and the server plays it to every caller through
[Plivo](https://www.plivo.com/)'s phone service. It is meant for members who
have no internet, smartphone or app (often older members on a landline), so
they can follow the service live or catch up on it afterwards, on
infrastructure the church controls.

- **Any phone, nothing to install for callers.** They dial a normal number
  from a landline or a mobile phone.
- **Each caller listens their own way.** A new caller starts at the
  beginning, a caller who hangs up and calls back carries on where they left
  off, and pressing **1** jumps to live. After the service the recording plays.
- **One small Windows app at the church.** A single `.exe` with no installer.
  It captures what the PC plays without touching OBS or other audio software,
  and sends it over one outgoing secure connection (port 443). It holds up to
  10 minutes of audio if the internet drops and catches up when it is back.
- **See who is listening.** The app shows each call (number, when they called
  and hung up, where they started, how far behind live they are) and a
  timeline of the audio level, the listeners and the calls.
- **A light server.** Node.js with no npm packages, running as a systemd
  service. A one-CPU virtual machine handles 100 callers (numbers below).
- **Test without a phone.** A browser test player with a QR code, and
  scripts that act as phone calls, from one call to a 100-caller load test.

![The Church Phone Stream app during a service: the stream is live, a level meter, connection and status lines, a timeline graph of audio level, listeners and calls, and the list of calls](docs/screenshot.png)

*A demo service on a local test server. The callers are made up (555-01xx
numbers and documentation IP addresses), and the app was capturing a second
sound output that played an organ hymn and a talk read by Windows' built-in
voice.*

## How it works

```text
PhoneStream.exe on the church PC       captures what the PC plays, 16 kHz
        |  one WebSocket over TLS (port 443)
        v
Apache (TLS) -> Node.js server         records the service, keeps each caller's place
        |  one WebSocket per call, 8 kHz mu-law
        v
Plivo -> phone network -> callers' phones
```

1. The app captures the PC's sound output (WASAPI loopback, shared mode) and
   streams it to the server as 16 kHz audio over a WebSocket.
2. The server records the service and keeps each caller's place in it.
3. When someone calls, Plivo asks the server what to do. The server answers
   with a greeting and an audio stream, and sends that caller's audio in
   phone format (8 kHz mu-law, the same as any phone call). Pressing 1 reaches
   the server as a key press.

## Performance

### Server: 10, 50 and 100 callers

`server/test/bench.mjs` runs the server on one CPU core, streams audio like
the app does, and connects pretend phone calls that listen live for 60
seconds. Measured on one core of an Intel Core i9-10900X (3.7 GHz),
Windows 11, Node.js 24:

| Callers | Every caller kept real-time pace | Server CPU (one core) | Server memory | Sent to callers | Delay: median / 95% / slowest |
| ---: | :---: | ---: | ---: | ---: | ---: |
| 10 | yes | 4.8% | 61 MB | 0.9 Mbps | 118 / 136 / 143 ms |
| 50 | yes | 8.3% | 64 MB | 4.6 Mbps | 120 / 138 / 159 ms |
| 100 | yes | 11.6% | 70 MB | 9.3 Mbps | 118 / 136 / 152 ms |

- **Server CPU** is the share of one core the Node.js server used. About 4%
  is the base (taking in the app's audio and keeping the app informed); each
  caller adds less than 0.1%.
- **Sent to callers** is the audio and control messages to Plivo. Each caller
  takes about 90 kbps (64 kbps of phone audio, sent as text).
- **Delay** is from the app sending a piece of audio to a caller's stream
  receiving it. On top of that, callers are kept 1 second behind on purpose
  (see Latency).
- Every caller received every second of audio, with no pauses.

### On a real one-CPU VM

The server in use runs on a virtual machine with 1 vCPU and 848 MiB of
memory (Ubuntu 24.04), behind Apache for TLS. `server/test/load.mjs` made
pretend calls over the internet from a PC to it:

| Callers | Every caller got all its audio, with no pauses | Slowest first audio |
| ---: | :---: | ---: |
| 10, all at once | yes | 117 ms |
| 50, all at once | yes | 126 ms |
| 100, joining over 20 seconds | yes | 358 ms |

When 100 calls arrived within the same second, 10 of the audio streams failed
to open and the other 90 played normally. Real callers arrive over minutes.

In an earlier run of 60-second calls, `btop` on the VM showed the following.
The calls played a recording and no app was streaming, so the server's CPU
is lower than in the table above, which includes taking in live audio.

| Callers | Whole VM CPU | Node.js server CPU | Server memory | Upload |
| ---: | ---: | ---: | ---: | ---: |
| 25 | 5% | 2% | 59 MB | 2.4 Mbps |
| 50 | 9 to 11% | 3 to 5.5% | 60 MB | 3.8 to 4.9 Mbps |

Most of the VM's CPU beyond the server's own is Apache (TLS) and the system.
Each caller adds about 100 kbps of upload from the server, TLS included:
about 90 MB per caller for a two-hour service, or 9 GB for 100 callers.

### Latency

From the sound in the church to a caller who pressed 1:

| Step | Delay |
| --- | --- |
| The app captures and sends (10 ms capture blocks, sent every 40 ms) | up to about 50 ms |
| Internet, church PC to server (round trip measured from the PC used here: 14 ms) | about 7 ms |
| The server passes new audio to the caller's stream (median, from the table above) | about 120 ms |
| Cushion kept at Plivo, so short network hiccups don't cut the sound | 1 s |
| Plivo and the phone network | not measured yet |

So a live caller hears the service about 1.2 seconds after the room, plus
whatever the phone network adds. Callers who did not press 1 are further
behind by design: they started at the beginning.

### The app

While streaming and showing a service with 7 callers, the app used about 4%
of one CPU core (0.2% of the 20-thread PC above) and 22 MB of memory, and sent
about 260 kbps (16 kHz, 16-bit mono). It runs on Windows 10 and 11.

## Getting started

You need a Linux server reachable on the internet with a domain name and
TLS, a Plivo account, and Visual Studio 2022 Build Tools on a Windows PC to
build the app.

1. **Build the app** on a Windows PC:
   `powershell -ExecutionPolicy Bypass -File app\build.ps1 -Server wss://phone.example.org/ingest`.
   The first build creates the key that the app and the server share
   (`app\ingest-key.txt`). See [app/README.md](app/README.md).
2. **Install the server**: run `make-vm-install.ps1`, then paste the
   `vm-install.sh` it writes into the server's root terminal. Or copy the
   `server` folder over and run `deploy/install.sh`. The install prints the
   Plivo Answer URL. Set up the web server as a TLS proxy in front of it
   (an Apache example is in [server/README.md](server/README.md)).
3. **Set up Plivo**: buy a phone number, create an XML application whose
   Answer URL (method POST) is the one the install printed, and attach the
   number to it.
4. **At the church**: copy `PhoneStream.exe` to the PC, choose the sound
   source, and press **Start streaming**. Call the number to check.

Tests: `node server/test/run.mjs` checks the server end to end with
pretend calls. `server/test/call.mjs` and `server/test/load.mjs` make
pretend calls to a running server.

## Security and privacy

- The key that lets an app stream to your server is built into your
  `PhoneStream.exe` and is in `vm-install.sh`. Keep both private. The
  repository's `.gitignore` keeps keys and builds out of git.
- The Plivo Answer URL and the test player link contain random secret keys.
  Anyone with the test player link can listen; delete `LISTEN_KEY` on the
  server to turn it off.
- The server keeps the call list (numbers and times) in memory only. It is
  cleared when the next service starts and never written to disk. Where a
  caller stopped is saved under an anonymous key made from their number.
- The app shows callers' numbers and writes them to its log on the church
  PC, kept 60 days.

## Status

The server, the app and the whole path to a caller's stream have been
tested with pretend calls: an end-to-end test, load tests up to 100 callers,
and the demo above. Real phone calls through Plivo have not been made yet.
The server only checks that Plivo's signature header is present (to list a
call as a phone call), not that it is valid; the Answer URL's secret key is
what keeps others out. Telnyx is not supported yet.

## Repository

- `app/`: the Windows app (C++17, Win32, no libraries beyond Windows)
- `server/`: the Node.js server, its install scripts and tests
- `make-vm-install.ps1`: packs the server into one block of text to paste
  into the VM
- `docs/`: the screenshot

## License

[MIT](LICENSE)
