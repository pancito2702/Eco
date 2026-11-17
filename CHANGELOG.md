# Eco changelog / Historial de versiones de Eco

## 2.3.0 — 17 November 2026

Compatible with 2.0–2.2 (protocol 4): clients and servers of any 2.x version keep understanding each other.

**Languages**
- Eco speaks **English and Spanish**: clients, server console and log, help, commands and the first-time
  assistant. By default the system language; change it in *Settings > Idioma / Language*, with `/language es|en`,
  `--language` or `ECO_LANG`. Every command works with both names (`/kick` = `/expulsar`).
- Server messages to clients still travel in Spanish and each client translates them, so an English client also
  understands 2.x servers.

**Open source**
- MIT license, `SECURITY.md`, `CONTRIBUTING.md`, README in English (`README.md`) and Spanish (`README.es.md`),
  GitHub Actions (builds and tests on Linux and Windows).
- Code, comments, names, files and folders in English (`build.ps1`, `SECURITY_AUDIT.md`, `THIRD_PARTY.md`,
  `dist/updates`, `tools/sign.cpp`...). The old Spanish command-line options keep working.
- No update key in the code: whoever builds Eco makes their own with `eco-sign new-key` (the tool was
  `eco-firmar`); without a key, automatic updates are off. Manifests are now `manifest.txt` / `manifest.sig`
  (the server still reads the old names).

**Security and performance**
- New tests: random and malformed data against the server (`eco-test-fuzz`), a malicious server against the
  client (`eco-test-fuzz-client`) and a load test (`eco-test-load`); all also run with AddressSanitizer and UBSan.
- The server log writes at most 60 logins, logouts and failed passwords per minute: thousands of connections can
  no longer fill the disk.
- Screen sharing: scaling down and encoding the tiles use several cores.

## 2.3.0 — 17 de noviembre de 2026 (en español)

Compatible con 2.0, 2.1 y 2.2 (protocolo 4).

- **Español e inglés** en los clientes, la consola y el registro del servidor, la ayuda, los comandos y el
  asistente. Por defecto, el idioma del sistema; se cambia en *Ajustes > Idioma / Language*, con `/idioma es|en`,
  `--idioma` o `ECO_IDIOMA`. Los comandos valen con los dos nombres (`/expulsar` = `/kick`).
- **Código abierto:** licencia MIT, `SECURITY.md`, `CONTRIBUTING.md`, README en inglés y en español
  (`README.es.md`) e integración continua en GitHub. El código, los comentarios y los nombres de archivos y
  carpetas pasan a inglés (`compilar.ps1` es ahora `build.ps1`, `AUDITORIA.md` es `SECURITY_AUDIT.md`,
  `TERCEROS.md` es `THIRD_PARTY.md`, `dist/actualizaciones` es `dist/updates`, `eco-firmar` es `eco-sign`).
  Las opciones antiguas en español siguen funcionando.
- **Cada uno firma lo suyo:** la clave pública de las actualizaciones ya no va en el código. CMake la lee de
  `local_data_DO_NOT_DISTRIBUTE/update_public_key.txt` (o de `-DECO_UPDATE_PUBLIC_KEY`); sin ella, las
  actualizaciones automáticas quedan apagadas.
- **Pruebas nuevas** de datos aleatorios contra el servidor, de un servidor malicioso contra el cliente y de
  carga, también con AddressSanitizer y UBSan. El registro del servidor ya no puede llenar el disco con miles de
  conexiones (como mucho 60 líneas de entradas y salidas por minuto).
- **Rendimiento:** al compartir pantalla, la reducción y la codificación de las teselas usan varios núcleos.

---

La versión se cambia en un solo sitio: `project(Eco VERSION x.y.z)` en `CMakeLists.txt`. De ahí llega al
código, a las propiedades de los `.exe` en Windows y al `Info.plist` en macOS.

- **x** (mayor): cambios que obligan a actualizar cliente y servidor a la vez. Suele coincidir con subir el
  protocolo (`kProtoVersion` en `common/protocol.hpp`): un cliente y un servidor con protocolos distintos no
  se conectan.
- **y** (menor): funciones nuevas que siguen siendo compatibles con las versiones anteriores del mismo número
  mayor.
- **z** (corrección): arreglos sin funciones nuevas.

Para ver la versión: `eco-server --version`, `eco-client --version`, `/version` dentro del chat, o en la GUI
el menú **Eco** (abajo) y la pantalla de inicio. Al conectar, cliente y servidor se dicen su versión, y el
servidor la apunta en su registro.

## 2.2.0 — 16 de noviembre de 2026

