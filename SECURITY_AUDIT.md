# Auditoría de seguridad de Eco

Fecha: 16 de noviembre de 2026 (cuarta revisión: 17 de noviembre). Alcance: todo el código propio (`common/`, `server/`, `client/`, `tests/`,
`CMakeLists.txt`, `dist/linux/install.sh`), unas 9 500 líneas, leídas enteras. Las librerías de terceros
(Monocypher, SQLite, stb, Opus, PortAudio, RNNoise, Asio, Dear ImGui, GLFW, FreeType) no se han auditado por
dentro: solo cómo las usa Eco.

Es una revisión interna de código hecha con el fuente delante. No sustituye a una auditoría externa ni a unas
pruebas de penetración.

## Resumen

No se encontró ningún fallo que permita leer mensajes, voz o pantalla, entrar en una cuenta ajena, ejecutar
código o corromper memoria. El diseño criptográfico es sólido y está bien aplicado.

Lo que sí había: tres debilidades de gravedad media en el servidor (un archivo sensible legible por otros
usuarios del sistema y dos formas de dejarlo sin servicio) y varias de gravedad baja en el cliente. Todas están
corregidas. Quedan riesgos conocidos que son decisiones de diseño o exigen un cambio de protocolo; están en la
última sección.

| Gravedad | Encontrados | Corregidos | Pendientes |
|---|---|---|---|
| Crítica / alta | 0 | – | – |
| Media | 3 | 3 | 0 |
| Baja | 8 | 8 | 0 (1 añadido en la cuarta revisión) |
| Riesgos aceptados o de diseño | 11 | – | 11 (documentados; 2 añadidos en la segunda revisión) |
| Para que lo decidas tú | 1 | – | 1 (`dist/windows/eco.db`) |

## Cuarta revisión (versión 2.3.0): ataque con datos aleatorios y sanitizadores

Las revisiones anteriores leyeron el código y probaron casos concretos. Esta vez se atacó a los programas en marcha
con datos aleatorios y mal formados ("fuzzing"), compilados con AddressSanitizer y UndefinedBehaviorSanitizer
(cualquier lectura o escritura fuera de memoria, uso tras liberar o comportamiento indefinido detiene el programa
y lo apunta). Las pruebas quedan en el repositorio y se pueden repetir.

**Contra el servidor (`tests/test_fuzz_server.cpp`).** Muchos clientes completan el intercambio de claves y
mandan mensajes cifrados válidos con contenido aleatorio o con forma engañosa: cadenas con un largo que miente,
bloques de bytes con tamaños enormes, UTF-8 roto, caracteres de control y bidi, comandos reales con datos raros,
mensajes de tipos que solo manda el servidor, antes y después de identificarse, y paquetes UDP basura o cifrados
con contenido raro. Un vigía comprueba cada medio segundo que el servidor sigue contestando.
- *Resultado (Linux, con ASan y UBSan, 60 s):* 1 523 221 mensajes TCP, 381 752 paquetes UDP y 45 521 conexiones.
  Ningún error de los sanitizadores, ningún ping sin respuesta a tiempo y se puede seguir entrando al acabar.
  En Windows (compilación normal), lo mismo: 941 412 mensajes y 43 049 conexiones sin un fallo.

**Contra el cliente (`tests/test_fuzz_client.cpp`).** Un servidor malicioso hace el saludo de verdad y luego manda
al cliente miles de mensajes de todos los tipos (listas de usuarios inventadas, fotogramas, claves, historial,
voz por TCP...) y paquetes UDP raros.
- *Resultado (Linux, con ASan y UBSan, 45 s):* 63 819 mensajes TCP y 168 160 paquetes UDP. El cliente sigue
  conectado, sin errores de los sanitizadores, y no se colaron secuencias de terminal (ESC), marcas bidi ni
  caracteres de control en lo que muestra.

