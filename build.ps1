# build.ps1 - rebuilds all of Eco's executables and leaves them in bin\ and dist\.
#
#   powershell -ExecutionPolicy Bypass -File build.ps1             everything
#   powershell -ExecutionPolicy Bypass -File build.ps1 -NoLinux    without the Linux servers (no Docker needed)
#   (-NoTests skips the tests; the Spanish names -SinLinux and -SinPruebas also work)
#
# Steps: 1) Windows with MinGW + Ninja (the build folder is outside OneDrive), 2) tests: encryption, server
# flood brakes and translations (if they fail, nothing is copied), 3) Linux x86_64 and arm64 servers
# with Docker, 4) copy to bin\ and dist\ (never touches eco.db or anything that isn't an Eco executable or document;
# eco-sign.exe goes to bin\windows and the tests to bin\windows\tests, never to dist\), 5) publishes the signed
# update in dist\updates if your key is there, 6) SHA-256 sums in dist\SHA256SUMS.txt.
#
# The signing key belongs to whoever builds: local_data_DO_NOT_DISTRIBUTE\update_signing_key.txt (private) and
# update_public_key.txt (the one the clients carry). Created with: eco-sign new-key <folder>\update_signing_key.txt
param([Alias('SinLinux')][switch]$NoLinux, [Alias('SinPruebas')][switch]$NoTests)

$ErrorActionPreference = 'Stop'
$repo = $PSScriptRoot
$mingw = 'C:\msys64\mingw64\bin'
$work = Join-Path $env:LOCALAPPDATA 'eco-build'
$win = Join-Path $work 'windows'
$lin = Join-Path $work 'linux'
$signingKey = Join-Path $repo 'local_data_DO_NOT_DISTRIBUTE\update_signing_key.txt'

function Step($t) { Write-Host ""; Write-Host "== $t" -ForegroundColor Cyan }
function Fail($t) { Write-Host "ERROR: $t" -ForegroundColor Red; exit 1 }

# ---------- 1) Windows ----------
Step 'Building for Windows (MinGW)'
if (-not (Test-Path "$mingw\g++.exe")) { Fail "MinGW is not in $mingw" }
$env:PATH = "$mingw;$env:PATH"
New-Item -ItemType Directory -Force $win | Out-Null
& cmake -S $repo -B $win -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ -DECO_TESTS=ON | Out-Null
if ($LASTEXITCODE -ne 0) { Fail 'cmake (configure) failed' }
& cmake --build $win
if ($LASTEXITCODE -ne 0) { Fail 'the build failed' }
$version = ((& "$win\eco-server.exe" --version) -split ' ')[1]
Write-Host "Version: $version"