Compatible con 2.0 y 2.1 (protocolo 4). Lo nuevo necesita cliente y servidor en 2.2; con versiones anteriores
todo lo demás sigue funcionando y Eco avisa de la diferencia al conectar.

**Más fácil de usar**
- Asistente de primera vez en `eco-gui`: nombre, micrófono y audífonos (con medidor para probar el micro y
  ajustar la sensibilidad) y servidor. Sale solo la primera vez; luego, desde la pantalla de inicio.
- Autocompletado en el chat: al escribir `/` sale la lista de comandos con lo que piden y lo que hacen (los de
  mod y admin solo si tienes el rol). Tab completa comandos, nicks y canales; otra vez Tab, la siguiente opción.

**Pantalla compartida**
- Elegir qué compartir: cada monitor o una ventana concreta (`/fuentes`, o el menú al pulsar *Compartir pantalla*).
- Captura con DXGI Desktop Duplication en Windows: unas 5 veces menos CPU que GDI (~3 ms frente a ~16 ms por
  fotograma a 1080p). Si no se puede (Escritorio remoto, por ejemplo), se usa GDI como antes.
- Audio de pantalla de una sola aplicación (Windows 10 2004+), para que no se cuelen avisos ni otras llamadas.
- Audio de pantalla también desde Linux (PulseAudio o PipeWire).
- Quien mira tiene un volumen propio para ese audio, y en la lista se ve quién comparte *pantalla + audio*.
- Los fotogramas llevan un contador anti-repetición (los clientes anteriores lo ignoran).

**Red**
- Voz por TCP cuando la red bloquea UDP: se detecta en unos 3 s y se vuelve a UDP en cuanto responde.
- Ping y voz perdida visibles en la barra de estado (y `/ping`).
- Reconexión automática en la ventana, con vuelta al mismo canal.
- Aviso al conectar si el servidor es de otra versión.

**Para publicar y mantener**
- Actualizaciones firmadas a través del propio servidor (`--actualizaciones`, `/actualizar`, botón *Instalar*),
  con la herramienta `eco-firmar` para publicarlas.
- Copias de seguridad diarias de `eco.db` (`--copias`, por defecto 7).
- Baneo por cuenta, también de cuentas desconectadas, y silencios que siguen al reconectar.
- Informes de cierre inesperado (solo en tu equipo, sin datos personales).
- Ventana *Acerca de Eco* con versión, privacidad y licencias (`TERCEROS.md`, con los textos completos).
- `compilar.ps1`: compila, prueba y copia todo, publica la actualización y escribe `dist/SHA256SUMS.txt`.
- Las pruebas se compilan siempre, y la prueba de inundación ya forma parte de ellas (`eco-test-flood`).

## 2.1.0 — 16 de noviembre de 2026

Compatible con 2.0 (protocolo 4): un cliente 2.1 entra en un servidor 2.0 y al revés. Lo nuevo de esta versión
necesita las dos partes en 2.1.

**Nuevo**
- **Audio del PC al compartir pantalla:** botón *Audio del PC: Sí/No* junto a *Compartir pantalla* (en consola,
  `/audiopantalla on|off`). Quienes miran tu pantalla oyen lo que suena en tu PC, sin las voces del canal. Va
  por UDP, cifrado de extremo a extremo y firmado. Viene apagado y por ahora solo se puede enviar desde
  Windows; se oye en todos los sistemas.
- La salida de audio pasa a estéreo cuando el dispositivo lo admite.
- Versión visible en los programas (`--version`, `/version`, la GUI y las propiedades del `.exe`), y cliente y
  servidor se dicen su versión al conectar.
- `/stats` muestra también el audio de pantalla enviado y recibido.

**Seguridad** (detalle en `AUDITORIA.md`, sección *Segunda revisión*)
- Límite de paquetes de voz por segundo y por usuario en el servidor. Antes, un cliente modificado podía hacer
  que se reenviaran miles por segundo a todo el canal.
- Límite de fotogramas de pantalla por segundo, no solo de bytes.
- Las DLL que se cargan en marcha se buscan solo en System32 (Windows).
- Audio de pantalla con contador anti-repetición y tamaño máximo por paquete.

**Rendimiento y arreglos**
- El decodificador de pantalla ya no copia cada tesela.
- Arreglado un posible bloqueo al dejar de compartir y volver a compartir enseguida.

## 2.0.0

Versión base: voz con Opus y supresión de ruido, chat y privados cifrados de extremo a extremo (Double Ratchet,
claves de emisor firmadas), pantalla compartida, cuentas y roles, invitaciones `eco://`, GUI para Windows,
Linux y macOS, y servidor para Windows y Linux (x86_64 y arm64). Primera auditoría de seguridad
(`AUDITORIA.md`).