**Corregido**
- **F1 (baja). El registro del servidor podía llenar el disco.** Con miles de conexiones por minuto (que los
  frenos sí dejan, porque vienen de muchas IP), cada entrada y salida escribía una línea: 3,3 MB en un minuto de
  ataque. *Arreglo:* entradas, salidas y contraseñas fallidas se apuntan como mucho 60 por minuto; el resto se
  resume en una línea ("... y N más en el último minuto").

**Para quien repita las pruebas.** El AddressSanitizer de GCC 12 se queda en un bucle escribiendo
`AddressSanitizer:DEADLYSIGNAL` con los kernels de Linux recientes (más entropía en la aleatorización de
direcciones). No es un fallo de Eco: hay que ejecutar las pruebas sin ASLR (`setarch $(uname -m) -R`; en Docker,
con `--security-opt seccomp=unconfined`).

**Las actualizaciones, ahora con la clave de cada uno.** La clave pública de las actualizaciones ya no va en el
código: cada uno que compila Eco usa la suya (`eco-sign new-key`). Así, quien compila desde el repositorio público
no acepta actualizaciones firmadas por otro, y sin clave las actualizaciones automáticas quedan apagadas.

## Tercera revisión (versión 2.2.0): lo nuevo de esta versión

Alcance: todo lo que añade la 2.2.0 y que se puede atacar desde fuera. Eso es: las actualizaciones (lo que sirve
el servidor y lo que acepta el cliente), la voz por TCP, el Ping/Pong, las sanciones guardadas, las copias de
seguridad, la captura de ventanas y la elección de aplicación para el audio, los informes de cierre y el
contador de los fotogramas.

**Corregido antes de publicar**
- **U1 (media). Nombres de dispositivo de Windows en las actualizaciones.** El servidor servía cualquier archivo
  con nombre "limpio" de la carpeta de actualizaciones. En Windows, `CON` o `NUL.txt` no son archivos sino
  dispositivos: pedir `CON` habría dejado el único hilo del servidor esperando a la consola, y con él la voz de
  todos. *Arreglo:* el servidor solo sirve archivos que nombra el manifiesto publicado y rechaza los nombres
  reservados (`CON`, `PRN`, `AUX`, `NUL`, `COM1-9`, `LPT1-9`).
- **U2 (baja). Ping con respuesta sin freno.** El Ping nuevo, que pide un Pong para medir el ping por TCP, no
  pasaba por el límite de mensajes. *Arreglo:* solo el Ping vacío de siempre queda exento.

**Revisado y bien**
- *Actualizaciones.* El cliente comprueba la firma EdDSA del manifiesto con una clave fija que va compilada
  dentro, que la plataforma es la suya y que la versión es estrictamente más nueva. Así no se puede volver a una
  versión anterior. Comprueba también el tamaño y el BLAKE2b-256 de cada archivo antes de tocar nada. Los nombres
  no llevan rutas, y si algo falla a medias se deja todo como estaba. *Probado:* una actualización buena se
  instala, un archivo manipulado en el servidor se rechaza y un manifiesto alterado se ignora.
- *Voz por TCP.* Pasa por el mismo freno que la UDP y por la misma comprobación de extremo a extremo en quien la
  recibe. Si quien la recibe va atrasado, se descarta en vez de acumular retraso.
- *Copias de seguridad.* `VACUUM INTO` con la ruta como parámetro, no pegada al SQL. En Linux, la carpeta va con
  permisos 0700 y las copias con 0600.
- *Contador de fotogramas.* Va dentro de lo firmado. Quitarlo invalida la firma, porque la marca va en los datos
  autenticados.

**Riesgos nuevos, aceptados**
- Quien publica Eco tiene una clave privada (`local_data_DO_NOT_DISTRIBUTE/update_signing_key.txt`): si se
  filtra, se pueden publicar versiones que los clientes aceptarán. Si se pierde, habrá que repartir a mano un
  cliente con otra clave.
- Un servidor puede **no ofrecer** una actualización (retener a sus clientes en una versión), pero nunca colar
  otra.
