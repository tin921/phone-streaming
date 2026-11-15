# Packs the phone server into one block of text to paste into the VM's root
# terminal (no SSH or file copy needed). Pasted, it writes
# /root/phone-stream-install.sh and runs it: the files are unpacked into
# /root/phone-stream-src and deploy/install.sh installs or updates the server
# with the church app's key (app/ingest-key.txt) and the server's name (from
# app/server-address.txt), so the app and the server match. Run this again
# after changing the server, then paste again.
#   powershell -ExecutionPolicy Bypass -File make-vm-install.ps1
# The output (vm-install.sh) contains the key: keep it private.
param([string]$Out = (Join-Path $PSScriptRoot 'vm-install.sh'))
$ErrorActionPreference = 'Stop'
$server = Join-Path $PSScriptRoot 'server'
$key = (Get-Content (Join-Path $PSScriptRoot 'app\ingest-key.txt') -Raw).Trim()
if ($key -notmatch '^[A-Za-z0-9_-]{16,}$') { throw 'app\ingest-key.txt must hold the app key (build the app once first)' }
$address = (Get-Content (Join-Path $PSScriptRoot 'app\server-address.txt') -Raw).Trim()
if ($address -notmatch '^wss://([A-Za-z0-9.-]+(:\d+)?)/') { throw 'app\server-address.txt must hold the app''s server address, e.g. wss://phone.example.org/ingest' }
$publicHost = $Matches[1]
$files = @(Get-ChildItem $server -Filter *.js | ForEach-Object Name) + @('README.md', 'deploy/install.sh', 'deploy/phone-stream.service')
foreach ($f in $files) {
    if ([IO.File]::ReadAllText((Join-Path $server $f)).Contains("`r")) { throw "$f has Windows line endings; the VM needs LF" }
}
$tgz = Join-Path ([IO.Path]::GetTempPath()) "phone-stream-$PID.tgz"
tar -czf $tgz -C $server @files
if ($LASTEXITCODE -ne 0) { throw 'tar failed' }
$b64 = [Convert]::ToBase64String([IO.File]::ReadAllBytes($tgz)) -replace '(.{76})', "`$1`n"
Remove-Item $tgz
$stamp = Get-Date -Format 'yyyy-MM-dd HH:mm'
$text = @"
cat > /root/phone-stream-install.sh <<'PHONE_STREAM_INSTALL'
#!/bin/bash
# Phone stream server, packed ${stamp}: install or update.
set -euo pipefail
dir=/root/phone-stream-src
rm -rf "`$dir"
mkdir -p "`$dir"
base64 -d <<'PHONE_STREAM_FILES' | tar -xz --no-same-owner -C "`$dir"
$b64
PHONE_STREAM_FILES
PUBLIC_HOST=$publicHost INGEST_TOKEN=$key bash "`$dir/deploy/install.sh"
PHONE_STREAM_INSTALL
bash /root/phone-stream-install.sh
"@
[IO.File]::WriteAllText($Out, ($text -replace "`r`n", "`n").TrimEnd("`n") + "`n", (New-Object Text.UTF8Encoding $false))
Write-Output "Wrote $Out ($([math]::Round((Get-Item $Out).Length / 1KB)) KB, $($files.Count) files) for $publicHost. Paste all of it into the VM's root terminal."
