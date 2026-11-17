#!/bin/sh
# Installs eco-server as a systemd service (also works to update it: data and options are kept).
# Usage:  sudo sh install.sh
# Afterwards: edit /etc/default/eco-server (passwords, port...) and  sudo systemctl restart eco-server
set -e

if [ "$(id -u)" -ne 0 ]; then
    echo "Run it with sudo: sudo sh install.sh"
    exit 1
fi
cd "$(dirname "$0")"

# 1) Unprivileged user and data folder (eco.db lives here)
id eco >/dev/null 2>&1 || useradd --system --home-dir /var/lib/eco --shell /usr/sbin/nologin eco 2>/dev/null \
    || adduser -S -D -H -h /var/lib/eco -s /sbin/nologin eco
mkdir -p /var/lib/eco
chown eco /var/lib/eco
chmod 700 /var/lib/eco

# 2) Program: the binary that matches this processor
case "$(uname -m)" in
    x86_64|amd64) BIN=eco-server ;;
    aarch64|arm64) BIN=eco-server-arm64 ;;
    *) echo "Processor $(uname -m) not supported (there are binaries for x86_64 and arm64)"; exit 1 ;;
esac
[ -f "$BIN" ] || { echo "The file $BIN is missing next to this script"; exit 1; }
install -m 755 "$BIN" /usr/local/bin/eco-server

# 3) Options (not overwritten if they already exist)
if [ ! -f /etc/default/eco-server ]; then
    cat > /etc/default/eco-server <<'EOF'
# eco-server options (see README). After changing them: sudo systemctl restart eco-server
# Passwords: better here than inside ECO_ARGS, so they don't show up in the process list (ps)
ECO_ADMIN_PASS=""
ECO_MOD_PASS=""
ECO_SERVER_PASS=""
# Language of the log: en or es (by default, the system's)
ECO_LANG=""
# Other options. Example: ECO_ARGS="--retain 90 --max 32"
ECO_ARGS=""
EOF
    chmod 600 /etc/default/eco-server
fi

# 4) Service
cat > /etc/systemd/system/eco-server.service <<'EOF'
[Unit]
Description=Eco - encrypted voice and chat server
After=network-online.target
Wants=network-online.target

[Service]
User=eco
WorkingDirectory=/var/lib/eco
EnvironmentFile=-/etc/default/eco-server
ExecStart=/usr/local/bin/eco-server --db /var/lib/eco/eco.db $ECO_ARGS
Restart=on-failure
RestartSec=3
# Hardening: it can only write to its data folder
NoNewPrivileges=yes
ProtectSystem=strict
ProtectHome=yes
PrivateTmp=yes
PrivateDevices=yes
ReadWritePaths=/var/lib/eco
MemoryMax=256M
CapabilityBoundingSet=
AmbientCapabilities=
ProtectKernelTunables=yes
ProtectKernelModules=yes
ProtectKernelLogs=yes
ProtectControlGroups=yes
ProtectClock=yes
ProtectHostname=yes
RestrictAddressFamilies=AF_INET AF_INET6 AF_UNIX
RestrictNamespaces=yes
RestrictRealtime=yes
RestrictSUIDSGID=yes
LockPersonality=yes
MemoryDenyWriteExecute=yes
SystemCallArchitectures=native
SystemCallFilter=@system-service
UMask=0077

[Install]
WantedBy=multi-user.target
EOF

systemctl daemon-reload
systemctl enable eco-server
systemctl restart eco-server  # also when updating: that way it starts with the new binary right away

# 5) Firewall, if a known one is active
PORT=9987
if command -v ufw >/dev/null 2>&1 && ufw status | grep -q "Status: active"; then
    ufw allow ${PORT}/tcp && ufw allow ${PORT}/udp
elif command -v firewall-cmd >/dev/null 2>&1 && firewall-cmd --state >/dev/null 2>&1; then
    firewall-cmd --permanent --add-port=${PORT}/tcp --add-port=${PORT}/udp && firewall-cmd --reload
fi

sleep 1
echo
echo "Eco installed and running. Status:  systemctl status eco-server"
echo "Log (fingerprint and master password):  journalctl -u eco-server -n 20"
echo "If your provider has its own firewall (AWS, Oracle, etc.), open ${PORT} TCP and UDP there too."
echo
journalctl -u eco-server -n 10 --no-pager 2>/dev/null || true
