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
LOCAL_MQTT_HOST="${DBMAP_LOCAL_MQTT_HOST:-hub.local}"
ADVERTISED_MQTT_HOST="${DBMAP_ADVERTISED_MQTT_HOST:-hub.local}"
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

append_san() {
  local -n san_ref=$1
  local host=$2
  [[ -z "$host" ]] && return 0
  local entry
  if [[ "$host" =~ ^([0-9]{1,3}\.){3}[0-9]{1,3}$ ]]; then
    entry="IP:${host}"
  elif [[ "$host" =~ ^[A-Za-z0-9.-]+$ ]]; then
    entry="DNS:${host}"
  else
    echo "Certificate host '${host}' must be a DNS name or IPv4 address." >&2
    exit 1
  fi
  case ",${san_ref}," in
    *",${entry},"*) return 0 ;;
  esac
  san_ref="${san_ref:+${san_ref},}${entry}"
}

issue_cert() {
  local name=$1
  local san=$2
  local cn=${3:-$name}
  openssl req -newkey rsa:2048 -nodes \
    -keyout "$OUT/${name}.key" \
    -out "$OUT/${name}.csr" \
    -subj "/CN=${cn}" \
    -addext "subjectAltName=${san}" \
    -addext "basicConstraints=critical,CA:FALSE" \
    -addext "keyUsage=critical,digitalSignature,keyEncipherment" \
    -addext "extendedKeyUsage=serverAuth"
  openssl x509 -req -in "$OUT/${name}.csr" -CA "$OUT/ca.crt" -CAkey "$OUT/ca.key" \
    -CAcreateserial -out "$OUT/${name}.crt" -days 825 -copy_extensions copy
  openssl x509 -in "$OUT/${name}.crt" -noout -ext subjectAltName
  rm -f "$OUT/${name}.csr"
}

# External HTTPS. 127.0.0.1 is the in-container health check; hub.local is the mDNS name.
NGINX_SAN="DNS:hub.local,DNS:localhost,IP:127.0.0.1,IP:${LAN_IP}"
append_san NGINX_SAN "$PUBLIC_HUB_HOST"
append_san NGINX_SAN "$LOCAL_MQTT_HOST"
issue_cert nginx "$NGINX_SAN"

# Internal Hub API. Nginx verifies DNS:hub-bootstrap; the health check uses localhost.
issue_cert hub-bootstrap "DNS:hub-bootstrap,DNS:localhost"

# Broker. Internal clients use mosquitto; the entrypoint bootstrap uses 127.0.0.1.
MOSQUITTO_SAN="DNS:mosquitto,IP:127.0.0.1,IP:${LAN_IP}"
append_san MOSQUITTO_SAN "$LOCAL_MQTT_HOST"
append_san MOSQUITTO_SAN "$ADVERTISED_MQTT_HOST"
issue_cert mosquitto "$MOSQUITTO_SAN"

issue_cert web "DNS:dbmap-web" dbmap-web
issue_cert opa "DNS:opa"
issue_cert firmware-builder "DNS:firmware-builder"

rm -f "$OUT/server.crt" "$OUT/server.key" "$OUT/server.csr"
chmod 600 "$OUT/ca.key" "$OUT/nginx.key" "$OUT/hub-bootstrap.key" "$OUT/mosquitto.key" \
  "$OUT/web.key" "$OUT/opa.key" "$OUT/firmware-builder.key"
echo "Wrote dBmap TLS material to $OUT"