- El silencio de un invitado se guarda por su IP (no tiene cuenta): afecta a otros invitados que entren desde la
  misma red, como en una casa o una oficina. Los que tienen cuenta no se ven afectados.
- `eco-client` y `eco-gui` importan ahora también `d3d11`, `dxgi` y `dwmapi` (captura de pantalla): se suman
  a la lista de DLL importadas del punto 10 de los riesgos.
- Los informes de cierre llevan el nombre de los módulos y desplazamientos dentro de ellos, nunca rutas con el
  nombre de usuario ni contenido de la memoria.

## Segunda revisión (16 nov 2026, tarde): audio de pantalla, inundaciones y rendimiento

Alcance: el código nuevo del audio al compartir pantalla (`client/loopback.*`, cambios en `audio.*`,
`client.cpp`, `gui.cpp`, `protocol.hpp` y `server.cpp`) y otra lectura de todo lo que un atacante puede
mandar al servidor (TCP y UDP) o al cliente (lo que llega de la red y las DLL que se cargan). Ningún fallo
permite leer, suplantar ni ejecutar código. Se encontraron dos formas de saturar el servidor que la primera
revisión no vio y tres puntos flojos del código nuevo. Todo está corregido y comprobado.

| Gravedad | Encontrados | Corregidos |
|---|---|---|
| Media | 2 | 2 |
| Baja | 4 | 4 |
| Rendimiento | 3 | 3 (+1 recomendación) |

**M4. La voz por UDP no tenía límite de ritmo.** Cada paquete válido se reenvía a todo el canal. Un miembro con
un cliente modificado podía mandar miles por segundo: CPU del servidor (que cifra una copia por oyente, en el
mismo hilo que la voz de todos) y la conexión de cada miembro multiplicadas. *Arreglo:* cubo de fichas por
usuario, de 300 paquetes de ráfaga y 150/s sostenidos (la voz usa 50/s y el audio de pantalla 17/s). Se
descuenta después de autenticar el paquete; si no, cualquiera podría gastar las fichas de otro falsificando su
id. *Comprobado:* de 3 000 paquetes seguidos, el servidor anterior reenviaba los 3 000 y el nuevo 303.

**M5. Fotogramas de pantalla: solo se limitaban los bytes, no la cantidad.** Con 6 MB/s permitidos, miles de
fotogramas de pocos bytes por segundo pasaban el freno y cada uno se reenviaba y se cifraba para cada visor.
*Arreglo:* cada fotograma cuenta como mínimo 16 KB (como mucho ~380 por segundo; un uso real está muy por
debajo). *Comprobado:* de 5 000 mini-fotogramas, el anterior reenviaba 5 000 y el nuevo 529 (la ráfaga).

**B8. El audio de pantalla se podía repetir (diseño inicial).** Iba firmado, pero el servidor podía volver a
mandar paquetes antiguos dentro de la misma época de claves. *Arreglo:* cada paquete lleva un contador de 64
bits que nunca se repite, dentro de lo firmado, y quien mira descarta lo repetido o demasiado antiguo
(ventana de 64).

**B9. Tamaño de los paquetes de audio de pantalla sin tope en el receptor.** Un emisor malicioso podía hacer
que cada visor guardase paquetes de megas en el colchón. *Arreglo:* el receptor descarta todo lo que pase de
1 275 bytes (el máximo de Opus) y el servidor no reenvía nada por encima de 1 300.

**B10. Carga de DLL desde la carpeta del programa.** PortAudio abre `avrt.dll` en marcha y el código nuevo abría
`mmdevapi.dll` por su nombre. Windows busca primero junto al `.exe`, así que una DLL con ese nombre dejada
en la misma carpeta (Descargas, por ejemplo) se ejecutaría dentro de Eco. *Arreglo:* los tres programas
llaman a `SetDefaultDllDirectories(System32)` lo primero, y `mmdevapi.dll` se carga explícitamente desde
System32. Queda un resto, en «Riesgos que quedan» (punto 10).