# ---------- 2) Tests ----------
if (-not $NoTests) {
    Step 'Encryption tests'
    & "$win\eco-test-crypto.exe" | Select-Object -Last 1
    if ($LASTEXITCODE -ne 0) { Fail 'the encryption tests failed: nothing is copied' }

    Step 'Server flood brake tests'
    $tmp = Join-Path $work 'server-test'
    if (Test-Path $tmp) { Remove-Item -Recurse -Force $tmp }
    New-Item -ItemType Directory -Force $tmp | Out-Null
    $srv = Start-Process -FilePath "$win\eco-server.exe" -ArgumentList '--port', '19988', '--db', "$tmp\test.db", '--backups', '0' `
        -WorkingDirectory $tmp -PassThru -WindowStyle Hidden -RedirectStandardOutput "$tmp\server.log"
    Start-Sleep -Seconds 2
    & "$win\eco-test-flood.exe" 19988
    $floodOk = $LASTEXITCODE -eq 0
    Stop-Process -Id $srv.Id -Force -ErrorAction SilentlyContinue
    if (-not $floodOk) { Fail "the server's brakes don't work: nothing is copied" }

    Step 'Translations (every text has its Spanish version)'
    $py = Get-Command python -ErrorAction SilentlyContinue
    if ($py) {
        & python "$repo\tools\check_translations.py" | Select-Object -Last 1
        if ($LASTEXITCODE -ne 0) { Fail 'missing Spanish translations (python tools\check_translations.py): nothing is copied' }
    } else {
        Write-Host 'Python is not installed: translations are not checked.' -ForegroundColor Yellow
    }
}

# ---------- 3) Linux (Docker) ----------
$linuxOk = $false
if (-not $NoLinux) {
    Step 'Linux servers (Docker: x86_64 and arm64)'
    docker info --format '{{.ServerVersion}}' 2>$null | Out-Null
    if ($LASTEXITCODE -ne 0) {
        Write-Host 'Docker is not running: start it (Docker Desktop) or use -NoLinux. Continuing without Linux.' -ForegroundColor Yellow
    } else {
        New-Item -ItemType Directory -Force $lin | Out-Null
        $linuxOk = $true
        foreach ($pl in 'amd64', 'arm64') {
            Write-Host "-- $pl"
            $cmd = 'apk add -q build-base cmake samurai asio-dev linux-headers >/dev/null && ' +
                   'cmake -S /src -B /b -G Ninja -DCMAKE_BUILD_TYPE=Release -DECO_SERVER_ONLY=ON -DECO_TESTS=OFF ' +
                   '-DCMAKE_CXX_STANDARD_LIBRARIES=-static-pie >/dev/null && cmake --build /b && ' +
                   "cp /b/eco-server /out/eco-server-$pl && /out/eco-server-$pl --version"
            docker run --rm --platform "linux/$pl" -v "${repo}:/src:ro" -v "${lin}:/out" alpine:3.22 sh -c $cmd
            if ($LASTEXITCODE -ne 0) { Write-Host "Could not build the Linux server for $pl" -ForegroundColor Yellow; $linuxOk = $false }
        }
    }
}

# ---------- 4) Copy ----------
Step 'Copying to bin\ and dist\'
foreach ($d in "$repo\bin\windows", "$repo\dist\windows") {
    New-Item -ItemType Directory -Force $d | Out-Null
    foreach ($f in 'eco-client.exe', 'eco-gui.exe', 'eco-server.exe') { Copy-Item "$win\$f" $d -Force }
}
foreach ($f in 'THIRD_PARTY.md', 'CHANGELOG.md', 'LICENSE') { Copy-Item "$repo\$f" "$repo\dist\windows" -Force }
# Only in bin\ (for whoever publishes and tests, not to hand out): the signing tool and the tests
Copy-Item "$win\eco-sign.exe" "$repo\bin\windows" -Force
New-Item -ItemType Directory -Force "$repo\bin\windows\tests" | Out-Null
Get-ChildItem "$win\eco-test-*.exe" | ForEach-Object { Copy-Item $_.FullName "$repo\bin\windows\tests" -Force }
if ($linuxOk) {
    New-Item -ItemType Directory -Force "$repo\dist\linux" | Out-Null
    Copy-Item "$lin\eco-server-amd64" "$repo\dist\linux\eco-server" -Force
    Copy-Item "$lin\eco-server-arm64" "$repo\dist\linux\eco-server-arm64" -Force
    foreach ($f in 'THIRD_PARTY.md', 'LICENSE') { Copy-Item "$repo\$f" "$repo\dist\linux" -Force }
}

# ---------- 5) Signed update ----------
if (Test-Path $signingKey) {
    Step "Publishing the signed $version update (dist\updates)"
    & "$win\eco-sign.exe" publish $signingKey $version windows-x64 "$repo\dist\updates" "$win\eco-gui.exe" "$win\eco-client.exe" | Select-Object -First 2
    & "$win\eco-sign.exe" verify "$repo\dist\updates" windows-x64 | Select-Object -Last 1
    if ($LASTEXITCODE -ne 0) { Fail "the published update doesn't verify (is update_public_key.txt the pair of your key?)" }
    Write-Host 'To offer it, start the server with: --updates <path of dist\updates>'
} else {
    Write-Host "No signing key ($signingKey): the update is not published." -ForegroundColor Yellow
    Write-Host "To create yours: bin\windows\eco-sign.exe new-key $signingKey" -ForegroundColor Yellow
}

# ---------- 6) SHA-256 sums ----------
Step 'SHA-256 sums (dist\SHA256SUMS.txt)'
$lines = @()
$files = Get-ChildItem "$repo\dist" -Recurse -File | Where-Object { $_.Extension -eq '.exe' -or $_.Name -like 'eco-server*' } |
    Where-Object { $_.FullName -notlike '*\updates\*' } | Sort-Object FullName
foreach ($f in $files) {
    $h = (Get-FileHash -Algorithm SHA256 $f.FullName).Hash.ToLower()
    $rel = $f.FullName.Substring($repo.Length + 6).Replace('\', '/')   # without "<repo>\dist\"
    $lines += "$h  $rel"
}
Set-Content -Path "$repo\dist\SHA256SUMS.txt" -Value ("# Eco $version - check with: Get-FileHash -Algorithm SHA256 <file> (Windows) or sha256sum -c (Linux)`n" + ($lines -join "`n")) -Encoding ascii
$lines | ForEach-Object { Write-Host $_ }

Step "Done: Eco $version"
