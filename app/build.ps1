# Builds PhoneStream.exe with Visual Studio 2022 (Build Tools) and its bundled
# CMake and Ninja. Two local files next to this script are built into the .exe
# and are not part of the repository:
#   ingest-key.txt      the key the app and the server share (created once,
#                       random); it must equal INGEST_TOKEN on the server
#   server-address.txt  the server the app starts with, e.g.
#                       wss://phone.example.org/ingest (-Server sets and saves it)
#   powershell -ExecutionPolicy Bypass -File build.ps1 [-Server wss://phone.example.org/ingest]
param(
    [string]$Server = '',
    [string]$VsVars = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat'
)
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$keyFile = Join-Path $here 'ingest-key.txt'
if (-not (Test-Path $keyFile)) {
    $bytes = New-Object byte[] 24
    [Security.Cryptography.RandomNumberGenerator]::Create().GetBytes($bytes)
    Set-Content -Path $keyFile -Value (($bytes | ForEach-Object { $_.ToString('x2') }) -join '') -NoNewline -Encoding ascii
    Write-Output "Created a new shared key in $keyFile"
}
$key = (Get-Content $keyFile -Raw).Trim()
if ($key -notmatch '^[A-Za-z0-9_-]{16,}$') { throw "ingest-key.txt must hold one key of at least 16 letters/digits" }
$serverFile = Join-Path $here 'server-address.txt'
if ($Server) {
    Set-Content -Path $serverFile -Value $Server -NoNewline -Encoding ascii
} elseif (Test-Path $serverFile) {
    $Server = (Get-Content $serverFile -Raw).Trim()
} else {
    throw "Give the server's address once: build.ps1 -Server wss://phone.example.org/ingest (it is kept in server-address.txt)"
}
if ($Server -notmatch '^wss://[A-Za-z0-9.-]+(:\d+)?/[A-Za-z0-9/_.-]*$') { throw "The server address must look like wss://phone.example.org/ingest" }
if (-not (Test-Path $VsVars)) { throw "Visual Studio Build Tools not found: $VsVars" }
$build = Join-Path $here 'build'
$cmd = "`"$VsVars`" >nul && cmake -S `"$here`" -B `"$build`" -G Ninja -DCMAKE_BUILD_TYPE=Release -DPS_INGEST_KEY=$key -DPS_DEFAULT_SERVER=$Server && cmake --build `"$build`""
cmd /c $cmd
if ($LASTEXITCODE -ne 0) { throw 'Build failed.' }
Write-Output "Built $(Join-Path $build 'PhoneStream.exe') for $Server"