**B11. Audio y fotogramas compartían el mismo tipo de dato cifrado.** No era explotable porque el bit
`kScreenAudio` va en los datos autenticados. Aun así, ahora el cliente rechaza de forma explícita un audio que
llegue como fotograma y al revés.

**Rendimiento**
- *Audio de pantalla por UDP, no por TCP.* Por TCP esperaba detrás de cada fotograma clave grande (cientos de
  KB) y se cortaba. Ahora va como la voz: de 3 tramas en 3 (60 ms), con una firma por paquete en vez de una por
  trama (17 firmas/s en lugar de 50) y unos 120 kbps en la red en lugar de ~150. Solo se captura y se manda si
  está activado y alguien está mirando.
- *Decodificador de pantalla sin copias:* cada tesela JPEG se lee dentro del propio mensaje en vez de copiarla
  a un búfer nuevo (hasta 600 reservas de memoria menos por fotograma clave a 4K).
- *Se arregló un posible bloqueo:* al dejar de compartir y volver a compartir enseguida, el hilo anterior
  podía no terminar nunca y la interfaz quedarse esperándolo.
- *Recomendación (no hecha):* la captura con GDI (`BitBlt`) es lo que más CPU gasta al compartir a muchos fps
  o en 4K. Pasar a *DXGI Desktop Duplication* la bajaría mucho, pero es un cambio grande del capturador.

**Sobre el audio de pantalla (privacidad).** Viene apagado. Cuando se activa se captura todo lo que suena en
el PC, avisos y otras llamadas incluidos, salvo el propio Eco (en Windows 10 2004 o posterior; en versiones
anteriores también Eco, y se avisa). Solo lo reciben quienes ven tu pantalla, cifrado de extremo a extremo y
firmado.

**Cómo se comprobó:** compilado sin avisos en el código propio; las 132 pruebas de cifrado correctas;
prueba de extremo a extremo con servidor local y dos clientes (547 tramas de audio de pantalla enviadas y
549 recibidas al medir un instante después, voz 840/840 a la vez); un cliente de prueba que habla el
protocolo directamente, para las inundaciones (cifras de arriba) y para comprobar que el audio sin la marca no
se reenvía; 20 000 fotogramas aleatorios o corruptos contra el decodificador con comprobación de límites;
arranque de `eco-gui` y del audio con la búsqueda de DLL restringida.

## Pendiente de tu decisión

**`dist/windows/eco.db` está dentro de la carpeta que se reparte.** Esa base de datos (con sus `-wal` y `-shm`,
del 15 de noviembre) contiene la clave privada de identidad de un servidor, cuentas e historial. Si comprimes
`dist/windows/` tal cual y la pasas a alguien, le das esa identidad: podría hacerse pasar por ese servidor ante
clientes que ya lo conocen. No la he tocado porque son datos tuyos y puede ser el servidor que usas. Antes de
repartir la carpeta, muévela a `local_data_DO_NOT_DISTRIBUTE/` (o bórrala si era de pruebas). El `.gitignore` ya
la excluye del repositorio, pero no de un zip hecho a mano.

## Corregido

### Gravedad media

**M1. `eco.db` legible por otros usuarios del sistema (Linux y macOS).**
El servidor creaba la base de datos con los permisos por defecto del proceso (normalmente 0644). Dentro están la
semilla de la identidad Ed25519 del servidor, en claro, y los hashes Argon2id de las contraseñas. Cualquier otro
usuario local del VPS podía copiarla y suplantar al servidor o atacar los hashes sin conexión. Con `install.sh`
no ocurría (carpeta 0700 y `UMask=0077`); sí al arrancarlo a mano, que es lo que propone el README para probar.
*Arreglo:* el servidor fija `umask(077)` antes de abrir la base de datos y pone 0600 a `eco.db`, `-wal` y `-shm`
si ya existían (`server/server.cpp`, `serverMain`). En Windows no aplica: el archivo hereda los permisos de la
carpeta.

