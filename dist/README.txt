ECO - files to install                                  (en español más abajo)
======================

windows/   (64-bit Windows 10/11)
  eco-gui.exe      client with a window (the recommended one)
  eco-client.exe   console client (optional)
  eco-server.exe   server, in case you want to host it on a Windows PC
  No installation or DLLs needed: just copy them to any folder and open them.
  Windows may show SmartScreen ("More info -> Run anyway").

linux/     (the server; works on any Linux: Ubuntu, Debian, Fedora, Alpine...)
  eco-server       static binary for regular PCs/VPSs (x86_64)
  eco-server-arm64 static binary for 64-bit ARM (Raspberry Pi 4/5, Oracle Ampere...)
  install.sh       installs the right one as a systemd service (also works to update)

Installing the server on Linux:
  1. Copy the linux/ folder to the server, e.g.:  scp -r linux user@my-server:~/eco
  2. On the server:  cd ~/eco && sudo sh install.sh
     (to update: copy the new folder and run it again; data and options are kept)
  3. Put your passwords in /etc/default/eco-server, e.g.:
        ECO_ADMIN_PASS="MyAdminPassword"
        ECO_MOD_PASS="MyModPassword"
     and restart:  sudo systemctl restart eco-server
  4. Open port 9987 TCP and UDP (the script does it on ufw/firewalld; in your VPS
     provider's panel you'll have to do it yourself).
  5. See the invitation with  journalctl -u eco-server -n 30
     (the line starting with eco://; replace TU-IP-PUBLICA with the VPS IP or domain)
     and give it to your friends: they paste it into Eco and that's it.

For your friends (Windows): they only need eco-gui.exe. They open it, press "Paste" with the
copied invitation, type their name and "Connect".

The data (accounts, channels, history) stays in /var/lib/eco/eco.db.
Try it by hand without installing:  chmod +x eco-server && ./eco-server --admin MyPassword

-------------------------------------------------------------------------------------------

ECO - archivos para instalar

windows/   (Windows 10/11 de 64 bits)
  eco-gui.exe      cliente con ventana (el recomendado)
  eco-client.exe   cliente de consola (opcional)
  eco-server.exe   servidor, por si quieres alojarlo en un PC con Windows
  No necesitan instalación ni DLLs: basta copiarlos a cualquier carpeta y abrirlos.
  Windows puede mostrar SmartScreen («Más información -> Ejecutar de todas formas»).

linux/     (el servidor; vale para cualquier Linux: Ubuntu, Debian, Fedora, Alpine...)
  eco-server       binario estático para PC/VPS normales (x86_64)
  eco-server-arm64 binario estático para ARM de 64 bits (Raspberry Pi 4/5, Oracle Ampere...)
  install.sh       instala el que corresponda como servicio de systemd (también sirve para actualizar)

Instalar el servidor en Linux:
  1. Copia la carpeta linux/ al servidor, p. ej.:  scp -r linux usuario@mi-servidor:~/eco
  2. En el servidor:  cd ~/eco && sudo sh install.sh
     (para actualizar: copia la carpeta nueva y vuelve a ejecutarlo; conserva datos y opciones)
  3. Pon tus claves en /etc/default/eco-server, por ejemplo:
        ECO_ADMIN_PASS="MiClaveAdmin"
        ECO_MOD_PASS="MiClaveMod"
     (mejor así que dentro de ECO_ARGS: en ECO_ARGS cualquier usuario del VPS las vería con "ps")
     y reinicia:  sudo systemctl restart eco-server
     Para el registro en español: ECO_LANG="es".
  4. Abre el puerto 9987 TCP y UDP (el script lo hace en ufw/firewalld; en el panel
     de tu proveedor de VPS tendrás que hacerlo tú).
  5. Mira la invitación con  journalctl -u eco-server -n 30
     (la línea que empieza por eco://; cambia TU-IP-PUBLICA por la IP o el dominio del VPS)
     y pásasela a tus amigos: la pegan en Eco y listo. La invitación lleva la huella del
     servidor, así que Eco comprueba solo que se conectan al auténtico.

Para tus amigos (Windows): solo necesitan eco-gui.exe. Lo abren, pulsan "Pegar" con la
invitación copiada, escriben su nombre y "Conectar". La primera vez sale un asistente.

Los datos (cuentas, canales, historial) quedan en /var/lib/eco/eco.db.
Para llevarte la base de datos de tu PC al servidor: para el servicio, copia eco.db
(con eco.db-wal si existe) a /var/lib/eco/, haz  sudo chown eco /var/lib/eco/eco.db*
y arráncalo. Se conserva la misma identidad, así que los clientes no verán aviso.

Probar a mano sin instalar:  chmod +x eco-server && ./eco-server --admin MiClave
