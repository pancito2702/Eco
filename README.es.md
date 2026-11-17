# Eco: chat de voz ultraligero y cifrado, estilo TeamSpeak (C++)

*[Read in English](README.md)*

Programas pequeños, sin instalador ni DLLs: canales de voz, chat de texto con historial, pantalla compartida, cuentas con roles y cifrado de extremo a extremo automático. En español y en inglés.

**Versión 2.3.0.** Qué cambia en cada versión: [CHANGELOG.md](CHANGELOG.md). Para ver la tuya: `eco-server --version`, `eco-client --version`, `/version` en el chat, o el menú **Eco** en la ventana.

| Programa | Tamaño (Windows) | Uso típico |
|---|---|---|
| `eco-server.exe` | ~2,4 MB | 1 hilo, ~6 MB de RAM, casi 0 % de CPU (solo reenvía paquetes) |
| `eco-client.exe` | ~2,3 MB | Cliente de consola. ~3 MB de RAM, <0,1 % de CPU en silencio, voz a 40 kbps solo mientras hablas |
| `eco-gui.exe` | ~3,9 MB | El mismo cliente con ventana (Windows, Linux y macOS). ~60 MB de RAM (casi todo es el driver gráfico), <1 % de CPU en reposo |

> **No hay ejecutables oficiales.** Eco es para ti y tus amigos: quien monta un servidor compila Eco desde el código y firma sus propias actualizaciones con su propia clave (ver [Compilar](#compilar) y
> [Tu propia clave de firma](#tu-propia-clave-de-firma)). Así nadie depende de la clave de otro.

## Empezar (lo fácil)

1. **Quien tiene el servidor** abre `eco-server.exe` (doble clic). En la ventana aparece una **invitación**, una línea
   que empieza por `eco://`. Si tus amigos se conectan por Internet, abre en el router el puerto 9987 (TCP y UDP) y cambia `TU-IP-PUBLICA` por tu IP pública o tu dominio. Ya dentro de Eco, el menú **Eco > Invitar a un amigo**  la copia con la dirección buena.
2. **Cada amigo** abre `eco-gui.exe`, pulsa **Pegar** (o pega la invitación en «Servidor o invitación»), escribe su nombre y pulsa **Conectar**. Nada más.

La invitación lleva la dirección, el puerto, la clave del servidor (si tiene) y la **huella** de su identidad: Eco comprueba solo que el servidor es el auténtico, así que ni la primera conexión es «a ciegas». La primera vez se muestra un asistente y una guía rápida (luego está en **Ayuda > Guía rápida**).

### A mano (consola)

```bat
eco-server --admin MiClaveAdmin            (quien aloja; abre el puerto 9987 TCP y UDP)
eco-client 192.168.1.10 Juan               (los demás)
eco-client mi-servidor.com:9987 Juan --clave MiClave
eco-client "eco://mi-servidor.com:9987/#f=..." Juan   (con invitación)
```
Con doble clic también funcionan: `eco-server.exe` arranca con los valores por defecto y `eco-client.exe` pregunta servidor y nick.

### Idioma

Eco habla **español e inglés**. Por defecto usa el idioma del sistema; se cambia en **Ajustes > Idioma / Language**, en el asistente de primera vez, con `/idioma es|en` en el chat, con `--idioma es|en` al arrancar o con la variable `ECO_IDIOMA`. Se recuerda. Los comandos valen en los dos idiomas (`/expulsar` y `/kick` son el mismo) y el autocompletado enseña los del tuyo. La consola y el registro del servidor siguen `--idioma` o el idioma del sistema. Lo que el servidor manda a los clientes viaja siempre en español y lo traduce cada cliente, así que un cliente en inglés también se entiende con servidores 2.x antiguos.

### Interfaz gráfica (`eco-gui`)
Abre `eco-gui.exe`, pega la invitación (o escribe el servidor), tu nombre y pulsa **Conectar** (`eco-gui "eco://..." Juan` o `eco-gui 192.168.1.10 Juan` conecta directamente).
- **Izquierda:** canales y usuarios. El punto se pone verde cuando alguien habla. Doble clic en un canal para entrar y en un usuario para escribirle por privado o ver su pantalla. Con clic derecho: privado, ver pantalla, **Verificar identidad** (muestra el código para compararlo y un botón «Coinciden»; quien está verificado lleva la etiqueta *verificado*, y si su identidad cambia, *¡identidad cambiada!*) y, si eres mod o admin, mover, silenciar, expulsar, banear o cambiar el rol.
- **La primera vez** sale un asistente: idioma, tu nombre, tu micrófono y audífonos (con una barra para comprobar que te capta y ajustar la sensibilidad) y el servidor. Se puede volver a abrir desde la pantalla de inicio.
- **Derecha:** el chat. La caja de texto acepta también todos los `/comandos` de la consola: al escribir `/` sale la lista con lo que hace cada uno, y **Tab** completa comandos, nicks y canales (otra vez Tab, la siguiente opción).
- **Arriba:** silenciar micro, dejar de oír, **Compartir pantalla** (pregunta qué: cada monitor o una ventana concreta), **Audio del PC: Sí/No** (mandar también lo que suena en tu PC a quienes miran tu pantalla, sin las voces del canal; se puede elegir una sola aplicación) y **Ajustes** (idioma, micrófono y salida, volumen, detección de voz o pulsar-para-hablar con medidor de nivel, calidad, fps y ancho al compartir, audio de pantalla, frase E2E).
- La pantalla de otros se ve dentro de la propia ventana; si lleva audio, tiene su propio volumen. En la lista, quien comparte con audio lleva la etiqueta *pantalla + audio*.
- **Abajo:** el estado de la conexión, el **ping** y la **voz perdida** de los últimos segundos (en amarillo o rojo si se nota).
- **Reconexión automática:** si se cae la conexión, Eco vuelve a intentarlo solo (1, 2, 4… hasta cada 30 s) y te devuelve al mismo canal. No lo hace si te expulsaron o si la identidad del servidor cambió.
- **Actualizaciones:** si el servidor ofrece una versión nueva firmada, sale un aviso con el botón **Instalar** y después **Reiniciar ahora**. *Eco > Acerca de Eco* muestra la versión, la del servidor, la privacidad y las licencias.
- Si Eco se cerró de forma inesperada, al abrirlo avisa una vez y dice dónde está el informe (sin datos personales; no se envía a nadie).
- Si la identidad del servidor cambia, muestra el aviso con la huella y un botón para confiar tras comprobarla.
- **Cuenta** > *Reservar mi nombre*, *Cambiar contraseña* y *Subir a mod/admin* piden la contraseña en una ventana con el texto oculto (y repetida, para no equivocarse).
- Recuerda servidor, nick y ajustes en `%APPDATA%\eco_gui.txt` (`~/.eco_gui` en Linux y macOS). La contraseña de la cuenta no se guarda.
- Para gastar poco, solo repinta mientras hay actividad; en reposo se despierta cuando llega un mensaje o mueves el ratón. En Windows dibuja con Direct3D 11 y en Linux y macOS con OpenGL 3.2.

Los ejecutables de Windows sin firma de código pueden mostrar el aviso de SmartScreen («Más información → Ejecutar de todas formas»).

## Qué incluye

- **Dispositivos de audio**: por defecto (`auto`) usa los audífonos que Windows tenga como salida y su propio micrófono; nunca elige «Mezcla estéreo». Si conectas o quitas unos audífonos, cambia solo. Con `/mic` y `/salida` fijas otros (también `--entrada` / `--salida` al iniciar, con número o parte del nombre).
- **Voz**: Opus (48 kHz, 20 ms, 40 kbps) por UDP. Si tu red bloquea UDP (cortafuegos de empresa, algunas wifi), Eco lo detecta en unos segundos y manda la voz por TCP, con algo más de retraso; cuando UDP vuelve, cambia solo. En silencio no envía nada; también hay pulsar-para-hablar global. Guarda 40 ms de antes de detectar tu voz y 400 ms de cola, así no corta el principio ni el final de las frases.
- **Supresión de ruido** (activada por defecto): quita de tu micro el ventilador, el teclado o la calle con [RNNoise](https://github.com/xiph/rnnoise). Botón «Quitar ruido: Sí/No» en `eco-gui` o `/ruido on|off` en la consola; se recuerda. Solo trabaja mientras hablas (~1 % de un núcleo). Además, un filtro de graves quita zumbidos y golpes.
- **Volumen**: el control automático de ganancia lleva tu voz a un nivel estándar (−20 dBFS): si tu micro capta bajo te sube hasta +15 dB y si satura te baja, sin tocar los silencios (`/agc on|off`, activado por defecto). Además puedes poner a cada persona su propio volumen de 0 a 300 % (clic derecho sobre ella en `eco-gui` o `/volumen <nick> <0-300>`); se recuerda por nick.
- **Red con tirones**: el búfer de jitter guarda los paquetes y los decodifica al reproducir. Si falta uno, lo reconstruye con la redundancia (FEC) del siguiente o con la ocultación de Opus. Empieza con 40 ms y crece solo hasta 120 ms si la red va a tirones; luego vuelve a bajar. Con varias personas a la vez, un limitador suave evita que la mezcla "rompa".
- **Chat con historial**: los mensajes de cada canal se guardan cifrados. Al entrar ves los últimos 30 y `/historial 200` trae más.
- **Mensajes privados**: `/pm`. Si el destinatario tiene cuenta y está desconectado, le llegan al entrar. `/privados` muestra los tuyos.
- **Pantalla compartida**: un monitor entero o una sola ventana (aunque esté tapada por otras). En Windows se captura con DXGI Desktop Duplication (~3 ms por fotograma a 1080p frente a ~16 ms con GDI, que queda como respaldo); si no cambió nada, ni se compara ni se codifica. Solo se envían en JPEG las zonas de 128×128 que cambiaron, y si nadie te mira no se captura nada.
- **Audio de la pantalla**: lo que suena en tu PC (juego, vídeo, música) para quienes miran tu pantalla, en estéreo, cifrado y firmado. En Windows 10 2004 o posterior se deja fuera el sonido de Eco y se puede elegir **una sola aplicación**. En Linux se comparte todo el sonido (PulseAudio o PipeWire).
- **Cuentas y roles**: `/registrar <clave>` reserva tu nick. Roles `admin`, `mod` y `usuario`, que se conservan entre reinicios igual que los canales y los baneos. Banear a alguien con cuenta banea también la cuenta, y se puede banear una cuenta aunque no esté conectada. Los silencios siguen al reconectar.
- **Base de datos**: todo se guarda en `eco.db` (SQLite, incluido dentro de `eco-server`). Cada día se guarda una copia en `copias/` junto a `eco.db` (las 7 últimas; `--copias N`). Lleva la identidad del servidor: guárdala como la propia `eco.db`.
- **Actualizaciones firmadas**: el propio servidor puede ofrecer a los clientes la versión nueva (`--updates carpeta`). Los clientes solo la instalan si va firmada con la clave que llevan compilada, es más nueva que la suya y es de su plataforma; un servidor malicioso no puede colar otra cosa ni hacer volver a una versión anterior.
- **Informes de cierre inesperado**: si un programa se cae, deja un archivo de texto (programa, versión, sistema y en qué punto del código falló, sin datos personales) en `%APPDATA%\eco_fallos` (`~/.eco_fallos`; el servidor, en `./fallos`). Eco nunca lo envía.

## Seguridad

Toda la criptografía viene de [Monocypher](https://monocypher.org) (una librería auditada de un solo archivo); Eco no inventa algoritmos propios.

**1. Cifrado de extremo a extremo automático (siempre activo, al estilo de Signal)**
- Cada dispositivo crea su propia clave de identidad (X25519) la primera vez. Nunca sale del equipo; el servidor solo reparte las claves públicas.
- **Privados: Double Ratchet**, como Signal. La sesión arranca con X25519 entre las dos identidades más una clave efímera (un X3DH simplificado, sin prekeys en el servidor). Después **cada mensaje usa una clave nueva que se borra al usarla** (secreto hacia adelante) y cada respuesta trae un intercambio X25519 nuevo (recuperación tras un compromiso).
- **Canales (texto, voz y pantalla):** cada miembro tiene una «clave de emisor» que entrega, cifrada uno a uno, a los demás miembros. Se renueva cada vez que alguien entra o sale.
- **Firmas por mensaje en canales:** cada clave de emisor trae su propio par EdDSA y cada mensaje de texto y cada fotograma de pantalla van firmados. Otro miembro del canal no puede hacerse pasar por ti; además se comprueba que la clave es de quien el servidor dice que envía.
- El servidor reenvía y guarda datos que no puede leer (XChaCha20-Poly1305). No hay modo sin cifrar.
- **Anti-repetición:** un mensaje que llega en vivo y ya se había recibido, o que usa una clave de emisor vieja, se descarta.
- `/verificar <nick>` muestra un **código de seguridad** de 30 dígitos. Si a los dos les sale el mismo, nadie se ha puesto en medio, ni siquiera el servidor. `/confirmar <nick>` lo marca como **verificado**: si su clave cambia más adelante, Eco lo **bloquea** hasta que lo compruebes otra vez y escribas `/aceptar <nick>`.

**2. Cifrado cliente ↔ servidor (por debajo de lo anterior)**
- Intercambio X25519 con claves nuevas en cada conexión y todo el tráfico con XChaCha20-Poly1305.
- El servidor tiene una identidad Ed25519. El cliente la recuerda la primera vez, como SSH, y si cambia **se niega a conectar**.
- Las contraseñas se guardan con Argon2id (19 MiB, 2 pasadas), calculado en un hilo aparte, con límite de intentos fallidos por IP (en IPv6, por red /64) y por cuenta.
- Protección contra abusos: límites de conexiones nuevas por IP, de conexiones sin identificar, de mensajes, de paquetes de voz, de bytes y fotogramas de pantalla, de tamaño de mensaje, de tiempo para identificarse y de memoria por cliente lento. Para que nadie llene el disco, se guardan como mucho 20 000 mensajes por canal, 5 000 privados por persona y 500 sin entregar, y el registro escribe como mucho 60 entradas y salidas por minuto.
- Los nicks y nombres de canal no pueden llevar caracteres invisibles, marcas de dirección (bidi) ni mezclar alfabetos que se confunden.
- Ejecutables con protección de pila, FORTIFY, ASLR y DEP (y RELRO y PIE en Linux) y comprobación de límites en los contenedores del código propio; las dependencias se descargan fijadas a un commit exacto.

**3. Frase compartida (opcional, `/e2e <frase>`)**
- Sustituye las claves automáticas por unas derivadas de una frase que el grupo comparte por otro medio.

**Límites que conviene conocer**
- El servidor sigue viendo los metadatos: nicks, canales, quién habla y cuándo.
- Mientras no comparéis el código de seguridad, un servidor malicioso podría repartir claves falsas. Verificad con `/verificar`.
- No hay prekeys firmadas en el servidor y las claves de canal rotan con cada entrada o salida, no por mensaje.
- Cada dispositivo tiene su identidad: en otra PC no podrás leer el historial anterior.
- No ha pasado una auditoría de seguridad externa. La revisión interna está en [`SECURITY_AUDIT.md`](SECURITY_AUDIT.md); cómo avisar de un fallo, en [`SECURITY.md`](SECURITY.md).

## Comandos del cliente

Todos valen también con su nombre en inglés (entre paréntesis).

| Comando | Qué hace |
|---|---|
| *texto* | Mensaje al canal |
| `/canales` · `/unir <canal>` | Ver canales y usuarios (`@`=admin, `+`=mod) / cambiar de canal (`/channels`, `/join`) |
| `/pm <nick> <texto>` · `/privados [n]` | Mensaje privado / ver los guardados (`/pms`) |
| `/historial [n]` | Últimos mensajes del canal, máx. 200 (`/history`) |
| `/registrar <clave>` · `/clave <nueva>` | Crear tu cuenta / cambiar contraseña (`/register`, `/password`) |
| `/verificar` · `/confirmar` · `/aceptar <nick>` | Código de seguridad / marcar como verificado / aceptar su clave nueva (`/verify`, `/confirm`, `/accept`) |
| `/invitar` | Invitación a este servidor para pasársela a un amigo (`/invite`) |
| `/e2e <frase>` · `/e2e off` | Opcional: cifrar con una frase compartida por el grupo |
| `/mute`, `/sordo` | Silenciar micro / dejar de oír (`/deafen`) |
| `/ptt v` · `/ptt off` | Pulsar-para-hablar con la tecla V (también `f8`, `ctrl`, `alt`, `mouse4`…) |
| `/umbral 12` · `/vol 150` · `/calidad 48` | Sensibilidad del micro / volumen (%) / bitrate de voz (`/threshold`, `/quality`) |
| `/ruido on\|off` · `/agc on\|off` · `/volumen <nick> <0-300>` | Supresión de ruido / volumen automático / volumen de una persona (`/noise`, `/volume`) |
| `/fuentes` · `/compartir [fps] [ancho] [fuente]` · `/parar` | Compartir pantalla (`/sources`, `/share`, `/stopshare`) |
| `/audiopantalla on\|off\|apps` · `/audiopantalla app <n>\|todo` | Audio del PC al compartir (`/shareaudio`) |
| `/ver <nick>` · `/dejar` | Abrir o cerrar la pantalla de alguien (`/watch`, `/unwatch`) |
| `/dispositivos` · `/mic [n\|auto]` · `/salida [n\|auto]` · `/nivel` | Dispositivos de audio (`/devices`, `/output`, `/level`) |
| `/version` · `/actualizar` · `/ping` · `/stats` | Versiones / instalar la versión ofrecida / ping y voz perdida / estadísticas (`/update`) |
| `/idioma es\|en` | Idioma de Eco (`/language`) |
| `/login <clave>` | Subir a mod o admin con la clave del servidor (`--admin`, `--mod`) |
| **Mod:** `/expulsar`, `/mover`, `/silenciar`, `/desilenciar`, `/crearcanal` | (`/kick`, `/move`, `/silence`, `/unsilence`, `/createchannel`) |
| **Admin:** `/banear`, `/desbanear`, `/baneados`, `/borrarcanal`, `/rol <nick> admin\|mod\|usuario`, `/cuentas`, `/borrarcuenta`, `/resetclave` | (`/ban`, `/unban`, `/bans`, `/deletechannel`, `/role`, `/accounts`, `/deleteaccount`, `/resetpassword`) |

## Opciones del servidor

```
eco-server [--puerto 9987] [--pass clave-para-entrar] [--admin clave] [--mod clave]
           [--max 64] [--db eco.db] [--retener dias] [--canales "Lobby,General,Juegos"]
           [--copias 7] [--updates carpeta] [--idioma es|en] [--version]
```
(También valen los nombres en inglés: `--port`, `--retain`, `--channels`, `--backups`, `--language`; y `--actualizaciones` para `--updates`.)
- `--copias 7`: copias diarias de `eco.db` en `copias/`, se guardan las últimas 7 (`0` = ninguna).
- `--updates dist/updates`: ofrece a los clientes la versión publicada ahí (ver *Tu propia clave de firma*).
- `--canales` solo se usa la primera vez; después mandan los canales guardados en `eco.db`.
- `--retener 90` borra los mensajes de más de 90 días (por defecto se guardan siempre).
- Sin `--admin`, y si aún no hay ningún admin, se muestra una clave maestra **de un solo uso** para crear el primero: entra, `/registrar <clave>` y `/login <clave maestra>`.
- Las claves se pueden dar también con `ECO_SERVER_PASS`, `ECO_ADMIN_PASS` y `ECO_MOD_PASS`. En el cliente: `ECO_CLAVE`, `ECO_PASS` y `ECO_E2E`.
- Quien entra con la clave de `--admin` (el dueño) es el único que puede actuar sobre **otro admin**.
- Si borras `eco.db` el servidor estrena identidad y los clientes verán el aviso de seguridad (se acepta con `--confiar`).
- En Linux, `dist/linux/install.sh` lo instala como servicio de systemd.

## Compilar

### Todo de una vez (Windows)
Con [MSYS2](https://www.msys2.org) MinGW-w64 en `C:\msys64\mingw64`, CMake y Ninja (Docker opcional, para los servidores de Linux):
```powershell
powershell -ExecutionPolicy Bypass -File build.ps1           # o -NoLinux si Docker no está en marcha
```
Compila fuera del repositorio (`%LOCALAPPDATA%\eco-build`), pasa las pruebas (si fallan no copia nada), copia los ejecutables a `bin/` y `dist/` (sin tocar `eco.db`), publica tu actualización firmada en `dist/updates` si tienes clave y escribe `dist/SHA256SUMS.txt`. `eco-sign.exe` queda en `bin/windows/` y las pruebas en `bin/windows/tests/`.

### Windows (a mano)
Necesitas **CMake** y **Visual Studio 2022** (o MinGW). Opus, PortAudio, Asio, RNNoise, Dear ImGui y GLFW se descargan solos; SQLite, Monocypher y stb ya vienen en `third_party/`.
```bat
cmake -S . -B build
cmake --build build --config Release
```

### Linux
```bash
sudo apt install cmake g++ git libasio-dev libopus-dev portaudio19-dev libx11-dev \
                 libgl-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev   # las 5 últimas, para eco-gui
cmake -S . -B build && cmake --build build -j
```
Solo el servidor (por ejemplo en un VPS): `-DECO_SERVER_ONLY=ON`, que solo necesita `libasio-dev`. Sin la interfaz gráfica: `-DECO_GUI=OFF`.

### macOS
```bash
brew install cmake asio opus portaudio glfw pkg-config
cmake -S . -B build && cmake --build build -j
open build/eco-gui.app
```
Pulsar-para-hablar pide el permiso de «Monitorización de entrada». **Aún no se puede compartir la pantalla desde un Mac** (falta ScreenCaptureKit); ver la de los demás sí funciona.

### Pruebas
```bash
build/eco-test-crypto                       # Double Ratchet, claves de emisor, firmas, anti-repetición, invitaciones
eco-server --puerto 19988 --copias 0 &      # y luego:
build/eco-test-flood 19988                  # frenos del servidor contra inundaciones
build/eco-test-fuzz 19988 60                # datos aleatorios y mal formados contra el servidor
build/eco-test-fuzz-client 19990 45         # un servidor malicioso (conéctale un eco-client con --confiar)
build/eco-test-load 19988 30 10             # 30 clientes hablando a la vez
python tools/check_translations.py          # todos los textos tienen su traducción al español
```
Para diagnosticar: `ECO_SIN_UDP=1` hace como si la red bloqueara UDP y `ECO_CAPTURA_GDI=1` fuerza la captura de pantalla antigua (GDI).

### Tu propia clave de firma

Las actualizaciones automáticas solo se aceptan si van firmadas con la clave que llevan compilada los clientes. El repositorio no trae ninguna: compilado sin clave, Eco simplemente tiene las actualizaciones automáticas apagadas. Para tener la tuya:
```bash
eco-sign new-key local_data_DO_NOT_DISTRIBUTE/update_signing_key.txt
```
Eso escribe tu clave privada y, a su lado, `update_public_key.txt`. CMake toma de ahí la pública (o de `-DECO_UPDATE_PUBLIC_KEY=<64 hex>`) y la mete en los clientes que compiles. Para publicar una versión: sube `project(Eco VERSION x.y.z)` en `CMakeLists.txt`, ejecuta `build.ps1` (o `eco-sign publish <clave> <versión> windows-x64 <carpeta> eco-gui.exe eco-client.exe`) y arranca el servidor con `--updates <carpeta>`. **No repartas ni subas nunca la clave privada**; `local_data_DO_NOT_DISTRIBUTE/` está en `.gitignore`.

## Arquitectura

Ver la sección *Architecture* de [README.md](README.md): el código, los comentarios y los nombres están en inglés, y los textos de la interfaz se traducen al español con `common/i18n_es.inc`.

## Contribuir y licencia

Las contribuciones son bienvenidas: ver [CONTRIBUTING.md](CONTRIBUTING.md). Eco se publica con la [licencia MIT](LICENSE); las librerías de terceros conservan las suyas, todas compatibles ([THIRD_PARTY.md](THIRD_PARTY.md)).

Desarrollado por [pancito2702](https://github.com/pancito2702), con Claude Code (Anthropic) como asistente de programación.