**M2. Sin límite al ritmo de conexiones: una sola IP podía acaparar el servidor.**
Cada conexión nueva obliga al servidor a un X25519 y una firma EdDSA, en el mismo y único hilo que reenvía la
voz. Había tope de 16 conexiones simultáneas por IP, pero no de conexiones por segundo: abrir y cerrar en bucle
bastaba para entrecortar la voz de todos.
*Arreglo:* cubo de fichas por IP (por /64 en IPv6): ráfaga de 20 y después una conexión cada 2 s. Lo que pasa
del límite se corta antes del intercambio de claves. Comprobado contra un servidor real: de 40 conexiones
seguidas se atienden 20.

**M3. Agotamiento de descriptores y bucle en caliente al aceptar.**
No había tope global de conexiones sin identificar (solo 16 por IP, con IPs ilimitadas), y si el sistema se
quedaba sin descriptores `accept()` fallaba y se reintentaba al instante, sin pausa: 100 % de CPU hasta que se
liberase alguno.
*Arreglo:* tope global (plazas de usuarios + 256 pendientes); al llenarse se cierra la conexión más antigua que
siga sin identificarse, de modo que quien entra de verdad no se queda fuera. Si `accept()` falla, se espera
200 ms antes de reintentar.

### Gravedad baja

**B1. Un servidor malicioso podía estropear el archivo de contactos del cliente.**
`eco_contactos` guarda `servidor nick clave` separados por espacios y se leía de corrido. El nick lo dicta el
servidor; uno con espacios o saltos de línea descolocaba la lectura de todo lo que venía después. Efecto: al
reiniciar se «olvidaban» contactos de *otros* servidores y dejaba de salir el aviso de «su clave cambió» para
ellos. Los contactos verificados no se veían afectados.
*Arreglo:* lectura línea a línea (una línea rota se ignora sola) y no se guardan nombres con espacios o
caracteres de control (`client/keys.cpp`).

**B2. Reutilización de una clave de mensaje tras un fallo de escritura (privados).**
El Double Ratchet usa cada clave de mensaje una vez, con nonce fijo. Si guardar el estado en disco fallaba
(disco lleno, permisos) el mensaje salía igualmente; tras un reinicio la cadena volvía atrás y la misma clave
cifraba otro mensaje distinto, lo que con XChaCha20-Poly1305 permite relacionar los dos textos. Hacían falta un
fallo de disco y un reinicio, pero el efecto es serio.
*Arreglo:* si el estado no se puede guardar, el privado no se envía y se avisa.

**B3. `/ptt f99999999999` cerraba el cliente en Windows.**
El número de la tecla de función se convertía con `std::stoi` dentro del hilo de pulsar-para-hablar; un número
enorme lanzaba una excepción sin capturar. Solo te lo podías hacer a ti mismo (o un `eco_gui.txt` manipulado).
*Arreglo:* conversión sin excepciones y rango F1–F24.

**B4. `eco-gui` en Linux y macOS guardaba sus ajustes en la carpeta desde la que se abría.**
El archivo puede llevar la clave del servidor y debía estar en `~/.eco_gui` (como dice el README); iba a
`./eco_gui.txt`, que puede ser una carpeta compartida.
*Arreglo:* usa la misma ruta que el resto de archivos del cliente. En Windows no cambia nada.

**B5. Texto cifrado demasiado largo: se recortaba en vez de rechazarse.**
Recortar un texto cifrado lo deja ilegible para todos y se guardaba así en el historial.
*Arreglo:* se rechaza con un error. El cliente ya limita a 1 200 caracteres, así que no afecta al uso normal.

**B6. Opciones sin validar.** `--max -1` daba un límite gigantesco y `--puerto 70000` se truncaba a otro puerto;
igual con `[ipv6]:puerto` en el cliente. *Arreglo:* rangos comprobados. Además el servidor avisa al arrancar si
alguna clave (`--admin`, `--mod`, `--pass`) tiene menos de 8 caracteres.

