# Church Phone Stream (Windows app)

Sends what the PC plays (desktop audio) to the phone server, and shows who is
listening. One `PhoneStream.exe`, nothing to install: it uses only what comes
with Windows 10/11 (WASAPI for the audio, WinHTTP for the connection). It does
not touch OBS or other audio software: it listens to Windows' output in shared
mode and opens one outgoing connection (TLS on port 443), so nothing needs
opening in a firewall.

The server address and the key are built in when you build it, so a copy works
on any PC. **The key is inside the .exe: don't publish your build.**

## Using it

- Open it: it connects and shows the server, the service and its callers.
- Choose the source: desktop audio (Windows' default speakers), a particular
  output device, or a test tone. The meter shows its level.
- **Start streaming** begins a new service (the server deletes the previous
  recording); **Stop streaming** ends it, and callers can still hear the
  recording.
- The test player link (and its QR code, for a phone) plays what callers hear.
- The graph shows the service: audio level, how many listen, and one bar per
  call (blue for a phone call, green after the caller pressed 1 for live,
  purple for the browser test player).
- The list shows each call: number, phone/test/web, IP address, when they
  called and hung up, where they started, how far behind live they are.
- The status lines show the connection (round trip, what is being sent, the
  level the server hears) and the app's own CPU and memory.
- The log is also written to `%APPDATA%\ChurchPhoneStream\logs` (one file a
  day, kept 60 days); **Open log folder** opens it.
- Several copies may be open (on different PCs) to watch; the one that
  presses Start streams. If another copy starts streaming, this one stops
  streaming and keeps watching.

Command line, for this run only (not saved): `--server wss://host/ingest`,
`--source default|tone|<speaker name>`, `--autostart`. `--key KEY` replaces the
built-in key and is kept (encrypted for this Windows user).

## Building

```powershell
powershell -ExecutionPolicy Bypass -File build.ps1 -Server wss://phone.example.org/ingest
```

Needs Visual Studio 2022 Build Tools (C++). Two files next to the script are
built in and are not part of the repository:

- `server-address.txt`: the server the app starts with (`-Server` writes it;
  later builds read it, so `build.ps1` alone is enough).
- `ingest-key.txt`: the key, created once at random. The server's
  `INGEST_TOKEN` must be the same; `make-vm-install.ps1` in the folder above
  takes care of that.

QR encoder check: `tests\qr_test.cpp` writes QR images that
`tests\qr_check.py` decodes with OpenCV (`py -m pip install opencv-python`).
