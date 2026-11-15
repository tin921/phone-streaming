#!/bin/bash
# Installs or updates the phone stream server on Ubuntu. Run as root:
#   bash deploy/install.sh            (from the folder that holds main.js)
# The first run creates /etc/phone-stream/phone-stream.env with new random
# keys; later runs only update the program and keep the settings.
# PUBLIC_HOST=<name> in the environment is the server's public name (e.g.
# phone.example.org); it is needed on the first run and can be changed later.
# INGEST_TOKEN=<key> sets the church app's key (the one built into
# PhoneStream.exe, app/ingest-key.txt), on the first run or later.
#   PUBLIC_HOST=phone.example.org INGEST_TOKEN=<key> bash deploy/install.sh
set -euo pipefail
SRC="$(cd "$(dirname "$0")/.." && pwd)"
APP=/opt/phone-stream
DATA=/var/lib/phone-stream
ETC=/etc/phone-stream
ENVFILE="$ETC/phone-stream.env"
PUBLIC_HOST="${PUBLIC_HOST:-}"

command -v node >/dev/null || { echo "Node.js is not installed (see README)."; exit 1; }
if [ -n "${INGEST_TOKEN:-}" ] && ! [[ "$INGEST_TOKEN" =~ ^[A-Za-z0-9_-]{16,}$ ]]; then
  echo "INGEST_TOKEN must be at least 16 letters, digits, - or _."; exit 1
fi
if [ -n "$PUBLIC_HOST" ] && ! [[ "$PUBLIC_HOST" =~ ^[A-Za-z0-9.-]+(:[0-9]+)?$ ]]; then
  echo "PUBLIC_HOST must be a host name such as phone.example.org."; exit 1
fi
if [ ! -f "$ENVFILE" ] && [ -z "$PUBLIC_HOST" ]; then
  echo "First install: give the server's public name, e.g. PUBLIC_HOST=phone.example.org bash deploy/install.sh"; exit 1
fi
[ -f "$SRC/main.js" ] || { echo "main.js not found next to deploy/: run this from the server folder."; exit 1; }

id phonestream >/dev/null 2>&1 || useradd --system --home-dir "$DATA" --shell /usr/sbin/nologin phonestream
install -d -m 755 "$APP"
install -d -m 750 -o phonestream -g phonestream "$DATA"
install -d -m 750 -g phonestream "$ETC"
install -m 644 "$SRC"/*.js "$APP"/

if [ ! -f "$ENVFILE" ]; then
  umask 027
  cat > "$ENVFILE" <<EOF
# Phone stream server settings (read by systemd; restart after a change:
#   systemctl restart phone-stream)
HOST=127.0.0.1
PORT=8090
PUBLIC_HOST=$PUBLIC_HOST
# The church app's key (Settings in the app).
INGEST_TOKEN=${INGEST_TOKEN:-$(openssl rand -hex 24)}
# Secret part of the Plivo Answer URL.
PLIVO_ANSWER_KEY=$(openssl rand -hex 16)
# Turns caller numbers into anonymous keys; changing it forgets all positions.
CALLER_SECRET=$(openssl rand -hex 32)
# Browser test player: https://$PUBLIC_HOST/listen/<LISTEN_KEY>. Remove the line to turn it off.
LISTEN_KEY=$(openssl rand -hex 16)
# New callers start at the beginning of the service, or at live:
NEW_CALLER_START=beginning
# Optional: SPEAK_LANGUAGE=en-US  SPEAK_VOICE=WOMAN  MAX_CALLERS=100  REWIND_SECONDS=3
# Optional spoken texts: TEXT_WELCOME_BEGINNING=...  TEXT_NO_SERVICE=...  TEXT_ENDED=...  (see README)
EOF
  chgrp phonestream "$ENVFILE"
  chmod 640 "$ENVFILE"
  echo "Created $ENVFILE with new keys."
else
  if [ -n "${INGEST_TOKEN:-}" ]; then sed -i "s/^INGEST_TOKEN=.*/INGEST_TOKEN=$INGEST_TOKEN/" "$ENVFILE"; fi
  if [ -n "$PUBLIC_HOST" ]; then sed -i "s/^PUBLIC_HOST=.*/PUBLIC_HOST=$PUBLIC_HOST/" "$ENVFILE"; fi
fi

install -m 644 "$SRC/deploy/phone-stream.service" /etc/systemd/system/phone-stream.service
systemctl daemon-reload
systemctl enable --quiet phone-stream
systemctl restart phone-stream
sleep 2
systemctl --no-pager --lines=8 status phone-stream || true

get() { grep -E "^$1=" "$ENVFILE" | head -1 | cut -d= -f2-; }
host="$(get PUBLIC_HOST)"
echo
echo "Status page:          https://$host/"
echo "Plivo Answer URL:     https://$host/plivo/answer/$(get PLIVO_ANSWER_KEY)   (method POST)"
if [ -n "$(get LISTEN_KEY)" ]; then echo "Browser test player:  https://$host/listen/$(get LISTEN_KEY)"; fi
echo "Church app server:    wss://$host/ingest"
tok="$(get INGEST_TOKEN)"
echo "Church app key:       ${tok:0:6}… (${#tok} characters)"