**B7. Memoria retenida por sesión.** Tras recibir un fotograma grande, cada sesión conservaba un búfer de hasta
8 MB para siempre. *Arreglo:* se libera cuando el usuario deja de compartir pantalla.

### Endurecimiento añadido

- Comprobación de límites en `vector`, `string` y `array` del código propio (`_GLIBCXX_ASSERTIONS`): un índice
  fuera de rango cierra el programa en vez de leer o escribir memoria ajena. Coste inapreciable.
- Ejecutables de posición independiente (PIE) también al compilar en Linux con CMake, para que ASLR mueva el
  código.
- Las pruebas del cifrado (`tests/test_crypto.cpp`) ahora se compilan desde CMake con `-DECO_TESTS=ON`.

## Lo que se revisó y está bien

- **Saludo cliente-servidor.** X25519 efímero por conexión, firma Ed25519 sobre etiqueta + versión + las dos
  claves efímeras (ata la firma a la conexión y a la versión: no hay degradación), rechazo de claves de orden
  bajo, cuatro claves separadas por uso y sentido, identidad del servidor recordada o fijada por la invitación.
- **Nonces.** TCP: contador implícito por sentido con claves distintas. UDP: contador explícito autenticado y
  ventana anti-repetición de 64. Voz E2E: identificador de clave + contador, con clave nueva en cada rotación.
  Texto y pantalla: nonce aleatorio de 192 bits. No hay reutilización de nonce con la misma clave (salvo el caso
  B2, ya corregido).
- **Double Ratchet.** Cabecera autenticada, trabajo sobre una copia del estado (un mensaje falso no lo altera),
  límites de claves saltadas, sesiones simultáneas, estado cifrado en disco con clave derivada de la identidad.
- **Claves de emisor.** Envoltorio cifrado con la clave entre identidades y atado a emisor y destinatario; firma
  por mensaje; se comprueba que la clave es de quien el servidor dice; anti-repetición en vivo; rotación al
  entrar o salir alguien.
- **Contraseñas.** Argon2id (19 MiB, 2 pasadas) con sal por cuenta, fuera del hilo principal, cola limitada en
  total y por IP, comparación en tiempo constante, límite de fallos por IP y por cuenta, memoria borrada.
- **Base de datos.** Todas las consultas son sentencias preparadas con parámetros: no hay inyección SQL.
- **Lectura de mensajes.** `Reader` comprueba la longitud antes de cada lectura; tamaños máximos antes y después
  de identificarse; una MAC inválida corta la conexión. Probado con datos basura y MAC falsa contra un servidor
  real.
- **Decodificador de pantalla.** Dimensiones, número de teselas y tamaño declarado del JPEG se limitan antes de
  reservar memoria o decodificar. Probado con 20 000 entradas aleatorias.
- **Lo que se muestra.** Todo el texto ajeno pasa por un filtro que quita secuencias de terminal, caracteres C1
  y marcas bidi; los nicks y canales se validan en el servidor (sin invisibles ni mezcla de alfabetos); no hay
  cadenas de formato controladas por terceros.
- **Permisos.** Un mod no puede con otro mod ni con un admin; solo el dueño puede con otro admin; clave maestra
  de un solo uso.
- **Secretos en disco (cliente).** Identidad con DPAPI en Windows y 0600 en Linux/macOS; llavero, sesiones y
  copia de privados cifrados con claves derivadas de la identidad; escrituras atómicas.

## Riesgos que quedan (aceptados o de diseño)

1. **El servidor ve los metadatos**: nicks, canales, quién habla y cuándo, y tamaños. Inherente al diseño.
2. **Primera conexión sin invitación**: se confía en la identidad que presente el servidor (como SSH). Con
   invitación queda comprobada.
3. **Sin verificar el código de seguridad, un servidor malicioso puede repartir claves falsas.** Es el límite de
   cualquier E2E; `/verificar` lo cierra.
