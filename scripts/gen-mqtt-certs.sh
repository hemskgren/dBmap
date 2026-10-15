#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ROTATE_CA=false
if [[ "${1:-}" == "--rotate-ca" ]]; then
  ROTATE_CA=true
  shift
fi
OUT="${1:-"$ROOT/mqtt/certs"}"
LAN_IP="${DBMAP_LAN_IP:-}"
PUBLIC_HUB_HOST="${DBMAP_PUBLIC_HUB_HOST:-}"
EXTERNAL_MQTT_HOST="${DBMAP_ADVERTISED_MQTT_HOST:-hub.local}"
if [[ -z "$LAN_IP" ]]; then
  echo "Set DBMAP_LAN_IP (the hub's LAN IPv4 address) before generating certificates." >&2
  exit 1
fi
if [[ ! "$LAN_IP" =~ ^([0-9]{1,3}\.){3}[0-9]{1,3}$ ]]; then
  echo "DBMAP_LAN_IP must be an IPv4 address." >&2
  exit 1
fi
mkdir -p "$OUT"

if [[ "$ROTATE_CA" == true ]]; then
  rm -f "$OUT/ca.crt" "$OUT/ca.key" "$OUT/ca.srl"
fi

if [[ -e "$OUT/ca.crt" && ! -e "$OUT/ca.key" ]] || [[ ! -e "$OUT/ca.crt" && -e "$OUT/ca.key" ]]; then
  echo "CA certificate/key pair is incomplete in $OUT; restore both or use --rotate-ca." >&2
  exit 1
fi
if [[ -e "$OUT/ca.crt" ]] && ! openssl x509 -in "$OUT/ca.crt" -noout -ext basicConstraints | grep -q 'CA:TRUE'; then
  echo "Existing CA is unsuitable for TLS verification; regenerate it with --rotate-ca." >&2
  exit 1
fi

if [[ ! -e "$OUT/ca.crt" ]]; then
  openssl req -x509 -newkey rsa:2048 -days 3650 -nodes \
    -keyout "$OUT/ca.key" \
    -out "$OUT/ca.crt" \
    -subj "/CN=dBmap-local-ca" \
    -addext "basicConstraints=critical,CA:TRUE" \
    -addext "keyUsage=critical,keyCertSign,cRLSign"
fi

SERVER_SAN="DNS:hub.local,DNS:mosquitto,DNS:localhost,IP:127.0.0.1,IP:${LAN_IP}"
for host in "$PUBLIC_HUB_HOST" "$EXTERNAL_MQTT_HOST"; do
  [[ -z "$host" || "$host" == "hub.local" ]] && continue
  if [[ "$host" =~ ^([0-9]{1,3}\.){3}[0-9]{1,3}$ ]]; then
    SERVER_SAN+=",IP:${host}"
  elif [[ "$host" =~ ^[A-Za-z0-9.-]+$ ]]; then
    SERVER_SAN+=",DNS:${host}"
  else
    echo "External Hub and MQTT hosts must be DNS names or IPv4 addresses." >&2
    exit 1
  fi
done

openssl req -newkey rsa:2048 -nodes \
  -keyout "$OUT/server.key" \
  -out "$OUT/server.csr" \
  -subj "/CN=${DBMAP_MQTT_CN:-hub.local}" \
  -addext "subjectAltName=${SERVER_SAN}" \
  -addext "basicConstraints=critical,CA:FALSE" \
  -addext "keyUsage=critical,digitalSignature,keyEncipherment" \
  -addext "extendedKeyUsage=serverAuth"

openssl x509 -req -in "$OUT/server.csr" -CA "$OUT/ca.crt" -CAkey "$OUT/ca.key" \
  -CAcreateserial -out "$OUT/server.crt" -days 825 -copy_extensions copy

openssl req -newkey rsa:2048 -nodes \
  -keyout "$OUT/web.key" \
  -out "$OUT/web.csr" \
  -subj "/CN=dbmap-web" \
  -addext "subjectAltName=DNS:dbmap-web" \
  -addext "basicConstraints=critical,CA:FALSE" \
  -addext "keyUsage=critical,digitalSignature,keyEncipherment" \
  -addext "extendedKeyUsage=serverAuth"

openssl x509 -req -in "$OUT/web.csr" -CA "$OUT/ca.crt" -CAkey "$OUT/ca.key" \
  -CAcreateserial -out "$OUT/web.crt" -days 825 -copy_extensions copy

openssl req -newkey rsa:2048 -nodes \
  -keyout "$OUT/opa.key" \
  -out "$OUT/opa.csr" \
  -subj "/CN=opa" \
  -addext "subjectAltName=DNS:opa" \
  -addext "basicConstraints=critical,CA:FALSE" \
  -addext "keyUsage=critical,digitalSignature,keyEncipherment" \
  -addext "extendedKeyUsage=serverAuth"

openssl x509 -req -in "$OUT/opa.csr" -CA "$OUT/ca.crt" -CAkey "$OUT/ca.key" \
  -CAcreateserial -out "$OUT/opa.crt" -days 825 -copy_extensions copy

rm -f "$OUT/server.csr" "$OUT/web.csr" "$OUT/opa.csr"
chmod 600 "$OUT/ca.key" "$OUT/server.key" "$OUT/web.key" "$OUT/opa.key"
echo "Wrote dBmap TLS material to $OUT"
