# Eco: ultra-light, end-to-end encrypted voice chat (C++)

*[Leer en español](README.es.md)*

Small programs with no installer and no DLLs: voice channels, text chat with history, screen sharing (with its
audio), accounts with roles and automatic end-to-end encryption. In English and Spanish.

**Version 2.3.0.** What changes in each version: [CHANGELOG.md](CHANGELOG.md). To see yours: `eco-server --version`,
`eco-client --version`, `/version` in the chat, or the **Eco** menu in the window.

| Program | Size (Windows) | Typical usage |
|---|---|---|
| `eco-server` | ~2.4 MB | 1 thread, ~6 MB of RAM, almost 0 % CPU (it only relays packets) |
| `eco-client` | ~2.3 MB | Console client. ~3 MB of RAM, <0.1 % CPU when silent, 40 kbps voice only while you talk |
| `eco-gui` | ~3.9 MB | The same client with a window (Windows, Linux and macOS). ~60 MB of RAM (mostly the graphics driver), <1 % CPU when idle |

> **There are no official binaries.** Eco is meant for you and your friends: whoever runs a server builds Eco
> from source and signs their own updates with their own key (see [Build](#build) and
> [Your own signing key](#your-own-signing-key)). That way nobody depends on someone else's key.

## Getting started

1. **Whoever hosts the server** opens `eco-server` (double click). The window shows an **invitation**, a line
   starting with `eco://`. If your friends connect over the Internet, open port 9987 (TCP and UDP) on your router
   and replace `TU-IP-PUBLICA` with your public IP or domain. Once inside Eco, the **Eco > Invite a friend** menu
   copies it with the right address.
2. **Each friend** opens `eco-gui`, presses **Paste** (or pastes the invitation into "Server or invitation"), types
   their name and presses **Connect**. That's it.

The invitation carries the address, the port, the server password (if any) and the **fingerprint** of the server's
identity: Eco checks by itself that the server is the real one, so not even the first connection is "blind".
The first time, a setup assistant and a quick guide show up (later in **Help > Quick guide**).

### By hand (console)

```bat
eco-server --admin MyAdminPassword          (the host; open port 9987 TCP and UDP)
eco-client 192.168.1.10 John                (everyone else)
eco-client my-server.com:9987 John --password MyPassword
eco-client "eco://my-server.com:9987/#f=..." John   (with an invitation)
```
Double click also works: `eco-server` starts with the defaults and `eco-client` asks for server and nick.

### Language

Eco speaks **English and Spanish**. By default it uses your system's language; you can change it in
**Settings > Idioma / Language**, in the first-time assistant, with `/language en|es` in the chat, with
`--language en|es` (`--idioma`) on the command line or with the environment variable `ECO_LANG` (`ECO_IDIOMA`).
The choice is remembered. Commands work in both languages (`/kick` and `/expulsar` are the same command), and the
autocompletion shows the names of your language. The server's console and log follow `--language` or the system
language. Messages the server sends to clients always travel in Spanish and each client translates them, so a
client in English also understands servers from older 2.x versions.

### Graphical interface (`eco-gui`)
Open `eco-gui`, paste the invitation (or type the server), your name and press **Connect**
(`eco-gui "eco://..." John` or `eco-gui 192.168.1.10 John` connects directly).
- **Left:** channels and users. The dot turns green when someone talks. Double-click a channel to join and a user
  to send them a private message or watch their screen. Right click: private message, watch screen, **Verify
  identity** (shows the code to compare and a "They match" button; verified people carry the *verified* tag, and if
  their identity changes, *identity changed!*) and, if you are mod or admin, move, mute, kick, ban or change role.
- **The first time** an assistant asks for your language, name, microphone and headphones (with a bar to check that
  it picks you up and to adjust the sensitivity) and the server. It can be reopened from the start screen.
- **Right:** the chat. The text box also takes all the console `/commands`: typing `/` shows the list with what
  each one does, and **Tab** completes commands, nicks and channels (Tab again, the next option).
- **Top:** mute mic, deafen, **Share screen** (asks what: each monitor or a specific window), **PC audio: On/Off**
  (also send what plays on your PC to those watching your screen, without the channel's voices; a single
  application can be chosen) and **Settings** (language, microphone and output, volume, voice detection or
  push-to-talk with level meter, quality, fps and width when sharing, screen audio, E2E phrase).
- Other people's screens are shown inside the window; if they carry audio, it has its own volume.
- **Bottom:** connection status, **ping** and **lost voice** in the last few seconds (yellow or red if noticeable).
- **Automatic reconnection:** if the connection drops, Eco retries by itself (1, 2, 4… up to every 30 s) and takes
  you back to the same channel. Not if you were kicked or the server identity changed.
- **Updates:** if the server offers a new signed version, a notice with **Install** and then **Restart now** shows
  up. *Eco > About Eco* shows the version, the server's version, privacy and licenses.
- If Eco closed unexpectedly, it says so once when opened and shows where the report is (no personal data; it is
  not sent anywhere).
- It remembers server, nick and settings in `%APPDATA%\eco_gui.txt` (`~/.eco_gui` on Linux and macOS). The account
  password is not saved.
- To use little power it only repaints while there is activity. On Windows it draws with Direct3D 11, on Linux and
  macOS with OpenGL 3.2.

## What's included

- **Audio devices:** by default (`auto`) it uses the headphones Windows has as output and their own microphone;
  it never picks "Stereo Mix". If you plug in or remove headphones, it switches by itself. `/mic` and `/output` pin
  others (also `--input` / `--output` at startup, by number or part of the name).
- **Voice:** Opus (48 kHz, 20 ms, 40 kbps) over UDP. If your network blocks UDP (corporate firewalls, some wifi),
  Eco detects it in a few seconds and sends the voice over TCP, with a bit more delay; when UDP comes back, it
  switches by itself. Nothing is sent while silent; there is also global push-to-talk.
- **Noise suppression** (on by default) with [RNNoise](https://github.com/xiph/rnnoise), plus a high-pass filter.
- **Volume:** automatic gain control takes your voice to a standard level (−20 dBFS). Each person can also have
  their own volume from 0 to 300 %, remembered by nick.
- **Choppy networks:** a jitter buffer with Opus FEC and loss concealment, from 40 ms up to 120 ms when needed.
- **Chat with history**, **private messages** (delivered when the recipient logs in, if they have an account).
- **Screen sharing:** a whole monitor or a single window. On Windows it uses DXGI Desktop Duplication (~3 ms per
  1080p frame, GDI as fallback). Only the 128×128 tiles that changed are sent (JPEG), and nothing is captured if
  nobody watches.
- **Screen audio:** what plays on your PC, in stereo, encrypted and signed. On Windows 10 2004+ Eco's own sound is
  left out and a single application can be chosen. On Linux, all the sound (PulseAudio or PipeWire).
- **Accounts and roles:** `/register <password>` reserves your nick. Roles `admin`, `mod` and `user`, kept across
  restarts like channels, bans and mutes.
- **Database:** everything is kept in `eco.db` (SQLite, built into `eco-server`), with a daily backup in `copias/`.
- **Signed updates** offered by the server itself (`--updates folder`). Clients only install them if they are
  signed with the key their build carries, newer than their own and for their platform.
- **Crash reports:** a text file with the program, version and point of failure, without personal data. Eco never
  sends it.

## Security

All the cryptography comes from [Monocypher](https://monocypher.org); Eco doesn't invent its own algorithms.

1. **Automatic end-to-end encryption (always on, Signal style):** each device creates its own identity key
   (X25519), which never leaves it. Private messages use a **Double Ratchet**. Channels (text, voice and screen)
   use **sender keys**, renewed whenever someone joins or leaves, and every text message and screen frame is
   **signed**. Replayed messages are dropped. `/verify <nick>` shows a 30-digit **safety code**; `/confirm` marks a
   contact as verified and Eco **blocks** them if their key changes until you `/accept` it.
2. **Client ↔ server encryption** underneath: X25519 with new keys on each connection and XChaCha20-Poly1305. The
   server has an Ed25519 identity that the client remembers (like SSH). Passwords are stored with Argon2id. Many
   limits against abuse (connections, messages, voice and screen packets, failed passwords, disk usage).
3. **Shared phrase (optional, `/e2e <phrase>`)**.

**Limits worth knowing:** the server still sees metadata (nicks, channels, who talks and when); until you compare
safety codes, a malicious server could hand out fake keys; there are no signed prekeys on the server; each device
has its own identity. Eco has not had an external security audit: the internal review is in
[`SECURITY_AUDIT.md`](SECURITY_AUDIT.md) (in Spanish) and how to report a vulnerability is in
[`SECURITY.md`](SECURITY.md).

## Client commands

Every command also works with its Spanish name (shown in parentheses).

| Command | What it does |
|---|---|
| *text* | Message to the channel |
| `/channels` · `/join <channel>` | List channels and users / switch channel (`/canales`, `/unir`) |
| `/pm <nick> <text>` · `/pms [n]` | Private message / saved ones (`/privados`) |
| `/history [n]` | Latest channel messages, max. 200 (`/historial`) |
| `/register <password>` · `/password <new>` | Create your account / change password (`/registrar`, `/clave`) |
| `/verify <nick>` · `/confirm <nick>` · `/accept <nick>` | Safety code / mark as verified / accept their new key (`/verificar`, `/confirmar`, `/aceptar`) |
| `/invite` | Invitation to this server for a friend (`/invitar`) |
| `/e2e <phrase>` · `/e2e off` | Optional: encrypt with a phrase shared by the group |
| `/mute`, `/deafen` | Mute mic / stop hearing others (`/sordo`) |
| `/ptt v` · `/ptt off` | Push-to-talk with the V key (also `f8`, `ctrl`, `alt`, `mouse4`…) |
| `/threshold 12` · `/vol 150` · `/quality 48` | Mic sensitivity / volume (%) / voice bitrate (`/umbral`, `/calidad`) |
| `/noise on\|off` · `/agc on\|off` · `/volume <nick> <0-300>` | Noise suppression / automatic gain / volume of one person (`/ruido`, `/volumen`) |
| `/sources` · `/share [fps] [width] [source]` · `/stopshare` | Share the screen (`/fuentes`, `/compartir`, `/parar`) |
| `/shareaudio on\|off\|apps` · `/shareaudio app <n>\|all` | PC audio while sharing (`/audiopantalla`) |
| `/watch <nick>` · `/unwatch` | Open or close someone's screen (`/ver`, `/dejar`) |
| `/devices` · `/mic [n\|auto]` · `/output [n\|auto]` · `/level` | Audio devices (`/dispositivos`, `/salida`, `/nivel`) |
| `/version` · `/update` · `/ping` · `/stats` | Versions / install the offered update / ping and lost voice / statistics (`/actualizar`) |
| `/language es\|en` | Eco's language (`/idioma`) |
| `/login <password>` | Become mod or admin with the server password (`--admin`, `--mod`) |
| **Mod:** `/kick`, `/move`, `/silence`, `/unsilence`, `/createchannel` | (`/expulsar`, `/mover`, `/silenciar`, `/desilenciar`, `/crearcanal`) |
| **Admin:** `/ban`, `/unban`, `/bans`, `/deletechannel`, `/role <nick> admin\|mod\|user`, `/accounts`, `/deleteaccount`, `/resetpassword` | (`/banear`, `/desbanear`, `/baneados`, `/borrarcanal`, `/rol`, `/cuentas`, `/borrarcuenta`, `/resetclave`) |

## Server options

```
eco-server [--port 9987] [--pass password-to-join] [--admin password] [--mod password]
           [--max 64] [--db eco.db] [--retain days] [--channels "Lobby,General,Games"]
           [--backups 7] [--updates folder] [--language es|en] [--version]
```
(The Spanish names `--puerto`, `--retener`, `--canales`, `--copias`, `--actualizaciones` and `--idioma` also work.)
- Without `--admin`, and if there is no admin yet, a **one-time** master password is shown to create the first one:
  join, `/register <password>` and `/login <master password>`.
- Passwords can also be given with `ECO_SERVER_PASS`, `ECO_ADMIN_PASS` and `ECO_MOD_PASS` (on the command line any
  user of the system can see them with `ps`). On the client: `ECO_CLAVE`, `ECO_PASS` and `ECO_E2E`.
- On Linux, `dist/linux/install.sh` installs the server as a systemd service.

## Build

### Windows (everything at once)
With [MSYS2](https://www.msys2.org) MinGW-w64 in `C:\msys64\mingw64`, CMake and Ninja (Docker optional, for the
Linux servers):
```powershell
powershell -ExecutionPolicy Bypass -File build.ps1           # or -NoLinux if Docker is not running
```
It builds outside the repository (`%LOCALAPPDATA%\eco-build`), runs the tests (if they fail nothing is copied),
copies the executables to `bin/` and `dist/`, publishes your signed update in `dist/updates` if you have a key and
writes `dist/SHA256SUMS.txt`.

### Windows (by hand)
CMake and Visual Studio 2022 (or MinGW). Opus, PortAudio, Asio, RNNoise, Dear ImGui and GLFW are downloaded by
themselves (pinned to exact versions); SQLite, Monocypher and stb are in `third_party/`.
```bat
cmake -S . -B build
cmake --build build --config Release
```

### Linux
```bash
sudo apt install cmake g++ git libasio-dev libopus-dev portaudio19-dev libx11-dev \
                 libgl-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev   # the last 5, for eco-gui
cmake -S . -B build && cmake --build build -j
```
Only the server (e.g. on a VPS): `-DECO_SERVER_ONLY=ON`, which only needs `libasio-dev`. Without the GUI: `-DECO_GUI=OFF`.

### macOS
```bash
brew install cmake asio opus portaudio glfw pkg-config
cmake -S . -B build && cmake --build build -j
open build/eco-gui.app
```
Push-to-talk asks for the "Input Monitoring" permission. **Sharing the screen from a Mac is not possible yet**
(ScreenCaptureKit is missing); watching other people's screens does work.

### Tests
```bash
build/eco-test-crypto                       # Double Ratchet, sender keys, signatures, anti-replay, invitations
eco-server --port 19988 --backups 0 &       # and then:
build/eco-test-flood 19988                  # the server's flood brakes
build/eco-test-fuzz 19988 60                # random and malformed data against the server
build/eco-test-fuzz-client 19990 45         # a malicious server (connect an eco-client to it with --trust)
build/eco-test-load 19988 30 10             # 30 clients talking at once
python tools/check_translations.py          # every text has its Spanish translation
```

### Your own signing key

Automatic updates are only accepted if they are signed with the key built into the clients. The repository has no
key: a build without one simply has automatic updates turned off. To have yours:
```bash
eco-sign new-key local_data_DO_NOT_DISTRIBUTE/update_signing_key.txt
```
That writes your private key and, next to it, `update_public_key.txt`. CMake reads the public key from there (or
from `-DECO_UPDATE_PUBLIC_KEY=<64 hex>`) and puts it into the clients you build. To publish a version: raise
`project(Eco VERSION x.y.z)` in `CMakeLists.txt`, run `build.ps1` (or
`eco-sign publish <key> <version> windows-x64 <folder> eco-gui.exe eco-client.exe`) and start the server with
`--updates <folder>`. **Never share or upload the private key**; `local_data_DO_NOT_DISTRIBUTE/` is in `.gitignore`.

## Architecture

```
common/protocol.hpp    binary messages
common/secure.*        key exchange, TCP/UDP encryption, anti-replay
common/i18n.*          languages: T("text") and the Spanish table (i18n_es.inc)
common/update*.hpp     signed updates: format, signature and public key
common/crash.hpp       crash reports
server/server.cpp      single-thread asio server: sessions, channels, permissions, relay
server/db.*            SQLite: accounts (Argon2id), channels, bans, messages
client/client.*        network, channel logic and commands; client.hpp is the interface for console and GUI
client/commands.hpp    the chat commands (English and Spanish names, autocompletion)
client/gui.cpp         eco-gui (Dear ImGui + GLFW); client/gui_render_* draws with Direct3D 11 or OpenGL 3.2
client/keys.*          automatic E2E: device identity, private messages and sender keys
client/e2e.*           optional E2E with a shared phrase
client/audio.*         PortAudio + Opus + RNNoise: capture, filters, voice detection, jitter, mix
client/screencodec.*   tiles + JPEG (stb); client/screen_*.cpp capture and viewer per platform
client/loopback.*      PC audio while sharing (WASAPI on Windows, PulseAudio/PipeWire on Linux)
tools/sign.cpp         eco-sign: publish signed versions
tools/check_translations.py   reports texts without a Spanish translation
tests/                 encryption, flood, fuzzing and load tests
build.ps1              builds and tests everything and copies it to bin/ and dist/
third_party/           sqlite (public domain), monocypher (CC0/BSD), stb (MIT / public domain)
```

## Contributing and license

Contributions are welcome: see [CONTRIBUTING.md](CONTRIBUTING.md). Eco is released under the
[MIT license](LICENSE); the third-party libraries keep their own licenses, all compatible
([THIRD_PARTY.md](THIRD_PARTY.md)).

Developed by [pancito2702](https://github.com/pancito2702), with Claude Code (Anthropic) as a programming assistant.