4. **La voz no va firmada** (sí cifrada y atada a la clave de quien habla): otro miembro del mismo canal, con un
   cliente modificado y la ayuda del servidor, podría inyectar voz atribuida a otro. Texto y pantalla sí van
   firmados.
5. **Modo de frase compartida (`/e2e`)**: todos comparten la misma clave, así que no hay autenticación entre
   miembros ni secreto hacia adelante, y su seguridad es la de la frase.
6. **Llavero indexado por un identificador de 64 bits**: un miembro del canal que vea el identificador de la
   clave de otra persona antes de que tú recibas esa clave puede registrarlo a su nombre y dejarte sin poder
   leer a esa persona hasta la siguiente rotación. Es una molestia (no leer), no una suplantación. Arreglarlo
   bien cambia el formato del llavero.
7. **Se puede saber si un nick está registrado** (el servidor pide contraseña) y pedir la clave pública de
   cualquier cuenta. Necesario para los privados sin conexión.
8. **Silenciar es por sesión y banear es por IP**: reconectar quita el silencio y otra IP salta el baneo.
9. **Quien tenga tu sesión de usuario del sistema** puede leer tus claves (DPAPI protege frente a otros
   usuarios y otros equipos, no frente a ti mismo), y puede modificar `eco_contactos` y `eco_verificados`, que no
   llevan firma.

10. **DLL importadas al arrancar** (`d3dcompiler_47`, `d3d11`, `winmm`, `crypt32`, `bcrypt`): Windows las
    resuelve antes de `main()`, así que el arreglo B10 no las cubre. Quien pueda escribir en la carpeta del
    `.exe` podría colocar una falsa. Repartid Eco en su propia carpeta (no suelto en Descargas). Cerrarlo del
    todo exige cargarlas en diferido, algo que el enlazador de MinGW no permite.
11. **Los fotogramas de pantalla se pueden repetir** dentro de la misma época de claves (un servidor malicioso
    podría volver a mandar imágenes viejas, nunca falsas). El audio ya lleva contador; ponerlo en los
    fotogramas cambia su formato y obliga a actualizar todos los clientes a la vez.

Otros apuntes: los ejecutables no están firmados (SmartScreen avisará); las claves pasadas por línea de comandos
se ven en la lista de procesos (hay variables de entorno para evitarlo); conviene actualizar de vez en cuando
las librerías incluidas en `third_party/`, sobre todo `stb_image`, que decodifica imágenes enviadas por otros
miembros del canal.

## Cómo se comprobó

- `eco-test-crypto`: 132 comprobaciones, 0 fallos (Double Ratchet, claves de emisor, firmas, anti-repetición,
  invitaciones, voz mientras se guardan privados).
- Prueba de extremo a extremo en local con el servidor y dos clientes de consola: conexión con invitación, chat
  de canal, privados en los dos sentidos, registro, `/login` de admin, crear canal y cambiarse, código de
  seguridad y UDP.
- Voz: un cliente envía el tono de prueba y el otro lo recibe y descifra (294 paquetes enviados, 342 recibidos
  al medir unos segundos después).
- Servidor: ráfaga de 40 conexiones (se atienden 20), recuperación tras la espera, basura en lugar de saludo y
  mensaje con MAC falsa (se corta la conexión en todos los casos).
- Códec de pantalla: ida y vuelta en cinco resoluciones, pantalla sin cambios, fotograma clave forzado, cambio
  parcial y cambio de resolución; la reducción nueva da el mismo resultado byte a byte que la anterior.

- Servidor de Linux (x86_64 y arm64, estáticos): los dos arrancan en un contenedor y crean `eco.db` con permisos
  0600; contra el de x86_64 se conectaron dos clientes de Windows (chat cifrado y UDP correctos).

No se probó a mano la ventana de `eco-gui` (compila y enlaza; su único cambio propio es la ruta del archivo de
ajustes), ni los clientes en Linux, ni nada en macOS.
