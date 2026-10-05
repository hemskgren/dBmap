#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ENV_FILE="$ROOT/.env"
LAN_IP="${1:-}"

if [[ -z "$LAN_IP" || ! "$LAN_IP" =~ ^([0-9]{1,3}\.){3}[0-9]{1,3}$ ]]; then
  echo "Usage: $0 <hub-lan-ipv4>" >&2
  exit 2
fi
if [[ -e "$ENV_FILE" ]]; then
  echo "$ENV_FILE already exists; refusing to overwrite it." >&2
  exit 1
fi

umask 077
cat > "$ENV_FILE" <<EOF
DBMAP_LAN_IP=$LAN_IP
DBMAP_ADMIN_TOKEN=$(openssl rand -hex 32)
DBMAP_MQTT_HUB_PASSWORD=$(openssl rand -hex 32)
DBMAP_DYNSEC_ADMIN_PASSWORD=$(openssl rand -hex 32)
DBMAP_ADVERTISED_MQTT_HOST=hub.local
DBMAP_ADVERTISED_MQTT_PORT=8883
EOF
chmod 0600 "$ENV_FILE"
echo "Wrote generated secrets to $ENV_FILE (mode 0600). Keep this file private and backed up."
